// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "bleprov.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "cJSON.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "config.h"

static const char *TAG = "bleprov";

/* The received creds are copied here in CRED_RECV; they are written to NVS ONLY in CRED_SUCCESS
   (the device really did connect). A wrong password never reaches the disk -> no risk of falling
   into a boot loop / in-place captive. wifi_sta_config_t fields may be at max length without a NUL
   -> sized copy + manual NUL (the identity.h buffer contract: ssid<=32, pass<=64 + NUL). */
static char s_ssid[33];
static char s_pass[65];
static bool s_saved = false;              /* NVS was written on SUCCESS -> restart on END */
static const gs_identity_t *s_id = NULL;  /* app_main's static g_id (lifetime is safe) */

/* ---- custom endpoint "gs-info": device identity (from inside the ENCRYPTED protocomm session) ----
   The request body is ignored; the response is JSON. If claim_code is not in NVS it returns "" — the
   app then asks for the code from the label by hand (older devices). protocomm frees outbuf itself
   (the IDF custom_prov example contract: the handler mallocs, the framework sends and releases). */
static esp_err_t gs_info_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                 uint8_t **outbuf, ssize_t *outlen, void *priv_data) {
    (void)session_id; (void)inbuf; (void)inlen; (void)priv_data;
    cJSON *j = cJSON_CreateObject();
    if (j == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "device_id", s_id ? s_id->device_id : "");
    cJSON_AddStringToObject(j, "model", GS_MODEL_NAME);
    cJSON_AddStringToObject(j, "fw", FW_VERSION);
    cJSON_AddStringToObject(j, "claim_code", s_id ? s_id->claim_code : "");
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (out == NULL) return ESP_ERR_NO_MEM;
    *outbuf = (uint8_t *)out;             /* cJSON's malloc — protocomm frees it */
    /* strlen (without the NUL): protocomm is length-delimited — if +1 (including the NUL) is sent,
       JSON.parse on the app side blows up because of the "\0" (F4 e2e finding; the app also tolerates it). */
    *outlen = strlen(out);
    return ESP_OK;
}

/* ---- wifi_prov_mgr lifecycle events ----
   RECV -> buffer the cred (NO write). SUCCESS -> write to NVS (the verified cred).
   FAIL -> reset the state machine (the app can retry in the same session; the auth/ap-not-found
   reason already reaches the app through protocomm). END -> restart only if something was saved (END
   arrives AFTER the app has READ the final "connected" state over BLE; an early restart would leave the app blind). */
static void prov_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "BLE provisioning started (adv: %s...)", GS_BLE_NAME_PREFIX);
        break;
    case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *c = (const wifi_sta_config_t *)data;
        size_t sl = strnlen((const char *)c->ssid, sizeof(c->ssid));
        size_t pl = strnlen((const char *)c->password, sizeof(c->password));
        if (sl >= sizeof(s_ssid)) sl = sizeof(s_ssid) - 1;
        if (pl >= sizeof(s_pass)) pl = sizeof(s_pass) - 1;
        memcpy(s_ssid, c->ssid, sl); s_ssid[sl] = '\0';
        memcpy(s_pass, c->password, pl); s_pass[pl] = '\0';
        ESP_LOGI(TAG, "credentials received: ssid=%s (password not logged) - trying to connect", s_ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        const wifi_prov_sta_fail_reason_t *r = (const wifi_prov_sta_fail_reason_t *)data;
        ESP_LOGW(TAG, "connect FAIL: %s - the app may retry (nothing written to NVS)",
                 (r && *r == WIFI_PROV_STA_AUTH_ERROR) ? "yanlis sifre (auth)" : "AP bulunamadi");
        /* Reset so the app can send a new cred (otherwise mgr stays stuck in the 'failed' state). */
        wifi_prov_mgr_reset_sm_state_on_failure();
        break;
    }
    case WIFI_PROV_CRED_SUCCESS: {
        /* The device REALLY connected to the network -> the cred is verified -> now write it persistently. */
        esp_err_t e = gs_identity_save_wifi(s_ssid, s_pass);
        if (e == ESP_OK) {
            s_saved = true;
            ESP_LOGW(TAG, "WiFi verified + saved (ssid=%s) - reboot after the app confirms", s_ssid);
        } else {
            /* Could not write: do NOT restart (it would return to the same captive/BLE branch, the user tries again). */
            ESP_LOGE(TAG, "gs_identity_save_wifi FAIL: %s", esp_err_to_name(e));
        }
        break;
    }
    case WIFI_PROV_END:
        if (s_saved) {
            ESP_LOGW(TAG, "provisioning done - rebooting for a normal boot");
            vTaskDelay(pdMS_TO_TICKS(500));   /* the final BLE notification + log flush */
            esp_restart();
        }
        ESP_LOGI(TAG, "provisioning session ended (nothing saved) - portal/BLE still waiting");
        break;
    default:
        break;
    }
}

esp_err_t gs_bleprov_start(const gs_identity_t *id) {
    if (id == NULL) return ESP_ERR_INVALID_ARG;
    s_id = id;

    /* STA netif: needed for prov mgr scanning + the connection attempt. In the captive branches
       wifi_start() never runs -> there is no STA netif; create one. (In the in-place fallback
       wifi_start had already created it -> a second creation crashes; guarded with esp_netif_get_handle.) */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "could not create STA netif (OOM?) - SKIPPING BLE prov (captive still works)");
            return ESP_FAIL;
        }
    }
    /* APSTA: the captive portal KEEPS LIVING on the AP, the STA is left to prov mgr. (If mgr changes
       the mode at the moment the cred is applied, the AP drops — setup is finishing at that point anyway, accepted.) */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "APSTA switch FAIL: %s - SKIPPING BLE prov", esp_err_to_name(e));
        return e;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event, NULL));

    /* scheme_event_handler NONE: on success the device restarts anyway — there is no need for the
       BT memory reclaim macros (FREE_BTDM etc.), this is the least surprising path. */
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    e = wifi_prov_mgr_init(cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init FAIL: %s - no BLE prov (captive still works)", esp_err_to_name(e));
        return e;
    }

    /* BLE adv name: the model prefix + the device_id tail -> the app both recognises the MODEL
       (GST1- = GeoShake T1) and tells devices apart. "GEO-XXXXX" -> "GST1-XXXXX" (10 chars, fits in adv).
       Buffer of 40: worst case prefix(5)+device_id(31)+NUL=37 (keeps -Werror=format-truncation quiet);
       real ids are 9 chars -> the name stays 10 chars. */
    char service_name[40];
    const char *suffix = strncmp(id->device_id, "GEO-", 4) == 0 ? id->device_id + 4 : id->device_id;
    snprintf(service_name, sizeof(service_name), "%s%s", GS_BLE_NAME_PREFIX, suffix);

    /* The custom endpoint ORDER matters (IDF contract): create BEFORE START, register AFTER. */
    wifi_prov_mgr_endpoint_create("gs-info");

    /* Security1: X25519 + PoP + AES-CTR. In v1 the PoP is a product-wide constant (zero-friction decision);
       the window is narrow — the device only advertises while it is NOT SET UP. QR-PoP is a possible future move. */
    e = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, (const void *)GS_BLE_POP,
                                         service_name, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "start_provisioning FAIL: %s - no BLE prov (captive still works)", esp_err_to_name(e));
        wifi_prov_mgr_deinit();
        return e;
    }
    wifi_prov_mgr_endpoint_register("gs-info", gs_info_handler, NULL);

    ESP_LOGI(TAG, "BLE setup ready: adv=%s security1 (captive portal also up)", service_name);
    return ESP_OK;
}
