// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

#include "esp_err.h"

/* Gain kabul araligi — TEK tanim: hem CAL-T4 olcum (main.c: aralik disi gain = ariza -> reddet)
   hem load-side sanitize (cal.c: flash bozulmasi -> 1.0). LSM6DSO birim-birim gain varyansi <%1;
   >2x duzeltme gerekiyorsa donanim arizasi -> sessiz clamp yerine reddet. */
#define CAL_GAIN_MIN 0.5f
#define CAL_GAIN_MAX 2.0f

/* ---- Per-sensör gain kalibrasyonu: 4 skalar (relative gain) ----
   4 birlikte-monteli LSM6DSO'nun aynı ivmeyi aynı okuması için ölçek düzeltmesi.
   Offset kalibre EDİLMEZ (pipeline DC'yi high-pass + mean-subtract ile zaten atar).
   DEPOLA + UYGULA (gain ölçümü CAL-T4'te eklendi). Kalibre edilmemiş cihazda
   gain=1.0 → uygulama byte-özdeş no-op'tur.

   NVS namespace "geoshake_cal", identity'nin "geoshake"inden AYRI (kasıtlı): reset-id
   (nvs_erase_all "geoshake") kalibrasyonu SİLMESİN. Tek blob anahtarı "gain" = 4×float. */

/* NVS "geoshake_cal"den 4 gain'i yükle (READONLY). Anahtar yok / açılış hatası /
   boyut uyuşmazlığı → dördü de 1.0f (asla başlatılmamış bırakma). gain 4 elemanlı;
   NULL ise sessizce döner. "geoshake" (identity) namespace'ine DOKUNMAZ. */
void gs_cal_load(float gain[4]);

/* 4 gain'i NVS "geoshake_cal"e yaz (READWRITE + commit). esp_err_t döner. gain NULL
   ise ESP_ERR_INVALID_ARG. "geoshake" (identity) namespace'ine DOKUNMAZ. CAL-T4 kullanır. */
esp_err_t gs_cal_save(const float gain[4]);
