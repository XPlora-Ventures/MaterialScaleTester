#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_MAX31865.h>
#include "mbedtls/aes.h"

#include "pin_definitions.h"
#include "wifi_config.h"

#define FW_VERSION   "2.0.0"
#define TELEMETRY_MS  2000

// =============================================================================
// PT1000 — MAX31865 (Board 1, Channel 1)
// =============================================================================

#define PT1000_RNOM          1000.0f
#define PT1000_RREF          4300.0f   // reference resistor on MAX31865 board

// Thermal cutout thresholds
#define THERMAL_CUTOFF_C     300.0f
#define THERMAL_RESET_C      270.0f    // must cool below this to auto-clear
#define THERMAL_ALERT_MS     (10UL * 60000UL)

// Hardware SPI — only CS pin passed; SPI bus started in setup()
// Change MAX31865_2WIRE to 3WIRE or 4WIRE to match your board wiring
static Adafruit_MAX31865 g_rtd(PT1000_B1_CH1_CS);

enum class ThermalState { OK, FAULT, ALERT };
static ThermalState g_thermal_state  = ThermalState::OK;
static uint32_t     g_fault_start_ms = 0;
static float        g_pt1000_temp_c  = 0.0f;
static bool         g_rtd_fault      = false;

static void readPT1000() {
    uint8_t fault = g_rtd.readFault();
    if (fault) {
        g_rtd_fault = true;
        g_rtd.clearFault();
        Serial.printf("[PT1000] fault register: 0x%02X\n", fault);
        return;
    }
    g_rtd_fault     = false;
    g_pt1000_temp_c = g_rtd.temperature(PT1000_RNOM, PT1000_RREF);
}

static const char* thermalStateName() {
    switch (g_thermal_state) {
        case ThermalState::OK:    return "ok";
        case ThermalState::FAULT: return "fault";
        case ThermalState::ALERT: return "alert";
    }
    return "ok";
}

// =============================================================================
// Tuya LAN plug control — protocol v3.3, TCP port 6668
// =============================================================================

static uint32_t g_tuya_seq = 1;

static void tuyaAesEcbEncrypt(const uint8_t *key, const uint8_t *in, uint8_t *out, size_t blocks) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    for (size_t i = 0; i < blocks; i++)
        mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in + i * 16, out + i * 16);
    mbedtls_aes_free(&ctx);
}

static uint32_t crc32buf(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1u));
    }
    return ~crc;
}

static void tuyaSetPlug(bool on) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TUYA] no WiFi — cannot control plug");
        return;
    }

    // Build control JSON
    char json[192];
    snprintf(json, sizeof(json),
        "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\",\"dps\":{\"%s\":%s}}",
        TUYA_PLUG_DEVICE_ID, TUYA_PLUG_DEVICE_ID,
        (unsigned long)(millis() / 1000),
        TUYA_PLUG_DPS_SWITCH, on ? "true" : "false");
    size_t jLen = strlen(json);

    // PKCS7 pad to 16-byte boundary
    uint8_t plain[256] = {};
    memcpy(plain, json, jLen);
    uint8_t pad = 16 - (uint8_t)(jLen % 16);
    for (uint8_t i = 0; i < pad; i++) plain[jLen + i] = pad;
    size_t plainLen = jLen + pad;

    // AES-128-ECB encrypt (key = first 16 bytes of local key)
    uint8_t enc[256] = {};
    uint8_t key[16];
    memcpy(key, TUYA_PLUG_LOCAL_KEY, 16);
    tuyaAesEcbEncrypt(key, plain, enc, plainLen / 16);

    // v3.3 payload = 12-byte version header + encrypted data
    uint8_t payload[300] = {};
    memcpy(payload, "3.3\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    memcpy(payload + 12, enc, plainLen);
    size_t payloadLen = 12 + plainLen;

    // Assemble Tuya packet
    uint8_t pkt[400] = {};
    size_t  p = 0;

    pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0x55; pkt[p++] = 0xAA; // prefix
    uint32_t seq = g_tuya_seq++;
    pkt[p++] = (seq >> 24) & 0xFF; pkt[p++] = (seq >> 16) & 0xFF;
    pkt[p++] = (seq >>  8) & 0xFF; pkt[p++] = (seq      ) & 0xFF;
    pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0x07; // cmd CONTROL
    uint32_t pktlen = (uint32_t)payloadLen + 8;                           // +CRC+suffix
    pkt[p++] = (pktlen >> 24) & 0xFF; pkt[p++] = (pktlen >> 16) & 0xFF;
    pkt[p++] = (pktlen >>  8) & 0xFF; pkt[p++] = (pktlen      ) & 0xFF;
    memcpy(pkt + p, payload, payloadLen); p += payloadLen;
    uint32_t crc = crc32buf(pkt, p);
    pkt[p++] = (crc >> 24) & 0xFF; pkt[p++] = (crc >> 16) & 0xFF;
    pkt[p++] = (crc >>  8) & 0xFF; pkt[p++] = (crc      ) & 0xFF;
    pkt[p++] = 0x00; pkt[p++] = 0x00; pkt[p++] = 0xAA; pkt[p++] = 0x55; // suffix

    WiFiClient client;
    client.setTimeout(2000);
    if (!client.connect(TUYA_PLUG_IP, 6668)) {
        Serial.printf("[TUYA] connect to %s:6668 failed\n", TUYA_PLUG_IP);
        return;
    }
    client.write(pkt, p);
    client.flush();
    delay(200);
    client.stop();
    Serial.printf("[TUYA] plug %s\n", on ? "ON" : "OFF");
}

// =============================================================================
// Thermal cutout state machine
// =============================================================================

// Forward declarations (defined later in file)
static void wsSend(const JsonDocument &doc);
static void cycleEngineStop();

static void emitThermalEvent(const char *type, const char *msg) {
    JsonDocument doc;
    doc["type"]   = type;
    doc["temp_c"] = g_pt1000_temp_c;
    doc["msg"]    = msg;
    wsSend(doc);
    Serial.printf("[THERMAL] %s — %.1f°C\n", type, g_pt1000_temp_c);
}

static void thermalCutoutTick() {
    switch (g_thermal_state) {
        case ThermalState::OK:
            if (!g_rtd_fault && g_pt1000_temp_c >= THERMAL_CUTOFF_C) {
                g_thermal_state  = ThermalState::FAULT;
                g_fault_start_ms = millis();
                cycleEngineStop();
                tuyaSetPlug(false);
                emitThermalEvent("thermal_fault",
                    "Temperature exceeded cutoff — plug OFF, cycle stopped");
            }
            break;

        case ThermalState::FAULT:
            if (g_rtd_fault || g_pt1000_temp_c < THERMAL_RESET_C) {
                // cooled down in time
                g_thermal_state = ThermalState::OK;
                tuyaSetPlug(true);
                emitThermalEvent("thermal_cleared", "Temperature normalised — plug restored");
            } else if (millis() - g_fault_start_ms >= THERMAL_ALERT_MS) {
                g_thermal_state = ThermalState::ALERT;
                emitThermalEvent("thermal_alert",
                    "Temperature did not drop after 10 minutes — check equipment immediately");
            }
            break;

        case ThermalState::ALERT:
            // stays here until the operator sends "reset_thermal_fault"
            break;
    }
}

// =============================================================================
// PCF8575
// =============================================================================

static uint8_t g_pcf_p0 = 0x00;
static uint8_t g_pcf_p1 = 0x00;
static bool    g_solenoid_humid = false;
static bool    g_solenoid_drier = false;

static void pcfFlush() {
    Wire.beginTransmission(PCF8575_ADDR);
    Wire.write(g_pcf_p0);
    Wire.write(g_pcf_p1);
    Wire.endTransmission();
}

static void setSolenoid(bool &state, uint8_t mask, bool on) {
    if (on) g_pcf_p0 |=  mask;
    else    g_pcf_p0 &= ~mask;
    pcfFlush();
    state = on;
}

// =============================================================================
// NVS — cycle count
// =============================================================================

static Preferences prefs;
static uint32_t    g_cycle_count = 0;

static void cycleCountSave() {
    prefs.putULong("cycles", g_cycle_count);
}

static void cycleCountReset() {
    g_cycle_count = 0;
    cycleCountSave();
}

// =============================================================================
// Cycle engine
// =============================================================================

enum class CycleState { IDLE, HUMID, DRIER, PAUSED };

static CycleState g_cycle_state    = CycleState::IDLE;
static CycleState g_paused_from    = CycleState::IDLE;
static uint32_t   g_phase_end_ms   = 0;
static uint32_t   g_paused_left_ms = 0;
static uint32_t   g_humid_ms       = 20UL * 60000;
static uint32_t   g_drier_ms       = 40UL * 60000;
static uint32_t   g_target_cycles  = 0;   // 0 = infinite

static void enterPhase(CycleState phase) {
    g_cycle_state = phase;
    if (phase == CycleState::HUMID) {
        setSolenoid(g_solenoid_drier, MOSFET_CH0_P0, false);
        setSolenoid(g_solenoid_humid, MOSFET_CH1_P0, true);
        g_phase_end_ms = millis() + g_humid_ms;
    } else {
        setSolenoid(g_solenoid_humid, MOSFET_CH1_P0, false);
        setSolenoid(g_solenoid_drier, MOSFET_CH0_P0, true);
        g_phase_end_ms = millis() + g_drier_ms;
    }
}

static void cycleEngineStart(uint32_t humid_ms, uint32_t drier_ms, uint32_t target) {
    g_humid_ms      = humid_ms;
    g_drier_ms      = drier_ms;
    g_target_cycles = target;
    cycleCountReset();
    enterPhase(CycleState::HUMID);
}

static void cycleEngineStop() {
    g_cycle_state = CycleState::IDLE;
    setSolenoid(g_solenoid_humid, MOSFET_CH1_P0, false);
    setSolenoid(g_solenoid_drier, MOSFET_CH0_P0, false);
    cycleCountReset();
}

static void cycleEnginePause() {
    if (g_cycle_state != CycleState::HUMID && g_cycle_state != CycleState::DRIER) return;
    g_paused_from    = g_cycle_state;
    g_paused_left_ms = g_phase_end_ms - millis();
    g_cycle_state    = CycleState::PAUSED;
}

static void cycleEngineResume() {
    if (g_cycle_state != CycleState::PAUSED) return;
    g_cycle_state  = g_paused_from;
    g_phase_end_ms = millis() + g_paused_left_ms;
}

static void cycleEngineTick() {
    if (g_cycle_state != CycleState::HUMID && g_cycle_state != CycleState::DRIER) return;
    if ((int32_t)(millis() - g_phase_end_ms) < 0) return;

    if (g_cycle_state == CycleState::HUMID) {
        enterPhase(CycleState::DRIER);
    } else {
        g_cycle_count++;
        cycleCountSave();
        if (g_target_cycles > 0 && g_cycle_count >= g_target_cycles) {
            cycleEngineStop();
        } else {
            enterPhase(CycleState::HUMID);
        }
    }
}

static uint32_t cycleTimeLeftMs() {
    if (g_cycle_state == CycleState::PAUSED)   return g_paused_left_ms;
    if (g_cycle_state == CycleState::IDLE)      return 0;
    int32_t left = (int32_t)(g_phase_end_ms - millis());
    return left > 0 ? (uint32_t)left : 0;
}

static const char* cycleStateName() {
    switch (g_cycle_state) {
        case CycleState::IDLE:   return "idle";
        case CycleState::HUMID:  return "humid";
        case CycleState::DRIER:  return "drier";
        case CycleState::PAUSED: return "paused";
    }
    return "idle";
}

// =============================================================================
// WebSocket
// =============================================================================

static WebSocketsClient ws;
static bool g_ws_connected = false;

static void wsSend(const JsonDocument &doc) {
    if (!g_ws_connected) return;
    String out;
    serializeJson(doc, out);
    ws.sendTXT(out);
}

static void emitTelemetry() {
    JsonDocument doc;
    doc["fw"]             = FW_VERSION;
    doc["state"]          = cycleStateName();
    doc["solenoid_humid"] = g_solenoid_humid;
    doc["solenoid_drier"] = g_solenoid_drier;
    doc["cycles"]         = g_cycle_count;
    doc["target"]         = g_target_cycles;
    doc["time_left_ms"]   = cycleTimeLeftMs();
    doc["humid_ms"]       = g_humid_ms;
    doc["drier_ms"]       = g_drier_ms;
    doc["temp_c"]         = g_pt1000_temp_c;
    doc["rtd_fault"]      = g_rtd_fault;
    doc["thermal"]        = thermalStateName();
    wsSend(doc);
}

static void handleCommand(const char *payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
    const char *cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "start") == 0) {
        uint32_t hms = (uint32_t)(doc["humid_dur"].as<float>() * 60000.0f);
        uint32_t dms = (uint32_t)(doc["drier_dur"].as<float>() * 60000.0f);
        uint32_t tgt = doc["target_cycles"] | 0;
        cycleEngineStart(hms, dms, tgt);
    } else if (strcmp(cmd, "stop")   == 0) { cycleEngineStop(); }
    else if (strcmp(cmd, "pause")  == 0) { cycleEnginePause(); }
    else if (strcmp(cmd, "resume") == 0) { cycleEngineResume(); }
    else if (strcmp(cmd, "set_humidity") == 0) setSolenoid(g_solenoid_humid, MOSFET_CH1_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "set_drier")    == 0) setSolenoid(g_solenoid_drier, MOSFET_CH0_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "reset_thermal_fault") == 0) {
        if (g_thermal_state != ThermalState::OK) {
            g_thermal_state = ThermalState::OK;
            tuyaSetPlug(true);
            Serial.println("[THERMAL] fault reset by operator — plug ON");
        }
    }
}

static void wsEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            g_ws_connected = true;
            digitalWrite(LED_WIFI, HIGH);
            Serial.println("[WS] connected");
            emitTelemetry();
            break;
        case WStype_DISCONNECTED:
            g_ws_connected = false;
            digitalWrite(LED_WIFI, LOW);
            Serial.println("[WS] disconnected");
            break;
        case WStype_TEXT:
            handleCommand((const char *)payload);
            break;
        default:
            break;
    }
}

// =============================================================================
// WiFi
// =============================================================================

static void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void wifiCheck() {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck < 5000) return;
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_WIFI, LOW);
        wifiConnect();
    }
}

// =============================================================================
// Setup / Loop
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Material Scale Tester v" FW_VERSION " ===");

    Serial.println("[I2C] init");
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    g_rtd.begin(MAX31865_2WIRE); // change to MAX31865_3WIRE / 4WIRE if your board uses those
    Serial.println("[PT1000] MAX31865 initialized");

    pinMode(LED_HB,   OUTPUT);
    pinMode(LED_SD,   OUTPUT); digitalWrite(LED_SD,   LOW);
    pinMode(LED_WIFI, OUTPUT); digitalWrite(LED_WIFI, LOW);
    pinMode(LED_FLT,  OUTPUT); digitalWrite(LED_FLT,  LOW);
    Serial.println("[GPIO] pins configured");

    pcfFlush();
    Serial.println("[PCF] flushed");

    prefs.begin("mst", false);
    g_cycle_count = prefs.getULong("cycles", 0);
    Serial.printf("[NVS] cycle count = %lu\n", g_cycle_count);

    Serial.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);
    wifiConnect();
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(250);
        digitalWrite(LED_HB, !digitalRead(LED_HB));
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] connected, IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] FAILED — continuing without WiFi");
    }

    Serial.printf("[WS] connecting to wss://%s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
    ws.beginSSL(WS_HOST, WS_PORT, WS_PATH);
    ws.onEvent(wsEvent);
    ws.setReconnectInterval(5000);

    Serial.println("[BOOT] setup complete");
}

void loop() {
    static uint32_t lastTelemetry = 0;
    static uint32_t lastPT1000    = 0;

    wifiCheck();
    ws.loop();
    cycleEngineTick();

    uint32_t now = millis();

    if (now - lastPT1000 >= 1000) {
        lastPT1000 = now;
        readPT1000();
        thermalCutoutTick();
    }

    if (now - lastTelemetry >= TELEMETRY_MS) {
        lastTelemetry = now;
        digitalWrite(LED_HB, !digitalRead(LED_HB));
        emitTelemetry();
    }
}
