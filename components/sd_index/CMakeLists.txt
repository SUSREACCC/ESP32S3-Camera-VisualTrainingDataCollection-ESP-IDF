idf_component_register(SRCS "src/sd_index.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sd_files_driver esp_timer cfg_store)
