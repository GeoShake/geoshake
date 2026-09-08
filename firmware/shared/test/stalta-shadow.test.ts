// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// ↙ GÖLGE DEDEKTÖR (2026-08-25, Trello #178) — canlı dedektörün yanında, düşük
// eşikli, YALNIZ KAYIT üreten ikinci STA/LTA. Parametreler 50 Hz için DOĞRU:
// STALTA_PARAMS 109 Hz T1 sabitleridir ve telefonda STA 1,1 s / LTA 65 s olur.
import { describe, expect, it } from "vitest";
import {
  STALTA_PARAMS, SHADOW_STALTA_PARAMS, initStalta, staltaStep, staltaStepWith,
  type StaltaParams,
} from "../src/stalta.js";

const HZ = 50;

describe("SHADOW_STALTA_PARAMS", () => {
  it("50 Hz için doğru pencereler: STA 0,5 s · LTA 30 s · HP 0,2 Hz", () => {
    expect(SHADOW_STALTA_PARAMS.nSta).toBe(25);          // 0,5 s × 50 Hz
    expect(SHADOW_STALTA_PARAMS.nLta).toBe(1500);        // 30 s × 50 Hz
    expect(SHADOW_STALTA_PARAMS.warmupSamples).toBe(1500);
    // TAM double, toBeCloseTo DEĞİL. Önceki hâli sabiti kendi ifadesiyle kıyaslıyordu
    // (döngüsel) ve 5e-5 toleransı 1 ULP'nin (3,47e-18) 13 mertebe üstündeydi:
    // sabit 0,02513'e kırpılsa bile bu dosyadaki testlerin hepsi geçiyordu (ölçüldü).
    // Kotlin ikizi bit-eş olmak zorunda; bu yüzden kilit TAM eşitlik.
    expect(SHADOW_STALTA_PARAMS.alphaHp).toBe(0.025132741228718346);
    expect(SHADOW_STALTA_PARAMS.alphaHp).toBe(2 * Math.PI * 0.2 / 50);  // işlem sırası
  });

  it("eşik 2,5 — ölçümle seçildi (medyan gürültüde M5,2 @ 43 km 2,7–3,0 üretir)", () => {
    expect(SHADOW_STALTA_PARAMS.trigger).toBe(2.5);
    expect(SHADOW_STALTA_PARAMS.detrigger).toBeLessThan(SHADOW_STALTA_PARAMS.trigger);
  });

  // initStalta parametresizdir ve lta tabanını STALTA_PARAMS'tan okur. Bu YALNIZCA
  // iki ltaMin eşitken doğrudur; eşitlik bozulursa gölge yanlış tabanla başlar ve
  // hiçbir test bunu göstermezdi. Kilit burada.
  it("ltaMin canlı ile AYNI — initStalta'nın parametresiz kalmasının koşulu", () => {
    expect(SHADOW_STALTA_PARAMS.ltaMin).toBe(STALTA_PARAMS.ltaMin);
    expect(initStalta().lta).toBe(SHADOW_STALTA_PARAMS.ltaMin);
  });

  it("canlı parametreler DEĞİŞMEDİ (canlı yol dokunulmaz)", () => {
    expect(STALTA_PARAMS.nSta).toBe(55);
    expect(STALTA_PARAMS.trigger).toBe(5);
    // 0.9.27: T1 firmware 4,0 + LTA dondurmaya gecti ama TELEFON yolu bu setten okur ve
    // bilerek dokunulmadi — stalta-t1-freeze.test.ts T1 setini ayrica kilitler.
    expect(STALTA_PARAMS.ltaFreeze).toBe(0);
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// Deterministik sahne: LCG gürültüsü (Math.random YOK → tekrarlanabilir, fixture'a
// çevrilebilir). Telefonun tek IMU'su 12 sayılık sözleşmeyi doldurmak için 4 kez
// tekrarlanır — mevcut stalta.test.ts'teki s12 deseniyle aynı.
// ─────────────────────────────────────────────────────────────────────────────
const lcg = (seed: number) => {
  let x = seed;
  return () => { x = (x * 1103515245 + 12345) & 0x7fffffff; return (x / 0x7fffffff) * 2 - 1; };
};

interface Scene { noise: number; pulseAt: number; amp: number; dur: number }

/** SAHNE: sessiz gürültü + t=pulseAt'ta 3 Hz'lik, Gauss zarflı darbe (m/s²). */
const sceneSample = (i: number, rnd: () => number, sc: Scene): number[] => {
  const t = i / HZ;
  const q = t >= sc.pulseAt && t < sc.pulseAt + 8
    ? sc.amp * Math.exp(-(((t - sc.pulseAt - 3) / 2) ** 2)) * Math.sin(2 * Math.PI * 3 * t)
    : 0;
  const axis = [0.1 + q + rnd() * sc.noise, -0.05 + q + rnd() * sc.noise, 9.81 + q * 0.3 + rnd() * sc.noise];
  return [...axis, ...axis, ...axis, ...axis];
};

/** Sahneyi P ile koştur; ilk tetik anını (s) ve son durumu döndür. */
function run(P: StaltaParams, sc: Scene) {
  const rnd = lcg(42);
  let st = initStalta();
  let firedAt = -1, fires = 0, ratio = 0;
  for (let i = 0; i < HZ * sc.dur; i++) {
    const r = staltaStepWith(st, sceneSample(i, rnd, sc), P);
    st = r.state;
    ratio = r.ratio;
    if (r.fired) { fires++; if (firedAt < 0) firedAt = t0(i); }
  }
  return { firedAt, fires, ratio, sta: st.sta, lta: st.lta, triggered: st.triggered, armed: st.armed };
}
const t0 = (i: number) => i / HZ;

/** Darbe her iki warmup'tan SONRA (canlı 65,4 s) + gerçekçi gürültü. */
const SAHNE: Scene = { noise: 0.0207, pulseAt: 100, amp: 0.15, dur: 130 };

describe("staltaStepWith — parametreli adım", () => {
  it("varsayılan parametrelerle staltaStep ile BİREBİR aynı sonucu verir", () => {
    // Sahne TETİKLİ olmalı. İlk sürüm 4000 sessiz örnek kullanıyordu ve `fired`
    // 4000 kez false===false karşılaştırılıyordu (ölçüldü: 0 tetik) — tetik ve
    // histerezis dalları hiç yürütülmüyordu, yani "birebir aynı" iddiası o
    // dallar için kanıtsızdı. Bu sahne canlıda 101,28 s'de tetikler.
    const rnd = lcg(42);
    let a = initStalta(), b = initStalta(), fires = 0;
    for (let i = 0; i < HZ * SAHNE.dur; i++) {
      const s = sceneSample(i, rnd, SAHNE);
      const ra = staltaStep(a, s); const rb = staltaStepWith(b, s, STALTA_PARAMS);
      a = ra.state; b = rb.state;
      expect(rb.ratio).toBe(ra.ratio);
      expect(rb.fired).toBe(ra.fired);
      expect(rb.pga).toBe(ra.pga);
      expect(rb.state).toEqual(ra.state);
      if (ra.fired) fires++;
    }
    expect(fires).toBeGreaterThan(0);  // tetik dalı gerçekten yürüdü
  });

  // MUTASYON KİLİDİ. Gövdede tek bir `P.x` yerine `STALTA_PARAMS.x` yazılırsa
  // gölge sessizce canlının sabitiyle koşar ve HİÇBİR davranış testi bunu
  // göstermeyebilir (ölçüldü: `P.nSta` → `STALTA_PARAMS.nSta` mutasyonu, darbe
  // canlının warmup'ı içindeyken hayatta kaldı). Burada her alan tek tek,
  // sonucu kesin değiştirecek bir değerle ezilir; fark yoksa alan okunmuyordur.
  it("P'nin HER alanı gerçekten okunuyor (tek tek ezilir, sonuç değişmeli)", () => {
    const sig = (r: ReturnType<typeof run>) => JSON.stringify(r);
    const taban = sig(run(SHADOW_STALTA_PARAMS, SAHNE));

    const ezmeler: Array<[keyof StaltaParams, number]> = [
      ["alphaHp", STALTA_PARAMS.alphaHp],   // 0,0115 — farklı HP köşesi, farklı hp/cf
      ["nSta", STALTA_PARAMS.nSta],         // 55 — canlının 1,1 s penceresi
      ["nLta", STALTA_PARAMS.nLta],         // 3270 — canlının 65 s penceresi
      ["ltaMin", 1e-2],                     // taban yukarı çekilir → oran ezilir
      ["trigger", 100],                     // erişilemez eşik → tetik yok
      ["detrigger", 0],                     // hiç de-trigger olmaz → LTA donuk kalır
      ["warmupSamples", HZ * 1000],         // sahneden uzun → tetik bastırılır
      ["ltaFreeze", 1.05],                  // 0 (kapalı) → gürültüde bile sık donar, LTA/oran değişir
    ];

    // KAPSAM: listeye yazılmamış bir alan sessizce kilitsiz kalmasın. Parametre
    // setine yeni alan eklenirse bu satır kırılır ve kilit yazmaya zorlar.
    expect(ezmeler.map(([k]) => k).sort()).toEqual(Object.keys(SHADOW_STALTA_PARAMS).sort());

    for (const [alan, deger] of ezmeler) {
      expect(
        sig(run({ ...SHADOW_STALTA_PARAMS, [alan]: deger }, SAHNE)),
        `P.${alan} değiştirildi ama sonuç AYNI kaldı → gövde bu alanı P'den değil ` +
        `STALTA_PARAMS'tan okuyor olmalı (mutasyon hayatta kaldı).`,
      ).not.toBe(taban);
    }
  });

  // NOT: darbe BİLEREK her iki warmup'tan SONRA (canlı 65,4 s, gölge 30 s). Erken
  // konursa test STA penceresini değil warmup farkını ölçer — ilk sürümü öyleydi ve
  // nSta mutasyonu hayatta kaldı. Gürültü de şart: gürültüsüzken LTA 1e-8 tabanına
  // yapışır, oran patlar ve iki dedektör aynı örnekte tetikler (ayrım kalmaz).
  it("gerçek gürültüde gölge aynı darbeyi canlıdan ÖNCE görür", () => {
    const live = run(STALTA_PARAMS, SAHNE);
    const shadow = run(SHADOW_STALTA_PARAMS, SAHNE);

    expect(shadow.firedAt).toBeGreaterThan(0);
    expect(live.firedAt).toBeGreaterThan(0);
    expect(shadow.firedAt).toBeLessThan(live.firedAt);

    // 0,5 s'lik STA, 1,1 s'lik olandan hızlı yükselir: aynı sahnede gölgenin
    // parametresini canlının nSta'sıyla ezmek tetiği GECİKTİRİR.
    const yavas = run({ ...SHADOW_STALTA_PARAMS, nSta: STALTA_PARAMS.nSta }, SAHNE);
    expect(yavas.firedAt).toBeGreaterThan(shadow.firedAt);
  });

  // Gölge warmup'ı 30 s, canlınınki 65,4 s (3270 örnek @ 50 Hz). İlk 65 s'de gelen
  // bir deprem canlı yolda ZATEN bastırılır; gölge o pencerede kayıt üretebilir.
  it("gölge ilk 65 s'de uygundur, canlı değil (warmup 30 s vs 65,4 s)", () => {
    expect(STALTA_PARAMS.warmupSamples / HZ).toBeCloseTo(65.4, 1);
    const erken: Scene = { noise: 0.0207, pulseAt: 40, amp: 0.15, dur: 60 };
    expect(run(SHADOW_STALTA_PARAMS, erken).firedAt).toBeGreaterThan(0);
    expect(run(STALTA_PARAMS, erken).firedAt).toBe(-1);
  });
});
