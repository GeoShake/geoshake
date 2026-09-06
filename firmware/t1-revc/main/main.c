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
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "config.h"     /* N1-T5: tum cihazlarda ORTAK sabitler (MQTT_BROKER, FW_VERSION); secrets.h emekli */
#include "identity.h"   /* cihaz kimligi runtime NVS'ten (gs_identity_t); WIFI/DEVICE/MQTT_USER/PASS artik g_id */
#include "captive.h"    /* N1-T3: provisioned-degil dalinda captive portal (SoftAP + form -> NVS) */
#include "bleprov.h"    /* BLEPROV: mobil app BLE kurulumu (captive ile YAN YANA; ayni NVS yolu) */
#include "provision.h"  /* GH-DEVPROV Katman 2: claim-token ile cloud'dan self-provisioning (HTTPS) */
#include "cal.h"        /* CAL-T3: per-sensör gain kalibrasyonu (NVS "geoshake_cal"); boot-load + apply */

/* ---- Pin haritasi: ozel 4-IMU sensor PCB'si (U1/U84/U85/U86) ---- */
#define PIN_SCLK 12
#define PIN_MOSI 11
#define PIN_MISO 13
static const int PIN_CS[4] = {6, 7, 15, 16}; /* sensor 0..3 = U1, U84, U85, U86 */
#define PIN_DRDY 17                          /* IMU1 (U1) INT1 */

/* ---- LED durum gostergesi (GPIO39, firmware'ce simdiye dek surulmuyordu) ----
   LEDC donanim PWM: 1 timer (~5 kHz, 13-bit) + 1 kanal -> GPIO39. led_task surer. */
#define PIN_LED 39
#define LED_ACTIVE_HIGH 1   /* polarite — board ters bagliysa 0 yap */
#define LED_TIMER LEDC_TIMER_0
#define LED_CH    LEDC_CHANNEL_0
#define LED_MAX   8191      /* 13-bit tam olcek duty */
/* LED durum pattern'leri (led_task durum makinesi kullanir) */
#define LED_BURST_MS    2500            /* tetik sonrasi parlak burst suresi */
#define LED_PANIK_MS    150             /* panik kare dalga periyodu (hizli) */
#define LED_BLINK_MS    500             /* baglanti yok yavas blink periyodu */
#define LED_NABIZ_MS    3000            /* normal nabiz (nefes) periyodu */
#define LED_NABIZ_PEAK  ((LED_MAX*3)/10)/* nabiz tepe duty ~%30 (~2457) */

#define REG_INT1_CTRL 0x0D
#define REG_WHO_AM_I  0x0F
#define REG_CTRL1_XL  0x10
#define REG_CTRL2_G   0x11
#define REG_CTRL3_C   0x12
#define REG_CTRL9_XL  0x18
#define REG_OUTX_L_A  0x28
#define WHO_AM_I_VAL  0x6C

/* LSB -> m/s^2 (+-2g tam olcek, 16 bit) — shared SCALE_MS2_PER_LSB ile ayni */
static const double SCALE = (4.0 * 9.80665) / 65536.0;

static const char *TAG = "geoshake";
static spi_device_handle_t s_dev[4];
static TaskHandle_t s_sample_task;
/* mqtt_task handle'i: stack yuksek-su olcumu icin. Buyutulen JSON
   tamponlari (tb[192], buf[320]) bu task'in stack'inde duruyor. */
static TaskHandle_t s_mqtt_task;

/* ---- Cihaz kimligi (runtime NVS) + MQTT topic tamponlari ----
   g_id N1-T2'de app_main'de gs_identity_load ile dolar; bu fazda zero-init (bos)
   kalir -> build/linker dogrulamasi + topic refactor icin yeterli. Eski DEVICE_ID
   literal-concat topic'leri yerine bu tamponlar g_id.device_id'den kurulur (gs_topics_init).
   Boylar device_id[31]+"geoshake/"(9)+"/health"(7)+NUL < 64 -> guvenli. */
static gs_identity_t g_id;
static char g_topic_trig[64];
static char g_topic_feat[64];
static char g_topic_health[64];
static char g_topic_cmd[64];
static char g_topic_sci[64];
/* wave topic seq'li (geoshake/<id>/wave/<seq>) -> sabit degil, kullanim yerinde uretilir. */

/* ---- CAL-T3: per-sensör gain (4 skalar) ----
   Boot'ta gs_cal_load ile NVS "geoshake_cal"den dolar (yoksa hepsi 1.0). sample_task
   read_accel'den sonra acc'ye uygular. gain=1.0 -> byte-özdeş no-op (ölçüm CAL-T4'te). */
static float s_cal_gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

/* ---- CAL-T4: oto-durgun relative-gain olcum sabitleri (hepsi donanimda ayarlanabilir) ----
   CAL_SAMPLES: ~1.5s ornek. ODR 104 Hz konfigure (REG_CTRL1_XL=0x40), efektif ~110 Hz
   (bkz. WIN_MAX/science yorumlari) -> 160 ornek ≈ 1.45..1.54s (WIN_MAX=160 ile ayni buyukluk).
   CAL_STILL_TOL: durgunluk esigi — CV^2 (var/mean^2); 0.01 = %10 CV, ustunde "hareketli".
   CAL_MIN_MAG: olu/kopuk sensör esigi — ham LSB; saglikli dinlenme ~16384 (=1g), 8000 ≈ 0.5g. */
#define CAL_SAMPLES   160
#define CAL_STILL_TOL 0.01f
#define CAL_MIN_MAG   8000.0f

/* Sabit topic'leri g_id.device_id'den kur. Kimlik yuklendikten (N1-T2) sonra,
   ilk publish/subscribe ONCESI cagrilir (app_main). g_id bos ise bos-id topic kurar. */
static void gs_topics_init(void) {
    snprintf(g_topic_trig,   sizeof(g_topic_trig),   "geoshake/%s/trig",   g_id.device_id);
    snprintf(g_topic_feat,   sizeof(g_topic_feat),   "geoshake/%s/feat",   g_id.device_id);
    snprintf(g_topic_health, sizeof(g_topic_health), "geoshake/%s/health", g_id.device_id);
    snprintf(g_topic_cmd,    sizeof(g_topic_cmd),    "geoshake/%s/cmd",    g_id.device_id);
    snprintf(g_topic_sci,    sizeof(g_topic_sci),    "geoshake/%s/sci",    g_id.device_id);
}

/* ---- Ozellik hesabi (shared computeWindowFeatures'i BIREBIR aynalar) ----
   Girdi: n ornek, her ornek 12 float (4 sensor x 3 eksen, sensor-major,
   gravity ÇIKARILMIŞ). 4 sensor ortalanir, eksen RMS + tepe vektor (PGA). */
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

/* ---- Faz 2b: cihaz-içi STA/LTA tespit — shared/stalta.ts'i BİREBİR aynalar ----
   Sabitler STALTA_PARAMS ile AYNI olmalı (golden_stalta_check kilitler). */
#define STALTA_ALPHA_HP   0.0115
#define STALTA_N_STA      55.0
#define STALTA_N_LTA      3270.0
#define STALTA_LTA_MIN    1e-8
#define STALTA_TRIGGER    6.0
#define STALTA_DETRIGGER  2.0
#define STALTA_WARMUP     3270u

typedef struct { double dc[3], sta, lta; bool armed, triggered; uint32_t count; } stalta_t;
typedef struct { bool fired; double ratio, pga; } stalta_out_t;

static stalta_out_t stalta_step(stalta_t *s, const double sample[12]) {
    /* 4-sensör eksen ortalaması — shared staltaStep ile BİREBİR (indeks + op-order).
       Ortalama İÇERİDE: fonksiyon sınırı TS ile aynı, golden ortalama-sırasını da kilitler. */
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
    double lta = s->triggered ? s->lta : s->lta + (cf - s->lta) / STALTA_N_LTA;
    if (lta < STALTA_LTA_MIN) lta = STALTA_LTA_MIN;
    double ratio = sta / lta;
    s->dc[0] = dcx; s->dc[1] = dcy; s->dc[2] = dcz; s->sta = sta; s->lta = lta; s->count++;
    bool fired = false;
    if (s->count >= STALTA_WARMUP && s->armed && ratio >= STALTA_TRIGGER) {
        fired = true; s->triggered = true; s->armed = false;
    }
    if (s->triggered && ratio <= STALTA_DETRIGGER) { s->triggered = false; s->armed = true; }
    stalta_out_t o = { fired, ratio, sqrt(cf) };
    return o;
}

static void stalta_init(stalta_t *s) {
    memset(s, 0, sizeof(*s)); s->lta = STALTA_LTA_MIN; s->armed = true;
}

/* Boot golden: shared stalta.test.ts "4 sensors DIFFER" golden'ı ile uyuşmalı.
   12-eleman girdi → ortalama op-order + recurrence BİRLİKTE kilitlenir (review #1).
   Değerler stalta.test.ts'teki kilitli literal'lerle AYNI — TS değişirse ikisini de güncelle. */
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

/* Boot golden check: shared test/features.test.ts'teki iki golden vektor.
   C ve TS hesabinin birebir uyumunu kilitler (eksen-swap / mean-abs-vs-RMS /
   eksik /4 gibi hatalari yakalar). */
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

/* ---- Faz 0 .gsd frame (science modu); golden ile Lab formatina kilitli ----
   magic(A5 5A)|ver|flags|seq(u32)|t_us(u64)|12xi16 HAM|crc16(CCITT-FALSE) */
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
/* Boot golden FRAME check: Faz 0 test_framing.py::test_golden_frame_vector hex'i.
   C frame encoder'i Lab'in (.gsd parser) okudugu formata BIREBIR kilitler. */
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

/* Science uplink tamponu (128 frame x 42 = 5376 byte; ~1.16s @110Hz) */
static uint8_t s_sci_buf[128 * FRAME_LEN];

/* Faz 3c: event-triggered dalga formu — ring (olay-öncesi) + collect (olay-sonrası) */
#define RING_FRAMES 256                 /* ~2.3s @110Hz olay-öncesi */
#define WAVE_FRAMES 720                 /* ring + ~4s post (~6.5s toplam) */
static uint8_t s_ring[RING_FRAMES * FRAME_LEN];
static uint8_t s_wave_buf[WAVE_FRAMES * FRAME_LEN];

/* 1 s pencere tamponu (109 Hz'de ~109 ornek; 160 guvenli ust sinir) */
#define WIN_MAX 160
static float s_win[WIN_MAX][12];

/* Son pencere ozellikleri (sample task yazar, B3'te MQTT task okur) */
static features_t s_last_feat;
static int s_last_n = 0;
static double s_last_fs = 0;
static volatile uint32_t s_feat_seq = 0;  /* her yeni pencere artar */

/* ---- Yonelim (orientation): son pencerenin 4-sensor ortalanmis yercekimi (DC) vektoru, m/s^2 ----
   Pencere kapanisinda gravity ZATEN mean[12] olarak hesaplaniyor ve atiliyordu; artik
   saklaniyor. Duran bir ivmeolcerde bu vektor DOGRUDAN cihazin yonelimidir:
   duz masa -> |gz| ~ 9.81, X/Y ~ 0;  duvar -> yatay eksenlerden biri ~ 9.81, gz ~ 0.
   Kullaniciya "duvara mi monte ettin" diye sormaya gerek yok; cihaz tasinirsa kendi gunceller.
   Tespit yoluna (STA/LTA) DOKUNMAZ — yalniz mean[] okunur, hicbir sey degistirilmez.
   Yazan: sample_task (core1), s_feat_seq seqlock'unun ICINDE (s_last_feat ile ayni bolum).
   Okuyan: mqtt_task (core0, health) + sample_task (tetik). grav_suffix seqlock'u uygular.
   Ilk pencere kapanana kadar s_grav_ok=false -> alan pakete HIC eklenmez (sahte [0,0,0] yok). */
static float s_last_grav[3] = {0, 0, 0};
static volatile bool s_grav_ok = false;

/* Yonelim JSON ekini kur (trig + health ORTAK tek-kaynak): ",\"grav\":[x,y,z]" ya da BOS string.
   Bos donmesi normaldir (ilk pencere oncesi) — alan OPSIYONEL, paket yine gecerli JSON.
   %.3f: 0.001 m/s^2 ~ 1.7 LSB; tuketicideki egim cozunurlugu atan(0.001/9.81) ~ 0.006 deg
   -> siniflandirma icin fazlasiyla yeterli, payload maliyeti minimum (+28 byte/paket).
   Donus: yazilan uzunluk (0 = ek yok). Tasma/kodlama hatasi -> ek atlanir (paket bozulmaz). */
static int grav_suffix(char *out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = '\0';
    if (!s_grav_ok) return 0;
    /* seqlock: s_last_feat okumasiyla AYNI idiom — pencere sinirinda yirtik vektor okunmaz. */
    uint32_t seq; float g[3];
    do { seq = s_feat_seq; memcpy(g, s_last_grav, sizeof(g)); } while (seq != s_feat_seq);
    int len = snprintf(out, cap, ",\"grav\":[%.3f,%.3f,%.3f]", g[0], g[1], g[2]);
    if (len < 0 || (size_t)len >= cap) { out[0] = '\0'; return 0; }
    return len;
}

/* Tetik yuku TEK-KAYNAK format: tasma halinde yonelim eki OLMADAN ayni formatla yeniden
   kurulur (iki yerde ayri literal tutulup drift etmesin). Son %s = grav_suffix ciktisi. */
#define TRIG_FMT "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"sta_lta\":%.3f,\"pga\":%.6f%s}"

#define WIFI_FALLBACK_TIMEOUT_MS 60000  /* N2d: boot'ta hic IP yoksa captive */

/* WiFi/zaman durumu (status task okur) */
static volatile bool s_wifi_up = false;
static volatile uint32_t s_wifi_rc = 0;   /* reconnect sayaci */
static volatile bool s_wifi_ever_up = false;     /* N2d: ilk GOT_IP -> creds gecerli (fallback iptal) */
static volatile bool s_captive_fallback = false; /* N2d: yerinde captive aktif -> STA reconnect DURDUR */
static volatile uint32_t s_samples = 0;   /* toplam ornek (DRDY) */
static esp_ip4_addr_t s_ip;

/* MQTT durumu (status_task okur, mqtt block kullanir) */
static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_up = false;
static volatile uint32_t s_mqtt_rc = 0;

/* LED burst tetigi: tetik aninin ms damgasi (esp_timer_get_time()/1000).
   led_task burst penceresini buradan okur; sample_task det.fired aninda set eder. */
static volatile int64_t s_trig_flash_ms = 0;

/* Boot golden saglik bayragi: 3 golden check (ozellik+frame+STA/LTA) HEPSI gecerse true.
   led_task panik modunu buradan suruyor (golden FAIL → C/TS uyumsuz → strobe alarm). */
static volatile bool s_golden_ok = false;

/* Science modu: mqtt_event yalniz REQUEST yazar (atomik 32-bit int); sample_task
   tek sahip — sci_on + deadline'i yalniz o tutar (cross-core yaris yok). */
#define SCI_REQ_NONE (-1000000)
static volatile int32_t s_sci_req = SCI_REQ_NONE;  /* secs: <0 dur, 0 suresiz, >0 sn */

/* CAL-T4: 'cal' cmd bayragi — mqtt_event (core0) set eder, sample_task (core1) tuketir.
   sci idiom (bool atomik; tek sahip). YALNIZ set edilince olcum calisir -> happy-path byte-özdeş. */
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
    /* HAM sensör degeri döndürür — gain UYGULAMAZ. CAL-T4 ölçüm dalı ham büyüklük
       okumalı (kalibre-üstüne-kalibre feedback bug'ı önlenir). Gain YALNIZ sample_task'ta
       apply_cal ile normal pipeline acc'sine uygulanır. Ayrı tut. */
    uint8_t tx[7] = {(uint8_t)(REG_OUTX_L_A | 0x80)}, rx[7] = {0};
    spi_transaction_t t = {.length = 56, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_dev[i], &t));
    for (int a = 0; a < 3; a++)
        out[a] = (int16_t)(rx[1 + 2 * a] | (rx[2 + 2 * a] << 8));
}

/* CAL-T3: int16 doygunluk (gain uygulamasında taşmayı klipsle). gain=1.0'da hiç tetiklenmez. */
static inline int16_t clamp_i16(long v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

/* CAL-T3: read_accel'in doldurdugu acc'ye per-sensör gain uygula (sensör-major).
   TÜM tüketicilerden (stalta_step / encode_frame / science) ÖNCE cagrilir; iki okuma
   sitesinde AYNI islem -> tutarlilik. gain=1.0 -> byte-özdeş no-op: int16 [-32768,32767]
   float'ta tam temsil edilir, *1.0f tam, lroundf tam, clamp sınırda kimliktir -> acc değişmez. */
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

/* B1: ornekleri oku ve say (WiFi yukuyle birlikte SPI/DRDY calisiyor mu).
   Ozellik hesabi B2, MQTT publish B3'te bu task'a eklenecek. */
static int64_t now_ms(void);   /* ileri bildirim — sample_task tetik t_ms'i icin (tanim asagida) */
static void sample_task(void *arg) {
    int16_t acc[4][3];
    /* DRDY self-test: takili INT'i okuma ile temizle (level sinyal) */
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
    stalta_t s_stalta; stalta_init(&s_stalta);   /* Faz 2b: cihaz-içi tespit durumu (yalniz bu task) */
    uint32_t s_trig_seq = 0;
    int ring_w = 0, ring_count = 0;           /* döngüsel ring yazma indeksi + dolu frame */
    uint32_t wave_fseq = 0;                   /* ring frame seq sayacı */
    bool wave_on = false; int64_t wave_deadline = 0; uint32_t wave_seq = 0; int wave_n = 0;
    int64_t win_start = 0;
    int sci_off = 0;          /* science batch tamponundaki byte */
    uint32_t sci_fseq = 0;    /* session ici frame sayaci (gap tespiti) */
    uint32_t sci_dropped = 0; /* mqtt down + tampon dolu -> dusen frame (I3) */
    bool sci_on = false;      /* science durumu — yalniz bu task yazar */
    int64_t sci_deadline = 0; /* 0 = suresiz */
    bool prev_sci = false;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
            for (int i = 0; i < 4; i++) read_accel(i, acc[i]); /* kacirilan kenar kurtar */
            apply_cal(acc);   /* CAL-T3: catch-up okumasi da kalibre (tüketici tutarliligi) */
            continue;
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < 4; i++) read_accel(i, acc[i]);
        apply_cal(acc);       /* CAL-T3: gain uygula — stalta/encode/science ONCE (gain=1.0 -> no-op) */
        s_samples++;

        /* Faz 3c: ham frame'i ring'e yaz (epoch µs damgalı — wave kendi yolu, science boot-µs'ten ayrı) */
        int64_t ep_ms = now_ms();
        int16_t s12r[12];
        for (int i = 0; i < 4; i++) for (int a = 0; a < 3; a++) s12r[i*3 + a] = acc[i][a];
        encode_frame(&s_ring[ring_w * FRAME_LEN], wave_fseq++, (uint64_t)(ep_ms * 1000), s12r);
        ring_w = (ring_w + 1) % RING_FRAMES;
        if (ring_count < RING_FRAMES) ring_count++;

        /* ---- Faz 2b: STA/LTA tespit (örnek-başına) ---- */
        {
            /* Per-sensör ölçekle (acc*SCALE) → stalta_step İÇİNDE ortala. Bu, shared
               staltaStep'in girdi sözleşmesiyle birebir (sample[i]=ham*SCALE), op-order
               aynı; golden_stalta_check farklı-sensör dizisiyle bunu kilitler. */
            double s12d[12];
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++) s12d[i*3 + a] = (double)acc[i][a] * SCALE;
            stalta_out_t det = stalta_step(&s_stalta, s12d);
            if (det.fired && ep_ms > 0 && s_mqtt_up) {
                uint32_t used_seq = s_trig_seq++;
                char gsuf[40];                 /* ",\"grav\":[-19.613,-19.613,-19.613]" = 33 + NUL */
                grav_suffix(gsuf, sizeof(gsuf));
                /* tb[] boy butcesi — host'ta snprintf ile OLCULDU (tahmin degil):
                     saha (GEO-XXXXX):  eksiz  86 -> grav'li 114 byte
                     en kotu hal     :  eksiz 134 -> grav'li 167 (+NUL 168)
                       [dev 31, seq 2^32-1, t_ms int64-max, sta_lta 1.15e11 (sta<=3*(2g)^2,
                        lta>=1e-8), pga 33.97, grav bilesenleri +-2g doygun]
                   160 YETMEZDI (167 > 159) -> 192 (24 byte pay). */
                char tb[192];
                int len = snprintf(tb, sizeof(tb), TRIG_FMT,
                    g_id.device_id, (unsigned long)used_seq, (long long)ep_ms,
                    det.ratio, det.pga, gsuf);
                if (len < 0 || len >= (int)sizeof(tb)) {
                    /* Beklenmedik tasma: tetigi ASLA dusurme (sismik olarak kritik) —
                       opsiyonel yonelim ekini atip yeniden kur (eksiz hal <=134 byte, kesin sigar). */
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
                s_trig_flash_ms = esp_timer_get_time() / 1000;  /* led_task ile AYNI monotonic taban → burst ~LED_BURST_MS sonra soner (epoch-ms degil) */
                if (!wave_on) {   /* olay-öncesi ring'i wave_buf'a kronolojik kopyala */
                    wave_n = 0;
                    int start = (ring_count == RING_FRAMES) ? ring_w : 0;  /* en eski */
                    for (int k = 0; k < ring_count; k++) {
                        int idx = (start + k) % RING_FRAMES;
                        memcpy(&s_wave_buf[wave_n * FRAME_LEN], &s_ring[idx * FRAME_LEN], FRAME_LEN);
                        wave_n++;
                    }
                    wave_on = true; wave_seq = used_seq; wave_deadline = now + 4000000;  /* +4s post */
                }
            }
        }

        /* Faz 3c: wave capture aktifse olay-sonrası frame'leri topla; bitince yayınla */
        if (wave_on) {
            if (wave_n < WAVE_FRAMES)   /* current frame ring'e yazıldı; aynısını wave'e ekle */
                memcpy(&s_wave_buf[wave_n++ * FRAME_LEN], &s_ring[((ring_w + RING_FRAMES - 1) % RING_FRAMES) * FRAME_LEN], FRAME_LEN);
            if (now >= wave_deadline || wave_n >= WAVE_FRAMES) {
                /* seq topic'te → header yok; ≤128 frame chunk'larla yayınla */
                char wt[64];
                snprintf(wt, sizeof(wt), "geoshake/%s/wave/%lu", g_id.device_id, (unsigned long)wave_seq);
                for (int o = 0; o < wave_n; o += 128) {
                    int cnt = (wave_n - o > 128) ? 128 : (wave_n - o);
                    if (s_mqtt_up)
                        esp_mqtt_client_publish(s_mqtt, wt, (const char *)&s_wave_buf[o * FRAME_LEN], cnt * FRAME_LEN, 1, 0);
                }
                ESP_LOGW(TAG, "WAVE #%lu published: %d frames", (unsigned long)wave_seq, wave_n);
                wave_on = false;
            }
        }

        /* Komut tuket: mqtt_event'in postaladigi science istegi — deadline/on'u
           YALNIZ bu task tutar (I1/I2: cross-core yaris yok). */
        int32_t req = s_sci_req;
        if (req != SCI_REQ_NONE) {
            s_sci_req = SCI_REQ_NONE;
            if (req < 0) { sci_on = false; }
            else if (req == 0) { sci_on = true; sci_deadline = 0; }
            else { int v = req > 300 ? 300 : req; sci_on = true; sci_deadline = now + (int64_t)v * 1000000; }
            ESP_LOGI(TAG, "science %s", !sci_on ? "KAPALI" : (sci_deadline ? "ACIK (sureli)" : "ON (unbounded)"));
        }

        /* ---- CAL-T4: oto-durgun relative-gain olcumu (sci idiom: mqtt_event bayrak postalar) ----
           YALNIZ 'cal' cmd geldiginde calisir -> normal akis byte-özdeş. ~1.5s HAM buyukluk topla
           (apply_cal YOK: kalibre-üstüne feedback bug'i onlenir), durgun+canli dogrula, relative
           gain hesapla, NVS'e kaydet + s_cal_gain'e uygula (yaziyi core1/sample_task ICINDE tut:
           reader=writer=core1 -> tearing yok). Olcum penceresi boyunca STA/LTA/encode/science DURUR
           (operator ~1.5s cihazi sabit tutar). Her yol (basari/red) s_cal_req'i temizler. */
        if (s_cal_req) {
            s_cal_req = false;                 /* tek-sahip tuketim: bir kez calis */
            double sum[4] = {0}, sumsq[4] = {0};
            int got = 0;
            bool reject = false;   /* TUM red yollari (dead-DRDY dahil) — loop ICINDE de set edilebilir */
            int stalls = 0;        /* ardisik DRDY-timeout sayaci; canli kenar sifirlar */
            while (got < CAL_SAMPLES) {
                /* DRDY bekle -> task bloklar, WDT beslenir. AMA main loop'un SONSUZ beklemesini
                   AYNALAMA: main loop surekli calismak icin var, cal penceresi ise BITMEK zorunda.
                   DRDY sensör-0/IMU1'den TEK gelir (PIN_DRDY); olcum sirasinda olurse loop asla
                   ilerlemez -> ardisik timeout'lari say, 3'te (~3s; ~110Hz'de kesin olu hat, jitter'a
                   bol pay) REDDET: eski gain korunur, sample_task normal akisa doner (hang YOK). */
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
                    if (++stalls >= 3) {
                        ESP_LOGW(TAG, "cal rejected: no DRDY / timeout - keeping previous gain");
                        reject = true; break;
                    }
                    continue;      /* tek kacirilan kenar; tekrar bekle */
                }
                stalls = 0;        /* canli kenar geldi -> sayaci sifirla */
                for (int i = 0; i < 4; i++) {
                    int16_t raw[3];
                    read_accel(i, raw);        /* HAM — apply_cal YOK (olcum ham buyuklukte olmali) */
                    /* int16^2 x3 int32'yi tasirir -> KARE ONCESI float'a cast et. */
                    float m = sqrtf((float)raw[0] * (float)raw[0] +
                                    (float)raw[1] * (float)raw[1] +
                                    (float)raw[2] * (float)raw[2]);
                    sum[i]   += m;
                    sumsq[i] += (double)m * m;
                }
                got++;
            }
            /* dead-DRDY reddi (reject) buraya duserse asagidaki analizin TAMAMI atlanir
               (eksik/bos accumulator'da bolme YOK) -> dogrudan n=0; continue. */
            float mean[4], gain[4];
            if (!reject) {
                /* Per-sensör ortalama + varyans (double birikimden: sumsq/N - mean^2 cancellation'a hassas). */
                for (int i = 0; i < 4; i++) {
                    double mn  = sum[i] / CAL_SAMPLES;
                    double var = sumsq[i] / CAL_SAMPLES - mn * mn;
                    if (var < 0) var = 0;          /* float hatasi -> kucuk negatif; 0'a kelepcele */
                    mean[i] = (float)mn;
                    /* 1) olu/kopuk sensör ONCE: dinlenmede saglikli sensör ~1g okur, kopuk ~0.
                       Once bunu bak — olu sensörun CV'si anlamsizdir (yoksa "hareketli" yanlis raporlanir). */
                    if (mn < (double)CAL_MIN_MAG) {
                        ESP_LOGW(TAG, "cal rejected: sensor %d dead/disconnected", i);
                        reject = true; break;
                    }
                    /* 2) durgun mu? CV^2 testi: var > tol*mean^2 -> cihaz hareketli (hangi sensör + cv2 logla) */
                    if (var > (double)CAL_STILL_TOL * mn * mn) {
                        ESP_LOGW(TAG, "cal rejected: sensor %d is moving (cv2=%.4f > %.4f; keep the device still)",
                                 i, var / (mn * mn), (double)CAL_STILL_TOL);
                        reject = true; break;
                    }
                }
            }
            if (!reject) {
                /* relative gain: her sensörü dizinin ortalamasina esitle (offset kalibre EDILMEZ) */
                float mean_avg = (mean[0] + mean[1] + mean[2] + mean[3]) / 4.0f;
                for (int i = 0; i < 4; i++) gain[i] = mean_avg / mean[i];
                /* 3) gain aralik disi -> donanim arizasi (birim varyansi <%1); sessiz clamp DEGIL, reddet */
                for (int i = 0; i < 4; i++)
                    if (gain[i] < CAL_GAIN_MIN || gain[i] > CAL_GAIN_MAX) {
                        ESP_LOGW(TAG, "cal rejected: gain[%d]=%.3f out of range (faulty?)", i, gain[i]);
                        reject = true; break;
                    }
            }
            if (!reject) {
                esp_err_t ce = gs_cal_save(gain);
                if (ce == ESP_OK) {
                    memcpy(s_cal_gain, gain, sizeof(s_cal_gain));   /* in-task yaz (tearing yok) */
                    ESP_LOGI(TAG, "cal OK: gain=[%.3f %.3f %.3f %.3f]",
                             gain[0], gain[1], gain[2], gain[3]);
                } else {
                    ESP_LOGE(TAG, "cal could not be saved (e=%s) - keeping previous gain",
                             esp_err_to_name(ce));
                }
            }
            n = 0;         /* olcum ~1.5s pencereyi kesti; ozellik penceresini temiz baslat */
            continue;      /* bu iterasyonun kalanini (science/window) atla, normal akisa don */
        }

        /* ---- Science uplink: HAM int16 frame'leri batch'le ---- */
        if (sci_on) {
            if (!prev_sci) { sci_off = 0; sci_fseq = 0; sci_dropped = 0; prev_sci = true; } /* session basi */
            int16_t s12[12];
            for (int i = 0; i < 4; i++)
                for (int a = 0; a < 3; a++) s12[i * 3 + a] = acc[i][a];
            if (sci_off + FRAME_LEN > (int)sizeof(s_sci_buf)) { /* tampon dolu: flush ya da dusur */
                if (s_mqtt_up)
                    esp_mqtt_client_publish(s_mqtt, g_topic_sci,
                                            (const char *)s_sci_buf, sci_off, 0, 0);
                else
                    sci_dropped += sci_off / FRAME_LEN;  /* mqtt yok -> batch'i dusur */
                sci_off = 0;
            }
            encode_frame(&s_sci_buf[sci_off], sci_fseq++, (uint64_t)now, s12);
            sci_off += FRAME_LEN;
            if (sci_deadline && now >= sci_deadline) sci_on = false; /* sure doldu */
        } else if (prev_sci) {
            if (sci_off > 0 && s_mqtt_up)  /* session sonu: kalan batch'i flush et */
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
        /* pencereyi 1 s'de (veya tampon dolunca) kapat */
        if (now - win_start >= 1000000 || n >= WIN_MAX) {
            double elapsed = (now - win_start) / 1e6;
            /* Gravity (DC) cikar: her (sensor,eksen) icin pencere ortalamasini cikar */
            double mean[12] = {0};
            for (int k = 0; k < n; k++)
                for (int j = 0; j < 12; j++) mean[j] += s_win[k][j];
            for (int j = 0; j < 12; j++) mean[j] /= n;
            for (int k = 0; k < n; k++)
                for (int j = 0; j < 12; j++) s_win[k][j] -= (float)mean[j];
            features_t f = compute_features(s_win, n);
            s_last_feat = f;             /* B3 MQTT publish bunu kullanacak */
            s_last_n = n;
            s_last_fs = n / elapsed;
            /* Yonelim: cikarilan gravity'yi atma — 4 sensoru compute_features/stalta_step ile
               AYNI op-order'da ortala. mean[] SALT-OKUNUR kullanilir; ozellik/tespit yolu
               degismez (golden'lar etkilenmez). s_feat_seq++ ONCESI yazilir -> seqlock butun. */
            s_last_grav[0] = (float)((mean[0] + mean[3] + mean[6] + mean[9])  / 4.0);
            s_last_grav[1] = (float)((mean[1] + mean[4] + mean[7] + mean[10]) / 4.0);
            s_last_grav[2] = (float)((mean[2] + mean[5] + mean[8] + mean[11]) / 4.0);
            s_grav_ok = true;
            s_feat_seq++;
            ESP_LOGI(TAG, "feat n=%d fs=%.1f rms=%.5f,%.5f,%.5f pga=%.5f",
                     n, s_last_fs, f.rms[0], f.rms[1], f.rms[2], f.pga);
            n = 0;
            /* science aktifse biriken batch'i pencere sonunda (~1s) flush et */
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
        write_reg(i, REG_CTRL2_G, 0x00);   /* gyro kapali */
        write_reg(i, REG_CTRL9_XL, 0xE2);  /* I3C disable */
        write_reg(i, REG_CTRL1_XL, 0x40);  /* 104 Hz, +-2g */
    }
    xTaskCreatePinnedToCore(sample_task, "sample", 4096, NULL, 10, &s_sample_task, 1);
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_DRDY, .mode = GPIO_MODE_INPUT,
                        .intr_type = GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_DRDY, drdy_isr, NULL));
    write_reg(0, REG_INT1_CTRL, 0x01);     /* INT routing kesme KURULDUKTAN sonra */
    ESP_LOGI(TAG, "4x LSM6DSO init OK (104 Hz, +-2g)");
}

/* ---------------- WiFi + SNTP ---------------- */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_captive_fallback) return;    /* N2d: yerinde captive'e gecis — STA'yi yeniden baglama */
        if (s_wifi_up) s_wifi_rc++;        /* yalniz gercek kopmalari say */
        s_wifi_up = false;
        esp_wifi_connect();                /* yeniden dene */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* N2d: BILEREK s_captive_fallback korumasiz — fallback sonrasi gec gelen GOT_IP zararsiz (reconnect yok; cihaz reboot edecek), guard EKLEME. */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_ip = e->ip_info.ip;
        s_wifi_up = true;
        s_wifi_ever_up = true;             /* N2d: en az bir kez baglandi -> creds gecerli */
        ESP_LOGI(TAG, "WiFi GOT_IP: " IPSTR, IP2STR(&s_ip));
    }
}

/* N2d: provisioned-path watchdog. 60sn icinde hic IP yoksa (creds bozuk/erisilmez) ->
   yerinde captive (reboot yok, creds korunur). Bir kez baglandiysa (s_wifi_ever_up)
   kendini siler; sonraki kopmalar bugunku gibi sonsuz retry (gecici kesinti captive'e dusurmez). */
static void wifi_guard_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(WIFI_FALLBACK_TIMEOUT_MS));
    if (s_wifi_ever_up) {
        ESP_LOGI(TAG, "WiFi connected - guard exiting (no fallback needed)");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "WiFi did not connect in %d s - in-place captive fallback", WIFI_FALLBACK_TIMEOUT_MS / 1000);
    s_captive_fallback = true;          /* esp_wifi_stop ONCESI: DISCONNECTED reconnect denemesin */
    gs_captive_start_inplace();          /* BLEPROV: artik noblock — portal acilir, doner */
    gs_bleprov_start(&g_id);             /* BLE kurulum da acik (app kurtarma yolu); fail -> yalniz captive */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));   /* bu task burada bekler; kurulum esp_restart eder */
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi started, SSID=%s", g_id.wifi_ssid);

    /* Sabit sunucu (pool.ntp.org); DHCP'den NTP alma KAPALI — o ozellik
       CONFIG_LWIP_DHCP_GET_NTP_SRV gerektirir, gereksiz. Default config start=true. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
}

static bool time_synced(void) {
    time_t now = 0;
    time(&now);
    return now > 1700000000;  /* 2023-11 sonrasi => SNTP oturdu */
}

static void status_task(void *arg) {
    uint32_t last = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t s = s_samples;
        int8_t rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
        /* Stack yuksek-su seviyesi (KALAN bos bayt, en kotu ana kadar).
           Bu projede sahada bir stack overflow yasandi (TLS main task'ta
           calisiyordu, 3584 B yetmedi -> ayri 8 KB'lik task'a alindi).
           O yuzden yeni bir alan eklendiginde "hesapla guvenli" demek
           yetmiyor; olculebilir olmali. Maliyeti bir sayac okumasi.
           sample = sismik dongu, self = bu status task'i.
           Alarm esigi: 512 B'nin altina inerse bakilmali. */
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
static volatile bool s_ota_busy = false;

static void ota_task(void *arg) {
    char *url = (char *)arg;
    ESP_LOGW(TAG, "OTA starting: %s", url);
    s_sci_req = -1;  /* science'i durdur (bant cakismasin) */
    esp_http_client_config_t http = { .url = url, .timeout_ms = 30000, .keep_alive_enable = true };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_err_t e = esp_https_ota(&cfg);  /* imza CONFIG_SECURE_SIGNED_ON_UPDATE ile dogrulanir */
    if (e == ESP_OK) { ESP_LOGW(TAG, "OTA OK - rebooting"); esp_restart(); }
    ESP_LOGE(TAG, "OTA FAIL: %s - staying on the current app (science stopped, "
             "gerekirse tekrar baslat)", esp_err_to_name(e));  /* I1 */
    free(url);
    s_ota_busy = false;
    vTaskDelete(NULL);
}

/* ---------------- MQTT publisher ---------------- */
static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_mqtt_up = true;
        esp_mqtt_client_subscribe(s_mqtt, g_topic_cmd, 0);
        ESP_LOGI(TAG, "MQTT connected: %s (cmd subscribe)", MQTT_BROKER);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        if (s_mqtt_up) s_mqtt_rc++;
        s_mqtt_up = false;
    } else if (id == MQTT_EVENT_DATA) {
        /* Topic guard (C1) + parcalanma guard (C2): yalniz TAM .../cmd payload'u komut say.
           Cok subscription / fragmente mesaj geldiginde yanlis parse'i onler. */
        const char *CMD_TOPIC = g_topic_cmd;   /* runtime kimlik topic'i; guard mantigi aynen korunur */
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) return;
        if (e->topic == NULL || e->topic_len != (int)strlen(CMD_TOPIC) ||
            strncmp(e->topic, CMD_TOPIC, e->topic_len) != 0) return;
        cJSON *j = cJSON_ParseWithLength(e->data, e->data_len);
        if (j) {
            cJSON *cmd = cJSON_GetObjectItem(j, "cmd");
            if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "sci") == 0) {
                cJSON *secs = cJSON_GetObjectItem(j, "secs");
                if (cJSON_IsNumber(secs)) {
                    s_sci_req = secs->valueint;   /* sample_task tuketecek (tek sahip) */
                    ESP_LOGI(TAG, "cmd: sci secs=%d", secs->valueint);
                }
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ota") == 0) {
                cJSON *url = cJSON_GetObjectItem(j, "url");
                /* M2: mqtt:// kanali kimliksiz — CLI dogrulamasina guvenme, firmware'de
                   zorla: yalniz http(s)://, makul uzunluk. Imza yine butunlugu garantiler
                   (yetkisiz kod calismaz) ama garip url DoS/SSRF onlenir. */
                if (cJSON_IsString(url) && !s_ota_busy) {
                    const char *us = url->valuestring;
                    size_t ul = strlen(us);
                    bool ok = ul >= 8 && ul < 256 &&
                              (strncmp(us, "http://", 7) == 0 || strncmp(us, "https://", 8) == 0);
                    if (!ok) {
                        ESP_LOGW(TAG, "OTA url rejected (not http(s):// or bad length)");
                    } else {
                        s_ota_busy = true;
                        char *u = strdup(us);   /* ota_task free eder */
                        if (u) xTaskCreatePinnedToCore(ota_task, "ota", 8192, u, 5, NULL, 0);
                        else s_ota_busy = false;  /* I2: core 0'a pinle (core 1 sampling'i korumak) */
                    }
                }
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "reset-id") == 0) {
                /* N1-T4 (N2a-guncel): TAM fabrika-sifirlama. NVS "geoshake" kimligini KOMPLE sil
                   (MQTT + WiFi) -> reboot. T2 boot-karar sonrasi mqtt_ready=false -> cihaz
                   factory-wait'e duser (admin USB ile MQTT NVS yazana kadar bekler; captive ACILMAZ).
                   Parametresiz; cihazi fabrikaya dondurur (panel N3 window.confirm sorar). */
                gs_identity_erase();
                ESP_LOGW(TAG, "Identity erased (reset-id, FULL) - rebooting (factory-wait; admin USB)");
                vTaskDelay(pdMS_TO_TICKS(500));   /* log flush + mqtt ack icin kisa gecikme */
                esp_restart();
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "reset-wifi") == 0) {
                /* N2a: musteri-guvenli WiFi sifirlama. YALNIZ WiFi anahtarlarini sil; MQTT kimligi
                   (admin esptool ile gomdu) korunur. Boot'ta mqtt_ready && !wifi_ready -> captive
                   portal (musteri WiFi'yi yeniden girer). Cihaz fabrikaya DONMEZ. */
                gs_identity_erase_wifi();
                ESP_LOGW(TAG, "WiFi erased (reset-wifi) - rebooting (captive; MQTT preserved)");
                vTaskDelay(pdMS_TO_TICKS(500));   /* log flush + mqtt ack icin kisa gecikme */
                esp_restart();
            } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "cal") == 0) {
                /* CAL-T4: oto-durgun kalibrasyon. Parametresiz — payload "action" alani (varsa)
                   yok sayilir (tek mod). sample_task (core1) tuketir; sci idiom (tek sahip, yaris yok). */
                s_cal_req = true;
                ESP_LOGI(TAG, "cmd: cal (auto still-state calibration)");
            }
            cJSON_Delete(j);
        }
    }
}

/* Let's Encrypt kok sertifikalari (ISRG Root X1 RSA + X2 ECDSA) — PUBLIC, commit edilebilir.
   T7 review CRITICAL: Caddy default'u ECDSA cert uretir (zincir -> ISRG Root X2); YALNIZ X1
   pin'lersek cihaz X2 zincirini dogrulayamaz -> mqtts el sikisma FAIL -> soak boyunca sessiz
   baglanamama. Ikisini de gomeriz -> Caddy ne uretirse (RSA->X1 / ECDSA->X2) cihaz dogrular.
   X1 SHA-256: 96:BC:EC:06:...:08:C6   X2 SHA-256: 69:72:9B:8E:...:14:70 (ikisi de openssl-dogrulandi) */
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
/* ISRG Root X2 (ECDSA P-384) — Caddy default ECDSA cert bu koke chain'lenir */
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
    /* buffer.size: varsayilan 1024 science batch'ine (~4.6KB) yetmez -> publish sessiz fail.
       8192 batch + feat/health icin yeterli.
       TLS (Faz C T7): MQTT_BROKER mqtts:// ise esp-mqtt TLS el sikisma yapar, sunucu
       sertifikasini ISRG Root X1'e karsi dogrular ve user/pass gonderir. mqtt:// (local
       1883, anonim) ise sema geregi TLS kapali kalir; CA + bos kimlik yok sayilir. */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER,
        .broker.verification.certificate = ISRG_ROOTS,
        .credentials.username = g_id.mqtt_user,
        .credentials.authentication.password = g_id.mqtt_pass,
        .buffer.size = 8192,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t e = esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "mqtt_start: broker=%s result=%s", MQTT_BROKER, esp_err_to_name(e));
}

/* SNTP synced ise gercek epoch ms, degilse 0 (shared sema 0'i kabul eder) */
static int64_t now_ms(void) {
    if (!time_synced()) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void mqtt_task(void *arg) {
    /* buf[] boy butcesi — health en uzun yuk; host'ta snprintf ile OLCULDU (tahmin degil):
         saha (GEO-XXXXX): eksiz 171 -> grav'li 199 byte
         en kotu hal     : grav'li 284 (+NUL 285)  [dev 31, sayaclar 2^32-1, t_ms int64-max,
                           fw 25, gain 4x2.0 (cal.h clamp), grav bilesenleri +-2g doygun]
       256 YETMEZDI (284 > 255) -> 320 (35 byte pay). feat yuku en kotu 165, ayni tamponda sigar. */
    char buf[320];
    uint32_t last_feat = 0, pubseq = 0, hseq = 0;
    int64_t last_health = -30000000;  /* ilk health hemen gitsin */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_mqtt_up) continue;

        /* yeni pencere -> feat publish (seqlock'lu tutarli okuma) */
        uint32_t seq;
        features_t f; int n; double fs;
        do { seq = s_feat_seq; f = s_last_feat; n = s_last_n; fs = s_last_fs; }
        while (seq != s_feat_seq);
        if (seq != last_feat && n > 0) {
            last_feat = seq;   /* saat yoksa da ilerlet: pencereyi ATLA, sonra bayat yayinlama */
            /* 0.9.3: SAAT YOKSA YAYINLAMA. now_ms() SNTP oturmadan 0 doner ve MQTT,
               SNTP'den ONCE baglanabiliyor -> o pencerede giden feat'ler t_ms=0 tasiyordu.
               Tetik yayini bunu zaten suzuyordu (asagida ep_ms > 0); feat suzmuyordu ->
               kayitlar SeedLink ring'ine 1970-01-01 damgasiyla dusup KALICI epoch-zero
               stream'i yaratti (saha: GS_FVYSA_00_L_N_Z). Feeder tarafinda da suzgec var
               (MIN_EPOCH_MS) ama dogru yer kaynak: bozuk damga cihazdan hic cikmasin.
               health BILINCLI olarak korunmuyor — saat yokken bile cihaz gorunur kalmali
               (fw/up_s/heap/rssi teshis icin gerekir; t_ms=0 orada "saat yok" sinyalidir). */
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

        /* 30 s'de bir health */
        int64_t t = esp_timer_get_time();
        if (t - last_health >= 30000000) {
            last_health = t;
            int8_t rssi = 0;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
            /* Yonelim health'te de gider: tetik nadirdir, oysa "cihaz tasindi" bilgisi 30 s
               icinde guncellenmeli. gain ile ayni yer/gerekce (yavas degisen cihaz ozelligi). */
            char gsuf[40];
            grav_suffix(gsuf, sizeof(gsuf));
            int len = snprintf(buf, sizeof(buf),
                "{\"v\":1,\"dev\":\"%s\",\"seq\":%lu,\"t_ms\":%lld,\"up_s\":%lu,\"heap\":%lu,"
                "\"rssi\":%d,\"wifi_rc\":%lu,\"mqtt_rc\":%lu,\"fw\":\"%s\","
                "\"gain\":[%.4f,%.4f,%.4f,%.4f]%s}",
                g_id.device_id, (unsigned long)hseq++, (long long)now_ms(),
                (unsigned long)(t / 1000000), (unsigned long)esp_get_free_heap_size(),
                rssi, (unsigned long)s_wifi_rc, (unsigned long)s_mqtt_rc, FW_VERSION,
                s_cal_gain[0], s_cal_gain[1], s_cal_gain[2], s_cal_gain[3], gsuf);
            if (len > 0 && len < (int)sizeof(buf))
                esp_mqtt_client_publish(s_mqtt, g_topic_health, buf, len, 0, 0);
            else
                ESP_LOGE(TAG, "health payload encode failed (%d) - dropped", len);
        }
    }
}

/* OTA sonrasi ilk boot: 30 s icinde WiFi+MQTT gelirse app gecerli, yoksa rollback. */
static void selftest_task(void *arg) {
    for (int i = 0; i < 30; i++) {
        if (s_wifi_up && s_mqtt_up) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "OTA self-test PASSED - app marked valid");
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "OTA self-test FAILED - rollback + reboot");
    esp_ota_mark_app_invalid_rollback_and_reboot();  /* eski app'e doner */
}

/* ---------------- LED durum gostergesi ----------------
   LEDC PWM ile GPIO39. led_set polariteyi + clamp'i uygular; led_init donanimi
   kurar; led_task tek surucu (durum makinesi Task 2'de). */
static void led_set(int duty) {
    if (duty < 0) duty = 0;
    if (duty > LED_MAX) duty = LED_MAX;
    int applied = LED_ACTIVE_HIGH ? duty : (LED_MAX - duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CH, applied);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CH);
}

static void led_init(void) {
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,   /* 13-bit -> LED_MAX 8191 */
        .timer_num       = LED_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num   = PIN_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LED_CH,
        .timer_sel  = LED_TIMER,
        .duty       = 0,          /* sonuk basla */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    ESP_LOGI(TAG, "LED init OK (GPIO%d, LEDC 5kHz 13-bit)", PIN_LED);
}

/* ---- Bekleme modu desenleri (app_main'in DORT bekleme dali) ----
   Gerekce: bu dallarda normal boot'un led_init/led_task'ina hic gelinmiyor; GPIO39
   SURULMEZSE LED sabit YANIK kalir ve musteri bunu "cihaz bozuk" diye okur
   (saha: GEO-BH2JN, 2026-07-25). Uygulama uc dilde "isigin yanip sondugunden emin
   ol" diyor — dogru olan metin, cihazdi yanlis olan (karar: 2026-07-27).

   Desenler AYIRT EDILEBILIR, ama musterinin gordugu her bekleme durumu "yanip
   sonuyor" olarak okunur — app metni her kosulda dogru kalir. Kasitli olarak
   NABIZ (yumusak nefes) KULLANILMADI: o desen led_task'ta "saglikli ve bagli"
   demek; bekleme icin kullanilsa tam da duzeltmeye calistigimiz karisikligi uretir. */
typedef enum {
    LED_WAIT_SETUP,   /* yavas blink 500ms — MUSTERI: WiFi kurulumu bekleniyor */
    LED_WAIT_PROV,    /* hizli blink 200ms — GECICI: cloud'dan kimlik aliniyor */
    LED_WAIT_ID,      /* cift flas — YALNIZ OPERATOR: kimlik yok, fabrika hazirligi gerek */
} led_wait_t;

#define LED_WAIT_FAST_MS 200    /* self-provisioning blink periyodu */
#define LED_DFLASH_MS   2000    /* cift flas tam periyodu */
#define LED_DFLASH_ON    120    /* her flasin suresi */
#define LED_DFLASH_GAP   200    /* iki flas arasi karanlik */

/* Cift flas: [0,120) yanik — [120,320) sonuk — [320,440) yanik — kalani sonuk. */
static bool led_dflash_on(int64_t t) {
    if (t < LED_DFLASH_ON) return true;
    const int64_t s = LED_DFLASH_ON + LED_DFLASH_GAP;
    return (t >= s && t < s + LED_DFLASH_ON);
}

/* Deseni sur + SONSUZ bekle (DONMEZ). 20ms tik: cift flasin 120ms'lik parcasi da,
   200ms'lik hizli blink de net gorunsun (30ms'te duty jitter'i deseni bulaniklastirir).
   Bu dalda baska is yok — tik maliyeti onemsiz. */
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

/* Durum makinesi: oncelik yuksek->dusuk = panik > burst > blink > nabiz.
   Yaklasim: her tikte hedef duty hesapla, gercek duty'yi hedefe dogru ramp et
   (manuel ramp, ISR yok — en basit, donanim fade kurulumu gerekmez). Boylece
   tepe/dip gecisleri sert sicramaz, nabiz nefes gibi yumusak akar. */
static void led_task(void *arg) {
    int duty = 0;                 /* o anki uygulanan duty (ramp icin tutulur) */
    const int RAMP = LED_MAX / 6; /* tik basina max duty degisimi (~30ms'de) */
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;   /* ms */
        int target;

        /* 1) panik (golden FAIL): hizli kare dalga, tam parlak.
           golden checklerden biri bile patlamissa (C/TS uyumsuz) cihaz guvenilmez → strobe. */
        bool panik = !s_golden_ok;

        /* 2) burst (DEPREM TETIK): tetikten sonra LED_BURST_MS boyunca parlak.
           tetik aninda s_trig_flash_ms set edilir (sample_task) → burst fire eder. */
        bool burst = (s_trig_flash_ms > 0) && (now - s_trig_flash_ms < LED_BURST_MS);

        if (panik) {
            /* kare dalga: periyodun ilk yarisi tam parlak, ikinci yarisi sonuk */
            target = ((now % LED_PANIK_MS) < (LED_PANIK_MS / 2)) ? LED_MAX : 0;
        } else if (burst) {
            target = LED_MAX;     /* sabit tam parlak */
        } else if (!(s_wifi_up && s_mqtt_up)) {
            /* 3) blink (baglanti yok): yavas kare dalga */
            target = ((now % LED_BLINK_MS) < (LED_BLINK_MS / 2)) ? LED_MAX : 0;
        } else {
            /* 4) nabiz (normal): ~3s periyotlu ucgen dalga, tepe ~%30 -> nefes hissi */
            int phase = now % LED_NABIZ_MS;          /* 0..LED_NABIZ_MS */
            int half  = LED_NABIZ_MS / 2;
            int tri   = (phase < half) ? phase : (LED_NABIZ_MS - phase); /* 0..half ucgen */
            target = (int)((int64_t)LED_NABIZ_PEAK * tri / half);        /* 0..peak */
        }

        /* panik: ramp'i ATLA — alarm sert karanlik↔parlak strobe olmali; ramp 150ms kareyi
           orta-band throb'a yumusatir. Diger durumlar nefes hissi icin ramp'i korur. */
        if (panik) {
            duty = target;
        } else if (duty < target) { duty += RAMP; if (duty > target) duty = target; }
        else if (duty > target) { duty -= RAMP; if (duty < target) duty = target; }
        led_set(duty);

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* GH-DEVPROV self-provisioning worker — AYRI task (main task 3584B stack TLS handshake'e yetmez →
   dogrudan cagirinca "stack overflow in task main"). 10240 stack: mbedtls TLS + crt_bundle cert
   dogrulama + cJSON marji. Akis: GOT_IP bekle → SNTP bekle → gs_provision_run backoff-retry →
   OK'te esp_restart (reboot sonrasi mqtt_ready → normal boot). g_id/s_wifi_ever_up/time_synced main.c'de. */
static void provision_task(void *arg) {
    (void)arg;
    while (!s_wifi_ever_up) vTaskDelay(pdMS_TO_TICKS(250));  /* GOT_IP bekle (WiFi fail'i wifi_guard devralir) */
    /* SNTP oturana kadar bekle (max ~30sn) — cert zaman dogrulamasi saat 1970'te patlar.
       Oturmasa bile devam: retry dongusu TLS fail'i yakalar, arada SNTP oturur. */
    for (int w = 0; !time_synced() && w < 30000; w += 500) vTaskDelay(pdMS_TO_TICKS(500));
    for (int attempt = 1; ; attempt++) {
        if (gs_provision_run(&g_id) == ESP_OK) {
            ESP_LOGW(TAG, "self-provisioning OK - rebooting (normal boot with mqtt_ready)");
            esp_restart();
        }
        int backoff = attempt < 6 ? attempt * 10 : 60;  /* 10,20,30,40,50,60s cap */
        ESP_LOGW(TAG, "self-provisioning failed (attempt %d) - retrying in %d s", attempt, backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
    }
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* N1-T2: cihaz kimligini NVS "geoshake" namespace'inden yukle. ESP_ERROR_CHECK
       KULLANILMAZ — NOT_FOUND (provisioned-degil) beklenen, kurtarilabilir durum.
       Banner/gs_topics_init/WiFi/MQTT gercek kimligi gormesi icin bunlardan ONCE. */
    gs_identity_load(&g_id);

    /* N2a + GH-DEVPROV boot-karar. MQTT kimligi iki yoldan gelir: (a) admin fabrikada full
       mqtt cred gomer (legacy), (b) cihaz claim_proof ile cloud'dan KENDI provision eder
       (self-provisioning). Durumlar:
       1) MQTT YOK:
          - claim_proof YOK -> gercek fabrika-blank/arizali. Captive ACILMAZ (musteri MQTT
            giremez); admin USB'den yazip reboot etmeli. Sonsuz bekle.
          - claim_proof VAR, WiFi YOK -> captive (musteri WiFi girsin) -> reboot -> self-provision.
          - claim_proof VAR, WiFi VAR -> SELF-PROVISION: HTTPS ile cloud'dan mqtt cred al,
            NVS'e yaz, reboot -> mqtt_ready ile normal boot.
       2) MQTT VAR ama WiFi YOK -> captive (asagidaki blok, legacy).
       3) Ikisi de VAR -> normal boot (asagi). */
    if (!gs_identity_mqtt_ready(&g_id)) {
        if (!gs_identity_claim_ready(&g_id)) {
            ESP_LOGE(TAG, "No MQTT or claim identity - needs factory prep (admin writes over USB)");
            /* Cift flas: musterinin ASLA gormedigi durum (kart fabrikadan kimliksiz cikmis
               ya da arizali). Operator icin blink'ten ayrilmasi onemli — "kurulum bekliyor"
               degil, "admin USB'den kimlik yazmali" demek. */
            led_wait_forever(LED_WAIT_ID);
        }
        if (!gs_identity_wifi_ready(&g_id)) {
            ESP_LOGW(TAG, "No MQTT, claim present, no WiFi - captive + BLE setup (self-provisions once WiFi is entered)");
            gs_captive_start();          /* BLEPROV: noblock — SoftAP + portal acilir */
            gs_bleprov_start(&g_id);     /* BLE kurulum (mobil app); fail -> yalniz captive calisir */
            /* KUTUDAN CIKAN CIHAZIN girdigi dal — musterinin gordugu tek ekran burasi.
               Sabit LED'in en cok zarar verdigi yer; app metniyle uyum icin blink SART. */
            led_wait_forever(LED_WAIT_SETUP);
        }
        /* claim + WiFi var, MQTT yok -> SELF-PROVISION (HTTPS, api.geoshake.org). */
        ESP_LOGW(TAG, "No MQTT identity - self-provisioning (fetching from cloud with claim token)");
        wifi_start();  /* STA + SNTP baslatir (TLS cert notBefore/notAfter icin saat gerek) */
        /* WiFi baglanmazsa (cred bozuk) mevcut 60sn watchdog yerinde captive'e dusurur
           (musteri WiFi'yi duzeltir -> reboot -> tekrar bu dal). Baglanirsa guard kendini siler. */
        xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 4, NULL);
        /* Provision'i AYRI 10240-stack task'ta calistir. TLS handshake (mbedtls + crt_bundle cert
           dogrulama) ~8KB stack ister; main task yalniz 3584B (CONFIG_ESP_MAIN_TASK_STACK_SIZE) ->
           gs_provision_run'u BURADA dogrudan cagirmak "stack overflow in task main" + reboot dongusu
           yapiyordu (saha bulgusu). ota_task da bu yuzden ayri 8192-stack task. provision_task GOT_IP
           + SNTP bekler, gs_provision_run eder, basarida esp_restart eder. */
        xTaskCreatePinnedToCore(provision_task, "provision", 10240, NULL, 5, NULL, 0);
        /* Hizli blink: gecici durum — provision_task cloud'dan kimlik alip reboot edecek.
           Kurulum blink'inden ayri, cunku burada musterinin YAPACAGI bir sey yok (beklesin).
           Uzun surerse destek "hizli mi yavas mi?" diye sorup asamayi ayirt edebilir. */
        led_wait_forever(LED_WAIT_PROV);
    }
    if (!gs_identity_wifi_ready(&g_id)) {
        ESP_LOGW(TAG, "No WiFi - starting captive portal + BLE setup (customer enters WiFi)");
        /* Iki kurulum yolu YAN YANA: captive (SoftAP "GeoShake-XXXX" + form, web) + BLE prov
           (mobil app, GST1-XXXXX). Ikisi de gs_identity_save_wifi yazar -> esp_restart ->
           tam kimlikle normal boot. BLE fail olursa (bellek vb.) yalniz captive calisir. */
        gs_captive_start();
        gs_bleprov_start(&g_id);
        /* wifi-reset sonrasi kurulum bekleme (MQTT var, WiFi yok). Musteri acisindan
           :1077 ile AYNI sey ("WiFi gir") -> deseni de AYNI olmali; farkli desen burada
           bilgi degil kafa karisikligi uretirdi. */
        led_wait_forever(LED_WAIT_SETUP);
    }

    /* ---- provisioned (kimlik tam): mevcut boot akisi AYNEN — REGRESYONSUZ ---- */
    ESP_LOGI(TAG, "GeoShake %s (%s) starting", g_id.device_id, FW_VERSION);
    /* 3 golden HEPSI gecerse cihaz saglikli; biri bile FAIL ise led_task panik strobe yakar.
       & (degil &&): kisa-devre OLMASIN — 3 check de calissin ve boot'ta loglasin. */
    s_golden_ok = golden_check()         /* C ozellik hesabi shared (TS) ile uyumlu mu? */
                & golden_frame_check()   /* C frame encoder Lab .gsd formatiyla uyumlu mu? */
                & golden_stalta_check(); /* C STA/LTA shared (TS) ile uyumlu mu? (Faz 2b) */
    /* MQTT topic'lerini kimlikten kur. g_id yukarida (N1-T2) gs_identity_load ile doldu
       ve iki ready-check'ten (MQTT + WiFi) gecti (provisioned). sample_task (sensors_init) publish'inden
       ve mqtt subscribe'tan ONCE olmali ki tum task'lar dolu topic gorsun. */
    gs_topics_init();
    gs_cal_load(s_cal_gain);   /* CAL-T3: per-sensör gain NVS "geoshake_cal"den (yoksa 1.0). sample_task (sensors_init) ONCE. */
    sensors_init();
    wifi_start();
    xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 4, NULL);  /* N2d: 60sn WiFi-fallback watchdog */
    mqtt_start();
    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_task, "mqtt", 4096, NULL, 6, &s_mqtt_task);
    led_init();   /* durum LED'i (GPIO39, LEDC PWM) */
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 3, NULL, 0);

    /* OTA sonrasi ilk boot ise (pending-verify) self-test ile dogrula/rollback */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "first boot after OTA - starting self-test");
        xTaskCreate(selftest_task, "selftest", 4096, NULL, 4, NULL);
    }
    ESP_LOGI(TAG, "running partition: %s", run->label);
}
