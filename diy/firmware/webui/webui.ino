// GeoShake DIY — seismometer with a live web interface
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything geoshake_diy does, plus a dashboard the device serves itself.
// Join the same Wi-Fi, open http://geoshake-diy.local (or the IP the serial
// monitor prints) and watch the ground move in real time.
//
// Two design decisions worth knowing:
//
// 1. The sensor runs on its own core. Sampling sits in a high-priority task
//    pinned to core 1; Wi-Fi and HTTP live on core 0. A web request can take
//    tens of milliseconds, which at 104 Hz would drop several samples — and a
//    seismic detector that stops sampling while someone loads a page is not a
//    detector. Pinning keeps the two apart.
//
// 2. No extra libraries. WiFi, WebServer, Preferences and mDNS all ship with
//    the ESP32 core, and the page is plain HTML/CSS/JS served from flash. The
//    dashboard works with no internet at all — which is the point: a station
//    that needs the cloud to show you its own data is the wrong design.
//
// First boot has no Wi-Fi saved, so the device starts its own access point
// named GeoShake-DIY (open, no password). Connect to it, and the setup page
// appears at http://192.168.4.1.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>

#include "stalta.h"
#include "golden.h"
#include "lsm6_spi.h"
#include "web_page.h"

using namespace geoshake;

// ---- wiring (run spi_probe if unsure) --------------------------------------
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int CS_PINS[] = {9, 10};
static const int SENSOR_COUNT = sizeof(CS_PINS) / sizeof(CS_PINS[0]);

#define AP_SSID   "GeoShake-DIY"
#define HOSTNAME  "geoshake-diy"

// ---- shared state (written by the sensor task, read by the web task) --------
static const int  LIVE_CAP = 512;         // ~5 s of scrollback at 104 Hz
static const int  TRIG_CAP = 12;          // detections kept in RAM
static const int  TRIG_WAVE = 220;        // ~2 s of waveform per detection

struct LiveSample { float x, y, z, ratio; };
struct TriggerRec {
  uint32_t id;
  uint32_t millisAt;
  float ratio, pga;
  LiveSample wave[TRIG_WAVE];
  int waveLen;
};

static LiveSample liveBuf[LIVE_CAP];
static volatile uint32_t liveSeq = 0;     // total samples ever produced

static TriggerRec triggers[TRIG_CAP];
static volatile int trigCount = 0;        // total detections (may exceed TRIG_CAP)

static portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// Scalars the dashboard shows. Written by the sensor task only.
static volatile float  gRatio = 0, gNoise = 0, gRate = 0;
static volatile bool   gArmed = false, gFiring = false;
static volatile int    gWarmPct = 0;
static volatile uint32_t gLastFireMs = 0;

static Lsm6Spi sensors;
static WebServer server(80);
static Preferences prefs;
static bool goldenOk = false;
static int  sensorsFound = 0;
static String partName = "—";

// Pending capture: after a trigger we keep filling its waveform for a while.
static int capturingIdx = -1;
static int captureLeft = 0;

// ============================ sensor task (core 1) ==========================

static void sensorTask(void *) {
  StaltaState det = initStalta();
  uint32_t rateStart = millis(), rateSamples = 0;

  for (;;) {
    if (!sensors.sampleReady()) { vTaskDelay(1); continue; }

    double s0[3], s1[3];
    const bool ok0 = sensors.read(0, s0);
    const bool ok1 = (SENSOR_COUNT > 1) ? sensors.read(1, s1) : false;
    if (!ok0 && !ok1) { vTaskDelay(1); continue; }

    double buf[12];
    if (ok0 && ok1)  expandTwoSensors(s0, s1, buf);
    else if (ok0)    expandSingleSensor(s0[0], s0[1], s0[2], buf);
    else             expandSingleSensor(s1[0], s1[1], s1[2], buf);

    const StaltaResult r = staltaStep(det, buf);

    const float ax = (buf[0] + buf[3] + buf[6] + buf[9]) / 4.0;
    const float ay = (buf[1] + buf[4] + buf[7] + buf[10]) / 4.0;
    const float az = (buf[2] + buf[5] + buf[8] + buf[11]) / 4.0;
    // High-pass state doubles as the DC estimate — subtract it so the page
    // shows ground motion instead of a flat 9.81 offset.
    const LiveSample smp = { (float)(ax - det.dc[0]), (float)(ay - det.dc[1]),
                             (float)(az - det.dc[2]), (float)r.ratio };

    portENTER_CRITICAL(&dataMux);
    liveBuf[liveSeq % LIVE_CAP] = smp;
    liveSeq++;

    if (r.fired) {
      const int slot = trigCount % TRIG_CAP;
      triggers[slot].id = trigCount + 1;
      triggers[slot].millisAt = millis();
      triggers[slot].ratio = r.ratio;
      triggers[slot].pga = r.pga;
      triggers[slot].waveLen = 0;
      trigCount++;
      capturingIdx = slot;
      captureLeft = TRIG_WAVE;
    }
    if (capturingIdx >= 0 && captureLeft > 0) {
      TriggerRec &t = triggers[capturingIdx];
      if (t.waveLen < TRIG_WAVE) t.wave[t.waveLen++] = smp;
      if (--captureLeft == 0) capturingIdx = -1;
    }
    portEXIT_CRITICAL(&dataMux);

    gRatio = r.ratio;
    gNoise = sqrt(det.lta);
    gArmed = det.count >= STALTA_PARAMS.warmupSamples;
    gFiring = det.triggered;
    gWarmPct = gArmed ? 100 : (int)(100.0 * det.count / STALTA_PARAMS.warmupSamples);
    if (r.fired) gLastFireMs = millis();

    rateSamples++;
    const uint32_t now = millis();
    if (now - rateStart >= 3000) {
      gRate = rateSamples * 1000.0f / (now - rateStart);
      rateStart = now; rateSamples = 0;
    }
  }
}

// ============================== HTTP handlers ===============================

static void sendProgmem(const char *page) {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", page);
}

static void handleRoot() { sendProgmem(PAGE_MAIN); }

static void handleLive() {
  const uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;

  // Copy out under the lock, format outside it — JSON building is slow and the
  // sensor task must never wait on it.
  LiveSample out[192];
  int n = 0;
  uint32_t seqNow;
  portENTER_CRITICAL(&dataMux);
  seqNow = liveSeq;
  uint32_t from = since;
  if (seqNow > LIVE_CAP && from < seqNow - LIVE_CAP) from = seqNow - LIVE_CAP;  // client fell behind
  if (from > seqNow) from = seqNow;                                            // device rebooted
  while (from < seqNow && n < 192) out[n++] = liveBuf[from++ % LIVE_CAP];
  portEXIT_CRITICAL(&dataMux);

  String j;
  j.reserve(220 + n * 34);
  j += "{\"seq\":"; j += seqNow;
  j += ",\"ratio\":"; j += String(gRatio, 2);
  j += ",\"noise\":"; j += String(gNoise, 5);
  j += ",\"rate\":"; j += String(gRate, 1);
  j += ",\"trigs\":"; j += trigCount;
  j += ",\"armed\":"; j += (gArmed ? "true" : "false");
  j += ",\"firing\":"; j += (gFiring ? "true" : "false");
  j += ",\"warm\":"; j += gWarmPct;
  j += ",\"thr\":"; j += String(STALTA_PARAMS.trigger, 0);
  j += ",\"up\":"; j += (millis() / 1000);
  j += ",\"rssi\":"; j += WiFi.RSSI();
  j += ",\"sensors\":"; j += sensorsFound;
  j += ",\"golden\":"; j += (goldenOk ? "true" : "false");
  j += ",\"part\":\""; j += partName;
  j += "\",\"ip\":\""; j += WiFi.localIP().toString();
  j += "\",\"s\":[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += '['; j += String(out[i].x, 4);
    j += ','; j += String(out[i].y, 4);
    j += ','; j += String(out[i].z, 4);
    j += ','; j += String(out[i].ratio, 2);
    j += ']';
  }
  j += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleTriggers() {
  String j = "[";
  const uint32_t now = millis();
  const int total = trigCount;
  const int keep = total < TRIG_CAP ? total : TRIG_CAP;
  for (int i = 0; i < keep; i++) {
    const int slot = (total - keep + i) % TRIG_CAP;
    if (i) j += ',';
    j += "{\"id\":";     j += triggers[slot].id;
    j += ",\"age\":";    j += (now - triggers[slot].millisAt) / 1000;
    j += ",\"ratio\":";  j += String(triggers[slot].ratio, 2);
    j += ",\"pga\":";    j += String(triggers[slot].pga, 4);
    j += '}';
  }
  j += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleTriggerCsv() {
  const uint32_t want = server.hasArg("id") ? strtoul(server.arg("id").c_str(), nullptr, 10) : 0;
  int slot = -1;
  for (int i = 0; i < TRIG_CAP; i++) if (triggers[i].id == want && triggers[i].waveLen) { slot = i; break; }
  if (slot < 0) { server.send(404, "text/plain", "no such trigger"); return; }

  String csv = "i,ax,ay,az,ratio\n";
  csv.reserve(triggers[slot].waveLen * 40 + 32);
  for (int i = 0; i < triggers[slot].waveLen; i++) {
    const LiveSample &s = triggers[slot].wave[i];
    csv += i; csv += ',';
    csv += String(s.x, 4); csv += ',';
    csv += String(s.y, 4); csv += ',';
    csv += String(s.z, 4); csv += ',';
    csv += String(s.ratio, 3); csv += '\n';
  }
  server.sendHeader("Content-Disposition",
                    "attachment; filename=geoshake-trigger-" + String(want) + ".csv");
  server.send(200, "text/csv", csv);
}

static void handleSetupPage() { sendProgmem(PAGE_SETUP); }

// Networks the radio can hear, strongest first. Anything absent here is 5 GHz,
// hidden, or out of range — worth knowing before blaming the password.
static void handleScan() {
  const int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (i) j += ',';
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    j += "{\"ssid\":\""; j += s;
    j += "\",\"rssi\":"; j += WiFi.RSSI(i);
    j += ",\"open\":"; j += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
    j += '}';
  }
  j += "]";
  WiFi.scanDelete();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void handleSetupPost() {
  const String ssid = server.arg("ssid");
  const String pass = server.arg("pass");
  if (!ssid.length()) { server.send(400, "text/plain", "ssid required"); return; }
  prefs.begin("geoshake", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  server.send(200, "text/html",
              "<meta charset=utf-8><body style=\"background:#0d0f12;color:#e8eaed;"
              "font:15px -apple-system,sans-serif;padding:40px;text-align:center\">"
              "<h2>Saved — restarting</h2><p style=\"color:#8b9099\">Reconnect to your "
              "normal Wi-Fi, then open <b>http://geoshake-diy.local</b></p></body>");
  delay(900);
  ESP.restart();
}

// ================================== setup ===================================

// Turns wl_status_t into something you can act on. "It didn't connect" wastes
// your evening; "that SSID isn't on the air" tells you where to look.
static const char *wifiReason(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:  return "network not found — check spelling, or it may be 5 GHz only / out of range";
    case WL_CONNECT_FAILED: return "rejected — usually a wrong password";
    case WL_CONNECTION_LOST:return "connection dropped after associating";
    case WL_IDLE_STATUS:    return "still negotiating when time ran out — try again, it may be slow";
    case WL_DISCONNECTED:   return "disconnected — wrong password, or the router refused the join";
    default:                return "unknown";
  }
}

// Lists what the radio can actually hear. The ESP32 is 2.4 GHz only, so a
// network missing from this list is either 5 GHz, hidden, or too far away —
// and no amount of retyping the password will fix any of those.
static void scanAndReport(const String &wanted) {
  Serial.println(F("Wi-Fi: scanning for 2.4 GHz networks the device can see..."));
  const int n = WiFi.scanNetworks();
  if (n <= 0) { Serial.println(F("        none found at all — is the antenna connected?")); return; }
  bool sawWanted = false;
  for (int i = 0; i < n && i < 14; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid == wanted) sawWanted = true;
    Serial.print(F("        "));
    Serial.print(ssid == wanted ? F("-> ") : F("   "));
    Serial.print(ssid);
    Serial.print(F("  ")); Serial.print(WiFi.RSSI(i)); Serial.print(F(" dBm"));
    Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? F("  (open)") : F(""));
  }
  WiFi.scanDelete();
  if (wanted.length()) {
    Serial.println();
    if (sawWanted)
      Serial.println(F("        Your network IS visible — so this is almost certainly the password."));
    else {
      Serial.print(F("        \"")); Serial.print(wanted);
      Serial.println(F("\" is NOT in that list."));
      Serial.println(F("        Phone hotspots often default to 5 GHz — set yours to 2.4 GHz"));
      Serial.println(F("        (iPhone: Personal Hotspot > Maximise Compatibility ON)."));
    }
  }
}

static bool connectSaved() {
  prefs.begin("geoshake", true);
  const String ssid = prefs.getString("ssid", "");
  const String pass = prefs.getString("pass", "");
  prefs.end();
  if (!ssid.length()) {
    Serial.println(F("Wi-Fi: no network saved yet."));
    return false;
  }

  // Show enough of the stored password to spot a mangled one, not enough to
  // read it over someone's shoulder. A "+" that arrived as a space, or a
  // truncated paste, is obvious here and invisible everywhere else.
  String hint = "(empty)";
  if (pass.length()) {
    hint = pass.substring(0, 1) + "…" + pass.substring(pass.length() - 1) +
           " / " + String(pass.length()) + " chars";
    if (pass.indexOf(' ') >= 0) hint += "  [contains a SPACE]";
  }
  Serial.print(F("Wi-Fi: connecting to \"")); Serial.print(ssid);
  Serial.print(F("\"  password ")); Serial.println(hint);

  // Clear any leftover AP/STA state before joining — a half-configured radio
  // fails in ways that look exactly like a wrong password.
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());

  // 20 s: some routers are slow to hand out a lease, and giving up early looks
  // identical to a real failure.
  for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    if (i % 4 == 0) Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print(F("Wi-Fi: FAILED — "));
  Serial.println(wifiReason(WiFi.status()));

  // Fully park the radio before scanning. Going straight from a failed join to
  // a scan gets "sta is connecting, cannot set config" and returns nothing —
  // which looks like "no networks exist" and sends you chasing the wrong fault.
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_OFF);
  delay(400);
  WiFi.mode(WIFI_STA);
  delay(300);
  scanAndReport(ssid);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println(F("GeoShake DIY — web interface"));
  Serial.println(F("============================"));

  Serial.print(F("Golden vector self-test ... "));
  goldenOk = (goldenSelfTest() == 0);
  if (!goldenOk) {
    Serial.println(F("FAILED — refusing to run a detector we can't trust."));
    while (true) delay(1000);
  }
  Serial.println(F("PASS"));

  Serial.print(F("Sensors ... "));
  sensorsFound = sensors.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, CS_PINS, SENSOR_COUNT);
  if (sensorsFound == 0) {
    Serial.println(F("NONE FOUND — run spi_probe to check the wiring."));
    while (true) delay(1000);
  }
  partName = sensors.partName(0);
  Serial.print(sensorsFound); Serial.print(F(" x ")); Serial.println(partName);

  const bool online = connectSaved();

  if (online) {
    Serial.print(F("Wi-Fi: connected, http://")); Serial.println(WiFi.localIP());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.print(F("        also http://")); Serial.print(HOSTNAME); Serial.println(F(".local"));
    }
    server.on("/", handleRoot);
    server.on("/api/live", handleLive);
    server.on("/api/triggers", handleTriggers);
    server.on("/api/trigger", handleTriggerCsv);
  } else {
    // Keep AP+STA so the setup page can still scan for networks while serving.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    Serial.println();
    Serial.print(F("Wi-Fi: setup mode — join \"")); Serial.print(AP_SSID);
    Serial.println(F("\" and open http://192.168.4.1"));
    server.on("/", handleSetupPage);
    server.on("/api/scan", handleScan);
    server.on("/setup", HTTP_POST, handleSetupPost);
    server.onNotFound(handleSetupPage);   // captive-portal-ish: any URL lands on setup
  }
  server.begin();

  // Sampling gets its own core so HTTP can never stall it.
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 3, nullptr, 1);

  Serial.println();
  Serial.println(F("Warming up ~30 s, then the detector arms."));
}

void loop() {
  server.handleClient();
  delay(1);
}
