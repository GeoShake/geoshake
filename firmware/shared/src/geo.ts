// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

/** P-dalgası hızı (km/s) — TEK KAYNAK. assoc motoru (locate), gs-backfill (varış penceresi) ve
 *  cluster yarıçapı fizik kuralı (maxClusterRadiusKm) AYNI sayıyı kullanmak ZORUNDA: kopyalar
 *  ayrışırsa kural, kümeleme ve backfill birbirinden farklı bir fizik varsayar ve tutarsızlık
 *  hiçbir yerde hata vermeden yayılır. Daha önce iki elle kopya vardı (index.ts + backfillCli.ts),
 *  yorumları "ikisi birlikte değişir" diyordu — insan hafızasına bağlı garanti, garanti değildir. */
export const VP_KM_S = 6;

/** Küre yarıçapı (km) — haversineKm ve boundingBoxDeg AYNI sayıyı kullanmak ZORUNDA:
 *  kutu, dairenin SÜPERKÜMESİ olmalı. Farklı yarıçaplar kutuyu daireden dar yapıp
 *  sınırdaki komşuyu sessizce eleyebilirdi (ön-eleme hiç hata vermez, sadece kaybeder). */
export const EARTH_RADIUS_KM = 6371;

/** 1° ENLEM kaç km (her enlemde AYNI): 2πR/360 = 111,195 km. Boylamın aksine sabit. */
export const KM_PER_DEG_LAT = (EARTH_RADIUS_KM * Math.PI) / 180;

/** İki nokta arası büyük-çember mesafe (km). Küçük bölgede yeterli. */
export function haversineKm(aLat: number, aLon: number, bLat: number, bLon: number): number {
  const toRad = (d: number) => (d * Math.PI) / 180;
  const dLat = toRad(bLat - aLat);
  const dLon = toRad(bLon - aLon);
  const s = Math.sin(dLat / 2) ** 2 +
    Math.cos(toRad(aLat)) * Math.cos(toRad(bLat)) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_RADIUS_KM * Math.asin(Math.sqrt(s));
}

/**
 * Bir noktanın etrafındaki r km'lik DAİRENİN sınırlayıcı kutusu (derece cinsinden
 * yarı-genişlikler). Ön-elemedir: kutu KESİN filtre değil, haversineKm'in ucuz
 * bekçisidir — "önce kutu, sonra daire" (kesin karar HER ZAMAN haversine'de).
 *
 * NEDEN VAR (2026-08-10, kalabalık kümeleyici): "20 km" bir HÜCRE ADIMI sayısıyla
 * ifade EDİLEMEZ. İki ayrı tuzak:
 *   1. Kare ≠ daire — 20 km'lik kutunun KÖŞESİ √2×20 ≈ 28 km eder.
 *   2. 0,01° BOYLAM enlemle daralır: 4°K'de 1,11 km, 41°K'de 0,84 km. Aynı adım
 *      sayısı Bogotá'da ve İstanbul'da FARKLI fiziksel yarıçap demektir.
 *
 * dLon MATUSCHEK formülüyle: en büyük boylam sapması dairenin TEĞET noktasında
 * olur, φ0±dLat'ta değil — naif `r / (km_per_deg_lon(φ0±dLat))` biraz GENİŞ çıkar
 * (zararsız ama gereksiz iş). Teğet noktada: Δλ = asin(sin(r/R) / cos φ0).
 *
 * KUTUP/SARMA: daire kutbu içine alıyorsa (veya cos φ0 çok küçükse) boylam
 * kısıtı ANLAMSIZDIR → dLon = 180 (tüm boylamlar). Çağıran ±180 sarmasını kendi
 * ele alır; haversine sarmayı zaten doğru hesaplar.
 *
 * Saf. Geçersiz/sonsuz girdi → {0,0} (hiçbir komşu seçilmez; sessiz "her yer" DEĞİL).
 */
export function boundingBoxDeg(lat: number, radiusKm: number): { dLat: number; dLon: number } {
  if (!Number.isFinite(lat) || !Number.isFinite(radiusKm) || radiusKm <= 0) return { dLat: 0, dLon: 0 };
  const dLatRad = radiusKm / EARTH_RADIUS_KM;
  const dLat = (dLatRad * 180) / Math.PI;
  const latRad = (lat * Math.PI) / 180;
  const HALF_PI = Math.PI / 2;
  // Kutup daire İÇİNDE → her boylam daireye girebilir.
  if (latRad + dLatRad >= HALF_PI || latRad - dLatRad <= -HALF_PI) return { dLat, dLon: 180 };
  const s = Math.sin(dLatRad) / Math.cos(latRad);
  if (!(s < 1)) return { dLat, dLon: 180 };   // NaN veya ≥1 → tam sarma
  return { dLat, dLon: (Math.asin(s) * 180) / Math.PI };
}
