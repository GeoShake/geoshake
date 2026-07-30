// GeoShake DIY — a real earthquake trigger on a breadboard
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware:  ESP32-S3 + one or two LSM6DS3 / LSM6DSO accelerometers on SPI.
// Detection: the same STA/LTA algorithm the shipping GeoShake T1 runs, ported
//            from the published reference implementation and verified against
//            its golden vectors at boot — on this actual silicon, not just on
//            a PC.
//
// What this build does:      detects shaking, reports STA/LTA ratio and PGA,
//                            measures your noise floor, dumps the waveform
//                            around each trigger as CSV.
// What this build does NOT:  no calibrated noise floor, no network validation,
//                            no confirmed events, no signed OTA, no alert to
//                            anyone but you. It teaches you the physics. A T1
//                            puts a verified station on a global network.
//
// Wiring (run spi_probe first if you are unsure):
//   sensor VDD -> 3V3        SCLK -> GPIO 12
//   sensor GND -> GND        MOSI -> GPIO 11  (sensor pin marked SDA/SDI)
//   CS sensor 1 -> GPIO 9    MISO -> GPIO 13  (sensor pin marked SDO)
//   CS sensor 2 -> GPIO 10
//
// Serial monitor: 115200 baud.
// Flash with USB CDC On Boot: Enabled, or the serial output never appears.

#include <SPI.h>
#include "stalta.h"
#include "golden.h"
#include "lsm6_spi.h"

using namespace geoshake;

// ---- wiring -----------------------------------------------------------------
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int CS_PINS[] = {9, 10};   // one per sensor; drop to {9} for a single sensor
static const int SENSOR_COUNT = sizeof(CS_PINS) / sizeof(CS_PINS[0]);

// ---- waveform ring buffer ---------------------------------------------------
// The T1 keeps ~6.5 s around each trigger. We keep ~2 s: enough to show the
// onset arriving *before* the trigger fired, which is the whole point of early
// warning — the P-wave is already in the buffer when the alarm goes off.
static const int RING_LEN = 220;              // ~2 s at ~109 Hz
static const int POST_TRIGGER_SAMPLES = 110;  // ~1 s recorded after the trigger

struct RingSample { double ax, ay, az, ratio; };
static RingSample ring[RING_LEN];
static int  ringHead = 0;
static bool ringFilled = false;

static StaltaState detector;
static Lsm6Spi sensors;

static int  postTriggerCountdown = -1;
static uint32_t triggerCount = 0;
static uint32_t sampleCount = 0;
static uint32_t lastStatusMs = 0;
static uint32_t rateWindowStart = 0;
static uint32_t rateWindowSamples = 0;
static double   measuredRate = 0.0;

static void ringPush(double ax, double ay, double az, double ratio) {
  ring[ringHead] = {ax, ay, az, ratio};
  ringHead = (ringHead + 1) % RING_LEN;
  if (ringHead == 0) ringFilled = true;
}

static void dumpWaveform() {
  Serial.println();
  Serial.println(F("--- WAVEFORM DUMP (CSV) ---"));
  Serial.println(F("i,ax,ay,az,ratio"));
  const int count = ringFilled ? RING_LEN : ringHead;
  const int start = ringFilled ? ringHead : 0;
  for (int i = 0; i < count; i++) {
    const RingSample &s = ring[(start + i) % RING_LEN];
    Serial.print(i);       Serial.print(',');
    Serial.print(s.ax, 4); Serial.print(',');
    Serial.print(s.ay, 4); Serial.print(',');
    Serial.print(s.az, 4); Serial.print(',');
    Serial.println(s.ratio, 3);
  }
  Serial.println(F("--- END DUMP ---"));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println(F("GeoShake DIY seismometer"));
  Serial.println(F("========================"));

  // 1. Prove the DSP before trusting a single thing it says.
  Serial.print(F("Golden vector self-test ... "));
  const int golden = goldenSelfTest();
  if (golden != 0) {
    Serial.print(F("FAILED (case ")); Serial.print(golden); Serial.println(F(")"));
    Serial.println(F("The detection maths does not match the GeoShake reference."));
    Serial.println(F("Almost always: float instead of double, or -ffast-math enabled."));
    while (true) delay(1000);
  }
  Serial.println(F("PASS"));
  Serial.println(F("  -> bit-compatible with the reference DSP, on this chip"));

  // 2. Bring up the sensors.
  Serial.print(F("Sensors ... "));
  const int found = sensors.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, CS_PINS, SENSOR_COUNT);
  if (found == 0) {
    Serial.println(F("NONE FOUND"));
    Serial.println(F("Run the spi_probe sketch — it sweeps MISO and reports the real wiring."));
    while (true) delay(1000);
  }
  Serial.print(found); Serial.print(F(" of ")); Serial.println(SENSOR_COUNT);
  for (int i = 0; i < SENSOR_COUNT; i++) {
    Serial.print(F("  CS=GPIO")); Serial.print(CS_PINS[i]);
    Serial.print(F("  WHO_AM_I=0x")); Serial.print(sensors.whoami(i), HEX);
    Serial.print(F("  ")); Serial.println(sensors.partName(i));
  }
  Serial.println(F("  104 Hz, +-2 g"));
  if (found >= 2) {
    Serial.println(F("  two sensors -> averaged: uncorrelated noise drops ~sqrt(2),"));
    Serial.println(F("  real ground motion does not. That is array gain, and it is"));
    Serial.println(F("  why the T1 carries four."));
  }

  detector = initStalta();
  rateWindowStart = millis();

  Serial.println();
  Serial.print(F("Warming up ~"));
  Serial.print(STALTA_PARAMS.warmupSamples / 109);
  Serial.println(F(" s — the long-term average has to fill before triggers arm."));
  Serial.println(F("Keep the desk still. Then knock on it."));
  Serial.println();
}

void loop() {
  if (!sensors.sampleReady()) return;

  double s[2][3];
  bool ok0 = sensors.read(0, s[0]);
  bool ok1 = (SENSOR_COUNT > 1) ? sensors.read(1, s[1]) : false;
  if (!ok0 && !ok1) return;
  if (!ok0) { s[0][0] = s[1][0]; s[0][1] = s[1][1]; s[0][2] = s[1][2]; }

  double sample12[12];
  if (ok0 && ok1) {
    expandTwoSensors(s[0], s[1], sample12);      // (s0 + s1) / 2 — real array gain
  } else {
    expandSingleSensor(s[0][0], s[0][1], s[0][2], sample12);
  }

  const StaltaResult r = staltaStep(detector, sample12);

  sampleCount++;
  rateWindowSamples++;
  // Averaged reading, for the log and the waveform dump.
  const double ax = (sample12[0] + sample12[3] + sample12[6] + sample12[9]) / 4.0;
  const double ay = (sample12[1] + sample12[4] + sample12[7] + sample12[10]) / 4.0;
  const double az = (sample12[2] + sample12[5] + sample12[8] + sample12[11]) / 4.0;
  ringPush(ax, ay, az, r.ratio);

  if (r.fired) {
    triggerCount++;
    Serial.println();
    Serial.println(F("*** SHAKING DETECTED ***"));
    Serial.print(F("  trigger #")); Serial.println(triggerCount);
    Serial.print(F("  STA/LTA  ")); Serial.print(r.ratio, 2);
    Serial.print(F("  (threshold ")); Serial.print(STALTA_PARAMS.trigger, 1); Serial.println(F(")"));
    Serial.print(F("  PGA      ")); Serial.print(r.pga, 4); Serial.println(F(" m/s^2"));
    postTriggerCountdown = POST_TRIGGER_SAMPLES;
  }

  if (postTriggerCountdown > 0) {
    postTriggerCountdown--;
  } else if (postTriggerCountdown == 0) {
    postTriggerCountdown = -1;
    dumpWaveform();
  }

  const uint32_t now = millis();

  // Measure the true sample rate over 5 s windows. The STA/LTA windows assume
  // ~109 Hz; if yours is far off, that is what to recompute the constants from.
  if (now - rateWindowStart >= 5000) {
    measuredRate = rateWindowSamples * 1000.0 / (now - rateWindowStart);
    rateWindowStart = now;
    rateWindowSamples = 0;
  }

  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    const bool warm = detector.count >= STALTA_PARAMS.warmupSamples;
    Serial.print(warm ? F("[armed] ") : F("[warmup] "));
    Serial.print(F("n="));      Serial.print(sampleCount);
    Serial.print(F("  rate=")); Serial.print(measuredRate, 1); Serial.print(F(" Hz"));
    Serial.print(F("  ratio=")); Serial.print(detector.sta / detector.lta, 2);
    // sqrt(LTA) is the RMS of the background — your noise floor, in m/s^2.
    Serial.print(F("  noise=")); Serial.print(sqrt(detector.lta), 5);
    Serial.print(F("  triggers=")); Serial.println(triggerCount);
  }
}
