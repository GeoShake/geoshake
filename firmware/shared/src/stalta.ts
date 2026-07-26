// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

/**
 * Cihaz-içi STA/LTA deprem tespiti — SAF adım fonksiyonu (örnek-başına).
 * Firmware C tarafı (sample_task) bu mantığı BİREBİR aynalar; golden vektör kilitler.
 *
 * Girdi: her örnek 12 sayı (4 sensör × 3 eksen, sensör-major, HAM/gravity DAHİL, m/s²).
 * 4 sensör eksen-başına ortalanır (Faz 0 dizi kazancı), 1-kutup high-pass ile
 * gravity/DC çıkarılır, CF = enerji (Σ hp²), özyineli STA/LTA oranı eşikle karşılaştırılır.
 *
 * Parametreler PLACEHOLDER — sonraki task'ta gerçek .gsd ile ayarlanır. Golden vektör
 * ARİTMETİĞİ kilitler, değerler değişse de C↔TS uyumu korunur.
 */
export const STALTA_PARAMS = {
  alphaHp: 0.0115,        // 1-kutup HP katsayısı (~0.2 Hz @ 109 Hz): 2π·fc/fs
  nSta: 55,               // STA pencere örnek (~0.5 s @ 109 Hz)
  nLta: 3270,             // LTA pencere örnek (~30 s @ 109 Hz)
  ltaMin: 1e-8,           // LTA tabanı (m/s²)² — Faz 0 gürültü altı; sıfıra bölme guard'ı
  trigger: 6,             // STA/LTA tetik oranı (sonra ayarlanır)
  detrigger: 2,           // STA/LTA de-trigger oranı (histerezis)
  warmupSamples: 3270,    // bu kadar örnek geçene dek tetik bastırılır (~30 s, LTA dolsun)
} as const;

export interface StaltaState {
  dc: [number, number, number];  // eksen-başına koşan DC kestirimi (HP)
  sta: number;
  lta: number;
  armed: boolean;                // tetiklemeye hazır (histerezis)
  triggered: boolean;            // şu an tetikte (LTA dondurulmuş)
  count: number;                 // görülen örnek (warm-up)
}

export interface StaltaResult {
  state: StaltaState;
  fired: boolean;                // bu örnek tetik eşiğini AŞTI (yükselen kenar)
  ratio: number;                 // STA/LTA
  pga: number;                   // |hp| büyüklüğü (tetik anı onset PGA, m/s²)
}

export function initStalta(): StaltaState {
  return { dc: [0, 0, 0], sta: 0, lta: STALTA_PARAMS.ltaMin, armed: true, triggered: false, count: 0 };
}

export function staltaStep(s: StaltaState, sample: number[]): StaltaResult {
  const P = STALTA_PARAMS;
  const ax = (sample[0] + sample[3] + sample[6] + sample[9]) / 4;
  const ay = (sample[1] + sample[4] + sample[7] + sample[10]) / 4;
  const az = (sample[2] + sample[5] + sample[8] + sample[11]) / 4;
  // İlk örnekte DC'yi örneğe eşitle (gravity başlangıç transientini önler)
  const dc: [number, number, number] = s.count === 0
    ? [ax, ay, az]
    : [s.dc[0] + P.alphaHp * (ax - s.dc[0]),
       s.dc[1] + P.alphaHp * (ay - s.dc[1]),
       s.dc[2] + P.alphaHp * (az - s.dc[2])];
  const hx = ax - dc[0], hy = ay - dc[1], hz = az - dc[2];
  const cf = hx * hx + hy * hy + hz * hz;
  const sta = s.sta + (cf - s.sta) / P.nSta;
  let lta = s.triggered ? s.lta : s.lta + (cf - s.lta) / P.nLta;  // LTA FREEZE tetikteyken
  if (lta < P.ltaMin) lta = P.ltaMin;
  const ratio = sta / lta;
  const count = s.count + 1;
  let armed = s.armed, triggered = s.triggered, fired = false;
  if (count >= P.warmupSamples && armed && ratio >= P.trigger) {
    fired = true; triggered = true; armed = false;
  }
  if (triggered && ratio <= P.detrigger) { triggered = false; armed = true; }
  return { state: { dc, sta, lta, armed, triggered, count }, fired, ratio, pga: Math.sqrt(cf) };
}
