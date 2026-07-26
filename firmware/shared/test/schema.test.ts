// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { FeaturePacket, HealthPacket, AssocConfig, DEFAULT_ASSOC_CONFIG } from "../src/schema.js";

const goodFeat = { v: 1, dev: "gs-0001", seq: 7, t_ms: 1718539200123, fs: 109.2, n: 109,
  rms: [0.0118, 0.0107, 0.0131], pga: 0.041 };

const goodHealth = { v: 1, dev: "gs-0001", seq: 1, t_ms: 1718539200123, up_s: 3600,
  heap: 234000, rssi: -62, wifi_rc: 0, mqtt_rc: 0, fw: "faz1-0.1.0" };

describe("FeaturePacket", () => {
  it("geçerli paketi kabul eder", () => {
    expect(FeaturePacket.parse(goodFeat)).toEqual(goodFeat);
  });
  it("rms 3 eleman değilse reddeder", () => {
    expect(() => FeaturePacket.parse({ ...goodFeat, rms: [1, 2] })).toThrow();
  });
  it("negatif fs reddeder", () => {
    expect(() => FeaturePacket.parse({ ...goodFeat, fs: -1 })).toThrow();
  });
  it("bilinmeyen device_id formatını reddeder", () => {
    expect(() => FeaturePacket.parse({ ...goodFeat, dev: "kötü id!" })).toThrow();
  });
  it("Infinity (1e400) fs reddedilir", () => {
    expect(() => FeaturePacket.parse({ ...goodFeat, fs: 1e400 })).toThrow();
  });
  it("Date sınırını aşan t_ms reddedilir", () => {
    expect(() => FeaturePacket.parse({ ...goodFeat, t_ms: 8.64e15 + 1 })).toThrow();
  });
  it("bilinmeyen alanları sessizce atar (ileri-uyumluluk)", () => {
    const out = FeaturePacket.parse({ ...goodFeat, future: 1 });
    expect("future" in out).toBe(false);
  });
});

describe("HealthPacket", () => {
  it("geçerli sağlık paketini kabul eder", () => {
    expect(HealthPacket.parse(goodHealth)).toEqual(goodHealth);
  });
  it("gain (4 float) ile geçerli — gain korunur", () => {
    const h = { ...goodHealth, gain: [1.0, 0.99, 1.01, 1.0] };
    expect(HealthPacket.parse(h)).toEqual(h);
  });
  it("gain'siz paket geçerli (opsiyonel — eski firmware)", () => {
    const out = HealthPacket.parse(goodHealth);
    expect("gain" in out).toBe(false);
  });
  it("gain 4 eleman değilse reddeder", () => {
    expect(() => HealthPacket.parse({ ...goodHealth, gain: [1, 2, 3] })).toThrow();
  });
});

const goodAssoc = { radiusKm: 50, dtMs: 15000, minDevices: 3, tolMs: 2000 };

describe("AssocConfig", () => {
  it("geçerli assoc config kabul edilir", () => {
    expect(AssocConfig.parse(goodAssoc)).toEqual(goodAssoc);
  });

  // minDevices — güvenlik kritik: <3 lokasyon under-determined (çöp konum)
  it("minDevices 2 reddedilir (min 3)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, minDevices: 2 })).toThrow();
  });
  it("minDevices 3.5 reddedilir (integer olmalı)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, minDevices: 3.5 })).toThrow();
  });

  // radiusKm — çok büyük → ilgisiz tetikleri kümeler
  it("radiusKm 600 reddedilir (max 500)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, radiusKm: 600 })).toThrow();
  });
  it("radiusKm 0 reddedilir (min 1)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, radiusKm: 0 })).toThrow();
  });

  // dtMs — küme zaman penceresi
  it("dtMs 500 reddedilir (min 1000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, dtMs: 500 })).toThrow();
  });
  it("dtMs 70000 reddedilir (max 60000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, dtMs: 70000 })).toThrow();
  });

  // tolMs — residual gate; çok büyük → gürültü sızar
  it("tolMs 50 reddedilir (min 100)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, tolMs: 50 })).toThrow();
  });
  it("tolMs 20000 reddedilir (max 10000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, tolMs: 20000 })).toThrow();
  });

  it("DEFAULT_ASSOC_CONFIG mevcut hardcoded sabitlere eşit (fail-safe fallback)", () => {
    expect(DEFAULT_ASSOC_CONFIG).toEqual({ radiusKm: 50, dtMs: 15000, minDevices: 3, tolMs: 2000 });
  });
});
