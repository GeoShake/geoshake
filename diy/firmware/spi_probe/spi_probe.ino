// GeoShake DIY — SPI wiring probe for LSM6 sensors
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finds which pins your LSM6DS3 / LSM6DSO sensors are actually wired to.
//
// Why this exists: on these parts the CS pin also selects the bus mode. Held
// high they speak I2C; pulled low they speak SPI. So a sensor wired for SPI is
// completely invisible to an I2C scanner — you get a clean "nothing found"
// even though the hardware is fine. If an I2C scan came up empty and your
// board has CS pins, you are on SPI and this is the tool you want.
//
// Edit SCLK/MOSI below if yours differ, then read the serial monitor at 115200.

#include <SPI.h>

// Known from the wiring. Change if your board differs.
#define PIN_SCLK 12
#define PIN_MOSI 11

// MISO is the one people most often forget to note down, so we sweep it.
// 13 first: that is what the GeoShake T1 uses.
static const int8_t MISO_CANDIDATES[] = {13, 14, 3, 21, 46, 10, 9, 18, 17, 8};
static const int MISO_COUNT = sizeof(MISO_CANDIDATES) / sizeof(MISO_CANDIDATES[0]);

// Chip-select pins to test, one per sensor.
static const int8_t CS_PINS[] = {9, 10};
static const int CS_COUNT = sizeof(CS_PINS) / sizeof(CS_PINS[0]);

static const uint8_t REG_WHO_AM_I = 0x0F;

static const char *identify(uint8_t id) {
  switch (id) {
    case 0x69: return "LSM6DS3";
    case 0x6A: return "LSM6DS3TR-C";
    case 0x6C: return "LSM6DSO / LSM6DSOX (the part the T1 ships with)";
    default:   return NULL;
  }
}

// SPI mode 3, 2 MHz — the same settings the T1 firmware uses.
static SPISettings spiSettings(2000000, MSBFIRST, SPI_MODE3);

static uint8_t readReg(int csPin, uint8_t reg) {
  SPI.beginTransaction(spiSettings);
  digitalWrite(csPin, LOW);
  SPI.transfer(0x80 | reg);           // MSB set = read
  const uint8_t val = SPI.transfer(0x00);
  digitalWrite(csPin, HIGH);
  SPI.endTransaction();
  return val;
}

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println();
  Serial.println(F("GeoShake DIY — SPI probe"));
  Serial.println(F("========================"));
  Serial.print(F("SCLK=GPIO")); Serial.print(PIN_SCLK);
  Serial.print(F("  MOSI=GPIO")); Serial.println(PIN_MOSI);
  Serial.print(F("Testing CS pins:"));
  for (int i = 0; i < CS_COUNT; i++) { Serial.print(F(" GPIO")); Serial.print(CS_PINS[i]); }
  Serial.println();
  Serial.println(F("Sweeping MISO candidates..."));
  Serial.println();

  for (int i = 0; i < CS_COUNT; i++) {
    pinMode(CS_PINS[i], OUTPUT);
    digitalWrite(CS_PINS[i], HIGH);   // deselect everything first
  }

  int foundMiso = -1;
  int foundCount = 0;

  for (int m = 0; m < MISO_COUNT; m++) {
    const int miso = MISO_CANDIDATES[m];

    // Skip a MISO candidate that collides with a pin already in use.
    bool collides = (miso == PIN_SCLK || miso == PIN_MOSI);
    for (int i = 0; i < CS_COUNT; i++) if (miso == CS_PINS[i]) collides = true;
    if (collides) continue;

    SPI.end();
    delay(5);
    SPI.begin(PIN_SCLK, miso, PIN_MOSI, -1);
    delay(10);

    int hitsThisMiso = 0;
    for (int i = 0; i < CS_COUNT; i++) {
      const uint8_t id = readReg(CS_PINS[i], REG_WHO_AM_I);
      const char *name = identify(id);
      if (name) {
        hitsThisMiso++;
        Serial.print(F("FOUND  sensor on CS=GPIO"));
        Serial.print(CS_PINS[i]);
        Serial.print(F("  MISO=GPIO"));
        Serial.print(miso);
        Serial.print(F("  WHO_AM_I=0x"));
        Serial.print(id, HEX);
        Serial.print(F("  -> "));
        Serial.println(name);
      }
    }

    if (hitsThisMiso > 0) {
      foundMiso = miso;
      foundCount = hitsThisMiso;
      break;   // first MISO that answers is the right one
    }
  }

  Serial.println();
  if (foundCount > 0) {
    Serial.println(F("--- WIRING CONFIRMED ---"));
    Serial.print(F("  #define PIN_SCLK ")); Serial.println(PIN_SCLK);
    Serial.print(F("  #define PIN_MOSI ")); Serial.println(PIN_MOSI);
    Serial.print(F("  #define PIN_MISO ")); Serial.println(foundMiso);
    Serial.print(F("  sensors responding: ")); Serial.println(foundCount);
    Serial.println();
    Serial.println(F("Flash geoshake_diy next — it will run the detector on these sensors."));
  } else {
    Serial.println(F("--- NO SENSOR ANSWERED ---"));
    Serial.println(F("Check, in this order:"));
    Serial.println(F("  1. 3V3 and GND actually connected (a floating ground is silent)"));
    Serial.println(F("  2. SCLK and MOSI are really GPIO12 / GPIO11 — swap them and retry"));
    Serial.println(F("  3. MISO is on a pin listed in MISO_CANDIDATES (add yours if not)"));
    Serial.println(F("  4. CS pins are GPIO9 / GPIO10 as configured above"));
  }
}

void loop() {
  delay(5000);
}
