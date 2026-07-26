// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// =============================================================================
// Cihaz komut JSON sözleşmesi (sci / ota / cal / reset-wifi / reset-id) — TEK KAYNAK.
// Firmware (firmware/main/main.c) bu JSON'u cJSON ile parse eder; anahtar adları/biçimi
// değişirse cihaz komutu kaçırır. Hem admin paneli (tüm komutlar) hem web müşteri
// endpoint'i (yalnız cal + reset-wifi) buradan tüketir → sözleşme kayması imkânsız.
//
// Komut topic'i: geoshake/<dev>/cmd (firmware abone). Payload bir JSON STRING'dir:
//   {cmd:"sci",secs}             → secs: N saniye / 0 süresiz / -1 durdur (Science örnekleme)
//   {cmd:"ota",url}              → url: http(s):// firmware ikilisi (OTA güncelleme)
//   {cmd:"cal","action":"auto"}  → otomatik kalibrasyon (parametresiz)
//   {cmd:"reset-wifi"}           → WiFi kimliğini sıfırla (parametresiz; MQTT korunur → captive)
//   {cmd:"reset-id"}             → cihaz kimliğini sıfırla (parametresiz; TAM fabrika, USB gerekir)
// =============================================================================
import { z } from "zod";

/**
 * sci komutu JSON'unu kurar: {"cmd":"sci","secs":<secs>}.
 * secs TAM SAYI olmalı (N saniye / 0 süresiz / -1 durdur); değilse Error (publish'e gitmez).
 */
export function buildSciCommand(secs: number): string {
  if (!Number.isInteger(secs)) throw new Error("secs tam sayı olmalı");
  return JSON.stringify({ cmd: "sci", secs });
}

/**
 * ota komutu JSON'unu kurar: {"cmd":"ota","url":"<url>"}.
 * url http(s):// ile başlamalı (`^https?:\/\/.+`); değilse Error (publish'e gitmez).
 */
export function buildOtaCommand(url: string): string {
  if (!/^https?:\/\/.+/.test(url)) throw new Error("url http(s):// ile baslamali");
  return JSON.stringify({ cmd: "ota", url });
}

/**
 * cal komutu JSON'unu kurar: {"cmd":"cal","action":"auto"}.
 * Parametresiz — action:"auto" SABİT (YAGNI: cal parametresi yok, tek mod otomatik kalibrasyon).
 */
export function buildCalCommand(): string {
  return JSON.stringify({ cmd: "cal", action: "auto" });
}

/**
 * reset komutu JSON'unu kurar: {"cmd":"reset-wifi"} veya {"cmd":"reset-id"}.
 * kind YALNIZ "wifi" veya "id" olmalı; değilse Error (publish'e gitmez).
 */
export function buildResetCommand(kind: "wifi" | "id"): string {
  if (kind !== "wifi" && kind !== "id") throw new Error("reset kind 'wifi' veya 'id' olmali");
  return JSON.stringify({ cmd: `reset-${kind}` });
}

/**
 * Cihaz komut gövde şeması (zod) — admin route POST /api/devices/[dev]/command bunu kullanır (TÜM komutlar).
 * Web müşteri endpoint'i AYRI dar şema kullanır (yalnız cal + reset-wifi). `type` ayraç (discriminated).
 * Bilinmeyen/eksik type, eksik alan, yanlış tip → reddedilir. Ekstra alanlar strip.
 */
export const commandSchema = z.discriminatedUnion("type", [
  z.object({
    type: z.literal("sci"),
    // secs sınırları firmware ile BİREBİR: alt -1 (durdur), üst 300 (firmware >300'ü clamp eder → fail-fast 400).
    secs: z
      .number()
      .int("secs tam sayı olmalı")
      .gte(-1, "secs -1 (durdur), 0 (süresiz) veya pozitif olmalı")
      .lte(300, "secs en fazla 300 saniye (firmware sınırı)"),
  }),
  z.object({
    type: z.literal("ota"),
    // url http(s):// + uzunluk firmware ile BİREBİR (main.c: url < 256).
    url: z
      .string()
      .max(255, "url en fazla 255 karakter (firmware sınırı)")
      .regex(/^https?:\/\/.+/, "url http(s):// ile başlamalı"),
  }),
  // Parametresiz komutlar — yalnız `type`, ek alan yok (fazla alanlar strip edilir).
  z.object({ type: z.literal("cal") }),
  z.object({ type: z.literal("reset-wifi") }),
  z.object({ type: z.literal("reset-id") }),
]);

/** Doğrulanmış komut gövdesi (route + UI bunu kullanır). */
export type CommandInput = z.infer<typeof commandSchema>;

/**
 * Doğrulanmış komutu firmware payload JSON STRING'ine çevirir — TEK eşleme noktası (UI {type}
 * → firmware {cmd}). build*Command'ları çağırır (sözleşme kopyalanmaz). SAF (I/O yok).
 * switch TÜKENMİŞ (exhaustive): yeni komut eklenip case unutulursa `never` ataması tsc'de patlar.
 */
export function commandToPayload(cmd: CommandInput): string {
  switch (cmd.type) {
    case "sci":
      return buildSciCommand(cmd.secs);
    case "ota":
      return buildOtaCommand(cmd.url);
    case "cal":
      return buildCalCommand();
    case "reset-wifi":
      return buildResetCommand("wifi");
    case "reset-id":
      return buildResetCommand("id");
    default: {
      const _exhaustive: never = cmd;
      throw new Error(`bilinmeyen komut type: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
