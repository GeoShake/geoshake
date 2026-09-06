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
  FeaturePacket, HealthPacket, AssocConfig, AssocConfigStrict, DEFAULT_ASSOC_CONFIG,
  resolveAssocConfig, effectiveClusterRadiusKm, maxClusterRadiusKm, ASSOC_DT_MS_MAX,
} from "../src/schema.js";
import { VP_KM_S } from "../src/geo.js";

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

  // grav = yönelim (yerçekimi DC vektörü). gain ile AYNI gerekçe: opsiyonel — sahadaki
  // eski firmware göndermez, zorunlu olursa o cihazlar sağlık telemetrisinden düşer.
  it("grav (3 float) ile geçerli — grav korunur", () => {
    const h = { ...goodHealth, grav: [0.03, -0.02, 9.8] };
    expect(HealthPacket.parse(h)).toEqual(h);
  });
  it("grav'sız paket geçerli (opsiyonel — eski firmware)", () => {
    expect("grav" in HealthPacket.parse(goodHealth)).toBe(false);
  });
  it("grav NEGATİF bileşen kabul eder (ters/duvar montaj)", () => {
    const h = { ...goodHealth, grav: [0, -9.8, 0.2] };
    expect(HealthPacket.parse(h)).toEqual(h);
  });
  it("grav 3 eleman değilse reddeder", () => {
    expect(() => HealthPacket.parse({ ...goodHealth, grav: [1, 2] })).toThrow();
  });

  // Şema versiyonu: yeni alan EKLEMEK v'yi artırmaz. v:2 yayınlanırsa sahadaki tüm
  // tüketiciler (z.literal(1)) paketleri reddeder → toplu sessiz veri kaybı.
  it("v yalnız 1 kabul eder — v bump'ı breaking'dir (opsiyonel alan bump gerektirmez)", () => {
    expect(() => HealthPacket.parse({ ...goodHealth, v: 2 })).toThrow();
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
  // dt tavanı motorun bellek penceresine bağlandı: 60000 → ASSOC_DT_MS_MAX (30000).
  it("dtMs 70000 reddedilir (max ASSOC_DT_MS_MAX)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, dtMs: 70000 })).toThrow();
  });
  it("dtMs 60000 artık reddedilir (eski tavan; motor 30 s'den eskisini bellekte tutmuyor)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, dtMs: 60000 })).toThrow();
  });
  it("dtMs tam ASSOC_DT_MS_MAX kabul edilir (sınır dahil)", () => {
    expect(AssocConfig.parse({ ...goodAssoc, dtMs: ASSOC_DT_MS_MAX }).dtMs).toBe(ASSOC_DT_MS_MAX);
  });

  // tolMs — residual gate; çok büyük → gürültü sızar
  it("tolMs 50 reddedilir (min 100)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, tolMs: 50 })).toThrow();
  });
  it("tolMs 20000 reddedilir (max 10000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, tolMs: 20000 })).toThrow();
  });

  it("DEFAULT_ASSOC_CONFIG canlı yapılandırmayla hizalı (fail-safe fallback gerçek bir config'tir)", () => {
    expect(DEFAULT_ASSOC_CONFIG).toEqual({
      // legacy radiusKm 90: eski kod onu HEM cluster HEM event için kullanır → kurala uyan değer
      // seçilir; event tarafında 10 km eksik erişim zararsız, kural ihlali değil (bkz. schema.ts).
      radiusKm: 90, dtMs: 15000, minDevices: 3, tolMs: 2000,
      clusterRadiusKm: 90, eventRadiusKm: 100, residualMaxMs: 2000, backfillTolMs: 2000,
      coincidenceMinFraction: 0.6,
    });
  });

  // ── ORAN KAPISI EŞİĞİ ──
  // Cross-field kural YOK (strict'e ek gerekmez): eşik hiçbir başka alanla fiziksel ilişki
  // kurmuyor, yalnız kendi aralığıyla sınırlı.
  it("coincidenceMinFraction 0.3 ve 1 kabul (kapsayıcı), 0.29 ve 1.01 ret", () => {
    expect(AssocConfig.parse({ ...goodAssoc, coincidenceMinFraction: 0.3 }).coincidenceMinFraction).toBe(0.3);
    expect(AssocConfig.parse({ ...goodAssoc, coincidenceMinFraction: 1 }).coincidenceMinFraction).toBe(1);
    expect(() => AssocConfig.parse({ ...goodAssoc, coincidenceMinFraction: 0.29 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, coincidenceMinFraction: 1.01 })).toThrow();
  });

  it("eşik opsiyonel: alanı olmayan eski config nesnesi hâlâ parse edilir", () => {
    expect(() => AssocConfig.parse(goodAssoc)).not.toThrow();
  });

  it("strict şema eşiği ENGELLEMEZ (kural yalnız cluster yarıçapına dair)", () => {
    expect(() => AssocConfigStrict.parse({
      ...goodAssoc, clusterRadiusKm: 90, dtMs: 15000, coincidenceMinFraction: 1,
    })).not.toThrow();
  });

  // ── dörtlü ayrıştırma (cluster/event yarıçapı + residual/backfill toleransı) ──
  // Yeni alanlar .optional(): eski 4 alanlı nesneler parse EDİLMELİ (migrasyon öncesi config,
  // eski panel POST'u). Kırılırsa assoc DEFAULT'a düşer = sessiz ayar kaybı.
  it("yeni alanlar olmadan (eski 4 alanlı nesne) parse edilir", () => {
    expect(AssocConfig.parse(goodAssoc)).toEqual(goodAssoc);
  });

  it("dörtlü ile birlikte parse edilir ve değerler korunur", () => {
    const split = { ...goodAssoc, clusterRadiusKm: 100, eventRadiusKm: 120, residualMaxMs: 1800, backfillTolMs: 3000 };
    expect(AssocConfig.parse(split)).toEqual(split);
  });

  // Sınırlar eski alanlarla AYNI: yarıçap 1..500, ms 100..10000 (aksi halde panelde
  // eski alan reddedilirken yeni alan geçerdi → tutarsız kapı).
  it("clusterRadiusKm 600 / 0 reddedilir (1..500)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, clusterRadiusKm: 600 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, clusterRadiusKm: 0 })).toThrow();
  });
  it("eventRadiusKm 600 / 0 reddedilir (1..500)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, eventRadiusKm: 600 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, eventRadiusKm: 0 })).toThrow();
  });
  it("residualMaxMs 50 / 20000 / ondalık reddedilir (int 100..10000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, residualMaxMs: 50 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, residualMaxMs: 20000 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, residualMaxMs: 1500.5 })).toThrow();
  });
  it("backfillTolMs 50 / 20000 / ondalık reddedilir (int 100..10000)", () => {
    expect(() => AssocConfig.parse({ ...goodAssoc, backfillTolMs: 50 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, backfillTolMs: 20000 })).toThrow();
    expect(() => AssocConfig.parse({ ...goodAssoc, backfillTolMs: 1500.5 })).toThrow();
  });
});

describe("resolveAssocConfig (geri uyum: eksik dörtlü eski ikiliden türetilir)", () => {
  it("yeni alan YOKKEN radiusKm→her iki yarıçap, tolMs→her iki tolerans", () => {
    expect(resolveAssocConfig(goodAssoc)).toEqual({
      dtMs: 15000, minDevices: 3,
      clusterRadiusKm: 50, eventRadiusKm: 50, residualMaxMs: 2000, backfillTolMs: 2000,
      // Oran eşiğinin ESKİ karşılığı yok (ayrıştırmayla değil yeni kapıyla geldi) → sabit varsayılan.
      coincidenceMinFraction: DEFAULT_ASSOC_CONFIG.coincidenceMinFraction,
    });
  });

  it("eşik verilmişse korunur, verilmemişse varsayılana düşer", () => {
    expect(resolveAssocConfig({ ...goodAssoc, coincidenceMinFraction: 0.8 }).coincidenceMinFraction).toBe(0.8);
    expect(resolveAssocConfig(goodAssoc).coincidenceMinFraction).toBe(0.6);
  });

  it("yeni alan VARKEN eski ikili YOK SAYILIR (yeni değerler kazanır)", () => {
    expect(resolveAssocConfig({
      radiusKm: 50, dtMs: 15000, minDevices: 3, tolMs: 2000,
      clusterRadiusKm: 100, eventRadiusKm: 250, residualMaxMs: 1200, backfillTolMs: 4000,
    })).toEqual({
      dtMs: 15000, minDevices: 3,
      clusterRadiusKm: 100, eventRadiusKm: 250, residualMaxMs: 1200, backfillTolMs: 4000,
      coincidenceMinFraction: 0.6,
    });
  });

  it("kısmi dörtlü: yalnız verilen alan kazanır, kalanı eskiden türer", () => {
    expect(resolveAssocConfig({ ...goodAssoc, eventRadiusKm: 100 })).toEqual({
      dtMs: 15000, minDevices: 3,
      clusterRadiusKm: 50, eventRadiusKm: 100, residualMaxMs: 2000, backfillTolMs: 2000,
      coincidenceMinFraction: 0.6,
    });
  });
});

// ── FİZİK KURALI: cluster yarıçapı ≤ dt × Vp ──────────────────────────────────────────────
// Aşıldığında tespit yöne bağlı olur (episantrın iki yanındaki çift kümelenir, aynı doğrultudaki
// çift pencereye sığmaz) — config bir menzil vaat eder, teslimat şansa kalır.
describe("maxClusterRadiusKm (dt penceresinde P dalgasının kat ettiği mesafe)", () => {
  it("dt=15 s → 90 km, dt=20 s → 120 km (kural dt ile ölçeklenir)", () => {
    expect(maxClusterRadiusKm(15000)).toBe(90);
    expect(maxClusterRadiusKm(20000)).toBe(120);
  });
  it("shared VP_KM_S'ten türer (elle sabit değil)", () => {
    expect(maxClusterRadiusKm(15000)).toBe((15000 / 1000) * VP_KM_S);
  });
});

describe("effectiveClusterRadiusKm (yeni alan varsa o, yoksa legacy)", () => {
  it("yeni alan kazanır, legacy yok sayılır", () => {
    expect(effectiveClusterRadiusKm({ ...goodAssoc, radiusKm: 100, clusterRadiusKm: 90 })).toBe(90);
  });
  it("yeni alan yoksa legacy'ye düşer", () => {
    expect(effectiveClusterRadiusKm({ ...goodAssoc, radiusKm: 100 })).toBe(100);
  });
});

describe("AssocConfigStrict (YALNIZ yazım yolu — taban şema kuralsız kalır)", () => {
  const at = (dtMs: number, clusterRadiusKm: number) => ({ ...goodAssoc, dtMs, clusterRadiusKm });

  it("dt=15 s: 90 km kabul (eşitlik dahil), 90.01 ret", () => {
    expect(AssocConfigStrict.parse(at(15000, 90)).clusterRadiusKm).toBe(90);
    expect(() => AssocConfigStrict.parse(at(15000, 90.01))).toThrow();
  });

  it("dt=20 s: 120 km kabul, 120.01 ret (tavan pencereyle büyür)", () => {
    expect(AssocConfigStrict.parse(at(20000, 120)).clusterRadiusKm).toBe(120);
    expect(() => AssocConfigStrict.parse(at(20000, 120.01))).toThrow();
  });

  // ETKİN alan semantiği: canlı satırda legacy radius_km ESKİ değerinde kalır (rollback için).
  // Kural legacy'ye bakarsa geçerli bir yapılandırma yanlışlıkla reddedilir — canlı tam bu şekilde.
  it("{cluster:90, legacy:100} GEÇER — kural etkin alana bakar, legacy'ye değil", () => {
    expect(() =>
      AssocConfigStrict.parse({ radiusKm: 100, dtMs: 15000, minDevices: 3, tolMs: 2000, clusterRadiusKm: 90 }),
    ).not.toThrow();
  });

  // İKİ ŞEMA'nın varlık sebebi: aynı nesne yazımda reddedilir, okumada kabul edilir.
  // Taban da reddetseydi loadConfig canlı satırı parse edemez, DEFAULT'a düşer ve yarıçap
  // kimseye sorulmadan değişirdi.
  it("legacy-only {radiusKm:100, dt=15 s}: strict REDDEDER, taban KABUL EDER", () => {
    const legacyOnly = { radiusKm: 100, dtMs: 15000, minDevices: 3, tolMs: 2000 };
    expect(() => AssocConfigStrict.parse(legacyOnly)).toThrow();
    expect(AssocConfig.parse(legacyOnly).radiusKm).toBe(100);
  });

  it("hata mesajı hesaplanmış tavanı ve düzeltmeyi içerir (kullanıcı ne yapacağını bilsin)", () => {
    const r = AssocConfigStrict.safeParse(at(15000, 100));
    expect(r.success).toBe(false);
    if (!r.success) {
      const issue = r.error.issues.find((i) => i.path[0] === "clusterRadiusKm");
      expect(issue?.message).toContain("90");
      expect(issue?.message).toContain("15");
    }
  });

  it("DEFAULT_ASSOC_CONFIG kurala uyar (defaultObeysRule — varsayılanı bozan burada yakalanır)", () => {
    expect(() => AssocConfigStrict.parse(DEFAULT_ASSOC_CONFIG)).not.toThrow();
    expect(effectiveClusterRadiusKm(DEFAULT_ASSOC_CONFIG))
      .toBeLessThanOrEqual(maxClusterRadiusKm(DEFAULT_ASSOC_CONFIG.dtMs));
  });

  it("DEFAULT'un legacy radiusKm'i de kurala uyar (rollback'te eski kod onu cluster sanır)", () => {
    expect(DEFAULT_ASSOC_CONFIG.radiusKm)
      .toBeLessThanOrEqual(maxClusterRadiusKm(DEFAULT_ASSOC_CONFIG.dtMs));
  });
});
