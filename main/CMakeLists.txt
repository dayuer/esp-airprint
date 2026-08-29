idf_component_register(SRCS "main.c" "usb_printer.c" "ipp_server.c" "netlog.c" "lcd_ui.c" "font_cn_16.c" "provision.c" "joblog.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb driver esp_timer esp_wifi nvs_flash esp_netif esp_http_server)
