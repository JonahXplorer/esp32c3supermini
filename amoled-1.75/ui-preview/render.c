#include "home_presenter.h"
#include "ui_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_ms;
enum { FRAME_MS = 33, MOTION_FRAMES = 48 };
static uint16_t framebuffer[UI_SIZE * UI_SIZE];
#if defined(SUGARAT_NATIVE_AMOLED_UI)
static uint16_t draw_buffer[UI_SIZE * UI_SIZE], draw_buffer_b[UI_SIZE * UI_SIZE];
#else
static uint16_t draw_buffer[UI_SIZE * 12];
#endif
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
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    (void)area;
    if (lv_display_flush_is_last(display)) {
        /* Like the port: DIRECT preserves the logical frame, and each completed
         * group of changed rectangles results in one full physical transfer. */
        memcpy(framebuffer, pixels, sizeof(framebuffer));
        flushed_pixels += UI_SIZE * UI_SIZE;
        dirty = (lv_area_t){0, 0, UI_SIZE - 1, UI_SIZE - 1};
    }
#else
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    const size_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    for (int y = 0; y < height; ++y) {
        memcpy(framebuffer + (area->y1 + y) * UI_SIZE + area->x1,
               pixels + y * stride, width * sizeof(*framebuffer));
    }
    flushed_pixels += width * height;
    if (dirty.x1 > area->x1) dirty.x1 = area->x1;
    if (dirty.y1 > area->y1) dirty.y1 = area->y1;
    if (dirty.x2 < area->x2) dirty.x2 = area->x2;
    if (dirty.y2 < area->y2) dirty.y2 = area->y2;
#endif
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

#if !defined(SUGARAT_NATIVE_AMOLED_UI)
static void align_window(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->x2 |= 1;
}

static lv_point_t pointer;
static bool pressed;

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = pointer;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void tap(lv_indev_t *input, int x, int y)
{
    pointer = (lv_point_t){x, y};
    pressed = true;
    lv_indev_read(input);
    pressed = false;
    lv_indev_read(input);
    advance(210);
}

static bool visible_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(obj, &lv_label_class) && strcmp(lv_label_get_text(obj), text) == 0) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        if (visible_label(lv_obj_get_child(obj, i), text)) return true;
    }
    return false;
}

static void render_settings(const char *folder)
{
    require(screen_init(true) == ESP_OK, "Create production Sugarat pages");
    screen_state_t state = mock_state(SUGARAT_VOICE_READY);
    state.settings = (device_settings_t){.volume = 65, .brightness = 74};
    state.manual = (manual_control_view_t){.online = true, .speed = 40, .angle_deg = 90,
        .angle_limit_deg = 145, .receiver_channel = 6};
    strcpy(state.voice.conversation_id, "main");
    screen_update(&state);
    advance(210);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_pointer);

    tap(input, 180, 40);
    lv_obj_t *root = lv_screen_active();
    require(visible_label(root, "Control center"), "Home header opens Control center");
    tap(input, 180, 166);
    require(visible_label(root, "Brightness") && visible_label(root, "Manual control"), "Open Settings");
    save(folder, "device_settings");

    static const char *const pages[] = {"Volume", "Brightness", "Sessions", "Manual control", "Network"};
    static const char *const names[] = {"device_volume", "device_brightness", "device_sessions", "device_manual", "device_network"};
    for (int i = 0; i < 5; ++i) {
        tap(input, 180, UI_MENU_START_Y + i * UI_MENU_STEP_Y + UI_MENU_ROW_H / 2);
        require(visible_label(root, pages[i]), "Navigate to a Sugarat settings page");
        require(!visible_label(root, "Settings"), "Hide the settings list on its child pages");
        screen_update(&state);
        screen_update(&state);
        advance(210);
        save(folder, names[i]);
        if (i == 3) {
            require(screen_manual_is_open(), "Manual control remains connected to the manual page");
            tap(input, 244, 132);
            screen_action_t action = screen_poll();
            require(action.kind == SCREEN_SET_TARGET && action.value == 1, "Position B produces the original action");
        }
        if (i == 4) {
            tap(input, 242, 246);
            require(visible_label(root, "Use China server?"), "Region change requires confirmation");
            tap(input, 235, 220);
            screen_action_t action = screen_poll();
            require(action.kind == SCREEN_SET_VOICE_SERVER && action.value == 1, "Confirm China emits the original action");
        }
        tap(input, 90, 66);
    }
    lv_indev_delete(input);
    puts("PASS: Sugarat Settings touch navigation, Position B, and network confirmation");
}
#endif

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display = lv_display_create(UI_SIZE, UI_SIZE);
    lv_display_set_theme(display, NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    lv_display_set_buffers(display, draw_buffer, draw_buffer_b, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_DIRECT);
#else
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display, align_window, LV_EVENT_INVALIDATE_AREA, NULL);
#endif
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
    for (unsigned phase = SUGARAT_VOICE_ACTIVATE; phase <= SUGARAT_VOICE_BIND_AGENT; ++phase) {
        screen_state_t onboarding = mock_state(phase);
        require(!strcmp(home_presenter_snapshot(&onboarding, 0).hint, "bot.sugarat.ai"),
                "Default onboarding points to the overseas server");
        onboarding.settings.voice_china = true;
        require(!strcmp(home_presenter_snapshot(&onboarding, 0).hint, "bot.deotaland.cn"),
                "China onboarding points to the China server");
    }
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
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    require(dirty.x1 == 0 && dirty.y1 == 0 && dirty.x2 == UI_SIZE - 1 && dirty.y2 == UI_SIZE - 1,
            "Full-frame path submits one complete frame");
#else
    /* LVGL refresh includes an edge pixel; the real flush also rounds to even columns. */
    require(dirty.x1 >= UI_ORBIT_X - 2 && dirty.y1 >= UI_ORBIT_Y - 1 &&
            dirty.x2 <= UI_ORBIT_X + UI_ORBIT_SIZE + 1 && dirty.y2 <= UI_ORBIT_Y + UI_ORBIT_SIZE,
            "Playback invalidates only the central orbit");
#endif
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
    fclose(metrics);
    home_screen_destroy(screen);
#if !defined(SUGARAT_NATIVE_AMOLED_UI)
    render_settings(argv[1]);
#endif
    lv_display_delete(display);
    puts("PASS: 16 device phases, regional onboarding, action guards, Listening during playback, refresh bounds and hidden-page pause");
    return 0;
}
