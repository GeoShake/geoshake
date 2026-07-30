// GeoShake DIY — I2C scanner / wiring troubleshooter
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Sensor NOT FOUND" is the single most common problem when building this
// project, and it is almost always the wiring — not a dead part. This sketch
// sweeps the usual ESP32-S3 pin pairs and every I2C address, then tells you
// exactly which pins and which address responded.
//
// Flash it, open the serial monitor at 115200, and copy the two numbers it
// prints into geoshake_diy.ino:
//     #define I2C_SDA_PIN <found sda>
//     #define I2C_SCL_PIN <found scl>

#include <Wire.h>

// Candidate SDA/SCL pairs, most likely first. Both orders are tried, because
// swapping SDA and SCL is the second most common mistake.
static const int8_t PIN_PAIRS[][2] = {
  { 8,  9},  // Arduino ESP32-S3 default
  { 9,  8},
  { 4,  5},
  { 5,  4},
  { 5,  6},
  { 6,  5},
  { 6,  7},
  { 1,  2},
  { 2,  1},
  {10, 11},
  {11, 10},
  {13, 14},
  {15, 16},
  {17, 18},
  {18, 17},
  {21, 47},
  {41, 42},
  {43, 44},
};
static const int PAIR_COUNT = sizeof(PIN_PAIRS) / sizeof(PIN_PAIRS[0]);

static const uint8_t REG_WHO_AM_I = 0x0F;

static const char *identify(uint8_t whoami) {
  switch (whoami) {
    case 0x69: return "LSM6DS3";
    case 0x6A: return "LSM6DS3TR-C";
    case 0x6C: return "LSM6DSO / LSM6DSOX (same family as the T1)";
    case 0x68: return "MPU6050 / MPU6000 (different family - driver won't match)";
    case 0x6B: return "LSM6DS0 or similar";
    case 0xE5: return "ADXL345 (different family - driver won't match)";
    default:   return "unknown part";
  }
}

static uint8_t readWhoAmI(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(REG_WHO_AM_I);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println();
  Serial.println(F("GeoShake DIY — I2C scanner"));
  Serial.println(F("=========================="));
  Serial.println(F("Sweeping common ESP32-S3 pin pairs, addresses 0x08-0x77..."));
  Serial.println();

  int totalFound = 0;

  for (int p = 0; p < PAIR_COUNT; p++) {
    const int sda = PIN_PAIRS[p][0];
    const int scl = PIN_PAIRS[p][1];

    Wire.end();
    delay(5);
    if (!Wire.begin(sda, scl, 100000)) continue;
    delay(20);

    // Warm-up transaction: the very first transfer after Wire.begin() can
    // report a false ACK while the bus settles. Throw one away.
    Wire.beginTransmission(0x00);
    Wire.endTransmission();
    delay(5);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
      // Require 3 out of 3 ACKs — a real device answers every time, bus noise
      // does not. This is what separates a found sensor from a floating pin.
      int acks = 0;
      for (int attempt = 0; attempt < 3; attempt++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) acks++;
        delay(2);
      }
      if (acks < 3) continue;

      totalFound++;
      Serial.print(F("FOUND  SDA=GPIO"));
      Serial.print(sda);
      Serial.print(F("  SCL=GPIO"));
      Serial.print(scl);
      Serial.print(F("  addr=0x"));
      Serial.print(addr, HEX);

      // 0x6A/0x6B are the LSM6 family addresses — ask what it is.
      if (addr == 0x6A || addr == 0x6B) {
        const uint8_t id = readWhoAmI(addr);
        Serial.print(F("  WHO_AM_I=0x"));
        Serial.print(id, HEX);
        Serial.print(F("  -> "));
        Serial.print(identify(id));
      }
      Serial.println();

      Serial.print(F("       => put this in geoshake_diy.ino:  #define I2C_SDA_PIN "));
      Serial.print(sda);
      Serial.print(F("   #define I2C_SCL_PIN "));
      Serial.println(scl);
      Serial.println();
    }
  }

  Serial.println(F("--- scan complete ---"));
  if (totalFound == 0) {
    Serial.println(F("Nothing responded on any pin pair. That means power or wiring:"));
    Serial.println(F("  1. Is VIN/VCC on 3V3 and GND on GND? (a missing ground is silent)"));
    Serial.println(F("  2. Are SDA and SCL actually on two of the scanned pins?"));
    Serial.println(F("     If you used other GPIOs, add them to PIN_PAIRS above."));
    Serial.println(F("  3. Some breakouts need a pull-up; most have them onboard."));
    Serial.println(F("  4. Try a different jumper — a broken wire looks exactly like this."));
  } else {
    Serial.print(F("Devices found: "));
    Serial.println(totalFound);
  }
}

void loop() {
  delay(5000);
}
