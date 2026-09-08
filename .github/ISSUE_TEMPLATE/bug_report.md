---
name: Bug report
about: Something in the firmware, the enclosure or the docs does not work as described
title: ''
labels: bug
---

<!--
Security problem? Do NOT open a public issue. See SECURITY.md and mail
info@geoshake.org instead.
-->

## Device

- **Model:** <!-- GeoShake T1 (Rev-C) / DIY build (diy/) / legacy Arduino variant -->
- **Firmware version:** <!-- e.g. blep-0.9.27 - shown in the GeoShake app, and defined as FW_VERSION in firmware/t1-revc/main/config.h -->
- **How it was flashed:** <!-- shipped as-is / OTA / USB-C reflash / self-built with ESP-IDF -->

## What you expected

<!-- One or two sentences. -->

## What actually happened

<!-- One or two sentences. If it is intermittent, give timestamps with a timezone. -->

## LED

<!--
The pattern matters more than the colour: is it steady, blinking slowly,
blinking fast, or off? Describe what it does over about ten seconds.
-->

## Serial log

<!--
T1 (ESP-IDF):  idf.py -p /dev/cu.usbmodemXXXX monitor
DIY sketches:  serial monitor at 115200 baud

Paste from the boot line onwards. The first lines matter: the boot
self-checks print `golden OK`, `golden FRAME OK` and `golden STA/LTA OK`.
A `FAIL` on any of those lines is itself the bug report.
-->

```
paste the log here
```

## Steps to reproduce

1.
2.
3.

## Anything else

<!-- Photos of the install, mounting surface, WiFi conditions, power supply. -->
