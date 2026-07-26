// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { EventPacket, StationInfo } from "../src/schema.js";

const ev = { v: 1, event_id: 8, originMs: 1718539200000, lat: 41.0, lon: 29.0,
  nStations: 3, intensityClass: "orta", maxPga: 0.18, rmsResidual: 0.2 };

describe("EventPacket", () => {
  it("geçerli olay", () => { expect(EventPacket.parse(ev)).toEqual(ev); });
  it("bilinmeyen şiddet sınıfı reddedilir", () => {
    expect(() => EventPacket.parse({ ...ev, intensityClass: "büyük" })).toThrow();
  });
  it("negatif n_stations reddedilir", () => {
    expect(() => EventPacket.parse({ ...ev, nStations: 0 })).toThrow();
  });
  it("Infinity lat reddedilir", () => {
    expect(() => EventPacket.parse({ ...ev, lat: 1e400 })).toThrow();
  });
});

describe("StationInfo", () => {
  it("geçerli istasyon", () => {
    expect(StationInfo.parse({ dev: "gs-0001", lat: 41, lon: 29 })).toEqual({ dev: "gs-0001", lat: 41, lon: 29 });
  });
  it("kötü dev (tire) reddedilir", () => {
    expect(() => StationInfo.parse({ dev: "gs-00-01", lat: 41, lon: 29 })).toThrow();
  });
});
