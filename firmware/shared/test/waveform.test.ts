// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { Waveform, StationDetail } from "../src/schema.js";

const wf = { t0Ms: 1718539200000, fs: 109.5, samples: [0.01, 0.2, -0.15, 0.3] };
const stBase = { dev: "gs-0001", lat: 41, lon: 29, tMs: 1718539202000, staLta: 8, pga: 0.3, distKm: 0, delayMs: 0 };

describe("Waveform", () => {
  it("geçerli dalga formu", () => { expect(Waveform.parse(wf)).toEqual(wf); });
  it("boş örnek dizisi geçerli", () => { expect(Waveform.parse({ ...wf, samples: [] }).samples).toEqual([]); });
  it("fs pozitif olmalı", () => { expect(() => Waveform.parse({ ...wf, fs: 0 })).toThrow(); });

  // BOŞLUK SÖZLEŞMESİ (kurucu kararı 2026-08-05): eksik örnek null olarak taşınır — 0'a
  // çevrilmez. Şema null'ı KABUL eder, NaN/±Infinity'yi ETMEZ (eşleyici onları null'a çevirir).
  it("null örnek geçerli (kayıt boşluğu — 'ölçüm sıfır' değil)", () => {
    expect(Waveform.parse({ ...wf, samples: [0.01, null, -0.15] }).samples).toEqual([0.01, null, -0.15]);
  });
  it("tümü null geçerli (hiç örnek kaydedilememiş pencere)", () => {
    expect(Waveform.parse({ ...wf, samples: [null, null] }).samples).toEqual([null, null]);
  });
  it("NaN/Infinity örnek reddedilir", () => {
    expect(() => Waveform.parse({ ...wf, samples: [NaN] })).toThrow();
    expect(() => Waveform.parse({ ...wf, samples: [Infinity] })).toThrow();
  });
  it("örnek metin olamaz (tip gevşemesi null İLE SINIRLI)", () => {
    expect(() => Waveform.parse({ ...wf, samples: ["0.01"] })).toThrow();
  });
});
describe("StationDetail.waveform opsiyonel", () => {
  it("waveform'suz geçerli (sim istasyon)", () => { expect(StationDetail.parse(stBase).waveform).toBeUndefined(); });
  it("waveform'lu geçerli (gerçek cihaz)", () => {
    expect(StationDetail.parse({ ...stBase, waveform: wf }).waveform).toEqual(wf);
  });
});
