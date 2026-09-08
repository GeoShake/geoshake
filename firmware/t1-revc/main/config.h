// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once
// GeoShake SHARED constants (identical on every device — not secret, they are committed).
// Device-specific identity (DEVICE_ID/WiFi/MQTT user-pass) lives in NVS (identity.c) + captive portal.
#define MQTT_BROKER "mqtts://mqtt.geoshake.org:8883"
#define FW_VERSION  "blep-0.9.27"   // sw_rst SIRA DUZELTMESI: NVS okumasi nvs_flash_init sonrasina alindi (0.9.21'de hep 0 bildiriyordu)
                                   // + eksen bazli KOSAN TEPE -> geoshake/<id>/peak (pk_h/pk_z/pk_v)
                                   // + UYARLANABILIR dalga penceresi: >= 6 s, sarsinti dinene kadar, tavan 24 s
                                   //   (PSRAM'siz derlemede DAVRANIS AYNI - #if CONFIG_SPIRAM ile ayrilmis)
// 0.9.14 -> 0.9.15: SURUM BUMP'I ZORUNLUYDU. PSRAM'li derleme onceden kendini 0.9.14 diye
// tanitiyordu ve filoda PSRAM'li cihazi ayirt etmenin yolu yoktu — kademeli yayinin butun
// amaci tam olarak bunu bilmek. Ayrica pulse_firmware'in PK'si `version`; ayni ad uretim
// imajini EZERDI.
// ⚠️ PSRAM ACARKEN TLS IKI KEZ KIRILDI, ikincisi ancak GERCEK bir dalga yayininda gorundu
// (acilis logu tertemizdi): MBEDTLS_EXTERNAL_MEM_ALLOC denendi -> "esp-aes: Failed to
// allocate memory" (S3'un donanim AES'i PSRAM'den DMA YAPAMAZ). Cozum: mbedTLS DAHILI kalir,
// yer bizim dalga tamponumuzu PSRAM'e tasiyarak acilir (55.440 bayt bosalir).
// Gerekce ve olcumler: firmware/sdkconfig.defaults + main.c wave_buf_init.
// ⚠️ OTA OZ-TESTI BAGLANTIYI dogrular, DALGA YAYININI DEGIL — yukaridaki hata o testten
// gecerdi. Her OTA sonrasi dalga teslimi ELLE dogrulanmali.
                                   // + mid-run WiFi watchdog (0.9.14): running device reboots after 10 min WiFi loss (#282 takip)
// 0.9.7 -> 0.9.8: power saving was TURNED OFF. esp_wifi_set_ps was never being called, so
// the IDF default WIFI_PS_MIN_MODEM was in effect and the radio slept between DTIMs.
// Measured defect (BH2JN, 30 days): mqtt_rc 298 / wifi_rc 3 — the link is solid, the MQTT
// session drops about once per hour, and every drop loses qos0 seismic data.
// Also: HT20 (RX sensitivity ~2.6 dB better, our RSSIs are weak), CPU 240 MHz,
// TX 80 (=20 dBm ceiling; 84 is a NO-OP, the IDF mapping is {[80,84]->80}), BT memory
// is left alone on the normal path, dynamic buffers 48/48, TCP window 11520.
// Design: docs/plans/2026-08-11-t1-maksimum-performans-design.md
// 0.9.6 -> 0.9.7: we moved the LED PWM out of the audible band. This did NOT SOLVE the
// buzzing on GEO-FVYSA (the sound continued in a test with the LED off — the source is the
// board's power rail), but leaving 5 kHz inside the band was not right either. 11-bit is
// required: the 13-bit ceiling is 9.7 kHz.
// 0.9.5 -> 0.9.6: 4 OTAs were attempted on GEO-FVYSA, the download completed 100% twice but
// the app could not connect within 30 s and was ROLLED BACK every time. The startup time
// measured at that site was avg 28 s / slowest 158 s. Details in the main.c selftest_task comment.
// 0.9.4 -> 0.9.5: brownout / panic / a real power cut COULD NOT be told apart from the
// outside (GEO-FVYSA: 32 reboots in 30 days, 269 hours lost). Now it is measured.
// The tag was bumped the SAME DAY — this was forgotten in 0.9.3, two different firmwares
// carried the same name and the version no longer told you which one was where in the fleet.
// 0.9.3 -> 0.9.4: the tag was not bumped on the day orientation was added; two DIFFERENT
// firmwares carried the same "blep-0.9.3" name and the version no longer told you which one
// was where in the fleet. Same class of mistake as "the metadata is lying" in #90.

// ---- BLEPROV: mobile app BLE setup (bleprov.c) ----
// The model name is returned to the app by the gs-info endpoint; the adv prefix enables model
// detection (GST1- = GeoShake T1; future models GST2- etc. -> the app list widens automatically).
#define GS_MODEL_NAME      "GeoShake T1"
#define GS_BLE_NAME_PREFIX "GST1-"
// Security1 proof-of-possession — in v1 a product-wide constant (zero-friction decision).
// The window is narrow: the device advertises over BLE ONLY while it is not set up. The mobile app uses the same constant.
#define GS_BLE_POP         "geoshake-t1"
