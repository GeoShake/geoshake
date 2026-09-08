// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#pragma once

#include <stdint.h>

/* ---- Trigger sequence that SURVIVES a reboot -------------------------------
   WHY IT EXISTS: waveforms is keyed by PRIMARY KEY (device_id, trig_seq). The counter
   used to be a plain local in sample_task, so it restarted at 0 on EVERY boot and the
   new waveforms silently OVERWROTE the old rows with the same seq. Observed on
   2026-08-20: after one reboot GEO-KK9YF's 14 fresh waves replaced 14 waves recorded on
   13 August — that data is gone. Power cuts are routine (BH2JN restarted 32 times in
   30 days), so this was steady, invisible data loss.

   HOW: each boot reserves a block of GS_SEQ_RESERVE numbers up front and writes the END
   of that block to NVS. A power cut therefore costs at most one block, never a repeat:
   the next boot starts beyond everything this boot could have used. One flash write per
   boot, plus one per GS_SEQ_RESERVE triggers (KK9YF does ~350/week → negligible).

   NVS namespace is SEPARATE from identity's "geoshake" (the cal.c reasoning): reset-id
   does nvs_erase_all on that namespace and must not reset the counter. */

#define GS_SEQ_RESERVE   1000u
/* First run on a device that has no stored counter: start ABOVE every seq the fleet has
   ever written (measured max 2774 on 2026-08-20) so the migration cannot collide. */
#define GS_SEQ_FIRST_RUN 1000000u

/** Boot: returns the seq to continue from and reserves the next block. */
uint32_t gs_seq_begin(void);

/** The reservation ran out — reserve the next block. `next` is the seq about to be used. */
void gs_seq_checkpoint(uint32_t next);
