// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// ↙ T1 FIRMWARE SOZLESMESI (blep-0.9.27, 2026-09-08): esik 4,0 + ON-TETIK LTA DONDURMA.
// Bu dosya uc seyi kilitler: (1) parametre setleri — telefon yolu DOKUNULMADI, T1 ayri;
// (2) firmware golden_stalta_freeze_check ile BIREBIR ayni deterministik dizi ve sayilar;
// (3) dondurmanin davranisi: isinma kapisi, nLta tavani, yavas yukselen sinyalde kazanc.
import { describe, expect, it } from "vitest";
import {
  STALTA_PARAMS, SHADOW_STALTA_PARAMS, T1_STALTA_PARAMS, initStalta, staltaStepWith,
  type StaltaParams,
} from "../src/stalta.js";

describe("T1_STALTA_PARAMS — sozlesme", () => {
  it("esik 4,0 ve ltaFreeze 2,0 (kurucu karari 2026-09-08); diger alanlar STALTA_PARAMS ile ayni", () => {
    expect(T1_STALTA_PARAMS.trigger).toBe(4);
    expect(T1_STALTA_PARAMS.ltaFreeze).toBe(2);
    const { trigger: _t, ltaFreeze: _f, ...rest } = T1_STALTA_PARAMS;
    const { trigger: _t2, ltaFreeze: _f2, ...restBase } = STALTA_PARAMS;
    expect(rest).toEqual(restBase);
  });

  it("telefon (STALTA_PARAMS) ve golge setinde dondurma KAPALI — o yollar bayt-bayt eski", () => {
    expect(STALTA_PARAMS.ltaFreeze).toBe(0);
    expect(SHADOW_STALTA_PARAMS.ltaFreeze).toBe(0);
    expect(STALTA_PARAMS.trigger).toBe(5);
  });
});

// ── Deterministik uretec: MINSTD (x = x*48271 mod 2^31-1). Carpim < 2^53, yani double'da
// TAM; C tarafi uint64 ile ayni tam sayilari uretir ve (double)x / 2147483647.0 ile ayni
// IEEE bolmeyi yapar. Transandantal fonksiyon YOK (sin/exp libm'de son ULP'de ayrisabilir).
function minstd(seed: number) {
  let x = seed;
  return () => { x = (x * 48271) % 2147483647; return x / 2147483647; };
}
/** Firmware golden_stalta_freeze_check ile AYNI dizi: 4 sensor x 3 eksen, sensor basina
 *  kucuk sabit fark, ±0,01 duz gurultu; STEP_AT'tan sonra tum eksenlere ±amp kare dalga
 *  (8 ornek periyot). amp=0,003 oran 2-4 arasinda tutar: dondurma ACIK, tetik YOK. */
const GOLDEN_N = 3600, GOLDEN_STEP_AT = 3300, GOLDEN_SEED = 12345, GOLDEN_AMP = 0.003;
function goldenSample(i: number, nx: () => number, amp = GOLDEN_AMP, stepAt = GOLDEN_STEP_AT): number[] {
  const out: number[] = [];
  for (let k = 0; k < 4; k++) {
    const base = [9.81 + 0.01 * k, 0.10 - 0.01 * k, 0.20 + 0.005 * k];
    for (let j = 0; j < 3; j++) {
      let v = base[j] + (nx() * 0.02 - 0.01);
      if (i >= stepAt) v += ((i % 8) < 4) ? amp : -amp;
      out.push(v);
    }
  }
  return out;
}
function runGolden(P: StaltaParams, n = GOLDEN_N, amp = GOLDEN_AMP, stepAt = GOLDEN_STEP_AT) {
  const nx = minstd(GOLDEN_SEED);
  let s = initStalta(), fired = 0, ratio = 0, frzMax = 0, rmin = Infinity, rmax = 0;
  const trace: number[] = [];
  for (let i = 0; i < n; i++) {
    const r = staltaStepWith(s, goldenSample(i, nx, amp, stepAt), P);
    s = r.state; ratio = r.ratio; trace.push(r.ratio);
    if (r.fired) fired++;
    if (s.frz > frzMax) frzMax = s.frz;
    if (i >= stepAt + 60) { rmin = Math.min(rmin, r.ratio); rmax = Math.max(rmax, r.ratio); }
  }
  return { state: s, ratio, fired, frzMax, rmin, rmax, trace };
}

describe("golden — firmware golden_stalta_freeze_check ile ayni sayilar", () => {
  // Bu literaller firmware/main/main.c golden_stalta_freeze_check icindekilerle AYNI olmali;
  // biri degisirse ikisi birden guncellenir (3-adimlik golden ile ayni sozlesme).
  it("3600 ornek, adim 3300'de: sta/lta/oran/frz TAM eslesir; tetik yok; oran 2-4 bandinda", () => {
    const g = runGolden(T1_STALTA_PARAMS);
    expect(g.state.sta).toBe(0.000049736206388472704);
    expect(g.state.lta).toBe(0.000015752361206703420);
    expect(g.ratio).toBe(3.1573810259827875);
    expect(g.state.frz).toBe(284);
    expect(g.fired).toBe(0);
    expect(g.state.triggered).toBe(false);
    expect(g.rmin).toBeGreaterThanOrEqual(2);   // dondurma esiginin ustunde kaldi
    expect(g.rmax).toBeLessThan(4);             // tetik esiginin altinda kaldi
  });

  it("dondurma KAPALI ayni dizide FARKLI lta/oran verir (golden dondurmayi gercekten sinar)", () => {
    const off = runGolden({ ...T1_STALTA_PARAMS, ltaFreeze: 0 });
    expect(off.state.frz).toBe(0);
    expect(off.state.lta).toBeGreaterThan(0.000015752361206703420 * 1.1);   // LTA sinyali "yedi"
    expect(off.ratio).toBeLessThan(3.1573810259827875 - 0.4);
    // sta AYNI: dondurma yalniz LTA'ya dokunur
    expect(off.state.sta).toBe(0.000049736206388472704);
  });
});

describe("on-tetik LTA dondurma — davranis", () => {
  it("isinma bitmeden ASLA donmaz: buyuk sinyalde bile frz=0 ve durum dondurma-kapaliyla birebir", () => {
    // Adim isinmanin ICINDE (1000. ornek), dizi isinma bitmeden (3270) durur.
    const on = runGolden(T1_STALTA_PARAMS, 3200, 0.05, 1000);
    const off = runGolden({ ...T1_STALTA_PARAMS, ltaFreeze: 0 }, 3200, 0.05, 1000);
    expect(on.frzMax).toBe(0);
    expect(on.state).toEqual(off.state);
    expect(on.trace).toEqual(off.trace);
  });

  it("tavan: kalici basamakta dondurma en fazla nLta ornek surer, sonra LTA uyum saglar ve oran 2'nin altina iner", () => {
    // 3300'de basla, 3270 donuk ornek + ~27 s uyum: LTA sinyale yaklasir, oran < 2, sayac azalir.
    const n = GOLDEN_STEP_AT + T1_STALTA_PARAMS.nLta + 3000;
    const g = runGolden(T1_STALTA_PARAMS, n);
    expect(g.frzMax).toBeGreaterThan(T1_STALTA_PARAMS.nLta);   // tavana ulasildi, sayim devam etti
    expect(g.fired).toBe(0);
    expect(g.ratio).toBeLessThan(2);
    expect(g.state.frz).toBeLessThan(g.frzMax);                 // sizdiran sayac geri iniyor
    // Uyum gercekten oldu: LTA, adim oncesi degerin (1,58e-5) cok ustunde
    expect(g.state.lta).toBeGreaterThan(0.000015752361206703420 * 1.8);
  });

  it("yavas yukselen (12 s rampa) sinyalde dondurma tepe orani YUKSELTIR — kararin sebebi", () => {
    // 77 km'den gelen M4,6-4,8 sinifi: 12 s boyunca dogrusal buyuyen kare dalga, sonra 3 s sabit.
    const N = 3300 + 1300 + 330;
    const ramp = (i: number) => {
      if (i < 3300) return 0;
      const k = Math.min(1, (i - 3300) / 1300);
      return 0.006 * k;
    };
    const run = (P: StaltaParams) => {
      const nx = minstd(GOLDEN_SEED);
      let s = initStalta(), peak = 0;
      for (let i = 0; i < N; i++) {
        const a = ramp(i);
        const smp = goldenSample(i, nx, a, 3300);
        const r = staltaStepWith(s, smp, P);
        s = r.state;
        if (i >= 3300 && r.ratio > peak) peak = r.ratio;
      }
      return peak;
    };
    // Tetik dallarini ayirmak icin erisilemez esik: yalniz LTA dinamigi olculur.
    const withFreeze = run({ ...T1_STALTA_PARAMS, trigger: 1e9 });
    const without = run({ ...T1_STALTA_PARAMS, trigger: 1e9, ltaFreeze: 0 });
    expect(withFreeze).toBeGreaterThan(without * 1.15);
  });
});
