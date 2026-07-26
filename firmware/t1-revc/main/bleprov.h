// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

#include "identity.h"
#include "esp_err.h"

/* BLEPROV — BLE uzerinden WiFi kurulumu (mobil app; IoT ev cihazi UX'i).
   ESP-IDF wifi_prov_mgr + scheme_ble (NimBLE): X25519 anahtar degisimi + PoP (Security1)
   ile SIFRELI kanal; app cihazin gordugu WiFi listesini ceker, cred yollar, cihaz
   BAGLANTIYI DOGRULAYIP oyle kaydeder (captive'in kor-kaydet+restart'indan daha iyi UX:
   yanlis sifre kaydedilmez, app'e "auth error" doner, kullanici tekrar dener).

   Captive portal ile YAN YANA calisir (web kurulumu bozulmaz): WiFi APSTA moduna
   gecirilir — AP'de portal yasar, STA'yi prov mgr tarama/baglanti icin kullanir.
   Iki yol da ayni NVS'e yazar (gs_identity_save_wifi) -> reboot -> normal boot.

   Custom "gs-info" endpoint'i (sifreli oturum icinden) cihaz kimligini dondurur:
   {device_id, model, fw, claim_code} — app claim_code ile sahipligi OTOMATIK baglar
   (fiziksel erisim = kanit; cihaz yalniz KURULMAMISKEN BLE yayini yapar, pencere dar). */

/* BLE provisioning'i baslat (NON-BLOCKING; kendi task'lari calisir).
   On kosul: esp_netif_init + default event loop + esp_wifi_init + esp_wifi_start yapilmis
   olmali (captive softap_start/softap_bringup saglar). STA netif yoksa olusturur ve modu
   APSTA'ya cevirir. id yasam suresi: cagiran sahiplenir (app_main'in g_id'si — static). */
esp_err_t gs_bleprov_start(const gs_identity_t *id);
