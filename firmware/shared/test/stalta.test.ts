// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { initStalta, staltaStep, STALTA_PARAMS } from "../src/stalta.js";

// 12-elemanlı örnek üret: 4 sensör de aynı [x,y,z] (sensör-major)
const s12 = (x: number, y: number, z: number) => [x, y, z, x, y, z, x, y, z, x, y, z];

// N örnek geçir, son adımı döndür
function run(samples: number[][]) {
  let st = initStalta();
  let last = { state: st, fired: false, ratio: 0, pga: 0 };
  let fires = 0;
  for (const s of samples) { last = staltaStep(last.state, s); if (last.fired) fires++; }
  return { last, fires };
}

describe("staltaStep", () => {
  it("sabit girdide ratio≈1, tetik yok", () => {
    const samples = Array.from({ length: 200 }, () => s12(9.81, 0, 0));
    const { last, fires } = run(samples);
    expect(fires).toBe(0);
    expect(last.ratio).toBeLessThan(STALTA_PARAMS.trigger);
  });

  it("warm-up bitince güçlü onset bir kez tetikler (yükselen kenar)", () => {
    const quiet = Array.from({ length: STALTA_PARAMS.warmupSamples + 100 }, () => s12(9.81, 0, 0));
    const shake = Array.from({ length: 50 }, (_, i) => s12(9.81 + (i % 2 ? 0.5 : -0.5), 0, 0));
    const { last, fires } = run([...quiet, ...shake]);
    expect(fires).toBe(1);
    expect(last.state.triggered || !last.state.armed).toBe(true);
  });

  it("warm-up sırasında güçlü sinyal tetiklemez", () => {
    const shakeEarly = Array.from({ length: 100 }, (_, i) => s12(9.81 + (i % 2 ? 1 : -1), 0, 0));
    const { fires } = run(shakeEarly);
    expect(fires).toBe(0);
  });

  it("tetikteyken LTA dondurulur (sabit kalır)", () => {
    let st = initStalta();
    for (let i = 0; i < STALTA_PARAMS.warmupSamples + 100; i++) st = staltaStep(st, s12(9.81, 0, 0)).state;
    let r = staltaStep(st, s12(20, 0, 0));
    while (!r.fired) r = staltaStep(r.state, s12(20, 0, 0));
    const ltaAtTrigger = r.state.lta;
    const r2 = staltaStep(r.state, s12(20, 0, 0));
    expect(r2.state.triggered).toBe(true);
    expect(r2.state.lta).toBe(ltaAtTrigger);
  });

  it("tetik anında pga>0 ve ratio≥trigger (TriggerPacket alanları)", () => {
    let st = initStalta();
    for (let i = 0; i < STALTA_PARAMS.warmupSamples + 100; i++) st = staltaStep(st, s12(9.81, 0, 0)).state;
    let r = staltaStep(st, s12(20, 0, 0));
    while (!r.fired) r = staltaStep(r.state, s12(20, 0, 0));
    expect(r.fired).toBe(true);
    expect(r.ratio).toBeGreaterThanOrEqual(STALTA_PARAMS.trigger);
    expect(r.pga).toBeGreaterThan(0);
  });

  it("de-trigger sonrası yeniden silahlanır (chattering önler)", () => {
    let st = initStalta();
    for (let i = 0; i < STALTA_PARAMS.warmupSamples + 100; i++) st = staltaStep(st, s12(9.81, 0, 0)).state;
    let r = staltaStep(st, s12(20, 0, 0));
    while (!r.fired) r = staltaStep(r.state, s12(20, 0, 0));
    expect(r.state.triggered).toBe(true);
    expect(r.state.armed).toBe(false);
    // sessiz besle → sta düşer → ratio ≤ detrigger → yeniden silahlan
    for (let i = 0; i < 1000 && r.state.triggered; i++) r = staltaStep(r.state, s12(9.81, 0, 0));
    expect(r.state.triggered).toBe(false);
    expect(r.state.armed).toBe(true);
  });
});

describe("STA/LTA golden vektör (C↔TS kilit)", () => {
  // Bilinen 5-örnek dizi → bilinen sta/lta/ratio. Değerler node'da gerçek
  // staltaStep çalıştırılarak üretildi VE bağımsız bir referans aritmetiğiyle
  // birebir doğrulandı. Firmware (sample_task) boot-assert bu üç sayıyı yeniden
  // üretecek; bu yüzden tam-precision float literal olarak kilitlenir.
  it("sabit golden dizi → sabit sta/lta/ratio", () => {
    const seq = [s12(9.81, 0, 0), s12(9.85, 0.02, 0), s12(9.80, -0.03, 0.01),
                 s12(9.90, 0.05, -0.02), s12(9.82, 0, 0)];
    let st = initStalta();
    let r = { state: st, fired: false, ratio: 0, pga: 0 };
    for (const s of seq) r = staltaStep(r.state, s);
    expect(r.state.sta).toBeCloseTo(0.0002452873445456898, 10);
    expect(r.state.lta).toBeCloseTo(0.000004237485960969144, 10);
    expect(r.ratio).toBeCloseTo(57.88511084284295, 8);
  });

  // 4 sensör FARKLI → ortalama (s0+s3+s6+s9)/4 indeks eşlemesini ve op-order'ı kilitler.
  // Üstteki golden 4 sensörü eşit verdiği için ortalama sırasına duyarsızdı (review #1).
  // Firmware golden_stalta_check bu AYNI farklı-sensör double dizisini kullanacak.
  it("golden — 4 sensör FARKLI (ortalama op-order kilidi)", () => {
    const d = [
      [9.81, 0.10, 0.20,  9.79, 0.11, 0.19,  9.83, 0.09, 0.21,  9.80, 0.12, 0.18],
      [9.85, 0.12, 0.22,  9.82, 0.10, 0.20,  9.88, 0.14, 0.24,  9.84, 0.11, 0.19],
      [9.78, 0.08, 0.18,  9.81, 0.13, 0.23,  9.79, 0.07, 0.17,  9.86, 0.15, 0.25],
    ];
    let st = initStalta();
    let r = { state: st, fired: false, ratio: 0, pga: 0 };
    for (const s of d) r = staltaStep(r.state, s);
    expect(r.state.sta).toBeCloseTo(0.00003883608158025049, 12);
    expect(r.state.lta).toBeCloseTo(6.74217320754769e-7, 12);
    expect(r.ratio).toBeCloseTo(57.60172630506509, 8);
  });
});
