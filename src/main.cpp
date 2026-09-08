#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_MAX31865.h>

#include "pin_definitions.h"
#include "wifi_config.h"

#define FW_VERSION   "2.0.0"
#define TELEMETRY_MS  2000

// =============================================================================
// PT1000 — MAX31865 (Board 1, Channel 1)
// =============================================================================

#define PT1000_RNOM          1000.0f
#define PT1000_RREF          4000.0f   // reference resistor on MAX31865 board

// Thermal cutout thresholds
#define THERMAL_CUTOFF_C     300.0f
#define THERMAL_RESET_C      270.0f    // must cool below this to auto-clear
#define THERMAL_ALERT_MS     (10UL * 60000UL)

// Hardware SPI — only CS pin passed; SPI bus started in setup()
// Change MAX31865_2WIRE to 3WIRE or 4WIRE to match your board wiring
static Adafruit_MAX31865 g_rtd(PT1000_B1_CH2_CS);

enum class ThermalState { OK, FAULT, ALERT };
static ThermalState g_thermal_state  = ThermalState::OK;
static uint32_t     g_fault_start_ms = 0;
static float        g_pt1000_temp_c  = 0.0f;
static bool         g_rtd_fault      = false;

static void readPT1000() {
    // Always read temperature — valid even when 0x08 (RTDINLOW) is set
    g_pt1000_temp_c = g_rtd.temperature(PT1000_RNOM, PT1000_RREF);
    uint8_t fault = g_rtd.readFault();
    if (fault) {
        g_rtd.clearFault();
        // 0x08 (RTDINLOW) is a known false positive in 2-wire mode — ignore it
        uint8_t serious = fault & ~0x08u;
        g_rtd_fault = serious != 0;
        if (serious) Serial.printf("[PT1000] fault: 0x%02X\n", serious);
    } else {
        g_rtd_fault = false;
    }
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
// Plug control — delegate to backend via WebSocket
// =============================================================================

// Forward declaration (wsSend defined in WebSocket section below)
static void wsSend(const JsonDocument &doc);

static void setPlug(bool on) {
    JsonDocument doc;
    doc["type"] = "set_plug";
    doc["val"]  = on;
    wsSend(doc);
    Serial.printf("[PLUG] requested %s via server\n", on ? "ON" : "OFF");
}

// =============================================================================
// Thermal cutout state machine
// =============================================================================

// Forward declaration (defined later in file)
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
                setPlug(false);
                emitThermalEvent("thermal_fault",
                    "Temperature exceeded cutoff — plug OFF, cycle stopped");
            }
            break;

        case ThermalState::FAULT:
            if (g_rtd_fault || g_pt1000_temp_c < THERMAL_RESET_C) {
                // cooled down in time
                g_thermal_state = ThermalState::OK;
                setPlug(true);
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

// SD forward declarations (defined in SD Card section below)
static void sdHandleListFiles();
static void sdHandleReadFile(const char *name);

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
            setPlug(true);
            Serial.println("[THERMAL] fault reset by operator — plug ON");
        }
    }
    else if (strcmp(cmd, "list_files") == 0) { sdHandleListFiles(); }
    else if (strcmp(cmd, "read_file")  == 0) {
        const char *name = doc["name"];
        if (name) sdHandleReadFile(name);
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
// SD Card CSV Logger
// =============================================================================

static File g_log_file;
static bool g_sd_ready = false;

static void sdInit() {
    if (digitalRead(SD_DETECT) == HIGH) {
        Serial.println("[SD] no card — skipping");
        return;
    }
    if (!SD.begin(SD_CS)) {
        Serial.println("[SD] mount failed");
        return;
    }
    uint32_t boot = prefs.getULong("bootcnt", 0) + 1;
    prefs.putULong("bootcnt", boot);

    char fname[24];
    snprintf(fname, sizeof(fname), "/mst_%05lu.csv", (unsigned long)boot);
    g_log_file = SD.open(fname, FILE_WRITE);
    if (!g_log_file) {
        Serial.printf("[SD] open %s failed\n", fname);
        return;
    }
    g_log_file.println(
        "millis,state,solenoid_humid,solenoid_drier,"
        "cycles,target,time_left_ms,temp_c,thermal"
    );
    g_log_file.flush();
    g_sd_ready = true;
    digitalWrite(LED_SD, HIGH);
    Serial.printf("[SD] logging to %s\n", fname);
}

static void sdHandleListFiles() {
    if (!g_sd_ready) {
        JsonDocument doc;
        doc["type"]  = "sd_files";
        doc["error"] = "SD not ready";
        wsSend(doc);
        return;
    }
    JsonDocument doc;
    doc["type"] = "sd_files";
    JsonArray arr = doc["files"].to<JsonArray>();
    File root = SD.open("/");
    while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            JsonObject f = arr.add<JsonObject>();
            f["name"] = entry.name();
            f["size"] = (uint32_t)entry.size();
        }
        entry.close();
    }
    root.close();
    wsSend(doc);
}

static void sdHandleReadFile(const char *name) {
    // Ensure path starts with '/'
    char path[64];
    if (name[0] == '/') snprintf(path, sizeof(path), "%s", name);
    else                snprintf(path, sizeof(path), "/%s", name);

    File f = SD.open(path);
    if (!f) {
        JsonDocument doc;
        doc["type"]  = "sd_chunk";
        doc["error"] = "file not found";
        doc["done"]  = true;
        wsSend(doc);
        return;
    }

    static char buf[512];
    while (f.available()) {
        size_t n   = f.readBytes(buf, sizeof(buf) - 1);
        buf[n]     = '\0';
        bool done  = !f.available();
        JsonDocument doc;
        doc["type"] = "sd_chunk";
        doc["data"] = buf;
        doc["done"] = done;
        wsSend(doc);
    }
    f.close();
}

static void sdLogRow() {
    if (!g_sd_ready) return;
    size_t written = g_log_file.printf(
        "%lu,%s,%d,%d,%lu,%lu,%lu,%.2f,%s\n",
        millis(),
        cycleStateName(),
        (int)g_solenoid_humid,
        (int)g_solenoid_drier,
        (unsigned long)g_cycle_count,
        (unsigned long)g_target_cycles,
        (unsigned long)cycleTimeLeftMs(),
        g_pt1000_temp_c,
        thermalStateName()
    );
    if (written == 0) {
        g_sd_ready = false;
        digitalWrite(LED_SD, LOW);
        Serial.println("[SD] write failed — logging stopped");
    } else {
        g_log_file.flush();
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

    pinMode(SD_DETECT, INPUT);  // hardware pull-up on board
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

    sdInit();

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
        sdLogRow();
    }
}
