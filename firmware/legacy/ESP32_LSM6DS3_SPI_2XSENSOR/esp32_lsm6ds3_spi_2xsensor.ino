/**
 * =============================================================================
 * GEOSHAKE IoT SEISMIC MONITORING STATION - ESP32-S3 + Dual LSM6DS3 (SPI)
 * =============================================================================
 *
 * This system monitors real-time seismic activity and publishes data
 * to HiveMQ MQTT broker for distributed earthquake monitoring network.
 *
 * Features:
 * - Mobile WiFi Provisioning (GeoLink-216 hotspot)
 * - Real-time telemetry publishing to MQTT
 * - PGA and Arias Intensity event detection
 * - P-wave and S-wave detection
 * - Unique station ID based on ESP chip ID
 * - Local web dashboard (optional)
 * - LED status indicators
 * - Command subscription for remote control
 * - Persistent WiFi credentials storage (EEPROM)
 *
 * COORDINATE SYSTEM - ENU (East-North-Up) Standard:
 * ================================================
 * This system follows the ENU coordinate system used in seismology:
 * - E (East):  Sensor X-axis → Positive = East,  Negative = West
 * - N (North): Sensor Y-axis → Positive = North, Negative = South
 * - U (Up):    Sensor Z-axis → Positive = Up (Sky), Negative = Down
 *
 * Gravity Handling:
 * - When sensor is level, Z-axis (Up) reads approximately +1.0g
 * - Calibration establishes gravity baseline (offsetZ ≈ 1.0g)
 * - All measurements subtract this baseline to isolate seismic motion
 *
 * Wave Detection:
 * - P-Wave (Primary/Vertical): Detected on U (Z-axis, vertical motion)
 * - S-Wave (Secondary/Horizontal): Detected on E-N plane (X-Y, horizontal
 * motion)
 *
 * MQTT Topic Structure:
 * - seismic/stations/{station_id}/status
 * - seismic/stations/{station_id}/telemetry
 * - seismic/stations/{station_id}/events/pga
 * - seismic/stations/{station_id}/events/arias
 * - seismic/stations/{station_id}/commands (subscribed)
 *
 * WiFi Provisioning Flow:
 * 1. Device creates "GeoLink-216" open hotspot on first boot
 * 2. Mobile app connects to this hotspot
 * 3. Mobile app sends WiFi credentials via HTTP POST to /provision
 * 4. Device stores credentials and connects to home WiFi
 * 5. Normal MQTT operation begins
 */

#include "SeismicDiscriminator.h"
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <HTTPUpdate.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <time.h>

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define PIN_SCK 12
#define PIN_MISO 13
#define PIN_MOSI 11
#define PIN_CS1 9
#define PIN_CS2 10
#define LED_PIN 48
#define BUZZER_PIN 5

// =============================================================================
// LSM6DS3 REGISTER ADDRESSES
// =============================================================================
#define REG_WHO_AM_I 0x0F
#define REG_CTRL1_XL 0x10
#define REG_CTRL2_G 0x11
#define REG_CTRL3_C 0x12
#define REG_CTRL6_C 0x15
#define REG_CTRL8_XL 0x17
#define REG_STATUS 0x1E
#define REG_OUTX_L_XL 0x28
#define REG_OUTX_H_XL 0x29
#define REG_OUTY_L_XL 0x2A
#define REG_OUTY_H_XL 0x2B
#define REG_OUTZ_L_XL 0x2C
#define REG_OUTZ_H_XL 0x2D

// =============================================================================
// EEPROM SETTINGS
// =============================================================================
#define EEPROM_SIZE 512
#define EEPROM_MAGIC_BYTE 0x47
#define EEPROM_CAL_ADDR 0
#define EEPROM_SSID_ADDR 50
#define EEPROM_PASS_ADDR 114
#define EEPROM_NAME_ADDR 178
#define EEPROM_LAT_ADDR 242
#define EEPROM_LON_ADDR 246
#define MAX_SSID_LENGTH 64
#define MAX_PASS_LENGTH 64
#define MAX_NAME_LENGTH 64

// =============================================================================
// WiFi PROVISIONING SETTINGS
// =============================================================================
const char *AP_SSID =
    "GeoLink-216"; // Don't change if you want to use Mobile app provisioning
const char *AP_PASSWORD =
    ""; // Don't change if you want to use Mobile app provisioning
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

String wifiSSID = "";
String wifiPassword = "";
bool isProvisioned = false;

// =============================================================================
// MQTT SETTINGS (HiveMQ Cloud)
// =============================================================================
const char *mqtt_broker = "144c5ef5900847dab4640521d5cad06c.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char *mqtt_username =
    "XXXXX"; // get your username and password from
             // https://www.geoshake.org/join-network.html
const char *mqtt_password = "XXXXX";

// Station Information
String stationId = "";
String stationName = "Geostation-S3";
float stationLat = 37.392869;
float stationLon = 34.629968;
const char *firmwareVersion = "2.0.0";
String deviceModel = "GeoShake T1";       // Device model
String sensorType = "LSM6DS3 Dual";       // Sensor type and count
String controllerType = "ESP32-S3-WROOM"; // Your controller name

// MQTT Topics
String topicStatus, topicTelemetry, topicEventPGA, topicEventArias;
String topicCommands, topicOTA, topicLWT;

// MQTT Connection
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;
const int MAX_RECONNECT_ATTEMPTS = 5;
unsigned long lastStatusPublish = 0;
const unsigned long STATUS_INTERVAL = 30000;

// =============================================================================
// NTP SETTINGS
// =============================================================================
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600; // UTC+3
const int daylightOffset_sec = 0;

// =============================================================================
// WEB SERVER
// =============================================================================
WebServer server(80);
bool webServerEnabled = false;

// =============================================================================
// LED MODES
// =============================================================================
enum LedMode {
  LED_OFF,
  LED_ON,
  LED_WIFI_CONNECTING,
  LED_WIFI_CONNECTED,
  LED_MQTT_CONNECTING,
  LED_MQTT_CONNECTED,
  LED_PGA_EVENT,
  LED_ARIAS_EVENT,
  LED_MONITORING,
  LED_ERROR
};
LedMode currentLedMode = LED_OFF;
unsigned long lastLedBlink = 0;
bool ledState = false;

// =============================================================================
// SENSOR CALIBRATION
// =============================================================================
float offX1 = 0, offY1 = 0, offZ1 = 0;
float offX2 = 0, offY2 = 0, offZ2 = 0;
float factor = 0.000061;

struct CalibrationData {
  uint16_t magic;
  float offX1, offY1, offZ1;
  float offX2, offY2, offZ2;
  uint32_t timestamp;
};

// =============================================================================
// SEISMIC DETECTION
// =============================================================================
SeismicDiscriminator discriminator;
float currentPGA = 0.0;
float maxPGA = 0.0;
float sessionMaxPGA = 0.0;
String currentWaveType = "None";
float estimatedDistanceKm = 0.0;
float staLtaRatioZ = 0.0, staLtaRatioH = 0.0;

// PGA Thresholds
const float PGA_THRESHOLD_LOW = 0.008;
const float PGA_THRESHOLD_MEDIUM = 0.015;
const float PGA_THRESHOLD_HIGH = 0.030;
const float PGA_THRESHOLD_SEVERE = 0.080;

// PGA Event Detection
bool isPgaEventActive = false;
unsigned long lastPgaAboveThreshold = 0;
unsigned long pgaEventStartTime = 0;
float currentEventMax = 0.0;
const float PGA_EVENT_THRESHOLD = 0.01;
const unsigned long PGA_COOLDOWN_MS = 2000;
int eventCount = 0;
int confirmCounter = 0;
const int EVENT_CONFIRM_COUNT = 5;

// =============================================================================
// ARIAS INTENSITY
// =============================================================================
float ariasIntensity = 0.0;
float currentAriasEventMax = 0.0;
const int ARIAS_BUFFER_SIZE = 200;
float ariasBuffer[200];
int ariasIndex = 0;
const float ARIAS_FACTOR = 1.5708 / 19.62;
unsigned long lastSampleMicros = 0;
float runningAriasSum = 0.0;

bool isAriasEventActive = false;
unsigned long lastAriasAboveThreshold = 0;
unsigned long ariasEventStartTime = 0;
const float ARIAS_EVENT_THRESHOLD = 0.009;
const unsigned long ARIAS_COOLDOWN_MS = 3000;

// =============================================================================
// SPIKE FILTER
// =============================================================================
const int SPIKE_FILTER_SIZE = 5;
const float MAX_VALID_PGA = 2.0;
float pgaBuffer[SPIKE_FILTER_SIZE] = {0};
int pgaBufferIndex = 0;
bool bufferFilled = false;
float lastValidPGA = 0.0;
unsigned long spikeCount = 0;

// =============================================================================
// CHART BUFFER
// =============================================================================
const int CHART_BUFFER_SIZE = 60;
float axisEastBuffer[60], axisNorthBuffer[60], axisUpBuffer[60];
int chartIndex = 0;

// =============================================================================
// OFFLINE BUFFER
// =============================================================================
enum BufferEntryType {
  ENTRY_TELEMETRY = 0,
  ENTRY_PGA_EVENT = 1,
  ENTRY_ARIAS_EVENT = 2
};

struct OfflineEntry {
  uint8_t entryType;
  uint32_t timestamp;
  float pga, arias, axisE, axisN, axisU, peakValue;
  int8_t rssi;
  char waveType[12];
  uint8_t reserved[6];
};

const int OFFLINE_BUFFER_SIZE = 20;
OfflineEntry offlineBuffer[OFFLINE_BUFFER_SIZE];
volatile int bufferHead = 0, bufferTail = 0, bufferCount = 0;
bool offlineMode = false;
unsigned long lastOfflineSave = 0;
const unsigned long OFFLINE_SAVE_INTERVAL = 5000;

// =============================================================================
// STATISTICS
// =============================================================================
unsigned long systemStartTime = 0;
unsigned long startTime = 0;
unsigned long sampleCount = 0;
int totalPgaEvents = 0;
int totalAriasEvents = 0;
unsigned long totalTelemetryPublished = 0;
unsigned long lastTelemetryPublish = 0;
const unsigned long TELEMETRY_ACTIVE_INTERVAL = 2000;
const unsigned long TELEMETRY_IDLE_INTERVAL = 60000;
bool printEnabled = true;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================
void setupWiFi();
void setupMQTT();
void startProvisioningMode();
void handleProvisioningRoot();
void handleProvisioningConfig();
void handleProvisioningStatus();
bool loadWiFiCredentials();
void saveWiFiCredentials(String ssid, String password);
void clearWiFiCredentials();
void loadStationConfig();
void saveStationName(String name);
void saveStationCoordinates(float lat, float lon);
bool loadCalibrationFromEEPROM();
void saveCalibrationToEEPROM();
void performCalibration();
void performOTA(String url, String version);
void mqttReconnect();
void mqttCallback(char *topic, byte *payload, unsigned int length);
void publishTelemetry();
void publishStatus();
void publishPgaEvent();
void publishAriasEvent();
void handleCommand(String command, String value);
String getStationId();
void updateLED();
void setLedMode(LedMode mode);
String getFormattedTime();
String getFormattedDate();
void initOfflineBuffer();
bool addToOfflineBuffer(BufferEntryType type, float pga, float arias, float e,
                        float n, float u, float peak, const char *wave);
bool hasOfflineData();
bool getFromOfflineBuffer(OfflineEntry *entry);
void flushOfflineBuffer();
String getRichterEstimation(float pga);
void calculateAriasIntensity(float acceleration);

// =============================================================================
// SPI FUNCTIONS
// =============================================================================
uint8_t readRegister(uint8_t csPin, uint8_t regAddr) {
  digitalWrite(csPin, LOW);
  SPI.transfer(regAddr | 0x80);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(csPin, HIGH);
  return value;
}

void writeRegister(uint8_t csPin, uint8_t regAddr, uint8_t data) {
  digitalWrite(csPin, LOW);
  SPI.transfer(regAddr & 0x7F);
  SPI.transfer(data);
  digitalWrite(csPin, HIGH);
}

void configureSensor(uint8_t csPin) {
  writeRegister(csPin, REG_CTRL1_XL, 0x50);
  uint8_t ctrl6 = readRegister(csPin, REG_CTRL6_C);
  ctrl6 &= ~(1 << 4);
  writeRegister(csPin, REG_CTRL6_C, ctrl6);
  writeRegister(csPin, REG_CTRL3_C, 0x44);
  writeRegister(csPin, REG_CTRL8_XL, 0xC0);
  writeRegister(csPin, REG_CTRL2_G, 0x00);
}

// =============================================================================
// SPIKE FILTER FUNCTIONS
// =============================================================================
float calculateMedian(float arr[], int size) {
  float temp[SPIKE_FILTER_SIZE];
  for (int i = 0; i < size; i++)
    temp[i] = arr[i];
  for (int i = 0; i < size - 1; i++) {
    for (int j = 0; j < size - i - 1; j++) {
      if (temp[j] > temp[j + 1]) {
        float swap = temp[j];
        temp[j] = temp[j + 1];
        temp[j + 1] = swap;
      }
    }
  }
  return temp[size / 2];
}

bool isSpike(float newPGA) {
  if (newPGA > MAX_VALID_PGA || newPGA < 0 || isnan(newPGA) || isinf(newPGA)) {
    spikeCount++;
    return true;
  }
  if (lastValidPGA < 0.005 && newPGA > 0.1) {
    if (bufferFilled) {
      float median = calculateMedian(pgaBuffer, SPIKE_FILTER_SIZE);
      if (median < 0.01 && newPGA > median * 10) {
        spikeCount++;
        return true;
      }
    } else {
      spikeCount++;
      return true;
    }
  }
  pgaBuffer[pgaBufferIndex] = newPGA;
  pgaBufferIndex = (pgaBufferIndex + 1) % SPIKE_FILTER_SIZE;
  if (pgaBufferIndex == 0)
    bufferFilled = true;
  lastValidPGA = newPGA;
  return false;
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================
const char *getIntensityLevel(float pga) {
  if (pga >= PGA_THRESHOLD_SEVERE)
    return "SEVERE";
  if (pga >= PGA_THRESHOLD_HIGH)
    return "HIGH";
  if (pga >= PGA_THRESHOLD_MEDIUM)
    return "MEDIUM";
  if (pga >= PGA_THRESHOLD_LOW)
    return "LOW";
  return "NONE";
}

String getRichterEstimation(float pga) {
  if (pga >= 1.0)
    return "6.0+";
  else if (pga >= 0.5)
    return "5.0-6.0";
  else if (pga >= 0.2)
    return "4.0-5.0";
  else if (pga >= 0.08)
    return "3.0-4.0";
  else if (pga >= 0.03)
    return "2.0-3.0";
  else
    return "<2.0";
}

String getStationId() {
  uint64_t chipId = ESP.getEfuseMac();
  return "GEO_S3_" + String((uint32_t)(chipId >> 32), HEX) +
         String((uint32_t)chipId, HEX);
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "--:--:--";
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

String getFormattedDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "--/--/----";
  char buffer[11];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y", &timeinfo);
  return String(buffer);
}

void updateLED() {
  unsigned long now = millis();
  switch (currentLedMode) {
  case LED_OFF:
    digitalWrite(LED_PIN, LOW);
    break;
  case LED_ON:
    digitalWrite(LED_PIN, HIGH);
    break;
  case LED_WIFI_CONNECTING:
  case LED_MQTT_CONNECTING:
    if (now - lastLedBlink >= 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = now;
    }
    break;
  case LED_PGA_EVENT:
    if (now - lastLedBlink >= 50) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = now;
    }
    break;
  case LED_MONITORING:
  case LED_WIFI_CONNECTED:
  case LED_MQTT_CONNECTED: {
    int brightness = (sin(now / 500.0) + 1) * 127;
    analogWrite(LED_PIN, brightness);
  } break;
  default:
    if (now - lastLedBlink >= 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      lastLedBlink = now;
    }
  }
}

void setLedMode(LedMode mode) {
  if (currentLedMode != mode) {
    currentLedMode = mode;
    lastLedBlink = millis();
  }
}

// =============================================================================
// ARIAS INTENSITY
// =============================================================================
void calculateAriasIntensity(float acceleration) {
  unsigned long currentMicros = micros();
  float dt = (currentMicros - lastSampleMicros) / 1000000.0;
  lastSampleMicros = currentMicros;
  if (dt > 0.1 || dt <= 0.0)
    dt = 0.005;

  float newEnergy = (acceleration * acceleration * 9.81 * 9.81) * dt;
  float oldEnergy = ariasBuffer[ariasIndex];
  runningAriasSum -= oldEnergy;
  ariasBuffer[ariasIndex] = newEnergy;
  runningAriasSum += newEnergy;
  if (runningAriasSum < 0)
    runningAriasSum = 0;
  ariasIndex = (ariasIndex + 1) % ARIAS_BUFFER_SIZE;

  if (ariasIndex == 0) {
    runningAriasSum = 0.0;
    for (int i = 0; i < ARIAS_BUFFER_SIZE; i++)
      runningAriasSum += ariasBuffer[i];
  }
  ariasIntensity = ARIAS_FACTOR * runningAriasSum;
}

// =============================================================================
// EEPROM FUNCTIONS
// =============================================================================
bool loadCalibrationFromEEPROM() {
  CalibrationData data;
  EEPROM.get(EEPROM_CAL_ADDR, data);
  if (data.magic != 0xCA11)
    return false;
  offX1 = data.offX1;
  offY1 = data.offY1;
  offZ1 = data.offZ1;
  offX2 = data.offX2;
  offY2 = data.offY2;
  offZ2 = data.offZ2;
  Serial.println("[OK] Calibration loaded from EEPROM");
  return true;
}

void saveCalibrationToEEPROM() {
  CalibrationData data;
  data.magic = 0xCA11;
  data.offX1 = offX1;
  data.offY1 = offY1;
  data.offZ1 = offZ1;
  data.offX2 = offX2;
  data.offY2 = offY2;
  data.offZ2 = offZ2;
  data.timestamp = millis();
  EEPROM.put(EEPROM_CAL_ADDR, data);
  EEPROM.commit();
  Serial.println("[OK] Calibration saved to EEPROM");
}

void performCalibration() {
  Serial.println("\n[...] Calibrating sensors...");
  delay(1000);
  long s1X = 0, s1Y = 0, s1Z = 0, s2X = 0, s2Y = 0, s2Z = 0;
  const int samples = 1000;
  for (int i = 0; i < samples; i++) {
    s1X += (int16_t)((readRegister(PIN_CS1, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTX_L_XL));
    s1Y += (int16_t)((readRegister(PIN_CS1, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTY_L_XL));
    s1Z += (int16_t)((readRegister(PIN_CS1, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTZ_L_XL));
    s2X += (int16_t)((readRegister(PIN_CS2, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTX_L_XL));
    s2Y += (int16_t)((readRegister(PIN_CS2, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTY_L_XL));
    s2Z += (int16_t)((readRegister(PIN_CS2, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTZ_L_XL));
    if (i % 100 == 0)
      Serial.print(".");
    delay(2);
  }
  offX1 = (s1X / (float)samples) * factor;
  offY1 = (s1Y / (float)samples) * factor;
  offZ1 = (s1Z / (float)samples) * factor;
  offX2 = (s2X / (float)samples) * factor;
  offY2 = (s2Y / (float)samples) * factor;
  offZ2 = (s2Z / (float)samples) * factor;
  Serial.println(" Done!");
  saveCalibrationToEEPROM();
  discriminator.reset();
}

bool loadWiFiCredentials() {
  if (EEPROM.read(EEPROM_SSID_ADDR - 1) != EEPROM_MAGIC_BYTE)
    return false;
  char ssidBuf[MAX_SSID_LENGTH + 1], passBuf[MAX_PASS_LENGTH + 1];
  for (int i = 0; i < MAX_SSID_LENGTH; i++) {
    ssidBuf[i] = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (ssidBuf[i] == '\0')
      break;
  }
  ssidBuf[MAX_SSID_LENGTH] = '\0';
  for (int i = 0; i < MAX_PASS_LENGTH; i++) {
    passBuf[i] = EEPROM.read(EEPROM_PASS_ADDR + i);
    if (passBuf[i] == '\0')
      break;
  }
  passBuf[MAX_PASS_LENGTH] = '\0';
  wifiSSID = String(ssidBuf);
  wifiPassword = String(passBuf);
  return wifiSSID.length() > 0;
}

void saveWiFiCredentials(String ssid, String password) {
  EEPROM.write(EEPROM_SSID_ADDR - 1, EEPROM_MAGIC_BYTE);
  int i;
  for (i = 0; i < ssid.length() && i < MAX_SSID_LENGTH; i++)
    EEPROM.write(EEPROM_SSID_ADDR + i, ssid[i]);
  EEPROM.write(EEPROM_SSID_ADDR + i, '\0');
  for (i = 0; i < password.length() && i < MAX_PASS_LENGTH; i++)
    EEPROM.write(EEPROM_PASS_ADDR + i, password[i]);
  EEPROM.write(EEPROM_PASS_ADDR + i, '\0');
  EEPROM.commit();
}

void clearWiFiCredentials() {
  EEPROM.write(EEPROM_SSID_ADDR - 1, 0x00);
  EEPROM.commit();
}

void loadStationConfig() {
  char nameBuf[MAX_NAME_LENGTH + 1];
  bool found = false;
  for (int i = 0; i < MAX_NAME_LENGTH; i++) {
    nameBuf[i] = EEPROM.read(EEPROM_NAME_ADDR + i);
    if (nameBuf[i] == '\0') {
      found = true;
      break;
    }
  }
  nameBuf[MAX_NAME_LENGTH] = '\0';
  if (found && strlen(nameBuf) > 0)
    stationName = String(nameBuf);
  float lat, lon;
  EEPROM.get(EEPROM_LAT_ADDR, lat);
  EEPROM.get(EEPROM_LON_ADDR, lon);
  if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
    stationLat = lat;
    stationLon = lon;
  }
}

void saveStationName(String name) {
  for (int i = 0; i < name.length() && i < MAX_NAME_LENGTH; i++)
    EEPROM.write(EEPROM_NAME_ADDR + i, name[i]);
  EEPROM.write(EEPROM_NAME_ADDR + name.length(), '\0');
  EEPROM.commit();
}

void saveStationCoordinates(float lat, float lon) {
  EEPROM.put(EEPROM_LAT_ADDR, lat);
  EEPROM.put(EEPROM_LON_ADDR, lon);
  EEPROM.commit();
}

// =============================================================================
// WiFi FUNCTIONS
// =============================================================================
void setupWiFi() {
  Serial.println("\n[...] Connecting to WiFi...");
  setLedMode(LED_WIFI_CONNECTING);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    updateLED();
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    setLedMode(LED_WIFI_CONNECTED);
  } else {
    Serial.println("\n[FAIL] WiFi failed! Starting provisioning mode...");
    setLedMode(LED_ERROR);
    startProvisioningMode();
  }
}

void startProvisioningMode() {
  Serial.println("\n========================================");
  Serial.println("  WiFi Provisioning Mode");
  Serial.println("  Connect to: GeoLink-216");
  Serial.println("  Open: http://192.168.4.1");
  Serial.println("========================================\n");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  setLedMode(LED_WIFI_CONNECTING);
  server.on("/", HTTP_GET, handleProvisioningRoot);
  server.on("/provision", HTTP_POST, handleProvisioningConfig);
  server.on("/status", HTTP_GET, handleProvisioningStatus);
  server.enableCORS(true);
  server.begin();
  while (true) {
    server.handleClient();
    updateLED();
    delay(10);
  }
}

void handleProvisioningRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' "
                "content='width=device-width,initial-scale=1'>";
  html += "<title>GeoLink-S3</"
          "title><style>body{font-family:Arial;background:#1a1a2e;color:#fff;"
          "margin:20px;}";
  html += ".container{background:#16213e;padding:20px;border-radius:10px;max-"
          "width:400px;margin:auto;}";
  html += "input{width:100%;padding:12px;margin:10px "
          "0;border-radius:5px;border:none;box-sizing:border-box;}";
  html += "button{background:#3282b8;color:#fff;padding:15px;border:none;"
          "border-radius:5px;width:100%;cursor:pointer;}</style></head>";
  html += "<body><div class='container'><h1>🌍 GeoLink-S3</h1><p>Station: " +
          stationId + "</p>";
  html += "<form id='f'><input type='text' id='s' placeholder='WiFi SSID' "
          "required>";
  html += "<input type='password' id='p' placeholder='WiFi Password'>";
  html += "<button type='submit'>Connect</button></form><p id='st'></p></div>";
  html += "<script>document.getElementById('f').onsubmit=function(e){e."
          "preventDefault();";
  html += "document.getElementById('st').innerHTML='Connecting...';";
  html +=
      "fetch('/provision',{method:'POST',headers:{'Content-Type':'application/"
      "json'},";
  html += "body:JSON.stringify({ssid:document.getElementById('s').value,"
          "password:document.getElementById('p').value})})";
  html += ".then(r=>r.json()).then(d=>{document.getElementById('st').innerHTML="
          "d.success?'✅ Yeniden başlatılıyor...':'❌ "
          "'+d.message;});};</script></body></html>";
  server.send(200, "text/html", html);
}

void handleProvisioningConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false}");
    return;
  }
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"success\":false}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String pass = doc["password"] | "";
  if (ssid.length() == 0) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"SSID is empty\"}");
    return;
  }
  saveWiFiCredentials(ssid, pass);
  StaticJsonDocument<128> res;
  res["success"] = true;
  res["stationId"] = stationId;
  String response;
  serializeJson(res, response);
  server.send(200, "application/json", response);
  delay(3000);
  ESP.restart();
}

void handleProvisioningStatus() {
  StaticJsonDocument<128> doc;
  doc["stationId"] = stationId;
  doc["mode"] = "provisioning";
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// =============================================================================
// MQTT FUNCTIONS
// =============================================================================
void setupMQTT() {
  Serial.println("\n[...] Configuring MQTT...");
  espClient.setInsecure();
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(2048);
  mqttReconnect();
}

void mqttReconnect() {
  if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    Serial.println("[FAIL] MQTT max attempts! Restarting...");
    delay(5000);
    ESP.restart();
  }
  Serial.print("[...] Connecting to MQTT...");
  setLedMode(LED_MQTT_CONNECTING);
  StaticJsonDocument<256> lwtDoc;
  lwtDoc["stationId"] = stationId;
  lwtDoc["status"] = "offline";
  String lwtMessage;
  serializeJson(lwtDoc, lwtMessage);
  String clientId = "seismic-" + stationId;
  if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password,
                         topicLWT.c_str(), 1, true, lwtMessage.c_str())) {
    Serial.println(" [OK] Connected!");
    setLedMode(LED_MONITORING);
    reconnectAttempts = 0;
    mqttClient.subscribe(topicCommands.c_str(), 1);
    mqttClient.subscribe(topicOTA.c_str(), 1);
    publishStatus();
  } else {
    Serial.printf(" [FAIL] Error: %d\n", mqttClient.state());
    reconnectAttempts++;
    setLedMode(LED_ERROR);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length))
    return;
  if (String(topic) == topicOTA) {
    performOTA(doc["url"] | "", doc["version"] | "");
    return;
  }
  handleCommand(doc["command"] | "", doc["value"] | "");
}

void handleCommand(String command, String value) {
  if (command == "restart") {
    delay(1000);
    ESP.restart();
  } else if (command == "status") {
    publishStatus();
  } else if (command == "calibrate") {
    performCalibration();
  } else if (command == "set_name" && value.length() > 0) {
    stationName = value;
    saveStationName(value);
    publishStatus();
  } else if (command == "clear_wifi" && value == "confirm") {
    clearWiFiCredentials();
    delay(2000);
    ESP.restart();
  }
}

void performOTA(String url, String version) {
  if (url.length() == 0 || version == String(firmwareVersion))
    return;
  Serial.println("[...] Starting OTA update...");
  mqttClient.disconnect();
  delay(1000);
  WiFiClientSecure client;
  client.setInsecure();
  t_httpUpdate_return ret = httpUpdate.update(client, url);
  if (ret == HTTP_UPDATE_OK) {
    Serial.println("[OK] OTA Successful!");
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("[FAIL] OTA Error!");
    mqttReconnect();
  }
}

// =============================================================================
// PUBLISH FUNCTIONS
// =============================================================================
void publishTelemetry() {
  unsigned long currentTime = millis();
  bool hasActivity =
      (currentPGA > 0.0 || ariasIntensity >= 0.01 || currentWaveType != "None");
  unsigned long interval =
      hasActivity ? TELEMETRY_ACTIVE_INTERVAL : TELEMETRY_IDLE_INTERVAL;
  if ((currentTime - lastTelemetryPublish) < interval)
    return;
  int idx = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;
  if (offlineMode || !mqttClient.connected()) {
    if (hasActivity)
      addToOfflineBuffer(ENTRY_TELEMETRY, currentPGA, ariasIntensity,
                         axisEastBuffer[idx], axisNorthBuffer[idx],
                         axisUpBuffer[idx], 0.0, currentWaveType.c_str());
    lastTelemetryPublish = currentTime;
    return;
  }
  StaticJsonDocument<512> doc;
  doc["stationId"] = stationId;
  doc["timestamp"] = time(nullptr);
  doc["pga"] = round(currentPGA * 1000) / 1000.0;
  doc["arias"] = round(ariasIntensity * 1000) / 1000.0;
  doc["waveType"] = currentWaveType;
  JsonObject axes = doc.createNestedObject("axes");
  axes["e"] = round(axisEastBuffer[idx] * 9.81 * 1000) / 1000.0;
  axes["n"] = round(axisNorthBuffer[idx] * 9.81 * 1000) / 1000.0;
  axes["u"] = round(axisUpBuffer[idx] * 9.81 * 1000) / 1000.0;
  doc["rssi"] = WiFi.RSSI();
  String message;
  serializeJson(doc, message);
  if (mqttClient.publish(topicTelemetry.c_str(), message.c_str(), false)) {
    totalTelemetryPublished++;
    lastTelemetryPublish = currentTime;
  }
}

void publishStatus() {
  if (!mqttClient.connected())
    return;
  StaticJsonDocument<512> doc;
  doc["stationId"] = stationId;
  doc["stationName"] = stationName;
  doc["status"] = "online";
  doc["timestamp"] = time(nullptr);
  doc["firmwareVersion"] = firmwareVersion;
  doc["deviceModel"] = deviceModel;
  doc["sensor"] = sensorType;
  doc["controller"] = controllerType;
  doc["rssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;
  JsonObject stats = doc.createNestedObject("statistics");
  stats["uptime"] = (millis() - systemStartTime) / 1000;
  stats["totalPgaEvents"] = totalPgaEvents;
  stats["totalAriasEvents"] = totalAriasEvents;
  String message;
  serializeJson(doc, message);
  mqttClient.publish(topicStatus.c_str(), message.c_str(), true);
}

void publishPgaEvent() {
  int idx = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;
  if (offlineMode || !mqttClient.connected()) {
    addToOfflineBuffer(ENTRY_PGA_EVENT, currentPGA, ariasIntensity,
                       axisEastBuffer[idx], axisNorthBuffer[idx],
                       axisUpBuffer[idx], currentEventMax,
                       currentWaveType.c_str());
    return;
  }
  StaticJsonDocument<512> doc;
  doc["stationId"] = stationId;
  doc["eventType"] = "pga";
  doc["timestamp"] = time(nullptr);
  doc["peakPGA"] = round(currentEventMax * 1000) / 1000.0;
  doc["waveType"] = currentWaveType;
  doc["richterEstimate"] = getRichterEstimation(currentEventMax);
  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;
  String message;
  serializeJson(doc, message);
  mqttClient.publish(topicEventPGA.c_str(), message.c_str(), false);
}

void publishAriasEvent() {
  int idx = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;
  if (offlineMode || !mqttClient.connected()) {
    addToOfflineBuffer(ENTRY_ARIAS_EVENT, currentPGA, ariasIntensity,
                       axisEastBuffer[idx], axisNorthBuffer[idx],
                       axisUpBuffer[idx], currentAriasEventMax, "Arias");
    return;
  }
  StaticJsonDocument<512> doc;
  doc["stationId"] = stationId;
  doc["eventType"] = "arias";
  doc["timestamp"] = time(nullptr);
  doc["peakArias"] = round(currentAriasEventMax * 1000) / 1000.0;
  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;
  String message;
  serializeJson(doc, message);
  mqttClient.publish(topicEventArias.c_str(), message.c_str(), false);
}

// =============================================================================
// OFFLINE BUFFER FUNCTIONS
// =============================================================================
void initOfflineBuffer() {
  for (int i = 0; i < OFFLINE_BUFFER_SIZE; i++)
    memset(&offlineBuffer[i], 0, sizeof(OfflineEntry));
  bufferHead = bufferTail = bufferCount = 0;
}

bool addToOfflineBuffer(BufferEntryType type, float pgaVal, float ariasVal,
                        float e, float n, float u, float peak,
                        const char *wave) {
  if (type == ENTRY_TELEMETRY &&
      (millis() - lastOfflineSave) < OFFLINE_SAVE_INTERVAL)
    return false;
  if (bufferCount >= OFFLINE_BUFFER_SIZE) {
    bufferTail = (bufferTail + 1) % OFFLINE_BUFFER_SIZE;
    bufferCount--;
  }
  OfflineEntry *entry = &offlineBuffer[bufferHead];
  entry->entryType = (uint8_t)type;
  entry->timestamp = time(nullptr);
  entry->pga = pgaVal;
  entry->arias = ariasVal;
  entry->axisE = e;
  entry->axisN = n;
  entry->axisU = u;
  entry->peakValue = peak;
  entry->rssi = (int8_t)WiFi.RSSI();
  strncpy(entry->waveType, wave, sizeof(entry->waveType) - 1);
  bufferHead = (bufferHead + 1) % OFFLINE_BUFFER_SIZE;
  bufferCount++;
  lastOfflineSave = millis();
  return true;
}

bool hasOfflineData() { return bufferCount > 0; }

bool getFromOfflineBuffer(OfflineEntry *entry) {
  if (bufferCount == 0)
    return false;
  memcpy(entry, &offlineBuffer[bufferTail], sizeof(OfflineEntry));
  bufferTail = (bufferTail + 1) % OFFLINE_BUFFER_SIZE;
  bufferCount--;
  return true;
}

void flushOfflineBuffer() {
  if (!mqttClient.connected() || bufferCount == 0)
    return;
  Serial.printf("[...] Sending %d buffered records...\n", bufferCount);
  OfflineEntry entry;
  while (getFromOfflineBuffer(&entry)) {
    StaticJsonDocument<512> doc;
    doc["stationId"] = stationId;
    doc["timestamp"] = entry.timestamp;
    doc["buffered"] = true;
    doc["pga"] = entry.pga;
    doc["arias"] = entry.arias;
    String topic = topicTelemetry;
    if (entry.entryType == ENTRY_PGA_EVENT)
      topic = topicEventPGA;
    else if (entry.entryType == ENTRY_ARIAS_EVENT)
      topic = topicEventArias;
    String message;
    serializeJson(doc, message);
    mqttClient.publish(topic.c_str(), message.c_str(), false);
    delay(50);
    mqttClient.loop();
  }
  Serial.println("[OK] Buffer sent");
}

// =============================================================================
// PRINT STATUS
// =============================================================================
void printStatus() {
  unsigned long uptime = (millis() - startTime) / 1000;
  Serial.println();
  Serial.println("=============== STATUS ===============");
  Serial.printf("  Uptime      : %lu sec\n", uptime);
  Serial.printf("  Samples     : %lu\n", sampleCount);
  Serial.printf("  PGA Events  : %d\n", totalPgaEvents);
  Serial.printf("  Spikes      : %lu\n", spikeCount);
  Serial.printf("  Max PGA     : %.5f g\n", sessionMaxPGA);
  Serial.printf("  Current PGA : %.5f g\n", currentPGA);
  Serial.printf("  Arias       : %.5f\n", ariasIntensity);
  Serial.printf("  Wave        : %s\n", currentWaveType.c_str());
  Serial.printf("  WiFi        : %d dBm\n", WiFi.RSSI());
  Serial.printf("  MQTT        : %s\n",
                mqttClient.connected() ? "Connected" : "Disconnected");
  Serial.println("======================================");
  Serial.println();
}

void printSeismicAlert(const char *level, float pga) {
  Serial.println();
  Serial.println("+==========================================+");
  Serial.println("|      SEISMIC ACTIVITY DETECTED           |");
  Serial.println("+==========================================+");
  Serial.printf("|  Level      : %-26s |\n", level);
  Serial.printf("|  PGA        : %.5f g                   |\n", pga);
  Serial.printf("|  Richter    : ~%s                       |\n",
                getRichterEstimation(pga).c_str());
  Serial.printf("|  Event #    : %d                          |\n",
                totalPgaEvents);
  Serial.println("+==========================================+");
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("+==========================================+");
  Serial.println("|   IoT SEISMIC MONITOR - ESP32-S3         |");
  Serial.println("|   Dual LSM6DS3 + MQTT + Provisioning     |");
  Serial.println("+==========================================+");
  Serial.println();

  EEPROM.begin(EEPROM_SIZE);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  tone(BUZZER_PIN, 2000, 100);
  delay(150);
  tone(BUZZER_PIN, 2500, 100);

  stationId = getStationId();
  Serial.print("Station ID: ");
  Serial.println(stationId);

  // MQTT Topics
  topicStatus = "seismic/stations/" + stationId + "/status";
  topicTelemetry = "seismic/stations/" + stationId + "/telemetry";
  topicEventPGA = "seismic/stations/" + stationId + "/events/pga";
  topicEventArias = "seismic/stations/" + stationId + "/events/arias";
  topicCommands = "seismic/stations/" + stationId + "/commands";
  topicOTA = "seismic/stations/" + stationId + "/ota";
  topicLWT = "seismic/stations/" + stationId + "/lwt";

  // Initialize SPI
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  pinMode(PIN_CS1, OUTPUT);
  pinMode(PIN_CS2, OUTPUT);
  digitalWrite(PIN_CS1, HIGH);
  digitalWrite(PIN_CS2, HIGH);
  delay(500);

  // Sensor 1
  uint8_t id1 = readRegister(PIN_CS1, REG_WHO_AM_I);
  if (id1 == 0x69) {
    Serial.println("[OK] Sensor 1 detected");
    configureSensor(PIN_CS1);
  } else {
    Serial.printf("[FAIL] Sensor 1 error: 0x%02X\n", id1);
    while (1)
      ;
  }

  // Sensor 2
  uint8_t id2 = readRegister(PIN_CS2, REG_WHO_AM_I);
  if (id2 == 0x69) {
    Serial.println("[OK] Sensor 2 detected");
    configureSensor(PIN_CS2);
  } else {
    Serial.printf("[FAIL] Sensor 2 error: 0x%02X\n", id2);
    while (1)
      ;
  }

  // Calibration
  if (!loadCalibrationFromEEPROM()) {
    Serial.println("[!] No calibration found");
    performCalibration();
  }

  // Buffers
  for (int i = 0; i < ARIAS_BUFFER_SIZE; i++)
    ariasBuffer[i] = 0.0;
  for (int i = 0; i < CHART_BUFFER_SIZE; i++) {
    axisEastBuffer[i] = 0.0;
    axisNorthBuffer[i] = 0.0;
    axisUpBuffer[i] = 0.0;
  }
  initOfflineBuffer();

  // WiFi
  isProvisioned = loadWiFiCredentials();
  if (!isProvisioned) {
    Serial.println("[!] No WiFi credentials found");
    startProvisioningMode();
    return;
  }
  loadStationConfig();
  setupWiFi();

  // NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("[OK] NTP configured");

  // MQTT
  setupMQTT();

  systemStartTime = millis();
  startTime = millis();
  setLedMode(LED_MONITORING);

  Serial.println("\n========================================");
  Serial.println("  SYSTEM READY - Seismic monitoring started!");
  Serial.println("========================================\n");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  updateLED();

  // MQTT Connection
  if (!mqttClient.connected()) {
    if (!offlineMode) {
      offlineMode = true;
      Serial.println("[OFFLINE] Disconnected from MQTT");
    }
    if (millis() - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = millis();
      mqttReconnect();
    }
  } else {
    if (offlineMode) {
      offlineMode = false;
      Serial.println("[ONLINE] Connected to MQTT");
      flushOfflineBuffer();
    }
    mqttClient.loop();
  }

  if (webServerEnabled)
    server.handleClient();

  // Serial Commands
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
    case 'q':
    case 'Q':
      printEnabled = !printEnabled;
      Serial.printf("\nOutput: %s\n\n", printEnabled ? "ON" : "OFF");
      break;
    case 's':
    case 'S':
      printStatus();
      break;
    case 'c':
    case 'C':
      performCalibration();
      break;
    case 'h':
    case 'H':
    case '?':
      Serial.println("\n========= COMMANDS =========");
      Serial.println("  q - Toggle output");
      Serial.println("  s - Status");
      Serial.println("  c - Calibrate");
      Serial.println("  h - Help");
      Serial.println("===========================\n");
      break;
    }
  }

  // Read Sensors
  uint8_t status1 = readRegister(PIN_CS1, REG_STATUS);
  uint8_t status2 = readRegister(PIN_CS2, REG_STATUS);
  if (!((status1 & 0x01) && (status2 & 0x01))) {
    delayMicroseconds(100);
    return;
  }

  int16_t rX1 = (readRegister(PIN_CS1, REG_OUTX_H_XL) << 8) |
                readRegister(PIN_CS1, REG_OUTX_L_XL);
  int16_t rY1 = (readRegister(PIN_CS1, REG_OUTY_H_XL) << 8) |
                readRegister(PIN_CS1, REG_OUTY_L_XL);
  int16_t rZ1 = (readRegister(PIN_CS1, REG_OUTZ_H_XL) << 8) |
                readRegister(PIN_CS1, REG_OUTZ_L_XL);
  int16_t rX2 = (readRegister(PIN_CS2, REG_OUTX_H_XL) << 8) |
                readRegister(PIN_CS2, REG_OUTX_L_XL);
  int16_t rY2 = (readRegister(PIN_CS2, REG_OUTY_H_XL) << 8) |
                readRegister(PIN_CS2, REG_OUTY_L_XL);
  int16_t rZ2 = (readRegister(PIN_CS2, REG_OUTZ_H_XL) << 8) |
                readRegister(PIN_CS2, REG_OUTZ_L_XL);

  float vX1 = (rX1 * factor) - offX1, vY1 = (rY1 * factor) - offY1,
        vZ1 = (rZ1 * factor) - offZ1;
  float vX2 = (rX2 * factor) - offX2, vY2 = (rY2 * factor) - offY2,
        vZ2 = (rZ2 * factor) - offZ2;

  float avgX = (vX1 + vX2) / 2.0, avgY = (vY1 + vY2) / 2.0,
        avgZ = (vZ1 + vZ2) / 2.0;

  float rawPGA = sqrt(avgX * avgX + avgY * avgY + avgZ * avgZ);
  sampleCount++;

  if (isSpike(rawPGA)) {
    delayMicroseconds(4800);
    return;
  }

  currentPGA = rawPGA;
  if (currentPGA < 0.005)
    currentPGA = 0.0;

  // STA/LTA
  SeismicDiscriminator::Result waveResult =
      discriminator.update(avgX, avgY, avgZ);
  currentWaveType = waveResult.waveType;
  estimatedDistanceKm = waveResult.estimatedDistanceKm;
  staLtaRatioZ = waveResult.verticalRatio;
  staLtaRatioH = waveResult.horizontalRatio;

  // Chart Buffer
  axisEastBuffer[chartIndex] = avgX;
  axisNorthBuffer[chartIndex] = avgY;
  axisUpBuffer[chartIndex] = avgZ;
  chartIndex = (chartIndex + 1) % CHART_BUFFER_SIZE;

  // Arias
  calculateAriasIntensity(currentPGA);

  if (currentPGA > sessionMaxPGA)
    sessionMaxPGA = currentPGA;

  // PGA Olay
  unsigned long now = millis();
  if (currentPGA >= PGA_EVENT_THRESHOLD) {
    lastPgaAboveThreshold = now;
    confirmCounter++;
    if (!isPgaEventActive && confirmCounter >= EVENT_CONFIRM_COUNT) {
      if (now - pgaEventStartTime > PGA_COOLDOWN_MS || pgaEventStartTime == 0) {
        isPgaEventActive = true;
        pgaEventStartTime = now;
        currentEventMax = currentPGA;
        totalPgaEvents++;
        setLedMode(LED_PGA_EVENT);
        tone(BUZZER_PIN, 3000, 200);
        printSeismicAlert(getIntensityLevel(currentPGA), currentPGA);
        publishPgaEvent();
      }
    } else if (isPgaEventActive && currentPGA > currentEventMax) {
      currentEventMax = currentPGA;
    }
  } else {
    confirmCounter = 0;
    if (isPgaEventActive && (now - lastPgaAboveThreshold) > PGA_COOLDOWN_MS) {
      isPgaEventActive = false;
      setLedMode(LED_MONITORING);
    }
  }

  // Arias Olay
  if (ariasIntensity >= ARIAS_EVENT_THRESHOLD) {
    lastAriasAboveThreshold = now;
    if (!isAriasEventActive) {
      isAriasEventActive = true;
      ariasEventStartTime = now;
      currentAriasEventMax = ariasIntensity;
      totalAriasEvents++;
      publishAriasEvent();
    } else if (ariasIntensity > currentAriasEventMax) {
      currentAriasEventMax = ariasIntensity;
    }
  }
  if (isAriasEventActive &&
      (now - lastAriasAboveThreshold) > ARIAS_COOLDOWN_MS) {
    isAriasEventActive = false;
    currentAriasEventMax = 0.0;
  }

  // Telemetry
  if (mqttClient.connected()) {
    publishTelemetry();
    if (now - lastStatusPublish >= STATUS_INTERVAL) {
      lastStatusPublish = now;
      publishStatus();
    }
  }

  delayMicroseconds(4800);
}