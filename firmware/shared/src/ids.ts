// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// Cihaz kimliği (device_id) TEK-KAYNAK regex/pattern. Fleet-geneli tutarlılık: format değişince
// YALNIZ burası değişir (ingest topic parse, assoc, shared payload şeması, web/admin validasyon,
// derive fonksiyonları hepsi buna bağlanır → drift + sessiz telemetri düşmesi olmaz).
//
// İki format desteklenir:
//   • gs-<4-28 küçük-harf/rakam>  — ESKİ cihazlar (grandfather; gs-0001..gs-0004). Toplam max 31 char.
//   • GEO-<5 BÜYÜK-harf/rakam>    — YENİ cihazlar (admin otomatik üretir; generateDeviceId).
//
// Kısıtlar:
//   • Firmware buffer'ları device_id[32] + mqtt_user[32] (=device_id) → 31 karakter sınırı. gs- (max 31)
//     ve GEO-XXXXX (9) ikisi de sığar.
//   • device_id ":" İÇEREMEZ → deriveMqttPassword/deriveClaimProof "mqtt:"/"claim:" uzayları ayrık kalır.
//   • GEO- BÜYÜK harf (üretim büyük harf); gs- küçük harf (eski). Karışmasın diye case-strict.

/** Anchor'SUZ iç pattern — topic regex'lerine gömmek için (ingest/assoc `geoshake/<id>/...`). Non-capturing. */
export const DEVICE_ID_PATTERN = "(?:gs-[0-9a-z]{4,28}|GEO-[A-Z0-9]{5})";

/** Anchor'LU tam-eşleşme regex — validasyon için (shared payload şeması, web/admin route, derive guard). */
export const DEVICE_ID_RE = new RegExp(`^${DEVICE_ID_PATTERN}$`);

/**
 * AMBİGÜ-SUZ alfabe (BÜYÜK harf + rakam, I/L/O/0/1 YOK — etikette/elle yazımda karışmaz). 31 karakter.
 * TEK-KAYNAK: hem admin `generateDeviceId` (GEO-XXXXX) hem shared `deriveClaimCode` (müşteri claim kodu)
 * buradan alır → drift olmaz. Düz string (crypto YOK) → barrel/tarayıcı-güvenli.
 */
export const ID_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
