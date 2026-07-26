// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

/* ---- Captive portal (WiFi kurulumu) — N2a-T3, BLEPROV'da noblock'a cevrildi ----
   MQTT kimligi var ama WiFi eksikse (mqtt_ready && !wifi_ready; boot karari main.c'de)
   app_main bunu cagirir. MQTT kimligi (device_id/mqtt_user/mqtt_pass) admin tarafindan
   esptool ile fabrikada gomulu; musteri YALNIZ WiFi girer.
   Acilan: SoftAP "GeoShake-XXXX" (acik ag) + HTTP form (http://192.168.4.1) + DNS hijack.
   Kullanici wifi_ssid / wifi_pass girer -> gs_identity_save_wifi (MQTT anahtarlarina
   dokunmaz, admin'in gomdugu kimlik korunur) -> esp_restart (artik tam kimlikle STA boot eder).

   NON-BLOCKING (BLEPROV): SoftAP + httpd + DNS kendi task'larinda calisir, fonksiyon DONER.
   Cagiran (app_main) ardindan gs_bleprov_start ile BLE kurulumu da acar ve kendisi
   for(;;) ile bekler. Basarili provisioning (POST /save veya BLE) esp_restart eder. */
void gs_captive_start(void);

/* ---- Yerinde captive (kurtarma) — N2d-T1, BLEPROV'da noblock'a cevrildi ----
   Provisioned cihaz (MQTT+WiFi NVS'te) WiFi'ye baglanamiyorsa (yanlis parola / zayif sinyal)
   60sn sonra wifi_guard_task (T3) bunu cagirir. Onkosul: wifi ZATEN init+start (STA modunda).
   Reboot ETMEZ (WiFi creds korunur): esp_wifi_stop (STA) -> AP+httpd. Cagiran
   s_captive_fallback bayragini ONCE set etmeli; aksi halde esp_wifi_stop'un urettigi
   STA_DISCONNECTED, main.c wifi_event'inde esp_wifi_connect() reconnect'ini tetikler.
   NON-BLOCKING: doner; cagiran (wifi_guard_task) BLE prov'u acip kendisi bekler. */
void gs_captive_start_inplace(void);
