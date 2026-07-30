// GeoShake DIY — STA/LTA seismic trigger (C++ port of the reference implementation)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Ported line-for-line from firmware/shared/src/stalta.ts — the same reference
// implementation the shipping GeoShake T1 firmware is bit-locked to. The golden
// vectors in golden.h prove this port reproduces the reference arithmetic exactly.
//
// TWO RULES if you modify this file:
//   1. Keep everything `double`. The reference is JavaScript (IEEE-754 double);
//      switching to float breaks the golden vectors immediately.
//   2. Keep the operation order. `(s0 + s3 + s6 + s9) / 4` is not the same
//      floating-point result as averaging in a different order.

#ifndef GEOSHAKE_DIY_STALTA_H
#define GEOSHAKE_DIY_STALTA_H

#include <math.h>
#include <stdint.h>

namespace geoshake {

// Detection parameters — identical to the reference implementation.
//
// These are tuned for ~109 Hz effective sample rate (the T1 configures the
// LSM6DSO for a nominal 104 Hz ODR; the part's internal clock runs slightly
// fast, giving ~109 Hz in practice).
//
// If you run your build at a different rate, recompute:
//     alphaHp       = 2 * PI * fc / fs      (fc ~= 0.2 Hz high-pass corner)
//     nSta          = 0.5 s * fs
//     nLta          = 30 s  * fs
//     warmupSamples = nLta
//
// But note: changing any of these invalidates the golden vectors, because the
// golden values encode these exact constants. Keep them as-is to stay verifiable,
// and to keep your build comparable to a real T1.
struct StaltaParams {
  double alphaHp        = 0.0115;  // 1-pole high-pass coefficient (~0.2 Hz @ 109 Hz)
  int    nSta           = 55;      // short-term average window, samples (~0.5 s)
  int    nLta           = 3270;    // long-term average window, samples (~30 s)
  double ltaMin         = 1e-8;    // LTA floor (m/s^2)^2 — divide-by-zero guard
  double trigger        = 6.0;     // STA/LTA ratio that fires a detection
  double detrigger      = 2.0;     // ratio that re-arms (hysteresis, stops chattering)
  uint32_t warmupSamples = 3270;   // suppress triggers until the LTA has filled (~30 s)
};

static const StaltaParams STALTA_PARAMS;

struct StaltaState {
  double   dc[3];      // running DC estimate per axis (the high-pass memory)
  double   sta;        // short-term average of the characteristic function
  double   lta;        // long-term average
  bool     armed;      // ready to fire
  bool     triggered;  // currently in a detection (LTA frozen)
  uint32_t count;      // samples seen — drives warm-up
};

struct StaltaResult {
  bool   fired;  // this sample crossed the trigger threshold (rising edge only)
  double ratio;  // STA / LTA
  double pga;    // |high-passed acceleration| at this sample, m/s^2
};

inline StaltaState initStalta() {
  StaltaState s;
  s.dc[0] = 0.0; s.dc[1] = 0.0; s.dc[2] = 0.0;
  s.sta = 0.0;
  s.lta = STALTA_PARAMS.ltaMin;
  s.armed = true;
  s.triggered = false;
  s.count = 0;
  return s;
}

// One detection step.
//
// `sample` holds 12 values: 4 sensors x 3 axes, sensor-major, RAW (gravity
// included), in m/s^2. The T1 has four physical accelerometers and averages
// them for array gain. A single-sensor DIY build passes the same reading four
// times (see expandSingleSensor below) — the average is then just that reading,
// the maths is preserved exactly, and the only thing you give up is the array
// gain that lowers the noise floor. That difference is the honest answer to
// "why does the T1 have four sensors?".
inline StaltaResult staltaStep(StaltaState &s, const double sample[12]) {
  const StaltaParams &P = STALTA_PARAMS;

  const double ax = (sample[0] + sample[3] + sample[6] + sample[9])  / 4.0;
  const double ay = (sample[1] + sample[4] + sample[7] + sample[10]) / 4.0;
  const double az = (sample[2] + sample[5] + sample[8] + sample[11]) / 4.0;

  // On the first sample, seed the DC estimate with the sample itself. Without
  // this the high-pass would ring for seconds while it chased gravity.
  double dc0, dc1, dc2;
  if (s.count == 0) {
    dc0 = ax; dc1 = ay; dc2 = az;
  } else {
    dc0 = s.dc[0] + P.alphaHp * (ax - s.dc[0]);
    dc1 = s.dc[1] + P.alphaHp * (ay - s.dc[1]);
    dc2 = s.dc[2] + P.alphaHp * (az - s.dc[2]);
  }

  const double hx = ax - dc0, hy = ay - dc1, hz = az - dc2;
  const double cf = hx * hx + hy * hy + hz * hz;  // characteristic function = energy

  const double sta = s.sta + (cf - s.sta) / (double)P.nSta;

  // LTA FREEZES while triggered — otherwise a long event would raise its own
  // baseline and de-trigger itself mid-shaking.
  double lta = s.triggered ? s.lta : s.lta + (cf - s.lta) / (double)P.nLta;
  if (lta < P.ltaMin) lta = P.ltaMin;

  const double ratio = sta / lta;
  const uint32_t count = s.count + 1;

  bool armed = s.armed, triggered = s.triggered, fired = false;
  if (count >= P.warmupSamples && armed && ratio >= P.trigger) {
    fired = true; triggered = true; armed = false;
  }
  if (triggered && ratio <= P.detrigger) { triggered = false; armed = true; }

  s.dc[0] = dc0; s.dc[1] = dc1; s.dc[2] = dc2;
  s.sta = sta; s.lta = lta;
  s.armed = armed; s.triggered = triggered; s.count = count;

  StaltaResult r;
  r.fired = fired;
  r.ratio = ratio;
  r.pga = sqrt(cf);
  return r;
}

// Single-sensor adapter: one accelerometer reading -> the 12-channel layout the
// reference DSP expects. Repeating the reading four times makes the average
// return exactly that reading, so the arithmetic is untouched.
inline void expandSingleSensor(double ax, double ay, double az, double out[12]) {
  for (int i = 0; i < 4; i++) {
    out[i * 3 + 0] = ax;
    out[i * 3 + 1] = ay;
    out[i * 3 + 2] = az;
  }
}

// Two-sensor adapter: each sensor fills two of the four slots, so the average
// becomes (s0 + s1) / 2 — real array gain from two parts, using the same
// unmodified reference arithmetic. Uncorrelated sensor noise drops by ~sqrt(2)
// while a real ground motion, which both sensors see identically, does not.
// That is the whole reason the T1 carries four.
inline void expandTwoSensors(const double s0[3], const double s1[3], double out[12]) {
  for (int axis = 0; axis < 3; axis++) {
    out[0 * 3 + axis] = s0[axis];
    out[1 * 3 + axis] = s0[axis];
    out[2 * 3 + axis] = s1[axis];
    out[3 * 3 + axis] = s1[axis];
  }
}

}  // namespace geoshake

#endif  // GEOSHAKE_DIY_STALTA_H
