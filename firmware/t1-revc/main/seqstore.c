// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "seqstore.h"

#include "nvs.h"
#include "esp_log.h"

/* SEPARATE from identity's "geoshake": reset-id erases that namespace wholesale and the
   counter must outlive it (same rationale as cal.c's "geoshake_cal"). */
#define GS_SEQ_NS  "geoshake_seq"
#define GS_SEQ_KEY "trig_hi"

static const char *SEQ_TAG = "seq";

/* Best-effort store. A failed write is NOT fatal: the worst case is that the next boot
   reuses this block, which is exactly the old behaviour — never worse than before. */
static void seq_store(uint32_t v) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_SEQ_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(SEQ_TAG, "nvs_open failed (%s) - seq is not persistent this time", esp_err_to_name(e));
        return;
    }
    e = nvs_set_u32(h, GS_SEQ_KEY, v);
    if (e == ESP_OK) e = nvs_commit(h);
    /* 0.9.23: yazma/commit hatasi da LOGLANIR — eskiden yalniz open loglaniyordu. */
    if (e != ESP_OK)
        ESP_LOGW(SEQ_TAG, "could not write seq (%s) - the next boot may reuse this block",
                 esp_err_to_name(e));
    nvs_close(h);
}

uint32_t gs_seq_begin(void) {
    nvs_handle_t h;
    uint32_t hi = 0;
    bool found = false;

    esp_err_t e = nvs_open(GS_SEQ_NS, NVS_READONLY, &h);
    if (e == ESP_OK) {
        e = nvs_get_u32(h, GS_SEQ_KEY, &hi);
        found = (e == ESP_OK);
        if (!found && e != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(SEQ_TAG, "could not read seq (%s) - will be treated as first run", esp_err_to_name(e));
        nvs_close(h);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        /* 0.9.23: NOT_FOUND = ad alani yok = bu firmware ile ilk calisma, BEKLENEN ve sessiz.
           Geri kalan her sey (NOT_INITIALIZED, INVALID_NAME, ...) gercek hatadir ve loglanir;
           sessiz kalsa "ilk calisma" ile "NVS okunamadi" ayirt edilemez. */
        ESP_LOGW(SEQ_TAG, "nvs_open failed (%s) - will be treated as first run", esp_err_to_name(e));
    }
    /* No namespace / no key -> first run with this firmware. */
    if (!found) hi = GS_SEQ_FIRST_RUN;

    /* Wrap guard: a corrupt read near UINT32_MAX would make hi + RESERVE overflow and hand
       out seqs BELOW what is already stored — the exact collision this module prevents. */
    if (hi > UINT32_MAX - 2u * GS_SEQ_RESERVE) hi = GS_SEQ_FIRST_RUN;

    seq_store(hi + GS_SEQ_RESERVE);
    ESP_LOGI(SEQ_TAG, "trigger seq resumes at %lu (reserved through %lu)",
             (unsigned long)hi, (unsigned long)(hi + GS_SEQ_RESERVE));
    return hi;
}

void gs_seq_checkpoint(uint32_t next) {
    if (next > UINT32_MAX - 2u * GS_SEQ_RESERVE) return;   /* leave the wrap to the next boot */
    seq_store(next + GS_SEQ_RESERVE);
    ESP_LOGI(SEQ_TAG, "trigger seq reserved through %lu", (unsigned long)(next + GS_SEQ_RESERVE));
}
