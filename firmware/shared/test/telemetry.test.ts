// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { TelemetrySample } from "../src/schema.js";

describe("TelemetrySample", () => {
  it("geçerli örnek parse olur", () => {
    expect(TelemetrySample.parse({ tMs: 1718539200000, rms: [0.01, 0.02, 0.03], pga: 0.05 }))
      .toEqual({ tMs: 1718539200000, rms: [0.01, 0.02, 0.03], pga: 0.05 });
  });
  it("rms 3 değilse reddeder", () => {
    expect(() => TelemetrySample.parse({ tMs: 1, rms: [0, 0], pga: 0 })).toThrow();
  });
  it("negatif pga reddeder", () => {
    expect(() => TelemetrySample.parse({ tMs: 1, rms: [0, 0, 0], pga: -1 })).toThrow();
  });
});
