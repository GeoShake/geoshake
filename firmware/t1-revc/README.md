# GeoShake T1 firmware (Rev-C — ESP32-S3, 4× LSM6DSO)

This is the firmware running on every **GeoShake T1** sold at
[geoshake.org](https://geoshake.org). ESP-IDF project, ESP32-S3, wired
to a custom 4× LSM6DSO accelerometer PCB.

License: **GPL-3.0-or-later** — see [../LICENSE](../LICENSE).

## What it does

- **Reads 4 IMUs over SPI.** Four LSM6DSO accelerometers
  (`U1/U84/U85/U86`) on a shared SPI2 bus, each with its own CS line.
  104 Hz ODR, ±2 g, gyro off. A DRDY interrupt from IMU1 drives the
  sample loop (~109 Hz effective).
- **Computes 1 Hz features.** Each ~1 s window: per-(sensor,axis) mean
  subtracted (gravity/DC removal), 4 sensors averaged, per-axis **RMS**
  plus peak-vector **PGA** published.
- **On-device STA/LTA detection.** A recurrence-based STA/LTA detector
  (high-pass DC tracker → short/long-term averages → ratio) runs on
  every sample. On a trigger it publishes a `trig` event plus the
  waveform around the event (~2.3 s pre + ~4 s post, from a ring
  buffer, as 42-byte `.gsd` frames).
- **Science mode.** A remote `sci` command streams raw int16 frames
  for research-grade captures.
- **Signed OTA with rollback.** Dual-app (A/B) partitions, RSA-3072
  image signing (Secure Boot V2 scheme, **without** burning
  secure-boot eFuses), signature verified on update, automatic
  rollback if the new image can't reach the network.
- **Golden self-checks at boot.** The feature math, STA/LTA and frame
  encoder are asserted against embedded reference vectors on every
  boot; a `FAIL` log line means the math drifted.

## Your device stays yours

The retail T1 is **user-reflashable over USB-C** — no secure-boot
eFuses are burned, in line with GPLv3 §6. You can build and flash your
own firmware at any time. Note: access to the *official* GeoShake
network requires credentials issued via
[geoshake.org/join-network](https://geoshake.org/join-network); a
self-built firmware can talk to **your own MQTT broker** by changing
`MQTT_BROKER` in [`main/config.h`](main/config.h).

## Provisioning (no hardcoded identity)

The binary is identity-less. Device-specific identity (device ID,
WiFi, MQTT credentials) is written to NVS on first setup:

- **BLE provisioning** (default): an unprovisioned device advertises
  as `GST1-…`; the GeoShake mobile app configures it.
- **Captive portal fallback**: SoftAP + web form (`captive.c`).

## Build & flash

Prerequisites: **ESP-IDF v5.3.x** (`. $IDF_PATH/export.sh`).

The build config enables signed-app verification, so you need an
RSA-3072 signing key (this repo does **not** contain ours):

```sh
cd firmware/t1-revc
# generate YOUR OWN signing key (never committed):
espsecure.py generate_signing_key --version 2 --scheme rsa3072 ota_signing_key.pem

idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Alternatively, disable the `CONFIG_SECURE_SIGNED_APPS_*` lines in
`sdkconfig.defaults` for unsigned local builds.

After changing `partitions.csv` or flash-layout config, run
`idf.py erase-flash` before flashing.

### Verify boot

```
golden OK ...
golden FRAME OK ...
golden STA/LTA OK ...
4x LSM6DSO init OK (104 Hz, +-2g)
```

## Hardware pinout (ESP32-S3)

| Signal | GPIO |
|--------|------|
| SCLK | 12 |
| MOSI | 11 |
| MISO | 13 |
| CS sensor 0..3 (U1/U84/U85/U86) | 6, 7, 15, 16 |
| DRDY (IMU1 INT1) | 17 |

The PCB design is proprietary and is not published. The pinout and sensor
configuration above are what the firmware expects — enough to build your
own carrier board. Enclosure designs:
[`../../hardware/enclosure/`](../../hardware/enclosure/).

## MQTT topics

All under `geoshake/<DEVICE_ID>/`:

| Topic | Payload | Rate | QoS |
|-------|---------|------|-----|
| `feat` | JSON `{v,dev,seq,t_ms,fs,n,rms[3],pga}` | ~1 Hz | 0 |
| `health` | JSON `{v,dev,seq,t_ms,up_s,heap,rssi,wifi_rc,mqtt_rc,fw}` | 30 s | 0 |
| `trig` | JSON `{v,dev,seq,t_ms,sta_lta,pga}` | on detection | 1 |
| `wave/<seq>` | binary `.gsd` frames (≤128/chunk) | on event | 1 |
| `sci` | binary `.gsd` frames (batch) | on command | 0 |

Subscribed: `cmd` (accepts `sci` and `ota`).

`.gsd` frame: 42-byte little-endian —
`magic(A5 5A) | ver | flags | seq(u32) | t_us(u64) | 12×i16 raw | crc16(CCITT-FALSE)`.

## Golden self-checks — bit-locked to `shared`

The feature math, STA/LTA detector and `.gsd` frame encoder in this C code
are hand-mirrors of the TypeScript reference in
[`../shared`](../shared/). Golden vectors are asserted at every boot:

```
golden OK ...
golden FRAME OK ...
golden STA/LTA OK ...
```

A `FAIL` means the C has drifted from the reference. If you change any
constant or computation, update **both** sides and refresh the golden
literals — see [`../../CONTRIBUTING.md`](../../CONTRIBUTING.md).

## Third-party code

`components/dns_server/` is from Espressif's ESP-IDF examples
(SPDX: `Unlicense OR CC0-1.0`) and keeps its original license header.
