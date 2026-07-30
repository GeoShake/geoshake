// GeoShake DIY — array gain measurement
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Answers a question with a number instead of a claim: how much does a second
// accelerometer actually buy you?
//
// Three detectors run in parallel on the SAME samples:
//   A: sensor 0 alone
//   B: sensor 1 alone
//   C: both, averaged  ((s0 + s1) / 2)
//
// Running them simultaneously rather than one after another matters. Background
// vibration changes minute to minute — someone walks past, a fridge kicks in —
// so sequential measurements would compare different rooms. These three see the
// identical noise, sample for sample.
//
// The noise floor is sqrt(LTA): the RMS of the high-passed background, in m/s^2.
// Sensor noise is uncorrelated between parts, so averaging two should reduce it
// by ~sqrt(2) = 1.41x. Ground motion is correlated, so it survives averaging
// untouched. That gap is array gain, and it is why the T1 carries four sensors.
//
// HOW TO RUN: flash, then leave the desk completely alone for ~4 minutes.
// Every knock and footstep is signal, not noise, and will pollute the result.

#include <SPI.h>
#include "stalta.h"
#include "lsm6_spi.h"

using namespace geoshake;

#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int CS_PINS[] = {9, 10};
static const int SENSOR_COUNT = 2;

static const uint32_t SETTLE_MS  = 90000;   // let the 30 s LTA converge properly
static const uint32_t REPORT_MS  = 10000;

// A knock, a footstep, a slammed door — any of it inflates the "noise" floor
// and quietly ruins the result. Rather than trusting the room to stay quiet,
// watch the STA/LTA ratio: in a still room it sits near 1. If it goes past this
// during a reporting window, that window is thrown away instead of averaged in.
static const double DISTURB_RATIO = 2.5;

static Lsm6Spi sensors;
static StaltaState detA, detB, detC;

static uint32_t sampleCount = 0;
static uint32_t startMs = 0;
static uint32_t lastReportMs = 0;
static bool settled = false;

// Running mean of the gain after settling, so one noisy report cannot decide it.
static double gainSum = 0.0;
static int    gainSamples = 0;
static int    discarded = 0;

// Peak ratio seen since the last report — the disturbance detector.
static double windowPeakRatio = 0.0;

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println(F("GeoShake DIY — array gain measurement"));
  Serial.println(F("====================================="));

  const int found = sensors.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, CS_PINS, SENSOR_COUNT);
  if (found < 2) {
    Serial.print(F("Need two sensors, found ")); Serial.println(found);
    Serial.println(F("Run spi_probe to check the wiring."));
    while (true) delay(1000);
  }
  Serial.print(F("Two sensors OK: "));
  Serial.print(sensors.partName(0)); Serial.print(F(" + ")); Serial.println(sensors.partName(1));
  Serial.println();
  Serial.println(F("KEEP THE DESK PERFECTLY STILL. Any knock counts as signal."));
  Serial.print(F("Settling for ")); Serial.print(SETTLE_MS / 1000); Serial.println(F(" s, then reporting every 10 s."));
  Serial.println();

  detA = initStalta();
  detB = initStalta();
  detC = initStalta();
  startMs = millis();
  lastReportMs = startMs;
}

void loop() {
  if (!sensors.sampleReady()) return;

  double s0[3], s1[3];
  if (!sensors.read(0, s0)) return;
  if (!sensors.read(1, s1)) return;

  double buf[12];

  expandSingleSensor(s0[0], s0[1], s0[2], buf);
  staltaStep(detA, buf);

  expandSingleSensor(s1[0], s1[1], s1[2], buf);
  staltaStep(detB, buf);

  // Refill buf before detector C — it must see the AVERAGE of both sensors.
  // (Reusing the previous buffer here silently turns C into a copy of B: the
  // reported "gain" then compares sensor 1 against itself and means nothing.)
  expandTwoSensors(s0, s1, buf);
  const StaltaResult rC = staltaStep(detC, buf);

  sampleCount++;
  if (rC.ratio > windowPeakRatio) windowPeakRatio = rC.ratio;

  const uint32_t now = millis();
  if (now - lastReportMs < REPORT_MS) return;
  lastReportMs = now;

  const double nA = sqrt(detA.lta);
  const double nB = sqrt(detB.lta);
  const double nC = sqrt(detC.lta);
  const double singleMean = (nA + nB) / 2.0;
  const double gain = (nC > 0.0) ? singleMean / nC : 0.0;

  const uint32_t elapsed = now - startMs;
  if (!settled && elapsed >= SETTLE_MS) {
    settled = true;
    Serial.println();
    Serial.println(F(">>> settled — measurements from here count <<<"));
    Serial.println();
  }

  const bool disturbed = windowPeakRatio > DISTURB_RATIO;
  const double peakThisWindow = windowPeakRatio;
  windowPeakRatio = 0.0;

  Serial.print(settled ? F("[measure] ") : F("[settle]  "));
  Serial.print(elapsed / 1000); Serial.print(F("s  n=")); Serial.print(sampleCount);
  Serial.print(F("  s0=")); Serial.print(nA, 6);
  Serial.print(F("  s1=")); Serial.print(nB, 6);
  Serial.print(F("  avg=")); Serial.print(nC, 6);
  Serial.print(F("  gain=")); Serial.print(gain, 3);
  Serial.print(F("x"));
  if (disturbed) {
    Serial.print(F("   <-- DISCARDED, something moved (peak ratio "));
    Serial.print(peakThisWindow, 1);
    Serial.print(F(")"));
  }
  Serial.println();

  if (settled) {
    if (disturbed) {
      discarded++;
    } else {
      gainSum += gain;
      gainSamples++;
    }

    // Summarise every 6 clean reports (~1 minute of usable data).
    if (gainSamples > 0 && gainSamples % 6 == 0 && !disturbed) {
      const double meanGain = gainSum / gainSamples;
      Serial.println();
      Serial.println(F("  ---- running result (clean windows only) ----"));
      Serial.print(F("  single-sensor noise floor : ")); Serial.print(singleMean, 6); Serial.println(F(" m/s^2"));
      Serial.print(F("  two-sensor  noise floor   : ")); Serial.print(nC, 6); Serial.println(F(" m/s^2"));
      Serial.print(F("  measured array gain       : ")); Serial.print(meanGain, 3);
      Serial.println(F("x   (theory for uncorrelated noise: 1.414x)"));
      Serial.print(F("  clean windows averaged    : ")); Serial.println(gainSamples);
      Serial.print(F("  discarded (room moved)    : ")); Serial.println(discarded);
      Serial.println();
    }
  }
}
