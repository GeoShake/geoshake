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

const FRAME_LEN = 42;
const SCALE = (4 * 9.80665) / 65536;

/** .gsd frame burst → işlenmiş iz. t_us EPOCH µs (firmware wave frame'i now_ms()*1000 damgalar;
 *  epoch µs ~1.75e15 < 2^53 → Number() kayıpsız). DC (pencere ort) çıkarılır → her frame
 *  4-sensör-ort büyüklük (m/s²) + 3 EKSEN izi ayrı ayrı (sx/sy/sz — SAC X/Y/Z export'u P/S
 *  seçimi için eksen ister; magnitüd her zaman ≥0, polarite kaybolur; helikorder/ENZ dikey ister).
 *  Saf. NOT: frame'ler ZAMAN-SIRALI varsayılır (firmware ring→post sırayla yayınlar,
 *  MQTT tek-topic QoS1 sırayı korur) — t0Ms/durS ilk&son frame'den türetilir. */
export function waveBurstToTrace(buf: Buffer): {
  t0Ms: number; fs: number; samples: number[];
  samplesX: number[]; samplesY: number[]; samplesZ: number[];
} {
  const fr: { tUs: number; s: number[] }[] = [];
  for (let off = 0; off + FRAME_LEN <= buf.length; off += FRAME_LEN) {
    if (buf[off] !== 0xa5 || buf[off + 1] !== 0x5a) continue;
    const tUs = Number(buf.readBigUInt64LE(off + 8));
    const s: number[] = [];
    for (let j = 0; j < 12; j++) s.push(buf.readInt16LE(off + 16 + j * 2) * SCALE);
    fr.push({ tUs, s });
  }
  if (fr.length === 0) return { t0Ms: 0, fs: 0, samples: [], samplesX: [], samplesY: [], samplesZ: [] };
  const mean = new Array(12).fill(0);
  for (const f of fr) for (let j = 0; j < 12; j++) mean[j] += f.s[j];
  for (let j = 0; j < 12; j++) mean[j] /= fr.length;
  const samplesX: number[] = [], samplesY: number[] = [], samplesZ: number[] = [];
  const samples = fr.map((f) => {
    const ax = ((f.s[0]-mean[0]) + (f.s[3]-mean[3]) + (f.s[6]-mean[6]) + (f.s[9]-mean[9])) / 4;
    const ay = ((f.s[1]-mean[1]) + (f.s[4]-mean[4]) + (f.s[7]-mean[7]) + (f.s[10]-mean[10])) / 4;
    const az = ((f.s[2]-mean[2]) + (f.s[5]-mean[5]) + (f.s[8]-mean[8]) + (f.s[11]-mean[11])) / 4;
    samplesX.push(ax); samplesY.push(ay); samplesZ.push(az);
    return Math.sqrt(ax*ax + ay*ay + az*az);
  });
  const t0Ms = Math.round(fr[0].tUs / 1000);
  const durS = (fr[fr.length - 1].tUs - fr[0].tUs) / 1e6;
  const fs = durS > 0 ? (fr.length - 1) / durS : 110;
  return { t0Ms, fs, samples, samplesX, samplesY, samplesZ };
}
