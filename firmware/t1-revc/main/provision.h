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

/* ---- GH-DEVPROV Katman 2: cihaz SELF-provisioning (B1 claim-token) ----
   Cihaz MQTT kimligi YOKKEN ama device_id+claim_proof VARKEN ve WiFi bagliyken
   cloud'dan MQTT credential'ini KENDI alir:

     POST https://api.geoshake.org/api/device/provision  {device_id, claim_proof}
       200 {mqttUser, mqttPass, broker, port} → NVS'e (gs_identity_save_mqtt) yaz.
       401 → claim gecersiz (secret uyusmuyor / yanlis device_id).

   Onkosul: WiFi ZATEN bagli (STA, GOT_IP). Cagiran (app_main provision dali) once
   WiFi'yi baslatip IP bekler, sonra bunu cagirir. Basari → cagiran esp_restart eder
   (reboot sonrasi mqtt_ready → normal boot). Sir (claim_proof/mqttPass) LOGLANMAZ.

   Donus: ESP_OK (mqtt NVS yazildi) | ESP_FAIL (HTTP/parse/kaydetme hatasi — cagiran
   backoff ile yeniden dener) | ESP_ERR_INVALID_ARG (claim_ready degil). */
esp_err_t gs_provision_run(const gs_identity_t *id);
