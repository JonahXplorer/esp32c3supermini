idf_component_register(
    SRCS "wake_word.c" "voice.c" "playback.c" "servo_tool.c" "servo_voice_command.c"
         "turn_timing.c" "voice_trace.c" "conversations.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES agent_link cjson esp_event esp_http_client esp_timer esp_wifi sugarat_board
)

option(SUGARAT_LOCAL_SERVO_COMMANDS "Allow local servo commands from final voice transcripts" ON)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SUGARAT_LOCAL_SERVO_COMMANDS=$<BOOL:${SUGARAT_LOCAL_SERVO_COMMANDS}>)
