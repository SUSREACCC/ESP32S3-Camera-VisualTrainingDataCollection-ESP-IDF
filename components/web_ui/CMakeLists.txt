idf_component_register(SRCS "src/web_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server json capture_recorder sd_index wifi_sta cfg_store
                       EMBED_TXTFILES "web/index.html")
