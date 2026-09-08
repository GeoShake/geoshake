// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { describe, it, expect } from "vitest";
import { FDSN_NETWORK_CODE, FDSN_NETWORK_REGISTERED, FDSN_NETWORK_DOI } from "../src/fdsn.js";

describe("FDSN tescil durumu", () => {
  it("ag tescilli", () => {
    expect(FDSN_NETWORK_REGISTERED).toBe(true);
  });

  it("DOI FDSN'in verdigi deger", () => {
    expect(FDSN_NETWORK_DOI).toBe("10.7914/ft1b-4x43");
  });

  /* DOI ile tescil bayragi AYRI sabitler ama birlikte anlamli: biri true digeri
     bos olamaz. Tescilsiz bir agin DOI'si olmaz. */
  it("tescil ve DOI birlikte tutarli", () => {
    expect(FDSN_NETWORK_REGISTERED).toBe(FDSN_NETWORK_DOI.length > 0);
  });

  it("kod hala GW", () => {
    expect(FDSN_NETWORK_CODE).toBe("GW");
  });
});
