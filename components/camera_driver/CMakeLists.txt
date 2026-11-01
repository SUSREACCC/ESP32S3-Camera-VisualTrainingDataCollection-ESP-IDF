idf_component_register(SRCS "camera_driver.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp32-camera
                    PRIV_REQUIRES esp_timer)
