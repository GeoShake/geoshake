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

/* NVS namespace: provisioning (admin panel) ayni adi yazar. */
#define GS_NVS_NS "geoshake"

/* NVS anahtarlari: load <-> save tutarliligi icin tek kaynak (typo'da sessiz bug
   olmasin -> alan hic yuklenmez/yazilmaz). */
#define GS_KEY_DEVICE_ID "device_id"
#define GS_KEY_WIFI_SSID "wifi_ssid"
#define GS_KEY_WIFI_PASS "wifi_pass"
#define GS_KEY_MQTT_USER "mqtt_user"
#define GS_KEY_MQTT_PASS "mqtt_pass"
#define GS_KEY_CLAIM_PROOF "claim_proof" /* GH-DEVPROV: self-provisioning claim-token */
#define GS_KEY_CLAIM_CODE "claim_code"   /* BLEPROV: musteri claim kodu (opsiyonel; bkz. identity.h) */

/* Tek string alan oku. Anahtar yok / tampona sigmaz / baska hata -> out bos birak
   (out[0] = 0). Kritik-alan eksikligi gs_identity_complete'te degerlendirilir; bu
   yuzden tekil alan hatasi load'u patlatmaz (orn. opsiyonel wifi_pass). */
static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    if (e != ESP_OK) {
        out[0] = '\0';
        if (e == ESP_ERR_NVS_INVALID_LENGTH)  /* sigmadi: provisioning hatasi (deger buffer'dan buyuk) */
            ESP_LOGW("identity", "NVS '%s' degeri buffer'a sigmadi (cap=%u)", key, (unsigned)cap);
    }
}

esp_err_t gs_identity_load(gs_identity_t *out) {
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* Namespace hic yoksa nvs_open ESP_ERR_NVS_NOT_FOUND verir -> provisioned-degil.
           Diger hatalar (orn. ESP_ERR_NVS_NOT_INITIALIZED) oldugu gibi yukari. */
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

    /* Eksik/bos kritik alan -> provisioned sayma (cagiran fallback'e gider). */
    if (!gs_identity_complete(out)) return ESP_ERR_NVS_NOT_FOUND;
    return ESP_OK;
}

bool gs_identity_mqtt_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->device_id[0] && id->mqtt_user[0] && id->mqtt_pass[0];
}

bool gs_identity_wifi_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->wifi_ssid[0] != '\0';   /* wifi_pass opsiyonel (acik ag) -> kontrol disinda */
}

bool gs_identity_claim_ready(const gs_identity_t *id) {
    if (id == NULL) return false;
    return id->device_id[0] && id->claim_proof[0];   /* self-provisioning icin yeter */
}

bool gs_identity_complete(const gs_identity_t *id) {
    /* Tarihsel API: MQTT + WiFi gruplarinin ikisi de hazirsa "tam". Asagidaki ifade
       device_id && mqtt_user && mqtt_pass && wifi_ssid ile ozdes (eski govdeyle ayni). */
    return gs_identity_mqtt_ready(id) && gs_identity_wifi_ready(id);
}

esp_err_t gs_identity_save(const gs_identity_t *id) {
    if (id == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* Her alan yaz; ilk hatada commit'siz cik (yarim yazma diske inmez). */
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

    /* SADECE WiFi anahtarlari; MQTT anahtarlarina DOKUNMA (admin'in gomdugu kimlik korunur).
       Ilk hatada commit'siz cik (yarim yazma diske inmez). Uzunluk dogrulamasi cagiranda. */
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

    /* SADECE MQTT anahtarlari; WiFi/claim/device_id'ye DOKUNMA. Self-provisioning
       (provision.c) cloud'dan donen cred'i yazar → reboot sonrasi mqtt_ready olur.
       Ilk hatada commit'siz cik (yarim yazma diske inmez). */
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
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* zaten yok -> basarili say */
    if (e != ESP_OK) return e;
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t gs_identity_erase_wifi(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* namespace yok -> silinecek WiFi yok */
    if (e != ESP_OK) return e;

    /* SADECE WiFi anahtarlari; MQTT anahtarlari korunur. Anahtar zaten yoksa sorun degil. */
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
