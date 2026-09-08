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

/* Gain acceptance range — SINGLE definition: used by both the CAL-T4 measurement (main.c: gain
   out of range = fault -> reject) and the load-side sanitize (cal.c: flash corruption -> 1.0).
   LSM6DSO unit-to-unit gain variance is <1%; if more than 2x correction is needed it is a
   hardware fault -> reject instead of silently clamping. */
#define CAL_GAIN_MIN 0.5f
#define CAL_GAIN_MAX 2.0f

/* ---- Per-sensor gain calibration: 4 scalars (relative gain) ----
   Scale correction so that the 4 co-mounted LSM6DSOs read the same acceleration identically.
   Offset is NOT calibrated (the pipeline already removes DC with high-pass + mean-subtract).
   STORE + APPLY (gain measurement was added in CAL-T4). On an uncalibrated device
   gain=1.0 → applying it is a byte-identical no-op.

   NVS namespace "geoshake_cal" is SEPARATE from identity's "geoshake" (deliberate): so that
   reset-id (nvs_erase_all "geoshake") does NOT ERASE the calibration. Single blob key "gain" = 4xfloat. */

/* Load the 4 gains from NVS "geoshake_cal" (READONLY). Missing key / open error /
   size mismatch → all four become 1.0f (NEVER leave them uninitialised). gain has 4 elements;
   if NULL the function returns silently. DOES NOT TOUCH the "geoshake" (identity) namespace. */
void gs_cal_load(float gain[4]);

/* Write the 4 gains to NVS "geoshake_cal" (READWRITE + commit). Returns esp_err_t. If gain is
   NULL, ESP_ERR_INVALID_ARG. DOES NOT TOUCH the "geoshake" (identity) namespace. Used by CAL-T4. */
esp_err_t gs_cal_save(const float gain[4]);
