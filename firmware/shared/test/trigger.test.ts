// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { TriggerPacket } from "../src/schema.js";

const good = { v: 1, dev: "gs-0001", seq: 42, t_ms: 1718539200123, sta_lta: 8.4, pga: 0.62 };

describe("TriggerPacket", () => {
  it("geçerli tetik", () => { expect(TriggerPacket.parse(good)).toEqual(good); });
  it("negatif sta_lta reddedilir", () => { expect(() => TriggerPacket.parse({ ...good, sta_lta: -1 })).toThrow(); });
  it("Infinity pga reddedilir", () => { expect(() => TriggerPacket.parse({ ...good, pga: 1e400 })).toThrow(); });
  it("kötü dev reddedilir", () => { expect(() => TriggerPacket.parse({ ...good, dev: "x!" })).toThrow(); });
});
