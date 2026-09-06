// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// .gsd dalga-formu frame decode — TEK KAYNAK (ingest DB yazımı + seedlink-feeder ENZ kanalı).
// Güvenlik-kritik: frame formatı firmware ile donanım-sözleşmesi; iki kopya = drift = yanlış
// sismik veri. Saf (I/O yok, tam test edilir).

import { GRAV_MAG_MIN_MS2, GRAV_MAG_MAX_MS2 } from "./orientation.js";

const FRAME_LEN = 42;
const SCALE = (4 * 9.80665) / 65536;

// Yerçekimi geçerlilik penceresi orientation.ts'ten gelir — TEK KAYNAK. Burada ayrı bir
// eşik tanımlamak sessiz bir çelişki üretirdi: aynı burst mount sınıflandırmasında
// "yönelim okunamaz" sayılırken dalga formunda döndürülmüş olurdu. Özellikle |g|≈7.35
// (dört sensörden biri ölü) daha gevşek bir eşikte İÇERİ girer ve ENZ uydurma bir
// dikeye döner — sessiz ve yanlış sismik veri.

// Yatay referans seçimi eşiği: dikey eksen gövde X'ine bundan daha hizalıysa X dejenere
// olur (ẑ'ye dik bileşeni sıfıra gider) ve yerine Y kullanılır. Eşik keyfi değil —
// 0.9'da gürültü payı ~1e-3, yani sabit monte bir cihazda seçim burst'ler arasında
// TİTREMEZ. Titreseydi EN1/EN2 aynı istasyonda anlamını değiştirirdi.
const DEGENERATE = 0.9;

export interface WaveTrace {
  t0Ms: number;
  fs: number;
  /** Frame başına 4-sensör-ortalama ivme büyüklüğü (m/s²). Rotasyondan bağımsız. */
  samples: number[];
  /** Gerçek dikey bileşen, YUKARI pozitif (SEED Z konvansiyonu). */
  samplesZ: number[];
  /** Yatay bileşen 1 — dikeye dik, ama azimutu BİLİNMİYOR (SEED kanal kodu `1`). */
  samples1: number[];
  /** Yatay bileşen 2 — `1`e ve dikeye dik, sağ-el sistemi (SEED kanal kodu `2`). */
  samples2: number[];
  /**
   * Yerçekimiyle hizalama yapılabildi mi. false ise eksenler GÖVDE çerçevesinde kalır
   * (samplesZ = gövde Z), yani "dikey" adı o burst için doğru değildir.
   */
  oriented: boolean;
}

/** .gsd frame burst → işlenmiş iz. t_us EPOCH µs (firmware wave frame'i now_ms()*1000 damgalar;
 *  epoch µs ~1.75e15 < 2^53 → Number() kayıpsız). DC (pencere ort) çıkarılır → her frame
 *  4-sensör-ort büyüklük (m/s²) + 3 EKSEN izi ayrı ayrı.
 *
 *  YÖNELİM: pencere ortalaması yerçekimi vektörüdür — ivmeölçer durgunken özgül kuvvet
 *  okur, yani vektör YUKARIYI gösterir. Ondan yerel bir çerçeve kurulur ve örnekler oraya
 *  döndürülür; böylece samplesZ cihazın nasıl monte edildiğinden bağımsız olarak GERÇEK
 *  dikeydir ve SeedLink'teki `ENZ` adı doğruyu söyler.
 *
 *  Yataylar `N`/`E` DEĞİL, `1`/`2`. Yerçekimi "aşağı"yı verir, pusula yönünü vermez —
 *  cihazda manyetometre yok. SEED'de `N`/`E` gerçek kuzey/doğu demektir; azimut
 *  bilinmiyorken `1`/`2` standardın öngördüğü koddur, kaçamak değil.
 *
 *  Rotasyon norm-koruyucudur: `samples` (büyüklük) bu değişiklikten etkilenmez.
 *
 *  Saf. NOT: frame'ler ZAMAN-SIRALI varsayılır (firmware ring→post sırayla yayınlar,
 *  MQTT tek-topic QoS1 sırayı korur) — t0Ms/durS ilk&son frame'den türetilir. */
export function waveBurstToTrace(buf: Buffer): WaveTrace {
  const fr: { tUs: number; s: number[] }[] = [];
  const seen = new Set<number>();
  for (let off = 0; off + FRAME_LEN <= buf.length; off += FRAME_LEN) {
    if (buf[off] !== 0xa5 || buf[off + 1] !== 0x5a) continue;
    const tUs = Number(buf.readBigUInt64LE(off + 8));
    // #181: firmware bir capture'da TAM 1320 frame yayınlar (WAVE_FRAMES). MQTT
    // redelivery bir chunk'ı TEKRAR getirince aynı frame'ler ikinci kez concat edilir ve
    // iz 1320'yi aşar (sahada 1571'e kadar görüldü — tek capture'dan fiziksel olarak
    // imkânsız). Her frame kendi donanım zaman damgasını taşır; meşru frame'ler ~9 ms
    // arayla FARKLI tUs taşır, yani aynı tUs = kopya → at. tUs=0 (saatsiz burst, olmamalı
    // çünkü trigger yolu ep>0 guard'lı) dedup'a girmez ki dejenere bir burst çökmesin.
    if (tUs !== 0) { if (seen.has(tUs)) continue; seen.add(tUs); }
    const s: number[] = [];
    for (let j = 0; j < 12; j++) s.push(buf.readInt16LE(off + 16 + j * 2) * SCALE);
    fr.push({ tUs, s });
  }
  if (fr.length === 0) {
    return { t0Ms: 0, fs: 0, samples: [], samplesZ: [], samples1: [], samples2: [], oriented: false };
  }
  const mean = new Array(12).fill(0);
  for (const f of fr) for (let j = 0; j < 12; j++) mean[j] += f.s[j];
  for (let j = 0; j < 12; j++) mean[j] /= fr.length;

  // Yerçekimi vektörü = 4 sensörün DC ortalaması (gövde çerçevesi).
  const gx = (mean[0] + mean[3] + mean[6] + mean[9]) / 4;
  const gy = (mean[1] + mean[4] + mean[7] + mean[10]) / 4;
  const gz = (mean[2] + mean[5] + mean[8] + mean[11]) / 4;
  const { basis, oriented } = localFrame(gx, gy, gz);
  const [uz, u1, u2] = basis;

  const samplesZ: number[] = [], samples1: number[] = [], samples2: number[] = [];
  const samples = fr.map((f) => {
    const ax = ((f.s[0]-mean[0]) + (f.s[3]-mean[3]) + (f.s[6]-mean[6]) + (f.s[9]-mean[9])) / 4;
    const ay = ((f.s[1]-mean[1]) + (f.s[4]-mean[4]) + (f.s[7]-mean[7]) + (f.s[10]-mean[10])) / 4;
    const az = ((f.s[2]-mean[2]) + (f.s[5]-mean[5]) + (f.s[8]-mean[8]) + (f.s[11]-mean[11])) / 4;
    samplesZ.push(ax*uz[0] + ay*uz[1] + az*uz[2]);
    samples1.push(ax*u1[0] + ay*u1[1] + az*u1[2]);
    samples2.push(ax*u2[0] + ay*u2[1] + az*u2[2]);
    return Math.sqrt(ax*ax + ay*ay + az*az);
  });
  const t0Ms = Math.round(fr[0].tUs / 1000);
  const durS = (fr[fr.length - 1].tUs - fr[0].tUs) / 1e6;
  const fs = durS > 0 ? (fr.length - 1) / durS : 110;
  return { t0Ms, fs, samples, samplesZ, samples1, samples2, oriented };
}

type Vec = [number, number, number];

/**
 * Yerçekimi vektöründen ortonormal yerel çerçeve kurar: [yukarı, yatay1, yatay2].
 * |g| makul aralığın dışındaysa yönelim okunamaz — gövde eksenleri aynen döner ve
 * oriented=false ile bildirilir (sessizce yanlış "dikey" üretmektense).
 */
function localFrame(gx: number, gy: number, gz: number): { basis: [Vec, Vec, Vec]; oriented: boolean } {
  const gm = Math.hypot(gx, gy, gz);
  if (!(gm >= GRAV_MAG_MIN_MS2 && gm <= GRAV_MAG_MAX_MS2)) {
    return { basis: [[0,0,1], [1,0,0], [0,1,0]], oriented: false };
  }
  const uz: Vec = [gx/gm, gy/gm, gz/gm];              // yukarı (özgül kuvvet yönü)
  // Dikeye en az hizalı gövde eksenini referans al, ona dik bileşeni yatay-1 yap.
  const ref: Vec = Math.abs(uz[0]) < DEGENERATE ? [1,0,0] : [0,1,0];
  const d = ref[0]*uz[0] + ref[1]*uz[1] + ref[2]*uz[2];
  let u1: Vec = [ref[0] - d*uz[0], ref[1] - d*uz[1], ref[2] - d*uz[2]];
  const m1 = Math.hypot(u1[0], u1[1], u1[2]);
  if (!(m1 > 1e-9)) return { basis: [[0,0,1], [1,0,0], [0,1,0]], oriented: false };
  u1 = [u1[0]/m1, u1[1]/m1, u1[2]/m1];
  const u2: Vec = [                                    // u2 = uz × u1 (sağ-el)
    uz[1]*u1[2] - uz[2]*u1[1],
    uz[2]*u1[0] - uz[0]*u1[2],
    uz[0]*u1[1] - uz[1]*u1[0],
  ];
  return { basis: [uz, u1, u2], oriented: true };
}
