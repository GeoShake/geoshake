// GeoShake DIY — golden-vector self test
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// These are the same golden vectors that lock the shipping T1 firmware to the
// TypeScript reference implementation (firmware/shared/test/stalta.test.ts).
// If your build reproduces these three numbers, your DSP is bit-compatible with
// the reference — the exact same detection maths that runs on a real GeoShake
// station. That is a claim you can make and prove, not just assert.
//
// Run goldenSelfTest() once at boot. If it fails, something in your toolchain
// broke the arithmetic (almost always: float instead of double, or -ffast-math).

#ifndef GEOSHAKE_DIY_GOLDEN_H
#define GEOSHAKE_DIY_GOLDEN_H

#include <math.h>
#include "stalta.h"

namespace geoshake {

struct GoldenResult {
  bool   passed;
  double sta;        // what this build produced
  double lta;
  double ratio;
  double staExpected;
  double ltaExpected;
  double ratioExpected;
  int    failedCase;  // 0 = none, 1 = identical-sensor, 2 = differing-sensor
};

// Vector 1 — all four sensors identical. Locks the core recursion.
inline GoldenResult goldenIdenticalSensors() {
  const double seq[5][3] = {
    {9.81,  0.00,  0.00},
    {9.85,  0.02,  0.00},
    {9.80, -0.03,  0.01},
    {9.90,  0.05, -0.02},
    {9.82,  0.00,  0.00},
  };
  StaltaState st = initStalta();
  StaltaResult r = {false, 0.0, 0.0};
  double s12[12];
  for (int i = 0; i < 5; i++) {
    expandSingleSensor(seq[i][0], seq[i][1], seq[i][2], s12);
    r = staltaStep(st, s12);
  }

  GoldenResult g;
  g.sta = st.sta;   g.staExpected   = 0.0002452873445456898;
  g.lta = st.lta;   g.ltaExpected   = 0.000004237485960969144;
  g.ratio = r.ratio; g.ratioExpected = 57.88511084284295;
  // Tolerances mirror the reference test's toBeCloseTo(value, digits):
  // |actual - expected| < 0.5 * 10^-digits
  g.passed = fabs(g.sta   - g.staExpected)   < 0.5e-10 &&
             fabs(g.lta   - g.ltaExpected)   < 0.5e-10 &&
             fabs(g.ratio - g.ratioExpected) < 0.5e-8;
  g.failedCase = g.passed ? 0 : 1;
  return g;
}

// Vector 2 — all four sensors DIFFER. Locks the (s0+s3+s6+s9)/4 index mapping
// and the operation order, which vector 1 cannot catch.
inline GoldenResult goldenDifferingSensors() {
  const double d[3][12] = {
    {9.81, 0.10, 0.20,  9.79, 0.11, 0.19,  9.83, 0.09, 0.21,  9.80, 0.12, 0.18},
    {9.85, 0.12, 0.22,  9.82, 0.10, 0.20,  9.88, 0.14, 0.24,  9.84, 0.11, 0.19},
    {9.78, 0.08, 0.18,  9.81, 0.13, 0.23,  9.79, 0.07, 0.17,  9.86, 0.15, 0.25},
  };
  StaltaState st = initStalta();
  StaltaResult r = {false, 0.0, 0.0};
  for (int i = 0; i < 3; i++) r = staltaStep(st, d[i]);

  GoldenResult g;
  g.sta = st.sta;   g.staExpected   = 0.00003883608158025049;
  g.lta = st.lta;   g.ltaExpected   = 6.74217320754769e-7;
  g.ratio = r.ratio; g.ratioExpected = 57.60172630506509;
  g.passed = fabs(g.sta   - g.staExpected)   < 0.5e-12 &&
             fabs(g.lta   - g.ltaExpected)   < 0.5e-12 &&
             fabs(g.ratio - g.ratioExpected) < 0.5e-8;
  g.failedCase = g.passed ? 0 : 2;
  return g;
}

// Behavioural checks — cheap, and they catch logic mistakes the golden numbers
// would not (a broken warm-up gate, a missing LTA freeze, no hysteresis).
inline bool behaviourSelfTest() {
  double s12[12];

  // 1. Steady input must never fire.
  {
    StaltaState st = initStalta();
    int fires = 0;
    for (int i = 0; i < 200; i++) {
      expandSingleSensor(9.81, 0.0, 0.0, s12);
      if (staltaStep(st, s12).fired) fires++;
    }
    if (fires != 0) return false;
  }

  // 2. A strong signal during warm-up must NOT fire.
  {
    StaltaState st = initStalta();
    int fires = 0;
    for (int i = 0; i < 100; i++) {
      expandSingleSensor(9.81 + ((i % 2) ? 1.0 : -1.0), 0.0, 0.0, s12);
      if (staltaStep(st, s12).fired) fires++;
    }
    if (fires != 0) return false;
  }

  // 3. After warm-up, a strong onset fires exactly once (rising edge).
  {
    StaltaState st = initStalta();
    for (uint32_t i = 0; i < STALTA_PARAMS.warmupSamples + 100; i++) {
      expandSingleSensor(9.81, 0.0, 0.0, s12);
      staltaStep(st, s12);
    }
    int fires = 0;
    for (int i = 0; i < 50; i++) {
      expandSingleSensor(9.81 + ((i % 2) ? 0.5 : -0.5), 0.0, 0.0, s12);
      if (staltaStep(st, s12).fired) fires++;
    }
    if (fires != 1) return false;
  }

  // 4. LTA freezes while triggered, and the trigger re-arms once it goes quiet.
  {
    StaltaState st = initStalta();
    for (uint32_t i = 0; i < STALTA_PARAMS.warmupSamples + 100; i++) {
      expandSingleSensor(9.81, 0.0, 0.0, s12);
      staltaStep(st, s12);
    }
    expandSingleSensor(20.0, 0.0, 0.0, s12);
    StaltaResult r = staltaStep(st, s12);
    int guard = 0;
    while (!r.fired && guard++ < 10000) r = staltaStep(st, s12);
    if (!r.fired) return false;
    if (r.pga <= 0.0) return false;
    if (r.ratio < STALTA_PARAMS.trigger) return false;
    const double ltaAtTrigger = st.lta;
    staltaStep(st, s12);
    if (st.lta != ltaAtTrigger) return false;   // must be frozen
    if (!st.triggered || st.armed) return false;

    guard = 0;
    while (st.triggered && guard++ < 5000) {
      expandSingleSensor(9.81, 0.0, 0.0, s12);
      staltaStep(st, s12);
    }
    if (st.triggered || !st.armed) return false;  // must re-arm
  }

  return true;
}

// Returns 0 on success, or the failing case number.
inline int goldenSelfTest() {
  GoldenResult g1 = goldenIdenticalSensors();
  if (!g1.passed) return 1;
  GoldenResult g2 = goldenDifferingSensors();
  if (!g2.passed) return 2;
  if (!behaviourSelfTest()) return 3;
  return 0;
}

}  // namespace geoshake

#endif  // GEOSHAKE_DIY_GOLDEN_H
