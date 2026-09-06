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

// grav = 4-sensör ortalanmış yerçekimi (DC) vektörü, m/s². OPSİYONEL: sahadaki eski
// firmware göndermez; zorunlu yapılırsa o cihazların TÜM tetikleri reddedilir (assoc'a
// hiç tetik düşmez → deprem asosiyasyonu durur). Bu yüzden .optional() KALDIRMA.
describe("TriggerPacket.grav (yönelim)", () => {
  it("grav'lı tetik geçerli — değer korunur", () => {
    const t = { ...good, grav: [0.02, -0.01, 9.79] };
    expect(TriggerPacket.parse(t)).toEqual(t);
  });
  it("grav'sız tetik geçerli (eski firmware — KRİTİK geri uyumluluk)", () => {
    expect("grav" in TriggerPacket.parse(good)).toBe(false);
  });
  it("NEGATİF bileşen kabul edilir (ters/duvar montaj)", () => {
    const t = { ...good, grav: [-9.79, 0.1, -0.05] };
    expect(TriggerPacket.parse(t)).toEqual(t);
  });
  it("3 eleman değilse reddedilir", () => {
    expect(() => TriggerPacket.parse({ ...good, grav: [1, 2] })).toThrow();
    expect(() => TriggerPacket.parse({ ...good, grav: [1, 2, 3, 4] })).toThrow();
  });
  it("Infinity bileşen reddedilir", () => {
    expect(() => TriggerPacket.parse({ ...good, grav: [1e400, 0, 9.8] })).toThrow();
  });
});
