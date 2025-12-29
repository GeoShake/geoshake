/**
 * =============================================================================
 * GEOSHAKE IoT SEISMIC MONITORING STATION - ESP8266 + Dual LSM6DS3 (SPI)
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

#include <SPI.h>
#include <Wire.h> // Keep Wire if needed by other libs, but usually SPI replaces it for sensors.
// Checking imports: ArduinoJson, PubSub don't need Wire.
// Actually, let's just remove Wire.h if we are strictly SPI.
// But let's check if any other component uses Wire. Only LSM6DS3 was using it.
// Replacing includes:
#include "SeismicDiscriminator.h"
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>

// --- ESP8266 NodeMCU Pins ---
// Common SPI Pins (Fixed)
#define PIN_SCL 14 // D5 (Clock)
#define PIN_SDO 12 // D6 (MISO)
#define PIN_SDA 13 // D7 (MOSI)

// Separate Chip Select (CS) Pins
#define PIN_CS1 16 // D0 -> Sensor 1
#define PIN_CS2 4  // D2 -> Sensor 2

// --- LSM6DS3 Register Addresses ---
#define REG_WHO_AM_I 0x0F
#define REG_CTRL1_XL 0x10
#define REG_CTRL2_G 0x11
#define REG_CTRL3_C 0x12
#define REG_CTRL6_C 0x15
#define REG_CTRL8_XL 0x17
#define REG_STATUS 0x1E
#define REG_TAP_CFG 0x58

// Data Registers
#define REG_OUTX_L_XL 0x28
#define REG_OUTX_H_XL 0x29
#define REG_OUTY_L_XL 0x2A
#define REG_OUTY_H_XL 0x2B
#define REG_OUTZ_L_XL 0x2C
#define REG_OUTZ_H_XL 0x2D

#include "SeismicDiscriminator.h" // NEW LOGIC
#include <ESP8266httpUpdate.h>    // OTA Updates for ESP8266

// ============================================
// WiFi Provisioning Configuration
// ============================================
const char *AP_SSID =
    "GeoLink-216"; // ⚠️ Don't change if you want to use Mobile app provisioning
const char *AP_PASSWORD =
    ""; // ⚠️ Don't change if you want to use Mobile app provisioning
const IPAddress AP_IP(192, 168, 4, 1); // AP IP address
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

// EEPROM Configuration
#define EEPROM_SIZE 512
#define EEPROM_MAGIC_BYTE 0x47 // 'G' for GeoLink
#define EEPROM_SSID_ADDR 1
#define EEPROM_PASS_ADDR 65  // SSID max 64 bytes
#define EEPROM_NAME_ADDR 129 // Password + 64 bytes
#define EEPROM_LAT_ADDR 193  // Name + 64 bytes
#define EEPROM_LON_ADDR 197  // After lat (4 bytes float)
#define MAX_SSID_LENGTH 64
#define MAX_PASS_LENGTH 64
#define MAX_NAME_LENGTH 64

#define BUZZER_PIN D1 // GPIO5 (Moved from D5 to avoid SPI conflict)
// WiFi credentials (will be loaded from EEPROM or provisioned)
String wifiSSID = "";
String wifiPassword = "";
bool isProvisioned = false;

// ============================================
// MQTT Configuration (HiveMQ Cloud)
// ============================================
const char *mqtt_broker = "144c5ef5900847dab4640521d5cad06c.s1.eu.hivemq.cloud";
const int mqtt_port = 8883; // TLS port
const char *mqtt_username =
    "xxxxxx"; // get your username and password from
              // https://www.geoshake.org/join-network.html
const char *mqtt_password = "xxxxxx";

// Station Configuration
String stationId = ""; // Will be generated from ESP chip ID
String stationName =
    "Geostation-ESP8266";     // ⚠️ You can customize this on mobile app
float stationLat = 37.392869; // ⚠️ You can customize this on mobile app
float stationLon = 34.629968; // ⚠️ You can customize this on mobile app
const char *firmwareVersion =
    "2.0.0"; // ⚠️ Don't change this for firmware updates

// Device Information (stored in EEPROM, can be updated via MQTT or mobile app)
String deviceModel = "GeoShake T1";           // Device model
String sensorType = "LSM6DS3 Dual";           // Sensor type and count
String controllerType = "NodeMCU ESP8266 v3"; // Controller type
String userId = "";                           // Owner user ID (null initially)
bool isApproved = false;                      // ⚠️ Don't change this

// MQTT Topics (will be constructed with station ID)
String topicStatus;
String topicTelemetry;
String topicEventPGA;
String topicEventArias;
String topicCommands;
String topicOTA; // Firmware Update Topic
String topicLWT; // Last Will and Testament

// MQTT Publishing Intervals
unsigned long lastStatusPublish = 0;
const unsigned long STATUS_INTERVAL = 30000; // Publish status every 30 seconds

// MQTT Connection
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;
const int MAX_RECONNECT_ATTEMPTS = 5;

// ============================================
// LED Configuration
// ============================================
const int LED_PIN = 15; // GPIO12 (D6 on NodeMCU)
unsigned long lastLedBlink = 0;
bool ledState = false;
enum LedMode {
  LED_OFF,
  LED_ON,
  LED_WIFI_CONNECTING, // Fast blink (100ms)
  LED_WIFI_CONNECTED,  // Slow pulse (2s)
  LED_MQTT_CONNECTING, // Medium blink (250ms)
  LED_MQTT_CONNECTED,  // Very slow pulse (3s)
  LED_PGA_EVENT,       // Very fast blink (50ms)
  LED_ARIAS_EVENT,     // Medium blink (200ms)
  LED_MONITORING,      // Breathing effect
  LED_ERROR            // SOS pattern
};
LedMode currentLedMode = LED_OFF;

// ============================================
// NTP Configuration
// ============================================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3 * 3600, 60000); // UTC+3

// ============================================
// Web Server (Optional - for local monitoring)
// ============================================
ESP8266WebServer server(80);
bool webServerEnabled = false; // Set to false to disable web server

// ============================================
// LSM6DS3 Sensor (SPI Mode)
// ============================================
// Removed Library Object

// Calibration Variables (Dual Sensor)
float offX1 = 0, offY1 = 0, offZ1 = 0;
float offX2 = 0, offY2 = 0, offZ2 = 0;
// Factor: 0.061 mg/LSB for 2g range -> 0.000061 g/LSB
float factor = 0.000061;

SeismicDiscriminator discriminator;

// Old offsets (legacy support variables, set to 0 as we handle offset in raw
// read now)
float offsetE = 0.0;
float offsetN = 0.0;
float offsetU = 0.0;

float pga = 0.0;
float currentEventMax = 0.0;
float lastEventPeak = 0.0;
String lastEventTime = "--:--:--";
String lastEventDate = "--/--/----";

// ============================================
// Arias Intensity Variables
// ============================================
float ariasIntensity = 0.0;
float currentEventArias = 0.0;
float lastEventArias = 0.0;
const int ARIAS_BUFFER_SIZE = 200;
float ariasBuffer[200];
int ariasIndex = 0;
const float ARIAS_FACTOR = 1.5708 / 19.62; // π/(2g)
unsigned long lastSampleMicros = 0;
float runningAriasSum = 0.0;
// ============================================
// Wave Type Detection Variables
// ============================================

// Wave Logic
String currentWaveType = "None";
float estimatedDistance = 0.0;

String waveType = "None";
String lastWaveType = "None";
float verticalIntensity = 0.0;
float horizontalIntensity = 0.0;
unsigned long waveDetectionTime = 0;
bool pWaveDetected = false;
bool sWaveDetected = false;
const float WAVE_THRESHOLD = 0.012;

// ============================================
// PGA Event Detection Variables
// ============================================
bool isPgaEventActive = false;
unsigned long lastPgaAboveThreshold = 0;
unsigned long pgaEventStartTime = 0;
float pgaEventDuration = 0.0;
const float PGA_EVENT_THRESHOLD = 0.01;
const unsigned long PGA_COOLDOWN_MS = 2000;

// ============================================
// Arias Event Detection Variables
// ============================================
bool isAriasEventActive = false;
unsigned long lastAriasAboveThreshold = 0;
unsigned long ariasEventStartTime = 0;
float ariasEventDuration = 0.0;
float currentAriasEventMax = 0.0;
float lastAriasEventPeak = 0.0;
String lastAriasEventTime = "--:--:--";
String lastAriasEventDate = "--/--/----";
const float ARIAS_EVENT_THRESHOLD = 0.009;
const unsigned long ARIAS_COOLDOWN_MS = 3000;

// ============================================
// ENU (East-North-Up) Axis Chart Data
// ============================================
const int CHART_BUFFER_SIZE = 60;
float axisEastBuffer[60];  // East (X-axis)
float axisNorthBuffer[60]; // North (Y-axis)
float axisUpBuffer[60];    // Up (Z-axis)
int chartIndex = 0;

// ============================================
// System Statistics
// ============================================
unsigned long systemStartTime = 0;
int totalPgaEvents = 0;
int totalAriasEvents = 0;
unsigned long totalTelemetryPublished = 0;
unsigned long mqttReconnects = 0;
unsigned long lastTelemetryPublish = 0;
const unsigned long TELEMETRY_ACTIVE_INTERVAL =
    2000; // 2 seconds when activity detected
const unsigned long TELEMETRY_IDLE_INTERVAL =
    60000; // 60 seconds when idle (no PGA)

// ============================================
// Offline Ring Buffer (Data Buffering)
// ============================================
// Stores critical data when WiFi/MQTT is disconnected
// Sends data in order when connection is restored

// Buffer entry types
enum BufferEntryType {
  ENTRY_TELEMETRY = 0,
  ENTRY_PGA_EVENT = 1,
  ENTRY_ARIAS_EVENT = 2
};

// Compact data structure for offline storage (~48 bytes per entry)
struct OfflineEntry {
  uint8_t
      entryType; // 1 byte: ENTRY_TELEMETRY, ENTRY_PGA_EVENT, ENTRY_ARIAS_EVENT
  uint32_t timestamp;  // 4 bytes: Unix timestamp
  float pga;           // 4 bytes: PGA value
  float arias;         // 4 bytes: Arias intensity
  float axisE;         // 4 bytes: East axis
  float axisN;         // 4 bytes: North axis
  float axisU;         // 4 bytes: Up axis
  float peakValue;     // 4 bytes: Peak PGA or Arias for events
  int8_t rssi;         // 1 byte: WiFi signal strength
  char waveType[12];   // 12 bytes: Wave type string
  uint8_t reserved[6]; // 6 bytes: Future use / alignment
};                     // Total: ~48 bytes

// Ring Buffer Configuration
const int OFFLINE_BUFFER_SIZE = 20; // Store up to 20 entries (~1KB RAM)
OfflineEntry offlineBuffer[OFFLINE_BUFFER_SIZE];
volatile int bufferHead = 0;       // Write position
volatile int bufferTail = 0;       // Read position
volatile int bufferCount = 0;      // Current entry count
bool offlineMode = false;          // True when MQTT is disconnected
unsigned long lastOfflineSave = 0; // Rate limiting for offline saves
const unsigned long OFFLINE_SAVE_INTERVAL = 5000; // Min 5 sec between saves
unsigned long totalOfflineEntries = 0;            // Statistics
unsigned long totalBufferOverflows = 0;           // Overflow count

// ============================================
// Forward Declarations
// ============================================
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
// Offline Buffer Functions
void initOfflineBuffer();
bool addToOfflineBuffer(BufferEntryType type, float pga, float arias, float e,
                        float n, float u, float peak, const char *wave);
bool hasOfflineData();
bool getFromOfflineBuffer(OfflineEntry *entry);
void flushOfflineBuffer();
void saveToOfflineBuffer();
String getRichterEstimation(float pga);
void calculateAriasIntensity(float acceleration);
void detectWaveType(float ax, float ay, float az);
// --- SPI HELPER FUNCTIONS (CS Pin Parameterized) ---

uint8_t readRegister(uint8_t csPin, uint8_t regAddr) {
  digitalWrite(csPin, LOW);           // Select sensor
  SPI.transfer(regAddr | 0x80);       // Read command
  uint8_t value = SPI.transfer(0x00); // Get data
  digitalWrite(csPin, HIGH);          // Release
  return value;
}

void writeRegister(uint8_t csPin, uint8_t regAddr, uint8_t data) {
  digitalWrite(csPin, LOW);  // Select sensor
  SPI.transfer(regAddr);     // Write command
  SPI.transfer(data);        // Send data
  digitalWrite(csPin, HIGH); // Release
}

// Sensor Configuration Function
void configureSensor(uint8_t csPin) {
  // 1. CTRL1_XL: 208Hz, 2g
  writeRegister(csPin, REG_CTRL1_XL, 0x50);

  // 2. CTRL6_C: High Performance Mode (Low Noise)
  uint8_t ctrl6 = readRegister(csPin, REG_CTRL6_C);
  ctrl6 &= ~(1 << 4);
  writeRegister(csPin, REG_CTRL6_C, ctrl6);

  // 3. CTRL3_C: BDU
  writeRegister(csPin, REG_CTRL3_C, 0x44);

  // 4. CTRL8_XL: LPF2 Composite Filter (ODR/9 ~23Hz)
  writeRegister(csPin, REG_CTRL8_XL, 0xC0);

  // 5. Others
  writeRegister(csPin, REG_TAP_CFG, 0x00);
  writeRegister(csPin, REG_CTRL2_G, 0x00); // Gyro disabled
}

// ============================================
// Setup Function
// ============================================

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n\n========================================");
  Serial.println("  Distributed Seismic Monitoring Station");
  Serial.println("  NodeMCU + LSM6DS3 + MQTT + Mobile");
  Serial.println("========================================\n");

  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("✓ EEPROM initialized");

  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  setLedMode(LED_OFF);
  Serial.println("✓ LED initialized on GPIO12");

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  // Boot Sound: Short chirp
  tone(BUZZER_PIN, 2000, 100);
  delay(150);
  tone(BUZZER_PIN, 2500, 100);
  Serial.println("✓ Buzzer initialized on GPIO14 (D5)");

  // Generate unique station ID
  stationId = getStationId();
  Serial.print("✓ Station ID: ");
  Serial.println(stationId);

  // Construct MQTT topics with station ID
  topicStatus = "seismic/stations/" + stationId + "/status";
  topicTelemetry = "seismic/stations/" + stationId + "/telemetry";
  topicEventPGA = "seismic/stations/" + stationId + "/events/pga";
  topicEventArias = "seismic/stations/" + stationId + "/events/arias";
  topicCommands = "seismic/stations/" + stationId + "/commands";
  topicOTA = "seismic/stations/" + stationId + "/ota";
  topicLWT = "seismic/stations/" + stationId + "/lwt";

  // Check if WiFi credentials exist
  Serial.println("\nChecking for saved WiFi credentials...");
  isProvisioned = loadWiFiCredentials();

  if (!isProvisioned) {
    Serial.println("✗ No WiFi credentials found!");
    Serial.println("⚙️  Starting WiFi Provisioning Mode...\n");
    startProvisioningMode();
    // Provisioning mode will run indefinitely until credentials are received
    // After receiving credentials, device will restart automatically
    return; // Don't continue with normal setup
  }

  Serial.println("✓ WiFi credentials loaded from EEPROM");
  Serial.print("SSID: ");
  Serial.println(wifiSSID);

  // Load station configuration (name, coordinates)
  Serial.println("\nLoading station configuration...");
  loadStationConfig();
  Serial.print("✓ Station Name: ");
  Serial.println(stationName);
  Serial.print("✓ Coordinates: ");
  Serial.print(stationLat, 6);
  Serial.print(", ");
  Serial.println(stationLon, 6);

  // Initialize SPI
  SPI.begin();

  // CS Pin Settings
  pinMode(PIN_CS1, OUTPUT);
  pinMode(PIN_CS2, OUTPUT);
  digitalWrite(PIN_CS1, HIGH);
  digitalWrite(PIN_CS2, HIGH);
  delay(100);

  // 1. SENSOR 1 CHECK AND CONFIGURE
  uint8_t id1 = readRegister(PIN_CS1, REG_WHO_AM_I);
  if (id1 == 0x69) {
    Serial.println("[OK] Sensor 1 (CS: D0) detected, configuring...");
    configureSensor(PIN_CS1);
  } else {
    Serial.print("[FAIL] Sensor 1 not found! (0x");
    Serial.print(id1, HEX);
    Serial.println(")");
    while (1) {
      setLedMode(LED_ERROR);
      updateLED();
      delay(100);
    }
  }

  // 2. SENSOR 2 CHECK AND CONFIGURE
  uint8_t id2 = readRegister(PIN_CS2, REG_WHO_AM_I);
  if (id2 == 0x69) {
    Serial.println("[OK] Sensor 2 (CS: D2) detected, configuring...");
    configureSensor(PIN_CS2);
  } else {
    Serial.print("[FAIL] Sensor 2 not found! (0x");
    Serial.print(id2, HEX);
    Serial.println(")");
    while (1) {
      setLedMode(LED_ERROR);
      updateLED();
      delay(100);
    }
  }

  delay(200); // Sensor stabilization

  // 3. DUAL SENSOR CALIBRATION
  Serial.println("CALIBRATION STARTING... (8 Seconds)");
  Serial.println("Please keep the device still!");
  ;

  long s1X = 0, s1Y = 0, s1Z = 0;
  long s2X = 0, s2Y = 0, s2Z = 0;
  int samples = 2000;

  for (int i = 0; i < samples; i++) {
    // Read Sensor 1
    s1X += (int16_t)((readRegister(PIN_CS1, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTX_L_XL));
    s1Y += (int16_t)((readRegister(PIN_CS1, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTY_L_XL));
    s1Z += (int16_t)((readRegister(PIN_CS1, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTZ_L_XL));

    // Read Sensor 2
    s2X += (int16_t)((readRegister(PIN_CS2, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTX_L_XL));
    s2Y += (int16_t)((readRegister(PIN_CS2, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTY_L_XL));
    s2Z += (int16_t)((readRegister(PIN_CS2, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTZ_L_XL));

    delay(4);
    if (i % 200 == 0)
      Serial.print(".");

    // Keep LED happy during blocking loop
    if (i % 50 == 0)
      updateLED();
  }
  Serial.println("");

  // Calculate Offsets
  offX1 = (s1X / (float)samples) * factor;
  offY1 = (s1Y / (float)samples) * factor;
  offZ1 = (s1Z / (float)samples) * factor;

  offX2 = (s2X / (float)samples) * factor;
  offY2 = (s2Y / (float)samples) * factor;
  offZ2 = (s2Z / (float)samples) * factor;

  Serial.println("[OK] Dual Sensor Calibration Complete!");

  // Initialize buffers
  for (int i = 0; i < ARIAS_BUFFER_SIZE; i++) {
    ariasBuffer[i] = 0.0;
  }
  for (int i = 0; i < CHART_BUFFER_SIZE; i++) {
    axisEastBuffer[i] = 0.0;  // East (X)
    axisNorthBuffer[i] = 0.0; // North (Y)
    axisUpBuffer[i] = 0.0;    // Up (Z)
  }
  Serial.println("✓ Buffers initialized (ENU coordinate system)");

  // Initialize offline ring buffer
  initOfflineBuffer();
  Serial.println("✓ Offline ring buffer initialized (20 entries)");

  // Setup WiFi
  setupWiFi();

  // Initialize NTP
  Serial.println("\nInitializing NTP client...");
  timeClient.begin();
  timeClient.update();
  Serial.println("✓ NTP client started");
  Serial.print("Current time: ");
  Serial.println(timeClient.getFormattedTime());

  // Setup MQTT
  setupMQTT();

  // Setup Web Server (optional)
  if (webServerEnabled) {
    server.on("/", []() {
      String html = "<html><body>";
      html += "<h1>Seismic Station: " + stationId + "</h1>";
      html += "<p>Status: Connected to MQTT</p>";
      html += "<p>PGA: " + String(pga, 3) + " g</p>";
      html += "<p>Arias: " + String(ariasIntensity, 3) + " m/s</p>";
      html += "<p>Wave Type: " + waveType + "</p>";
      html += "<p>Uptime: " + String(millis() / 1000) + " seconds</p>";
      html += "</body></html>";
      server.send(200, "text/html", html);
    });
    server.begin();
    Serial.println("✓ Web server started on port 80");
  }

  systemStartTime = millis();

  Serial.println("\n========================================");
  Serial.println("System ready!");
  Serial.print("Station: ");
  Serial.println(stationName);
  Serial.print("Location: ");
  Serial.print(stationLat, 6);
  Serial.print(", ");
  Serial.println(stationLon, 6);
  Serial.print("MQTT Broker: ");
  Serial.println(mqtt_broker);
  Serial.println("Monitoring seismic activity...");
  Serial.println("========================================\n");

  // Force immediate telemetry publish
  lastTelemetryPublish = millis() - TELEMETRY_IDLE_INTERVAL - 1000;

  setLedMode(LED_MONITORING);
}

// ============================================
// Main Loop
// ============================================

void loop() {
  // Update LED state
  updateLED();

  // Maintain MQTT connection with offline mode handling
  if (!mqttClient.connected()) {
    // Enter offline mode
    if (!offlineMode) {
      offlineMode = true;
      Serial.println("📴 Entering OFFLINE mode - buffering data locally");
    }

    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      mqttReconnect();
    }
  } else {
    // Connection restored - flush offline buffer
    if (offlineMode) {
      offlineMode = false;
      Serial.println("📶 ONLINE mode restored - flushing offline buffer");
      flushOfflineBuffer();
    }
    mqttClient.loop();
  }

  // Handle web server
  if (webServerEnabled) {
    server.handleClient();
  }

  // Update NTP time
  timeClient.update();

  // Check data ready (Sensor 1 sync)
  // Non-blocking check to ensure MQTT loop runs even if sensor is slow
  if (readRegister(PIN_CS1, REG_STATUS) & 0x01) {

    // --- SENSOR 1 READING ---
    int16_t rawX1 = (readRegister(PIN_CS1, REG_OUTX_H_XL) << 8) |
                    readRegister(PIN_CS1, REG_OUTX_L_XL);
    int16_t rawY1 = (readRegister(PIN_CS1, REG_OUTY_H_XL) << 8) |
                    readRegister(PIN_CS1, REG_OUTY_L_XL);
    int16_t rawZ1 = (readRegister(PIN_CS1, REG_OUTZ_H_XL) << 8) |
                    readRegister(PIN_CS1, REG_OUTZ_L_XL);

    // --- SENSOR 2 READING ---
    int16_t rawX2 = (readRegister(PIN_CS2, REG_OUTX_H_XL) << 8) |
                    readRegister(PIN_CS2, REG_OUTX_L_XL);
    int16_t rawY2 = (readRegister(PIN_CS2, REG_OUTY_H_XL) << 8) |
                    readRegister(PIN_CS2, REG_OUTY_L_XL);
    int16_t rawZ2 = (readRegister(PIN_CS2, REG_OUTZ_H_XL) << 8) |
                    readRegister(PIN_CS2, REG_OUTZ_L_XL);

    // Convert to physical units and subtract offset (Calibration)
    float valX1 = (rawX1 * factor) - offX1;
    float valY1 = (rawY1 * factor) - offY1;
    float valZ1 = (rawZ1 * factor) - offZ1;

    float valX2 = (rawX2 * factor) - offX2;
    float valY2 = (rawY2 * factor) - offY2;
    float valZ2 = (rawZ2 * factor) - offZ2;

    // --- NOISE REDUCTION (AVERAGING) ---
    // ENU Mapping: X->East, Y->North, Z->Up
    float seismicE = (valX1 + valX2) / 2.0;
    float seismicN = (valY1 + valY2) / 2.0;
    float seismicU = (valZ1 + valZ2) / 2.0;

    // Pass corrected values to discriminator
    SeismicDiscriminator::Result waveRes =
        discriminator.update(seismicE, seismicN, seismicU);
    currentWaveType = waveRes.waveType;
    estimatedDistance = waveRes.estimatedDistanceKm;

    // Store corrected ENU axis data in buffers for telemetry
    // All axes now oscillate around 0 (seismic motion only)
    axisEastBuffer[chartIndex] = seismicE;
    axisNorthBuffer[chartIndex] = seismicN;
    axisUpBuffer[chartIndex] = seismicU;
    chartIndex = (chartIndex + 1) % CHART_BUFFER_SIZE;

    // Calculate PGA from corrected values (true ground motion)
    // PGA = Peak Ground Acceleration = magnitude of seismic motion vector
    pga = sqrt(seismicE * seismicE + seismicN * seismicN + seismicU * seismicU);

    // Noise filtering: ignore values below sensor noise floor
    const float pgaLimit = 0.0071; // ~7.1mg noise threshold
    if (pga < pgaLimit) {
      pga = 0.0;
    }

    // Calculate Arias Intensity
    calculateAriasIntensity(pga);

    // Detect wave type (P-wave on Up axis, S-wave on East-North plane)
    detectWaveType(seismicE, seismicN, seismicU);

  } // End of sensor read (if status is ready)

  // ============ PGA Event Detection ============
  if (pga >= PGA_EVENT_THRESHOLD) {
    lastPgaAboveThreshold = millis();

    if (!isPgaEventActive) {
      isPgaEventActive = true;
      pgaEventStartTime = millis();
      currentEventMax = pga;
      lastEventPeak = currentEventMax;
      lastEventTime = getFormattedTime();
      lastEventDate = getFormattedDate();

      setLedMode(LED_PGA_EVENT);
      totalPgaEvents++;
      // Publish PGA event to MQTT
      publishPgaEvent();
    } else {
      if (pga > currentEventMax) {
        currentEventMax = pga;
      }
    }
  }

  // Check if PGA event ended
  if (isPgaEventActive &&
      (millis() - lastPgaAboveThreshold) > PGA_COOLDOWN_MS) {
    isPgaEventActive = false;
    pgaEventDuration = (millis() - pgaEventStartTime) / 1000.0;

    currentEventMax = 0.0;

    if (!isAriasEventActive) {
      setLedMode(LED_MONITORING);
    }
  }

  // ============ Arias Event Detection ============
  if (ariasIntensity >= ARIAS_EVENT_THRESHOLD) {
    lastAriasAboveThreshold = millis();

    if (!isAriasEventActive) {
      isAriasEventActive = true;
      ariasEventStartTime = millis();
      currentAriasEventMax = ariasIntensity;

      if (!isPgaEventActive) {
        setLedMode(LED_ARIAS_EVENT);
      }

      totalAriasEvents++;

      // Publish Arias event to MQTT
      publishAriasEvent();
    } else {
      if (ariasIntensity > currentAriasEventMax) {
        currentAriasEventMax = ariasIntensity;
      }
    }
  }

  // Check if Arias event ended
  if (isAriasEventActive &&
      (millis() - lastAriasAboveThreshold) > ARIAS_COOLDOWN_MS) {
    isAriasEventActive = false;
    ariasEventDuration = (millis() - ariasEventStartTime) / 1000.0;
    lastAriasEventPeak = currentAriasEventMax;
    lastAriasEventTime = getFormattedTime();
    lastAriasEventDate = getFormattedDate();

    currentAriasEventMax = 0.0;

    if (!isPgaEventActive) {
      setLedMode(LED_MONITORING);
    }
  }

  // ============ Publish MQTT Data ============
  if (mqttClient.connected()) {
    unsigned long now = millis();

    // Publish telemetry data (includes automatic heartbeat)
    publishTelemetry();

    // Publish status update every 30 seconds
    if (now - lastStatusPublish >= STATUS_INTERVAL) {
      lastStatusPublish = now;
      publishStatus();
    }
  }
}

// ============================================
// WiFi Setup
// ============================================

void setupWiFi() {
  Serial.println("\nConnecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(wifiSSID);

  setLedMode(LED_WIFI_CONNECTING);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  WiFi.setSleep(false);
  WiFi.setOutputPower(20.5); // Sets WiFi RF power output to highest level,
                             // highest RF power usage
  //
  int attempts = 0;
  unsigned long lastLedUpdate = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    if (millis() - lastLedUpdate >= 100) {
      updateLED();
      lastLedUpdate = millis();
    }
    // Subtle tick sound while connecting
    if (attempts % 2 == 0) {
      tone(BUZZER_PIN, 4000, 10);
    }
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    // Success Melody: Ascending tones (C5-E5-G5)
    tone(BUZZER_PIN, 523, 100);
    delay(120); // Do
    tone(BUZZER_PIN, 659, 100);
    delay(120);                 // Mi
    tone(BUZZER_PIN, 784, 200); // Sol

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    setLedMode(LED_WIFI_CONNECTED);
    delay(2000);
  } else {
    Serial.println("\n✗ WiFi connection failed!");
    // Error Sound: Descending tones
    tone(BUZZER_PIN, 400, 300);
    delay(350);
    tone(BUZZER_PIN, 200, 500);

    Serial.println("Saved credentials may be invalid.");
    Serial.println(
        "Clearing credentials and restarting into provisioning mode...");
    setLedMode(LED_ERROR);
    clearWiFiCredentials();
    delay(3000);
    ESP.restart();
  }
}

// ============================================
// MQTT Setup
// ============================================

void setupMQTT() {
  Serial.println("\nConfiguring MQTT...");

  // Set TLS certificate verification (use insecure for testing)
  espClient.setInsecure(); // Skip certificate validation
  // For production, use: espClient.setCACert(root_ca);
  espClient.setBufferSizes(1024, 1024);
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);

  // Set buffer size for larger messages
  mqttClient.setBufferSize(2048); // 2048 bytes

  Serial.println("✓ MQTT configured");

  // Initial connection
  mqttReconnect();
}

// ============================================
// MQTT Reconnect
// ============================================

void mqttReconnect() {
  if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    Serial.println("Max MQTT reconnect attempts reached. Restarting...");
    delay(5000);
    ESP.restart();
  }

  Serial.print("Connecting to MQTT broker...");
  setLedMode(LED_MQTT_CONNECTING);

  // Create Last Will and Testament message
  StaticJsonDocument<256> lwtDoc;
  lwtDoc["stationId"] = stationId;
  lwtDoc["status"] = "offline";
  lwtDoc["timestamp"] = timeClient.getEpochTime();

  String lwtMessage;
  serializeJson(lwtDoc, lwtMessage);

  // Attempt connection with LWT
  String clientId = "seismic-" + stationId;

  if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password,
                         topicLWT.c_str(), 1, true, lwtMessage.c_str())) {
    Serial.println(" ✓ Connected!");
    Serial.print("Client ID: ");
    Serial.println(clientId);

    setLedMode(LED_MQTT_CONNECTED);
    delay(1000);
    setLedMode(LED_MONITORING);

    reconnectAttempts = 0;
    mqttReconnects++;

    // Subscribe to command topic
    mqttClient.subscribe(topicCommands.c_str(), 1);
    mqttClient.subscribe(topicOTA.c_str(), 1);
    Serial.print("✓ Subscribed to: ");
    Serial.println(topicCommands);
    Serial.println(topicOTA);

    // Publish online status
    publishStatus();

  } else {
    Serial.print(" ✗ Failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" (will retry in 5 seconds)");
    reconnectAttempts++;
    setLedMode(LED_ERROR);
  }
}

// ============================================
// MQTT Callback (Command Handler)
// ============================================

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message received on topic: ");
  Serial.println(topic);

  // Parse JSON
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return;
  }

  // Check if message is for OTA Update
  if (String(topic) == topicOTA) {
    String newVersion = doc["version"] | "";
    String downloadUrl = doc["url"] | "";

    Serial.println("📦 OTA Update Request Received");
    Serial.print("   New Version: ");
    Serial.println(newVersion);
    Serial.print("   Current Version: ");
    Serial.println(firmwareVersion);
    Serial.print("   URL: ");
    Serial.println(downloadUrl);

    if (newVersion.length() > 0 && downloadUrl.length() > 0) {
      if (newVersion != String(firmwareVersion)) {
        Serial.println("⚠️  Version mismatch! Starting update process...");
        performOTA(downloadUrl, newVersion);
      } else {
        Serial.println("✓ Device is already up to date.");
        // Optional: Send feedback
        mqttClient.publish("seismic/logs",
                           "Device already on requested version", false);
      }
    } else {
      Serial.println("❌ Invalid OTA message format. Required: {version, url}");
    }
    return; // Don't process as a command
  }

  // Handle Standard Commands
  String command = doc["command"] | "";
  String value = doc["value"] | "";

  Serial.print("Command: ");
  Serial.print(command);
  Serial.print(", Value: ");
  Serial.println(value);

  handleCommand(command, value);
}

// ============================================
// Perform OTA Update
// ============================================

void performOTA(String url, String version) {
  // 1. Publish status: UPDATING
  if (mqttClient.connected()) {
    StaticJsonDocument<200> statusDoc;
    statusDoc["stationId"] = stationId;
    statusDoc["status"] = "UPDATING";
    statusDoc["targetVersion"] = version;
    String statusMsg;
    serializeJson(statusDoc, statusMsg);
    mqttClient.publish(topicStatus.c_str(), statusMsg.c_str(), true);
    delay(500); // Wait for message to be sent
  }

  // 2. Prepare for update
  Serial.println("⬇️ Starting firmware download...");
  Serial.print("Target URL: ");
  Serial.println(url);

  // OTA Start Sound
  tone(BUZZER_PIN, 1000, 100);
  delay(150);
  tone(BUZZER_PIN, 1000, 100);
  delay(150);
  tone(BUZZER_PIN, 2000, 300);

  setLedMode(LED_WIFI_CONNECTING); // Fast blink

  // STOP EVERYTHING to free up heap!
  mqttClient.disconnect();
  // server.stop(); // If web server is running
  // timeClient.end();

  delay(1000); // Wait for sockets to close and heap to recover

  WiFiClientSecure client;

  client.setInsecure();

  client.setBufferSizes(4096, 512);
  client.setTimeout(20000);

  // Increase timeout
  // ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);

  // 5. Handle Result
  switch (ret) {
  case HTTP_UPDATE_FAILED: {
    int errCode = ESPhttpUpdate.getLastError();
    String errStr = ESPhttpUpdate.getLastErrorString();
    Serial.printf("❌ HTTP_UPDATE_FAILED Error (%d): %s\n",
                  ESPhttpUpdate.getLastError(),
                  ESPhttpUpdate.getLastErrorString().c_str());

    // Error Sound
    tone(BUZZER_PIN, 400, 300);
    delay(350);
    tone(BUZZER_PIN, 200, 500);

    // Try to recover
    mqttReconnect();
    mqttClient.publish("seismic/logs", "OTA Update Failed: Connection Error",
                       false);

    setLedMode(LED_ERROR);
    if (mqttClient.connected()) {
      StaticJsonDocument<512> errorDoc;
      errorDoc["stationId"] = stationId;
      errorDoc["type"] = "OTA_ERROR";
      errorDoc["targetVersion"] = version;
      errorDoc["errorCode"] = errCode;
      errorDoc["errorMessage"] =
          errStr; // e.g., "Connection Refused" or "Not Enough Space"
      errorDoc["timestamp"] = timeClient.getEpochTime();

      String errorMsg;
      serializeJson(errorDoc, errorMsg);

      // Send log to dedicated channel: seismic/stations/{stationId}/logs
      String logTopic = "seismic/stations/" + stationId + "/logs";

      if (mqttClient.publish(logTopic.c_str(), errorMsg.c_str(), false)) {
        Serial.println("📡 Error log published successfully!");
      } else {
        Serial.println("❌ Failed to publish error log.");
      }

      // Also set status back to 'online' so it doesn't stay as 'updating'
      // on the panel
      publishStatus();
    }
    break;
  }
  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("⚠️ HTTP_UPDATE_NO_UPDATES");
    mqttReconnect();

    if (mqttClient.connected()) {
      String logTopic = "seismic/stations/" + stationId + "/logs";
      mqttClient.publish(
          logTopic.c_str(),
          "{\"type\":\"OTA_INFO\",\"message\":\"No updates found\"}", false);
      publishStatus();
    }
    break;

  case HTTP_UPDATE_OK:
    Serial.println("✅ HTTP_UPDATE_OK");

    // Success Sound
    tone(BUZZER_PIN, 1000, 100);
    delay(120);
    tone(BUZZER_PIN, 2000, 100);
    delay(120);
    tone(BUZZER_PIN, 3000, 300);

    Serial.println("🔄 Update success! Rebooting in 3 seconds...");
    delay(3000);
    ESP.restart();
    break;
  }
}

// ============================================
// Handle Commands
// ============================================

void handleCommand(String command, String value) {
  if (command == "restart") {
    Serial.println("🔄 Restart command received. Restarting...");
    delay(1000);
    ESP.restart();

  } else if (command == "status") {
    Serial.println("📊 Status request received.");
    publishStatus();

  } else if (command == "calibrate") {
    Serial.println("⚙️  Calibration command received. Recalibrating sensor...");
    performCalibration();

  } else if (command == "set_name") {
    if (value.length() > 0 && value.length() <= MAX_NAME_LENGTH) {
      Serial.print("📝 Setting station name to: ");
      Serial.println(value);
      stationName = value;
      saveStationName(value);
      Serial.println("✓ Station name updated!");
      publishStatus(); // Publish updated status
    } else {
      Serial.println("❌ Invalid station name (1-64 characters required)");
    }

  } else if (command == "set_coordinate") {
    // Expected format: "lat,lon" e.g., "37.483256,34.471726"
    int commaIndex = value.indexOf(',');
    if (commaIndex > 0) {
      String latStr = value.substring(0, commaIndex);
      String lonStr = value.substring(commaIndex + 1);

      float newLat = latStr.toFloat();
      float newLon = lonStr.toFloat();

      // Validate coordinates
      if (newLat >= -90.0 && newLat <= 90.0 && newLon >= -180.0 &&
          newLon <= 180.0) {
        Serial.print("📍 Setting coordinates to: ");
        Serial.print(newLat, 6);
        Serial.print(", ");
        Serial.println(newLon, 6);

        stationLat = newLat;
        stationLon = newLon;
        saveStationCoordinates(newLat, newLon);

        Serial.println("✓ Coordinates updated!");
        publishStatus(); // Publish updated status
      } else {
        Serial.println(
            "❌ Invalid coordinates (lat: -90 to 90, lon: -180 to 180)");
      }
    } else {
      Serial.println("❌ Invalid coordinate format. Use: lat,lon (e.g., "
                     "37.483256,34.471726)");
    }

  } else if (command == "set_threshold_pga") {
    // Adjust PGA threshold (would need to make it non-const)
    Serial.print("⚠️  PGA threshold adjustment requested: ");
    Serial.println(value);
    Serial.println(
        "Note: PGA threshold is currently const. Firmware update needed.");

  } else if (command == "set_interval") {
    // Adjust telemetry interval
    Serial.print("⏱️  Telemetry interval adjustment requested: ");
    Serial.println(value);
    Serial.println("Note: Interval adjustment not yet implemented.");

  } else if (command == "set_user_id") {
    Serial.print("👤 Setting user ID to: ");
    Serial.println(value);
    userId = value;
    Serial.println("✓ User ID updated!");
    publishStatus(); // Publish updated status

  } else if (command == "set_device_model") {
    if (value.length() > 0) {
      Serial.print("📦 Setting device model to: ");
      Serial.println(value);
      deviceModel = value;
      Serial.println("✓ Device model updated!");
      publishStatus();
    } else {
      Serial.println("❌ Device model cannot be empty");
    }

  } else if (command == "set_sensor") {
    if (value.length() > 0) {
      Serial.print("🔬 Setting sensor type to: ");
      Serial.println(value);
      sensorType = value;
      Serial.println("✓ Sensor type updated!");
      publishStatus();
    } else {
      Serial.println("❌ Sensor type cannot be empty");
    }

  } else if (command == "set_controller") {
    if (value.length() > 0) {
      Serial.print("🎛️  Setting controller type to: ");
      Serial.println(value);
      controllerType = value;
      Serial.println("✓ Controller type updated!");
      publishStatus();
    } else {
      Serial.println("❌ Controller type cannot be empty");
    }

  } else if (command == "set_approved") {
    if (value == "true" || value == "1") {
      Serial.println("✅ Setting station as approved");
      isApproved = true;
    } else {
      Serial.println("❌ Setting station as not approved");
      isApproved = false;
    }
    Serial.println("✓ Approval status updated!");
    publishStatus();

  } else if (command == "clear_wifi") {
    if (value == "confirm") {
      Serial.println("🗑️  Clearing WiFi credentials...");
      clearWiFiCredentials();
      Serial.println("✓ WiFi credentials cleared. Device will restart...");
      delay(2000);
      ESP.restart();
    } else {
      Serial.println("❌ WiFi clear requires confirmation. Send: "
                     "{\"command\":\"clear_wifi\",\"value\":\"confirm\"}");
    }

  } else {
    Serial.print("❌ Unknown command: ");
    Serial.println(command);
  }

  // Send acknowledgment
  StaticJsonDocument<256> ackDoc;
  ackDoc["stationId"] = stationId;
  ackDoc["command"] = command;
  ackDoc["status"] = "acknowledged";
  ackDoc["timestamp"] = timeClient.getEpochTime();

  String ackMessage;
  serializeJson(ackDoc, ackMessage);

  String ackTopic = "seismic/stations/" + stationId + "/ack";
  mqttClient.publish(ackTopic.c_str(), ackMessage.c_str(), false);
}

// ============================================
// Publish Telemetry Data
// ============================================

void publishTelemetry() {
  unsigned long currentTime = millis();
  bool hasSignificantActivity =
      (pga > 0.0 || ariasIntensity >= 0.01 || currentWaveType != "None");

  // Dynamic interval: fast when active, slow when idle
  unsigned long interval = hasSignificantActivity ? TELEMETRY_ACTIVE_INTERVAL
                                                  : TELEMETRY_IDLE_INTERVAL;
  bool timeForPublish = (currentTime - lastTelemetryPublish) >= interval;

  // DEBUG
  // Serial.print("Check Pub: Active="); Serial.print(hasSignificantActivity);
  // Serial.print(" Interval="); Serial.print(interval);
  // Serial.print(" TimeSince="); Serial.println(currentTime -
  // lastTelemetryPublish);

  // Publish if: 1) There's activity AND time passed OR 2) It's time for
  // heartbeat
  if (!timeForPublish && !hasSignificantActivity) {
    return; // Skip this cycle
  }

  // Always publish immediately if there's significant activity
  if (hasSignificantActivity && (currentTime - lastTelemetryPublish) < 500) {
    // Rate limiting: minimum 500ms between publishes during activity
    return;
  }

  int latestIndex = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;

  // ============ OFFLINE MODE: Save to ring buffer ============
  if (offlineMode || !mqttClient.connected()) {
    Serial.println(">> Offline/No MQTT - Buffering...");
    // Only save significant activity to buffer (not heartbeats)
    if (hasSignificantActivity) {
      addToOfflineBuffer(
          ENTRY_TELEMETRY, pga, ariasIntensity, axisEastBuffer[latestIndex],
          axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
          0.0, // No peak value for regular telemetry
          currentWaveType.c_str());
    }
    lastTelemetryPublish = currentTime;
    return;
  }

  // ============ ONLINE MODE: Publish to MQTT ============
  StaticJsonDocument<512> doc;

  doc["stationId"] = stationId;
  doc["timestamp"] = timeClient.getEpochTime();
  doc["pga"] = round(pga * 1000) / 1000.0;           // PGA in 'g' units
  doc["pgaMs2"] = round(pga * 9.81 * 1000) / 1000.0; // PGA in m/s² units
  doc["arias"] = round(ariasIntensity * 1000) / 1000.0;
  doc["waveType"] = currentWaveType;
  if (estimatedDistance > 0) {
    doc["estDistKm"] = round(estimatedDistance * 10) / 10.0;
  }

  // ENU (East-North-Up) acceleration axes in m/s² (converting from 'g' to m/s²)
  JsonObject axes = doc.createNestedObject("axes");
  axes["e"] = round(axisEastBuffer[latestIndex] * 9.81 * 1000) / 1000.0;
  axes["n"] = round(axisNorthBuffer[latestIndex] * 9.81 * 1000) / 1000.0;
  axes["u"] = round(axisUpBuffer[latestIndex] * 9.81 * 1000) / 1000.0;
  doc["coordinateSystem"] = "ENU"; // Coordinate system indicator
  doc["unit"] = "m/s²";            // Unit indicator for acceleration data

  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = (millis() - systemStartTime) / 1000;

  String message;
  serializeJson(doc, message);

  if (mqttClient.publish(topicTelemetry.c_str(), message.c_str(), false)) {

    totalTelemetryPublished++;
    lastTelemetryPublish = currentTime; // Update last publish time
  } else {
    Serial.println("✗ Failed to publish telemetry");
    // Save to offline buffer on publish failure
    if (hasSignificantActivity) {
      addToOfflineBuffer(
          ENTRY_TELEMETRY, pga, ariasIntensity, axisEastBuffer[latestIndex],
          axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex], 0.0,
          currentWaveType.c_str());
    }
  }
}

// ============================================
// Publish Status
// ============================================

void publishStatus() {
  if (!mqttClient.connected())
    return;

  StaticJsonDocument<768> doc; // Increased size for additional fields

  doc["stationId"] = stationId;
  doc["stationName"] = stationName;
  doc["status"] = "online";
  doc["timestamp"] = timeClient.getEpochTime();
  doc["firmwareVersion"] = firmwareVersion;

  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;

  JsonObject stats = doc.createNestedObject("statistics");
  stats["uptime"] = (millis() - systemStartTime) / 1000;
  stats["totalPgaEvents"] = totalPgaEvents;
  stats["totalAriasEvents"] = totalAriasEvents;
  stats["telemetryPublished"] = totalTelemetryPublished;
  stats["mqttReconnects"] = mqttReconnects;

  // Offline buffer statistics
  JsonObject bufferStats = doc.createNestedObject("offlineBuffer");
  bufferStats["currentCount"] = bufferCount;
  bufferStats["maxSize"] = OFFLINE_BUFFER_SIZE;
  bufferStats["totalBuffered"] = totalOfflineEntries;
  bufferStats["totalOverflows"] = totalBufferOverflows;

  doc["rssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["ipAddress"] = WiFi.localIP().toString();

  // Device Information
  doc["deviceModel"] = deviceModel;
  doc["sensor"] = sensorType;
  doc["controller"] = controllerType;

  // User ID (null if not set)
  if (userId.length() > 0) {
    doc["userId"] = userId;
  }

  // Calibration timestamp (ISO format)
  // Note: Using boot time as reference, in production use RTC or NTP
  if (systemStartTime > 0) {
    unsigned long calibrationTime = timeClient.getEpochTime();
    doc["calibratedAt"] = calibrationTime;
  }

  // Approval status
  doc["isApproved"] = isApproved;

  String message;
  serializeJson(doc, message);

  if (mqttClient.publish(topicStatus.c_str(), message.c_str(),
                         true)) { // Retained
    // Serial.println("📡 Status published");
  } else {
    Serial.println("✗ Failed to publish status");
  }
}

// ============================================
// Publish PGA Event
// ============================================

void publishPgaEvent() {
  int latestIndex = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;

  // ============ OFFLINE MODE: Save PGA event to ring buffer ============
  if (offlineMode || !mqttClient.connected()) {
    addToOfflineBuffer(ENTRY_PGA_EVENT, pga, ariasIntensity,
                       axisEastBuffer[latestIndex],
                       axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
                       currentEventMax, // Peak PGA value
                       waveType.c_str());
    Serial.println("📴 PGA Event saved to offline buffer");
    return;
  }

  // ============ ONLINE MODE: Publish to MQTT ============
  StaticJsonDocument<512> doc;

  doc["stationId"] = stationId;
  doc["eventType"] = "pga";
  doc["timestamp"] = timeClient.getEpochTime();
  doc["date"] = lastEventDate;
  doc["time"] = lastEventTime;
  doc["peakPGA"] = round(currentEventMax * 1000) / 1000.0;
  doc["waveType"] = waveType;
  doc["richterEstimate"] = getRichterEstimation(currentEventMax);

  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;

  String message;
  serializeJson(doc, message);

  if (mqttClient.publish(topicEventPGA.c_str(), message.c_str(), false)) {
    // Serial.println("🚨 PGA Event published to MQTT");
  } else {
    Serial.println("✗ Failed to publish PGA event - saving to buffer");
    addToOfflineBuffer(ENTRY_PGA_EVENT, pga, ariasIntensity,
                       axisEastBuffer[latestIndex],
                       axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
                       currentEventMax, waveType.c_str());
  }
}

// ============================================
// Publish Arias Event
// ============================================

void publishAriasEvent() {
  int latestIndex = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;

  // ============ OFFLINE MODE: Save Arias event to ring buffer ============
  if (offlineMode || !mqttClient.connected()) {
    addToOfflineBuffer(ENTRY_ARIAS_EVENT, pga, ariasIntensity,
                       axisEastBuffer[latestIndex],
                       axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
                       currentAriasEventMax, // Peak Arias value
                       "Arias");

    return;
  }

  // ============ ONLINE MODE: Publish to MQTT ============
  StaticJsonDocument<512> doc;

  doc["stationId"] = stationId;
  doc["eventType"] = "arias";
  doc["timestamp"] = timeClient.getEpochTime();
  doc["date"] = lastAriasEventDate;
  doc["time"] = lastAriasEventTime;
  doc["peakArias"] = round(currentAriasEventMax * 1000) / 1000.0;

  String damageLevel = "Low";
  if (currentAriasEventMax > 0.30) {
    damageLevel = "High";
  } else if (currentAriasEventMax > 0.16) {
    damageLevel = "Moderate";
  }
  doc["damageLevel"] = damageLevel;

  JsonObject location = doc.createNestedObject("location");
  location["lat"] = stationLat;
  location["lon"] = stationLon;

  String message;
  serializeJson(doc, message);

  if (mqttClient.publish(topicEventArias.c_str(), message.c_str(), false)) {
    // Serial.println("🚨 Arias Event published to MQTT");
  } else {
    Serial.println("✗ Failed to publish Arias event - saving to buffer");
    addToOfflineBuffer(ENTRY_ARIAS_EVENT, pga, ariasIntensity,
                       axisEastBuffer[latestIndex],
                       axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
                       currentAriasEventMax, "Arias");
  }
}

// ============================================
// Helper Functions
// ============================================

String getStationId() {
  // Generate unique ID from ESP chip ID
  uint32_t chipId = ESP.getChipId();
  return "GEO_" + String(chipId, HEX);
}

void updateLED() {
  unsigned long currentMillis = millis();

  switch (currentLedMode) {
  case LED_OFF:
    digitalWrite(LED_PIN, LOW);
    break;

  case LED_ON:
    digitalWrite(LED_PIN, HIGH);
    break;

  case LED_WIFI_CONNECTING:
  case LED_MQTT_CONNECTING:
    if (currentMillis - lastLedBlink >= 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = currentMillis;
    }
    break;

  case LED_WIFI_CONNECTED:
  case LED_MQTT_CONNECTED: {
    int brightness = (sin(currentMillis / 318.0) + 1) * 127.5;
    analogWrite(LED_PIN, brightness);
  } break;

  case LED_PGA_EVENT:
    if (currentMillis - lastLedBlink >= 50) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = currentMillis;
    }
    break;

  case LED_ARIAS_EVENT:
    if (currentMillis - lastLedBlink >= 200) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = currentMillis;
    }
    break;

  case LED_MONITORING: {
    int brightness = (sin(currentMillis / 477.0) + 1) * 127.5;
    analogWrite(LED_PIN, brightness);
  } break;

  case LED_ERROR:
    // SOS pattern: ... --- ...
    // Implement SOS blink pattern
    if (currentMillis - lastLedBlink >= 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      lastLedBlink = currentMillis;
    }
    break;
  }
}

void setLedMode(LedMode mode) {
  if (currentLedMode != mode) {
    currentLedMode = mode;
    lastLedBlink = millis();
  }
}

String getFormattedTime() { return timeClient.getFormattedTime(); }

String getFormattedDate() {
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = epochTime;
  struct tm *ti = localtime(&rawTime);

  char buffer[11];
  sprintf(buffer, "%02d/%02d/%04d", ti->tm_mday, ti->tm_mon + 1,
          ti->tm_year + 1900);
  return String(buffer);
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

void calculateAriasIntensity(float acceleration) {
  unsigned long currentMicros = micros();

  // Calculate actual elapsed time (Dynamic Delta T)
  float dt = (currentMicros - lastSampleMicros) / 1000000.0;
  lastSampleMicros = currentMicros;

  // Sanity check
  if (dt > 0.1 || dt <= 0.0)
    dt = 0.005;

  // Calculate new energy component
  float newEnergy = (acceleration * acceleration * 9.81 * 9.81) * dt;

  // [OPTIMIZATION] Rolling Sum Algorithm
  // 1. Get the OLD value that will be overwritten
  float oldEnergy = ariasBuffer[ariasIndex];

  // 2. Subtract old value from sum
  runningAriasSum -= oldEnergy;

  // 3. Write new value to buffer
  ariasBuffer[ariasIndex] = newEnergy;

  // 4. Add new value to sum
  runningAriasSum += newEnergy;

  // Negative value check (guard against float errors)
  if (runningAriasSum < 0)
    runningAriasSum = 0;

  // Advance index
  ariasIndex = (ariasIndex + 1) % ARIAS_BUFFER_SIZE;

  // [SAFETY] Float Drift Correction
  // When array wraps around (approximately once per second), recalculate
  // sum from scratch. This clears small mathematical drifts that may
  // accumulate during long runtime.
  if (ariasIndex == 0) {
    runningAriasSum = 0.0;
    for (int i = 0; i < ARIAS_BUFFER_SIZE; i++) {
      runningAriasSum += ariasBuffer[i];
    }
  }

  // Calculate result
  ariasIntensity = ARIAS_FACTOR * runningAriasSum;
}

void detectWaveType(float seismicE, float seismicN, float seismicU) {
  // ENU-based Wave Detection:
  // P-Wave (Primary): Vertical motion on Up (Z) axis
  // S-Wave (Secondary): Horizontal motion on East-North (X-Y) plane
  //
  // Note: Input values are already offset-corrected (zero-baseline)
  // All axes oscillate around 0 during seismic activity

  float gEast = abs(seismicE);
  float gNorth = abs(seismicN);
  float gUp = abs(seismicU);

  // Vertical intensity: Up (Z) axis motion
  verticalIntensity = gUp;

  // Horizontal intensity: Vector sum of East (X) and North (Y) axes
  horizontalIntensity = sqrt(gEast * gEast + gNorth * gNorth);

  // P-Wave Detection: Dominant vertical motion
  if (verticalIntensity > WAVE_THRESHOLD &&
      verticalIntensity > horizontalIntensity * 1.5) {
    if (!pWaveDetected) {
      pWaveDetected = true;
      waveDetectionTime = millis();
    }
  }

  // S-Wave Detection: Dominant horizontal motion
  if (horizontalIntensity > WAVE_THRESHOLD &&
      horizontalIntensity > verticalIntensity * 1.5) {
    if (!sWaveDetected) {
      sWaveDetected = true;
    }
  }

  // Determine wave type
  if (pWaveDetected && sWaveDetected) {
    waveType = "P+S";
  } else if (pWaveDetected) {
    waveType = "P-wave";
  } else if (sWaveDetected) {
    waveType = "S-wave";
  } else {
    waveType = "None";
  }

  // Reset detection after 5 seconds
  if (millis() - waveDetectionTime > 5000) {
    pWaveDetected = false;
    sWaveDetected = false;
    waveType = "None";
  }
}

// ============================================
// WiFi Provisioning Functions
// ============================================

void startProvisioningMode() {
  Serial.println("========================================");
  Serial.println("  WiFi Provisioning Mode Active");
  Serial.println("========================================");
  Serial.print("Hotspot Name: ");
  Serial.println(AP_SSID);
  Serial.println("Password: (none - open network)");
  Serial.println("IP Address: 192.168.4.1");
  Serial.println("\nWaiting for mobile app to connect...");
  Serial.println("========================================\n");

  // Configure Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD); // Open network (no password)

  setLedMode(LED_WIFI_CONNECTING); // Blink to indicate provisioning mode

  Serial.print("✓ AP started: ");
  Serial.println(WiFi.softAPIP());

  // Setup provisioning web server
  server.on("/", HTTP_GET, handleProvisioningRoot);
  server.on("/provision", HTTP_POST, handleProvisioningConfig);
  server.on("/status", HTTP_GET, handleProvisioningStatus);

  // Enable CORS for mobile app
  server.enableCORS(true);

  server.begin();
  Serial.println("✓ Provisioning server started on port 80");
  Serial.println("\nEndpoints:");
  Serial.println("  GET  http://192.168.4.1/");
  Serial.println("  POST http://192.168.4.1/provision");
  Serial.println("  GET  http://192.168.4.1/status");

  // Infinite loop - wait for provisioning
  while (true) {
    server.handleClient();
    updateLED();
    delay(10);
  }
}

void handleProvisioningRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>GeoLink Provisioning</title>";
  html += "<style>body{font-family:Arial;margin:20px;background:#f0f0f0;}";
  html += ".container{background:white;padding:20px;border-radius:10px;max-"
          "width:400px;margin:auto;}";
  html += "h1{color:#2196F3;}input{width:100%;padding:10px;margin:10px "
          "0;box-sizing:border-box;}";
  html += "button{background:#2196F3;color:white;padding:15px;border:none;"
          "border-radius:5px;width:100%;cursor:pointer;}";
  html += "button:hover{background:#1976D2;}</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>🌍 GeoLink WiFi Setup</h1>";
  html += "<p><strong>Station ID:</strong> " + stationId + "</p>";
  html += "<form id='wifiForm'>";
  html += "<label>WiFi Network (SSID):</label>";
  html += "<input type='text' id='ssid' name='ssid' placeholder='Enter WiFi "
          "name' required>";
  html += "<label>WiFi Password:</label>";
  html += "<input type='password' id='password' name='password' "
          "placeholder='Enter WiFi password' required>";
  html += "<button type='submit'>Connect to WiFi</button>";
  html += "</form>";
  html += "<p id='status' style='margin-top:20px;color:#666;'></p>";
  html += "</div>";
  html += "<script>";
  html += "document.getElementById('wifiForm').addEventListener('submit',"
          "function(e){";
  html += "e.preventDefault();";
  html += "var ssid=document.getElementById('ssid').value;";
  html += "var password=document.getElementById('password').value;";
  html += "document.getElementById('status').innerHTML='⏳ Connecting...';";
  html +=
      "fetch('/provision',{method:'POST',headers:{'Content-Type':'application/"
      "json'},";
  html += "body:JSON.stringify({ssid:ssid,password:password})})";
  html += ".then(r=>r.json()).then(data=>{";
  html += "if(data.success){";
  html += "document.getElementById('status').innerHTML='✅ Success! Device is "
          "connecting to WiFi and will restart.';";
  html += "}else{";
  html +=
      "document.getElementById('status').innerHTML='❌ Error: '+data.message;";
  html += "}}).catch(err=>{document.getElementById('status').innerHTML='❌ "
          "Connection failed';});";
  html += "});";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleProvisioningConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"No data received\"}");
    return;
  }

  String body = server.arg("plain");
  Serial.println("\n📨 Received provisioning data:");
  Serial.println(body);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(error.c_str());
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  String receivedSSID = doc["ssid"] | "";
  String receivedPassword = doc["password"] | "";

  if (receivedSSID.length() == 0) {
    Serial.println("❌ SSID is empty");
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"SSID cannot be empty\"}");
    return;
  }

  Serial.println("✅ Valid credentials received:");
  Serial.print("   SSID: ");
  Serial.println(receivedSSID);
  Serial.print("   Password: ");
  Serial.println(receivedPassword.length() > 0 ? "********" : "(empty)");

  // Save credentials to EEPROM
  saveWiFiCredentials(receivedSSID, receivedPassword);

  // Send success response
  StaticJsonDocument<256> response;
  response["success"] = true;
  response["message"] =
      "Credentials saved. Device will restart and connect to WiFi.";
  response["stationId"] = stationId;

  String responseStr;
  serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);

  Serial.println("\n✅ Provisioning complete!");
  Serial.println("🔄 Restarting device in 3 seconds...");

  delay(3000);
  ESP.restart();
}

void handleProvisioningStatus() {
  StaticJsonDocument<256> doc;
  doc["stationId"] = stationId;
  doc["mode"] = "provisioning";
  doc["apSSID"] = AP_SSID;
  doc["apIP"] = WiFi.softAPIP().toString();
  doc["connectedClients"] = WiFi.softAPgetStationNum();

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// ============================================
// EEPROM WiFi Credentials Management
// ============================================

bool loadWiFiCredentials() {
  // Check magic byte
  byte magic = EEPROM.read(0);
  if (magic != EEPROM_MAGIC_BYTE) {
    Serial.println("No valid credentials in EEPROM (magic byte mismatch)");
    return false;
  }

  // Read SSID
  char ssidBuf[MAX_SSID_LENGTH + 1];
  for (int i = 0; i < MAX_SSID_LENGTH; i++) {
    ssidBuf[i] = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (ssidBuf[i] == '\0')
      break;
  }
  ssidBuf[MAX_SSID_LENGTH] = '\0';
  wifiSSID = String(ssidBuf);

  // Read Password
  char passBuf[MAX_PASS_LENGTH + 1];
  for (int i = 0; i < MAX_PASS_LENGTH; i++) {
    passBuf[i] = EEPROM.read(EEPROM_PASS_ADDR + i);
    if (passBuf[i] == '\0')
      break;
  }
  passBuf[MAX_PASS_LENGTH] = '\0';
  wifiPassword = String(passBuf);

  if (wifiSSID.length() == 0) {
    Serial.println("SSID is empty in EEPROM");
    return false;
  }

  return true;
}

void saveWiFiCredentials(String ssid, String password) {
  Serial.println("\n💾 Saving WiFi credentials to EEPROM...");

  // Write magic byte
  EEPROM.write(0, EEPROM_MAGIC_BYTE);

  // Write SSID
  int i;
  for (i = 0; i < ssid.length() && i < MAX_SSID_LENGTH; i++) {
    EEPROM.write(EEPROM_SSID_ADDR + i, ssid[i]);
  }
  EEPROM.write(EEPROM_SSID_ADDR + i, '\0'); // Null terminator

  // Write Password
  for (i = 0; i < password.length() && i < MAX_PASS_LENGTH; i++) {
    EEPROM.write(EEPROM_PASS_ADDR + i, password[i]);
  }
  EEPROM.write(EEPROM_PASS_ADDR + i, '\0'); // Null terminator

  EEPROM.commit();
  Serial.println("✅ Credentials saved successfully!");
}

void clearWiFiCredentials() {
  Serial.println("🗑️  Clearing WiFi credentials from EEPROM...");
  EEPROM.write(0, 0x00); // Clear magic byte
  EEPROM.commit();
  Serial.println("✅ Credentials cleared!");
}

// ============================================
// Station Configuration Management
// ============================================

void loadStationConfig() {
  // Load station name
  char nameBuf[MAX_NAME_LENGTH + 1];
  bool nameFound = false;

  for (int i = 0; i < MAX_NAME_LENGTH; i++) {
    nameBuf[i] = EEPROM.read(EEPROM_NAME_ADDR + i);
    if (nameBuf[i] == '\0') {
      nameFound = true;
      break;
    }
  }
  nameBuf[MAX_NAME_LENGTH] = '\0';

  if (nameFound && strlen(nameBuf) > 0) {
    stationName = String(nameBuf);
  }
  // else: keep default "Station-001"

  // Load coordinates
  float savedLat, savedLon;
  EEPROM.get(EEPROM_LAT_ADDR, savedLat);
  EEPROM.get(EEPROM_LON_ADDR, savedLon);

  // Validate coordinates
  if (savedLat >= -90.0 && savedLat <= 90.0 && savedLon >= -180.0 &&
      savedLon <= 180.0) {
    stationLat = savedLat;
    stationLon = savedLon;
  }
  // else: keep default coordinates
}

void saveStationName(String name) {
  Serial.println("\n💾 Saving station name to EEPROM...");

  int i;
  for (i = 0; i < name.length() && i < MAX_NAME_LENGTH; i++) {
    EEPROM.write(EEPROM_NAME_ADDR + i, name[i]);
  }
  EEPROM.write(EEPROM_NAME_ADDR + i, '\0'); // Null terminator

  EEPROM.commit();
  Serial.println("✅ Station name saved!");
}

void saveStationCoordinates(float lat, float lon) {
  Serial.println("\n💾 Saving coordinates to EEPROM...");

  EEPROM.put(EEPROM_LAT_ADDR, lat);
  EEPROM.put(EEPROM_LON_ADDR, lon);

  EEPROM.commit();
  Serial.println("✅ Coordinates saved!");
}

void performCalibration() {
  Serial.println("\n⚙️  Starting ENU per-axis calibration...");
  Serial.println("Keep device steady and level...");
  Serial.println("(Measuring offset for each axis independently)");

  setLedMode(LED_WIFI_CONNECTING); // Fast blink during calibration

  // Wait for device to stabilize
  Serial.println("Stabilizing... (2 seconds)");
  delay(2000);

  long s1X = 0, s1Y = 0, s1Z = 0;
  long s2X = 0, s2Y = 0, s2Z = 0;
  const int samples = 2000;

  Serial.print("Collecting ");
  Serial.print(samples);
  Serial.println(" samples...");

  for (int i = 0; i < samples; i++) {
    // Read Sensor 1
    s1X += (int16_t)((readRegister(PIN_CS1, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTX_L_XL));
    s1Y += (int16_t)((readRegister(PIN_CS1, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTY_L_XL));
    s1Z += (int16_t)((readRegister(PIN_CS1, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS1, REG_OUTZ_L_XL));

    // Read Sensor 2
    s2X += (int16_t)((readRegister(PIN_CS2, REG_OUTX_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTX_L_XL));
    s2Y += (int16_t)((readRegister(PIN_CS2, REG_OUTY_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTY_L_XL));
    s2Z += (int16_t)((readRegister(PIN_CS2, REG_OUTZ_H_XL) << 8) |
                     readRegister(PIN_CS2, REG_OUTZ_L_XL));

    if (i % 200 == 0) {
      Serial.print(".");
    }

    delay(4);
  }
  Serial.println();

  // Offsetleri Hesapla
  offX1 = (s1X / (float)samples) * factor;
  offY1 = (s1Y / (float)samples) * factor;
  offZ1 = (s1Z / (float)samples) * factor;

  offX2 = (s2X / (float)samples) * factor;
  offY2 = (s2Y / (float)samples) * factor;
  offZ2 = (s2Z / (float)samples) * factor;

  Serial.println("✅ Dual Sensor calibration complete!");
  Serial.print("   S1 X offset:  ");
  Serial.println(offX1, 4);
  Serial.print("   S1 Y offset:  ");
  Serial.println(offY1, 4);
  Serial.print("   S1 Z offset:  ");
  Serial.println(offZ1, 4);
  Serial.print("   S2 X offset:  ");
  Serial.println(offX2, 4);
  Serial.print("   S2 Y offset:  ");
  Serial.println(offY2, 4);
  Serial.print("   S2 Z offset:  ");
  Serial.println(offZ2, 4);

  // Reset Arias buffer to prevent stale data affecting new readings
  for (int i = 0; i < ARIAS_BUFFER_SIZE; i++) {
    ariasBuffer[i] = 0.0;
  }
  ariasIndex = 0;
  runningAriasSum = 0.0;
  ariasIntensity = 0.0;
  currentEventArias = 0.0;
  Serial.println("✓ Arias buffer reset");

  // Reset ENU axis chart buffers
  for (int i = 0; i < CHART_BUFFER_SIZE; i++) {
    axisEastBuffer[i] = 0.0;
    axisNorthBuffer[i] = 0.0;
    axisUpBuffer[i] = 0.0;
  }
  chartIndex = 0;
  Serial.println("✓ Chart buffers reset");

  // Reset wave detection state
  pWaveDetected = false;
  sWaveDetected = false;
  waveType = "None";
  currentWaveType = "None";
  waveDetectionTime = 0;
  Serial.println("✓ Wave detection state reset");

  // Reset PGA state
  pga = 0.0;
  currentEventMax = 0.0;
  isPgaEventActive = false;
  Serial.println("✓ PGA state reset");

  // Reset timing for Arias calculation
  lastSampleMicros = micros();

  // Reset discriminator state
  discriminator.reset();
  Serial.println("✓ Discriminator reset");

  setLedMode(LED_MONITORING);

  // Publish status with updated calibration
  publishStatus();

  Serial.println("📐 Calibration and state reset complete!\n");
}

// ============================================
// Offline Ring Buffer Functions
// ============================================
// Implements a circular buffer for storing telemetry data
// when MQTT connection is unavailable

void initOfflineBuffer() {
  // Clear all buffer entries
  for (int i = 0; i < OFFLINE_BUFFER_SIZE; i++) {
    memset(&offlineBuffer[i], 0, sizeof(OfflineEntry));
  }
  bufferHead = 0;
  bufferTail = 0;
  bufferCount = 0;
  offlineMode = false;
  totalOfflineEntries = 0;
  totalBufferOverflows = 0;
  Serial.println("✓ Offline buffer initialized");
}

bool addToOfflineBuffer(BufferEntryType type, float pgaVal, float ariasVal,
                        float e, float n, float u, float peak,
                        const char *wave) {
  // Rate limiting - don't save too frequently
  unsigned long now = millis();
  if (type == ENTRY_TELEMETRY &&
      (now - lastOfflineSave) < OFFLINE_SAVE_INTERVAL) {
    return false; // Skip this save (rate limited)
  }

  // Check if buffer is full (overwrite oldest)
  if (bufferCount >= OFFLINE_BUFFER_SIZE) {
    // Buffer overflow - move tail forward (lose oldest data)
    bufferTail = (bufferTail + 1) % OFFLINE_BUFFER_SIZE;
    bufferCount--;
    totalBufferOverflows++;
    Serial.println("⚠️ Buffer overflow - oldest entry overwritten");
  }

  // Create new entry
  OfflineEntry *entry = &offlineBuffer[bufferHead];
  entry->entryType = (uint8_t)type;
  entry->timestamp = timeClient.getEpochTime();
  entry->pga = pgaVal;
  entry->arias = ariasVal;
  entry->axisE = e;
  entry->axisN = n;
  entry->axisU = u;
  entry->peakValue = peak;
  entry->rssi = (int8_t)WiFi.RSSI();

  // Copy wave type string (safely)
  strncpy(entry->waveType, wave, sizeof(entry->waveType) - 1);
  entry->waveType[sizeof(entry->waveType) - 1] = '\0';

  // Move head forward
  bufferHead = (bufferHead + 1) % OFFLINE_BUFFER_SIZE;
  bufferCount++;
  totalOfflineEntries++;
  lastOfflineSave = now;

  Serial.print("💾 Buffered (");
  Serial.print(bufferCount);
  Serial.print("/");
  Serial.print(OFFLINE_BUFFER_SIZE);
  Serial.print("): ");
  switch (type) {
  case ENTRY_TELEMETRY:
    Serial.println("Telemetry");
    break;
  case ENTRY_PGA_EVENT:
    Serial.println("PGA Event");
    break;
  case ENTRY_ARIAS_EVENT:
    Serial.println("Arias Event");
    break;
  }

  return true;
}

bool hasOfflineData() { return bufferCount > 0; }

bool getFromOfflineBuffer(OfflineEntry *entry) {
  if (bufferCount == 0) {
    return false; // Buffer empty
  }

  // Copy oldest entry (at tail)
  memcpy(entry, &offlineBuffer[bufferTail], sizeof(OfflineEntry));

  // Move tail forward
  bufferTail = (bufferTail + 1) % OFFLINE_BUFFER_SIZE;
  bufferCount--;

  return true;
}

void flushOfflineBuffer() {
  if (!mqttClient.connected()) {
    Serial.println("❌ Cannot flush buffer - MQTT not connected");
    return;
  }

  if (bufferCount == 0) {
    Serial.println("✓ Offline buffer is empty - nothing to flush");
    return;
  }

  Serial.print("📤 Flushing offline buffer (");
  Serial.print(bufferCount);
  Serial.println(" entries)...");

  int successCount = 0;
  int failCount = 0;

  OfflineEntry entry;
  while (getFromOfflineBuffer(&entry)) {
    // Build JSON message based on entry type
    StaticJsonDocument<512> doc;
    doc["stationId"] = stationId;
    doc["timestamp"] = entry.timestamp;
    doc["buffered"] = true; // Flag indicating this was buffered offline
    doc["pga"] = round(entry.pga * 1000) / 1000.0;
    doc["arias"] = round(entry.arias * 1000) / 1000.0;
    doc["waveType"] = entry.waveType;

    JsonObject axes = doc.createNestedObject("axes");
    axes["e"] = round(entry.axisE * 9.81 * 1000) / 1000.0;
    axes["n"] = round(entry.axisN * 9.81 * 1000) / 1000.0;
    axes["u"] = round(entry.axisU * 9.81 * 1000) / 1000.0;

    doc["rssi"] = entry.rssi;

    String topic;
    switch (entry.entryType) {
    case ENTRY_TELEMETRY:
      topic = topicTelemetry;
      doc["coordinateSystem"] = "ENU";
      doc["unit"] = "m/s²";
      break;

    case ENTRY_PGA_EVENT:
      topic = topicEventPGA;
      doc["eventType"] = "pga";
      doc["peakPGA"] = round(entry.peakValue * 1000) / 1000.0;
      doc["richterEstimate"] = getRichterEstimation(entry.peakValue);
      {
        JsonObject location = doc.createNestedObject("location");
        location["lat"] = stationLat;
        location["lon"] = stationLon;
      }
      break;

    case ENTRY_ARIAS_EVENT:
      topic = topicEventArias;
      doc["eventType"] = "arias";
      doc["peakArias"] = round(entry.peakValue * 1000) / 1000.0;
      {
        String damageLevel = "Low";
        if (entry.peakValue > 0.30)
          damageLevel = "High";
        else if (entry.peakValue > 0.16)
          damageLevel = "Moderate";
        doc["damageLevel"] = damageLevel;

        JsonObject location = doc.createNestedObject("location");
        location["lat"] = stationLat;
        location["lon"] = stationLon;
      }
      break;
    }

    String message;
    serializeJson(doc, message);

    if (mqttClient.publish(topic.c_str(), message.c_str(), false)) {
      successCount++;
    } else {
      failCount++;
      Serial.println("✗ Failed to publish buffered entry");
    }

    // Small delay between messages to avoid overwhelming the broker
    delay(50);
    mqttClient.loop(); // Process MQTT
    yield();           // Feed watchdog
  }

  Serial.print("✅ Buffer flush complete: ");
  Serial.print(successCount);
  Serial.print(" sent, ");
  Serial.print(failCount);
  Serial.println(" failed");

  // Publish buffer statistics
  if (mqttClient.connected()) {
    StaticJsonDocument<256> statsDoc;
    statsDoc["stationId"] = stationId;
    statsDoc["type"] = "buffer_flush";
    statsDoc["entriesSent"] = successCount;
    statsDoc["entriesFailed"] = failCount;
    statsDoc["totalBuffered"] = totalOfflineEntries;
    statsDoc["totalOverflows"] = totalBufferOverflows;
    statsDoc["timestamp"] = timeClient.getEpochTime();

    String statsMsg;
    serializeJson(statsDoc, statsMsg);

    String logTopic = "seismic/stations/" + stationId + "/logs";
    mqttClient.publish(logTopic.c_str(), statsMsg.c_str(), false);
  }
}

// Helper function to save current state to offline buffer (for periodic saves)
void saveToOfflineBuffer() {
  if (!offlineMode)
    return; // Only save in offline mode

  int latestIndex = (chartIndex - 1 + CHART_BUFFER_SIZE) % CHART_BUFFER_SIZE;
  bool hasActivity =
      (pga > 0.0 || ariasIntensity >= 0.01 || currentWaveType != "None");

  if (hasActivity) {
    addToOfflineBuffer(ENTRY_TELEMETRY, pga, ariasIntensity,
                       axisEastBuffer[latestIndex],
                       axisNorthBuffer[latestIndex], axisUpBuffer[latestIndex],
                       0.0, currentWaveType.c_str());
  }
}
