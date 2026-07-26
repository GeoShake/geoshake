// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { haversineKm } from "../src/geo.js";

describe("haversineKm", () => {
  it("aynı nokta 0", () => { expect(haversineKm(41, 29, 41, 29)).toBe(0); });
  it("İstanbul-Ankara ~350 km", () => {
    const d = haversineKm(41.01, 28.98, 39.93, 32.86);
    expect(d).toBeGreaterThan(330); expect(d).toBeLessThan(370);
  });
  it("simetrik", () => {
    expect(haversineKm(41, 29, 40, 30)).toBeCloseTo(haversineKm(40, 30, 41, 29), 9);
  });
});
