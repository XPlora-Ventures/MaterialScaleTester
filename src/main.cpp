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
static Adafruit_MAX31865 g_rtd(PT1000_B1_CH2_CS);

enum class ThermalState { OK, FAULT, ALERT };
static ThermalState g_thermal_state      = ThermalState::OK;
static uint32_t     g_fault_start_ms     = 0;
static float        g_pt1000_temp_c      = 0.0f;
static bool         g_rtd_fault          = false;
static uint32_t     g_rtd_fault_since_ms = 0;   // millis() when fault first appeared

static void readPT1000() {
    // Always read temperature — valid even when 0x08 (RTDINLOW) is set
    g_pt1000_temp_c = g_rtd.temperature(PT1000_RNOM, PT1000_RREF);
    uint8_t fault = g_rtd.readFault();
    if (fault) {
        g_rtd.clearFault();
        // 0x08 (RTDINLOW) is a known false positive in 2-wire mode — ignore it
        uint8_t serious = fault & ~0x08u;
        bool new_fault = serious != 0;
        if (new_fault && !g_rtd_fault) g_rtd_fault_since_ms = millis();
        g_rtd_fault = new_fault;
        if (serious) Serial.printf("[PT1000] fault: 0x%02X\n", serious);
    } else {
        g_rtd_fault = false;
        g_rtd_fault_since_ms = 0;
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
            if (!g_rtd_fault && g_pt1000_temp_c < THERMAL_RESET_C) {
                // cooled down in time
                g_thermal_state = ThermalState::OK;
                emitThermalEvent("thermal_cleared", "Temperature normalised — restart cycle to resume");
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
static bool    g_solenoid_pump  = false;  // CH3

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
// Phase / Peripheral Config
// =============================================================================

struct PhaseConfig { bool dry; bool rh; bool hot_plate; bool pump; };
// Defaults from image: Discharge=RH+Pump, Heating=Dry+HotPlate, Cooling=Dry+Pump
static PhaseConfig g_phase_cfg[3] = {
    { false, true,  false, true  },   // 0 = Discharge (HUMID)
    { true,  false, true,  false },   // 1 = Heating   (REGEN)
    { true,  false, false, true  },   // 2 = Cooling   (COOLDOWN)
};

static void phaseCfgSave() {
    uint16_t bits = 0;
    for (int i = 0; i < 3; i++) {
        bits |= (uint16_t)(g_phase_cfg[i].dry       ? 1 : 0) << (i*4 + 0);
        bits |= (uint16_t)(g_phase_cfg[i].rh        ? 1 : 0) << (i*4 + 1);
        bits |= (uint16_t)(g_phase_cfg[i].hot_plate ? 1 : 0) << (i*4 + 2);
        bits |= (uint16_t)(g_phase_cfg[i].pump      ? 1 : 0) << (i*4 + 3);
    }
    prefs.putUShort("phase_cfg", bits);
}

static void phaseCfgLoad() {
    // 0x095A = Discharge:RH+Pump, Heating:Dry+HotPlate, Cooling:Dry+Pump
    uint16_t bits = prefs.getUShort("phase_cfg", 0x095Au);
    for (int i = 0; i < 3; i++) {
        g_phase_cfg[i].dry       = (bits >> (i*4 + 0)) & 1;
        g_phase_cfg[i].rh        = (bits >> (i*4 + 1)) & 1;
        g_phase_cfg[i].hot_plate = (bits >> (i*4 + 2)) & 1;
        g_phase_cfg[i].pump      = (bits >> (i*4 + 3)) & 1;
    }
}

// =============================================================================
// Cycle engine
// =============================================================================

enum class CycleState { IDLE, HUMID, REGEN, COOLDOWN, PAUSED };

static CycleState g_cycle_state    = CycleState::IDLE;
static CycleState g_paused_from    = CycleState::IDLE;
static uint32_t   g_phase_end_ms   = 0;
static uint32_t   g_paused_left_ms = 0;
static uint32_t   g_humid_ms       = 13UL * 60000;
static uint32_t   g_regen_ms       = 14UL * 60000;
static uint32_t   g_cooldown_ms    = 32UL * 60000;
static uint32_t   g_target_cycles  = 0;   // 0 = infinite

static void enterPhase(CycleState phase) {
    g_cycle_state = phase;
    int idx      = (phase == CycleState::HUMID) ? 0 : (phase == CycleState::REGEN) ? 1 : 2;
    uint32_t dur = (phase == CycleState::HUMID) ? g_humid_ms :
                   (phase == CycleState::REGEN)  ? g_regen_ms : g_cooldown_ms;
    const PhaseConfig &cfg = g_phase_cfg[idx];
    setSolenoid(g_solenoid_humid, MOSFET_CH0_P0, cfg.rh);
    setSolenoid(g_solenoid_drier, MOSFET_CH1_P0, cfg.dry);
    setSolenoid(g_solenoid_pump,  MOSFET_CH3_P0, cfg.pump);
    setPlug(cfg.hot_plate);
    g_phase_end_ms = millis() + dur;
}

static void cycleEngineStart(uint32_t humid_ms, uint32_t regen_ms, uint32_t cooldown_ms, uint32_t target) {
    g_humid_ms      = humid_ms;
    g_regen_ms      = regen_ms;
    g_cooldown_ms   = cooldown_ms;
    g_target_cycles = target;
    cycleCountReset();
    enterPhase(CycleState::HUMID);
}

static void cycleEngineStop() {
    g_cycle_state = CycleState::IDLE;
    setSolenoid(g_solenoid_humid, MOSFET_CH0_P0, false);
    setSolenoid(g_solenoid_drier, MOSFET_CH1_P0, false);
    setSolenoid(g_solenoid_pump,  MOSFET_CH3_P0, false);
    setPlug(false);
}

static void cycleEnginePause() {
    if (g_cycle_state != CycleState::HUMID &&
        g_cycle_state != CycleState::REGEN &&
        g_cycle_state != CycleState::COOLDOWN) return;
    g_paused_from    = g_cycle_state;
    int32_t left = (int32_t)(g_phase_end_ms - millis());
    g_paused_left_ms = left > 0 ? (uint32_t)left : 0;
    g_cycle_state    = CycleState::PAUSED;
}

static void cycleEngineResume() {
    if (g_cycle_state != CycleState::PAUSED) return;
    g_cycle_state  = g_paused_from;
    g_phase_end_ms = millis() + g_paused_left_ms;
}

static void cycleEngineTick() {
    if (g_cycle_state != CycleState::HUMID &&
        g_cycle_state != CycleState::REGEN &&
        g_cycle_state != CycleState::COOLDOWN) return;
    if ((int32_t)(millis() - g_phase_end_ms) < 0) return;

    if (g_cycle_state == CycleState::HUMID) {
        enterPhase(CycleState::REGEN);
    } else if (g_cycle_state == CycleState::REGEN) {
        enterPhase(CycleState::COOLDOWN);
    } else {
        // COOLDOWN complete — one full cycle done
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

static const char* phaseNameOf(CycleState s) {
    switch (s) {
        case CycleState::HUMID:    return "humid";
        case CycleState::REGEN:    return "regen";
        case CycleState::COOLDOWN: return "cooldown";
        default:                   return "idle";
    }
}

static const char* cycleStateName() {
    switch (g_cycle_state) {
        case CycleState::IDLE:     return "idle";
        case CycleState::HUMID:    return "humid";
        case CycleState::REGEN:    return "regen";
        case CycleState::COOLDOWN: return "cooldown";
        case CycleState::PAUSED:   return "paused";
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
    doc["solenoid_pump"]  = g_solenoid_pump;
    doc["cycles"]         = g_cycle_count;
    doc["target"]         = g_target_cycles;
    doc["time_left_ms"]   = cycleTimeLeftMs();
    doc["humid_ms"]       = g_humid_ms;
    doc["regen_ms"]       = g_regen_ms;
    doc["cooldown_ms"]    = g_cooldown_ms;
    doc["total_cycle_ms"] = g_humid_ms + g_regen_ms + g_cooldown_ms;
    if (g_cycle_state == CycleState::PAUSED)
        doc["paused_from"] = phaseNameOf(g_paused_from);
    doc["temp_c"]         = g_pt1000_temp_c;
    doc["rtd_fault"]      = g_rtd_fault && (millis() - g_rtd_fault_since_ms >= 10000);
    doc["thermal"]        = thermalStateName();
    JsonArray phaseCfg = doc["phase_cfg"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
        JsonObject p = phaseCfg.add<JsonObject>();
        p["dry"]       = g_phase_cfg[i].dry;
        p["rh"]        = g_phase_cfg[i].rh;
        p["hot_plate"] = g_phase_cfg[i].hot_plate;
        p["pump"]      = g_phase_cfg[i].pump;
    }
    wsSend(doc);
}

static void handleCommand(const char *payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
    const char *cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "start") == 0) {
        uint32_t hms = (uint32_t)(doc["humid_dur"].as<float>()    * 60000.0f);
        uint32_t rms = (uint32_t)(doc["regen_dur"].as<float>()    * 60000.0f);
        uint32_t cms = (uint32_t)(doc["cooldown_dur"].as<float>() * 60000.0f);
        uint32_t tgt = doc["target_cycles"] | 0;
        cycleEngineStart(hms, rms, cms, tgt);
    } else if (strcmp(cmd, "stop")   == 0) { cycleEngineStop(); }
    else if (strcmp(cmd, "pause")  == 0) { cycleEnginePause(); }
    else if (strcmp(cmd, "resume") == 0) { cycleEngineResume(); }
    else if (strcmp(cmd, "set_humidity") == 0) setSolenoid(g_solenoid_humid, MOSFET_CH0_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "set_drier")    == 0) setSolenoid(g_solenoid_drier, MOSFET_CH1_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "set_pump")     == 0) setSolenoid(g_solenoid_pump,  MOSFET_CH3_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "reset_thermal_fault") == 0) {
        if (g_thermal_state != ThermalState::OK) {
            g_thermal_state = ThermalState::OK;
            Serial.println("[THERMAL] fault reset by operator");
        }
    }
    else if (strcmp(cmd, "set_phase_config") == 0) {
        JsonArrayConst phases = doc["phases"];
        if (phases.size() == 3) {
            for (int i = 0; i < 3; i++) {
                g_phase_cfg[i].dry       = phases[i]["dry"].as<bool>();
                g_phase_cfg[i].rh        = phases[i]["rh"].as<bool>();
                g_phase_cfg[i].hot_plate = phases[i]["hot_plate"].as<bool>();
                g_phase_cfg[i].pump      = phases[i]["pump"].as<bool>();
            }
            phaseCfgSave();
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
        "millis,state,solenoid_humid,solenoid_drier,solenoid_pump,"
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
        "%lu,%s,%d,%d,%d,%lu,%lu,%lu,%.2f,%s\n",
        millis(),
        cycleStateName(),
        (int)g_solenoid_humid,
        (int)g_solenoid_drier,
        (int)g_solenoid_pump,
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

    // Deselect all PT1000 CS pins before begin() — prevents bus contention
    for (uint8_t cs : {PT1000_B1_CH1_CS, PT1000_B1_CH2_CS, PT1000_B2_CH1_CS, PT1000_B2_CH2_CS}) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
    }
    g_rtd.begin(MAX31865_2WIRE);
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
    phaseCfgLoad();
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
