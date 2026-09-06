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

/**
 * GÖLGE DEDEKTÖR PARAMETRELERİ (2026-08-25, Trello #178).
 *
 * NEDEN VAR: 26 günde telefonların 60 km'sinde 97 gerçek deprem (M5,8 dahil) oldu,
 * canlı dedektör hiçbirinde tetiklemedi. Neden tetiklemediğini göremiyoruz — sunucuya
 * yalnız tetik anındaki iki sayı gidiyor. Gölge, canlının YANINDA koşan ikinci bir
 * STA/LTA: düşük eşik, ALARM DEĞİL, yalnız 30 s ham dalga biçimi kaydı tetikler.
 *
 * NEDEN AYRI PARAMETRE: STALTA_PARAMS 109 Hz T1 sensöründen kopyalandı ("~0,5 s @
 * 109 Hz"); telefon 50 Hz'te örnekliyor → STA gerçekte 1,1 s, LTA 65 s, HP 0,09 Hz.
 * Ölçüm (simülasyon, gerçek aritmetik): bu düzeltme TEK BAŞINA anlamsız (M5,2 oranı
 * 3,0 → 2,2) — çözüm değil ama gölge doğru değerlerle koşmalı ki iki pencere yan
 * yana ölçülsün. Canlı dedektör bu commit'te DEĞİŞMİYOR (canlı yol dokunulmaz).
 *
 * EŞİK 2,5 ÖLÇÜMLE: medyan gürültüde (0,0207 m/s²) M5,2 @ 43 km 2,7–3,0 üretiyor,
 * M5,3 @ 59 km ~5. Hacim korkulukları fazlasını keser — bkz.
 * docs/plans/2026-08-25-kalabalik-golge-dedektor-design.md §4. Repo dışından okuyan
 * belgeye erişemez, o yüzden özet burada. Üçü de KODDA; cihaz tarafı henüz servise
 * BAĞLI DEĞİL (plan Task 6 — CrowdSensorService'e bağlama), o ayrım aşağıda:
 *   · saatlik tavan: sunucu POST /api/crowd/waveform cihaz başına saatte 2 kayıt
 *     kabul eder (DEVICE_LIMIT=2, pencere 1 sa), fazlası 429; cihazda ShadowGate.kt
 *     ShadowHourCounter MAX_PER_HOUR=2 aynı tavanı gönderimden ÖNCE uygular. Tasarımın
 *     ilk metnindeki "fazlası yalnız özet gider" fikri ÇIKARILDI (YAGNI): fazlası gitmez.
 *   · panel bayrağı: mobile_flags.shadow_waveform_enabled, varsayılan KAPALI ve
 *     fail-closed (okunamazsa kapalı sayılır); cihaz bayrağı ≤30 dk'da tazeler
 *     (checkFlags), yani sürüm çıkmadan filoya iner.
 *   · sessizlik kapısı: cihaz son 5 dk'da kayıt yaptıysa yenisini almaz — ShadowGate.kt,
 *     passesQuietGate'in ikizi (QUIET_MS = 300_000 = CROWD_QUIET_PERIOD_MS; null→geçer,
 *     negatif→geçmez, tam sınır→geçmez; testli). Bu TS fonksiyonu BUGÜN yalnız canlı oy
 *     yolunda (apps/assoc crowdTier) çağrılıyor; sunucunun gölge ucu sessizlik kapısı
 *     UYGULAMAZ — tek savunma cihazda.
 */
export const SHADOW_STALTA_PARAMS = {
  alphaHp: 2 * Math.PI * 0.2 / 50,   // 0,2 Hz @ 50 Hz = 0,025132741228718346
  // SIRA ÖNEMLİ: ((2·π)·0,2)/50. Yeniden gruplama (ör. 2·π/50·0,2) 1 ULP farklı
  // double verir (ölçüldü: 3,47e-18); Kotlin ikizi bu sırayı AYNEN yazmalı.
  nSta: 25,                          // 0,5 s @ 50 Hz
  nLta: 1500,                        // 30 s @ 50 Hz
  ltaMin: 1e-8,
  trigger: 2.5,
  detrigger: 1.5,
  warmupSamples: 1500,
} as const;

/** staltaStepWith'in parametre sözleşmesi: STALTA_PARAMS ile AYNI alanlar, sayı olarak
 *  (as const literal tipleri genişletilir ki iki set de geçsin). */
export type StaltaParams = { readonly [K in keyof typeof STALTA_PARAMS]: number };

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

/** GÖLGE DE BU init'i kullanır — parametreli bir init YOK, çünkü buradan okunan tek
 *  parametre ltaMin ve iki sette de 1e-8. SHADOW_STALTA_PARAMS.ltaMin bir gün
 *  farklılaşırsa bu fonksiyon da parametre almalı; stalta-shadow.test.ts iki ltaMin'in
 *  eşitliğini kilitler, yani sapma sessizce geçmez. */
export function initStalta(): StaltaState {
  return { dc: [0, 0, 0], sta: 0, lta: STALTA_PARAMS.ltaMin, armed: true, triggered: false, count: 0 };
}

/**
 * Parametreli adım — staltaStep'in aritmetiğinin BİREBİR aynısı, P dışarıdan gelir.
 * staltaStep bunu STALTA_PARAMS ile çağırır (tek gövde, iki giriş; ayrışamaz).
 *
 * P'nin HER alanının gerçekten BURADAN okunduğunu stalta-shadow.test.ts kilitler:
 * yedi alan tek tek ezilir, sonuç değişmezse test kırılır — yedi mutasyonun yedisi de
 * öldürüldü (ölçüldü). Hangisinin nerede öldüğü EŞİT DEĞİL, çünkü hepsi davranışa
 * aynı güçte yansımıyor:
 *   · nSta/nLta ayrıca DAVRANIŞSAL testte de ölür (gölge darbeyi canlıdan önce görür).
 *   · warmupSamples ayrıca warmup testinde ölür.
 *   · alphaHp/trigger/detrigger yalnız o sentetik ezme testinde ölür.
 *   · ltaMin EŞDEĞER MUTANT: iki sette de 1e-8, yani `P.ltaMin`i `STALTA_PARAMS.ltaMin`
 *     yapmak sahada hiçbir şeyi değiştirmez. Onu yalnız sentetik ezme yakalar; asıl
 *     koruma, iki değerin eşitliğini kilitleyen ltaMin testidir.
 *
 * İKİZLER (ikisi de artık MEVCUT):
 *   · Kotlin ikizi: StaLtaDetectorP (StaLta.kt) — parametreli; canlı yol için mevcut
 *     StaLtaDetector korundu (CrowdSensorService onu kullanmaya devam ediyor).
 *   · Golden fixture (crowd-golden.json) iki seti de kilitler: staltaParams/stalta
 *     canlı, shadowStaltaParams/shadowStalta gölge; aynı girdi vektörü.
 */
export function staltaStepWith(s: StaltaState, sample: number[], P: StaltaParams): StaltaResult {
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

export function staltaStep(s: StaltaState, sample: number[]): StaltaResult {
  return staltaStepWith(s, sample, STALTA_PARAMS);
}
