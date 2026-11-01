idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES spi_flash
                                  camera_driver sd_files_driver sd_index
                                  capture_recorder cfg_store wifi_sta web_ui
                                  esp_wifi nvs_flash esp_netif esp_event esp_timer json
                    INCLUDE_DIRS ".")
