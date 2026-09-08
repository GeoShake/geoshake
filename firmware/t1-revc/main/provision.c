// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "provision.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

/* GH-DEVPROV Layer 2 — the device self-provisioning HTTPS client.
   TLS: esp_crt_bundle (sdkconfig CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, full Mozilla
   CA) → the Let's Encrypt cert (api.geoshake.org) is verified; no separate cert is embedded.
   Secret hygiene: claim_proof / mqttPass are NEVER logged (only device_id + the HTTP status). */

static const char *TAG = "provision";

#define PROV_URL "https://api.geoshake.org/api/device/provision"
#define PROV_TIMEOUT_MS 15000
#define RESP_MAX 768   /* {mqttUser, mqttPass, broker, port} — 512+ is enough, leave headroom */

esp_err_t gs_provision_run(const gs_identity_t *id) {
    if (id == NULL || !gs_identity_claim_ready(id)) return ESP_ERR_INVALID_ARG;

    /* Request body: {device_id, claim_proof}. cJSON → string; the secret is in the body but NEVER reaches the LOG. */
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) return ESP_FAIL;
    cJSON_AddStringToObject(body, "device_id", id->device_id);
    cJSON_AddStringToObject(body, "claim_proof", id->claim_proof);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) return ESP_FAIL;

    esp_http_client_config_t cfg = {
        .url = PROV_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PROV_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* full CA bundle → verifies the LE cert */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(body_str);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t ret = ESP_FAIL;
    char resp[RESP_MAX];
    int total = 0;

    /* open(write_len) → write(body) → fetch_headers → read(response). A deterministic flow. */
    esp_err_t e = esp_http_client_open(client, (int)strlen(body_str));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "could not open connection: %s", esp_err_to_name(e));
        goto cleanup;
    }
    int w = esp_http_client_write(client, body_str, (int)strlen(body_str));
    if (w < 0) {
        ESP_LOGW(TAG, "could not write body");
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGW(TAG, "could not fetch headers");
        goto cleanup;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* 401 = invalid claim (secret/device_id do not match). Print the status without logging the secret. */
        ESP_LOGW(TAG, "provision HTTP %d (401=invalid claim; other=infrastructure)", status);
        goto cleanup;
    }

    /* Read the response (small JSON). If the buffer fills up the excess is dropped → the parse is still attempted. */
    int r;
    while (total < RESP_MAX - 1 &&
           (r = esp_http_client_read(client, resp + total, RESP_MAX - 1 - total)) > 0) {
        total += r;
    }
    resp[total] = '\0';

    cJSON *j = cJSON_ParseWithLength(resp, (size_t)total);
    if (j == NULL) {
        ESP_LOGW(TAG, "response is not JSON");
        goto cleanup;
    }
    /* ONLY mqttUser/mqttPass are taken. The broker/port in the response are DELIBERATELY ignored — the
       device uses the compile-time MQTT_BROKER constant (main.c). If the server ever wants to route
       brokers per device, broker/port would have to be read here and written to NVS too (right now there is a single broker). */
    const cJSON *u = cJSON_GetObjectItem(j, "mqttUser");
    const cJSON *p = cJSON_GetObjectItem(j, "mqttPass");
    if (cJSON_IsString(u) && cJSON_IsString(p) && u->valuestring[0] && p->valuestring[0]) {
        /* Write ONLY the mqtt keys to NVS (device_id/claim/wifi are preserved). */
        esp_err_t se = gs_identity_save_mqtt(u->valuestring, p->valuestring);
        if (se == ESP_OK) {
            ESP_LOGI(TAG, "self-provisioning OK: mqtt identity written to NVS (%s)", id->device_id);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "could not write mqtt to NVS: %s", esp_err_to_name(se));
        }
    } else {
        ESP_LOGW(TAG, "response has no/empty mqttUser or mqttPass");
    }
    cJSON_Delete(j);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body_str);
    return ret;
}
