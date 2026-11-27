#include "home_presenter.h"
#include <string.h>

typedef struct {
    home_visual_state_t visual;
    const char *title;
    const char *hint;
    const char *button;
    home_action_t action;
} phase_copy_t;

/* One entry per existing device phase. These are display mappings, not a new
 * business state machine. Connection preparation is never called AI thinking. */
static const phase_copy_t phases[] = {
    [SUGARAT_VOICE_BOOT] = {HOME_BUSY, "Starting", "Preparing device", "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_SETUP] = {HOME_OFFLINE, "Wi-Fi setup", "Join Sugarat-Jarvis on your phone", "Wi-Fi", HOME_SETUP_WIFI},
    [SUGARAT_VOICE_JOINING] = {HOME_BUSY, "Joining Wi-Fi", "Connecting to your network", "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_PLATFORM_CONNECTING] = {HOME_BUSY, "Connecting", "Contacting the platform", "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_PLATFORM_RETRYING] = {HOME_OFFLINE, "Offline", "Platform unavailable / retrying", "Settings", HOME_OPEN_SETTINGS},
    [SUGARAT_VOICE_ACTIVATE] = {HOME_IDLE, "Activate", NULL, "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_BIND_AGENT] = {HOME_IDLE, "Choose agent", NULL, "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_PREPARING] = {HOME_BUSY, "Preparing", "Connecting to the voice server", "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_READY] = {HOME_IDLE, "Ready", "Say Jarvis or tap Start call", "Start call", HOME_START_CALL},
    [SUGARAT_VOICE_OPENING] = {HOME_BUSY, "Listening", "Opening voice connection", "End call", HOME_END_CALL},
    [SUGARAT_VOICE_IN_CALL] = {HOME_LISTENING, "Listening", "Microphone is on", "End call", HOME_END_CALL},
    [SUGARAT_VOICE_RECONNECTING] = {HOME_BUSY, "Reconnecting", "Restoring the voice connection", "Settings", HOME_OPEN_SETTINGS},
    [SUGARAT_VOICE_ENDING] = {HOME_BUSY, "Ending call", "Closing the voice stream", "Waiting", HOME_NO_ACTION},
    [SUGARAT_VOICE_ERROR] = {HOME_OFFLINE, "Voice error", "Restart device to try again", "Settings", HOME_OPEN_SETTINGS},
    [SUGARAT_VOICE_RETRY] = {HOME_BUSY, "Interrupted", "Restoring voice connection", "Settings", HOME_OPEN_SETTINGS},
    [SUGARAT_VOICE_NO_SESSION] = {HOME_IDLE, "No session", "Create a session to continue", "Sessions", HOME_OPEN_SESSIONS},
};
_Static_assert(sizeof(phases) / sizeof(phases[0]) == SUGARAT_VOICE_NO_SESSION + 1,
               "Every device phase needs a home presentation");

static bool action_enabled(const screen_state_t *state, home_action_t action)
{
    if (!state->touch_ready) return false;
    const bool call = state->voice.can_end || state->voice.phase == SUGARAT_VOICE_OPENING;
    switch (action) {
    case HOME_START_CALL: return state->voice.phase == SUGARAT_VOICE_READY && !call;
    case HOME_END_CALL: return state->voice.can_end;
    case HOME_SETUP_WIFI: return !call && !state->wifi_setup_active;
    case HOME_OPEN_SESSIONS: return !call && state->voice.agent_id > 0;
    case HOME_OPEN_SETTINGS: return true;
    default: return false;
    }
}

home_snapshot_t home_presenter_snapshot(const screen_state_t *state, unsigned playback_level)
{
    unsigned phase = (unsigned)state->voice.phase;
    if (phase >= sizeof(phases) / sizeof(phases[0])) phase = SUGARAT_VOICE_ERROR;
    const phase_copy_t *copy = &phases[phase];
    home_snapshot_t result = {
        .state = copy->visual, .title = copy->title, .hint = copy->hint,
        .action_label = copy->button, .action = copy->action,
        .wifi_connected = state->wifi_connected,
        .battery_known = false, /* No battery telemetry is exposed by this application. */
        .level = (state->voice.level > 100 ? 100 : state->voice.level) / 5,
    };
    if (phase == SUGARAT_VOICE_IN_CALL) {
        /* Speaking changes central drawing only. Copy and controls stay identical;
         * the original mic waveform level continues to come from the AEC path. */
        result.state = playback_level ? HOME_SPEAKING : HOME_LISTENING;
    } else if ((phase == SUGARAT_VOICE_RECONNECTING || phase == SUGARAT_VOICE_ENDING) && state->voice.can_end) {
        if (phase == SUGARAT_VOICE_RECONNECTING) result.title = "Listening";
        result.action_label = "End call";
        result.action = HOME_END_CALL;
    } else if (phase == SUGARAT_VOICE_ACTIVATE) {
        memcpy(result.activation_code, state->voice.activation_code, sizeof(result.activation_code) - 1);
    }
    if (phase == SUGARAT_VOICE_ACTIVATE || phase == SUGARAT_VOICE_BIND_AGENT) {
        result.hint = sugarat_voice_server_host(state->settings.voice_china);
    }
    result.action_enabled = action_enabled(state, result.action);
    return result;
}
