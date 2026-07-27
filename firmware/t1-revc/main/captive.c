// GeoShake T1 firmware
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See firmware/LICENSE.

#include "captive.h"
#include <string.h>
#include <stdlib.h>   /* strtol */
#include <ctype.h>    /* isxdigit */
#include <stdio.h>    /* snprintf */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"      /* esp_restart, esp_get_free_heap_size */
#include "esp_mac.h"         /* esp_read_mac, ESP_MAC_WIFI_SOFTAP */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "dns_server.h"        /* N2e: DNS hijack (vendored, T1) — start_dns_server / DNS_SERVER_CONFIG_SINGLE */
#include "identity.h"

#define GS_BODY_MAX 1024   /* POST /save govde tamponu ust siniri (content-len guard) */

static const char *TAG = "captive";

static httpd_handle_t s_server = NULL;
static dns_server_handle_t s_dns = NULL;   /* N2e: DNS hijack handle (start_webserver'da kurulur, restart'a kadar yasar) */

/* ---- Sayfa sablonlari ----
   Tek-tirnakli HTML oznitelikleri (C string'inde \" kacisini elemek icin); charset utf-8.
   Kullanici girdisi sayfalara ASLA yazilmaz (XSS yok); hata mesajlari sabit/derleyici-kontrollu. */
static const char FORM_HEAD[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>GeoShake Setup</title><style>"
"body{font-family:system-ui,sans-serif;max-width:380px;margin:24px auto;padding:0 16px;color:#111}"
"h1{font-size:1.3rem}label{display:block;margin-top:12px;font-weight:600}"
"input{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;font-size:1rem}"
"button{margin-top:18px;width:100%;padding:11px;font-size:1rem;cursor:pointer}"
".note{color:#555;font-size:.85rem}.err{color:#c00;font-weight:600;margin:8px 0}"
"</style></head><body>"
"<h1>GeoShake Wi-Fi Setup</h1>"
"<p class='note'>Connect your device to Wi-Fi: enter your network name (SSID) and password.</p>";

static const char FORM_TAIL[] =
"<form method='POST' action='/save'>"
"<label>Wi-Fi Network (SSID)<input name='wifi_ssid' required></label>"
"<label>Wi-Fi Password<input name='wifi_pass' type='password'></label>"
"<button type='submit'>Save &amp; Connect</button>"
"</form></body></html>";

static const char SUCCESS_HTML[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>GeoShake</title></head>"
"<body style='font-family:system-ui,sans-serif;max-width:380px;margin:40px auto;padding:0 16px'>"
"<h1>Saved</h1>"
"<p>Your device is restarting and will connect to your Wi-Fi. You can close this page.</p>"
"</body></html>";

/* ---- yardimcilar ---- */

/* application/x-www-form-urlencoded decode (yerinde): %XX hex + '+' -> bosluk.
   Bozuk %XX (hex degil) literal '%' olarak gecer. */
static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hx[3] = { s[1], s[2], '\0' };
            *d++ = (char)strtol(hx, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

/* urlencoded govdeden 'key' degerini cikar -> out (URL-decode edilmis, out_sz'e clamp).
   Bulunamazsa out[0]='\0'. Eslesme alan sinirinda (basta veya '&' sonrasi) yapilir. */
static void form_get(const char *body, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *amp = strchr(v, '&');
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

/* Formu gonder; err verilirse (bos degilse) uste hata satiri eklenir. */
static esp_err_t send_form(httpd_req_t *req, const char *err) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, FORM_HEAD);
    if (err && err[0]) {
        httpd_resp_sendstr_chunk(req, "<p class='err'>");
        httpd_resp_sendstr_chunk(req, err);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req, FORM_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);   /* yaniti sonlandir */
    return ESP_OK;
}

/* ---- HTTP handler'lari ---- */

static esp_err_t get_root(httpd_req_t *req) {
    return send_form(req, NULL);
}

static esp_err_t post_save(httpd_req_t *req) {
    int total = req->content_len;
    if (total <= 0 || total > GS_BODY_MAX) {
        ESP_LOGW(TAG, "POST /save invalid content_len=%d", total);
        return send_form(req, "Gecersiz istek (govde boyutu).");
    }

    char body[GS_BODY_MAX + 1];
    int off = 0;
    int timeouts = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) {
            /* Sinirli yeniden dene: stall eden client (Content-Length verip govde gondermeyen)
               tek-thread httpd task'ini suresiz kilitlemesin -> ~5 ardisik timeout sonra birak. */
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            ESP_LOGW(TAG, "POST /save body read error/timeout (%d)", r);
            return ESP_FAIL;
        }
        timeouts = 0;
        off += r;
    }
    body[off] = '\0';

    char wifi_ssid[64], wifi_pass[96];
    form_get(body, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    form_get(body, "wifi_pass", wifi_pass, sizeof(wifi_pass));

    /* Dogrula (uzunluk siniri gs_identity_save_wifi'de YOK -> burada zorunlu upstream guard;
       asiri uzun SSID/parola NVS'e yazilirsa sonraki boot'ta sessizce yuklenemez).
       Sir DEGERLERI loglanmaz; yalniz hangi alanin neden gecersiz oldugu kullaniciya doner. */
    if (wifi_ssid[0] == '\0' || strlen(wifi_ssid) > 32)
        return send_form(req, "Wi-Fi network name is required (max 32 characters).");
    if (strlen(wifi_pass) > 63)
        return send_form(req, "Wi-Fi password is too long (max 63 characters).");

    /* SADECE WiFi anahtarlarini yaz; MQTT kimligi (admin esptool ile gomdu) korunur.
       wifi_pass bos olabilir (acik ag) -> gs_identity_save_wifi(ssid, "") gecerli. */
    esp_err_t e = gs_identity_save_wifi(wifi_ssid, wifi_pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "gs_identity_save_wifi FAIL: %s", esp_err_to_name(e));
        return send_form(req, "Could not save settings. Please try again.");
    }
    /* ssid sir degil; wifi_pass LOGLANMAZ. */
    ESP_LOGI(TAG, "WiFi saved: ssid=%s (password not logged) - rebooting", wifi_ssid);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, SUCCESS_HTML);

    vTaskDelay(pdMS_TO_TICKS(1500));   /* yanitin flush'i + tarayici render icin kisa gecikme */
    esp_restart();
    return ESP_OK;                     /* erisilmez (esp_restart geri donmez) */
}

/* ---- SoftAP ----
   Ortak: AP netif + mode(AP) + SSID config + start. esp_wifi_init CAGIRMAZ (cagiran saglar):
   boot dalinda softap_start init eder; yerinde dalda wifi_start zaten init+start etmistir.
   Gereksinim: esp_netif_init + default event loop yapilmis olmali (her iki cagiran da yapar);
   esp_wifi_init'e baglilik YOK (AP netif olusumu wifi surucusu durumuna dokunmaz). */
static void softap_bringup(void) {
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        /* AP netif yok -> DHCP yapilandirilmaz, client IP alamaz, portal erisilemez. */
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap NULL (OOM?) - rebooting");
        esp_restart();
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t mac[6] = {0};
    esp_err_t me = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);   /* efuse'tan; SSID benzersizligi icin son 2 bayt */
    if (me != ESP_OK)   /* basarisizsa mac {0} kalir -> GeoShake-0000 fallback (loglanir) */
        ESP_LOGW(TAG, "esp_read_mac FAIL: %s - falling back to SSID 0000", esp_err_to_name(me));

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "GeoShake-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* acik ag — YAGNI, yalniz provisioning ani */
    ap.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up: SSID=%s (open network) IP=192.168.4.1", (char *)ap.ap.ssid);
}

/* Boot varyanti (mevcut davranis AYNEN): wifi henuz init DEGIL -> netif + event loop + wifi_init
   sonra bringup. netif + default event loop: unprovisioned dalda wifi_start CALISMADI -> burada
   temiz init. Yine de cift-init'e karsi guard (idempotent davranis). */
static void softap_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(le);
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    softap_bringup();
}

/* ---- HTTP server ---- */

/* N2e: CPD probe'lari (iOS captive.apple.com / Android generate_204 / Win connecttest) DNS-hijack'le
   buraya gelir → bilinmeyen URI → 302 form'a. iOS icin govde ZORUNLU (sirf redirect yetmez). */
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirecting to GeoShake setup", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void) {
    /* N2e: redirect trafigi (CPD probe → 302) httpd loglarini doldurur; gurultuyu kis (kozmetik). */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;          /* post_save govde tamponu (~1.5KB) icin rahat pay */
    config.lru_purge_enable = true;
    config.max_open_sockets = 13;      /* N2e: 13 = upstream captive_portal degeri. httpd +3 internal
                                          (esp_http_server.h) → 13+3=16 LWIP(16)'yi TAM DOLDURUR (bos marj YOK);
                                          DNS(+1)/in-place MQTT(+1) LRU-purge + graceful-degrade ile tolere — marji geri alma */
    esp_err_t e = httpd_start(&s_server, &config);
    if (e != ESP_OK) {
        /* HTTP yoksa SoftAP acik ama portal yanitsiz -> cihaz unprovisionable. Yeniden dene. */
        ESP_LOGE(TAG, "httpd_start FAIL: %s - rebooting", esp_err_to_name(e));
        esp_restart();
    }
    httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = get_root,  .user_ctx = NULL };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = post_save, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_save);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler);
    /* DNS hijack: tum A sorgulari → softAP IP (192.168.4.1) → CPD probe'lari cihaza gelir. */
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (s_dns == NULL)
        ESP_LOGE(TAG, "start_dns_server FAIL - portal reachable only MANUALLY (192.168.4.1)");
    else
        ESP_LOGI(TAG, "DNS hijack active: all queries -> 192.168.4.1 (CPD auto-open)");
    ESP_LOGI(TAG, "HTTP portal ready: http://192.168.4.1/ (GET / form, POST /save)");
}

void gs_captive_start(void) {
    ESP_LOGW(TAG, "Captive portal mode - waiting for provisioning (sensors/mqtt/STA not started)");
    softap_start();
    start_webserver();
    ESP_LOGI(TAG, "Setup: join the 'GeoShake-XXXX' network; your phone opens the setup page AUTOMATICALLY "
                  "(DNS hijack + 302 aktif). Elle: http://192.168.4.1");
    ESP_LOGI(TAG, "captive heap=%lu", (unsigned long)esp_get_free_heap_size());
    /* BLEPROV: artik NON-BLOCKING — SoftAP + httpd kendi task'larinda yasar; cagiran (app_main)
       BLE prov'u da acip kendisi bekler. Basarili POST /save yine esp_restart eder. */
}

/* N2d: provisioned-path 60sn baglanamadi -> wifi ZATEN init+start (STA). Reboot YOK:
   STA'yi durdur (init korunur) -> AP+captive. Cagiran (wifi_guard_task) s_captive_fallback'i
   ONCE set etmeli ki esp_wifi_stop'un DISCONNECTED'i reconnect tetiklemesin. */
void gs_captive_start_inplace(void) {
    ESP_LOGW(TAG, "WiFi did not connect in 60 s - in-place captive (STA->AP, no reboot; creds preserved)");
    ESP_ERROR_CHECK(esp_wifi_stop());   /* STA durur; esp_wifi_init korunur */
    softap_bringup();                   /* AP netif + mode(AP) + config + start */
    start_webserver();
    ESP_LOGI(TAG, "Recovery portal: join the 'GeoShake-XXXX' network; your phone opens it automatically. Manually: http://192.168.4.1");
    ESP_LOGI(TAG, "inplace-captive heap=%lu", (unsigned long)esp_get_free_heap_size());
    /* BLEPROV: NON-BLOCKING — cagiran (wifi_guard_task) BLE prov'u acip kendisi bekler. */
}
