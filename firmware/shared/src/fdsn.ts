// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// FDSN ağ kodu — TEK KAYNAK. web (StationXML, SAC, /fdsnws) ve seedlink-feeder
// (miniSEED stream id) AYNI değeri kullanmalı; ayrışırsa jAmaSeis/ObsPy tarafında
// stream seçimi sessizce boşa düşer.
//
// GEÇMİŞ (GH-FDSN): kod 2026-07-27'ye kadar "GS" idi. Bu YANLIŞTI — fdsn.org
// kayıt defterine göre GS, "US Geological Survey Networks" (ASL/USGS, 1980–,
// DOI 10.7914/SN/GS) ağına ait. "Tescilsiz geçici kod" değil, doğrudan İSİM
// ÇAKIŞMASI: verimizi GS altında ortak arşive alan biri gerçek USGS verisiyle
// karıştırır. Dokümantasyonda ASLA "GS tescilli değil" DENMEZ (yanlış beyan).
//
// 2026-07-27: kod "GW" olarak değiştirildi. fdsn.org/ws/networks/1/query ile
// doğrulandı — GW kayıt defterinde YOK (atanmamış). FDSN başvurusu ayrı iş;
// HENÜZ BAŞVURU YAPILMADI. Başvuru onaylanınca YALNIZ FDSN_NETWORK_REGISTERED
// true yapılır — dokümantasyonda "başvuru sürüyor" gibi bir ifade KULLANILMAZ.
//
// Kod değişimi KIRICI bir değişikliktir: 27 Tem 2026 öncesi indirilmiş SAC /
// StationXML dosyaları ve eski SeedLink stream id'leri (GS_XXXXX_00_LNZ) hâlâ
// eski kodu taşır. O dosyaları GS altında ortak arşive KATMAYIN — yeniden
// çekildiğinde GW ile gelirler.

/** Yayınlanan ağ kodu (StationXML `<Network code>`, SAC KNETWK, SEED net alanı). */
export const FDSN_NETWORK_CODE = "GW";

/** FDSN'de tescilli mi? false → dokümantasyonda "provisional" ibaresi gösterilir. */
export const FDSN_NETWORK_REGISTERED = false;
