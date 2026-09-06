// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

/**
 * Cihaz yönelimi — yerçekimi (DC) vektöründen türetilir.
 *
 * Firmware her 1 s'lik pencerede zaten gravity'yi DC ortalaması olarak çıkarıyor
 * (main.c, "Gravity (DC) cikar"). Duran bir ivmeölçerde o ortalama vektör
 * DOĞRUDAN cihazın yönelimidir; artık atılmıyor, `grav` alanıyla yayınlanıyor.
 * Kullanıcıya "duvara mı monte ettin" diye sormaya gerek yok — cihaz kendi bilir,
 * taşınırsa bir sonraki health/tetik paketinde kendi günceller.
 *
 * Girdi sözleşmesi: [gx, gy, gz], m/s², 4 sensör ORTALANMIŞ (firmware ortalamayı
 * compute_features/stalta_step ile aynı op-order'da yapar). Uzunluk 3 değilse,
 * sonlu değilse ya da büyüklük fizik penceresinin dışındaysa yönelim ÜRETİLMEZ.
 */

/** Montaj sınıfı. Değerler ASCII/Türkçe — repo sözleşmesi (bkz. intensityClass: hafif/orta/siddetli). */
export type MountClass = "duz" | "duvar" | "egik";

export interface Orientation {
  /**
   * Cihaz +Z ekseninin DİKEYDEN (yukarı yön) sapması, derece. 0..180.
   * 0 = Z yukarı (düz masa), 90 = Z yatay (duvar), 180 = Z aşağı (ters düz).
   */
  tiltDeg: number;
  mountClass: MountClass;
  /** Ölçülen yerçekimi büyüklüğü (m/s²) — teşhis alanı; sağlıklı cihazda ~9.81 beklenir. */
  gMag: number;
}

/* ---- Sınıflandırma eşikleri (derece) ----
   Ürün kararı; kodda SABİT tutulur (kullanıcıya sorulmaz, UI'dan ayarlanmaz).
   düz  : 0..20     — masa/zemin/raf gibi yatay yüzey
   duvar: 70..110   — dikey yüzey; 90'ın iki yanına simetrik pay bırakır
   eğik : geri kalan (20..70 ve 110..180)
   NOT: 110..180 bölgesi (ters/baş aşağı düz montaj) bu literal sözleşmeye göre "egik"
   çıkar. Ters-düzü "duz" saymak ayrı bir ürün kararıdır — bilerek YAPILMADI. */
export const TILT_FLAT_MAX_DEG = 20;
export const TILT_WALL_MIN_DEG = 70;
export const TILT_WALL_MAX_DEG = 110;

/* ---- Geçerlilik penceresi (m/s²) ----
   Dünya yüzeyinde g = 9.78..9.83. Pencere buna sensör kalibrasyon toleransı payı ekler.
   Dışarıda kalan tipik durumlar (hepsinde yönelim ANLAMSIZDIR, null döner):
     • |g|≈0      → serbest düşüş, ölü/kopuk sensör dizisi, kalibrasyon penceresi
     • |g|≈7.35   → 4 sensörden biri ölü (0 okuyan sensör ortalamayı %25 düşürür)
     • |g| büyük  → pencere boyunca süren şiddetli sarsıntı (DC ortalama bozulur) */
export const GRAV_MAG_MIN_MS2 = 8.5;
export const GRAV_MAG_MAX_MS2 = 11.0;

const RAD2DEG = 180 / Math.PI;

/** Eğim açısından montaj sınıfı. Saklanmış tiltDeg'den yeniden sınıflandırmak için de kullanılır. */
export function classifyMount(tiltDeg: number): MountClass {
  if (tiltDeg <= TILT_FLAT_MAX_DEG) return "duz";
  if (tiltDeg >= TILT_WALL_MIN_DEG && tiltDeg <= TILT_WALL_MAX_DEG) return "duvar";
  return "egik";
}

/**
 * Yerçekimi vektöründen yönelim. Girdi kullanılamazsa null (çağıran null saklar) —
 * asla uydurma bir yönelim üretmez.
 */
export function computeOrientation(grav: readonly number[] | null | undefined): Orientation | null {
  if (!Array.isArray(grav) || grav.length !== 3) return null;
  const [gx, gy, gz] = grav;
  if (!Number.isFinite(gx) || !Number.isFinite(gy) || !Number.isFinite(gz)) return null;

  const gMag = Math.hypot(gx, gy, gz);
  if (gMag < GRAV_MAG_MIN_MS2 || gMag > GRAV_MAG_MAX_MS2) return null;

  /* atan2(yatay, dikey) — acos(gz/|g|) DEĞİL: acos ±1 civarında (yani tam düz montajda,
     en sık durumda) duyarlılığını yitirir; küçük gz yuvarlamaları derecelerce hata verir.
     atan2 her iki uçta da koşullu-iyi ve doğrudan [0,π] üretir (yatay bileşen ≥ 0). */
  const tiltDeg = Math.atan2(Math.hypot(gx, gy), gz) * RAD2DEG;
  return { tiltDeg, mountClass: classifyMount(tiltDeg), gMag };
}
