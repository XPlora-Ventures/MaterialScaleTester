#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>

#include "pin_definitions.h"

#define FW_VERSION   "1.0.0"
#define TELEMETRY_MS  2000

// ── MOSFET state ──────────────────────────────────────────────────────────────
static bool      g_solenoid_humid  = false;   // CH1
static bool      g_solenoid_drier  = false;   // CH0
static uint8_t   g_pcf_p0  = 0x00;
static uint8_t   g_pcf_p1  = 0x00;

// ── Serial Rx ─────────────────────────────────────────────────────────────────
static String g_rxBuf;

// =============================================================================
// PCF8575
// =============================================================================

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
// Telemetry
// =============================================================================

static void emitTelemetry() {
    JsonDocument doc;
    doc["solenoid_humid"]  = g_solenoid_humid;
    doc["solenoid_drier"]  = g_solenoid_drier;
    doc["fw"]              = FW_VERSION;
    serializeJson(doc, Serial);
    Serial.print('\n');
}

// =============================================================================
// Command parser
// =============================================================================

static void handleCommand(const String &line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;
    const char *cmd = doc["cmd"];
    if (!cmd) return;
    if      (strcmp(cmd, "set_humidity") == 0) setSolenoid(g_solenoid_humid, MOSFET_CH1_P0, doc["val"].as<bool>());
    else if (strcmp(cmd, "set_drier")    == 0) setSolenoid(g_solenoid_drier, MOSFET_CH0_P0, doc["val"].as<bool>());
}

static void checkSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            g_rxBuf.trim();
            if (g_rxBuf.length()) handleCommand(g_rxBuf);
            g_rxBuf = "";
        } else {
            g_rxBuf += c;
        }
    }
}

// =============================================================================
// Setup / Loop
// =============================================================================

void setup() {
    Serial.begin(115200);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    pinMode(LED_HB,   OUTPUT);
    pinMode(LED_SD,   OUTPUT); digitalWrite(LED_SD,   LOW);
    pinMode(LED_WIFI, OUTPUT); digitalWrite(LED_WIFI, LOW);
    pinMode(LED_FLT,  OUTPUT); digitalWrite(LED_FLT,  LOW);

    pcfFlush();   // all MOSFETs off
}

void loop() {
    static uint32_t lastTelemetry = 0;

    checkSerial();

    uint32_t now = millis();
    if (now - lastTelemetry >= TELEMETRY_MS) {
        lastTelemetry = now;
        digitalWrite(LED_HB, !digitalRead(LED_HB));
        emitTelemetry();
    }
}
