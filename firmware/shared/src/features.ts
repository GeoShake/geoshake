// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

/**
 * Bir 1 s penceresinden özellik çıkarır. Girdi: örnek listesi; her örnek
 * 12 sayı (4 sensör × 3 eksen, sensör-major, gravity ÇIKARILMIŞ, m/s²).
 * 4 sensör ortalanır (Faz 0 dizi kazancı), sonra eksen-başına RMS ve
 * pencere-içi tepe vektör büyüklüğü (PGA) hesaplanır.
 * Firmware C tarafı bu mantığı birebir aynalar (golden test kilitler).
 * Her örnek TAM 12 elemanlı olmalı (doğrulanmaz; eksikse NaN üretir — çağıran
 * sabit-şekil veri vermeli).
 */
export interface WindowFeatures { rms: [number, number, number]; pga: number; }

export function computeWindowFeatures(samples: number[][]): WindowFeatures {
  const n = samples.length;
  if (n === 0) return { rms: [0, 0, 0], pga: 0 };
  let sx = 0, sy = 0, sz = 0, pga = 0;
  for (const s of samples) {
    const ax = (s[0] + s[3] + s[6] + s[9]) / 4;
    const ay = (s[1] + s[4] + s[7] + s[10]) / 4;
    const az = (s[2] + s[5] + s[8] + s[11]) / 4;
    sx += ax * ax; sy += ay * ay; sz += az * az;
    const mag = Math.sqrt(ax * ax + ay * ay + az * az);
    if (mag > pga) pga = mag;
  }
  return { rms: [Math.sqrt(sx / n), Math.sqrt(sy / n), Math.sqrt(sz / n)], pga };
}
