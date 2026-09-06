// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import {
  computeOrientation, classifyMount,
  TILT_FLAT_MAX_DEG, TILT_WALL_MIN_DEG, TILT_WALL_MAX_DEG,
  GRAV_MAG_MIN_MS2, GRAV_MAG_MAX_MS2,
} from "../src/orientation.js";

const G = 9.80665;

/** tiltDeg derecelik yönelime karşılık gelen yerçekimi vektörü (X-Z düzleminde). */
function vecAtTilt(deg: number, mag = G): number[] {
  const r = (deg * Math.PI) / 180;
  return [mag * Math.sin(r), 0, mag * Math.cos(r)];
}

describe("computeOrientation — temel yönelimler", () => {
  it("düz masa (Z yukarı, +1g) → 0°, duz", () => {
    const o = computeOrientation([0, 0, G])!;
    expect(o.tiltDeg).toBeCloseTo(0, 6);
    expect(o.mountClass).toBe("duz");
    expect(o.gMag).toBeCloseTo(G, 6);
  });

  it("duvar, X ekseni +1g → 90°, duvar", () => {
    const o = computeOrientation([G, 0, 0])!;
    expect(o.tiltDeg).toBeCloseTo(90, 6);
    expect(o.mountClass).toBe("duvar");
  });

  it("duvar, Y ekseni -1g → 90°, duvar (işaretten bağımsız)", () => {
    const o = computeOrientation([0, -G, 0])!;
    expect(o.tiltDeg).toBeCloseTo(90, 6);
    expect(o.mountClass).toBe("duvar");
  });

  it("45° eğik → 45°, egik", () => {
    const o = computeOrientation(vecAtTilt(45))!;
    expect(o.tiltDeg).toBeCloseTo(45, 6);
    expect(o.mountClass).toBe("egik");
  });

  it("ters düz (Z aşağı, -1g) → 180°", () => {
    const o = computeOrientation([0, 0, -G])!;
    expect(o.tiltDeg).toBeCloseTo(180, 6);
  });

  it("tiltDeg her zaman 0..180 aralığında", () => {
    for (const v of [[1, 2, 9.5], [-3, 4, -8.5], [0, 9.8, 0.1], [5, -5, -7]]) {
      const o = computeOrientation(v);
      if (!o) continue;
      expect(o.tiltDeg).toBeGreaterThanOrEqual(0);
      expect(o.tiltDeg).toBeLessThanOrEqual(180);
    }
  });

  it("yatay bileşen dikeye göre küçükken bile hassas (near-vertical acos kaybı yok)", () => {
    // gx=0.01 m/s² → atan(0.01/9.80665) ≈ 0.0584°. acos(gz/|g|) bu bölgede duyarlılığını yitirir.
    const o = computeOrientation([0.01, 0, G])!;
    expect(o.tiltDeg).toBeCloseTo(0.0584, 3);
  });
});

describe("classifyMount — eşikler", () => {
  it("eşik sabitleri sözleşmede belirtilen değerler", () => {
    expect(TILT_FLAT_MAX_DEG).toBe(20);
    expect(TILT_WALL_MIN_DEG).toBe(70);
    expect(TILT_WALL_MAX_DEG).toBe(110);
  });

  it("0-20° düz", () => {
    expect(classifyMount(0)).toBe("duz");
    expect(classifyMount(19.9)).toBe("duz");
  });

  it("20° üstü artık düz değil", () => {
    expect(classifyMount(20.1)).toBe("egik");
  });

  it("70-110° duvar", () => {
    expect(classifyMount(70.1)).toBe("duvar");
    expect(classifyMount(90)).toBe("duvar");
    expect(classifyMount(109.9)).toBe("duvar");
  });

  it("70° altı / 110° üstü duvar değil", () => {
    expect(classifyMount(69.9)).toBe("egik");
    expect(classifyMount(110.1)).toBe("egik");
  });

  it("ters düz (≈180°) literal sözleşmeye göre egik", () => {
    expect(classifyMount(180)).toBe("egik");
  });
});

describe("computeOrientation — geçersiz girdi savunması", () => {
  it("undefined / null → null (eski firmware alanı göndermez)", () => {
    expect(computeOrientation(undefined)).toBeNull();
    expect(computeOrientation(null)).toBeNull();
  });

  it("3 elemanlı değilse → null", () => {
    expect(computeOrientation([1, 2])).toBeNull();
    expect(computeOrientation([1, 2, 3, 4])).toBeNull();
    expect(computeOrientation([])).toBeNull();
  });

  it("NaN / Infinity → null", () => {
    expect(computeOrientation([NaN, 0, G])).toBeNull();
    expect(computeOrientation([0, Infinity, G])).toBeNull();
    expect(computeOrientation([0, 0, -Infinity])).toBeNull();
  });

  it("serbest düşüş / ölü sensör (|g|≈0) → null", () => {
    expect(computeOrientation([0, 0, 0])).toBeNull();
  });

  it("|g| geçerlilik penceresi dışı → null", () => {
    expect(computeOrientation([0, 0, GRAV_MAG_MIN_MS2 - 0.1])).toBeNull();
    expect(computeOrientation([0, 0, GRAV_MAG_MAX_MS2 + 0.1])).toBeNull();
    expect(GRAV_MAG_MIN_MS2).toBeLessThan(9.78);   // dünya g alt sınırını dışlamamalı
    expect(GRAV_MAG_MAX_MS2).toBeGreaterThan(9.83); // dünya g üst sınırını dışlamamalı
  });

  it("|g| penceresi sınırında kabul edilir", () => {
    expect(computeOrientation([0, 0, GRAV_MAG_MIN_MS2])).not.toBeNull();
    expect(computeOrientation([0, 0, GRAV_MAG_MAX_MS2])).not.toBeNull();
  });

  it("bir sensör ölü olsaydı üretilecek ~0.75g mean reddedilir (bozuk cihaz yönelim raporlamaz)", () => {
    expect(computeOrientation([0, 0, G * 0.75])).toBeNull();
  });
});
