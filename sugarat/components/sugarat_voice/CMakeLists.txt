idf_component_register(
    SRCS "wake_word.c" "voice.c" "playback.c" "servo_tool.c" "servo_voice_command.c"
         "turn_timing.c" "voice_trace.c" "conversations.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES agent_link cjson esp_event esp_http_client esp_timer esp_wifi sugarat_board
)
