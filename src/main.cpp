#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_MAX31865.h>
#include <ArduinoJson.h>

#include "pin_definitions.h"

#define FW_VERSION   "1.0.0"
#define TELEMETRY_MS  2000

// ── Thermostat setpoint / hysteresis ─────────────────────────────────────────
static float g_setpoint   = 0.0f;
static float g_hysteresis = 0.5f;

// ── PT1000 ────────────────────────────────────────────────────────────────────
#define PT1000_R_NOM  1000.0f
#define PT1000_R_REF  4000.0f
static Adafruit_MAX31865 g_rtd(PT1000_B1_CH1_CS, SPI_MOSI, SPI_MISO, SPI_SCK);
static float   g_rtd_temp  = NAN;
static uint8_t g_rtd_fault = 0;

// ── MOSFET state ──────────────────────────────────────────────────────────────
static bool      g_heater_l = false;   // CH0
static bool      g_heater_r = false;   // CH1
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

static void setHeaters(bool on) {
    if (on) g_pcf_p0 |=  (MOSFET_CH0_P0 | MOSFET_CH1_P0);
    else    g_pcf_p0 &= ~(MOSFET_CH0_P0 | MOSFET_CH1_P0);
    pcfFlush();
    g_heater_l = on;
    g_heater_r = on;
}

// =============================================================================
// PT1000
// =============================================================================

static void readPT1000() {
    g_rtd_fault = g_rtd.readFault();
    if (g_rtd_fault) {
        g_rtd.clearFault();
        g_rtd_temp = NAN;
    } else {
        g_rtd_temp = g_rtd.temperature(PT1000_R_NOM, PT1000_R_REF);
    }
}

// =============================================================================
// Thermostat
// =============================================================================

static void updateHeaters() {
    if (isnan(g_rtd_temp)) { setHeaters(false); return; }
    if (!g_heater_l && g_rtd_temp < g_setpoint - g_hysteresis) setHeaters(true);
    if ( g_heater_l && g_rtd_temp >= g_setpoint)               setHeaters(false);
}

// =============================================================================
// Telemetry
// =============================================================================

static void emitTelemetry() {
    JsonDocument doc;
    if (isnan(g_rtd_temp)) doc["rtd"] = nullptr;
    else                   doc["rtd"] = roundf(g_rtd_temp * 10) / 10.0f;
    doc["rtd_fault"]  = g_rtd_fault;
    doc["heater_l"]   = g_heater_l;
    doc["heater_r"]   = g_heater_r;
    doc["setpoint"]   = roundf(g_setpoint   * 10) / 10.0f;
    doc["hysteresis"] = roundf(g_hysteresis * 10) / 10.0f;
    doc["fw"]         = FW_VERSION;
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
    if      (strcmp(cmd, "set_sp")   == 0) g_setpoint   = doc["val"].as<float>();
    else if (strcmp(cmd, "set_hyst") == 0) g_hysteresis = max(0.0f, doc["val"].as<float>());
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

    g_rtd.begin(MAX31865_3WIRE);

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
        readPT1000();
        updateHeaters();
        digitalWrite(LED_HB, !digitalRead(LED_HB));
        emitTelemetry();
    }
}
