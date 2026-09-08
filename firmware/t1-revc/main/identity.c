// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "identity.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

/* NVS namespace: provisioning (the admin panel) writes the same name. */
#define GS_NVS_NS "geoshake"

/* NVS keys: one source of truth for load <-> save consistency (so a typo cannot cause a
   silent bug -> a field never being loaded/written). */
#define GS_KEY_DEVICE_ID "device_id"
#define GS_KEY_WIFI_SSID "wifi_ssid"
#define GS_KEY_WIFI_PASS "wifi_pass"
#define GS_KEY_MQTT_USER "mqtt_user"
#define GS_KEY_MQTT_PASS "mqtt_pass"
#define GS_KEY_CLAIM_PROOF "claim_proof" /* GH-DEVPROV: self-provisioning claim token */
#define GS_KEY_CLAIM_CODE "claim_code"   /* BLEPROV: customer claim code (optional; see identity.h) */

/* Read a single string field. Missing key / does not fit the buffer / any other error -> leave
   out empty (out[0] = 0). A missing critical field is judged in gs_identity_complete; that is
   why a single field error does not blow up the load (e.g. the optional wifi_pass). */
static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    if (e != ESP_OK) {
        out[0] = '\0';
        if (e == ESP_ERR_NVS_INVALID_LENGTH)  /* did not fit: a provisioning error (value larger than the buffer) */
            ESP_LOGW("identity", "NVS '%s' value did not fit the buffer (cap=%u)", key, (unsigned)cap);
    }
}

esp_err_t gs_identity_load(gs_identity_t *out) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* If the namespace does not exist at all, nvs_open returns ESP_ERR_NVS_NOT_FOUND -> not-provisioned.
           Other errors (e.g. ESP_ERR_NVS_NOT_INITIALIZED) are passed up as they are. */
        return e;
    }

    load_str(h, GS_KEY_DEVICE_ID, out->device_id, sizeof(out->device_id));
    load_str(h, GS_KEY_WIFI_SSID, out->wifi_ssid, sizeof(out->wifi_ssid));
    load_str(h, GS_KEY_WIFI_PASS, out->wifi_pass, sizeof(out->wifi_pass));
    load_str(h, GS_KEY_MQTT_USER, out->mqtt_user, sizeof(out->mqtt_user));
    load_str(h, GS_KEY_MQTT_PASS, out->mqtt_pass, sizeof(out->mqtt_pass));
    load_str(h, GS_KEY_CLAIM_PROOF, out->claim_proof, sizeof(out->claim_proof));
    load_str(h, GS_KEY_CLAIM_CODE, out->claim_code, sizeof(out->claim_code));

    nvs_close(h);

    /* Missing/empty critical field -> do not count it as provisioned (the caller goes to the fallback). */
    if (!gs_identity_complete(out)) return ESP_ERR_NVS_NOT_FOUND;
    return ESP_OK;
}

bool gs_identity_mqtt_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->device_id[0] && id->mqtt_user[0] && id->mqtt_pass[0];
}

bool gs_identity_wifi_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->wifi_ssid[0] != '\0';   /* wifi_pass is optional (open network) -> not checked */
}

bool gs_identity_claim_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->device_id[0] && id->claim_proof[0];   /* enough for self-provisioning */
}

bool gs_identity_complete(const gs_identity_t *id) {
    /* Historical API: "complete" if both the MQTT and WiFi groups are ready. The expression below
       is identical to device_id && mqtt_user && mqtt_pass && wifi_ssid (same as the old body). */
    return gs_identity_mqtt_ready(id) && gs_identity_wifi_ready(id);
}

esp_err_t gs_identity_save(const gs_identity_t *id) {
    if (id == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* Write every field; on the first error leave without committing (a half write never reaches the disk). */
    if ((e = nvs_set_str(h, GS_KEY_DEVICE_ID, id->device_id)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_WIFI_SSID, id->wifi_ssid)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_WIFI_PASS, id->wifi_pass)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_MQTT_USER, id->mqtt_user)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_MQTT_PASS, id->mqtt_pass)) != ESP_OK) goto done;
    e = nvs_commit(h);
done:
    nvs_close(h);
    return e;
}

esp_err_t gs_identity_save_wifi(const char *ssid, const char *pass) {
    if (ssid == NULL || pass == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* ONLY the WiFi keys; DO NOT TOUCH the MQTT keys (the identity embedded by the admin is preserved).
       On the first error leave without committing (a half write never reaches the disk). Length validation is the caller's job. */
    if ((e = nvs_set_str(h, GS_KEY_WIFI_SSID, ssid)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_WIFI_PASS, pass)) != ESP_OK) goto done;
    e = nvs_commit(h);
done:
    nvs_close(h);
    return e;
}

esp_err_t gs_identity_save_mqtt(const char *user, const char *pass) {
    if (user == NULL || pass == NULL || user[0] == '\0' || pass[0] == '\0')
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* ONLY the MQTT keys; DO NOT TOUCH WiFi/claim/device_id. Self-provisioning
       (provision.c) writes the cred returned by the cloud → after a reboot the device is mqtt_ready.
       On the first error leave without committing (a half write never reaches the disk). */
    if ((e = nvs_set_str(h, GS_KEY_MQTT_USER, user)) != ESP_OK) goto done;
    if ((e = nvs_set_str(h, GS_KEY_MQTT_PASS, pass)) != ESP_OK) goto done;
    e = nvs_commit(h);
done:
    nvs_close(h);
    return e;
}

esp_err_t gs_identity_erase(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* already gone -> count it as success */
    if (e != ESP_OK) return e;
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t gs_identity_erase_wifi(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* no namespace -> no WiFi to erase */
    if (e != ESP_OK) return e;

    /* ONLY the WiFi keys; the MQTT keys are preserved. If a key is already gone, that is fine. */
    e = nvs_erase_key(h, GS_KEY_WIFI_SSID);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) {
        e = nvs_erase_key(h, GS_KEY_WIFI_PASS);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
