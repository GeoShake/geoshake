// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { StationInfo } from "../src/schema.js";

describe("StationInfo lastHealthMs", () => {
  it("lastHealthMs'siz geçerli", () => {
    expect(StationInfo.parse({ dev: "gs-sim001", lat: 41, lon: 29 }).lastHealthMs).toBeUndefined();
  });
  it("lastHealthMs varsa korunur", () => {
    expect(StationInfo.parse({ dev: "gs-0001", lat: 41, lon: 29, lastHealthMs: 1718539200000 }).lastHealthMs).toBe(1718539200000);
  });
});
