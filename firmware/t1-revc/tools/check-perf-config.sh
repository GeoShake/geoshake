#!/usr/bin/env bash
# T1 performans paketi koruma betigi (2026-08-11).
#
# NEDEN VAR: sdkconfig gitignore'da ve sdkconfig.defaults'a yazmak VAR OLAN
# sdkconfig'i EZMEZ. Yani "ekledim" demek yetmiyor — URETILEN dosyada da
# olmali. Bu betik ikisini de dogrular; sessiz basarisizligi imkansiz kilar.
#
# Kullanim: bash firmware/tools/check-perf-config.sh
set -uo pipefail
cd "$(dirname "$0")/.."
FAIL=0
chk() { # dosya  desen  aciklama
  if grep -qE "$2" "$1" 2>/dev/null; then echo "  OK   $3"
  else echo "  EKSIK $3   ($1 icinde '$2' YOK)"; FAIL=1; fi
}
echo "-- sdkconfig.defaults (commit'li kaynak) --"
chk sdkconfig.defaults '^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y'    "CPU 240 MHz"
chk sdkconfig.defaults '^CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=48' "dinamik RX tampon 48"
# NOT (2026-09-05): asagidaki satir sdkconfig.defaults'ta DURUYOR ama ETKISIZ.
# CONFIG_ESP_WIFI_TX_BUFFER_TYPE=0 (STATIK) secili oldugu icin Kconfig dinamik TX
# secenegini hic gostermiyor; deger uretilen sdkconfig'e GECMIYOR. 11 Agustos
# performans paketinden beri uygulandigi SANILIYORDU. Yururlukteki ayar
# STATIK TX tampon 16'dir. Dinamige gecmek ayri bir karardir (bellek profili degisir),
# bu yuzden burada YALNIZ yururlukteki deger dogrulanir — betik gercegi olcmelidir.
chk sdkconfig.defaults '^CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=48' "dinamik TX tampon 48 (defaults'ta yazili — ETKISIZ, asagiya bak)"
chk sdkconfig.defaults '^CONFIG_LWIP_TCP_WND_DEFAULT=11520'        "TCP penceresi 11520"
chk sdkconfig.defaults '^CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520'    "TCP gonderim 11520"

echo "-- sdkconfig (URETILEN — asil kanit) --"
if [ -f sdkconfig ]; then
  chk sdkconfig '^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240'             "CPU 240 uretilene islendi"
  chk sdkconfig '^CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=48'        "RX tampon uretilene islendi"
  chk sdkconfig '^CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16'         "TX tampon YURURLUKTE (statik 16)"
  chk sdkconfig '^CONFIG_LWIP_TCP_WND_DEFAULT=11520'               "TCP penceresi uretilene islendi"
  chk sdkconfig '^CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520'           "TCP gonderim uretilene islendi"
else
  echo "  UYARI sdkconfig yok (henuz reconfigure edilmemis)"
fi

# FW_VERSION kontrolu (2026-08-11 spec incelemesi ekletti): surum degistirilip
# COMMIT EDILMEDEN binary uretilmisti. Ayni gun ucuncu kez yasanan hata sinifi —
# sahadaki cihaz git'te var olmayan bir surum bildirir. Betik artik yakaliyor.
echo "-- surum --"
# NIYET (2026-08-11): "surum degistirilip COMMIT EDILMEDEN binary uretilmesin" —
# sahadaki cihaz git'te var olmayan bir surum bildirmesin diye.
# ESKI UYGULAMA YANLISTI: surum SABIT yazilmisti ('blep-0.9.8'), bu yuzden her
# surum yukseltmesinde YANLIS "EKSIK" veriyordu ve on bir surumdur oyleydi.
# Boyle bir kapi okunmaz hale gelir. Artik niyetin kendisi olculuyor: config.h'deki
# FW_VERSION satiri calisma agacinda commit'lenmemis mi?
if git -C . diff --quiet -- main/config.h 2>/dev/null; then
  V=$(grep -oE '"blep-[0-9.]+"' main/config.h | head -1)
  echo "  OK   FW_VERSION commit'li ($V)"
else
  V=$(grep -oE '"blep-[0-9.]+"' main/config.h | head -1)
  echo "  EKSIK FW_VERSION commit'lenmemis ($V) — binary git'te olmayan surum bildirir"
  FAIL=1
fi

echo "-- main.c (calisma ani) --"
chk main/main.c 'esp_wifi_set_ps\(WIFI_PS_NONE\)'                  "guc tasarrufu KAPALI"
chk main/main.c 'esp_wifi_set_max_tx_power\(80\)'                  "TX gucu 80 (20 dBm, tavan)"
chk main/main.c 'esp_wifi_set_bandwidth\(WIFI_IF_STA, WIFI_BW_HT20\)' "bant genisligi HT20"
chk main/main.c 'esp_bt_controller_mem_release'                    "BT bellegi birakiliyor"

echo
[ $FAIL -eq 0 ] && { echo "TUMU GECTI"; exit 0; } || { echo "EKSIK VAR"; exit 1; }
