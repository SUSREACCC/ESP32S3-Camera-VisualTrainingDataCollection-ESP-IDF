idf_component_register(SRCS "src/capture_recorder.c" "src/gseq_writer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sd_index sd_files_driver esp_timer)
