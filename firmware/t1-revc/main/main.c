// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>   /* golge tetik sayaci: ornekleme gorevi yazar, saglik gorevi okur+sifirlar */
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_reset_reason() — the "rst" field in health */
#include "esp_heap_caps.h" /* heap_caps_malloc — dalga tamponunu PSRAM'e tasimak icin */
#include "esp_wifi.h"
#include "esp_bt.h"     /* esp_bt_controller_mem_release — BLE is only used during setup */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "config.h"     /* N1-T5: constants SHARED by all devices (MQTT_BROKER, FW_VERSION); secrets.h is retired */
#include "identity.h"   /* device identity from NVS at runtime (gs_identity_t); WIFI/DEVICE/MQTT_USER/PASS are now g_id */
#include "captive.h"    /* N1-T3: captive portal on the not-provisioned branch (SoftAP + form -> NVS) */
#include "bleprov.h"    /* BLEPROV: mobile app BLE setup (SIDE BY SIDE with captive; same NVS path) */
#include "provision.h"  /* GH-DEVPROV Layer 2: self-provisioning from the cloud with a claim token (HTTPS) */
#include "cal.h"        /* CAL-T3: per-sensor gain calibration (NVS "geoshake_cal"); boot-load + apply */
#include "seqstore.h"   /* trigger seq that survives a reboot (waveforms PK is (device_id, trig_seq)) */

/* ---- Pin map: custom 4-IMU sensor PCB (U1/U84/U85/U86) ---- */
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int PIN_CS[4] = {6, 7, 15, 16}; /* sensor 0..3 = U1, U84, U85, U86 */
#define PIN_DRDY 17                          /* IMU1 (U1) INT1 */

/* ---- LED status indicator (GPIO39, which the firmware had never driven until now) ----
   LEDC hardware PWM: 1 timer (25 kHz, 11-bit) + 1 channel -> GPIO39. led_task drives it. */
#define PIN_LED 39
#define LED_ACTIVE_HIGH 1   /* polarity — set to 0 if the board is wired the other way round */
#define LED_TIMER LEDC_TIMER_0
#define LED_CH    LEDC_CHANNEL_0
/* 11-bit / 2047 (lowered from 13-bit / 8191 on 2026-08-11) — the rationale is in led_init. */
#define LED_MAX   2047      /* 11-bit full-scale duty */
/* LED status patterns (used by the led_task state machine) */
#define LED_BURST_MS    2500            /* duration of the bright burst after a trigger */
#define LED_PANIK_MS    150             /* panic square-wave period (fast) */
#define LED_BLINK_MS    500             /* slow blink period for no connection */
#define LED_NABIZ_MS    3000            /* normal pulse (breathing) period */
#define LED_NABIZ_PEAK  ((LED_MAX*3)/10)/* pulse peak duty ~30% (614 at 11-bit) */

#define REG_INT1_CTRL 0x0D
#define REG_WHO_AM_I  0x0F
#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_CTRL3_C   0x12
#define REG_CTRL9_XL  0x18
#define REG_OUTX_L_A  0x28
#define WHO_AM_I_VAL  0x6C

/* LSB -> m/s^2 (+-2g full scale, 16 bit) — same as the shared SCALE_MS2_PER_LSB */
static const double SCALE = (4.0 * 9.80665) / 65536.0;

static const char *TAG = "geoshake";
static spi_device_handle_t s_dev[4];
static TaskHandle_t s_sample_task;
/* The mqtt_task handle: for the stack high-water measurement. The enlarged JSON
   buffers (tb[192], buf[320]) sit on this task's stack. */
static TaskHandle_t s_mqtt_task;

/* ---- Device identity (runtime NVS) + MQTT topic buffers ----
   g_id is filled by gs_identity_load in app_main in N1-T2; in this phase it stays zero-init
   (empty) -> enough for build/linker verification + the topic refactor. Instead of the old
   DEVICE_ID literal-concat topics, these buffers are built from g_id.device_id (gs_topics_init).
   Sizes: device_id[31]+"geoshake/"(9)+"/health"(7)+NUL < 64 -> safe. */
static gs_identity_t g_id;
static char g_topic_trig[64];
static char g_topic_feat[64];
static char g_topic_health[64];
static char g_topic_cmd[64];
static char g_topic_sci[64];
static char g_topic_ack[64];
static char g_topic_peak[64];   /* KOSAN TEPE (2026-09-02) */
/* The wave topic carries a seq (geoshake/<id>/wave/<seq>) -> not constant, built at the point of use. */

/* ---- CAL-T3: per-sensor gain (4 scalars) ----
   Filled at boot from NVS "geoshake_cal" by gs_cal_load (all 1.0 if absent). sample_task applies
   it to acc after read_accel. gain=1.0 -> byte-identical no-op (the measurement is in CAL-T4). */
static float s_cal_gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

/* ---- CAL-T4: auto still-state relative-gain measurement constants (all tuned on hardware) ----
   CAL_SAMPLES: ~1.5 s of samples. The ODR is configured to 104 Hz (REG_CTRL1_XL=0x40), effectively
   ~110 Hz (see the WIN_MAX/science comments) -> 160 samples ≈ 1.45..1.54 s (same magnitude as WIN_MAX=160).
   CAL_STILL_TOL: the stillness threshold — CV^2 (var/mean^2); 0.01 = 10% CV, above that "moving".
   CAL_MIN_MAG: dead/disconnected sensor threshold — raw LSB; a healthy sensor at rest reads ~16384 (=1g), 8000 ≈ 0.5g. */
#define CAL_SAMPLES   160
#define CAL_STILL_TOL 0.01f
#define CAL_MIN_MAG   8000.0f

/* Build the constant topics from g_id.device_id. Called (from app_main) after the identity has been
   loaded (N1-T2) and BEFORE the first publish/subscribe. If g_id is empty it builds empty-id topics. */
static void gs_topics_init(void) {
    snprintf(g_topic_trig,   sizeof(g_topic_trig),   "geoshake/%s/trig",   g_id.device_id);
    snprintf(g_topic_feat,   sizeof(g_topic_feat),   "geoshake/%s/feat",   g_id.device_id);
    snprintf(g_topic_health, sizeof(g_topic_health), "geoshake/%s/health", g_id.device_id);
    snprintf(g_topic_cmd,    sizeof(g_topic_cmd),    "geoshake/%s/cmd",    g_id.device_id);
    snprintf(g_topic_sci,    sizeof(g_topic_sci),    "geoshake/%s/sci",    g_id.device_id);
    snprintf(g_topic_ack,    sizeof(g_topic_ack),    "geoshake/%s/ack",    g_id.device_id);
    snprintf(g_topic_peak,   sizeof(g_topic_peak),   "geoshake/%s/peak",   g_id.device_id);
}

/* ---- Feature computation (mirrors the shared computeWindowFeatures EXACTLY) ----
   Input: n samples, each sample 12 floats (4 sensors x 3 axes, sensor-major,
   gravity ALREADY REMOVED). The 4 sensors are averaged, then per-axis RMS + peak vector (PGA). */
typedef struct { float rms[3]; float pga; } features_t;

static features_t compute_features(const float (*samp)[12], int n) {
    if (n == 0) { features_t z = {{0, 0, 0}, 0}; return z; }
    double sx = 0, sy = 0, sz = 0, pga = 0;
    for (int k = 0; k < n; k++) {
        const float *s = samp[k];
        double ax = (s[0] + s[3] + s[6] + s[9]) / 4.0;
        double ay = (s[1] + s[4] + s[7] + s[10]) / 4.0;
        double az = (s[2] + s[5] + s[8] + s[11]) / 4.0;
        sx += ax * ax; sy += ay * ay; sz += az * az;
        double mag = sqrt(ax * ax + ay * ay + az * az);
        if (mag > pga) pga = mag;
    }
    features_t f;
    f.rms[0] = (float)sqrt(sx / n);
    f.rms[1] = (float)sqrt(sy / n);
    f.rms[2] = (float)sqrt(sz / n);
    f.pga = (float)pga;
    return f;
}

/* ---- Phase 2b: on-device STA/LTA detection — mirrors shared/stalta.ts EXACTLY ----
   The constants must be THE SAME as STALTA_PARAMS (golden_stalta_check locks this in). */
#define STALTA_ALPHA_HP   0.0115
#define STALTA_N_STA      55.0
#define STALTA_N_LTA      3270.0
#define STALTA_LTA_MIN    1e-8
/* 5,0 -> 4,0 (blep-0.9.27, 2026-09-08, kurucu karari). OLCULDU: 13 etiketli olay +
   14 gun/7 istasyon 1 Hz ROC (gercek tetik ve golge sayaciyla dogrulandi). 4,0'da
   M4,8@77 km kesin, M4,6@77 km yazi-tura; filo yanlis alarmi 54 -> 80 episod/gun.
   3,0 SECILMEDI (137/gun, iki-istasyon cakisma riski ~15x). Sozlesme: shared
   T1_STALTA_PARAMS (telefon yolu STALTA_PARAMS 5,0'da KALDI, bilerek). */
#define STALTA_TRIGGER    4.0
#define STALTA_DETRIGGER  2.0
#define STALTA_WARMUP     3270u
/* ── ON-TETIK LTA DONDURMA (0.9.27) — shared T1_STALTA_PARAMS.ltaFreeze ikizi ─────
   NEDEN: LTA 30 s EMA ve yalniz tetikte donuyordu. 77 km'den 12-13 s yavas yukselen
   M4,6-4,8 sinyalinde LTA da yukselip orani BASIYORDU (1 Hz sim: M4,8 5,90, M4,6 4,66;
   dondurmayla 6,6-7,0 ve 6,1). Oran (yeni sta / eski lta) >= 2,0 ise LTA o ornekte
   uyum saglamaz. Uc kapi: (1) isinma bitmeden ASLA — LTA 1e-8 tabanindan buyurken
   oran devasadir, dondurma tabani kilitlerdi; (2) tetikteyken zaten donuk; (3) SIZDIRAN
   SAYAC frz_n: aday ornekte +1, degilse -1; N_LTA'yi (bir LTA penceresi) asinca
   dondurma BITER — kalici gurultu basamagi (makine acildi) LTA'yi sonsuza dek bayat
   birakamaz. Sifirlamak yerine azaltmak SART (TS'te olculdu): sifirlansaydi oran 2'nin
   altina bir kez dusunce dondurma 30 s daha yeniden baslar, oran 2,0-2,3'te dakikalarca
   asili kalirdi. golden_stalta_freeze_check bu aritmetigi TS ile kilitler. */
#define STALTA_LTA_FREEZE        2.0
#define STALTA_LTA_FREEZE_MAX_N  3270u   /* = STALTA_N_LTA (tavan bir LTA penceresi) */

typedef struct {
    double dc[3], sta, lta; bool armed, triggered; uint32_t count;
    uint32_t frz_n;            /* 0.9.27 on-tetik dondurma sizdiran sayaci (TS: state.frz) */
    /* ---- KOSAN TEPE (2026-09-02) — tetikten de-trigger'a kadar eksen bazli maksimum.
       NEDEN: `trig` mesajindaki pga TEPE DEGIL, tetigin atildigi andaki anlik genlik
       (apps/web/lib/eventPga.ts). Sunucu tarafinda gercek tepe artik telemetriden
       okunuyor, AMA telemetri yalniz VEKTOR tepesini tasiyor (sqrt(x²+y²+z²)).
       Her GMPE, ShakeMap ve PGA->MMI bagintisi MAKSIMUM YATAY kullanir ve duseyi
       asla karistirmaz; olculen fark 1,03-1,16 kat. Eksen bazli tepe yalnizca
       BURADA uretilebilir — telemetri eksen basina RMS tasiyor, tepe tasimiyor.
       Uc skaler; tampon YOK. */
    double pk_v, pk_h, pk_z;   /* vektor · yatay sqrt(hx²+hy²) · dusey |hz| */
    uint32_t pk_n;             /* tepe penceresindeki ornek sayisi (sure = n/fs) */
    bool pk_done;              /* tavan asildi, yayinlandi: bir daha yayinlama */
    bool sh_armed, sh_triggered;   /* golge dedektor (esik STALTA_SHADOW_TRIGGER) */
    /* 0.9.26 bant filtreli OLCUM kanali — birincil yola DOKUNMAZ (golden korunur). */
    double bp_lp15[3], bp_lp2[3], fsta, flta;
} stalta_t;
typedef struct {
    bool fired, released; double ratio, pga;
    double pk_v, pk_h, pk_z; uint32_t pk_n;
} stalta_out_t;

/* Tepe izleme tavani (ornek). ~110 Hz'de 120 s. Dedektor bir sekilde takili kalirsa
   olcum sonsuza kadar yayinlanmadan beklemesin; tavanda YAYINLANIR ama tetik durumu
   DEGISTIRILMEZ (o karar STALTA_DETRIGGER'in isi, burasi yalniz olcum). */
#define STALTA_PEAK_MAX_N 13200u

/* ── GOLGE TETIK SAYACI (2026-09-03) ─────────────────────────────────────────
   NE YAPAR: ikinci bir esikle (3,0) AYNI ratio akisini izler ve kac kez
   ateslerdi diye SAYAR. Hicbir sey yayinlamaz, tetik durumunu degistirmez,
   sta/lta/ratio'ya DOKUNMAZ — golden testi bozulmaz.

   NEDEN VAR: tetiklemeyen olaylarda STA/LTA hicbir yere yazilmiyor. 2026-09-02
   Fiordland dizisinde M4,8 ve M4,6 telemetride tabanin 2,9 ve 2,7 kati iz
   birakti ama esigi (6,0) gecmedi; o an oranin KACA ulastigini bilmiyoruz ve
   geriye donuk hesaplanamaz (telemetri 1 sn tepe, STA penceresi 0,5 sn).

   SORUYU BIREBIR CEVAPLAR: "esik 6 yerine 3 olsaydi kac kez tetiklerdi".
   Maksimum degil SAYAC secildi cunku maksimumdan o sayiya gitmek modelleme
   ister; ayni aralikta iki kez gecen olayi bir kez sayar ve sureyi hic soylemez.

   BIRAKMA MANTIGI SART: esigi gecen her ORNEK degil, her GECIS sayilir. Uzun
   bir gurultu olayi tek olay olmali; bunu ancak durum makinesi dogru yapar.

   GOREVLER ARASI: s_stalta ornekleme gorevinin YEREL degiskeni, saglik paketi
   baska gorevden yayinlaniyor. Sayac bu yuzden dosya kapsaminda ve ATOMIK;
   saglik paketi atomic_exchange ile okur ve SIFIRLAR.

   ⚠️ SIFIRLAMA YAYINDAN ONCE: atomic_exchange snprintf'in ARGUMANI, yani
   publish basarisiz olsa da sayac sifirlanir ve o pencere KAYBOLUR. Bu
   BILINCLI: paket "raporladigim pencerede kac gecis oldu" der; paket
   gitmediyse o pencere gozlenmemistir — heap/rssi ile ayni semantik. Sayac
   BIRIKMEZ, yani degeri her zaman TEK pencereyle sinirlidir (~109 Hz'de,
   gecis basina en az iki ornek: 30 sn icin tavan ~1.635). */
/* ── BANT FILTRELI OLCUM KANALI (0.9.26) ─────────────────────────────────────
   2-15 Hz bant gecirgen; tek-kutuplu LP(15) ve ondan cikarilan LP(2).
   a = 1 - exp(-2*pi*fc/fs), fs=109 Hz nominal (STALTA_ALPHA_HP ile ayni sozlesme).
   BANT NEDEN 2-15: 274YU'nun IKI tetikleyen olayinin (M5,3 ve M5,2) ham dalga
   formundan OLCULDU (12,1 sn @107,86 Hz, P oncesi ~6 sn saf gurultu penceresiyle).
   Enerji SNR kazanci 3-eksende: 2-15 Hz -> 2,82x ve 2,61x (ortalama 2,71x).
   Daha dar bantlar TEK olaya uyuyor, digerinde cokuyor (3-10 Hz: 3,28x ama 1,18x)
   — 2-15 iki olayda da tutarli oldugu icin secildi.
   NEDEN BEDELSIZ: STA/LTA bir ORANDIR; duragan gurultude filtre STA ve LTA'yi
   ayni oranda dusurur, oran degismez. Kazanc sinyalin bantta YOGUN, gurultunun
   tum banda YAYILMIS olmasindan gelir.
   Analiz: docs/plans/2026-09-07-stalta-esik-ve-eksen-kanallari.md */
#define STALTA_BP_A_HI  0.578755   /* LP 15 Hz */
#define STALTA_BP_A_LO  0.108885   /* LP  2 Hz (bant alt kenari) */

#define STALTA_SHADOW_TRIGGER 3.0
static _Atomic uint32_t s_shadow_trig = 0;

/* 0.9.26: pencere ici MAKSIMUM STA/LTA — biri filtresiz (mevcut dedektorun gordugu),
   biri 2-15 Hz filtreli. Oran x100 TAM SAYI olarak saklanir (float bicimlendirme yok,
   rs_a ile ayni idiom). Saglik paketi atomic_exchange ile OKUR ve SIFIRLAR; deger her
   zaman TEK pencereye aittir (sh_trig ile ayni semantik).
   NEDEN MAKSIMUM, sayac degil: gölge sayac "kac kez gecti" der ama esige NE KADAR
   yaklastigini soylemez; filtre kazancini olcmek icin yaklasma mesafesi gerekir. */
static _Atomic uint32_t s_sl_max  = 0;
static _Atomic uint32_t s_slf_max = 0;
/* 0.9.27: pencerede LTA'nin ON-TETIK dondurulmus oldugu ornek sayisi (saglik `lta_frz`).
   sh_trig ile ayni sozlesme: atomic_exchange ile okunur ve sifirlanir, TEK pencereye
   aittir. Tavan ~3270/pencere (30 s x 109 Hz). Sahada dondurmanin GERCEKTEN devreye
   girdigini ve ne kadar surdugunu bu alan gosterir — "hata yoklugu kanit degildir". */
static _Atomic uint32_t s_lta_frz = 0;

/* Kilitsiz maksimum: yalniz buyuk deger yazilir. Tavanda doyar (tasip sifirlanmaz). */
static inline void gs_max_update(_Atomic uint32_t *dst, double ratio) {
    if (!(ratio > 0.0)) return;
    double v = ratio * 100.0;
    uint32_t nv = (v >= 4294967295.0) ? UINT32_MAX : (uint32_t)v;
    uint32_t cur = atomic_load(dst);
    while (nv > cur && !atomic_compare_exchange_weak(dst, &cur, nv)) { }
}

static stalta_out_t stalta_step(stalta_t *s, const double sample[12]) {
    /* 4-sensor per-axis average — EXACTLY like the shared staltaStep (index + op-order).
       The averaging is INSIDE: the function boundary matches TS, and the golden also locks the averaging order. */
    double ax = (sample[0] + sample[3] + sample[6] + sample[9]) / 4.0;
    double ay = (sample[1] + sample[4] + sample[7] + sample[10]) / 4.0;
    double az = (sample[2] + sample[5] + sample[8] + sample[11]) / 4.0;
    double dcx, dcy, dcz;
    if (s->count == 0) { dcx = ax; dcy = ay; dcz = az; }
    else {
        dcx = s->dc[0] + STALTA_ALPHA_HP * (ax - s->dc[0]);
        dcy = s->dc[1] + STALTA_ALPHA_HP * (ay - s->dc[1]);
        dcz = s->dc[2] + STALTA_ALPHA_HP * (az - s->dc[2]);
    }
    double hx = ax - dcx, hy = ay - dcy, hz = az - dcz;
    double cf = hx*hx + hy*hy + hz*hz;
    double sta = s->sta + (cf - s->sta) / STALTA_N_STA;
    /* ON-TETIK LTA DONDURMA (0.9.27) — shared staltaStepWith ile BIREBIR ayni sira:
       aday = !triggered && ESKI count >= WARMUP && sta/ESKI lta >= FREEZE. s->lta hicbir
       zaman LTA_MIN'in altina inmez (init + clamp), bolme guvenli. */
    bool frz_aday = !s->triggered && s->count >= STALTA_WARMUP
                    && (sta / s->lta) >= STALTA_LTA_FREEZE;
    uint32_t frz_n = frz_aday ? s->frz_n + 1 : (s->frz_n ? s->frz_n - 1 : 0);
    bool donuk = s->triggered || (frz_aday && frz_n <= STALTA_LTA_FREEZE_MAX_N);
    double lta = donuk ? s->lta : s->lta + (cf - s->lta) / STALTA_N_LTA;
    if (lta < STALTA_LTA_MIN) lta = STALTA_LTA_MIN;
    double ratio = sta / lta;
    if (donuk && !s->triggered) {   /* yalniz ON-TETIK donmayi say (tetik donmasi eski davranis) */
        uint32_t onceki = atomic_load(&s_lta_frz);
        if (onceki < UINT32_MAX) atomic_fetch_add(&s_lta_frz, 1);
    }
    s->dc[0] = dcx; s->dc[1] = dcy; s->dc[2] = dcz; s->sta = sta; s->lta = lta; s->count++;
    s->frz_n = frz_n;
    bool fired = false;
    if (s->count >= STALTA_WARMUP && s->armed && ratio >= STALTA_TRIGGER) {
        fired = true; s->triggered = true; s->armed = false;
    }
    /* ---- GOLGE DEDEKTOR (yalniz sayar) ---------------------------------------
       Ayni ratio, dusuk esik. Isinma penceresi GERCEK dedektorle ayni; birakma
       esigi de ayni (STALTA_DETRIGGER), yani "esik 3 olsaydi" sorusu birebir
       ayni durum makinesiyle cevaplanir. Sayac tavanda doyar: doygunluk zaten
       "bu istasyon uygun degil" sinyalidir, tasip sifira donmemeli. */
    if (s->count >= STALTA_WARMUP) {
        if (s->sh_armed && ratio >= STALTA_SHADOW_TRIGGER) {
            s->sh_armed = false; s->sh_triggered = true;
            uint32_t onceki = atomic_load(&s_shadow_trig);
            if (onceki < UINT32_MAX) atomic_fetch_add(&s_shadow_trig, 1);
        } else if (s->sh_triggered && ratio <= STALTA_DETRIGGER) {
            s->sh_triggered = false; s->sh_armed = true;
        }
    }

    /* ---- BANT FILTRELI OLCUM KANALI (0.9.26) — YALNIZ OLCER -------------------
       hx/hy/hz'den 2-15 Hz bant gecirgen, ayri bir STA/LTA. TETIKLEMEZ, tetik
       durumuna DOKUNMAZ, sta/lta/ratio'yu DEGISTIRMEZ — golden testi korunur
       (golge dedektorle ayni sozlesme). Amac: filtrenin gercek kazancini SAHADA,
       gercek gurultu ve gercek olaylar uzerinde olcmek; tetige baglamadan once.
       flta ASLA dondurulmaz (olcum kanali, tetik durumu yok). */
    {
        s->bp_lp15[0] += STALTA_BP_A_HI * (hx - s->bp_lp15[0]);
        s->bp_lp15[1] += STALTA_BP_A_HI * (hy - s->bp_lp15[1]);
        s->bp_lp15[2] += STALTA_BP_A_HI * (hz - s->bp_lp15[2]);
        s->bp_lp2[0]  += STALTA_BP_A_LO * (s->bp_lp15[0] - s->bp_lp2[0]);
        s->bp_lp2[1]  += STALTA_BP_A_LO * (s->bp_lp15[1] - s->bp_lp2[1]);
        s->bp_lp2[2]  += STALTA_BP_A_LO * (s->bp_lp15[2] - s->bp_lp2[2]);
        double bx = s->bp_lp15[0] - s->bp_lp2[0];
        double by = s->bp_lp15[1] - s->bp_lp2[1];
        double bz = s->bp_lp15[2] - s->bp_lp2[2];
        double cff = bx*bx + by*by + bz*bz;
        s->fsta += (cff - s->fsta) / STALTA_N_STA;
        s->flta += (cff - s->flta) / STALTA_N_LTA;
        if (s->flta < STALTA_LTA_MIN) s->flta = STALTA_LTA_MIN;
        if (s->count >= STALTA_WARMUP) {
            gs_max_update(&s_sl_max,  ratio);
            gs_max_update(&s_slf_max, s->fsta / s->flta);
        }
    }

    /* ---- KOSAN TEPE ----------------------------------------------------------
       SIRA ONEMLI: izleme tetik kararindan SONRA, de-trigger kararindan ONCE.
       Boylece tetigi ateşleyen ornek tepeye DAHIL olur ve de-trigger'in oldugu
       ornek de sayilir. sta/lta/ratio'ya HIC dokunulmuyor — golden bozulmaz. */
    stalta_out_t o = { fired, false, ratio, sqrt(cf), 0, 0, 0, 0 };
    if (fired) { s->pk_v = s->pk_h = s->pk_z = 0.0; s->pk_n = 0; s->pk_done = false; }
    if (s->triggered && !s->pk_done) {
        double av = sqrt(cf), ah = sqrt(hx*hx + hy*hy), azz = fabs(hz);
        if (av > s->pk_v) s->pk_v = av;
        if (ah > s->pk_h) s->pk_h = ah;
        if (azz > s->pk_z) s->pk_z = azz;
        s->pk_n++;
    }
    bool detrig = (s->triggered && ratio <= STALTA_DETRIGGER);
    bool tavan  = (s->triggered && !s->pk_done && s->pk_n >= STALTA_PEAK_MAX_N);
    if (detrig) { s->triggered = false; s->armed = true; }
    /* Tavanda da yayinla ama tetik durumunu DEGISTIRME (yukaridaki yorum). */
    if ((detrig && !s->pk_done) || tavan) {
        o.released = true;
        o.pk_v = s->pk_v; o.pk_h = s->pk_h; o.pk_z = s->pk_z; o.pk_n = s->pk_n;
        s->pk_done = true;
    }
    return o;
}

static void stalta_init(stalta_t *s) {
    memset(s, 0, sizeof(*s)); s->lta = STALTA_LTA_MIN; s->armed = true; s->sh_armed = true;
    s->flta = STALTA_LTA_MIN;   /* 0.9.26 filtreli kanal */
}

/* Boot golden: must agree with the shared stalta.test.ts "4 sensors DIFFER" golden.
   12-element input → the averaging op-order + the recurrence are locked TOGETHER (review #1).
   The values are THE SAME as the locked literals in stalta.test.ts — if TS changes, update both. */
static bool golden_stalta_check(void) {
    stalta_t s; stalta_init(&s);
    const double seq[3][12] = {
        {9.81,0.10,0.20, 9.79,0.11,0.19, 9.83,0.09,0.21, 9.80,0.12,0.18},
        {9.85,0.12,0.22, 9.82,0.10,0.20, 9.88,0.14,0.24, 9.84,0.11,0.19},
        {9.78,0.08,0.18, 9.81,0.13,0.23, 9.79,0.07,0.17, 9.86,0.15,0.25},
    };
    stalta_out_t o = {0};
    for (int i = 0; i < 3; i++) o = stalta_step(&s, seq[i]);
    bool ok = fabs(s.sta - 0.00003883608158025049) < 1e-12 &&
              fabs(s.lta - 6.74217320754769e-7) < 1e-12 &&
              fabs(o.ratio - 57.60172630506509) < 1e-6;
    if (ok) ESP_LOGI(TAG, "golden STA/LTA OK - C matches the TS reference");
    else ESP_LOGE(TAG, "golden STA/LTA FAIL - sta=%.12f lta=%.12f ratio=%.6f", s.sta, s.lta, o.ratio);
    return ok;
}

/* 0.9.27 golden — ON-TETIK LTA DONDURMA aritmetigi, shared test/stalta-t1-freeze.test.ts
   "golden" ile BIREBIR ayni dizi ve sayilar. Ustteki 3-adimlik golden isinma icinde kalir
   ve dondurmayi HIC calistirmaz; bu dizi 3600 ornek: MINSTD gurultusu (x = x*48271 mod
   2^31-1, carpim < 2^53 -> double ile ayni tam sayilar), 3300'den sonra tum eksenlere
   ±0,003 kare dalga (8 ornek periyot) -> oran 2-4 bandinda: dondurma ACIK, tetik YOK.
   Dondurma kapali olsaydi lta 1,88e-5 / oran 2,64 cikardi (TS'te olculdu) — yani bu
   golden dondurmayi GERCEKTEN sinar. Literaller degisirse TS testi de degisir. */
static bool golden_stalta_freeze_check(void) {
    stalta_t s; stalta_init(&s);
    uint64_t x = 12345;
    stalta_out_t o = {0};
    uint32_t fired = 0;
    for (int i = 0; i < 3600; i++) {
        double smp[12];
        for (int k = 0; k < 4; k++) {
            double base[3] = { 9.81 + 0.01 * k, 0.10 - 0.01 * k, 0.20 + 0.005 * k };
            for (int j = 0; j < 3; j++) {
                x = (x * 48271ULL) % 2147483647ULL;
                double v = base[j] + ((double)x / 2147483647.0 * 0.02 - 0.01);
                if (i >= 3300) v += ((i % 8) < 4) ? 0.003 : -0.003;
                smp[k * 3 + j] = v;
            }
        }
        o = stalta_step(&s, smp);
        if (o.fired) fired++;
    }
    bool ok = fabs(s.sta - 0.000049736206388472704) < 1e-12 &&
              fabs(s.lta - 0.000015752361206703420) < 1e-12 &&
              fabs(o.ratio - 3.1573810259827875) < 1e-6 &&
              s.frz_n == 284 && fired == 0 && !s.triggered;
    if (ok) ESP_LOGI(TAG, "golden STA/LTA FREEZE OK - LTA dondurma TS ile ayni (frz_n=%lu)", (unsigned long)s.frz_n);
    else ESP_LOGE(TAG, "golden STA/LTA FREEZE FAIL - sta=%.12f lta=%.12f ratio=%.6f frz_n=%lu fired=%lu",
                  s.sta, s.lta, o.ratio, (unsigned long)s.frz_n, (unsigned long)fired);
    /* Bu golden s_lta_frz sayacini artirdi (donuk ornekler); ilk saglik paketine sizmasin. */
    atomic_store(&s_lta_frz, 0);
    return ok;
}

/* Boot golden check: the two golden vectors from the shared test/features.test.ts.
   Locks the exact agreement between the C and TS computation (catches mistakes such as
   axis swaps / mean-abs-vs-RMS / a missing /4). */
static bool golden_check(void) {
    float w1[2][12], w2[2][12];
    for (int i = 0; i < 4; i++) {
        w1[0][i*3]=0.02f; w1[0][i*3+1]=0; w1[0][i*3+2]=0;
        w1[1][i*3]=-0.02f; w1[1][i*3+1]=0; w1[1][i*3+2]=0;
        w2[0][i*3]=0.03f; w2[0][i*3+1]=0.04f; w2[0][i*3+2]=0;
        w2[1][i*3]=0.01f; w2[1][i*3+1]=0; w2[1][i*3+2]=0.02f;
    }
    features_t f1 = compute_features(w1, 2), f2 = compute_features(w2, 2);
    bool ok = fabsf(f1.rms[0]-0.02f)<1e-4f && fabsf(f1.rms[1])<1e-4f &&
              fabsf(f1.pga-0.02f)<1e-4f &&
              fabsf(f2.rms[0]-0.0223607f)<1e-4f && fabsf(f2.rms[1]-0.0282843f)<1e-4f &&
              fabsf(f2.rms[2]-0.0141421f)<1e-4f && fabsf(f2.pga-0.05f)<1e-4f;
    if (ok)
        ESP_LOGI(TAG, "golden OK - feature maths matches shared (TS)");
    else
        ESP_LOGE(TAG, "golden FAIL - C/TS MISMATCH! f1=%.5f,%.5f,%.5f/%.5f f2=%.5f,%.5f,%.5f/%.5f",
                 f1.rms[0],f1.rms[1],f1.rms[2],f1.pga, f2.rms[0],f2.rms[1],f2.rms[2],f2.pga);
    return ok;
}

/* ---- Phase 0 .gsd frame (science mode); locked to the Lab format by a golden ----
   magic(A5 5A)|ver|flags|seq(u32)|t_us(u64)|12xi16 RAW|crc16(CCITT-FALSE) */
#define FRAME_LEN 42
static uint16_t crc16_ccitt(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t k = 0; k < n; k++) {
        crc ^= (uint16_t)d[k] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}
static void encode_frame(uint8_t *buf, uint32_t seq, uint64_t t_us, const int16_t s[12]) {
    buf[0] = 0xA5; buf[1] = 0x5A; buf[2] = 1; buf[3] = 0;
    memcpy(&buf[4], &seq, 4);
    memcpy(&buf[8], &t_us, 8);
    memcpy(&buf[16], s, 24);
    uint16_t crc = crc16_ccitt(buf, FRAME_LEN - 2);
    memcpy(&buf[40], &crc, 2);
}
/* Boot golden FRAME check: the hex from Phase 0 test_framing.py::test_golden_frame_vector.
   Locks the C frame encoder EXACTLY to the format the Lab (.gsd parser) reads. */
static bool golden_frame_check(void) {
    static const uint8_t G[FRAME_LEN] = {
        0xa5,0x5a,0x01,0x00, 0x87,0xd6,0x12,0x00, 0xca,0xf3,0xc8,0xf4,0xe5,0x00,0x00,0x00,
        0x64,0x00,0x38,0xff,0x2c,0x01,0x70,0xfe,0xf4,0x01,0xa8,0xfd,0xbc,0x02,0xe0,0xfc,
        0x84,0x03,0x18,0xfc,0x4c,0x04,0x50,0xfb,0x84,0xa1 };
    int16_t s[12] = {100,-200,300,-400,500,-600,700,-800,900,-1000,1100,-1200};
    uint8_t b[FRAME_LEN];
    encode_frame(b, 1234567, 987654321098ULL, s);
    bool ok = (memcmp(b, G, FRAME_LEN) == 0);
    if (ok)
        ESP_LOGI(TAG, "golden FRAME OK - .gsd format matches Lab");
    else
        ESP_LOGE(TAG, "golden FRAME FAIL - C encoder does NOT match the Lab format");
    return ok;
}

/* Science uplink buffer (128 frames x 42 = 5376 bytes; ~1.16 s @110Hz) */
static uint8_t s_sci_buf[128 * FRAME_LEN];

/* Phase 3c: event-triggered waveform — ring (pre-event) + collect (post-event) */
#define RING_FRAMES 660                 /* ~6.0 s @110Hz pre-trigger — even on an S-wave trigger P is still in the buffer (R<=50 km; S-P=R/8.4) */
#define WAVE_FRAMES 1320                /* PSRAM YOKKEN yedek boyut: ring + ~6 s post (~12 s).
                                           PSRAM varsa WAVE_FRAMES_PSRAM kullanilir (asagida). */

/* ---- Paced wave publish (2026-08-20) ----------------------------------------
   MEASURED PROBLEM: the old loop handed all ~11 chunks to esp-mqtt in one burst.
   esp_mqtt_client_publish COPIES the payload, so the outbox briefly held the whole
   wave (~55.8 KB). Over a 20-hour run the free-heap floor hit 14,256 B and EVERY dip
   below 40 KB coincided with a trigger — the margin collapsed exactly during seismic
   activity. At that level the device keeps running, but a WiFi drop needing a fresh
   TLS handshake (~20-30 KB) would fail: the station would leave the network at the
   one moment it exists for.

   FIX: hand over ONE chunk at a time, and only while the outbox is below a floor, so
   the transient is ~FLOOR + one chunk (~17 KB) instead of the whole wave.

   NON-BLOCKING ON PURPOSE: this runs inside sample_task. A vTaskDelay here would stall
   sampling — the one thing that must never pause. Hence a state machine: at most one
   chunk per task iteration (~110 Hz), never waiting. */
#define WAVE_OUTBOX_FLOOR   12288       /* bytes: queue the next chunk only below this */
#define WAVE_PUB_TIMEOUT_US 30000000    /* 30 s @1320 kare: s_wave_buf'in kilitli kaldigi tavan */
static uint8_t s_ring[RING_FRAMES * FRAME_LEN];

/* ---- UYARLANABILIR PENCERE (2026-09-02) ------------------------------------
   OLCULEN SORUN: pencere SABIT 6 s tetik-sonrasi. Fiordland M5,5'te (76,5 km)
   tepe sarsinti kokenden +28 s'de geldi; pencere +19 s'de kapandi. Guclu
   sarsintinin yalnizca %19'u arsive girdi ve S dalgasi HIC girmedi
   (S ancak R_hip <= 51 km'de sigar).
   Daha uzun kaydetmenin onundeki engel BELLEKTI: 12 s = 55,4 KB ve bos heap
   ~70 KB idi. Modulde 8 MB PSRAM VAR ama firmware'de hic acilmamisti.
   ⚠️ PSRAM YOKSA DAVRANIS BIREBIR AYNI KALIR: tampon statik dizide kalir,
   kapasite 1320'dir ve asagidaki uyarlanabilir dal 6 s'den once ATESLENEMEZ
   (tampon zaten tam 6 s'de dolar). Yani bu degisiklik PSRAM acilana kadar ATIL. */
#define WAVE_FRAMES_PSRAM   3300        /* ~6 s on + ~24 s post (~30 s) = 138,6 KB */
#define WAVE_POST_MIN_US     6000000    /* en az 6 s — bugunku taban, kisaltilmiyor */
#define WAVE_POST_MAX_US    24000000    /* tavan; WAVE_FRAMES_PSRAM kapasitesiyle uyumlu */
#define WAVE_PUB_US_PER_FRAME 22727     /* 30 s / 1320 kare — eski tavani BIREBIR korur */
/* ⚠️ PSRAM ACIKKEN STATIK DIZI DERLENMEZ. Sebep olculdu (2026-09-02): statik
   birakilirsa 55.440 bayt DAHILI RAM'i bosuna tutar ve asil ihtiyac orada —
   mbedTLS dahili RAM'den ayirmak ZORUNDA (bir alttaki nota bak). Tamponu PSRAM'e
   tasimak ayni anda dahili RAM'i de 55 KB rahatlatiyor: tek degisiklik, iki kazanc. */
#if CONFIG_SPIRAM
static uint8_t *s_wave_buf = NULL;
static int s_wave_cap = 0;
#else
static uint8_t s_wave_static[WAVE_FRAMES * FRAME_LEN];
static uint8_t *s_wave_buf = s_wave_static;
static int s_wave_cap = WAVE_FRAMES;
#endif

/* Tamponu PSRAM'e tasi (varsa). Basarisizlik SESSIZ DEGIL ama OLUMCUL de degil:
   statik dizide kalinir ve cihaz bugunku gibi calisir. */
static void wave_buf_init(void) {
#if CONFIG_SPIRAM
    uint8_t *p = heap_caps_malloc((size_t)WAVE_FRAMES_PSRAM * FRAME_LEN, MALLOC_CAP_SPIRAM);
    if (p) {
        s_wave_buf = p; s_wave_cap = WAVE_FRAMES_PSRAM;
        ESP_LOGI(TAG, "wave buffer PSRAM'de: %d kare (%d bayt, ~%.1f s)",
                 s_wave_cap, s_wave_cap * FRAME_LEN, s_wave_cap / 110.0);
    } else {
        /* PSRAM derlenmis ama ayirma basarisiz: dahili RAM'e eski boyutta dus.
           Dalga kaybetmek, cihazi kaybetmekten iyidir. */
        p = heap_caps_malloc((size_t)WAVE_FRAMES * FRAME_LEN, MALLOC_CAP_INTERNAL);
        s_wave_buf = p; s_wave_cap = p ? WAVE_FRAMES : 0;
        ESP_LOGE(TAG, "PSRAM ayrilamadi - dahili yedege dusuldu (%d kare)", s_wave_cap);
    }
#else
    ESP_LOGI(TAG, "PSRAM kapali - wave buffer dahili RAM'de (%d kare)", s_wave_cap);
#endif
}

/* 1 s window buffer (~109 samples at 109 Hz; 160 is a safe upper bound) */
#define WIN_MAX 160
static float s_win[WIN_MAX][12];

/* Features of the last window (written by the sample task, read by the MQTT task in B3) */
static features_t s_last_feat;
static int s_last_n = 0;
static double s_last_fs = 0;
static volatile uint32_t s_feat_seq = 0;  /* incremented on every new window */

/* ---- Orientation: the 4-sensor averaged gravity (DC) vector of the last window, m/s^2 ----
   At window close, gravity was ALREADY being computed as mean[12] and thrown away; now it is
   kept. On a stationary accelerometer this vector IS DIRECTLY the device's orientation:
   flat on a table -> |gz| ~ 9.81, X/Y ~ 0;  on a wall -> one horizontal axis ~ 9.81, gz ~ 0.
   There is no need to ask the user "did you mount it on a wall"; if the device is moved it updates itself.
   It DOES NOT TOUCH the detection path (STA/LTA) — mean[] is only read, nothing is changed.
   Writer: sample_task (core1), INSIDE the s_feat_seq seqlock (the same section as s_last_feat).
   Readers: mqtt_task (core0, health) + sample_task (trigger). grav_suffix applies the seqlock.
   Until the first window closes s_grav_ok=false -> the field is NEVER added to the packet (no fake [0,0,0]). */
static float s_last_grav[3] = {0, 0, 0};
static volatile bool s_grav_ok = false;

/* Build the orientation JSON suffix (a SHARED single source for trig + health): ",\"grav\":[x,y,z]" or an EMPTY string.
   Returning empty is normal (before the first window) — the field is OPTIONAL, the packet is still valid JSON.
   %.3f: 0.001 m/s^2 ~ 1.7 LSB; the resulting tilt resolution at the consumer is atan(0.001/9.81) ~ 0.006 deg
   -> far more than enough for classification, at minimal payload cost (+28 bytes/packet).
   Returns: the length written (0 = no suffix). On overflow/encoding error -> the suffix is skipped (the packet is not corrupted). */
static int grav_suffix(char *out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = '\0';
    if (!s_grav_ok) return 0;
    /* seqlock: THE SAME idiom as the s_last_feat read — no torn vector is read at a window boundary. */
    uint32_t seq; float g[3];
    do { seq = s_feat_seq; memcpy(g, s_last_grav, sizeof(g)); } while (seq != s_feat_seq);
    int len = snprintf(out, cap, ",\"grav\":[%.3f,%.3f,%.3f]", g[0], g[1], g[2]);
    if (len < 0 || (size_t)len >= cap) { out[0] = '\0'; return 0; }
    return len;
}

/* SINGLE-SOURCE format for the trigger payload: on overflow it is rebuilt with the same format but
   WITHOUT the orientation suffix (so two separate literals cannot be kept and drift apart). The final %s = the grav_suffix output. */
#define TRIG_FMT "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"sta_lta\":%.3f,\"pga\":%.6f%s}"
/* KOSAN TEPE mesaji — tetikle AYNI seq'i tasir, sunucu ikisini eslestirir.
   pk_h/pk_z AYRI verilir: standart PGA sozlesmesi MAKSIMUM YATAY'dir, vektor degil.
   dur_ms = tepe penceresinin suresi (tetikten de-trigger'a) — kisa sure yerel
   darbeye, uzun sure gercek sarsintiya isaret eder. */
#define PEAK_FMT "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"pk_h\":%.6f,\"pk_z\":%.6f,\"pk_v\":%.6f,\"dur_ms\":%lu,\"n\":%lu}"
static uint32_t s_peak_seq = 0;   /* son tetigin seq'i — tepe mesajinda tekrar edilir */

/* #282: was 60 s. A power outage reboots the device AND the router together; a home
   modem/router needs 1-3 min (often more) to boot and bring up WiFi, so 60 s tripped the
   captive fallback while the AP was simply not up yet. 180 s keeps the STA retrying long
   enough to catch a normal modem boot before any fallback. */
#define WIFI_FALLBACK_TIMEOUT_MS 180000  /* N2d/#282: captive only if no IP after 3 min at boot */
/* #282: how long to hold the recovery captive before REBOOTING to re-attempt the STA. Without
   this the guard sat in captive forever and a power-outage device needed a MANUAL power-cycle
   (QA2HF/BH2JN: 69-92 min dead after a 30 min outage). Reboot -> fresh STA attempt -> a router
   that finished booting is picked up; the device self-heals. Generous window so a customer
   entering new WiFi in the portal finishes first (a successful save reboots immediately anyway). */
#define CAPTIVE_REBOOT_AFTER_MS  300000  /* 5 min */
/* 0.9.14 (#282 takip): CALISAN cihaz WiFi'yi ortada kaybederse (modem kapatilir ama sensor guclu
   kalir) wifi_guard_task KAPSAMAZ (o guard ilk baglantidan sonra kendini siler). Bu kadar sure
   BAGLANDIKTAN SONRA bagli degilse temiz bir yeniden-denemeye reboot et. Normal reconnect (saniyeler)
   cok ustunde, boylece kisa blip'ler reboot ETMEZ. */
#define WIFI_MIDRUN_REBOOT_MS 600000  /* 10 min kesintisiz mid-run down -> reboot */
/* 0.9.20 — MQTT kolu esikleri. Normal yeniden baglanma SANIYELER surer, bu yuzden
   ikisi de o olcegin cok ustunde. Once yumusak deneme, cozmezse reboot. */
#define MQTT_MIDRUN_NUDGE_MS   300000  /*  5 dk: esp_mqtt_client_reconnect ile durt */
#define MQTT_MIDRUN_REBOOT_MS 1800000  /* 30 dk: hala yoksa temiz boot */

/* WiFi/time state (read by the status task) */
static volatile bool s_wifi_up = false;
static volatile uint32_t s_wifi_rc = 0;   /* reconnect counter */
static volatile bool s_wifi_ever_up = false;     /* N2d: the first GOT_IP -> the creds are valid (fallback cancelled) */
static volatile bool s_captive_fallback = false; /* N2d: in-place captive is active -> STOP the STA reconnect */
static volatile uint32_t s_samples = 0;   /* total samples (DRDY) */
static esp_ip4_addr_t s_ip;

/* MQTT state (read by status_task, used by the mqtt block) */
static esp_mqtt_client_handle_t s_mqtt;
/* ---- 0.9.21: YAZILIM YENIDEN BASLATMA SEBEBI --------------------------------
   NEDEN VAR: esp_reset_reason() butun esp_restart() cagrilarini tek deger olarak
   bildirir (3 = SW). Yani OTA mi, WiFi watchdog mi, MQTT watchdog mi ayirt edilemiyor
   ve sahada "duzeltme calisti mi" SORULAMIYOR. Bugun (2026-09-05) tam olarak bunu
   yasadik: MQTT watchdog eklendi ama filo verisinde ateslemesini gorecek alan yok.
   NVS AD ALANI AYRI (seqstore/cal ile ayni gerekce): reset-id kimlik ad alanini
   butunuyle siler, bu kayit ondan uzun yasamali.
   BEST-EFFORT: yazma basarisiz olursa sebep bilinmez olur, davranis DEGISMEZ —
   ama 0.9.23'ten beri hata LOGLANIR (asagidaki iki fonksiyonun notu). */
#define GS_RST_NS  "geoshake_rst"
#define GS_RST_KEY "sw"
enum { GS_RST_NONE = 0, GS_RST_WIFI_WD = 1, GS_RST_MQTT_WD = 2, GS_RST_CAPTIVE = 3,
       GS_RST_OTA = 4, GS_RST_ID = 5, GS_RST_WIFI_CMD = 6, GS_RST_SELFPROV = 7 };
static uint8_t s_sw_rst = GS_RST_NONE;   /* bu acilisi TETIKLEYEN sebep (bir kez bildirilir) */

/* esp_restart()'tan HEMEN once cagrilir. commit sart: restart yazmayi beklemez.
   0.9.23: HER hata yolu LOGLAR. 0.9.21'de bu iki fonksiyon sessiz return ile yaziliydi;
   gs_take_restart_reason nvs_flash_init()'ten once cagrilinca ESP_ERR_NVS_NOT_INITIALIZED
   sessizce 0'a donustu ve filo bir gun boyunca sw_rst=0 bildirdi — seri konsolda TEK
   uyari satiri bunu 30 saniyede gosterirdi. Kural: best-effort "sessiz" demek DEGILDIR;
   davranis degismez, ama hata GORUNUR. Tezgah kabulu: ad alani bozuk bir derlemede bu
   satirlar seri logda cikmali (mutasyon testi, docs/runbooks/firmware-surum-kapisi.md). */
static void gs_note_restart(uint8_t code) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(GS_RST_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "rst: nvs_open failed (%s) - reason %u NOT WRITTEN",
                 esp_err_to_name(e), (unsigned)code);
        return;
    }
    e = nvs_set_u8(h, GS_RST_KEY, code);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "rst: could not write reason %u (%s)", (unsigned)code, esp_err_to_name(e));
    nvs_close(h);
}
/* Acilista bir kez: oku, SIL (yoksa sonraki acilisa sizar), dondur.
   NOT_FOUND beklenen durumdur (son acilis yazilim restart'i degildi) ve loglanmaz;
   geri kalan her hata loglanir. */
static uint8_t gs_take_restart_reason(void) {
    nvs_handle_t h; uint8_t v = GS_RST_NONE;
    esp_err_t e = nvs_open(GS_RST_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "rst: nvs_open failed (%s) - reason NOT READ, sw_rst=0 will be sent",
                 esp_err_to_name(e));
        return GS_RST_NONE;
    }
    e = nvs_get_u8(h, GS_RST_KEY, &v);
    if (e != ESP_OK) {
        v = GS_RST_NONE;
        if (e != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "rst: read failed (%s)", esp_err_to_name(e));
    }
    if (v != GS_RST_NONE) {
        e = nvs_erase_key(h, GS_RST_KEY);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e != ESP_OK)
            ESP_LOGW(TAG, "rst: erase failed (%s) - the reason may LEAK into the next boot",
                     esp_err_to_name(e));
    }
    nvs_close(h);
    return v;
}

static volatile bool s_mqtt_up = false;
/* 0.9.20: MQTT kolu icin iki bayrak. s_mqtt_ever_up ACILISI korur — cihaz ilk kez
   baglanmadan watchdog'un konusmasi anlamsiz olurdu (o yolu boot-time WiFi guard'i
   zaten kapsiyor). s_ota_busy bildirimini buraya TASIDIM (once 1182'deydi, watchdog'un
   ALTINDA kaliyordu); tanim ayni, yalniz gorunurluk one alindi. */
static volatile bool s_mqtt_ever_up = false;
static volatile bool s_ota_busy = false;
static volatile uint32_t s_mqtt_rc = 0;
/* 0.9.24: MQTT baglanma teshis sayaclari. s_mqtt_try her DENEME (BEFORE_CONNECT),
   s_mqtt_fail her tasima/TLS hatasi (EVENT_ERROR). Takili cihaz "try 50, fail 50"
   gosterir (surekli deniyor ama TLS'te oluyor); "try 0" ise istemci hic denemiyor —
   iki farkli ariza, iki farkli duzeltme. wifi_rc/mqtt_rc gibi kumulatif (acilistan beri). */
static volatile uint32_t s_mqtt_try = 0;
static volatile uint32_t s_mqtt_fail = 0;

/* LED burst trigger: the ms stamp of the trigger moment (esp_timer_get_time()/1000).
   led_task reads the burst window from here; sample_task sets it at the det.fired moment. */
static volatile int64_t s_trig_flash_ms = 0;

/* Boot golden health flag: true only if ALL 4 golden checks (features+frame+STA/LTA+LTA-freeze) pass.
   led_task drives panic mode from here (golden FAIL → C/TS mismatch → strobe alarm). */
static volatile bool s_golden_ok = false;

/* Science mode: mqtt_event only writes a REQUEST (an atomic 32-bit int); sample_task is the
   sole owner — only it holds sci_on + the deadline (no cross-core race). */
#define SCI_REQ_NONE (-1000000)
static volatile int32_t s_sci_req = SCI_REQ_NONE;  /* secs: <0 stop, 0 unbounded, >0 seconds */

/* CAL-T4: the 'cal' cmd flag — set by mqtt_event (core0), consumed by sample_task (core1).
   The sci idiom (bool is atomic; single owner). The measurement runs ONLY when it is set -> the happy path is byte-identical. */
static volatile bool s_cal_req = false;

/* ---------------- SPI / LSM6DSO ---------------- */
static uint8_t read_reg(int i, uint8_t reg) {
    uint8_t tx[2] = {(uint8_t)(reg | 0x80), 0x00}, rx[2] = {0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev[i], &t));
    return rx[1];
}
static void write_reg(int i, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = {reg, val};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev[i], &t));
}
static void read_accel(int i, int16_t out[3]) {
    /* Returns the RAW sensor value — it DOES NOT APPLY the gain. The CAL-T4 measurement branch must
       read the raw magnitude (this prevents a calibrate-on-top-of-calibrate feedback bug). The gain is
       applied ONLY in sample_task, via apply_cal, to the normal pipeline's acc. Keep them separate. */
    uint8_t tx[7] = {(uint8_t)(REG_OUTX_L_A | 0x80)}, rx[7] = {0};
    spi_transaction_t t = {.length = 56, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev[i], &t));
    for (int a = 0; a < 3; a++)
        out[a] = (int16_t)(rx[1 + 2 * a] | (rx[2 + 2 * a] << 8));
}

/* CAL-T3: int16 saturation (clips overflow when the gain is applied). At gain=1.0 it never fires. */
static inline int16_t clamp_i16(long v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

/* CAL-T3: apply the per-sensor gain to the acc that read_accel filled in (sensor-major).
   Called BEFORE ALL consumers (stalta_step / encode_frame / science); THE SAME operation at both
   read sites -> consistency. gain=1.0 -> a byte-identical no-op: int16 [-32768,32767] is exactly
   representable in float, *1.0f is exact, lroundf is exact, and clamp is the identity at the bounds -> acc does not change. */
static inline void apply_cal(int16_t acc[4][3]) {
    for (int i = 0; i < 4; i++)
        for (int a = 0; a < 3; a++)
            acc[i][a] = clamp_i16(lroundf((float)acc[i][a] * s_cal_gain[i]));
}

static void IRAM_ATTR drdy_isr(void *arg) {
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_sample_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* B1: read and count samples (does SPI/DRDY work while WiFi is loaded?).
   Feature computation is added to this task in B2, MQTT publish in B3. */
static int64_t now_ms(void);   /* forward declaration — for the sample_task trigger t_ms (defined below) */
static void sample_task(void *arg) {
    int16_t acc[4][3];
    /* DRDY self-test: clear a stuck INT with a read (it is a level signal) */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0) {
        for (int i = 0; i < 4; i++) read_accel(i, acc[i]);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) == 0)
            ESP_LOGE(TAG, "DRDY MISSING - check GPIO%d / IMU1 INT1 line", PIN_DRDY);
        else
            ESP_LOGI(TAG, "DRDY OK (via primer read)");
    } else {
        ESP_LOGI(TAG, "DRDY OK");
    }
    int n = 0;
    stalta_t s_stalta; stalta_init(&s_stalta);   /* Phase 2b: on-device detection state (this task only) */
    /* Resumes from NVS instead of 0: waveforms is keyed (device_id, trig_seq), so a counter
       that restarts every boot made new waves OVERWRITE the old rows. See seqstore.h. */
    uint32_t s_trig_seq = gs_seq_begin();
    uint32_t s_trig_reserved = s_trig_seq + GS_SEQ_RESERVE;
    int ring_w = 0, ring_count = 0;           /* circular ring write index + frames filled */
    uint32_t wave_fseq = 0;                   /* ring frame seq counter */
    bool wave_on = false; int64_t wave_deadline = 0, wave_hard = 0; uint32_t wave_seq = 0; int wave_n = 0;
    /* Wave PUBLISH state (2026-08-20). wave_pub_o < 0 means "not publishing"; otherwise it is the
       offset of the next chunk still to be handed to the MQTT client. See the block below for why
       the chunks are no longer pushed in one burst. */
    int wave_pub_o = -1, wave_pub_n = 0; int64_t wave_pub_deadline = 0;
    int64_t win_start = 0;
    int sci_off = 0;          /* bytes in the science batch buffer */
    uint32_t sci_fseq = 0;    /* frame counter within the session (gap detection) */
    uint32_t sci_dropped = 0; /* mqtt down + buffer full -> dropped frames (I3) */
    bool sci_on = false;      /* science state — only this task writes it */
    int64_t sci_deadline = 0; /* 0 = unbounded */
    bool prev_sci = false;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
            for (int i = 0; i < 4; i++) read_accel(i, acc[i]); /* recover a missed edge */
            apply_cal(acc);   /* CAL-T3: the catch-up read is calibrated too (consumer consistency) */
            continue;
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < 4; i++) read_accel(i, acc[i]);
        apply_cal(acc);       /* CAL-T3: apply the gain — BEFORE stalta/encode/science (gain=1.0 -> no-op) */
        s_samples++;

        /* Phase 3c: write the raw frame to the ring (stamped in epoch µs — wave has its own path, separate from science's boot-µs) */
        int64_t ep_ms = now_ms();
        int16_t s12r[12];
        for (int i = 0; i < 4; i++) for (int a = 0; a < 3; a++) s12r[i*3 + a] = acc[i][a];
        encode_frame(&s_ring[ring_w * FRAME_LEN], wave_fseq++, (uint64_t)(ep_ms * 1000), s12r);
        ring_w = (ring_w + 1) % RING_FRAMES;
        if (ring_count < RING_FRAMES) ring_count++;

        /* ---- Phase 2b: STA/LTA detection (per sample) ---- */
        {
            /* Scale per sensor (acc*SCALE) → average INSIDE stalta_step. This matches the shared
               staltaStep's input contract exactly (sample[i]=raw*SCALE) and the op-order is the
               same; golden_stalta_check locks this in with a differing-sensor sequence. */
            double s12d[12];
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++) s12d[i*3 + a] = (double)acc[i][a] * SCALE;
            stalta_out_t det = stalta_step(&s_stalta, s12d);
            if (det.fired && ep_ms > 0 && s_mqtt_up) {
                uint32_t used_seq = s_trig_seq++;
                s_peak_seq = used_seq;      /* tepe mesaji ayni seq ile eslesir */
                /* Reservation used up → reserve the next block. One flash write per
                   GS_SEQ_RESERVE triggers; the trigger path itself is untouched. */
                if (s_trig_seq >= s_trig_reserved) {
                    s_trig_reserved = s_trig_seq + GS_SEQ_RESERVE;
                    gs_seq_checkpoint(s_trig_seq);
                }
                char gsuf[40];                 /* ",\"grav\":[-19.613,-19.613,-19.613]" = 33 + NUL */
                grav_suffix(gsuf, sizeof(gsuf));
                /* tb[] size budget — MEASURED with snprintf on the host (not guessed):
                     field (GEO-XXXXX): without suffix  86 -> with grav 114 bytes
                     worst case      : without suffix 134 -> with grav 167 (+NUL 168)
                       [dev 31, seq 2^32-1, t_ms int64-max, sta_lta 1.15e11 (sta<=3*(2g)^2,
                        lta>=1e-8), pga 33.97, grav components saturated at +-2g]
                   160 WOULD NOT HAVE BEEN ENOUGH (167 > 159) -> 192 (24 bytes of headroom). */
                char tb[192];
                int len = snprintf(tb, sizeof(tb), TRIG_FMT,
                    g_id.device_id, (unsigned long)used_seq, (long long)ep_ms,
                    det.ratio, det.pga, gsuf);
                if (len < 0 || len >= (int)sizeof(tb)) {
                    /* Unexpected overflow: NEVER drop the trigger (it is seismically critical) —
                       drop the optional orientation suffix and rebuild (without it <=134 bytes, guaranteed to fit). */
                    ESP_LOGW(TAG, "trigger payload too long (%d) - republishing without orientation", len);
                    len = snprintf(tb, sizeof(tb), TRIG_FMT,
                        g_id.device_id, (unsigned long)used_seq, (long long)ep_ms,
                        det.ratio, det.pga, "");
                }
                if (len > 0 && len < (int)sizeof(tb))
                    esp_mqtt_client_publish(s_mqtt, g_topic_trig, tb, len, 1, 0);
                else
                    ESP_LOGE(TAG, "trigger payload encode failed (%d) - dropped", len);
                ESP_LOGW(TAG, "TRIGGER! ratio=%.2f pga=%.4f t_ms=%lld", det.ratio, det.pga, (long long)ep_ms);
                s_trig_flash_ms = esp_timer_get_time() / 1000;  /* THE SAME monotonic base as led_task → the burst fades after ~LED_BURST_MS (not epoch-ms) */
                /* wave_pub_o >= 0 → the PREVIOUS wave is still being handed to the client and
                   s_wave_buf must not be overwritten. Before the paced publish existed this could not
                   happen (the burst emptied the buffer within one iteration). */
                if (!wave_on && wave_pub_o < 0 && s_wave_buf && s_wave_cap > 0) {   /* copy the pre-event ring into wave_buf in chronological order */
                    wave_n = 0;
                    int start = (ring_count == RING_FRAMES) ? ring_w : 0;  /* oldest */
                    for (int k = 0; k < ring_count; k++) {
                        int idx = (start + k) % RING_FRAMES;
                        memcpy(&s_wave_buf[wave_n * FRAME_LEN], &s_ring[idx * FRAME_LEN], FRAME_LEN);
                        wave_n++;
                    }
                    wave_on = true; wave_seq = used_seq;
                    wave_deadline = now + WAVE_POST_MIN_US;        /* en az bu kadar */
                    wave_hard = now + WAVE_POST_MAX_US;            /* asla bunu gecme */
                }
            }

            /* ---- KOSAN TEPE yayini: de-trigger (ya da tavan) aninda -------------
               Tetik mesaji olayin BASINDA gider ve icindeki `pga` yalnizca o andaki
               genliktir. Bu mesaj olayin SONUNDA gider ve olcunun kendisini tasir.
               AYRI BIR KONU: `trig` sozlesmesi degismiyor, eski tuketiciler etkilenmiyor.
               Sunucu bu konuyu HENUZ dinlemiyor — o zamana kadar broker'da dusuyor,
               zarari yok (ACL geoshake/# altina izin veriyor). */
            if (det.released && ep_ms > 0 && s_mqtt_up) {
                double fs = (s_last_fs > 1.0) ? s_last_fs : 110.0;
                unsigned long dur_ms = (unsigned long)((det.pk_n * 1000.0) / fs);
                char pb[224];
                int pl = snprintf(pb, sizeof(pb), PEAK_FMT,
                    g_id.device_id, (unsigned long)s_peak_seq, (long long)ep_ms,
                    det.pk_h, det.pk_z, det.pk_v, dur_ms, (unsigned long)det.pk_n);
                if (pl > 0 && pl < (int)sizeof(pb))
                    esp_mqtt_client_publish(s_mqtt, g_topic_peak, pb, pl, 1, 0);
                ESP_LOGW(TAG, "PEAK #%lu h=%.4f z=%.4f v=%.4f dur=%lums n=%lu",
                         (unsigned long)s_peak_seq, det.pk_h, det.pk_z, det.pk_v, dur_ms,
                         (unsigned long)det.pk_n);
            }
        }

        /* Phase 3c: if wave capture is active, collect the post-event frames; publish when finished */
        if (wave_on) {
            if (wave_n < s_wave_cap)   /* the current frame was written to the ring; append the same one to wave */
                memcpy(&s_wave_buf[wave_n++ * FRAME_LEN], &s_ring[((ring_w + RING_FRAMES - 1) % RING_FRAMES) * FRAME_LEN], FRAME_LEN);
            /* BITIS: (a) tampon doldu, (b) tavan, ya da (c) en az sure gecti VE
               sarsinti dindi. (c) uzun olaylarda pencereyi acik tutar, kisa yerel
               darbede bant genisligi harcamaz. PSRAM yokken (a) her zaman ~6 s'de
               once gerceklesir -> davranis degismez. */
            if (wave_n >= s_wave_cap || now >= wave_hard ||
                (now >= wave_deadline && !s_stalta.triggered)) {
                /* Capture done → hand it over to the PACED publisher below. */
                wave_pub_o = 0; wave_pub_n = 0;
                wave_pub_deadline = now + (int64_t)wave_n * WAVE_PUB_US_PER_FRAME;
                wave_on = false;
            }
        }

        /* Phase 3d: paced wave publish — at most ONE chunk per iteration, never blocking.
           s_wave_buf stays owned by this state until it finishes, so a new trigger cannot
           start a capture meanwhile (guarded at the trigger site). */
        if (wave_pub_o >= 0) {
            if (now >= wave_pub_deadline) {
                /* The link never drained in time. Give the buffer back rather than locking
                   capture out indefinitely — a missed waveform beats a deaf station. */
                ESP_LOGW(TAG, "WAVE #%lu timeout: %d/%d frames handed over",
                         (unsigned long)wave_seq, wave_pub_n, wave_n);
                wave_pub_o = -1;
            } else if (s_mqtt_up && esp_mqtt_client_get_outbox_size(s_mqtt) <= WAVE_OUTBOX_FLOOR) {
                char wt[64];
                snprintf(wt, sizeof(wt), "geoshake/%s/wave/%lu", g_id.device_id, (unsigned long)wave_seq);
                int cnt = (wave_n - wave_pub_o > 128) ? 128 : (wave_n - wave_pub_o);
                int mid = esp_mqtt_client_publish(s_mqtt, wt,
                              (const char *)&s_wave_buf[wave_pub_o * FRAME_LEN], cnt * FRAME_LEN, 1, 0);
                if (mid < 0) {
                    /* enqueue failed (-1 error, -2 outbox cap): memory IS the problem here — forcing
                       the remaining chunks would make it worse. Bail; the wave stays partial
                       server-side and the detection path is untouched. */
                    ESP_LOGW(TAG, "wave chunk publish fail (%d) @%d/%d", mid, wave_pub_o, wave_n);
                    wave_pub_o = -1;
                } else {
                    wave_pub_n += cnt;
                    wave_pub_o += cnt;
                    if (wave_pub_o >= wave_n) {
                        ESP_LOGW(TAG, "WAVE #%lu published: %d/%d frames",
                                 (unsigned long)wave_seq, wave_pub_n, wave_n);
                        wave_pub_o = -1;
                    }
                }
            }
            /* else: outbox still draining (or link down) → retry next iteration, no wait */
        }

        /* Consume the command: the science request posted by mqtt_event — ONLY this task holds
           the deadline/on state (I1/I2: no cross-core race). */
        int32_t req = s_sci_req;
        if (req != SCI_REQ_NONE) {
            s_sci_req = SCI_REQ_NONE;
            if (req < 0) { sci_on = false; }
            else if (req == 0) { sci_on = true; sci_deadline = 0; }
            else { int v = req > 300 ? 300 : req; sci_on = true; sci_deadline = now + (int64_t)v * 1000000; }
            ESP_LOGI(TAG, "science %s", !sci_on ? "KAPALI" : (sci_deadline ? "ACIK (sureli)" : "ON (unbounded)"));
        }

        /* ---- CAL-T4: auto still-state relative-gain measurement (sci idiom: mqtt_event posts a flag) ----
           Runs ONLY when a 'cal' cmd arrives -> the normal flow is byte-identical. Collect ~1.5 s of RAW
           magnitudes (NO apply_cal: this prevents a calibrate-on-top feedback bug), verify still+alive,
           compute the relative gain, save it to NVS + apply it to s_cal_gain (keep the write INSIDE
           core1/sample_task: reader=writer=core1 -> no tearing). Throughout the measurement window
           STA/LTA/encode/science are STOPPED (the operator holds the device still for ~1.5 s).
           Every path (success/reject) clears s_cal_req. */
        if (s_cal_req) {
            s_cal_req = false;                 /* single-owner consumption: run once */
            double sum[4] = {0}, sumsq[4] = {0};
            int got = 0;
            bool reject = false;   /* ALL reject paths (including dead-DRDY) — it can also be set INSIDE the loop */
            int stalls = 0;        /* consecutive DRDY-timeout counter; a live edge resets it */
            while (got < CAL_SAMPLES) {
                /* Wait for DRDY -> the task blocks and the WDT is fed. BUT DO NOT MIRROR the main loop's
                   INFINITE wait: the main loop exists to run forever, whereas the cal window MUST END.
                   DRDY comes from a SINGLE source, sensor-0/IMU1 (PIN_DRDY); if it dies during the
                   measurement the loop never advances -> count consecutive timeouts and REJECT at 3
                   (~3 s; at ~110Hz that is a definitively dead line, with plenty of margin for jitter):
                   the old gain is preserved and sample_task returns to the normal flow (NO hang). */
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
                    if (++stalls >= 3) {
                        ESP_LOGW(TAG, "cal rejected: no DRDY / timeout - keeping previous gain");
                        reject = true; break;
                    }
                    continue;      /* a single missed edge; wait again */
                }
                stalls = 0;        /* a live edge arrived -> reset the counter */
                for (int i = 0; i < 4; i++) {
                    int16_t raw[3];
                    read_accel(i, raw);        /* RAW — NO apply_cal (the measurement must be on raw magnitudes) */
                    /* int16^2 x3 overflows int32 -> cast to float BEFORE squaring. */
                    float m = sqrtf((float)raw[0] * (float)raw[0] +
                                    (float)raw[1] * (float)raw[1] +
                                    (float)raw[2] * (float)raw[2]);
                    sum[i]   += m;
                    sumsq[i] += (double)m * m;
                }
                got++;
            }
            /* If a dead-DRDY reject lands here, the ENTIRE analysis below is skipped
               (NO division on an incomplete/empty accumulator) -> straight to n=0; continue. */
            float mean[4], gain[4];
            if (!reject) {
                /* Per-sensor mean + variance (from double accumulation: sumsq/N - mean^2 is sensitive to cancellation). */
                for (int i = 0; i < 4; i++) {
                    double mn  = sum[i] / CAL_SAMPLES;
                    double var = sumsq[i] / CAL_SAMPLES - mn * mn;
                    if (var < 0) var = 0;          /* float error -> slightly negative; clamp to 0 */
                    mean[i] = (float)mn;
                    /* 1) dead/disconnected sensor FIRST: at rest a healthy sensor reads ~1g, a disconnected one ~0.
                       Check this first — the CV of a dead sensor is meaningless (otherwise "moving" is reported wrongly). */
                    if (mn < (double)CAL_MIN_MAG) {
                        ESP_LOGW(TAG, "cal rejected: sensor %d dead/disconnected", i);
                        reject = true; break;
                    }
                    /* 2) is it still? CV^2 test: var > tol*mean^2 -> the device is moving (log which sensor + cv2) */
                    if (var > (double)CAL_STILL_TOL * mn * mn) {
                        ESP_LOGW(TAG, "cal rejected: sensor %d is moving (cv2=%.4f > %.4f; keep the device still)",
                                 i, var / (mn * mn), (double)CAL_STILL_TOL);
                        reject = true; break;
                    }
                }
            }
            if (!reject) {
                /* relative gain: bring every sensor to the array mean (the offset is NOT calibrated) */
                float mean_avg = (mean[0] + mean[1] + mean[2] + mean[3]) / 4.0f;
                for (int i = 0; i < 4; i++) gain[i] = mean_avg / mean[i];
                /* 3) gain out of range -> hardware fault (unit-to-unit variance is <1%); NOT a silent clamp, reject */
                for (int i = 0; i < 4; i++)
                    if (gain[i] < CAL_GAIN_MIN || gain[i] > CAL_GAIN_MAX) {
                        ESP_LOGW(TAG, "cal rejected: gain[%d]=%.3f out of range (faulty?)", i, gain[i]);
                        reject = true; break;
                    }
            }
            if (!reject) {
                esp_err_t ce = gs_cal_save(gain);
                if (ce == ESP_OK) {
                    memcpy(s_cal_gain, gain, sizeof(s_cal_gain));   /* written in-task (no tearing) */
                    ESP_LOGI(TAG, "cal OK: gain=[%.3f %.3f %.3f %.3f]",
                             gain[0], gain[1], gain[2], gain[3]);
                } else {
                    ESP_LOGE(TAG, "cal could not be saved (e=%s) - keeping previous gain",
                             esp_err_to_name(ce));
                }
            }
            n = 0;         /* the ~1.5 s measurement cut the window; start the feature window clean */
            continue;      /* skip the rest of this iteration (science/window), return to the normal flow */
        }

        /* ---- Science uplink: batch RAW int16 frames ---- */
        if (sci_on) {
            if (!prev_sci) { sci_off = 0; sci_fseq = 0; sci_dropped = 0; prev_sci = true; } /* start of session */
            int16_t s12[12];
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++) s12[i * 3 + a] = acc[i][a];
            if (sci_off + FRAME_LEN > (int)sizeof(s_sci_buf)) { /* buffer full: flush or drop */
                if (s_mqtt_up)
                    esp_mqtt_client_publish(s_mqtt, g_topic_sci,
                                            (const char *)s_sci_buf, sci_off, 0, 0);
                else
                    sci_dropped += sci_off / FRAME_LEN;  /* no mqtt -> drop the batch */
                sci_off = 0;
            }
            encode_frame(&s_sci_buf[sci_off], sci_fseq++, (uint64_t)now, s12);
            sci_off += FRAME_LEN;
            if (sci_deadline && now >= sci_deadline) sci_on = false; /* time is up */
        } else if (prev_sci) {
            if (sci_off > 0 && s_mqtt_up)  /* end of session: flush the remaining batch */
                esp_mqtt_client_publish(s_mqtt, g_topic_sci,
                                        (const char *)s_sci_buf, sci_off, 0, 0);
            sci_off = 0;
            prev_sci = false;
            ESP_LOGI(TAG, "science session ended (%lu frames, %lu dropped)",
                     (unsigned long)sci_fseq, (unsigned long)sci_dropped);
        }
        if (n == 0) win_start = now;
        if (n < WIN_MAX) {
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++)
                    s_win[n][i * 3 + a] = (float)(acc[i][a] * SCALE);
            n++;
        }
        /* close the window at 1 s (or when the buffer is full) */
        if (now - win_start >= 1000000 || n >= WIN_MAX) {
            double elapsed = (now - win_start) / 1e6;
            /* Remove gravity (DC): subtract the window mean for each (sensor,axis) */
            double mean[12] = {0};
            for (int k = 0; k < n; k++)
                for (int j = 0; j < 12; j++) mean[j] += s_win[k][j];
            for (int j = 0; j < 12; j++) mean[j] /= n;
            for (int k = 0; k < n; k++)
                for (int j = 0; j < 12; j++) s_win[k][j] -= (float)mean[j];
            features_t f = compute_features(s_win, n);
            s_last_feat = f;             /* the B3 MQTT publish will use this */
            s_last_n = n;
            s_last_fs = n / elapsed;
            /* Orientation: do not throw away the removed gravity — average the 4 sensors in THE SAME
               op-order as compute_features/stalta_step. mean[] is used READ-ONLY; the feature/detection
               path does not change (the goldens are unaffected). Written BEFORE s_feat_seq++ -> the seqlock stays intact. */
            s_last_grav[0] = (float)((mean[0] + mean[3] + mean[6] + mean[9])  / 4.0);
            s_last_grav[1] = (float)((mean[1] + mean[4] + mean[7] + mean[10]) / 4.0);
            s_last_grav[2] = (float)((mean[2] + mean[5] + mean[8] + mean[11]) / 4.0);
            s_grav_ok = true;
            s_feat_seq++;
            ESP_LOGI(TAG, "feat n=%d fs=%.1f rms=%.5f,%.5f,%.5f pga=%.5f",
                     n, s_last_fs, f.rms[0], f.rms[1], f.rms[2], f.pga);
            n = 0;
            /* if science is active, flush the accumulated batch at the end of the window (~1 s) */
            if (sci_on && sci_off > 0 && s_mqtt_up) {
                esp_mqtt_client_publish(s_mqtt, g_topic_sci,
                                        (const char *)s_sci_buf, sci_off, 0, 0);
                sci_off = 0;
            }
        }
    }
}

static void sensors_init(void) {
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    for (int i = 0; i < 4; i++) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 2 * 1000 * 1000, .mode = 3,
            .spics_io_num = PIN_CS[i], .queue_size = 4,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_dev[i]));
        uint8_t who = read_reg(i, REG_WHO_AM_I);
        if (who != WHO_AM_I_VAL) { ESP_LOGE(TAG, "sensor %d not found (0x%02X)", i, who); return; }
        write_reg(i, REG_CTRL3_C, 0x44);   /* BDU=1, IF_INC=1 */
        write_reg(i, REG_CTRL2_G, 0x00);   /* gyro off */
        write_reg(i, REG_CTRL9_XL, 0xE2);  /* I3C disable */
        write_reg(i, REG_CTRL1_XL, 0x40);  /* 104 Hz, +-2g */
    }
    xTaskCreatePinnedToCore(sample_task, "sample", 4096, NULL, 10, &s_sample_task, 1);
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_DRDY, .mode = GPIO_MODE_INPUT,
                        .intr_type = GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_DRDY, drdy_isr, NULL));
    write_reg(0, REG_INT1_CTRL, 0x01);     /* INT routing AFTER the interrupt has been SET UP */
    ESP_LOGI(TAG, "4x LSM6DSO init OK (104 Hz, +-2g)");
}

/* ---------------- WiFi + SNTP ---------------- */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_captive_fallback) return;    /* N2d: switching to in-place captive — do not reconnect the STA */
        if (s_wifi_up) s_wifi_rc++;        /* count only real drops */
        s_wifi_up = false;
        esp_wifi_connect();                /* retry */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* N2d: DELIBERATELY unguarded by s_captive_fallback — a late GOT_IP after the fallback is harmless (no reconnect; the device is about to reboot), DO NOT ADD a guard. */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_wifi_up = true;
        /* 0.9.21: YEDEK DNS. Broker bir ALAN ADI (mqtts://mqtt.geoshake.org), yani her
           yeniden baglanma DNS cozumlemesi gerektiriyor. Modemin cozumleyicisi olur ama
           interneti calisirsa cihaz adi cozemez ve MQTT'ye BIR DAHA baglanamaz — WiFi
           ayakta kalir, LED yanip soner, veri gelmez. 2026-09-05'te uc cihazda goruldu;
           GEO-FVYSA modeme cok yakin ve RSSI -45 iken takildi, yani sinyalle ilgisi yok.
           DHCP yalniz MAIN ve BACKUP'i doldurur; FALLBACK ucuncu yuvadir ve DHCP ONA
           DOKUNMAZ (CONFIG_LWIP_DNS_MAX_SERVERS=3, yuva mevcut). Her GOT_IP'de yeniden
           yaziliyor: kira yenilemesi sonrasi da yerinde kalsin.
           SINIR, ACIKCA: WAN tumden gitmisse hicbir DNS yardimci olmaz — o durumu MQTT
           watchdog'u (0.9.20) reboot ile kapatir. Bu satir yalniz "internet var, modemin
           DNS'i yok" halini kurtarir ve onu reboot'suz kurtarir. */
        esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_if) {
            esp_netif_dns_info_t d = { 0 };
            d.ip.type = ESP_IPADDR_TYPE_V4;
            d.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
            esp_err_t dns_rc = esp_netif_set_dns_info(sta_if, ESP_NETIF_DNS_FALLBACK, &d);
            if (dns_rc != ESP_OK) ESP_LOGW(TAG, "could not write fallback DNS: %s", esp_err_to_name(dns_rc));
        }
        s_wifi_ever_up = true;             /* N2d: it connected at least once -> the creds are valid */
        ESP_LOGI(TAG, "WiFi GOT_IP: " IPSTR, IP2STR(&s_ip));
    }
}

/* N2d/#282: provisioned-path watchdog. If there is no IP within WIFI_FALLBACK_TIMEOUT_MS (3 min)
   -> in-place captive, then REBOOT after CAPTIVE_REBOOT_AFTER_MS to re-attempt the STA (see below;
   a power-outage router boots slower than us, we must not brick until a manual power-cycle). If it
   has connected once (s_wifi_ever_up) it deletes itself; later drops keep retrying forever as they
   do today (a temporary outage AFTER the first connection does not drop it into captive). */
static void wifi_guard_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(WIFI_FALLBACK_TIMEOUT_MS));
    if (s_wifi_ever_up) {
        ESP_LOGI(TAG, "WiFi connected - guard exiting (no fallback needed)");

        /* BLE EXISTS ONLY FOR SETUP (bleprov.c) and is NO LONGER NEEDED on this boot:
           all THREE call sites of gs_bleprov_start are now BEHIND this point (this task's
           fallback branch + app_main's two no-identity branches). WiFi connected -> those
           branches are never entered again.

           The IDF esp_bt.h documentation describes exactly this scenario: "if bluetooth is
           only used for WiFi configuration, release its memory afterwards".
           mem_release must be called BEFORE controller_init — on the normal path BLE is never
           started, so that condition is satisfied.

           The gain is NOT GUESSED, it is measured: the log prints the heap before/after.
           This is what finances the cost of enlarging the buffers (~28 KB). */
        uint32_t heap_once = esp_get_free_heap_size();
        esp_err_t bt_rc = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        uint32_t heap_sonra = esp_get_free_heap_size();
        ESP_LOGW(TAG, "BT bellegi birakildi (%s): heap %lu -> %lu (%+ld B)",
                 esp_err_to_name(bt_rc), (unsigned long)heap_once,
                 (unsigned long)heap_sonra, (long)heap_sonra - (long)heap_once);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "WiFi did not connect in %d s - in-place captive fallback", WIFI_FALLBACK_TIMEOUT_MS / 1000);
    s_captive_fallback = true;          /* BEFORE esp_wifi_stop: so DISCONNECTED does not attempt a reconnect */
    gs_captive_start_inplace();          /* BLEPROV: non-blocking now — the portal comes up and it returns */
    gs_bleprov_start(&g_id);             /* BLE setup is open too (the app recovery path); on failure -> captive only */
    /* #282: DO NOT sit here forever. In captive the STA is stopped, so WiFi can only return via a
       reboot. A power outage brings the router up AFTER us; giving it a window and then rebooting
       re-attempts the (still-valid) creds and the device self-heals — no manual power-cycle. A
       customer entering NEW creds in the portal triggers an immediate esp_restart on save
       (captive.c), so they are unaffected; this timer only fires when nobody fixed it. */
    vTaskDelay(pdMS_TO_TICKS(CAPTIVE_REBOOT_AFTER_MS));
    ESP_LOGW(TAG, "captive %d s, still no WiFi - rebooting to re-attempt the AP (#282)",
             CAPTIVE_REBOOT_AFTER_MS / 1000);
    gs_note_restart(GS_RST_CAPTIVE);
    esp_restart();
}

/* 0.9.14 (#282 takip): MID-RUN WiFi watchdog. wifi_guard_task yalniz BOOT-zamani baglanma
   hatasini kapsar (ilk GOT_IP'den sonra kendini siler). Bagli iken WiFi'yi kaybeden cihaz
   (router/modem kapatilir, sensor guclu kalir) yalnizca wifi_event()'teki esp_wifi_connect()
   retry'ina kalir; o da AP geri gelince guvenilir sekilde toparlamaz (gecikmesiz siki
   DISCONNECTED->connect dongusu). Sahada dogrulandi 2026-08-28 GEO-QA2HF: modem ~20 dk kapali,
   sonra acik, cihaz ~85 dk olu kaldi, ELLE power-cycle gerekti (reset_reason=1, self-heal reboot yok).
   Bu task SONSUZ calisir ve cihaz en az bir kez baglandiktan sonra, WIFI_MIDRUN_REBOOT_MS boyunca
   KESINTISIZ bagli degilse reboot eder; boylece geri gelen AP temiz bir boot ile yakalanir. Kisa
   blip'ler tetiklemez (esik >> normal reconnect). OTA indirmesi sirasinda WiFi UP'tir, bu yuzden
   bir OTA'yi asla bolmez. */
static void wifi_midrun_watchdog_task(void *arg) {
    int64_t down_since_ms = 0;
    int64_t mqtt_down_since_ms = 0;
    bool    mqtt_nudged = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));            /* 30 s'de bir yokla */
        if (s_captive_fallback) {                    /* captive'i boot-time guard yonetir */
            down_since_ms = 0; mqtt_down_since_ms = 0; mqtt_nudged = false; continue;
        }
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (s_wifi_up || !s_wifi_ever_up) {
            down_since_ms = 0;                        /* su an bagli, ya da hala boot-time ilk denemede */
        } else {
            if (down_since_ms == 0) {
                down_since_ms = now_ms;
            } else if (now_ms - down_since_ms >= WIFI_MIDRUN_REBOOT_MS) {
                ESP_LOGW(TAG, "WiFi %d s mid-run down - rebooting to retry the AP (0.9.14)",
                         WIFI_MIDRUN_REBOOT_MS / 1000);
                gs_note_restart(GS_RST_WIFI_WD);
                esp_restart();
            }
        }

        /* ---- 0.9.20: MQTT KOLU — "WiFi ayakta ama broker yok" acigi -------------
           NEDEN VAR: yukaridaki WiFi kolu, s_wifi_up TRUE oldugu anda sayacini
           SIFIRLIYOR. WiFi iliskisi tutup broker'a ulasilamayan cihaz (modemin
           internet'i gitmis, NAT/DHCP takilmis, oturum olmus) bu yuzden SONSUZA
           KADAR oyle kaliyordu: LED yanip soner (kosul !(wifi_up && mqtt_up)),
           veri gelmez, hicbir sey kendini toparlamaz.
           SAHADA DOGRULANDI 2026-09-05 GEO-A73T7: musteri "isik yanip soniyor" dedi,
           ELLE restart cozdu. Ayni imza veride de var — GEO-T7ND5 27 Agustos'ta 7 saat
           sustuktan sonra 21 SAATLIK uptime ile dondu, yani hic yeniden baslamamisti.
           Acik 0.9.14'un getirdigi bir hata DEGIL; o surumun kapattigi delik yalnizca
           WiFi tarafiydi, bu taraf acik kalmisti.

           IKI KADEMELI, bilerek: reboot son care. Once yumusak deneme, cunku cogu
           takilma istemci tarafindadir ve reconnect yeter; boot ise veri kaybettirir
           (outbox) ve ~30 sn goturur.

           ESIKLER normal davranistan UZAK secildi: saglikli bir yeniden baglanma
           saniyeler surer, 5 dk onun onlarca kati. OTA sirasinda MQTT kisa sure
           dusebilir -> s_ota_busy ile korunuyor, indirme asla bolunmez.

           KABUL EDILEN BEDEL: broker UZUN sure erisilemezse (27 Agustos cert olayi,
           7 saat) filo 30 dakikada bir yeniden baslar. Reboot orada bir sey cozmez,
           ama zarar da vermez — ve filo GENELINDE es zamanli uptime sifirlanmasi
           sunucu tarafi bir arizanin acik imzasidir, tek cihazin takilmasindan
           kolayca ayrilir. Tersi (cihaz 22 saat olu kalir) daha pahali. */
        if (s_wifi_up && s_mqtt_ever_up && !s_mqtt_up && !s_ota_busy) {
            if (mqtt_down_since_ms == 0) {
                mqtt_down_since_ms = now_ms;
            } else {
                int64_t d = now_ms - mqtt_down_since_ms;
                if (!mqtt_nudged && d >= MQTT_MIDRUN_NUDGE_MS) {
                    mqtt_nudged = true;
                    ESP_LOGW(TAG, "MQTT %d s down (WiFi UP) - soft reconnect attempt (0.9.20)",
                             MQTT_MIDRUN_NUDGE_MS / 1000);
                    if (s_mqtt) esp_mqtt_client_reconnect(s_mqtt);
                } else if (d >= MQTT_MIDRUN_REBOOT_MS) {
                    ESP_LOGW(TAG, "MQTT %d s down (WiFi UP) - clean boot (0.9.20, sibling of #282)",
                             MQTT_MIDRUN_REBOOT_MS / 1000);
                    gs_note_restart(GS_RST_MQTT_WD);
                    esp_restart();
                }
            }
        } else {
            mqtt_down_since_ms = 0;
            mqtt_nudged = false;
        }
    }
}

static void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, g_id.wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_id.wifi_pass, sizeof(wc.sta.password) - 1);

    /* 0.9.17: AYNI SSID'DE BIRDEN COK AP VARSA EN GUCLUSUNU SEC (mesh / repeater).
       `wc = {0}` scan_method'u WIFI_FAST_SCAN yapar ve IDF basligi ne yaptigini
       acikca yaziyor: "scan will end after find SSID match AP". Yani tarama
       SSID'ye uyan ILK AP'de duruyor ve sinyal HIC hesaba girmiyor.

       OLCULDU (2026-09-04, GEO-QA2HF): iki AP'li bir ev agi. Cihaz -91/-93 dBm'deki
       AP'ye baglandi, oysa ayni SSID'de -63 dBm'lik AP vardi ve ayni odadaydi.
       Fis cekilip takildi — AYNI zayif AP'ye geri dondu (uptime 17 sn, bssid
       degismedi). Yani bu bir "kotu sans" degil, yapilandirmanin dogrudan sonucu.

       ALTTAKI ALAN ZATEN DOGRUYDU AMA ETKISIZDI: sort_method sifirdir, yani
       WIFI_CONNECT_AP_BY_SIGNAL ("sort match AP in scan list by RSSI") — ama
       hizli tarama SIRALANACAK BIR LISTE toplamadan duruyor. Asagida acikca
       yaziliyor ki niyet belgeli olsun; tek basina yazilmasi HICBIR SEY degistirmez,
       degeri ALL_CHANNEL_SCAN ile birlikte ortaya cikar.

       BEDEL: baglanma aninda tam kanal taramasi, ~1-2 sn. Nadiren yeniden baglanan
       bir cihaz icin onemsiz; karsiliginda zayif AP'ye kilitlenme bitiyor.

       #282'yi COZMEZ: o, elektrik gelince AP'ye hic donmemekle ilgili ayri bir
       mekanizma (bkz. wifi_guard_task, 0.9.14). Burasi "hangi AP" sorusu. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* MAXIMUM PERFORMANCE (2026-08-11, founder's decision; 5V 2A adapter, ample power budget).
       Design: docs/plans/2026-08-11-t1-maksimum-performans-design.md

       PS_NONE — THE REAL LEVER. With the default WIFI_PS_MIN_MODEM the radio SLEEPS between
       DTIM beacons; while it sleeps the TCP keepalive is delayed and the broker ping
       times out. Measured symptom (BH2JN, 30 days): mqtt_rc 298 while
       wifi_rc was 3 — the link is solid, it is the MQTT session that drops, about once an hour.
       Every drop means "Losing qos0 data", i.e. seismic data is lost. The cost is ~+80-100 mA.

       TX power 80 = 20 dBm = the REAL CEILING on the ESP32-S3. DO NOT WRITE 84: the IDF 5.3.2
       esp_wifi.h mapping table says {[80,84] -> 80}, so 84 is a NO-OP and does not
       give 21 dBm. Since the default is already 80 the GAIN IS ZERO; it is written out
       explicitly so that the intent is documented and so it does not silently drop if the
       PHY init data changes. It MUST NOT BE COUNTED as the fix; the real gain is in PS_NONE.

       HT20: the bandwidth was never being set, so it negotiated with the AP by default and
       could go up to HT40. RX sensitivity is 3.0 dB BETTER on HT20 — WROOM-1 datasheet
       v1.8 Table 7-4: HT20 MCS0 -93.0 dBm vs HT40 MCS0 -90.0 dBm. (The earlier comment
       here said 2.6 dB from a secondary source; the official figure is 3.0.) Our RSSIs are
       weak (BH2JN -71, A73T7 -81) and our data need is one small packet per second — we do
       NOT need throughput, we DO need link budget. The only cost is OTA speed.

       PROTOCOL 11B|11G|11N (0.9.19): previously left at the IDF default, which happens to be
       the same value. It is written out because DROPPING 11b IS THE OBVIOUS "SPEED TWEAK"
       AND IT WOULD COST US RANGE: datasheet Table 7-4 puts 802.11b 1 Mbps at -98.2 dBm,
       the most sensitive rate the radio has — 5.2 dB better than the best 11n rate
       (HT20 MCS0, -93.0). A station at the edge falls back to exactly that rate to stay
       alive. Writing the bitmap here makes the intent unmissable and survives an IDF
       default change.

       WHY NOT push rates UP: the same table spreads 24 dB across the rate set
       (11b 1 Mbps -98.2 ... HT20 MCS7 -74.2). At GEO-A73T7's -82 dBm every rate above
       HT20 MCS3 is BELOW sensitivity and cannot work; the TX side agrees, since Table 7-2
       gives 20.5 dBm at 11b 1 Mbps but only 17.5 dBm at HT20 MCS7. For this product
       "maximum performance" means link robustness, not throughput — the payload is one
       small packet per second.

       None of the three uses ESP_ERROR_CHECK: if they fail the device must KEEP running with
       the old behaviour — a station does not go down because of a performance setting. */
    esp_err_t ps_rc = esp_wifi_set_ps(WIFI_PS_NONE);
    esp_err_t tx_rc = esp_wifi_set_max_tx_power(80);
    esp_err_t bw_rc = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_err_t pr_rc = esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    ESP_LOGW(TAG, "perf: ps=NONE(%s) tx=80(%s) bw=HT20(%s) proto=BGN(%s)",
             esp_err_to_name(ps_rc), esp_err_to_name(tx_rc), esp_err_to_name(bw_rc),
             esp_err_to_name(pr_rc));
    ESP_LOGI(TAG, "WiFi started, SSID=%s", g_id.wifi_ssid);

    /* Fixed server (pool.ntp.org); taking NTP from DHCP is OFF — that feature
       requires CONFIG_LWIP_DHCP_GET_NTP_SRV and is unnecessary. The default config has start=true. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
}

static bool time_synced(void) {
    time_t now = 0;
    time(&now);
    return now > 1700000000;  /* after 2023-11 => SNTP has settled */
}

static void status_task(void *arg) {
    uint32_t last = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t s = s_samples;
        int8_t rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
        /* Stack high-water mark (the REMAINING free bytes, down to the worst moment).
           This project suffered a stack overflow in the field (TLS was running on the
           main task, 3584 B was not enough -> it was moved to a separate 8 KB task).
           That is why, when a new field is added, saying "it is safe by calculation" is
           not enough; it must be measurable. The cost is a single counter read.
           sample = the seismic loop, self = this status task.
           Alarm threshold: it needs looking at if it drops below 512 B. */
        UBaseType_t hw_sample = s_sample_task ? uxTaskGetStackHighWaterMark(s_sample_task) : 0;
        UBaseType_t hw_self   = uxTaskGetStackHighWaterMark(NULL);
        UBaseType_t hw_mqtt   = s_mqtt_task ? uxTaskGetStackHighWaterMark(s_mqtt_task) : 0;
        ESP_LOGI(TAG, "wifi=%s ip=" IPSTR " ntp=%s mqtt=%s mrc=%lu rssi=%d rc=%lu fs~%.1fHz heap=%lu stack sample=%u mqtt=%u status=%u",
                 s_wifi_up ? "UP" : "DOWN", IP2STR(&s_ip),
                 time_synced() ? "synced" : "bekliyor",
                 s_mqtt_up ? "UP" : "DOWN", (unsigned long)s_mqtt_rc, rssi,
                 (unsigned long)s_wifi_rc, (s - last) / 5.0,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned)hw_sample, (unsigned)hw_mqtt, (unsigned)hw_self);
        last = s;
    }
}

/* ---------------- OTA ---------------- */
static void ota_task(void *arg) {
    char *url = (char *)arg;
    ESP_LOGW(TAG, "OTA starting: %s", url);
    s_sci_req = -1;  /* stop science (so the bandwidth does not collide) */
    esp_http_client_config_t http = { .url = url, .timeout_ms = 30000, .keep_alive_enable = true };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_err_t e = esp_https_ota(&cfg);  /* the signature is verified via CONFIG_SECURE_SIGNED_ON_UPDATE */
    if (e == ESP_OK) { ESP_LOGW(TAG, "OTA OK - rebooting"); gs_note_restart(GS_RST_OTA); esp_restart(); }
    ESP_LOGE(TAG, "OTA FAIL: %s - staying on the current app (science stopped, "
             "gerekirse tekrar baslat)", esp_err_to_name(e));  /* I1 */
    free(url);
    s_ota_busy = false;
    vTaskDelete(NULL);
}

/* ---- Command ACK (2026-08-15) -------------------------------------------
   Publishes {"cmd":"<name>","ok":true|false,"ms":<uptime>} to geoshake/<id>/ack.

   WHY IT EXISTS: until now the panel could only say "sent". EMQX returns 200 even when
   nobody is subscribed, so "sent" never meant "done" — and when a command was lost there
   was no way to tell. The device is the only party that knows it acted.

   ACCEPTED, NOT FINISHED: for sci/cal the flag is handed to sample_task, so the ack means
   "received and accepted". For reset-* it is published just before the reboot; the 500 ms
   delay that already existed gives the QoS 1 PUBACK a chance to leave. If it does not, the
   command still ran — an ack is a bonus, never a precondition.

   The server side does not consume this topic yet (EMQX drops it). It is emitted NOW because
   firmware ships in an image and a server-side reader can be added later WITHOUT a new OTA.
   ACL: acl.conf already allows geoshake/# for every client, so no rule change is needed. */
static void gs_cmd_ack(const char *cmd, bool ok) {
    if (!s_mqtt || g_topic_ack[0] == '\0') return;
    char body[96];
    int n = snprintf(body, sizeof(body), "{\"cmd\":\"%s\",\"ok\":%s,\"ms\":%lld}",
                     cmd, ok ? "true" : "false", (long long)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(body)) return;
    esp_mqtt_client_publish(s_mqtt, g_topic_ack, body, n, 1, 0);
}

/* ---------------- MQTT publisher ---------------- */
static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_mqtt_up = true;
        s_mqtt_ever_up = true;   /* 0.9.20: watchdog'un MQTT kolu ancak bundan sonra konusur */
        /* QoS 1 (was 0, 2026-08-15): the effective QoS is min(publish, subscribe), so the
           server side alone could not fix delivery. With QoS 0 a command sent while the device
           was rebooting/off simply evaporated and NOBODY was told (observed 2026-07-15: the web
           command vanished, EMQX still returned 200). */
        esp_mqtt_client_subscribe(s_mqtt, g_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected: %s (cmd subscribe qos1)", MQTT_BROKER);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        if (s_mqtt_up) s_mqtt_rc++;
        s_mqtt_up = false;
    } else if (id == MQTT_EVENT_BEFORE_CONNECT) {
        s_mqtt_try++;   /* 0.9.24: her baglanma DENEMESI (basarili ya da degil) */
    } else if (id == MQTT_EVENT_ERROR) {
        s_mqtt_fail++;  /* 0.9.24: TCP/TLS tasima hatasi ya da CONNECTION_REFUSED */
    } else if (id == MQTT_EVENT_DATA) {
        /* Topic guard (C1) + fragmentation guard (C2): only a COMPLETE .../cmd payload counts as a command.
           Prevents a wrong parse when multiple subscriptions / fragmented messages arrive. */
        const char *CMD_TOPIC = g_topic_cmd;   /* the runtime identity topic; the guard logic is preserved as is */
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) return;
        if (e->topic == NULL || e->topic_len != (int)strlen(CMD_TOPIC) ||
            strncmp(e->topic, CMD_TOPIC, e->topic_len) != 0) return;
        cJSON *j = cJSON_ParseWithLength(e->data, e->data_len);
        if (j) {
            cJSON *cmd = cJSON_GetObjectItem(j, "cmd");
            if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "sci") == 0) {
                cJSON *secs = cJSON_GetObjectItem(j, "secs");
                if (cJSON_IsNumber(secs)) {
                    s_sci_req = secs->valueint;   /* sample_task will consume it (single owner) */
                    ESP_LOGI(TAG, "cmd: sci secs=%d", secs->valueint);
                    gs_cmd_ack("sci", true);
                }
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ota") == 0) {
                cJSON *url = cJSON_GetObjectItem(j, "url");
                /* M2: the mqtt:// channel is unauthenticated — do not trust CLI validation, enforce it
                   in the firmware: http(s):// only, sensible length. The signature still guarantees
                   integrity (unauthorised code will not run), but a weird url DoS/SSRF is prevented. */
                if (cJSON_IsString(url) && !s_ota_busy) {
                    const char *us = url->valuestring;
                    size_t ul = strlen(us);
                    bool ok = ul >= 8 && ul < 256 &&
                              (strncmp(us, "http://", 7) == 0 || strncmp(us, "https://", 8) == 0);
                    if (!ok) {
                        ESP_LOGW(TAG, "OTA url rejected (not http(s):// or bad length)");
                        gs_cmd_ack("ota", false);
                    } else {
                        s_ota_busy = true;
                        char *u = strdup(us);   /* ota_task frees it */
                        if (u) { gs_cmd_ack("ota", true); xTaskCreatePinnedToCore(ota_task, "ota", 8192, u, 5, NULL, 0); }
                        else s_ota_busy = false;  /* I2: pin to core 0 (to protect the sampling on core 1) */
                    }
                }
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "reset-id") == 0) {
                /* N1-T4 (N2a-current): a FULL factory reset. Erase the NVS "geoshake" identity COMPLETELY
                   (MQTT + WiFi) -> reboot. After the T2 boot decision mqtt_ready=false -> the device
                   falls into factory-wait (it waits until the admin writes the MQTT NVS over USB; captive DOES NOT OPEN).
                   No parameters; it returns the device to factory state (the N3 panel asks with window.confirm). */
                gs_cmd_ack("reset-id", true);
                gs_identity_erase();
                ESP_LOGW(TAG, "Identity erased (reset-id, FULL) - rebooting (factory-wait; admin USB)");
                vTaskDelay(pdMS_TO_TICKS(500));   /* short delay for the log flush + mqtt ack */
                gs_note_restart(GS_RST_ID);
                esp_restart();
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "reset-wifi") == 0) {
                /* N2a: a customer-safe WiFi reset. Erase ONLY the WiFi keys; the MQTT identity
                   (embedded by the admin with esptool) is preserved. At boot mqtt_ready && !wifi_ready -> captive
                   portal (the customer re-enters WiFi). The device DOES NOT return to factory state. */
                gs_cmd_ack("reset-wifi", true);
                gs_identity_erase_wifi();
                ESP_LOGW(TAG, "WiFi erased (reset-wifi) - rebooting (captive; MQTT preserved)");
                vTaskDelay(pdMS_TO_TICKS(500));   /* short delay for the log flush + mqtt ack */
                gs_note_restart(GS_RST_WIFI_CMD);
                esp_restart();
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "cal") == 0) {
                /* CAL-T4: auto still-state calibration. No parameters — the payload's "action" field (if any)
                   is ignored (there is a single mode). sample_task (core1) consumes it; the sci idiom (single owner, no race). */
                s_cal_req = true;
                ESP_LOGI(TAG, "cmd: cal (auto still-state calibration)");
                gs_cmd_ack("cal", true);
            }
            cJSON_Delete(j);
        }
    }
}

/* Let's Encrypt root certificates (ISRG Root X1 RSA + X2 ECDSA) — PUBLIC, safe to commit.
   T7 review CRITICAL: Caddy by default produces an ECDSA cert (chain -> ISRG Root X2); if we pin
   ONLY X1 the device cannot verify the X2 chain -> the mqtts handshake FAILS -> a silent inability
   to connect for the whole soak. We embed both -> whatever Caddy produces (RSA->X1 / ECDSA->X2) the device verifies.
   X1 SHA-256: 96:BC:EC:06:...:08:C6   X2 SHA-256: 69:72:9B:8E:...:14:70 (both openssl-verified) */
static const char ISRG_ROOTS[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n"
/* ISRG Root X2 (ECDSA P-384) — Caddy's default ECDSA cert chains to this root */
"-----BEGIN CERTIFICATE-----\n"
"MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
"CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
"R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
"MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
"ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
"EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
"+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
"ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
"AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
"zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
"tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
"/q4AaOeMSQ+2b1tbFfLn\n"
"-----END CERTIFICATE-----\n";

static void mqtt_start(void) {
    /* buffer.size: the default 1024 is not enough for a science batch (~4.6KB) -> the publish silently fails.
       8192 is enough for the batch + feat/health.
       TLS (Phase C T7): if MQTT_BROKER is mqtts://, esp-mqtt performs a TLS handshake, verifies the
       server certificate against ISRG Root X1 and sends user/pass. If it is mqtt:// (local
       1883, anonymous) TLS stays off per the scheme; the CA + the empty credentials are ignored. */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER,
        .broker.verification.certificate = ISRG_ROOTS,
        .credentials.username = g_id.mqtt_user,
        .credentials.authentication.password = g_id.mqtt_pass,
        .buffer.size = 8192,
        /* PERSISTENT SESSION (2026-08-15). Without this the broker drops everything addressed to
           an offline client, so QoS 1 alone would still lose commands: the queue would never exist.
           The session is keyed by client id; esp-mqtt derives it from the MAC, so it survives a
           reboot. TRADE-OFF, on purpose: a command is no longer "now or never" — a queued command
           WILL run when the device comes back, even days later. That is the intent (a firmware
           update must not be lost because the power blinked), but it means commands are durable
           and must be sent as such. */
        .session.disable_clean_session = true,
        /* 60 KB outbox cap (review): a wave flood must not drag the heap floor below what WiFi/TLS
           needs. Reject-new semantics, verified in esp-mqtt v5.3.2 (mqtt_client.c publish path):
           publish returns -2 when the NEW message WOULD push the outbox past the cap (len + size >
           limit) — queued messages are never dropped, and one full wave (10 full + 1 short chunk,
           ~55.8 KB worst case serialized) always fits under it. The wave loop bails on that -2. */
        .outbox.limit = 61440,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t e = esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "mqtt_start: broker=%s result=%s", MQTT_BROKER, esp_err_to_name(e));
}

/* Real epoch ms if SNTP is synced, otherwise 0 (the shared schema accepts 0) */
static int64_t now_ms(void) {
    if (!time_synced()) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---- Reset reason: WHY the device restarted ------------------------------
   A blind spot that opened up in the field on 2026-08-11: GEO-FVYSA restarted 32 times in
   30 days (269 hours lost) but the reason COULD NOT BE MEASURED — from the outside a brownout
   (a loose USB/adapter -> voltage collapse), a firmware panic and a real power
   cut ALL LOOK ALIKE: in every case the uptime resets and every one of them looks
   "offline". Measurement instead of guesswork: esp_reset_reason() tells them apart.
     1=POWERON (power came back)  3=SW  4=PANIC (firmware crash)
     5=INT_WDT 6=TASK_WDT 7=WDT  8=DEEPSLEEP  9=BROWNOUT (the power rail!)
   The value is read once at boot and DOES NOT CHANGE; it is carried in every health so that
   after a device goes quiet we can come back and ask "what happened last" (if it were
   published only once, that information would be lost with the one packet that got missed). */
static uint8_t s_rst_reason;

static void mqtt_task(void *arg) {
    /* buf[] size budget — health is the longest payload; MEASURED with snprintf on the host (not
       guessed, re-measured on 2026-08-11 after rst was added):
         field (GEO-FVYSA, with grav, with rst): 210 bytes
         worst case                            : 291 (+NUL 292)  [dev 31, counters 2^32-1,
                           t_ms 2^53-1, fw 25, gain 4x2.0 (the cal.h clamp), grav components
                           saturated at +-2g, rst 3 digits]
       256 WOULD NOT HAVE BEEN ENOUGH (292 > 256). Re-measured on 2026-08-21 after bssid+ch:
         worst case NOW: 328 (+NUL 329)  [the apsuf field costs 37 bytes]
       320 WOULD HAVE OVERFLOWED BY 9 (329 > 320) and health would have been dropped — the warning
       on this very line caught it before the change shipped. -> 384, leaving 55 bytes of headroom.
       The feat payload is at worst 165 and fits in the same buffer. If another field is to be added,
       re-measure this line FIRST: on overflow health IS DROPPED (the len check below discards it). */
    /* 384 -> 416 (2026-09-03, sh_trig alani eklenirken). En kotu durum olculdu:
       tum alanlar tavanda 339 bayt, yani 384 de yeterdi (pay 45) — ama sabit tampona
       alan eklerken payi olcup buyutmek, bir sonraki alanda tasmayi onler.
       Tasma SESSIZ DEGIL (len kontrolu var, paket dusurulup hata loglanir) ama
       o durumda saglik paketi tumden durur ve cihaz SESSIZ gorunur. */
    /* 416 -> 480 (2026-09-04, rs_* alanlari eklenirken). Bu dosyanin kurali:
       ALAN EKLERKEN ONCE OLC. Olculdu — dort yeni alanin en kotu durumu:
         ,"rs_n":4294967295  = 18      ,"rs_a":-1280 = 13
         ,"rs_lo":-128       = 13      ,"rs_hi":-128 = 13     toplam 57
       Onceki en kotu durum 339 + 57 = 396. 416 YETERDI (pay 20) ama pay
       daralmisti; 480 ile pay 84'e cikti. Tasma sessiz DEGIL (len kontrolu
       paketi dusurup hata basar) ama o durumda saglik tumden durur ve cihaz
       SESSIZ gorunur — bu yuzden pay burada ucuz sigortadir. */
    /* 480 -> 560 (2026-09-06, heap_int/heap_blk/mqtt_try/mqtt_fail eklenirken). OLCULDU:
         ,"heap_int":4294967295  = 22    ,"heap_blk":4294967295  = 22
         ,"mqtt_try":4294967295  = 21    ,"mqtt_fail":4294967295 = 22    toplam 87
       Onceki en kotu (rs_* + sw_rst dahil) 409. 409 + 87 = 496 > 480 — 480 TASARDI.
       560 ile pay 64. ALAN EKLERKEN ONCE OLC (bu dosyanin kurali). */
    /* 560 -> 608 (2026-09-08, sl_max/slf_max eklenirken). OLCULDU:
         ,"sl_max":4294967295  = 21      ,"slf_max":4294967295 = 22    toplam 43
       Onceki en kotu 496 + 43 = 539. 560 YETERDI (pay 21) ama pay daralmisti;
       608 ile pay 69. ALAN EKLERKEN ONCE OLC (bu dosyanin kurali). */
    /* 608 -> 640 (2026-09-08, 0.9.27, lta_frz/gold eklenirken). OLCULDU:
         ,"lta_frz":4294967295 = 22      ,"gold":1 = 9     toplam 31
       Onceki en kotu 539 + 31 = 570. 608 YETERDI (pay 38) ama pay daralmisti;
       640 ile pay 70. ALAN EKLERKEN ONCE OLC (bu dosyanin kurali). */
    char buf[640];
    uint32_t last_feat = 0, pubseq = 0, hseq = 0;
    int64_t last_health = -30000000;  /* so the first health goes out immediately */

    /* ---- RSSI istatistigi (0.9.18, kasa RF karsilastirmasi icin) ------------
       NEDEN: saglik paketi 30 saniyede bir TEK anlik RSSI tasiyordu. Kasa
       karsilastirmasinda o yetmiyor, cunku olculdu (GEO-KK9YF, 2176 kayit):
       tek okumanin std'si 3,07 dB ve okumalar BAGIMSIZ DEGIL — 20 okumayi
       ortalamak hatayi 0,69'a degil 2,78'e indiriyor. Yani her paket tek bir
       zayif olcumdur.
       1 Hz ornekleme bunu duzeltmez (yavas suruklenme tasarimla cozulur, bkz.
       docs/plans) AMA min/max ile SONUMLEME DERINLIGINI acar: kayipli ya da
       rezonans yapan bir kasa yalniz ortalamayi dusurmez, dalgalanmayi da
       buyutur. O bilgi bugun hic toplanmiyordu.
       KILIT YOK, GEREKMIYOR: bu dort degiskenin TEK sahibi mqtt_task — hem
       yazan hem okuyup sifirlayan o. sh_trig'den farki bu (o cekirdekler
       arasi gecti, atomik gerekti). */
    int32_t rs_sum = 0; uint32_t rs_n = 0;
    int8_t rs_lo = 0, rs_hi = 0;
    int64_t last_rssi = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_mqtt_up) continue;

        /* 1 Hz RSSI ornegi. Dongu 5 Hz doner; saat kontrolu ile saniyede bire
           iniyoruz — beacon araligi ~100 ms oldugundan 1 Hz ardisik ornekleri
           ayni beacon'dan okumaz. */
        int64_t tr = esp_timer_get_time();
        if (tr - last_rssi >= 1000000) {
            last_rssi = tr;
            wifi_ap_record_t rr;
            if (esp_wifi_sta_get_ap_info(&rr) == ESP_OK) {
                if (rs_n == 0 || rr.rssi < rs_lo) rs_lo = rr.rssi;
                if (rs_n == 0 || rr.rssi > rs_hi) rs_hi = rr.rssi;
                rs_sum += rr.rssi;
                rs_n++;
            }
        }

        /* a new window -> publish feat (a consistent seqlock-protected read) */
        uint32_t seq;
        features_t f; int n; double fs;
        do { seq = s_feat_seq; f = s_last_feat; n = s_last_n; fs = s_last_fs; }
        while (seq != s_feat_seq);
        if (seq != last_feat && n > 0) {
            last_feat = seq;   /* advance even without a clock: SKIP the window, do not publish it stale later */
            /* 0.9.3: DO NOT PUBLISH WITHOUT A CLOCK. now_ms() returns 0 before SNTP settles, and MQTT
               can connect BEFORE SNTP -> the feats sent in that window carried t_ms=0.
               The trigger publish already filtered this (ep_ms > 0 below); feat did not ->
               the records landed in the SeedLink ring with a 1970-01-01 stamp and created a
               PERMANENT epoch-zero stream (field: GS_FVYSA_00_L_N_Z). There is a filter on the feeder
               side too (MIN_EPOCH_MS) but the right place is the source: a broken stamp must never leave the device.
               health is DELIBERATELY not protected this way — the device must stay visible even without a clock
               (fw/up_s/heap/rssi are needed for diagnosis; there, t_ms=0 is the "no clock" signal). */
            int64_t ep = now_ms();
            if (ep > 0) {
                int len = snprintf(buf, sizeof(buf),
                    "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"fs\":%.2f,\"n\":%d,"
                    "\"rms\":[%.6f,%.6f,%.6f],\"pga\":%.6f}",
                    g_id.device_id, (unsigned long)pubseq++, (long long)ep, fs, n,
                    f.rms[0], f.rms[1], f.rms[2], f.pga);
                esp_mqtt_client_publish(s_mqtt, g_topic_feat, buf, len, 0, 0);
            }
        }

        /* health once every 30 s */
        int64_t t = esp_timer_get_time();
        if (t - last_health >= 30000000) {
            last_health = t;
            int8_t rssi = 0;
            /* ",\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"ch\":165" = 37 + NUL. Stays empty when not
               associated — both fields are OPTIONAL, exactly like grav (see grav_suffix). */
            char apsuf[48] = "";
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
                /* This struct was ALWAYS filled; until 0.9.12 only .rssi was read and .bssid/.primary
                   were thrown away. That cost a real diagnosis: on 2026-08-21 GEO-QA2HF fell
                   -61 -> -86 dBm across an OTA reboot and "did it roam to another AP, or did the
                   environment change?" could not be answered — the answer had been arriving every
                   30 s and being discarded. bssid equality reduces that question to one comparison. */
                int n = snprintf(apsuf, sizeof(apsuf),
                                 ",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ch\":%u",
                                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                                 ap.bssid[3], ap.bssid[4], ap.bssid[5], (unsigned)ap.primary);
                /* Same rule as grav_suffix: on overflow drop the SUFFIX, never corrupt the packet. */
                if (n < 0 || (size_t)n >= sizeof(apsuf)) apsuf[0] = '\0';
            }
            /* Orientation goes out in health too: triggers are rare, whereas "the device was moved"
               must be updated within 30 s. Same place/rationale as gain (a slowly changing device property). */
            char gsuf[40];
            grav_suffix(gsuf, sizeof(gsuf));
            int len = snprintf(buf, sizeof(buf),
                "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"up_s\":%lu,\"heap\":%lu,"
                "\"rssi\":%d,\"wifi_rc\":%lu,\"mqtt_rc\":%lu,\"rst\":%u,\"fw\":\"%s\","
                "\"sh_trig\":%lu,"
                "\"rs_n\":%lu,\"rs_a\":%d,\"rs_lo\":%d,\"rs_hi\":%d,\"sw_rst\":%u,"
                "\"heap_int\":%lu,\"heap_blk\":%lu,\"mqtt_try\":%lu,\"mqtt_fail\":%lu,"
                "\"sl_max\":%lu,\"slf_max\":%lu,"
                "\"lta_frz\":%lu,\"gold\":%u,"
                "\"gain\":[%.4f,%.4f,%.4f,%.4f]%s%s}",
                g_id.device_id, (unsigned long)hseq++, (long long)now_ms(),
                (unsigned long)(t / 1000000), (unsigned long)esp_get_free_heap_size(),
                rssi, (unsigned long)s_wifi_rc, (unsigned long)s_mqtt_rc,
                (unsigned)s_rst_reason, FW_VERSION,
                /* OKU VE SIFIRLA — atomik degis-tokus, kayipsiz devir: iki
                   saglik paketi arasindaki gecisler tam bir kez raporlanir. */
                (unsigned long)atomic_exchange(&s_shadow_trig, 0),
                /* rs_a: ortalama x10, TAM SAYI olarak. Ondalik yerine olcek
                   kullanmak iki sey kazandirir: float bicimlendirme yok, ve
                   -64,5 dBm gibi bir deger yuvarlanmadan gecer. Ornek yoksa
                   (rs_n==0) alanlarin uclu de 0 gider ve rs_n bunu ayirt eder —
                   "0 dBm olctuk" ile "olcemedik" karismaz. */
                (unsigned long)rs_n,
                rs_n ? (int)((rs_sum * 10) / (int32_t)rs_n) : 0,
                rs_n ? (int)rs_lo : 0, rs_n ? (int)rs_hi : 0,
                (unsigned)s_sw_rst,
                /* 0.9.24: IC RAM — TLS'in GERCEKTEN ihtiyaci olan bellek. heap (yukarida
                   esp_get_free_heap_size) PSRAM'li derlemede ~8,27 MB gosterir ve %99'u PSRAM;
                   mbedTLS ise BILEREK ic RAM'de. TLS el sikismasi ~30-45 KB BITISIK ic blok ister.
                   heap_blk bunun altina inerse her yeniden baglanma TLS'te olur ve REBOOT disinda
                   hicbir sey duzeltmez — A73T7'nin -90 dBm'de MQTT'yi dusurmesinin saf sinyal mi
                   yoksa ic-bellek parcalanmasi mi oldugunu bu alan ayirir. */
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned long)s_mqtt_try, (unsigned long)s_mqtt_fail,
                /* OKU VE SIFIRLA — sh_trig ile ayni sozlesme: deger TEK pencereye ait.
                   Oran x100 TAM SAYI (589 = 5,89). */
                (unsigned long)atomic_exchange(&s_sl_max, 0),
                (unsigned long)atomic_exchange(&s_slf_max, 0),
                /* 0.9.27: lta_frz OKU VE SIFIRLA (sh_trig sozlesmesi). gold: boot golden'larin
                   HEPSI gecti mi — LED panik cizgisinin sunucudan gorunur hali; filo OTA'sinda
                   yeni golden'in (freeze) sahada gectigini seri log olmadan bu alan soyler. */
                (unsigned long)atomic_exchange(&s_lta_frz, 0),
                (unsigned)(s_golden_ok ? 1 : 0),
                s_cal_gain[0], s_cal_gain[1], s_cal_gain[2], s_cal_gain[3], gsuf, apsuf);
            /* OKU VE SIFIRLA — bir sonraki pencere temiz baslar. sh_trig'in
               atomic_exchange'i ile ayni sozlesme; burada tek sahip oldugu icin
               duz atama yeterli. */
            rs_sum = 0; rs_n = 0; rs_lo = 0; rs_hi = 0;
            if (len > 0 && len < (int)sizeof(buf))
                esp_mqtt_client_publish(s_mqtt, g_topic_health, buf, len, 0, 0);
            else
                ESP_LOGE(TAG, "health payload encode failed (%d) - dropped", len);
        }
    }
}

/* First boot after an OTA: if WiFi+MQTT come up within SELFTEST_S the app is valid, otherwise rollback.
   This self-test is the INCOMING app's own window (not the outgoing version's) — the new app boots
   and runs its own timer. So widening the window here rescues the devices that are OTA'd TO this
   version; there is no chicken-and-egg problem.

   30 -> 180 (2026-08-11, field measurement): 4 OTAs were attempted on GEO-FVYSA; the download
   completed 100% twice and in both cases the app failed to connect within 30 s and was ROLLED BACK.
   The measured "startup -> first health" time (14 days, 19 startups): avg 28 s, slowest 158 s, 11% of
   them exceed 30 s. So 30 s was a coin flip at that site, and even 120 s would have missed the worst
   case (158 s) -> 180 s was chosen: it covers every observed startup with ~14% margin on top.
   For comparison: BH2JN/QA2HF/A73T7 connect in 18-24 s, so it makes no difference for them.

   The protection IS NOT WEAKENED: a genuinely broken app cannot bring up WiFi+MQTT in 180 s either, so
   the rollback still works. The only cost is that on a bad update the rescue starts 2.5 min later
   — and the device is unusable at that point anyway. */
#define SELFTEST_S 180
static void selftest_task(void *arg) {
    for (int i = 0; i < SELFTEST_S; i++) {
        if (s_wifi_up && s_mqtt_up) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "OTA self-test PASSED - app marked valid");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "OTA self-test FAILED (%d s) - rollback + reboot", SELFTEST_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();  /* returns to the old app */
}

/* ---------------- LED status indicator ----------------
   GPIO39 via LEDC PWM. led_set applies the polarity + clamp; led_init sets up the
   hardware; led_task is the only driver (the state machine is in Task 2). */
static void led_set(int duty) {
    if (duty < 0) duty = 0;
    if (duty > LED_MAX) duty = LED_MAX;
    int applied = LED_ACTIVE_HIGH ? duty : (LED_MAX - duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CH, applied);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CH);
}

static void led_init(void) {
    /* The PWM was moved ABOVE the audible band (5 kHz -> 25 kHz, 2026-08-11).
       NOTE: this did NOT SOLVE the buzzing on GEO-FVYSA — it was tested with the LED
       switched off entirely and the sound continued; the source is not the LED but the
       board's power rail. The change was KEPT anyway: 5 kHz is right in the audible band
       and it is generally correct that the LED load should not contribute to acoustic
       noise. Constraint: freq x 2^resolution <= 80 MHz (APB) -> the 13-bit ceiling is
       9.7 kHz; going above the band requires 11-bit. 2048 steps is more than enough for a status LED. */
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,   /* 11-bit -> LED_MAX 2047 */
        .timer_num       = LED_TIMER,
        .freq_hz         = 25000,               /* ABOVE the audible band (20 kHz) */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num   = PIN_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_CH,
        .timer_sel  = LED_TIMER,
        .duty       = 0,          /* start dark */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    ESP_LOGI(TAG, "LED init OK (GPIO%d, LEDC 25kHz 11-bit)", PIN_LED);
}

/* ---- Waiting-mode patterns (app_main's FOUR waiting branches) ----
   Rationale: these branches never reach the normal boot's led_init/led_task; if GPIO39 is
   NOT DRIVEN the LED stays permanently ON and the customer reads that as "the device is broken"
   (field: GEO-BH2JN, 2026-07-25). The app says "make sure the light is blinking" in three
   languages — the text was the correct thing, the device was the wrong one (decision: 2026-07-27).

   The patterns ARE DISTINGUISHABLE, but every waiting state the customer sees reads as
   "blinking" — the app text stays true in every case. The PULSE (soft breathing) was
   DELIBERATELY NOT USED: in led_task that pattern means "healthy and connected"; using it
   for waiting would produce exactly the confusion we are trying to fix. */
typedef enum {
    LED_WAIT_SETUP,   /* slow blink 500ms — CUSTOMER: waiting for WiFi setup */
    LED_WAIT_PROV,    /* fast blink 200ms — TRANSIENT: fetching the identity from the cloud */
    LED_WAIT_ID,      /* double flash — OPERATOR ONLY: no identity, factory prep required */
} led_wait_t;

#define LED_WAIT_FAST_MS 200    /* self-provisioning blink period */
#define LED_DFLASH_MS   2000    /* full period of the double flash */
#define LED_DFLASH_ON    120    /* duration of each flash */
#define LED_DFLASH_GAP   200    /* darkness between the two flashes */

/* Double flash: [0,120) on — [120,320) off — [320,440) on — the rest off. */
static bool led_dflash_on(int64_t t) {
    if (t < LED_DFLASH_ON) return true;
    const int64_t s = LED_DFLASH_ON + LED_DFLASH_GAP;
    return (t >= s && t < s + LED_DFLASH_ON);
}

/* Drive the pattern + wait FOREVER (NEVER RETURNS). 20ms tick: so that both the 120ms part of the
   double flash and the 200ms fast blink look crisp (at 30ms the duty jitter blurs the pattern).
   There is no other work on this branch — the cost of the tick is negligible. */
static void led_wait_forever(led_wait_t mode) {
    led_init();
    for (;;) {
        const int64_t now = esp_timer_get_time() / 1000;
        bool on;
        switch (mode) {
            case LED_WAIT_PROV: on = (now % LED_WAIT_FAST_MS) < (LED_WAIT_FAST_MS / 2); break;
            case LED_WAIT_ID:   on = led_dflash_on(now % LED_DFLASH_MS);                break;
            case LED_WAIT_SETUP:
            default:            on = (now % LED_BLINK_MS) < (LED_BLINK_MS / 2);         break;
        }
        led_set(on ? LED_MAX : 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* State machine: priority high->low = panic > burst > blink > pulse.
   Approach: compute the target duty on every tick and ramp the actual duty towards the target
   (a manual ramp, no ISR — the simplest option, no hardware fade setup needed). This way the
   peak/trough transitions do not jump abruptly and the pulse flows softly like breathing. */
static void led_task(void *arg) {
    int duty = 0;                 /* the currently applied duty (kept for the ramp) */
    const int RAMP = LED_MAX / 6; /* max duty change per tick (~every 30ms) */
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;   /* ms */
        int target;

        /* 1) panic (golden FAIL): fast square wave, full brightness.
           If even one of the golden checks blew up (C/TS mismatch) the device is untrustworthy → strobe. */
        bool panik = !s_golden_ok;

        /* 2) burst (EARTHQUAKE TRIGGER): bright for LED_BURST_MS after a trigger.
           s_trig_flash_ms is set at the trigger moment (sample_task) → the burst fires. */
        bool burst = (s_trig_flash_ms > 0) && (now - s_trig_flash_ms < LED_BURST_MS);

        if (panik) {
            /* square wave: full brightness for the first half of the period, dark for the second */
            target = ((now % LED_PANIK_MS) < (LED_PANIK_MS / 2)) ? LED_MAX : 0;
        } else if (burst) {
            target = LED_MAX;     /* steady full brightness */
        } else if (!(s_wifi_up && s_mqtt_up)) {
            /* 3) blink (no connection): slow square wave */
            target = ((now % LED_BLINK_MS) < (LED_BLINK_MS / 2)) ? LED_MAX : 0;
        } else {
            /* 4) pulse (normal): triangle wave with a ~3 s period, peak ~30% -> a breathing feel */
            int phase = now % LED_NABIZ_MS;          /* 0..LED_NABIZ_MS */
            int half  = LED_NABIZ_MS / 2;
            int tri   = (phase < half) ? phase : (LED_NABIZ_MS - phase); /* 0..half triangle */
            target = (int)((int64_t)LED_NABIZ_PEAK * tri / half);        /* 0..peak */
        }

        /* panic: SKIP the ramp — an alarm must be a hard dark↔bright strobe; the ramp would soften the
           150ms square into a mid-band throb. The other states keep the ramp for the breathing feel. */
        if (panik) {
            duty = target;
        } else if (duty < target) { duty += RAMP; if (duty > target) duty = target; }
        else if (duty > target) { duty -= RAMP; if (duty < target) duty = target; }
        led_set(duty);

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* GH-DEVPROV self-provisioning worker — a SEPARATE task (the main task's 3584B stack is not enough for
   a TLS handshake → calling it directly gives "stack overflow in task main"). 10240 stack: mbedtls TLS +
   crt_bundle cert verification + margin for cJSON. Flow: wait for GOT_IP → wait for SNTP → gs_provision_run
   with backoff-retry → esp_restart on OK (after the reboot mqtt_ready → normal boot). g_id/s_wifi_ever_up/time_synced are in main.c. */
static void provision_task(void *arg) {
    (void)arg;
    while (!s_wifi_ever_up) vTaskDelay(pdMS_TO_TICKS(250));  /* wait for GOT_IP (wifi_guard takes over a WiFi failure) */
    /* Wait until SNTP settles (max ~30 s) — cert time validation blows up with the clock at 1970.
       Continue even if it does not settle: the retry loop catches the TLS failure and SNTP settles in the meantime. */
    for (int w = 0; !time_synced() && w < 30000; w += 500) vTaskDelay(pdMS_TO_TICKS(500));
    for (int attempt = 1; ; attempt++) {
        if (gs_provision_run(&g_id) == ESP_OK) {
            ESP_LOGW(TAG, "self-provisioning OK - rebooting (normal boot with mqtt_ready)");
            gs_note_restart(GS_RST_SELFPROV);
            esp_restart();
        }
        int backoff = attempt < 6 ? attempt * 10 : 60;  /* 10,20,30,40,50,60s cap */
        ESP_LOGW(TAG, "self-provisioning failed (attempt %d) - retrying in %d s", attempt, backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
    }
}

/* ---- 0.9.23: NVS'E BAGLI ACILIS OKUMALARI TEK YERDE ------------------------
   nvs_flash_init()'ten SONRA, app_main'den TEK kez cagrilir. NVS okuyan hicbir
   acilis adimi bunun DISINDA yasamaz. NEDEN: 0.9.21'de gs_take_restart_reason
   app_main icinde nvs_flash_init()'ten ONCE duruyordu; nvs_open
   ESP_ERR_NVS_NOT_INITIALIZED dondu, fonksiyon sessizce 0 verdi ve filo bir gun
   boyunca sw_rst=0 bildirdi (ayrinti: app_main'deki 0.9.22 notu). Sira hatasi
   artik dagilmis satirlarin degil TEK cagrinin yerinin hatasidir — bir bakista
   gorulur. esp_reset_reason() BURAYA GIRMEZ: donanim yazmaci, NVS'ten bagimsiz,
   app_main'in ilk satirinda kalir. */
static void boot_read_nvs_state(void) {
    /* 1) Yazilim yeniden baslatma sebebi (0.9.21). Oku ve SIL. */
    s_sw_rst = gs_take_restart_reason();
    if (s_sw_rst != GS_RST_NONE)
        ESP_LOGW(TAG, "boot: yazilim yeniden baslatma sebebi = %u "
                      "(1=wifi_wd 2=mqtt_wd 3=captive 4=ota 5=reset-id 6=reset-wifi 7=selfprov)",
                 (unsigned)s_sw_rst);
    /* 2) Kimlik (N1-T2), "geoshake" ad alani. ESP_ERROR_CHECK YOK — NOT_FOUND
       (provision edilmemis) beklenen, kurtarilabilir bir durumdur. Banner /
       gs_topics_init / WiFi / MQTT'den ONCE, hepsi gercek kimligi gorsun. */
    gs_identity_load(&g_id);
    /* 3) Sensor kazanclari (CAL-T3), "geoshake_cal" ad alani; yoksa 1.0.
       sample_task (sensors_init) BASLAMADAN once — o diziyi okuyor. */
    gs_cal_load(s_cal_gain);
    /* Ileride eklenecek her NVS acilis okumasi BURAYA, yukaridaki sirayla. */
}

void app_main(void) {
    /* Read the reset reason BEFORE ANYTHING ELSE: so the reason is not corrupted if heavy operations
       such as nvs_flash_erase themselves cause a reset. The value stays constant for the whole boot
       and is published in every health packet. */
    s_rst_reason = (uint8_t)esp_reset_reason();
    ESP_LOGW(TAG, "boot: reset reason = %u (1=poweron 4=panic 6=task_wdt 9=brownout)",
             (unsigned)s_rst_reason);

    /* Dalga tamponu: PSRAM varsa oraya, yoksa statik dizide kalir (davranis degismez).
       sample_task BASLAMADAN once cagrilmali — tampon isaretcisini o okuyor. */
    wave_buf_init();

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 0.9.22 DUZELTME — SIRA: bu cagri 0.9.21'de nvs_flash_init()'ten ONCE duruyordu.
       nvs_open() baslatilmamis NVS'te ESP_ERR_NVS_NOT_INITIALIZED doner, fonksiyon
       sessizce GS_RST_NONE (0) verir ve sebep HICBIR ZAMAN okunmaz. Yani olcmek icin
       eklenen alan, siradan oturu HEP 0 bildiriyordu.
       SAHADA YAKALANDI 2026-09-05 22:44, GEO-A73T7: cihaz kendini yeniden baslatti
       (reset_reason=3 = yazilim, elektrik degil), bosluk 31,1 dk — MQTT esigi 30 dk ile
       ortusuyor, ve kopmadan once wifi_rc=0 oldugu icin WiFi kolu olamaz. Yani watchdog
       ATESLEDI ama sw_rst 0 bildirdi; ceviklikle degil, bu hatayla.
       esp_reset_reason() YUKARIDA KALIR (donanim yazmaci, en basta okunmali); NVS
       okumasinin oyle bir kisiti yok, yalniz NVS'in hazir olmasini ister. */
    /* 0.9.23: NVS'e bagli TUM acilis okumalari (sw_rst, kimlik, kalibrasyon) tek
       fonksiyonda — sira hatasi bir daha dagilmis satirlara saklanamaz. */
    boot_read_nvs_state();

    /* The N2a + GH-DEVPROV boot decision. The MQTT identity arrives via two routes: (a) the admin
       embeds the full mqtt cred at the factory (legacy), (b) the device provisions ITSELF from the
       cloud using claim_proof (self-provisioning). The cases:
       1) NO MQTT:
          - NO claim_proof -> genuinely factory-blank/faulty. Captive DOES NOT OPEN (the customer
            cannot enter MQTT); the admin must write it over USB and reboot. Wait forever.
          - claim_proof PRESENT, NO WiFi -> captive (let the customer enter WiFi) -> reboot -> self-provision.
          - claim_proof PRESENT, WiFi PRESENT -> SELF-PROVISION: fetch the mqtt cred from the cloud over
            HTTPS, write it to NVS, reboot -> a normal boot with mqtt_ready.
       2) MQTT PRESENT but NO WiFi -> captive (the block below, legacy).
       3) BOTH present -> normal boot (below). */
    if (!gs_identity_mqtt_ready(&g_id)) {
        if (!gs_identity_claim_ready(&g_id)) {
            ESP_LOGE(TAG, "No MQTT or claim identity - needs factory prep (admin writes over USB)");
            /* Double flash: a state the customer NEVER sees (the board left the factory without an
               identity, or it is faulty). Distinguishing it from blink matters for the operator — it does
               not mean "waiting for setup", it means "the admin must write the identity over USB". */
            led_wait_forever(LED_WAIT_ID);
        }
        if (!gs_identity_wifi_ready(&g_id)) {
            ESP_LOGW(TAG, "No MQTT, claim present, no WiFi - captive + BLE setup (self-provisions once WiFi is entered)");
            gs_captive_start();          /* BLEPROV: non-blocking — SoftAP + the portal come up */
            gs_bleprov_start(&g_id);     /* BLE setup (mobile app); on failure -> captive alone works */
            /* The branch an OUT-OF-THE-BOX DEVICE enters — this is the only screen the customer sees.
               It is where a steady LED does the most damage; blink is MANDATORY here to match the app text. */
            led_wait_forever(LED_WAIT_SETUP);
        }
        /* claim + WiFi present, no MQTT -> SELF-PROVISION (HTTPS, api.geoshake.org). */
        ESP_LOGW(TAG, "No MQTT identity - self-provisioning (fetching from cloud with claim token)");
        wifi_start();  /* starts STA + SNTP (a clock is needed for the TLS cert notBefore/notAfter) */
        /* If WiFi does not connect (broken cred) the existing 60 s watchdog drops it into in-place captive
           (the customer fixes WiFi -> reboot -> this branch again). If it connects, the guard deletes itself. */
        xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 4, NULL);
        /* Run the provisioning in a SEPARATE 10240-stack task. The TLS handshake (mbedtls + crt_bundle cert
           verification) needs ~8KB of stack; the main task has only 3584B (CONFIG_ESP_MAIN_TASK_STACK_SIZE) ->
           calling gs_provision_run directly HERE produced "stack overflow in task main" + a reboot loop
           (field finding). ota_task is a separate 8192-stack task for the same reason. provision_task waits for
           GOT_IP + SNTP, runs gs_provision_run, and does an esp_restart on success. */
        xTaskCreatePinnedToCore(provision_task, "provision", 10240, NULL, 5, NULL, 0);
        /* Fast blink: a transient state — provision_task will fetch the identity from the cloud and reboot.
           It is distinct from the setup blink because here there is NOTHING for the customer to DO (just wait).
           If it drags on, support can ask "fast or slow?" and tell the stages apart. */
        led_wait_forever(LED_WAIT_PROV);
    }
    if (!gs_identity_wifi_ready(&g_id)) {
        ESP_LOGW(TAG, "No WiFi - starting captive portal + BLE setup (customer enters WiFi)");
        /* Two setup paths SIDE BY SIDE: captive (SoftAP "GeoShake-XXXX" + form, web) + BLE prov
           (mobile app, GST1-XXXXX). Both write via gs_identity_save_wifi -> esp_restart ->
           a normal boot with a complete identity. If BLE fails (memory etc.) captive alone works. */
        gs_captive_start();
        gs_bleprov_start(&g_id);
        /* Waiting for setup after a wifi-reset (MQTT present, no WiFi). From the customer's point of
           view this is THE SAME thing as :1077 ("enter WiFi") -> so the pattern must be THE SAME too;
           a different pattern here would produce confusion, not information. */
        led_wait_forever(LED_WAIT_SETUP);
    }

    /* ---- provisioned (identity complete): the existing boot flow UNCHANGED — NO REGRESSION ---- */
    ESP_LOGI(TAG, "GeoShake %s (%s) starting", g_id.device_id, FW_VERSION);
    /* The device is healthy only if ALL 4 goldens pass; if even one FAILs, led_task lights the panic strobe.
       & (not &&): there must be NO short-circuit — all 3 checks must run and log at boot. */
    s_golden_ok = golden_check()         /* does the C feature computation agree with shared (TS)? */
                & golden_frame_check()   /* does the C frame encoder agree with the Lab .gsd format? */
                & golden_stalta_check()  /* does the C STA/LTA agree with shared (TS)? (Phase 2b) */
                & golden_stalta_freeze_check(); /* 0.9.27: on-tetik LTA dondurma TS ile ayni mi? */
    /* Build the MQTT topics from the identity. g_id was filled above (N1-T2) by gs_identity_load
       and passed both ready-checks (MQTT + WiFi) (provisioned). This must happen BEFORE the sample_task
       (sensors_init) publish and the mqtt subscribe, so that every task sees populated topics. */
    gs_topics_init();
    /* gs_cal_load (CAL-T3) 0.9.23'te boot_read_nvs_state icine tasindi — sensors_init'ten
       once oldugu degismedi, yalniz NVS okumalari tek yerde toplandi. */
    sensors_init();
    wifi_start();
    xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 4, NULL);  /* N2d: 60 s WiFi-fallback watchdog */
    xTaskCreate(wifi_midrun_watchdog_task, "wifi_midrun", 3072, NULL, 3, NULL);  /* 0.9.14: mid-run WiFi kaybi -> 10 dk sonra reboot */
    mqtt_start();
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_task, "mqtt", 4096, NULL, 6, &s_mqtt_task);
    led_init();   /* status LED (GPIO39, LEDC PWM) */
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 3, NULL, 0);

    /* If this is the first boot after an OTA (pending-verify), verify with the self-test / roll back */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "first boot after OTA - starting self-test");
        xTaskCreate(selftest_task, "selftest", 4096, NULL, 4, NULL);
    }
    ESP_LOGI(TAG, "running partition: %s", run->label);
}
