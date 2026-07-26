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
// DİKKAT (GH-FDSN): "GS" ŞU AN TESCİLLİ DEĞİL — fdsn.org kaydına göre GS,
// "US Geological Survey Networks" (ASL/USGS, 1980–) ağına ait. Kendi kodumuz
// için FDSN'e başvuru yapılacak (bkz. docs/fdsn-network-code-basvuru.md).
// Kod geldiğinde YALNIZ buradaki sabit değişir; tüketiciler otomatik uyar.
//
// Kod değişimi KIRICI bir değişikliktir: mevcut SeedLink stream id'leri
// (GS_XXXXX_00_LNZ) ve yayınlanmış SAC/StationXML dosyaları eski kodu taşır.
// Değişim öncesi /developer sayfasında ve mevcut kullanıcılara duyuru gerekir.

/** Yayınlanan ağ kodu (StationXML `<Network code>`, SAC KNETWK, SEED net alanı). */
export const FDSN_NETWORK_CODE = "GS";

/** FDSN'de tescilli mi? false → dokümantasyonda "provisional" ibaresi gösterilir. */
export const FDSN_NETWORK_REGISTERED = false;
