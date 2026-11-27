#include "home_presenter.h"
#include "ui_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_ms;
enum { FRAME_MS = 33, MOTION_FRAMES = 48 };
static uint16_t framebuffer[UI_SIZE * UI_SIZE], draw_buffer[UI_SIZE * UI_SIZE], draw_buffer_b[UI_SIZE * UI_SIZE];
static unsigned flushed_pixels, clicks;
static home_action_t clicked_action;
static lv_area_t dirty;
static const char *phase_names[] = {
    "boot", "setup", "joining", "platform_connecting", "platform_retrying", "activate",
    "bind_agent", "preparing", "ready", "opening", "in_call", "reconnecting",
    "ending", "error", "retry", "no_session"
};
_Static_assert(sizeof(phase_names) / sizeof(phase_names[0]) == SUGARAT_VOICE_NO_SESSION + 1,
               "Render every device phase");

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(3); }
}
static uint32_t tick(void) { return now_ms; }
static void clicked(home_action_t action, void *context)
{
    (void)context; ++clicks; clicked_action = action;
}
static void reset_stats(void)
{
    flushed_pixels = 0; dirty = (lv_area_t){UI_SIZE, UI_SIZE, -1, -1};
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    if (lv_display_flush_is_last(display)) {
        /* Like the port: DIRECT preserves the logical frame, and each completed
         * group of changed rectangles results in one full physical transfer. */
        memcpy(framebuffer, pixels, sizeof(framebuffer));
        flushed_pixels += UI_SIZE * UI_SIZE;
        dirty = (lv_area_t){0, 0, UI_SIZE - 1, UI_SIZE - 1};
    }
    lv_display_flush_ready(display);
}
static void advance(unsigned elapsed)
{
    now_ms += elapsed;
    lv_refr_now(NULL); /* Advances real LVGL animation and renders it once. */
}
static void save(const char *folder, const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    FILE *f = fopen(path, "wb"); require(f != NULL, "Open native screenshot");
    fprintf(f, "P6\n%d %d\n255\n", UI_SIZE, UI_SIZE);
    for (int i = 0; i < UI_SIZE * UI_SIZE; ++i) {
        const uint16_t p = framebuffer[i];
        const unsigned char rgb[] = {(unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63), (unsigned char)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    fclose(f);
}
static void sample(FILE *metrics, const char *name, unsigned frame)
{
    fprintf(metrics, "%s,%u,%u,%.3f,%d,%d,%d,%d\n", name, frame, flushed_pixels,
            flushed_pixels * 100.0 / (UI_SIZE * UI_SIZE), (int)dirty.x1, (int)dirty.y1,
            (int)dirty.x2, (int)dirty.y2);
}
static screen_state_t mock_state(unsigned phase)
{
    screen_state_t state = {.touch_ready = true, .wifi_connected = phase > SUGARAT_VOICE_JOINING};
    state.voice.phase = (sugarat_voice_phase_t)phase;
    state.voice.agent_id = phase == SUGARAT_VOICE_BIND_AGENT ? 0 : 42;
    state.voice.can_end = phase == SUGARAT_VOICE_OPENING || phase == SUGARAT_VOICE_IN_CALL ||
                          phase == SUGARAT_VOICE_RECONNECTING;
    state.voice.level = 55;
    memcpy(state.voice.activation_code, "582041", 7);
    return state;
}
static void check_action(home_screen_t *screen, screen_state_t state)
{
    home_snapshot_t snap = home_presenter_snapshot(&state, 0);
    require(snap.title && snap.hint && snap.action_label, "Every phase has UI copy");
    require(!snap.battery_known, "Do not invent a battery reading");
    home_screen_update(screen, &snap);
    unsigned before = clicks;
    lv_obj_send_event(home_screen_action_button(screen), LV_EVENT_CLICKED, NULL);
    require(clicks == before + (snap.action_enabled ? 1 : 0), "Only enabled actions dispatch");
    if (snap.action_enabled) require(clicked_action == snap.action, "Dispatch mapped action");
    if (snap.action == HOME_START_CALL && snap.action_enabled)
        require(state.voice.phase == SUGARAT_VOICE_READY, "Start only in READY");
    if (snap.action == HOME_END_CALL && snap.action_enabled)
        require(state.voice.can_end, "End only when existing interface permits it");
    state.touch_ready = false;
    snap = home_presenter_snapshot(&state, 0);
    require(!snap.action_enabled, "Missing touchscreen disables the control");
    home_screen_update(screen, &snap); before = clicks;
    lv_obj_send_event(home_screen_action_button(screen), LV_EVENT_CLICKED, NULL);
    require(clicks == before, "Forced disabled click cannot dispatch");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(UI_SIZE, UI_SIZE);
    lv_display_set_theme(display, NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, draw_buffer_b, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, UI_BG, 0); lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    home_screen_t *screen = home_screen_create(root, clicked, NULL);
    require(screen != NULL, "Create A screen");
    char path[1024], name[128];
    snprintf(path, sizeof(path), "%s/device-refresh.csv", argv[1]);
    FILE *metrics = fopen(path, "wb"); require(metrics != NULL, "Open metrics");
    fprintf(metrics, "state,frame,flushed_pixels,screen_percent,x1,y1,x2,y2\n");
    snprintf(path, sizeof(path), "%s/device-states.csv", argv[1]);
    FILE *states = fopen(path, "wb"); require(states != NULL, "Open state manifest");
    fprintf(states, "file,phase,title,hint,button,enabled,visual\n");
    for (unsigned phase = 0; phase <= SUGARAT_VOICE_NO_SESSION; ++phase) {
        screen_state_t state = mock_state(phase);
        check_action(screen, state);
        home_snapshot_t snap = home_presenter_snapshot(&state, 0);
        home_screen_update(screen, &snap); advance(200);
        snprintf(name, sizeof(name), "device_%02u_%s", phase, phase_names[phase]);
        fprintf(states, "%s,%s,%s,%s,%s,%d,%d\n", name, phase_names[phase], snap.title,
                snap.hint, snap.action_label, snap.action_enabled, snap.state);
        for (unsigned frame = 0; frame < MOTION_FRAMES; ++frame) {
            reset_stats();
            if (phase == SUGARAT_VOICE_IN_CALL) state.voice.level = (uint8_t)(20 + frame * 7 % 70);
            snap = home_presenter_snapshot(&state, 0); home_screen_update(screen, &snap); advance(FRAME_MS);
            sample(metrics, phase_names[phase], frame);
            if (phase == SUGARAT_VOICE_IN_CALL) {
                char frame_name[128]; snprintf(frame_name, sizeof(frame_name), "device_listening_%02u", frame);
                save(argv[1], frame_name);
            }
            if (frame == 5) save(argv[1], name);
        }
    }
    fclose(states);
    screen_state_t call = mock_state(SUGARAT_VOICE_IN_CALL);
    home_snapshot_t listening = home_presenter_snapshot(&call, 0);
    home_snapshot_t speaking = home_presenter_snapshot(&call, 40);
    require(!strcmp(listening.title, "Listening") && !strcmp(speaking.title, listening.title) &&
            !strcmp(speaking.hint, listening.hint) && !strcmp(speaking.action_label, listening.action_label) &&
            speaking.action == listening.action && speaking.action_enabled == listening.action_enabled,
            "Playback changes central visual only, with Listening and mic indication preserved");
    home_screen_update(screen, &listening); advance(200); save(argv[1], "device_listening");
    reset_stats(); home_screen_update(screen, &speaking); advance(FRAME_MS);
    sample(metrics, "listening_to_speaking", 0);
    require(dirty.x1 == 0 && dirty.y1 == 0 && dirty.x2 == 465 && dirty.y2 == 465,
            "Full-frame path submits one complete frame");
    for (unsigned i = 0; i < MOTION_FRAMES; ++i) {
        reset_stats(); advance(FRAME_MS); sample(metrics, "speaking", i);
        snprintf(name, sizeof(name), "device_speaking_%02u", i); save(argv[1], name);
        if (i == 5) save(argv[1], "device_speaking");
    }
    home_screen_set_visible(screen, false); advance(200); reset_stats(); advance(800);
    require(flushed_pixels == 0, "Home motion stops while another page is open");
    home_screen_set_visible(screen, true);
    call.voice.phase = SUGARAT_VOICE_READY; call.voice.can_end = false;
    home_snapshot_t idle = home_presenter_snapshot(&call, 0); home_screen_update(screen, &idle); advance(200);
    lv_obj_add_state(home_screen_action_button(screen), LV_STATE_PRESSED); advance(200);
    save(argv[1], "device_button_pressed");
    idle.action_enabled = false; home_screen_update(screen, &idle);
    lv_obj_remove_state(home_screen_action_button(screen), LV_STATE_PRESSED); advance(200);
    save(argv[1], "device_button_disabled");
    screen_state_t setup = mock_state(SUGARAT_VOICE_SETUP); setup.wifi_setup_active = true;
    require(!home_presenter_snapshot(&setup, 0).action_enabled, "Do not restart an active setup hotspot");
    screen_state_t bad = mock_state(999);
    require(!strcmp(home_presenter_snapshot(&bad, 0).title, "Voice error"), "Unknown phase has a safe presentation");
    fclose(metrics); home_screen_destroy(screen); lv_display_delete(display);
    puts("PASS: 16 device phases, action guards, Listening during playback, full-frame submission and hidden-page pause");
    return 0;
}
