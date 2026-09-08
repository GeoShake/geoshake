// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

/**
 * SARSINTI MERDIVENI — PGA esikleri, histerezis ve enstruman rampasi.
 * TEK KAYNAK: web (harita + istasyon sayfasi) ve mobil (konsol + titresim) buradan okur.
 *
 * ── NEDEN BURAYA TASINDI (2026-09-07) ───────────────────────────────────────
 * Ayni merdiven IKI dosyada bagimsiz yasiyordu ve senkronu bir YORUM tutuyordu:
 *   apps/web/lib/shake.ts        -> SHAKE_UP  { .06 / .15 / .40 }
 *     ustunde: "Esikler apps/mobile/src/utils/shakeLevel.ts ile SENKRON tutulmali"
 *   apps/mobile/src/utils/shakeLevel.ts -> PGA_THRESHOLDS { .06 / .15 / .40 }
 * Kopya yalniz sayilarda degildi: `nextShakeLevel` govdesi de iki yerde AYNI
 * algoritmayla, farkli degisken adlariyla yazilmisti. Bu depoda kural kopyasi
 * 2026-08-19/20'de dort kez hataya yol acti (bkz. kok CLAUDE.md).
 *
 * ── ESIKLERIN KAYNAGI (degistirme, olculdu) ─────────────────────────────────
 * Saha kalibrasyonu 2026-07-15. Ilk degerler (0.02/0.05/0.1) masa tikirtisini
 * bile sariya boyuyordu; deprem siddet olcegine hizalandi:
 *   light    ~ %0,6 g  hissedilir titresim   (MMI II-III)
 *   moderate ~ %1,5 g  herkes hisseder       (MMI IV)
 *   strong   ~ %4 g    esya oynatan sarsinti (MMI V+)
 *
 * ── BURADA OLMAYAN, KASITLI OLARAK ──────────────────────────────────────────
 *   · Titresim (haptics) tercihi  -> mobil; MMKV'ye yazar, paylasilamaz.
 *   · `severityWord`              -> web; Ingilizce tetik kelimesi ("noticeable").
 *   · `SHAKE_MARKER_CSS`          -> web; MapLibre isaretcisinin CSS'i.
 * Bunlar yuzeye ozgu; merdiven degil.
 */

export type ShakeLevel = "calm" | "light" | "moderate" | "strong";

const ORDER: ShakeLevel[] = ["calm", "light", "moderate", "strong"];

/** Merdivendeki sira (calm = 0). Karsilastirma icin; disaridan da kullanilir. */
export const rankOf = (l: ShakeLevel): number => ORDER.indexOf(l);

/** PGA esikleri (m/s^2) — yukselme yonu. */
export const PGA_THRESHOLDS: Record<Exclude<ShakeLevel, "calm">, number> = {
  light: 0.06,
  moderate: 0.15,
  strong: 0.4,
};

/**
 * Dusme carpani. PGA 1 Hz gelir ve esik sinirinda saniyelik ziplar; seviye
 * YUKSELIRKEN normal esik, DUSERKEN esigin %70'i kullanilir. Hem gorsel durum
 * hem titresim ayni seviyeyi paylastigi icin flap iki yerde birden gorunurdu.
 */
export const SHAKE_HYSTERESIS = 0.7;

/** PGA -> seviye, HISTEREZISSIZ. Zaman-serisi noktalarini boyamak icin
 *  (helikorder binleri, statik gosterimler). Canli akista `nextShakeLevel` kullan. */
export function pgaLevelOf(pga: number): ShakeLevel {
  if (pga >= PGA_THRESHOLDS.strong) return "strong";
  if (pga >= PGA_THRESHOLDS.moderate) return "moderate";
  if (pga >= PGA_THRESHOLDS.light) return "light";
  return "calm";
}

/** Bir sonraki seviye: yukselme ANINDA, dusme histerezisli. */
export function nextShakeLevel(prev: ShakeLevel, pga: number): ShakeLevel {
  const up = pgaLevelOf(pga);
  if (rankOf(up) > rankOf(prev)) return up;
  let lvl = prev;
  while (
    lvl !== "calm" &&
    pga < PGA_THRESHOLDS[lvl as Exclude<ShakeLevel, "calm">] * SHAKE_HYSTERESIS
  ) {
    lvl = ORDER[rankOf(lvl) - 1];
  }
  return lvl;
}

/**
 * ENSTRUMAN RAMPASI — dort basamak + olculemedi.
 *
 * ⚠ BASAMAKLAR RUTBEYE GORE ADLANDIRILDI, ANLAMA GORE DEGIL. Ayni rampa hem
 * zemin hareketini (iyi = sakin) hem WiFi sinyal gucunu (iyi = guclu) tasiyor;
 * anlami cagiran fonksiyon verir. `calm`/`strong` diye adlandirilsaydi sinyal
 * tarafinda okunmaz olurdu.
 *
 * Kurucu karari 2026-09-05: rampa KISIK, enstruman tonunda — onaylanan
 * prototipin dili. Uygulamanin genel Material doygunlugundan bilincli olarak
 * ayri; koyu bir konsolda fark gercek.
 */
export const SHAKE_RAMP = {
  /** En iyi uc: sakin zemin / mukemmel sinyal. */
  iyi: "#7FB069",
  /** Bir adim asagi. */
  orta: "#9EB069",
  /** Uyari basamagi. */
  uyari: "#D4883A",
  /** En kotu uc: guclu sarsinti / zayif sinyal. */
  kotu: "#C4443A",
  /** Olculemedi (veri yok / cihaz cevrimdisi). */
  bilinmiyor: "#6B7075",
} as const;

/**
 * UC EKSEN TONU (X/Y/Z) — RMS grafigi.
 *
 * ⚠ RAMPADAN AYRI BIR AILE ve bu ZORUNLU: rampa ANLAMSAL (siddet: iyi -> kotu),
 * eksenler KATEGORIK (X/Y/Z arasinda siralama YOK). Eksenlere rampa rengi
 * verilseydi kirmizi bir Z ekseni "tehlike" diye okunurdu.
 *
 * Eski uclunun HUE'lari korundu (kullanicinin ogrendigi X=mercan / Y=turkuaz /
 * Z=altin degismesin), doygunluk ve aciklik rampanin kaydina cekildi.
 * X'in hue'su 5,5 -> 350 kaydirildi: kisilmis X (#BD5C52) siddet kirmizisina
 * (#C4443A) CIE76 dE=16 kadar yaklasiyordu, 350'de mesafe 23,4 ile korundu.
 * Olculen mesafeler (dE): X-Y 81,7 · X-Z 61,4 · Y-Z 60,0.
 */
export const SHAKE_AXIS = {
  /** Yatay 1 — mercan, hue 350. */
  x: "#BD5264",
  /** Yatay 2 — turkuaz. */
  y: "#52BDB6",
  /** Dikey — altin. */
  z: "#BDA652",
} as const;

/** Seviye -> rampa tonu. Merdivenin tek boyama noktasi. */
export const SHAKE_COLOR: Record<ShakeLevel, string> = {
  calm: SHAKE_RAMP.iyi,
  light: SHAKE_RAMP.orta,
  moderate: SHAKE_RAMP.uyari,
  strong: SHAKE_RAMP.kotu,
};

/** PGA -> rampa tonu (seviye araligi disarida gorunmeden). */
export function shakeColorOf(pga: number): string {
  return SHAKE_COLOR[pgaLevelOf(pga)];
}

/**
 * SARSINTI ANIMASYONU — seviye -> genlik (px/pt) ve sure (ms).
 *
 * Prototipten BIREBIR: "My Station Single Device", en alttaki "Cihaz
 * sarsildiginda ne olsun" bolumu, A paneli. Degerler tarayicida sahte saatle
 * surulup OLCULDU, uydurulmadi.
 *
 * ⚠ OLCEKLENMEZ ve bu bilincli: prototipin cihaz gorseli 190 px, tuketicilerde
 * 220. Oran 1,16 — genligi bu oranla carpmak 11'i 12,7'ye cikarirdi, yani
 * kurucunun ONAYLADIGI hareketi degistirirdi. Onay gorsel uzerinden verildi,
 * sayi uzerinden degil.
 *
 * ⚠ TETIK YENI BIR ESIK AILESI ACMAZ: bu tablo PGA'ya bakmaz, hazir bir
 * `ShakeLevel` ile indekslenir. Seviye `nextShakeLevel`ten gelir.
 */
export const SHAKE_MOTION: Record<Exclude<ShakeLevel, "calm">, { genlik: number; sure: number }> = {
  light: { genlik: 2, sure: 900 },
  moderate: { genlik: 5, sure: 1250 },
  strong: { genlik: 11, sure: 1650 },
};

/**
 * Sonumlenen salinim zarfi: sert giris, ussel sonum. Gercek bir sismogram
 * zarfi boyle davranir; SABIT genlikli bir titreme "bozuk" gibi okunur.
 *
 * Prototipte olculen ornek (strong, genlik 11 / sure 1650):
 *   96 ms -> 8,52 · 480 ms -> 2,42 · 1120 ms -> 0,50 · sonra temiz.
 */
export function shakeDecay(t: number, sure: number): number {
  return Math.exp(-4.2 * (t / sure));
}

/**
 * FIRMWARE LED DESENI — cihazin GERCEK davranisi (firmware/main/main.c, led_task).
 * Yuzeylerdeki cihaz gorseli bu sayilari taklit eder; uydurma bir ritim
 * kullanmak, kullanicinin masasindaki cihazla ekrandakinin AYRI seyler
 * soylemesi demek olurdu.
 *
 * Not: burada yalniz uzaktan BILINEBILEN iki durum var. `blink` (baglanti yok),
 * `panik` (self-check hatasi) ve kimlik desenleri cihaz-yerel durumlardir —
 * o hallerdeki bir cihaz sunucuya zaten RAPOR ETMEZ, dolayisiyla web/mobil
 * onlari bilemez ve TAKLIT ETMEMELIDIR.
 */
export const LED_PATTERN = {
  /** Normal nabiz: ucgen dalga, 3 s periyot (LED_NABIZ_MS). */
  nabizMs: 3000,
  /** Nabzin tepe parlakligi — tam parlakligin %30'u (LED_NABIZ_PEAK). */
  nabizTepe: 0.3,
  /** Tetik sonrasi TAM parlak sabit yanma suresi (LED_BURST_MS). */
  patlamaMs: 2500,
} as const;
