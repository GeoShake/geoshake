# Security Policy

## Reporting a vulnerability

Please report security issues privately to **info@geoshake.org**
(or https://geoshake.org/contact if email is unavailable). Do **not**
open a public GitHub issue for a security problem.

We aim to acknowledge reports within 3 business days and to keep you
updated until the issue is resolved. We are happy to credit you in the
fix announcement unless you prefer to stay anonymous.

Please give us a reasonable window to ship a fix before public
disclosure. GeoShake devices update over the air, so a fix needs to reach
the installed fleet, not just the repository.

## In scope

- The firmware in this repository (`firmware/t1-revc`, `firmware/shared`)
- Device provisioning flows (BLE provisioning, captive portal,
  self-provisioning claim)
- Public GeoShake network endpoints (`api.geoshake.org`,
  `mqtt.geoshake.org`, `seedlink.geoshake.org`)

## Out of scope

- The GeoShake PCB design (proprietary, not published)
- Internal cloud architecture and infrastructure
- Denial-of-service testing against production endpoints — please do not
  run load or DoS tests against the live network; ask us and we will
  arrange a test environment
- Social engineering of GeoShake staff or customers

## Known limitations (disclosed deliberately)

We would rather document these than have them found and reported as
surprises:

- **Fixed BLE proof-of-possession.** `GS_BLE_POP` in
  `firmware/t1-revc/main/config.h` is a product-wide constant. The
  exposure window is narrow — a device only advertises over BLE while it
  is *unprovisioned* — but someone within Bluetooth range of a
  newly-unboxed device could complete provisioning before the owner does.
  A per-device proof-of-possession is planned for a future hardware
  revision.
- **Claim code is printed on the device label.** By design, the
  customer-facing claim code is low-value and expected to become visible
  (it is on the label). It is protected by per-IP and per-device
  throttling on the claim endpoint, not by secrecy. Please do not post
  photographs of device labels publicly.
- **Claim codes are deterministic** and therefore cannot currently be
  rotated when a device is resold. This is a known v1 limitation.

## Running your own firmware

GeoShake T1 devices are user-reflashable over USB-C; no secure-boot
eFuses are burned. You are free to build and flash your own firmware.
Note that access to the *official* network requires credentials issued by
us — a self-built firmware can always publish to your own MQTT broker.
