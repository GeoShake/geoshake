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

/* BLEPROV — WiFi setup over BLE (mobile app; IoT home-device UX).
   ESP-IDF wifi_prov_mgr + scheme_ble (NimBLE): an ENCRYPTED channel via X25519 key exchange +
   PoP (Security1); the app pulls the WiFi list the device can see, sends the cred, and the device
   VERIFIES THE CONNECTION before saving it (better UX than captive's blind-save+restart:
   a wrong password is not saved, an "auth error" goes back to the app, the user tries again).

   It runs SIDE BY SIDE with the captive portal (web setup is not broken): WiFi is switched to
   APSTA mode — the portal lives on the AP, while prov mgr uses the STA for scanning/connecting.
   Both paths write to the same NVS (gs_identity_save_wifi) -> reboot -> normal boot.

   The custom "gs-info" endpoint (from inside the encrypted session) returns the device identity:
   {device_id, model, fw, claim_code} — with claim_code the app links ownership AUTOMATICALLY
   (physical access = proof; the device advertises over BLE ONLY while it is NOT SET UP, so the window is narrow). */

/* Start BLE provisioning (NON-BLOCKING; it runs its own tasks).
   Precondition: esp_netif_init + the default event loop + esp_wifi_init + esp_wifi_start must have
   been done (captive softap_start/softap_bringup provides this). If there is no STA netif it creates
   one and switches the mode to APSTA. id lifetime: owned by the caller (app_main's g_id — static). */
esp_err_t gs_bleprov_start(const gs_identity_t *id);
