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

#define GS_BODY_MAX 1024   /* upper bound of the POST /save body buffer (content-len guard) */

static const char *TAG = "captive";

static httpd_handle_t s_server = NULL;
static dns_server_handle_t s_dns = NULL;   /* N2e: DNS hijack handle (set up in start_webserver, lives until restart) */

/* ---- Page templates ----
   Single-quoted HTML attributes (to avoid \" escapes inside the C string); charset utf-8.
   User input is NEVER written into the pages (no XSS); error messages are constant/compiler-checked. */
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

/* ---- helpers ---- */

/* application/x-www-form-urlencoded decode (in place): %XX hex + '+' -> space.
   A malformed %XX (not hex) passes through as a literal '%'. */
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

/* Extract the value of 'key' from the urlencoded body -> out (URL-decoded, clamped to out_sz).
   If not found, out[0]='\0'. Matching is done at a field boundary (at the start or after an '&'). */
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

/* Send the form; if err is supplied (and not empty) an error line is added on top. */
static esp_err_t send_form(httpd_req_t *req, const char *err) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, FORM_HEAD);
    if (err && err[0]) {
        httpd_resp_sendstr_chunk(req, "<p class='err'>");
        httpd_resp_sendstr_chunk(req, err);
        httpd_resp_sendstr_chunk(req, "</p>");
    }
    httpd_resp_sendstr_chunk(req, FORM_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);   /* terminate the response */
    return ESP_OK;
}

/* ---- HTTP handlers ---- */

static esp_err_t get_root(httpd_req_t *req) {
    return send_form(req, NULL);
}

static esp_err_t post_save(httpd_req_t *req) {
    int total = req->content_len;
    if (total <= 0 || total > GS_BODY_MAX) {
        ESP_LOGW(TAG, "POST /save invalid content_len=%d", total);
        return send_form(req, "Invalid form submission. Please try again.");
    }

    char body[GS_BODY_MAX + 1];
    int off = 0;
    int timeouts = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) {
            /* Bounded retry: a stalling client (one that gives a Content-Length but sends no body)
               must not lock the single-threaded httpd task forever -> give up after ~5 consecutive timeouts. */
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

    /* Validate (there is NO length limit in gs_identity_save_wifi -> this upstream guard is mandatory
       here; an over-long SSID/password written to NVS silently fails to load on the next boot).
       Secret VALUES are not logged; only which field was invalid and why goes back to the user. */
    if (wifi_ssid[0] == '\0' || strlen(wifi_ssid) > 32)
        return send_form(req, "Wi-Fi network name is required (max 32 characters).");
    if (strlen(wifi_pass) > 63)
        return send_form(req, "Wi-Fi password is too long (max 63 characters).");

    /* Write ONLY the WiFi keys; the MQTT identity (embedded by the admin with esptool) is preserved.
       wifi_pass may be empty (open network) -> gs_identity_save_wifi(ssid, "") is valid. */
    esp_err_t e = gs_identity_save_wifi(wifi_ssid, wifi_pass);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "gs_identity_save_wifi FAIL: %s", esp_err_to_name(e));
        return send_form(req, "Could not save settings. Please try again.");
    }
    /* The ssid is not a secret; wifi_pass is NOT LOGGED. */
    ESP_LOGI(TAG, "WiFi saved: ssid=%s (password not logged) - rebooting", wifi_ssid);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, SUCCESS_HTML);

    vTaskDelay(pdMS_TO_TICKS(1500));   /* short delay for the response flush + browser render */
    esp_restart();
    return ESP_OK;                     /* unreachable (esp_restart does not return) */
}

/* ---- SoftAP ----
   Shared: AP netif + mode(AP) + SSID config + start. It DOES NOT CALL esp_wifi_init (the caller
   provides it): on the boot branch softap_start does the init; on the in-place branch wifi_start has
   already done init+start. Requirement: esp_netif_init + the default event loop must have been done
   (both callers do); there is NO dependency on esp_wifi_init (creating the AP netif does not touch the wifi driver state). */
static void softap_bringup(void) {
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        /* No AP netif -> DHCP is not configured, the client cannot get an IP, the portal is unreachable. */
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap NULL (OOM?) - rebooting");
        esp_restart();
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    uint8_t mac[6] = {0};
    esp_err_t me = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);   /* from efuse; the last 2 bytes make the SSID unique */
    if (me != ESP_OK)   /* on failure mac stays {0} -> GeoShake-0000 fallback (logged) */
        ESP_LOGW(TAG, "esp_read_mac FAIL: %s - falling back to SSID 0000", esp_err_to_name(me));

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "GeoShake-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = 1;
    ap.ap.max_connection = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* open network — YAGNI, only for the provisioning moment */
    ap.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP up: SSID=%s (open network) IP=192.168.4.1", (char *)ap.ap.ssid);
}

/* Boot variant (existing behaviour UNCHANGED): wifi is NOT yet init'ed -> netif + event loop + wifi_init
   then bringup. netif + default event loop: on the unprovisioned branch wifi_start DID NOT RUN -> a clean
   init here. There is still a guard against double-init (idempotent behaviour). */
static void softap_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(le);
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    softap_bringup();
}

/* ---- HTTP server ---- */

/* N2e: CPD probes (iOS captive.apple.com / Android generate_204 / Win connecttest) arrive here via the
   DNS hijack → unknown URI → 302 to the form. A body is MANDATORY for iOS (a bare redirect is not enough). */
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirecting to GeoShake setup", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void) {
    /* N2e: the redirect traffic (CPD probe → 302) floods the httpd logs; turn the noise down (cosmetic). */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;          /* comfortable headroom for the post_save body buffer (~1.5KB) */
    config.lru_purge_enable = true;
    config.max_open_sockets = 13;      /* N2e: 13 = the upstream captive_portal value. httpd adds +3 internal
                                          (esp_http_server.h) → 13+3=16 FILLS LWIP(16) COMPLETELY (NO spare margin);
                                          DNS(+1)/in-place MQTT(+1) are tolerated via LRU-purge + graceful degradation — do not take the margin back */
    esp_err_t e = httpd_start(&s_server, &config);
    if (e != ESP_OK) {
        /* Without HTTP the SoftAP is up but the portal does not answer -> the device is unprovisionable. Retry. */
        ESP_LOGE(TAG, "httpd_start FAIL: %s - rebooting", esp_err_to_name(e));
        esp_restart();
    }
    httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = get_root,  .user_ctx = NULL };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = post_save, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_save);
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler);
    /* DNS hijack: all A queries → the softAP IP (192.168.4.1) → the CPD probes reach the device. */
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
                  "(DNS hijack + 302 active). Manual: http://192.168.4.1");
    ESP_LOGI(TAG, "captive heap=%lu", (unsigned long)esp_get_free_heap_size());
    /* BLEPROV: now NON-BLOCKING — SoftAP + httpd live in their own tasks; the caller (app_main)
       also opens BLE prov and waits itself. A successful POST /save still does an esp_restart. */
}

/* N2d/#282: the provisioned path failed to connect within the boot timeout -> wifi is ALREADY
   init+start (STA). This function itself does NOT reboot: it stops the STA (init preserved) -> AP+
   captive. The caller (wifi_guard_task) sets s_captive_fallback FIRST so the DISCONNECTED from
   esp_wifi_stop does not trigger a reconnect, and REBOOTS after CAPTIVE_REBOOT_AFTER_MS if WiFi is
   still down (power-outage self-heal). A customer save here still esp_restart's immediately. */
void gs_captive_start_inplace(void) {
    ESP_LOGW(TAG, "WiFi did not connect at boot - in-place captive (STA->AP; caller reboots if still down)");
    ESP_ERROR_CHECK(esp_wifi_stop());   /* the STA stops; esp_wifi_init is preserved */
    softap_bringup();                   /* AP netif + mode(AP) + config + start */
    start_webserver();
    ESP_LOGI(TAG, "Recovery portal: join the 'GeoShake-XXXX' network; your phone opens it automatically. Manually: http://192.168.4.1");
    ESP_LOGI(TAG, "inplace-captive heap=%lu", (unsigned long)esp_get_free_heap_size());
    /* BLEPROV: NON-BLOCKING — the caller (wifi_guard_task) opens BLE prov and waits itself. */
}
