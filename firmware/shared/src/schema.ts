// GeoShake shared — DSP reference implementation (TypeScript)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

import { z } from "zod";
import { DEVICE_ID_RE } from "./ids.js";
import { VP_KM_S } from "./geo.js";
import type { MountClass } from "./orientation.js";

/** Montaj sınıfı literal listesi — `satisfies` ile orientation.ts'e BAĞLI: MountClass
 *  değişirse burası DERLEME ZAMANINDA kırılır (sessiz sürüklenme olmaz). */
const MOUNT_CLASSES = ["duz", "duvar", "egik"] as const satisfies readonly MountClass[];

// Cihaz id TEK-KAYNAK: ids.ts (gs- grandfather + GEO- yeni; firmware 31-char buffer sınırı).
const devId = z.string().regex(DEVICE_ID_RE);

// Bilinmeyen alanlar sessizce atılır (zod .strip varsayılanı) — cihaz filosunun
// ileri-uyumlu yükseltmesi için bilinçli karar; eski ingest yeni alanları yok sayar.

/**
 * Yönelim yükü: 4-sensör ortalanmış yerçekimi (DC) vektörü [gx,gy,gz], m/s².
 * TEK-KAYNAK — trigger + health AYNI tanımı kullanır (drift olmaz).
 *
 * .optional() KALDIRMA — SAHA-KRİTİK: alanı göndermeyen eski firmware'ler sahada;
 * zorunlu yapılırsa o cihazların paketleri şemadan düşer (tetikleri assoc'a hiç
 * ulaşmaz → deprem asosiyasyonu sessizce durur).
 * .nonnegative() EKLEME — bileşenler işaretlidir; duvar/ters montajda negatif olur.
 * Şema versiyonu (v) ARTMAZ: alan eklemek geriye/ileriye uyumlu (bkz. gain, 0.9.x).
 */
const gravVector = z.array(z.number().finite()).length(3).optional();
export const FeaturePacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  fs: z.number().positive().finite(),
  n: z.number().int().positive(),
  rms: z.array(z.number().nonnegative().finite()).length(3),
  pga: z.number().nonnegative().finite(),
});
export type FeaturePacket = z.infer<typeof FeaturePacket>;

// Grafik için hafif telemetry örneği (geçmiş telemetry satırı + canlı feat ortak şekli).
export const TelemetrySample = z.object({
  tMs: z.number().int().nonnegative().max(8.64e15),
  rms: z.array(z.number().nonnegative().finite()).length(3),
  pga: z.number().nonnegative().finite(),
});
export type TelemetrySample = z.infer<typeof TelemetrySample>;

export const TriggerPacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  sta_lta: z.number().nonnegative().finite(),
  pga: z.number().nonnegative().finite(),
  grav: gravVector,   // yönelim: tetik anındaki yerçekimi (DC) vektörü, m/s²
});
export type TriggerPacket = z.infer<typeof TriggerPacket>;

/**
 * KOSAN TEPE paketi (firmware 2026-09-02, `geoshake/<id>/peak`).
 *
 * NEDEN AYRI BIR PAKET: `TriggerPacket` olayin BASINDA gider ve icindeki `pga`
 * tetigin atildigi ANDAKI anlik genliktir — tepe ivme DEGIL (apps/web/lib/
 * eventPga.ts). Bu paket olayin SONUNDA, dedektor dindiginde gider ve olcunun
 * kendisini tasir. `seq` tetigin seq'iyle AYNIDIR; sunucu ikisini onunla eslestirir.
 *
 * pk_h / pk_z AYRI: standart PGA sozlesmesi MAKSIMUM YATAY'dir, vektor degil.
 * Her GMPE, ShakeMap ve PGA->MMI bagintisi duseyi ayirir. Telemetri eksen basina
 * yalniz RMS tasidigi icin bu ayrim SADECE burada uretilebilir.
 *
 * dur_ms: tetikten de-trigger'a gecen sure. Kisa sure yerel darbeye, uzun sure
 * gercek sarsintiya isaret eder — tek basina bir ayirt edici degil, ama kartta
 * gosterilen ve gurultuyu sorgulatan bir sayi.
 */
export const PeakPacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  pk_h: z.number().nonnegative().finite(),
  pk_z: z.number().nonnegative().finite(),
  pk_v: z.number().nonnegative().finite(),
  dur_ms: z.number().int().nonnegative(),
  n: z.number().int().nonnegative(),
});
export type PeakPacket = z.infer<typeof PeakPacket>;

export const HealthPacket = z.object({
  v: z.literal(1),
  dev: devId,
  seq: z.number().int().nonnegative(),
  t_ms: z.number().int().nonnegative().max(8.64e15),
  up_s: z.number().int().nonnegative(),
  heap: z.number().int().nonnegative(),
  rssi: z.number().int(),
  wifi_rc: z.number().int().nonnegative(),
  mqtt_rc: z.number().int().nonnegative(),
  /* Cihazın SON açılış sebebi (esp_reset_reason). Brownout / panic / gerçek
     elektrik kesintisi dışarıdan aynı görünüyordu; bu alan onları ayırır.
     1=poweron 3=sw 4=panic 5=int_wdt 6=task_wdt 7=wdt 8=deepsleep 9=brownout.
     OPSİYONEL: blep-0.9.4 ve öncesi göndermez — alan yoksa "bilinmiyor". */
  rst: z.number().int().nonnegative().max(255).optional(),
  fw: z.string().min(1),
  gain: z.array(z.number().finite()).length(4).optional(),   // per-sensör kalibrasyon kazancı; eski firmware göndermez (opsiyonel)
  grav: gravVector,   // yönelim: 4-sensör ortalanmış yerçekimi (DC) vektörü, m/s²
  /* Bağlı olunan erişim noktası. `esp_wifi_sta_get_ap_info` bu ikisini ZATEN
     dolduruyordu; yalnız rssi okunup gerisi atılıyordu (2026-08-21'e kadar).
     NEDEN eklendi: GEO-QA2HF bir OTA yeniden başlatmasında -61 → -86 düştü ve
     "başka bir AP'ye mi bağlandı yoksa ortam mı bozuldu" sorusu ELDEKİ VERİYLE
     ayırt edilemedi. bssid eşitliği bu soruyu tek karşılaştırmaya indirir.
     OPSİYONEL: blep-0.9.11 ve öncesi göndermez — alan yoksa "bilinmiyor". */
  bssid: z.string().regex(/^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/).optional(),
  ch: z.number().int().min(1).max(233).optional(),   // ap.primary (uint8); 2,4 GHz 1-14, 5 GHz 36-165
  /* GÖLGE TETİK SAYACI — "eşik 6 yerine 3 olsaydı kaç kez tetiklerdi".
     Cihazda canlı dedektörün YANINDA ikinci bir STA/LTA durum makinesi koşar
     (eşik 3,0, bırakma eşiği canlıyla aynı); hiçbir şey yayınlamaz, yalnız
     eşiği geçen GEÇİŞLERİ sayar — örnekleri değil, ki uzun bir gürültü olayı
     tek olay sayılsın.
     NEDEN VAR: tetiklemeyen olaylarda STA/LTA hiçbir yere yazılmıyor.
     2026-09-02'de GEO-274YU'nun 77 km uzağındaki M4,8 ve M4,6 telemetride
     tabanın 2,9 ve 2,7 katı iz bıraktı ama eşiği geçmedi; oranın kaça ulaştığı
     BİLİNMİYOR ve geriye dönük hesaplanamaz (telemetri 1 sn tepe taşıyor,
     STA penceresi 0,5 sn).
     KÜMÜLATİF DEĞİL: cihaz her sağlık paketinde değeri atomic_exchange ile
     okuyup SIFIRLAR, yani her paket "son 30 saniyedeki geçiş sayısı"dır.
     Toplam istenirse satırlar TOPLANIR — son değer okunmaz.
     OPSİYONEL: blep-0.9.15 ve öncesi göndermez — alan yoksa "ölçüm yok",
     0 ise "ölçtük, hiç geçmedi". rst ve gain ile AYNI gerekçe: zorunlu olsaydı
     sahadaki eski firmware'li cihazların sağlık telemetrisi TOPLUCA düşerdi. */
  sh_trig: z.number().int().nonnegative().optional(),
  /* RSSI İSTATİSTİĞİ — cihazda 1 Hz örneklenip her sağlık paketinde özetlenir.
     `rssi` alanı TEK anlık okumadır ve kasa/anten karşılaştırması için yetmez:
     ölçüldü (GEO-KK9YF, 2176 kayıt) tek okumanın std'si 3,07 dB VE okumalar
     bağımsız değil — 20 okumayı ortalamak hatayı 0,69'a değil 2,78'e indiriyor.
     Yani her paket tek bir zayıf ölçüm taşıyor.
     rs_lo/rs_hi ASIL KAZANÇ: sönümleme derinliği. Kayıplı ya da rezonans yapan
     bir muhafaza yalnız ortalamayı düşürmez, dalgalanmayı da büyütür — ortalama
     eşit çıkıp derinlik farklıysa bu tek başına bir sonuçtur.
     rs_a ×10 ÖLÇEKLİ TAM SAYIDIR (-645 = -64,5 dBm): float biçimlendirme yok ve
     yarım dB yuvarlanmadan geçer. Bölerken 10'a bölmeyi unutma.
     rs_n=0 "ölçemedik" demektir; o durumda diğer üçü 0 gider ve bu "0 dBm
     ölçtük" ile karışmaz — ayrımı rs_n yapar.
     KÜMÜLATİF DEĞİL: sh_trig gibi her pakette sıfırlanır, yani değerler son
     ~30 saniyeye aittir.
     OPSİYONEL: blep-0.9.17 ve öncesi göndermez. Zorunlu olsaydı sahadaki eski
     firmware'li cihazların sağlık telemetrisi TOPLUCA düşerdi (rst/gain/sh_trig
     ile aynı gerekçe). */
  rs_n: z.number().int().nonnegative().optional(),
  rs_a: z.number().int().optional(),
  rs_lo: z.number().int().optional(),
  rs_hi: z.number().int().optional(),
  /* YAZILIM YENIDEN BAŞLATMA SEBEBİ — bu açılışı hangi kod yolu tetikledi.
     NEDEN VAR: `rst` alanı `esp_reset_reason()`tir ve BÜTÜN `esp_restart()`
     çağrılarını tek değere (3 = SW) indirger. OTA mı, WiFi watchdog'u mu, MQTT
     watchdog'u mu ayırt edilemiyordu — yani sahaya bir kurtarma mekanizması
     koyup ÇALIŞIP ÇALIŞMADIĞINI ölçemiyorduk. 2026-09-05'te tam olarak bu
     yaşandı: MQTT watchdog'u eklendi, ateşlediğini görecek alan yoktu.
       0 = bilinmiyor (elektrik, panik, ya da 0.9.20 ve öncesi firmware)
       1 = WiFi mid-run watchdog'u      2 = MQTT watchdog'u
       3 = captive zaman aşımı          4 = OTA
       5 = reset-id                     6 = reset-wifi
       7 = kendi kendine provisioning
     BİR KEZ bildirilir: cihaz açılışta NVS'ten okuyup SİLER, yani bu değer
     yalnız yeniden başlatmayı izleyen ilk paketlerde 0'dan farklıdır. Toplamak
     için sayılır, son değer okunmaz.
     OPSİYONEL: blep-0.9.20 ve öncesi göndermez (rst/gain/sh_trig ile aynı gerekçe —
     zorunlu olsaydı eski firmware'li cihazların telemetrisi TOPLUCA düşerdi). */
  sw_rst: z.number().int().min(0).max(255).optional(),
  /* IC RAM + MQTT DENEME TEŞHİSİ (blep-0.9.24) — A73T7 gerilemesini ayırmak için.
     heap_int/heap_blk: iç RAM (MALLOC_CAP_INTERNAL); `heap` alanı PSRAM'li derlemede
     ~8,27 MB gösterir ve %99'u PSRAM'dir, oysa mbedTLS bilerek iç RAM'de çalışır. TLS
     el sıkışması ~30-45 KB BİTİŞİK iç blok ister → heap_blk o eşiğin altına inerse
     yeniden bağlanma TLS'te ölür ve reboot dışında hiçbir şey düzeltmez; free_heap ile
     bu GÖRÜLEMİYORDU. heap_int serbest iç RAM toplamı, heap_blk en büyük bitişik blok.
     mqtt_try/mqtt_fail: KÜMÜLATİF (açılıştan beri) bağlanma denemesi / taşıma-hata sayacı.
     "try çok, fail çok" sürekli deniyor ama TLS/TCP'de ölüyor; "try 0" istemci hiç
     denemiyor — iki farklı arıza, iki farklı çözüm.
     OPSİYONEL: blep-0.9.23 ve öncesi göndermez (rst/sw_rst ile aynı gerekçe — zorunlu
     olsaydı eski firmware'li cihazların telemetrisi TOPLUCA düşerdi). */
  heap_int: z.number().int().nonnegative().optional(),
  heap_blk: z.number().int().nonnegative().optional(),
  mqtt_try: z.number().int().nonnegative().optional(),
  mqtt_fail: z.number().int().nonnegative().optional(),
});
export type HealthPacket = z.infer<typeof HealthPacket>;

/**
 * ŞİDDET SINIFI — AÇIK KÜME (open enum). Bugün üretilen üç değer.
 *
 * TÜKETİMDE HOŞGÖRÜLÜ / ÜRETİMDE KATI (T12b, 2026-08-11). Şema kapısı bilinmeyen
 * bir sınıfı REDDETMEZ; üreten taraf (assoc `intensity()`) ise yalnız bu üçünü
 * döndürebilir — dönüş tipi `KnownIntensityClass`. Asimetri bilinçli:
 *
 * - Sunucu dördüncü bir sınıf yayarsa (`cok_siddetli` gibi) ve kapı kapalı
 *   olsaydı, o olayları taşıyan yanıtlar MAĞAZADAKİ eski uygulamada şemadan
 *   geçemezdi. `/api/events` en yeni 50 olayı döndürür (LIMIT 50); büyük bir
 *   artçı serisinde 50'sinin de yeni sınıf olması gerçekçi. O noktada mobilin
 *   sert-hata kuralı (`assertContract`) ateşlenir ve olay katmanı, haritanın EN
 *   ÇOK gerektiği anda kararırdı. Yayınlanmış uygulama hotfix'lenemez.
 * - Veritabanı zaten açık: kolon `intensity_class TEXT`, CHECK kısıtı YOK
 *   (infra/db/init/02_faz2.sql). Kapalı olan tek yer BU şema kapısıydı.
 *
 * `(string & {})` KALIBI BİLEREK — düz `string` yazılsaydı `Record<IntensityClass, X>`
 * TAMLIK kontrolünü kaybederdi (ölçüldü: eksik tablo sorunsuz derlenirdi). Bu kalıpla
 * `hafif`/`orta`/`siddetli` anahtarları ZORUNLU kalır; apps/admin'in üç renk tablosu ve
 * apps/assoc'un RANK tablosu korumasını sürdürür ve `INTENSITY_CLASSES`'a dördüncü değer
 * eklendiği gün derlenmeyerek her tabloyu adres gösterir.
 *
 * Gevşeme "her şey geçer" DEĞİL: boş dize ve dize-olmayan hâlâ reddedilir — bunlar
 * bir sınıf adı değil, eksik veridir. Uzunluk ÜST SINIRI bilerek KONMADI: yeni bir
 * drop yolu eklemek, bu işin kaldırdığı riskin aynısını geri getirirdi.
 */
export const INTENSITY_CLASSES = ["hafif", "orta", "siddetli"] as const;
export type KnownIntensityClass = (typeof INTENSITY_CLASSES)[number];
export type IntensityClass = KnownIntensityClass | (string & {});

/** Bilinen sınıf mı? `true` dönerse tip TAM daralır → üç dal için özel davranış yazılabilir. */
export function isKnownIntensityClass(v: string): v is KnownIntensityClass {
  return (INTENSITY_CLASSES as readonly string[]).includes(v);
}

/** Şema kapısı: bilinen üçü + ileriye dönük bilinmeyen sınıf. Gerekçe yukarıda. */
export const intensityClassSchema: z.ZodType<IntensityClass> =
  z.union([z.enum(INTENSITY_CLASSES), z.string().min(1)]);

/** Association olay ilanı — assoc yayınlar (geoshake/events), web tüketir (API + WS). */
export const EventPacket = z.object({
  v: z.literal(1),
  event_id: z.number().int().positive(),
  originMs: z.number().int().nonnegative().max(8.64e15),
  lat: z.number().finite(),
  lon: z.number().finite(),
  nStations: z.number().int().positive(),
  intensityClass: intensityClassSchema,
  maxPga: z.number().nonnegative().finite(),
  rmsResidual: z.number().nonnegative().finite(),
  // Büyüklük (GeoShake anchored attenuation v1) — OPSİYONEL ve SINIRSIZ (yalnız .finite()).
  // Opsiyonel: eski olaylarda yok (geriye doldurma yalanı yapılmadı) ve v'yi artırmaz —
  // alan EKLEMEK breaking değil (bkz. şema versiyonu notu). Eski tüketiciler zod .strip
  // ile etkilenmez; alanı okumayanlar için görünmez.
  // Alt/üst SINIR YOK bilinçli: model v1 ve kalibre edilmemiş. Dar bir aralık koyup şema
  // throw'u yemek, olay YAYININI sessizce öldürür (bu depoda originMs ile bir kez yaşandı).
  magnitude: z.number().finite().optional(),
});
export type EventPacket = z.infer<typeof EventPacket>;

/** Harita istasyonu (konumlu device). */
export const StationInfo = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  lastHealthMs: z.number().int().nonnegative().max(8.64e15).optional(),   // yalnız gerçek cihaz; sim → yok
});
export type StationInfo = z.infer<typeof StationInfo>;

/** Event-triggered dalga formu: işlenmiş tek-iz (4-sensör ort büyüklük, m/s²) + zaman ekseni. */
export const Waveform = z.object({
  t0Ms: z.number().int().nonnegative().max(8.64e15),
  fs: z.number().positive().finite(),
  // null = O ÖRNEK YOK (kayıt boşluğu) — "ölçüm sıfır" DEĞİL. Kurucu kararı (2026-08-05):
  // boşluk boşluk kalır; 0'a çevirme/enterpolasyon yok, çizim çizgiyi orada kırar.
  // Sayı geliyorsa SONLU olmalı (NaN/±Infinity kabul edilmez — eşleyici onu null'a çevirir).
  samples: z.array(z.number().finite().nullable()),
});
export type Waveform = z.infer<typeof Waveform>;

/** Detay sayfası istasyonu: tetik + konum + türetilmiş mesafe/gecikme. */
export const StationDetail = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  tMs: z.number().int().nonnegative().max(8.64e15),
  staLta: z.number().nonnegative().finite(),
  pga: z.number().nonnegative().finite(),
  distKm: z.number().nonnegative().finite(),
  delayMs: z.number().finite(),   // origin'e göre varış gecikmesi; NEGATİF olabilir (origin back-solve'lu) — .nonnegative() EKLEME
  waveform: Waveform.optional(),   // yalnız gerçek cihaz (event-triggered); sim/eksik → yok
});
export type StationDetail = z.infer<typeof StationDetail>;

/** Bir olayın tam detayı: olay + katkı veren istasyonlar. */
export const EventDetail = z.object({ event: EventPacket, stations: z.array(StationDetail) });
export type EventDetail = z.infer<typeof EventDetail>;

/** İstasyon detay sayfası (/station/[dev]) yükü. StationDetail (Faz 3b olay-içi) ile karıştırma. */
export const StationProfile = z.object({
  dev: devId,
  lat: z.number().finite(),
  lon: z.number().finite(),
  /** Sahibin verdiği isim (device_owners.nickname) — HERKESE AÇIK gösterilir (kullanıcı
   *  kararı, 2026-07-15); rename UI'ları "isim herkese görünür" notu düşer. Sahipsiz → yok. */
  nickname: z.string().optional(),
  /** Cihazın ağa ilk kaydı (devices.first_seen) — "X days on air" rozeti için. */
  firstSeenMs: z.number().int().nonnegative().max(8.64e15).optional(),
  health: z.object({
    rssi: z.number().int(),
    uptimeS: z.number().int().nonnegative(),
    freeHeap: z.number().int().nonnegative(),
    fwVersion: z.string(),
    lastHealthMs: z.number().int().nonnegative().max(8.64e15),
    /** YÖNELİM (#91-A) — cihazın bildirdiği duruş. .optional() ZORUNLU: eski firmware
     *  `grav` göndermiyor, ingest yeni koda geçene kadar kolonlar boş → alanlar YOK. */
    tiltDeg: z.number().min(0).max(180).optional(),
    mountClass: z.enum(MOUNT_CLASSES).optional(),
    /** Ölçülen yerçekimi vektörü [gx,gy,gz] m/s² (cihaz gövde ekseni) — tiltDeg'in HAM kaynağı.
     *  Arayüzde 3B duruş çizimi kutunun hangi YÖNE yattığını buradan okur (açıyı YENİDEN
     *  HESAPLAMAZ, tiltDeg'i kullanır). Coğrafi yön İÇERMEZ: manyetometre yok, bu vektör
     *  yalnız "aşağı"yı bilir. .optional() gerekçesi tiltDeg ile aynı. */
    grav: gravVector,
  }).optional(),   // yalnız gerçek cihaz; sim → yok
  triggers: z.array(z.object({
    seq: z.number().int().nonnegative(),
    tMs: z.number().int().nonnegative().max(8.64e15),
    staLta: z.number().nonnegative().finite(),
    pga: z.number().nonnegative().finite(),
    eventId: z.number().int().positive().optional(),
    /** JAMASEIS-B1: dalga formu saklandı mı (SAC indirme linki) / 3 eksen var mı (X/Y/Z linkleri). */
    hasWave: z.boolean().optional(),
    hasAxes: z.boolean().optional(),
  })),
  events: z.array(z.object({
    eventId: z.number().int().positive(),
    originMs: z.number().int().nonnegative().max(8.64e15),
    // EventPacket ile AYNI açık küme — istasyon sayfası da dördüncü sınıfta kararmasın.
    intensityClass: intensityClassSchema,
  })),
});
export type StationProfile = z.infer<typeof StationProfile>;

/**
 * Association motoru çalışma-zamanı ayarları — admin panel YAZAR, assoc motoru OKUR.
 * TEK KAYNAK: hem panel (form/POST doğrular) hem assoc (DB okumasını doğrular; geçersiz/eksikse
 * DEFAULT_ASSOC_CONFIG'e düşer) bu şemayı kullanır. Sınırlar GÜVENLİK-KRİTİK (deprem erken uyarı):
 * minDevices<3 → lokasyon under-determined (çöp konum); büyük yarıçap → ilgisiz tetikleri kümeler;
 * büyük tolerans → gürültü sızar. Sınırlar hatalı-yapılandırmayı önler.
 *
 * AŞIRI-YÜKLEME AYRIŞTIRMASI: radiusKm ve tolMs'in HER BİRİ iki AYRI fiziksel büyüklüğü besliyordu
 * (yarıçap: istasyon↔istasyon kümeleme VE episantr↔istasyon backfill; tolerans: RMS residual kapısı VE
 * backfill varış penceresi). Tek sayıya bağlıyken biri için doğru olan değer diğeri için yanlıştı —
 * ayarlanamaz hale gelmişti. Dörde ayrıldı.
 *
 * Yeni alanlar .optional(): GERİYE DÖNÜK UYUMLULUK. Migrasyon öncesi yazılmış config nesneleri
 * (ve yeni kolonları olmayan DB satırları) hâlâ parse edilir; eksik alan resolveAssocConfig ile
 * eski karşılığından türetilir. Eski alanlar KALDIRILMADI: rollback'te eski assoc sürümü onları okur.
 */
/**
 * dt_ms ÜST SINIRI — assoc motorunun bellek penceresine (core.ts WINDOW_MS) BAĞLI.
 * assoc yalnız en yeni tetikten WINDOW_MS geriye kadar olan tetikleri bellekte tutar; dt bundan
 * büyük olursa kümeye girmesi gereken tetik daha kümeleme çalışmadan pencereden düşer — config
 * "60 s'lik pencere" der, motor 30 s uygular. Sessiz yalan yerine sınırı motorun gerçeğine çek.
 * Eskiden 60000'di (WINDOW_MS'in İKİ KATI). Canlı değer 15000 → etkilenmez.
 * WINDOW_MS büyütülürse burası da büyütülebilir; ilişki windowInvariant testiyle kilitli.
 */
export const ASSOC_DT_MS_MAX = 30000;

export const AssocConfig = z.object({
  radiusKm: z.number().min(1).max(500),          // ESKİ (geri uyum): cluster+event yarıçapının ortak atası
  dtMs: z.number().int().min(1000).max(ASSOC_DT_MS_MAX),  // küme zaman penceresi (ms); tavan = motor belleği
  minDevices: z.number().int().min(3).max(50),   // min ayrık istasyon (asla <3: lokasyon under-determined)
  tolMs: z.number().int().min(100).max(10000),   // ESKİ (geri uyum): residual+backfill toleransının ortak atası
  clusterRadiusKm: z.number().min(1).max(500).optional(),        // istasyon↔istasyon: kümeye kimler girer
  eventRadiusKm: z.number().min(1).max(500).optional(),          // episantr↔istasyon: backfill'de olaya kimler bağlanır
  residualMaxMs: z.number().int().min(100).max(10000).optional(),// RMS residual kapısı: olay gerçek mi
  backfillTolMs: z.number().int().min(100).max(10000).optional(),// backfill varış zamanı toleransı
  // ORAN KAPISI eşiği: beklenen istasyonların en az bu kadarı tetiklenmiş olmalı.
  // Alt sınır 0.3 — daha aşağısı kapıyı fiilen kapatır (her şey geçer) ve yanlış güven verir;
  // üst sınır 1 = "istisnasız hepsi görmeli" (tek sessiz istasyon olayı öldürür, çok katı ama meşru).
  coincidenceMinFraction: z.number().min(0.3).max(1).optional(),
});
export type AssocConfigT = z.infer<typeof AssocConfig>;

/** Fail-safe fallback — DB config eksik/geçersizse assoc BUNUNLA koşar, yani gerçek bir çalışma
 *  yapılandırmasıdır. CANLI ile hizalı tutulur: eskiden 50 km'ydi ve canlı 100'ken DB okunamayınca
 *  ağ sessizce yarı yarıya körleşiyordu (fallback "güvenli" değil, sadece farklıydı).
 *
 *  clusterRadiusKm 90: fizik kuralının tam sınırı (dt 15 s × Vp 6 = 90). eventRadiusKm 100: backfill
 *  kendi zaman penceresini yarıçaptan türetir, kurala tabi değil.
 *
 *  Legacy radiusKm NEDEN 90 (100 değil): eski assoc sürümü onu HEM cluster HEM event için kullanır.
 *  100 yazsaydık rollback'te cluster yarıçapı kurala aykırı (yöne bağlı tespit) hale gelirdi;
 *  90 yazınca en kötü ihtimalle backfill 10 km eksik erişir — zararsız, geri alınabilir fail-safe.
 *  Legacy tolMs 2000: residual ve backfill toleransı zaten aynı değerde, ayrım gerekmiyor.
 *
 *  Required<>: varsayılan HER alanı DOLDURUR → tüketici `?? ` yapmadan somut sayı görür (tip garantisi).
 *  AssocConfigStrict'e uyduğu testle kilitli (defaultObeysRule) — ileride varsayılanı bozan yakalanır. */
export const DEFAULT_ASSOC_CONFIG: Required<AssocConfigT> = {
  radiusKm: 90, dtMs: 15000, minDevices: 3, tolMs: 2000,
  clusterRadiusKm: 90, eventRadiusKm: 100, residualMaxMs: 2000, backfillTolMs: 2000,
  // 0.6: beklenenlerin ~2/3'ü. Sönümleme tekdüze azalsa da istasyonlar eşit duyarlı değil
  // (montaj, zemin, gürültü tabanı) → 1.0 gerçek olayları keserdi; 0.5 altı ise kamyon
  // gürültüsünü ayırt etmezdi. İlk haftalarda RET loglarıyla ayarlanacak.
  coincidenceMinFraction: 0.6,
};

/** Ayrıştırılmış config: dört yeni alanın hepsi SOMUT (opsiyonel değil). */
export interface ResolvedAssocConfigT {
  dtMs: number; minDevices: number;
  clusterRadiusKm: number; eventRadiusKm: number;
  residualMaxMs: number; backfillTolMs: number;
  coincidenceMinFraction: number;
}

/** Eksik yeni alanları eski karşılığından türetir — geri uyumun TEK KAYNAĞI (assoc + panel aynı kuralı
 *  kullanır, drift yok). Eşleme, ayrıştırma öncesi kodun fiilen yaptığı işi birebir yansıtır:
 *  radiusKm hem kümelemeyi hem backfill yarıçapını, tolMs hem residual kapısını hem backfill
 *  toleransını besliyordu. Saf. */
export function resolveAssocConfig(c: AssocConfigT): ResolvedAssocConfigT {
  return {
    dtMs: c.dtMs,
    minDevices: c.minDevices,
    clusterRadiusKm: c.clusterRadiusKm ?? c.radiusKm,
    eventRadiusKm: c.eventRadiusKm ?? c.radiusKm,
    residualMaxMs: c.residualMaxMs ?? c.tolMs,
    backfillTolMs: c.backfillTolMs ?? c.tolMs,
    // Bunun ESKİ karşılığı YOK (ayrıştırmayla değil, yeni bir kapıyla geldi) → eksikse sabit
    // varsayılana düşer. Kolonu olmayan DB'de (bugünkü canlı) tam olarak bu yol işler.
    coincidenceMinFraction: c.coincidenceMinFraction ?? DEFAULT_ASSOC_CONFIG.coincidenceMinFraction,
  };
}

/** ETKİN cluster yarıçapı: yeni alan varsa o, yoksa eski radiusKm. Kural ve kırpma bunun üzerinden
 *  çalışır — canlı satırda legacy radius_km ESKİ değerinde kalabilir (rollback için), ona bakıp
 *  reddetmek geçerli bir yapılandırmayı yanlışlıkla ihlal saydırırdı. Saf. */
export function effectiveClusterRadiusKm(c: AssocConfigT): number {
  return c.clusterRadiusKm ?? c.radiusKm;
}

/**
 * Cluster yarıçapının FİZİKSEL tavanı: dt penceresinde P dalgasının kat ettiği mesafe (km).
 *
 * NEDEN: cluster iki tetiği "aynı olay" saymak için |Δt| ≤ dt İSTER. Yarıçap dt×Vp'yi aşarsa aynı
 * yapılandırma YÖNE BAĞLI davranır: episantrın iki YANINDAKİ çift (ör. 50+50 km) neredeyse eşzamanlı
 * tetiklenir ve kümelenir; AYNI DOĞRULTUDA dizilmiş 100 km'lik çift 16,7 s arayla tetiklenir ve
 * 15 s penceresine sığmaz. Config 100 km vaat ederken teslimat yön şansına kalır — ayarlanabilir
 * görünen ama gerçekte belirsiz bir sistem. Eşitlik geçerlidir (90 ≤ 90).
 *
 * Vs DEĞİL Vp: locate() saf P-varışı fit eder. S ile tetiklenen bir istasyon kümeye girse bile
 * varışı P modeliyle uyuşmaz, residual kapısı olayı öldürür — yani S'i saymak menzil kazandırmaz.
 * Locator ileride S varışlarını da modellerse bu tavan yeniden değerlendirilmeli.
 */
export function maxClusterRadiusKm(dtMs: number): number {
  return (dtMs / 1000) * VP_KM_S;
}

/**
 * YAZIM yolu şeması (admin panel): taban + fizik kuralı.
 *
 * MİMARİ — KURAL NEDEN TABANA KONULMADI: taban şemayı assoc'un OKUMA yolu da kullanır. Kural
 * tabanda olsaydı, kurala aykırı bir CANLI satır (bugün cluster=100, dt=15000 tam bu durumda)
 * parse edilemez, loadConfig DEFAULT'a düşer ve yarıçap kimseye sormadan değişirdi — yapılandırma
 * hatasını "sessizce başka bir ağ çalıştırarak" cezalandırmak, düzeltmekten beterdir.
 * Okuma yolu bu yüzden tabanı kullanıp değeri KIRPAR (bkz. assoc/src/db.ts clampClusterRadius);
 * yazım yolu ise burada REDDEDER — hata, oluşturulduğu anda ve onu düzeltebilecek kişiye gösterilir.
 */
export const AssocConfigStrict = AssocConfig.superRefine((c, ctx) => {
  const eff = effectiveClusterRadiusKm(c);
  const limit = maxClusterRadiusKm(c.dtMs);
  if (eff > limit) {
    ctx.addIssue({
      code: z.ZodIssueCode.custom,
      // path: kullanıcının düzelteceği alan (panel hatayı bu alanın yanında gösterebilsin)
      path: ["clusterRadiusKm"],
      message: `cluster yarıçapı ${eff} km, dt=${c.dtMs / 1000} s penceresinde P dalgasının ` +
        `taşıyabileceği ${limit} km'yi aşıyor (kural: cluster ≤ dt × Vp, Vp=${VP_KM_S} km/s). ` +
        `Ya yarıçapı ≤${limit} km yap ya da pencereyi büyüt.`,
    });
  }
});

/**
 * İSTASYON CANLILIK PENCERESİ (ms): bir istasyonun son görülme anı bu kadar geriye kadarsa
 * "canlı" sayılır. VP_KM_S deseni — tek kaynak sabit, sahibi shared, tüketicisi assoc.
 *
 * NEDEN 120 s: cihazlar health mesajını 30 sn kadansla gönderiyor, yani 4 kaçırılmış kadans.
 * Bir-iki paket kaybı (Wi-Fi mikro kesintisi, MQTT reconnect) istasyonu ölü göstermesin diye
 * 1× değil; ama gerçekten fişten çekilmiş bir cihaz dakikalar boyunca canlı görünmesin diye de
 * 10× değil.
 *
 * ASİMETRİK RİSK — sıkı tarafta durmanın sebebi: yanlış-"canlı" TEHLİKELİ yön. Görev 4'ün oran
 * testi ("görmesi gerekenler gördü mü") beklenen küme'yi canlı istasyonlardan kuracak; fişi çekik
 * bir cihaz canlı sayılırsa beklenen küme şişer, oran düşer ve GERÇEK olay elenir. Yanlış-"ölü"
 * ise yalnız o istasyonu beklentiden çıkarır — tespit zaten tetiğe bakar, kayıp yok.
 *
 * CONFIG DEĞİL, SABİT: panelden ayarlanma ihtiyacı henüz KANITLANMADI (migrasyon + iki şema +
 * form alanı bedeli boşa gitmesin). Görev 4 sahada ayar gerektirdiğini gösterirse assoc_config'e
 * terfi eder — o zamana kadar YAGNI.
 */
export const LIVENESS_WINDOW_MS = 120_000;
