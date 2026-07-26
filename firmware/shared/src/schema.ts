// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { z } from "zod";
import { DEVICE_ID_RE } from "./ids.js";

// Cihaz id TEK-KAYNAK: ids.ts (gs- grandfather + GEO- yeni; firmware 31-char buffer sınırı).
const devId = z.string().regex(DEVICE_ID_RE);

// Bilinmeyen alanlar sessizce atılır (zod .strip varsayılanı) — cihaz filosunun
// ileri-uyumlu yükseltmesi için bilinçli karar; eski ingest yeni alanları yok sayar.
export const FeaturePacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  fs: z.number().positive().finite(),
  n: z.number().int().positive(),
  rms: z.array(z.number().nonnegative().finite()).length(3),
  pga: z.number().nonnegative().finite(),
});
export type FeaturePacket = z.infer<typeof FeaturePacket>;

// Grafik için hafif telemetry örneği (geçmiş telemetry satırı + canlı feat ortak şekli).
export const TelemetrySample = z.object({
  tMs: z.number().int().nonnegative().max(8.64e15),
  rms: z.array(z.number().nonnegative().finite()).length(3),
  pga: z.number().nonnegative().finite(),
});
export type TelemetrySample = z.infer<typeof TelemetrySample>;

export const TriggerPacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  sta_lta: z.number().nonnegative().finite(),
  pga: z.number().nonnegative().finite(),
});
export type TriggerPacket = z.infer<typeof TriggerPacket>;

export const HealthPacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  up_s: z.number().int().nonnegative(),
  heap: z.number().int().nonnegative(),
  rssi: z.number().int(),
  wifi_rc: z.number().int().nonnegative(),
  mqtt_rc: z.number().int().nonnegative(),
  fw: z.string().min(1),
  gain: z.array(z.number().finite()).length(4).optional(),   // per-sensör kalibrasyon kazancı; eski firmware göndermez (opsiyonel)
});
export type HealthPacket = z.infer<typeof HealthPacket>;

/** Association olay ilanı — assoc yayınlar (geoshake/events), web tüketir (API + WS). */
export const EventPacket = z.object({
  v: z.literal(1),
  event_id: z.number().int().positive(),
  originMs: z.number().int().nonnegative().max(8.64e15),
  lat: z.number().finite(),
  lon: z.number().finite(),
  nStations: z.number().int().positive(),
  intensityClass: z.enum(["hafif", "orta", "siddetli"]),
  maxPga: z.number().nonnegative().finite(),
  rmsResidual: z.number().nonnegative().finite(),
});
export type EventPacket = z.infer<typeof EventPacket>;

/** Harita istasyonu (konumlu device). */
export const StationInfo = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  lastHealthMs: z.number().int().nonnegative().max(8.64e15).optional(),   // yalnız gerçek cihaz; sim → yok
});
export type StationInfo = z.infer<typeof StationInfo>;

/** Event-triggered dalga formu: işlenmiş tek-iz (4-sensör ort büyüklük, m/s²) + zaman ekseni. */
export const Waveform = z.object({
  t0Ms: z.number().int().nonnegative().max(8.64e15),
  fs: z.number().positive().finite(),
  samples: z.array(z.number().finite()),
});
export type Waveform = z.infer<typeof Waveform>;

/** Detay sayfası istasyonu: tetik + konum + türetilmiş mesafe/gecikme. */
export const StationDetail = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  tMs: z.number().int().nonnegative().max(8.64e15),
  staLta: z.number().nonnegative().finite(),
  pga: z.number().nonnegative().finite(),
  distKm: z.number().nonnegative().finite(),
  delayMs: z.number().finite(),   // origin'e göre varış gecikmesi; NEGATİF olabilir (origin back-solve'lu) — .nonnegative() EKLEME
  waveform: Waveform.optional(),   // yalnız gerçek cihaz (event-triggered); sim/eksik → yok
});
export type StationDetail = z.infer<typeof StationDetail>;

/** Bir olayın tam detayı: olay + katkı veren istasyonlar. */
export const EventDetail = z.object({ event: EventPacket, stations: z.array(StationDetail) });
export type EventDetail = z.infer<typeof EventDetail>;

/** İstasyon detay sayfası (/istasyon/[dev]) yükü. StationDetail (Faz 3b olay-içi) ile karıştırma. */
export const StationProfile = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  /** Sahibin verdiği isim (device_owners.nickname) — HERKESE AÇIK gösterilir (kullanıcı
   *  kararı, 2026-07-15); rename UI'ları "isim herkese görünür" notu düşer. Sahipsiz → yok. */
  nickname: z.string().optional(),
  /** Cihazın ağa ilk kaydı (devices.first_seen) — "X days on air" rozeti için. */
  firstSeenMs: z.number().int().nonnegative().max(8.64e15).optional(),
  health: z.object({
    rssi: z.number().int(),
    uptimeS: z.number().int().nonnegative(),
    freeHeap: z.number().int().nonnegative(),
    fwVersion: z.string(),
    lastHealthMs: z.number().int().nonnegative().max(8.64e15),
  }).optional(),   // yalnız gerçek cihaz; sim → yok
  triggers: z.array(z.object({
    seq: z.number().int().nonnegative(),
    tMs: z.number().int().nonnegative().max(8.64e15),
    staLta: z.number().nonnegative().finite(),
    pga: z.number().nonnegative().finite(),
    eventId: z.number().int().positive().optional(),
    /** JAMASEIS-B1: dalga formu saklandı mı (SAC indirme linki) / 3 eksen var mı (X/Y/Z linkleri). */
    hasWave: z.boolean().optional(),
    hasAxes: z.boolean().optional(),
  })),
  events: z.array(z.object({
    eventId: z.number().int().positive(),
    originMs: z.number().int().nonnegative().max(8.64e15),
    intensityClass: z.enum(["hafif", "orta", "siddetli"]),
  })),
});
export type StationProfile = z.infer<typeof StationProfile>;

/**
 * Association motoru çalışma-zamanı ayarları — admin panel YAZAR, assoc motoru OKUR.
 * TEK KAYNAK: hem panel (form/POST doğrular) hem assoc (DB okumasını doğrular; geçersiz/eksikse
 * DEFAULT_ASSOC_CONFIG'e düşer) bu şemayı kullanır. Sınırlar GÜVENLİK-KRİTİK (deprem erken uyarı):
 * minDevices<3 → lokasyon under-determined (çöp konum); büyük radiusKm → ilgisiz tetikleri kümeler;
 * büyük tolMs → gürültü sızar. Sınırlar hatalı-yapılandırmayı önler.
 */
export const AssocConfig = z.object({
  radiusKm: z.number().min(1).max(500),          // küme/tespit yarıçapı (km)
  dtMs: z.number().int().min(1000).max(60000),   // küme zaman penceresi (ms)
  minDevices: z.number().int().min(3).max(50),   // min ayrık istasyon (asla <3: lokasyon under-determined)
  tolMs: z.number().int().min(100).max(10000),   // residual gate (ms)
});
export type AssocConfigT = z.infer<typeof AssocConfig>;

/** Fail-safe fallback = mevcut hardcoded assoc sabitleri (DB config eksik/geçersizse assoc bunu kullanır). */
export const DEFAULT_ASSOC_CONFIG: AssocConfigT = { radiusKm: 50, dtMs: 15000, minDevices: 3, tolMs: 2000 };
