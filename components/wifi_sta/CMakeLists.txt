idf_component_register(SRCS "src/wifi_sta.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer)
