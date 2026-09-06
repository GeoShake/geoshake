// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { StationProfile } from "../src/schema.js";

const base = { dev: "gs-0001", lat: 41, lon: 29, triggers: [], events: [] };

describe("StationProfile", () => {
  it("health'siz (sim) geçerli", () => {
    expect(StationProfile.parse({ ...base, dev: "gs-sim001" }).health).toBeUndefined();
  });
  it("health'li (gerçek cihaz) korunur", () => {
    const p = StationProfile.parse({ ...base, health: { rssi: -69, uptimeS: 8434, freeHeap: 180772, fwVersion: "faz3c-0.4.0", lastHealthMs: 1718539200000 } });
    expect(p.health?.rssi).toBe(-69);
    expect(p.health?.fwVersion).toBe("faz3c-0.4.0");
  });
  it("triggers + opsiyonel eventId", () => {
    const p = StationProfile.parse({ ...base, triggers: [
      { seq: 5, tMs: 1718539202000, staLta: 12.6, pga: 0.4, eventId: 8 },
      { seq: 6, tMs: 1718539203000, staLta: 7.1, pga: 0.1 } ] });
    expect(p.triggers).toHaveLength(2);
    expect(p.triggers[0].eventId).toBe(8);
    expect(p.triggers[1].eventId).toBeUndefined();
  });
  it("events", () => {
    const p = StationProfile.parse({ ...base, events: [{ eventId: 8, originMs: 1718539200000, intensityClass: "orta" }] });
    expect(p.events[0].intensityClass).toBe("orta");
  });

  // YÖNELİM (#91-A). .optional() SAHA-KRİTİK: alanı göndermeyen eski firmware'lerin
  // istasyon sayfası aksi halde .parse'ta 500 verirdi (fail-loud buildStationProfile).
  describe("health.tiltDeg / health.mountClass", () => {
    const h = { rssi: -69, uptimeS: 8434, freeHeap: 180772, fwVersion: "1.0.0", lastHealthMs: 1718539200000 };

    it("yönelimsiz health geçerli (alanlar undefined)", () => {
      const p = StationProfile.parse({ ...base, health: h });
      expect(p.health?.mountClass).toBeUndefined();
      expect(p.health?.tiltDeg).toBeUndefined();
    });
    it("yönelimli health korunur", () => {
      const p = StationProfile.parse({ ...base, health: { ...h, tiltDeg: 88.4, mountClass: "duvar" } });
      expect(p.health?.tiltDeg).toBeCloseTo(88.4, 6);
      expect(p.health?.mountClass).toBe("duvar");
    });
    it("üç sınıf da kabul edilir; başka değer reddedilir", () => {
      for (const m of ["duz", "duvar", "egik"]) {
        expect(StationProfile.parse({ ...base, health: { ...h, mountClass: m } }).health?.mountClass).toBe(m);
      }
      expect(() => StationProfile.parse({ ...base, health: { ...h, mountClass: "flat" } })).toThrow();
    });
    it("tiltDeg sözleşme aralığı 0..180 dışında reddedilir", () => {
      expect(() => StationProfile.parse({ ...base, health: { ...h, tiltDeg: 181 } })).toThrow();
      expect(() => StationProfile.parse({ ...base, health: { ...h, tiltDeg: -1 } })).toThrow();
    });

    // grav: 3B duruş çiziminin yatma YÖNÜ kaynağı. FeaturePacket/HealthPacket ile AYNI
    // gravVector şeması — bileşenler İŞARETLİDİR (duvar/ters montajda negatif).
    it("grav'sız health geçerli (alan undefined — eski firmware)", () => {
      expect(StationProfile.parse({ ...base, health: h }).health?.grav).toBeUndefined();
    });
    it("grav korunur; negatif bileşen kabul edilir", () => {
      const p = StationProfile.parse({ ...base, health: { ...h, grav: [-9.79, 0.2, -0.4] } });
      expect(p.health?.grav).toEqual([-9.79, 0.2, -0.4]);
    });
    it("uzunluğu 3 olmayan / sonlu olmayan grav reddedilir", () => {
      expect(() => StationProfile.parse({ ...base, health: { ...h, grav: [1, 2] } })).toThrow();
      expect(() => StationProfile.parse({ ...base, health: { ...h, grav: [1, 2, Infinity] } })).toThrow();
    });
  });
});
