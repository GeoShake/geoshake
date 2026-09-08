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
#include <math.h>       /* isfinite — load-side sanitize (guards against flash corruption) */
#include "nvs.h"
#include "esp_log.h"

/* NVS namespace: SEPARATE from identity's "geoshake". A DELIBERATELY separate namespace so that
   reset-id (nvs_erase_all "geoshake") does not erase the calibration. The identity ns is never touched from here. */
#define GS_CAL_NS "geoshake_cal"

/* Single blob key: one source of truth for load <-> save consistency (so a typo cannot silently break it). */
#define GS_CAL_KEY_GAIN "gain"
#define GS_CAL_GAIN_BYTES (4 * sizeof(float))   /* 4 x float = 16 bytes */

static const char *CAL_TAG = "cal";

void gs_cal_load(float gain[4]) {
    if (gain == NULL) return;

    /* Default FIRST (1.0f x 4); only a valid read is written OVER it. That way every error
       path (no namespace / no key / read error / size mismatch) → 1.0. */
    for (int i = 0; i < 4; i++) gain[i] = 1.0f;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_CAL_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* NOT_FOUND: ad alani hic yazilmamis -> kalibre edilmemis, 1.0 (BEKLENEN, sessiz).
           0.9.23: geri kalan her hata (NOT_INITIALIZED, INVALID_NAME, ...) LOGLANIR —
           sessiz return, "kalibrasyon yok" ile "NVS okunamadi"yi ayirt edilmez kiliyordu. */
        if (e != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(CAL_TAG, "nvs_open failed (%s) -> default 1.0", esp_err_to_name(e));
        return;
    }

    float buf[4];
    size_t len = GS_CAL_GAIN_BYTES;   /* 16 = buffer capacity */
    e = nvs_get_blob(h, GS_CAL_KEY_GAIN, buf, &len);
    nvs_close(h);

    if (e == ESP_OK && len == GS_CAL_GAIN_BYTES) {
        memcpy(gain, buf, GS_CAL_GAIN_BYTES);   /* valid 16-byte blob -> apply */
        /* Defend against flash corruption: an element that is NaN/inf or outside
           [CAL_GAIN_MIN,CAL_GAIN_MAX] -> reset that element to 1.0 (do not feed garbage/NaN to
           lroundf -> apply_cal). Per element: if one sensor is corrupt, the valid gains of the
           others are preserved. */
        for (int i = 0; i < 4; i++)
            if (!isfinite(gain[i]) || gain[i] < CAL_GAIN_MIN || gain[i] > CAL_GAIN_MAX)
                gain[i] = 1.0f;
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        /* Key exists but could not be read / wrong size -> the 1.0 default is kept, warn. */
        ESP_LOGW(CAL_TAG, "gain blob invalid (e=%d len=%u) -> default 1.0",
                 (int)e, (unsigned)len);
    }
}

esp_err_t gs_cal_save(const float gain[4]) {
    if (gain == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_CAL_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    /* Write a single blob (16 bytes), then commit. The gain parameter decays to a pointer -> DO NOT
       USE sizeof, write with the GS_CAL_GAIN_BYTES constant. The identity "geoshake" ns is not touched. */
    e = nvs_set_blob(h, GS_CAL_KEY_GAIN, gain, GS_CAL_GAIN_BYTES);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
