// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "cal.h"
#include <string.h>
#include <math.h>       /* isfinite — load-side sanitize (flash bozulmasina karsi) */
#include "nvs.h"
#include "esp_log.h"

/* NVS namespace: identity'nin "geoshake"inden AYRI. reset-id (nvs_erase_all "geoshake")
   kalibrasyonu silmesin diye KASITLI ayrı namespace. Buradan identity ns'ine dokunulmaz. */
#define GS_CAL_NS "geoshake_cal"

/* Tek blob anahtarı: load <-> save tutarlılığı için tek kaynak (typo'da sessiz bug olmasın). */
#define GS_CAL_KEY_GAIN "gain"
#define GS_CAL_GAIN_BYTES (4 * sizeof(float))   /* 4 × float = 16 byte */

static const char *CAL_TAG = "cal";

void gs_cal_load(float gain[4]) {
    if (gain == NULL) return;

    /* ÖNCE default'la (1.0f × 4); yalnız geçerli okuma ÜZERİNE yaz. Böylece her hata
       yolu (namespace yok / anahtar yok / okuma hatası / boyut uyuşmazlığı) → 1.0. */
    for (int i = 0; i < 4; i++) gain[i] = 1.0f;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_CAL_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* Namespace hiç yoksa nvs_open ESP_ERR_NVS_NOT_FOUND verir -> kalibre-değil (default 1.0). */
        return;
    }

    float buf[4];
    size_t len = GS_CAL_GAIN_BYTES;   /* 16 = tampon kapasitesi */
    e = nvs_get_blob(h, GS_CAL_KEY_GAIN, buf, &len);
    nvs_close(h);

    if (e == ESP_OK && len == GS_CAL_GAIN_BYTES) {
        memcpy(gain, buf, GS_CAL_GAIN_BYTES);   /* geçerli 16-byte blob -> uygula */
        /* Flash bozulmasina karsi savun: NaN/inf ya da [CAL_GAIN_MIN,CAL_GAIN_MAX] disi
           eleman -> o elemani 1.0'a dondur (lroundf'e cop/NaN besleme -> apply_cal). Eleman
           bazli: bir sensör bozuksa digerlerinin gecerli gain'i korunur. */
        for (int i = 0; i < 4; i++)
            if (!isfinite(gain[i]) || gain[i] < CAL_GAIN_MIN || gain[i] > CAL_GAIN_MAX)
                gain[i] = 1.0f;
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        /* Anahtar var ama okunamadı / boyut yanlış -> default 1.0 korunur, uyar. */
        ESP_LOGW(CAL_TAG, "gain blob invalid (e=%d len=%u) -> default 1.0",
                 (int)e, (unsigned)len);
    }
}

esp_err_t gs_cal_save(const float gain[4]) {
    if (gain == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_CAL_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* Tek blob (16 byte) yaz, sonra commit. gain param'ı pointer'a çürür -> sizeof KULLANMA,
       sabit GS_CAL_GAIN_BYTES ile yaz. Identity "geoshake" ns'ine dokunulmaz. */
    e = nvs_set_blob(h, GS_CAL_KEY_GAIN, gain, GS_CAL_GAIN_BYTES);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
