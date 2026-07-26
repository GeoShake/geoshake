// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { StationDetail, EventDetail } from "../src/schema.js";

const st = { dev: "gs-sim001", lat: 41.02, lon: 28.98, tMs: 1718539200500, staLta: 8.4, pga: 0.12, distKm: 2.8, delayMs: 466 };
const ev = { v: 1, event_id: 8, originMs: 1718539200000, lat: 41, lon: 29, nStations: 3, intensityClass: "orta", maxPga: 0.18, rmsResidual: 0.2 };

describe("StationDetail", () => {
  it("geçerli istasyon detayı", () => { expect(StationDetail.parse(st)).toEqual(st); });
  it("negatif delayMs kabul (origin sonrası değil de önce olabilir)", () => {
    expect(StationDetail.parse({ ...st, delayMs: -10 }).delayMs).toBe(-10);
  });
  it("negatif distKm reddedilir", () => { expect(() => StationDetail.parse({ ...st, distKm: -1 })).toThrow(); });
});
describe("EventDetail", () => {
  it("olay + istasyonlar", () => { expect(EventDetail.parse({ event: ev, stations: [st] })).toEqual({ event: ev, stations: [st] }); });
  it("boş istasyon listesi geçerli", () => { expect(EventDetail.parse({ event: ev, stations: [] }).stations).toEqual([]); });
});
