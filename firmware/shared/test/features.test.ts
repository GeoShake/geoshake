// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { computeWindowFeatures } from "../src/features.js";

function frame(vals: number[]) { return vals; }

describe("computeWindowFeatures", () => {
  it("sıfır pencerede RMS ve PGA sıfır", () => {
    const win = [frame(new Array(12).fill(0)), frame(new Array(12).fill(0))];
    const f = computeWindowFeatures(win);
    expect(f.rms).toEqual([0, 0, 0]);
    expect(f.pga).toBe(0);
  });

  it("golden vektör — bilinen girdi bilinen çıktı", () => {
    const s0 = [0.02,0,0, 0.02,0,0, 0.02,0,0, 0.02,0,0];
    const s1 = [-0.02,0,0, -0.02,0,0, -0.02,0,0, -0.02,0,0];
    const f = computeWindowFeatures([s0, s1]);
    expect(f.rms[0]).toBeCloseTo(0.02, 6);
    expect(f.rms[1]).toBeCloseTo(0, 6);
    expect(f.rms[2]).toBeCloseTo(0, 6);
    expect(f.pga).toBeCloseTo(0.02, 6);
  });

  // İkinci golden: üç ekseni de aktif + örnekler-arası değişken genlik. İlk golden
  // y=z=0 ve sabit genlik olduğu için şunları KAÇIRIR; bu vektör onları kilitler:
  //  - RMS ≠ mean-abs (s0/s1 için |x| ortalaması 0.02 ama rms_x=0.0223607)
  //  - PGA 3-eksenli vektör büyüklüğü (s0'ın 3-4-5 büyüklüğü 0.05 baskın)
  //  - eksen sıralaması (x≠y≠z): rms_x≈0.0223607, rms_y≈0.0282843, rms_z≈0.0141421
  // Değerler node'da gerçek computeWindowFeatures çalıştırılarak el-hesabıyla
  // birebir doğrulandı (B2 firmware bu golden'a karşı assert edecek).
  it("ikinci golden — 3 eksen, değişken genlik (RMS, PGA, eksen sırası kilitli)", () => {
    const s0 = [0.03,0.04,0, 0.03,0.04,0, 0.03,0.04,0, 0.03,0.04,0];
    const s1 = [0.01,0,0.02, 0.01,0,0.02, 0.01,0,0.02, 0.01,0,0.02];
    const f = computeWindowFeatures([s0, s1]);
    expect(f.rms[0]).toBeCloseTo(0.0223607, 6); // sqrt((0.03²+0.01²)/2)
    expect(f.rms[1]).toBeCloseTo(0.0282843, 6); // sqrt(0.04²/2)
    expect(f.rms[2]).toBeCloseTo(0.0141421, 6); // sqrt(0.02²/2)
    expect(f.pga).toBeCloseTo(0.05, 6);         // max(√(.03²+.04²), √(.01²+.02²)) = 0.05
  });
});
