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

/* GH-DEVPROV Katman 2 — cihaz self-provisioning HTTPS istemcisi.
   TLS: esp_crt_bundle (sdkconfig CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, full Mozilla
   CA) → Let's Encrypt cert'i (api.geoshake.org) dogrulanir; ayri cert gomulmez.
   Sir hijyeni: claim_proof / mqttPass ASLA loglanmaz (yalniz device_id + HTTP durum). */

static const char *TAG = "provision";

#define PROV_URL "https://api.geoshake.org/api/device/provision"
#define PROV_TIMEOUT_MS 15000
#define RESP_MAX 768   /* {mqttUser, mqttPass, broker, port} — 512+ yeter, pay birak */

esp_err_t gs_provision_run(const gs_identity_t *id) {
    if (id == NULL || !gs_identity_claim_ready(id)) return ESP_ERR_INVALID_ARG;

    /* İstek gövdesi: {device_id, claim_proof}. cJSON → string; secret gövdede ama LOG'a girmez. */
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
        .crt_bundle_attach = esp_crt_bundle_attach,   /* full CA bundle → LE cert dogrular */
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

    /* open(write_len) → write(body) → fetch_headers → read(response). Deterministik akis. */
    esp_err_t e = esp_http_client_open(client, (int)strlen(body_str));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "baglanti acilamadi: %s", esp_err_to_name(e));
        goto cleanup;
    }
    int w = esp_http_client_write(client, body_str, (int)strlen(body_str));
    if (w < 0) {
        ESP_LOGW(TAG, "govde yazilamadi");
        goto cleanup;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGW(TAG, "header alinamadi");
        goto cleanup;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* 401 = claim gecersiz (secret/device_id uyusmuyor). Sir loglamadan durum bas. */
        ESP_LOGW(TAG, "provision HTTP %d (401=claim gecersiz; diger=altyapi)", status);
        goto cleanup;
    }

    /* Yaniti oku (kucuk JSON). Tampon dolarsa fazlasi atilir → parse yine de dener. */
    int r;
    while (total < RESP_MAX - 1 &&
           (r = esp_http_client_read(client, resp + total, RESP_MAX - 1 - total)) > 0) {
        total += r;
    }
    resp[total] = '\0';

    cJSON *j = cJSON_ParseWithLength(resp, (size_t)total);
    if (j == NULL) {
        ESP_LOGW(TAG, "yanit JSON degil");
        goto cleanup;
    }
    /* Yalnız mqttUser/mqttPass alınır. Yanıttaki broker/port BİLİNÇLİ yok sayılır — cihaz
       derleme-zamanı MQTT_BROKER sabitini kullanır (main.c). Sunucu ileride cihaz-başı broker
       yönlendirmek isterse burada broker/port'u da okuyup NVS'e yazmak gerekir (şu an tek broker). */
    const cJSON *u = cJSON_GetObjectItem(j, "mqttUser");
    const cJSON *p = cJSON_GetObjectItem(j, "mqttPass");
    if (cJSON_IsString(u) && cJSON_IsString(p) && u->valuestring[0] && p->valuestring[0]) {
        /* NVS'e YALNIZ mqtt anahtarlarini yaz (device_id/claim/wifi korunur). */
        esp_err_t se = gs_identity_save_mqtt(u->valuestring, p->valuestring);
        if (se == ESP_OK) {
            ESP_LOGI(TAG, "self-provisioning OK: mqtt kimligi NVS'e yazildi (%s)", id->device_id);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "mqtt NVS yazilamadi: %s", esp_err_to_name(se));
        }
    } else {
        ESP_LOGW(TAG, "yanitta mqttUser/mqttPass yok/bos");
    }
    cJSON_Delete(j);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body_str);
    return ret;
}
