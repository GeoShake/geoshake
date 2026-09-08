// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

#include "esp_err.h"
#include "identity.h"

/* ---- GH-DEVPROV Layer 2: device SELF-provisioning (B1 claim token) ----
   While the device has NO MQTT identity but DOES HAVE device_id+claim_proof and WiFi is
   connected, it fetches the MQTT credential from the cloud BY ITSELF:

     POST https://api.geoshake.org/api/device/provision  {device_id, claim_proof}
       200 {mqttUser, mqttPass, broker, port} → write to NVS (gs_identity_save_mqtt).
       401 → invalid claim (secret does not match / wrong device_id).

   Precondition: WiFi is ALREADY connected (STA, GOT_IP). The caller (the app_main provision
   branch) first starts WiFi and waits for an IP, then calls this. On success → the caller does
   esp_restart (after the reboot mqtt_ready → normal boot). Secrets (claim_proof/mqttPass) are NOT LOGGED.

   Returns: ESP_OK (mqtt written to NVS) | ESP_FAIL (HTTP/parse/save error — the caller
   retries with backoff) | ESP_ERR_INVALID_ARG (not claim_ready). */
esp_err_t gs_provision_run(const gs_identity_t *id);
