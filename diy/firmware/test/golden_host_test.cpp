// GeoShake DIY — host-side golden vector test
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Verifies the C++ port against the reference implementation's golden vectors
// without any hardware. Build and run:
//
//     c++ -std=c++11 -O2 -I../geoshake_diy golden_host_test.cpp -o golden_test && ./golden_test
//
// Exit code 0 = your DSP is bit-compatible with the GeoShake reference.

#include <stdio.h>
#include "stalta.h"
#include "golden.h"

using namespace geoshake;

static int report(const char *name, const GoldenResult &g) {
  printf("%-34s %s\n", name, g.passed ? "PASS" : "FAIL");
  printf("   sta   got %.17g\n         want %.17g\n", g.sta, g.staExpected);
  printf("   lta   got %.17g\n         want %.17g\n", g.lta, g.ltaExpected);
  printf("   ratio got %.17g\n         want %.17g\n", g.ratio, g.ratioExpected);
  return g.passed ? 0 : 1;
}

int main() {
  printf("GeoShake DIY — STA/LTA golden vector test\n");
  printf("=========================================\n\n");

  int failures = 0;
  failures += report("golden 1: identical sensors", goldenIdenticalSensors());
  printf("\n");
  failures += report("golden 2: differing sensors", goldenDifferingSensors());
  printf("\n");

  const bool behaviour = behaviourSelfTest();
  printf("%-34s %s\n\n", "behaviour suite", behaviour ? "PASS" : "FAIL");
  if (!behaviour) failures++;

  if (failures == 0) {
    printf("ALL PASS — this build is bit-compatible with the GeoShake reference DSP.\n");
    return 0;
  }
  printf("%d FAILURE(S). Check: double (not float) throughout, no -ffast-math.\n", failures);
  return 1;
}
