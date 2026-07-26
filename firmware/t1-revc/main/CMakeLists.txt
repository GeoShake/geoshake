idf_component_register(SRCS "main.c" "identity.c" "captive.c" "cal.c" "provision.c" "bleprov.c"
  INCLUDE_DIRS "."
  REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_timer driver mqtt json
           esp_https_ota app_update esp_http_server dns_server
           esp_http_client esp-tls mbedtls wifi_provisioning)
