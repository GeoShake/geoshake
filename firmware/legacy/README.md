# Legacy DIY firmware (unmaintained)

These are the original DIY GeoShake firmware variants, based on
**2× LSM6DS3** (SPI) or **1× LSM6DS3** (I2C) with ESP32 / ESP8266 and
the Arduino framework. They predate the GeoShake T1 (Rev-C) product
and are kept for the DIY community and for reference.

| Variant | MCU | Sensors | Bus |
|---|---|---|---|
| `ESP32_LSM6DS3_SPI_2XSENSOR` | ESP32 | 2× LSM6DS3 | SPI |
| `ESP8266_LSM6DS3_SPI_2XSENSOR` | ESP8266 | 2× LSM6DS3 | SPI |
| `ESP8266_LSM6DS3_I2C_XSENSOR` | ESP8266 | 1× LSM6DS3 | I2C |

**Status: unmaintained.** No new features, no support. The current,
maintained firmware for the shipping product is
[`../t1-revc/`](../t1-revc/) (ESP-IDF, ESP32-S3, 4× LSM6DSO).

License: GPL-3.0-or-later, same as the rest of `/firmware`
(see [../LICENSE](../LICENSE)).
