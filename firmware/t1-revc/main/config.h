// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once
// GeoShake ORTAK sabitler (tüm cihazlarda aynı — sır değil, commit'lenir).
// Cihaz-özel kimlik (DEVICE_ID/WiFi/MQTT user-pass) NVS'te (identity.c) + captive portal.
#define MQTT_BROKER "mqtts://mqtt.geoshake.org:8883"
#define FW_VERSION  "blep-0.9.2"   // + kurulum modu LED yavas blink (kurulmamis/wifi-reset -> GPIO39 500ms)

// ---- BLEPROV: mobil app BLE kurulumu (bleprov.c) ----
// Model adi gs-info endpoint'inde app'e doner; adv prefix'i model tespiti saglar
// (GST1- = GeoShake T1; gelecek modeller GST2- vb. -> app listesi otomatik genisler).
#define GS_MODEL_NAME      "GeoShake T1"
#define GS_BLE_NAME_PREFIX "GST1-"
// Security1 proof-of-possession — v1 urun-geneli sabit (sifir-surtunme karari).
// Pencere dar: cihaz YALNIZ kurulmamisken BLE yayini yapar. Mobil app ayni sabiti kullanir.
#define GS_BLE_POP         "geoshake-t1"
