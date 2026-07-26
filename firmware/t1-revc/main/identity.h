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

/* ---- Cihaz kimligi: runtime NVS'ten yuklenir (build-time secrets.h yerine) ----
   Tek binary stratejisi: ayni firmware tum cihazlara flaslanir; cihaz-ozel kimlik
   NVS "geoshake" namespace'inde tutulur (admin panel / provisioning yazar).
   Tampon boylari NUL dahil: device_id 31, wifi_ssid 32, wifi_pass 64 (WPA2 max),
   mqtt_user 31, mqtt_pass 63 karakter. */
typedef struct {
    char device_id[32];
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_user[32];
    char mqtt_pass[64];
    /* GH-DEVPROV Katman 2: fabrikada gömülü self-provisioning claim-token.
       claim_proof = base64url(HMAC-SHA256(DEVICE_SECRET, "claim:"+device_id)) (43 char).
       MQTT kimliği YOKKEN cihaz bunu cloud'a ibraz eder → cloud mqtt_user/pass döner. */
    char claim_proof[64];
    /* BLEPROV: etikete basılan müşteri claim kodu (deriveClaimCode, 10 hane ID_ALPHABET).
       Fabrikada NVS'e DE yazılır; BLE kurulumda gs-info endpoint'i app'e döner → app
       sahipliği OTOMATIK bağlar (kod elle girilmez). OPSIYONEL: eski cihazlarda yok ("")
       → app etiketten elle ister. Hiçbir ready-check'e dahil değil. */
    char claim_code[16];
} gs_identity_t;

/* NVS "geoshake" namespace'inden kimligi yukle (READONLY).
   out sifirlanir, sonra her alan nvs_get_str ile okunur (eksik anahtar -> bos string).
   Namespace yoksa veya kritik alan (device_id/wifi_ssid/mqtt_user/mqtt_pass) eksik/bos
   ise ESP_ERR_NVS_NOT_FOUND doner (provisioned-degil sinyali). Sir loglamaz. */
esp_err_t gs_identity_load(gs_identity_t *out);

/* Kritik alanlar dolu mu? device_id && wifi_ssid && mqtt_user && mqtt_pass.
   wifi_pass bos olabilir (acik/parolasiz ag) -> kontrol disinda.
   NOT: gs_identity_mqtt_ready(id) && gs_identity_wifi_ready(id) ile matematiksel
   ozdes; geriye-uyumluluk icin korunur. */
bool gs_identity_complete(const gs_identity_t *id);

/* MQTT kimligi hazir mi? device_id && mqtt_user && mqtt_pass dolu.
   Admin esptool ile fabrikada yazar; WiFi'dan bagimsiz grup (N2a). */
bool gs_identity_mqtt_ready(const gs_identity_t *id);

/* WiFi kimligi hazir mi? wifi_ssid dolu (wifi_pass bos olabilir = acik ag).
   Musteri captive portal'dan girer; MQTT'den bagimsiz grup (N2a). */
bool gs_identity_wifi_ready(const gs_identity_t *id);

/* GH-DEVPROV: self-provisioning icin hazir mi? device_id && claim_proof dolu.
   Fabrikada gomulur; MQTT kimligi YOKKEN cihaz cloud'dan mqtt cred'i self-provision eder. */
bool gs_identity_claim_ready(const gs_identity_t *id);

/* SADECE MQTT anahtarlarini (mqtt_user, mqtt_pass) yaz + commit. WiFi/claim'e DOKUNMAZ.
   Self-provisioning (provision.c) cloud'dan donen cred'i buraya yazar → reboot sonrasi
   mqtt_ready olur. user/pass NULL/bos ise ESP_ERR_INVALID_ARG. */
esp_err_t gs_identity_save_mqtt(const char *user, const char *pass);

/* Kimligi NVS'e yaz (READWRITE + commit). Her alan nvs_set_str, sonra commit.
   N1-T3 (provisioning) kullanir; burada API olarak tanimli. */
esp_err_t gs_identity_save(const gs_identity_t *id);

/* SADECE WiFi anahtarlarini (wifi_ssid, wifi_pass) yaz + commit. MQTT anahtarlarina
   DOKUNMAZ (captive portal cagirir; admin'in gomdugu MQTT kimligi korunur).
   ssid/pass NULL ise ESP_ERR_INVALID_ARG. N2a-T3 kullanir. */
esp_err_t gs_identity_save_wifi(const char *ssid, const char *pass);

/* "geoshake" namespace'inin tum anahtarlarini sil + commit (factory-reset kimlik).
   Namespace zaten yoksa ESP_OK. N1-T4 (reset-id komutu) kullanir. */
esp_err_t gs_identity_erase(void);

/* SADECE WiFi anahtarlarini (wifi_ssid, wifi_pass) sil + commit. MQTT anahtarlari
   korunur. Namespace / anahtar zaten yoksa ESP_OK. N2a-T4 (reset-wifi) kullanir. */
esp_err_t gs_identity_erase_wifi(void);
