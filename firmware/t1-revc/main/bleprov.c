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

/* Alinan cred'ler CRED_RECV'de buraya kopyalanir; YALNIZ CRED_SUCCESS'te (cihaz gercekten
   baglandi) NVS'e yazilir. Yanlis sifre diske hic inmez -> boot-loop/inplace-captive'e
   dusme riski yok. wifi_sta_config_t alanlari max-uzunlukta NUL'suz olabilir -> boyutlu
   kopya + elle NUL (identity.h tampon sozlesmesi: ssid<=32, pass<=64 + NUL). */
static char s_ssid[33];
static char s_pass[65];
static bool s_saved = false;              /* SUCCESS'te NVS yazildi -> END'de restart */
static const gs_identity_t *s_id = NULL;  /* app_main'in static g_id'si (yasam suresi guvenli) */

/* ---- custom endpoint "gs-info": cihaz kimligi (SIFRELI protocomm oturumu icinden) ----
   Istek govdesi yok sayilir; yanit JSON. claim_code NVS'te yoksa "" doner — app o zaman
   etiketten elle kod ister (eski cihazlar). protocomm outbuf'u kendisi free eder (IDF
   custom_prov ornegi sozlesmesi: handler malloc'lar, framework gonderip birakir). */
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
    *outbuf = (uint8_t *)out;             /* cJSON malloc'u — protocomm free eder */
    /* strlen (NUL'suz): protocomm length-delimited — +1 (NUL dahil) gonderilirse app
       tarafinda JSON.parse "\0" yuzunden patlar (F4 e2e bulgusu; app ayrica tolere eder). */
    *outlen = strlen(out);
    return ESP_OK;
}

/* ---- wifi_prov_mgr yasam dongusu olaylari ----
   RECV -> cred'i tamponla (yazma YOK). SUCCESS -> NVS'e yaz (dogrulanmis cred).
   FAIL -> state-machine resetle (app ayni oturumda tekrar deneyebilir; auth/ap-not-found
   nedeni app'e protocomm'dan zaten gider). END -> yalniz kayit olduysa restart (app son
   "connected" durumunu BLE'den OKUDUKTAN sonra END gelir; erken restart app'i kor birakirdi). */
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
        /* Resetle ki app yeni cred gonderebilsin (yoksa mgr 'failed' durumunda takili kalir). */
        wifi_prov_mgr_reset_sm_state_on_failure();
        break;
    }
    case WIFI_PROV_CRED_SUCCESS: {
        /* Cihaz aga GERCEKTEN baglandi -> cred dogrulandi -> simdi kalici yaz. */
        esp_err_t e = gs_identity_save_wifi(s_ssid, s_pass);
        if (e == ESP_OK) {
            s_saved = true;
            ESP_LOGW(TAG, "WiFi verified + saved (ssid=%s) - reboot after the app confirms", s_ssid);
        } else {
            /* Yazilamadi: restart ETME (yeniden ayni captive/BLE dalina doner, kullanici tekrar dener). */
            ESP_LOGE(TAG, "gs_identity_save_wifi FAIL: %s", esp_err_to_name(e));
        }
        break;
    }
    case WIFI_PROV_END:
        if (s_saved) {
            ESP_LOGW(TAG, "provisioning done - rebooting for a normal boot");
            vTaskDelay(pdMS_TO_TICKS(500));   /* son BLE bildirimi + log flush */
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

    /* STA netif: prov mgr tarama + baglanti denemesi icin gerekli. Captive dallarinda
       wifi_start() hic kosmaz -> STA netif yok; olustur. (In-place fallback'te wifi_start
       olusturmustu -> ikinci olusum crash olur; esp_netif_get_handle ile guard.) */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "could not create STA netif (OOM?) - SKIPPING BLE prov (captive still works)");
            return ESP_FAIL;
        }
    }
    /* APSTA: AP'de captive portal YASAMAYA DEVAM eder, STA prov mgr'a kalir. (Cred-apply
       aninda mgr modu degistirirse AP duser — o an zaten kurulum bitiyor, kabul.) */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "APSTA switch FAIL: %s - SKIPPING BLE prov", esp_err_to_name(e));
        return e;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event, NULL));

    /* scheme_event_handler NONE: basarida cihaz zaten restart ediyor — BT bellegini
       geri kazanma makrolarina (FREE_BTDM vb.) gerek yok, en az suprizli yol. */
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    e = wifi_prov_mgr_init(cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init FAIL: %s - no BLE prov (captive still works)", esp_err_to_name(e));
        return e;
    }

    /* BLE adv adi: model prefix'i + device_id kuyrugu -> app hem MODEL tanir (GST1- =
       GeoShake T1) hem cihazi ayirt eder. "GEO-XXXXX" -> "GST1-XXXXX" (10 char, adv'ye sigar).
       Tampon 40: worst-case prefix(5)+device_id(31)+NUL=37 (-Werror=format-truncation susar);
       gercek id'ler 9 char -> ad 10 char kalir. */
    char service_name[40];
    const char *suffix = strncmp(id->device_id, "GEO-", 4) == 0 ? id->device_id + 4 : id->device_id;
    snprintf(service_name, sizeof(service_name), "%s%s", GS_BLE_NAME_PREFIX, suffix);

    /* Custom endpoint SIRASI onemli (IDF sozlesmesi): create START'tan ONCE, register SONRA. */
    wifi_prov_mgr_endpoint_create("gs-info");

    /* Security1: X25519 + PoP + AES-CTR. PoP v1'de urun-geneli sabit (sifir surtunme karari);
       pencere dar — cihaz yalniz KURULMAMISKEN yayinda. Ileride QR-PoP'a gecilebilir. */
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
