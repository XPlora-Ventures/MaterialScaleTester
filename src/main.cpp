#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "pin_definitions.h"
#include "wifi_config.h"

#define FW_VERSION   "2.0.0"
#define TELEMETRY_MS  2000

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

    wifiCheck();
    ws.loop();
    cycleEngineTick();

    uint32_t now = millis();
    if (now - lastTelemetry >= TELEMETRY_MS) {
        lastTelemetry = now;
        digitalWrite(LED_HB, !digitalRead(LED_HB));
        emitTelemetry();
    }
}
