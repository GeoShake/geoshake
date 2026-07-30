// GeoShake DIY — LSM6DS3 / LSM6DSO driver over SPI (1..4 sensors on one bus)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same bus settings the shipping T1 firmware uses: SPI mode 3 at 2 MHz, one
// chip-select per sensor, 104 Hz / +-2 g.
//
// Note on bus choice: on these parts the CS pin also picks the protocol. Held
// high the sensor speaks I2C; pulled low it speaks SPI. A sensor wired for SPI
// is therefore invisible to an I2C scan. SPI is what you want once you have
// more than one sensor — every part gets its own select line instead of needing
// a distinct address, and the timing is deterministic enough to sample several
// sensors inside one sample period.

#ifndef GEOSHAKE_DIY_LSM6_SPI_H
#define GEOSHAKE_DIY_LSM6_SPI_H

#include <Arduino.h>
#include <SPI.h>

namespace geoshake {

static const uint8_t REG_WHO_AM_I  = 0x0F;
static const uint8_t REG_CTRL1_XL  = 0x10;
static const uint8_t REG_CTRL3_C   = 0x12;
static const uint8_t REG_STATUS    = 0x1E;
static const uint8_t REG_OUTX_L_XL = 0x28;

static const uint8_t WHOAMI_LSM6DS3    = 0x69;
static const uint8_t WHOAMI_LSM6DS3TRC = 0x6A;
static const uint8_t WHOAMI_LSM6DSO    = 0x6C;

// ODR 104 Hz (0b0100 in bits 7:4), FS +-2 g (0b00 in bits 3:2).
// Byte-identical to what the T1 writes. Nominal 104 Hz; the internal clock runs
// slightly fast, landing near 109 Hz — which is what the STA/LTA windows assume.
static const uint8_t CTRL1_XL_104HZ_2G = 0x40;
static const uint8_t CTRL3_C_BDU_IFINC = 0x44;  // block data update + auto-increment

// +-2 g sensitivity: 0.061 mg/LSB -> m/s^2 per LSB
static const double LSB_TO_MS2 = 0.061e-3 * 9.80665;

static const int MAX_SENSORS = 4;

class Lsm6Spi {
 public:
  // Returns the number of sensors that answered with a known WHO_AM_I.
  int begin(int sclk, int miso, int mosi, const int *csPins, int count) {
    count_ = (count > MAX_SENSORS) ? MAX_SENSORS : count;
    settings_ = SPISettings(2000000, MSBFIRST, SPI_MODE3);

    for (int i = 0; i < count_; i++) {
      cs_[i] = csPins[i];
      pinMode(cs_[i], OUTPUT);
      digitalWrite(cs_[i], HIGH);
    }
    SPI.begin(sclk, miso, mosi, -1);
    delay(10);

    found_ = 0;
    for (int i = 0; i < count_; i++) {
      const uint8_t id = readReg(i, REG_WHO_AM_I);
      whoami_[i] = id;
      present_[i] = (id == WHOAMI_LSM6DS3 || id == WHOAMI_LSM6DS3TRC || id == WHOAMI_LSM6DSO);
      if (present_[i]) {
        writeReg(i, REG_CTRL3_C, CTRL3_C_BDU_IFINC);
        writeReg(i, REG_CTRL1_XL, CTRL1_XL_104HZ_2G);
        found_++;
      }
    }
    delay(20);   // let the first conversions land
    return found_;
  }

  int count() const { return count_; }
  int found() const { return found_; }
  bool present(int i) const { return present_[i]; }
  uint8_t whoami(int i) const { return whoami_[i]; }

  const char *partName(int i) const {
    switch (whoami_[i]) {
      case WHOAMI_LSM6DS3:    return "LSM6DS3";
      case WHOAMI_LSM6DS3TRC: return "LSM6DS3TR-C";
      case WHOAMI_LSM6DSO:    return "LSM6DSO (the part the T1 ships with)";
      default:                return "not found";
    }
  }

  // New-sample flag (XLDA) of the first present sensor. All sensors share the
  // same ODR, so one is enough to pace the loop.
  bool sampleReady() {
    for (int i = 0; i < count_; i++) {
      if (present_[i]) return (readReg(i, REG_STATUS) & 0x01) != 0;
    }
    return false;
  }

  // Reads sensor `i` in m/s^2, gravity included (the DSP high-passes it).
  bool read(int i, double out[3]) {
    if (!present_[i]) return false;
    uint8_t b[6];
    readBytes(i, REG_OUTX_L_XL, b, 6);
    const int16_t rx = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    const int16_t ry = (int16_t)((uint16_t)b[3] << 8 | b[2]);
    const int16_t rz = (int16_t)((uint16_t)b[5] << 8 | b[4]);
    out[0] = (double)rx * LSB_TO_MS2;
    out[1] = (double)ry * LSB_TO_MS2;
    out[2] = (double)rz * LSB_TO_MS2;
    return true;
  }

 private:
  int cs_[MAX_SENSORS] = {-1, -1, -1, -1};
  bool present_[MAX_SENSORS] = {false, false, false, false};
  uint8_t whoami_[MAX_SENSORS] = {0, 0, 0, 0};
  int count_ = 0;
  int found_ = 0;
  SPISettings settings_ = SPISettings(2000000, MSBFIRST, SPI_MODE3);

  uint8_t readReg(int i, uint8_t reg) {
    SPI.beginTransaction(settings_);
    digitalWrite(cs_[i], LOW);
    SPI.transfer(0x80 | reg);          // MSB set = read
    const uint8_t v = SPI.transfer(0x00);
    digitalWrite(cs_[i], HIGH);
    SPI.endTransaction();
    return v;
  }

  void readBytes(int i, uint8_t reg, uint8_t *buf, int len) {
    SPI.beginTransaction(settings_);
    digitalWrite(cs_[i], LOW);
    SPI.transfer(0x80 | reg);
    for (int k = 0; k < len; k++) buf[k] = SPI.transfer(0x00);
    digitalWrite(cs_[i], HIGH);
    SPI.endTransaction();
  }

  void writeReg(int i, uint8_t reg, uint8_t val) {
    SPI.beginTransaction(settings_);
    digitalWrite(cs_[i], LOW);
    SPI.transfer(reg & 0x7F);          // MSB clear = write
    SPI.transfer(val);
    digitalWrite(cs_[i], HIGH);
    SPI.endTransaction();
  }
};

}  // namespace geoshake

#endif  // GEOSHAKE_DIY_LSM6_SPI_H
