// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* ---- Device identity: loaded from NVS at runtime (instead of build-time secrets.h) ----
   Single-binary strategy: the same firmware is flashed to every device; the device-specific
   identity is kept in the NVS "geoshake" namespace (written by the admin panel / provisioning).
   Buffer sizes include the NUL: device_id 31, wifi_ssid 32, wifi_pass 64 (WPA2 max),
   mqtt_user 31, mqtt_pass 63 characters. */
typedef struct {
    char device_id[32];
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_user[32];
    char mqtt_pass[64];
    /* GH-DEVPROV Layer 2: self-provisioning claim token embedded at the factory.
       claim_proof = base64url(HMAC-SHA256(DEVICE_SECRET, "claim:"+device_id)) (43 chars).
       While the device has NO MQTT identity it presents this to the cloud → the cloud returns mqtt_user/pass. */
    char claim_proof[64];
    /* BLEPROV: the customer claim code printed on the label (deriveClaimCode, 10 chars from ID_ALPHABET).
       It is ALSO written to NVS at the factory; during BLE setup the gs-info endpoint returns it to the
       app → the app links ownership AUTOMATICALLY (the code is not typed in by hand). OPTIONAL: older
       devices do not have it ("") → the app asks for it from the label by hand. It is part of NO ready-check. */
    char claim_code[16];
} gs_identity_t;

/* Load the identity from the NVS "geoshake" namespace (READONLY).
   out is zeroed, then every field is read with nvs_get_str (missing key -> empty string).
   If the namespace does not exist, or a critical field (device_id/wifi_ssid/mqtt_user/mqtt_pass)
   is missing/empty, returns ESP_ERR_NVS_NOT_FOUND (the not-provisioned signal). Logs no secrets. */
esp_err_t gs_identity_load(gs_identity_t *out);

/* Are the critical fields filled in? device_id && wifi_ssid && mqtt_user && mqtt_pass.
   wifi_pass may be empty (open/passwordless network) -> not checked.
   NOTE: mathematically identical to gs_identity_mqtt_ready(id) && gs_identity_wifi_ready(id);
   kept for backwards compatibility. */
bool gs_identity_complete(const gs_identity_t *id);

/* Is the MQTT identity ready? device_id && mqtt_user && mqtt_pass are filled in.
   The admin writes it at the factory with esptool; a group independent of WiFi (N2a). */
bool gs_identity_mqtt_ready(const gs_identity_t *id);

/* Is the WiFi identity ready? wifi_ssid is filled in (wifi_pass may be empty = open network).
   The customer enters it via the captive portal; a group independent of MQTT (N2a). */
bool gs_identity_wifi_ready(const gs_identity_t *id);

/* GH-DEVPROV: is it ready for self-provisioning? device_id && claim_proof are filled in.
   Embedded at the factory; while there is NO MQTT identity the device self-provisions the mqtt cred from the cloud. */
bool gs_identity_claim_ready(const gs_identity_t *id);

/* Write ONLY the MQTT keys (mqtt_user, mqtt_pass) + commit. DOES NOT TOUCH WiFi/claim.
   Self-provisioning (provision.c) writes the cred returned by the cloud here → after a reboot
   the device becomes mqtt_ready. If user/pass are NULL/empty, ESP_ERR_INVALID_ARG. */
esp_err_t gs_identity_save_mqtt(const char *user, const char *pass);

/* Write the identity to NVS (READWRITE + commit). nvs_set_str for every field, then commit.
   Used by N1-T3 (provisioning); defined here as API. */
esp_err_t gs_identity_save(const gs_identity_t *id);

/* Write ONLY the WiFi keys (wifi_ssid, wifi_pass) + commit. DOES NOT TOUCH the MQTT keys
   (the captive portal calls it; the MQTT identity embedded by the admin is preserved).
   If ssid/pass are NULL, ESP_ERR_INVALID_ARG. Used by N2a-T3. */
esp_err_t gs_identity_save_wifi(const char *ssid, const char *pass);

/* Erase all keys of the "geoshake" namespace + commit (factory-reset the identity).
   If the namespace does not exist already, ESP_OK. Used by N1-T4 (the reset-id command). */
esp_err_t gs_identity_erase(void);

/* Erase ONLY the WiFi keys (wifi_ssid, wifi_pass) + commit. The MQTT keys are
   preserved. If the namespace / key does not exist already, ESP_OK. Used by N2a-T4 (reset-wifi). */
esp_err_t gs_identity_erase_wifi(void);
