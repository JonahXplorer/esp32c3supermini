idf_component_register(SRCS "wake_word.c" "voice.c" "playback.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES agent_link esp_event esp_timer esp_wifi sugarat_board)
