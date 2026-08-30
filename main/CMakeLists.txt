idf_component_register(SRCS "main.c" "usb_printer.c" "cloud_client.c" "netlog.c" "lcd_ui.c" "font_cn_16.c" "provision.c" "joblog.c" "printer_profile.c" "profile_script.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb driver esp_timer esp_wifi nvs_flash esp_netif esp_http_server mqtt esp_http_client esp-tls)
