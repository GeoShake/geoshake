// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

/* ---- Captive portal (WiFi setup) — N2a-T3, made non-blocking in BLEPROV ----
   If the MQTT identity exists but WiFi is missing (mqtt_ready && !wifi_ready; the boot decision is
   in main.c) app_main calls this. The MQTT identity (device_id/mqtt_user/mqtt_pass) is embedded by
   the admin at the factory with esptool; the customer enters ONLY WiFi.
   What comes up: SoftAP "GeoShake-XXXX" (open network) + an HTTP form (http://192.168.4.1) + DNS hijack.
   The user enters wifi_ssid / wifi_pass -> gs_identity_save_wifi (does not touch the MQTT keys,
   the identity embedded by the admin is preserved) -> esp_restart (it now boots STA with a complete identity).

   NON-BLOCKING (BLEPROV): SoftAP + httpd + DNS run in their own tasks, the function RETURNS.
   The caller (app_main) then also opens BLE setup with gs_bleprov_start and waits itself
   with for(;;). Successful provisioning (POST /save or BLE) does an esp_restart. */
void gs_captive_start(void);

/* ---- In-place captive (recovery) — N2d-T1, made non-blocking in BLEPROV ----
   If a provisioned device (MQTT+WiFi in NVS) cannot connect to WiFi (wrong password / weak signal)
   wifi_guard_task (T3) calls this after 60 s. Precondition: wifi is ALREADY init+start (in STA mode).
   It does NOT reboot (the WiFi creds are preserved): esp_wifi_stop (STA) -> AP+httpd. The caller
   must set the s_captive_fallback flag FIRST; otherwise the STA_DISCONNECTED produced by
   esp_wifi_stop triggers the esp_wifi_connect() reconnect in main.c's wifi_event.
   NON-BLOCKING: it returns; the caller (wifi_guard_task) opens BLE prov and waits itself. */
void gs_captive_start_inplace(void);
