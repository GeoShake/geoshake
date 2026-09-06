// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

// FDSN ağ kodu — TEK KAYNAK. web (StationXML, SAC, /fdsnws) ve seedlink-feeder
// (miniSEED stream id) AYNI değeri kullanmalı; ayrışırsa jAmaSeis/ObsPy tarafında
// stream seçimi sessizce boşa düşer.
//
// GEÇMİŞ: kod 2026-07-27'de değişti; sebebi ve o günkü karar zinciri
// docs/FDSN/ag-kodu-basvurusu.md'de duruyor (İÇ belge, yayınlanmıyor).
// Kodu buradan DEĞİŞTİRME — hangi kodların neden elenmiş olduğu o belgede.
//
// 2026-07-27: kod "GW" olarak değiştirildi. fdsn.org/ws/networks/1/query ile
// doğrulandı — GW o gün kayıt defterinde YOKTU (atanmamış); tescil başvurusu
// ayrı iş olarak yürütüldü.
//
// BAŞVURU ONAYLANDI (2026-08-27): GW kalıcı ve FDSN'de tescilli, DOI mint edildi.
// Kayıt defteri: fdsn.org/networks/detail/GW/ — dogrulamadan iddia degistirmeyin.
//
// Geçiş anı bilerek seçildi: SeedLink beslemesi zaten koptuğu için (aynı gün
// bulundu ve onarıldı) kırılacak canlı jAmaSeis kullanıcısı YOKTU. Ring'de eski
// eski stream id'leri buffer'dan düşene kadar bayat olarak görünmeye devam eder.
//
// Kod değişimi KIRICI bir değişikliktir: 27 Tem 2026 öncesi indirilmiş SAC /
// StationXML dosyaları ve eski SeedLink stream id'leri hâlâ eski kodu taşır.
// O dosyaları ortak bir arşive KATMAYIN — yeniden çekildiğinde GW ile gelirler.

/** Yayınlanan ağ kodu (StationXML `<Network code>`, SAC KNETWK, SEED net alanı). */
export const FDSN_NETWORK_CODE = "GW";

/** FDSN'de tescilli mi? 2026-08-27'de ONAYLANDI → dokümantasyonda "provisional"
 *  ibaresi GÖSTERİLMEZ. Kural OTOMATİK değil: bu sabiti okuyan tüketici yok,
 *  ibareler metinlere elle yazılı — bayrağı çevirmek onları düzeltmez. */
export const FDSN_NETWORK_REGISTERED = true;

/** FDSN'in mint ettiği ağ DOI'si. StationXML <Identifier>, developer portalı
 *  "Cite as" ve geoshake.org/network AYNI sabitten okumalı — atıf bölünmesin.
 *  DİKKAT: Zenodo katalog DOI'si (10.5281/zenodo.*) BAŞKA bir şeydir; o katalog
 *  anlık görüntüsünü, bu ağın kendisini gösterir. */
export const FDSN_NETWORK_DOI = "10.7914/ft1b-4x43";

/** FDSN'in verdiği atıf metni — birebir. Değiştirme; atıf dizinleri eşleştiriyor. */
export const FDSN_NETWORK_CITATION =
  "GeoShake (2026): GeoShake Global Seismic Network. International Federation of " +
  "Digital Seismograph Networks. Dataset/Seismic Network. " +
  "https://doi.org/" + FDSN_NETWORK_DOI;

// ─────────────────────────────────────────────────────────────────────────────
// SEED kanal kimliği — TEK KAYNAK. seedlink-feeder yayınlarken, web StationXML
// `level=channel` üretirken AYNI değerleri kullanmak ZORUNDA. Ayrışırsa envanter
// dalga formuna eşlenemez ve bunu kimse fark etmez (sessiz yanlış eşleşme).
// ─────────────────────────────────────────────────────────────────────────────

/** SEED location kodu (feeder LOC + StationXML Channel@locationCode). */
export const FDSN_LOCATION_CODE = "00";

/** 1 sps sürekli kanal. DİKKAT: içeriği skaler büyüklük zarfı, bileşen DEĞİL. */
export const FDSN_CHAN_ENVELOPE = "LNZ";

/** ~110 sps tetik-burst dikey kanalı — GERÇEK dikey, yukarı pozitif.
 *  (2026-08-05'e kadar cihaz gövdesi Z eksenini taşıyordu, yani ad yalandı;
 *  waveBurstToTrace artık örnekleri yerçekimiyle kurulan yerel çerçeveye
 *  döndürüyor, bu yüzden `Z` bileşen harfi doğruyu söylüyor.) */
export const FDSN_CHAN_EVENT = "ENZ";

/** ~110 sps tetik-burst yatay kanalları. Bileşen harfi `1`/`2` — `N`/`E` DEĞİL.
 *  SEED'de N/E gerçek kuzey/doğu demektir; cihazda manyetometre yok, yerçekimi
 *  yalnız "aşağı"yı verir, azimut BİLİNMİYOR. Azimutu bilinmeyen dik yatay çift
 *  için standardın öngördüğü kod `1`/`2`'dir. */
export const FDSN_CHAN_EVENT_H1 = "EN1";
export const FDSN_CHAN_EVENT_H2 = "EN2";

/** LNZ örnekleme hızı (sps) — sabit. */
export const FDSN_ENVELOPE_SPS = 1;

/** ENZ NOMİNAL örnekleme hızı (sps). Gerçek hız burst başına ölçülür ve azıcık
 *  oynar; otoritatif değer miniSEED kayıt başlığındadır. Metadata nominali yayınlar. */
export const FDSN_EVENT_SPS_NOMINAL = 110;

/** counts = m/s² × bu katsayı. Feeder GAIN varsayılanı ve yayınlanan metadata ölçeği
 *  AYNI sabitten gelir — iki yerde yazılırsa metadata sessizce yalan söyler. */
export const FDSN_COUNTS_PER_MS2 = 1_000_000;

/** İstasyon kodu deseni — ISC kayıt formunun kuralı: 3-5 karakter, BÜYÜK harf +
 *  rakam, HARFLE BAŞLAR. AYNI desen devices.seed_code CHECK kısıtında da duruyor
 *  (infra/db/init/43_fdsn_station_identity.sql); ikisinin birebir aynı kaldığı
 *  test/fdsn-station-code.test.ts ile kilitli — bir kural iki yerde yazılırsa
 *  ayrışır ve biri diğerinin ürettiğini reddeder. */
export const FDSN_SEED_CODE_RE = /^[A-Z][A-Z0-9]{2,4}$/;

/** device_id → istasyon kodu. TEK KOD: hem envanter (StationXML `<Station code>`,
 *  FDSN-station text) hem SeedLink stream id'si BUNU kullanır. Kural:
 *    1) "GEO-"/"gs-" öneki kırpılır, büyütülür, alfanümerik olmayan atılır
 *    2) ilk karakter RAKAMSA başa "G" eklenir
 *    3) ilk 5 karakter alınır (miniSEED istasyon alanı 5 BAYT)
 *
 *  G ÖNEKİ NEDEN (2026-08-27): ISC istasyon kodu HARFLE BAŞLAMAK ZORUNDA ve
 *  tescillenince DEĞİŞTİRİLEMEZ. GEO-274YU / GEO-2TRS8 / gs-0001 öneksiz kuralda
 *  rakamla başlayan kod üretiyordu (274YU, 2TRS8, 0001) → ISC reddederdi.
 *
 *  SAF KALMAK ZORUNDA (DB YOK): apps/seedlink-feeder stream id'sini bu fonksiyondan
 *  üretir ve DB'ye HİÇ bakmaz. Kod ayrıca devices.seed_code'ta SAKLANIR (kalıcılık +
 *  çakışma koruması) ama türetme BIRAKILAMAZ — bırakılırsa feeder eski kodu
 *  yayınlamaya devam eder ve envanterle ayrışır; düzeltmeye çalıştığımız hatanın
 *  ta kendisi. Türetme ile saklanan değer AYNI sonucu vermek zorundadır.
 *
 *  ÇAKIŞMA: kırpma yüzünden iki kimlik aynı kodu verebilir (gs-abcdef / gs-abcdeg
 *  → ABCDE). Bu fonksiyon çakışmayı ÇÖZMEZ; devices_seed_code_uniq benzersiz
 *  indeksi backfill sırasında yakalar, çözüm elle saklanan değere yazılır. */
export function seedStationCode(dev: string): string {
  const ham = dev.replace(/^(GEO-|gs-)/i, "").replace(/[^A-Za-z0-9]/g, "").toUpperCase();
  return (/^[0-9]/.test(ham) ? "G" + ham : ham).slice(0, 5);
}
