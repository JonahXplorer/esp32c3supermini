#include "home_screen.h"
#include "ui_theme.h"
#include <stdlib.h>
#include <string.h>

struct home_screen {
    lv_obj_t *root, *hero, *title, *hint, *header, *status, *battery;
    lv_obj_t *button, *button_icon, *button_label, *code;
    home_snapshot_t snapshot;
    home_action_cb_t on_action;
    void *context;
    int32_t motion;
    uint32_t motion_at;
    bool visible;
};

static lv_style_t button_base, button_pressed, button_disabled, button_active;
static lv_style_transition_dsc_t press_transition;
static bool styles_ready;

static bool active(const home_screen_t *s)
{
    return s->snapshot.state == HOME_LISTENING || s->snapshot.state == HOME_SPEAKING;
}

static home_action_t action_for(const home_screen_t *s)
{
    if (s->snapshot.action_label) return s->snapshot.action;
    if (active(s)) return HOME_END_CALL;
    return s->snapshot.state == HOME_OFFLINE ? HOME_SETUP_WIFI : HOME_START_CALL;
}

/* One 2 px rounded line family for status, microphone and action icons. */
static void line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                 lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = (lv_point_precise_t){x1, y1};
    d.p2 = (lv_point_precise_t){x2, y2};
    d.width = width; d.color = color; d.opa = opa;
    d.round_start = d.round_end = 1;
    d.raw_end = 1;
    lv_draw_line(layer, &d);
}

static void arc(lv_layer_t *layer, int x, int y, int r, int a, int b,
                lv_color_t color, int width, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center = (lv_point_t){x, y}; d.radius = (uint16_t)r;
    d.start_angle = a; d.end_angle = b;
    d.color = color; d.width = width; d.opa = opa; d.rounded = 1;
    lv_draw_arc(layer, &d);
}

static void rect(lv_layer_t *layer, int x, int y, int w, int h, int radius,
                 lv_color_t color, lv_opa_t opa, bool fill)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_color = color; d.bg_opa = fill ? opa : LV_OPA_TRANSP;
    d.border_color = color; d.border_opa = fill ? LV_OPA_TRANSP : opa;
    d.border_width = fill ? 0 : UI_STROKE;
    lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &d, &area);
}

static void mic(lv_layer_t *layer, int x, int y, lv_color_t color, lv_opa_t opa)
{
    rect(layer, x - 6, y - 13, 12, 20, 6, color, opa, false);
    arc(layer, x, y + 1, 12, 0, 180, color, UI_STROKE, opa);
    line(layer, x - 11, y - 1, x - 11, y + 1, color, UI_STROKE, opa);
    line(layer, x + 11, y - 1, x + 11, y + 1, color, UI_STROKE, opa);
    line(layer, x, y + 13, x, y + 18, color, UI_STROKE, opa);
    line(layer, x - 5, y + 18, x + 5, y + 18, color, UI_STROKE, opa);
}

static void wifi(lv_layer_t *layer, int x, int y, bool connected, lv_opa_t opa)
{
    arc(layer, x, y + 13, 17, 222, 318, UI_TEXT_DIM, UI_STROKE, opa);
    arc(layer, x, y + 13, 10, 222, 318, UI_TEXT_DIM, UI_STROKE, opa);
    rect(layer, x - 2, y + 10, 4, 4, 2, UI_TEXT_DIM, opa, true);
    if (!connected) line(layer, x - 14, y - 4, x + 13, y + 17,
                         UI_TEXT_DIM, UI_STROKE, opa);
}

static void draw_status(lv_event_t *event)
{
    home_screen_t *s = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a; lv_obj_get_coords(s->status, &a);
    wifi(layer, a.x1 + (s->snapshot.battery_known ? UI_PAD : UI_STATUS_W / 2),
         a.y1 + 4, s->snapshot.wifi_connected, LV_OPA_COVER);
    if (s->snapshot.battery_known) {
        const int x = a.x1 + 108, y = a.y1 + 5;
        rect(layer, x, y, 26, 14, 3, UI_TEXT_DIM, LV_OPA_COVER, false);
        line(layer, x + 28, y + 4, x + 28, y + 9,
             UI_TEXT_DIM, UI_STROKE, LV_OPA_COVER);
        int w = 18 * s->snapshot.battery_percent / 100;
        if (w > 0) rect(layer, x + 4, y + 4, w, 6, 1, UI_TEXT_DIM, LV_OPA_COVER, true);
    }
}

static int wave(int angle)
{
    return lv_trigo_sin(angle % 360); /* Integer sine; no floating point on device. */
}

static void draw_orbit(home_screen_t *s, lv_layer_t *layer, int x, int y)
{
    const int phase = s->motion * 360 / 1024;
    const home_visual_state_t state = s->snapshot.state;
    if (s->snapshot.activation_code[0]) {
        arc(layer, x, y, UI_RING_RADIUS, 0, 360, UI_ACCENT, UI_STROKE, LV_OPA_50);
        return;
    }
    if (state == HOME_OFFLINE) {
        arc(layer, x, y, UI_RING_RADIUS, 18, 146, UI_TEXT_DIM, UI_STROKE, LV_OPA_50);
        arc(layer, x, y, UI_RING_RADIUS, 198, 326, UI_TEXT_DIM, UI_STROKE, LV_OPA_50);
        wifi(layer, x, y - UI_PAD, false, LV_OPA_COVER);
        return;
    }
    arc(layer, x, y, UI_RING_RADIUS, 0, 360, UI_ACCENT, UI_STROKE, LV_OPA_20);
    if (state == HOME_IDLE) {
        arc(layer, x, y, UI_RING_INNER, 0, 360, UI_ACCENT, UI_STROKE, LV_OPA_50);
        mic(layer, x, y - 3, UI_TEXT_DIM, LV_OPA_COVER);
    } else if (state == HOME_LISTENING) {
        const int r = UI_RING_INNER + wave(phase) * 3 / 32768;
        arc(layer, x, y, r, 0, 360, UI_ACCENT, UI_STROKE, 210);
        for (int i = -3; i <= 3; ++i) {
            int amplitude = 7 + (3 - abs(i)) * 5;
            int h = amplitude + abs(wave(phase + i * 43)) * (s->snapshot.level + 4) / 32768;
            line(layer, x + i * 10, y - h, x + i * 10, y + h, UI_ACCENT, 3, LV_OPA_COVER);
        }
    } else if (state == HOME_BUSY) {
        arc(layer, x, y, UI_RING_INNER, phase, phase + 92, UI_ACCENT, 3, LV_OPA_COVER);
        arc(layer, x, y, UI_RING_INNER, phase + 180, phase + 212, UI_ACCENT, UI_STROKE, LV_OPA_50);
        for (int i = -1; i <= 1; ++i)
            rect(layer, x + i * 18 - 3, y - 3, 6, 6, 3, UI_ACCENT,
                 (lv_opa_t)(130 + wave(phase + i * 80) * 90 / 32768), true);
    } else {
        for (int i = 0; i < 3; ++i) {
            int t = (s->motion + i * 341) % 1024;
            arc(layer, x, y, 38 + t * 41 / 1024, 0, 360,
                UI_ACCENT, UI_STROKE, (lv_opa_t)(230 - t * 190 / 1024));
        }
        mic(layer, x, y - 3, UI_ACCENT, LV_OPA_COVER);
    }
}

static void draw_hero(lv_event_t *event)
{
    home_screen_t *s = lv_event_get_user_data(event);
    lv_area_t a; lv_obj_get_coords(s->hero, &a);
    const int x = (a.x1 + a.x2 + 1) / 2, y = (a.y1 + a.y2 + 1) / 2;
    draw_orbit(s, lv_event_get_layer(event), x, y);
}

static void draw_action_icon(lv_event_t *event)
{
    home_screen_t *s = lv_event_get_user_data(event);
    lv_area_t a; lv_obj_get_coords(s->button_icon, &a);
    lv_layer_t *layer = lv_event_get_layer(event);
    int x = a.x1 + UI_BUTTON_ICON_SIZE / 2, y = a.y1 + UI_BUTTON_ICON_SIZE / 2;
    lv_opa_t opa = s->snapshot.action_enabled ? LV_OPA_COVER : LV_OPA_40;
    if (action_for(s) == HOME_END_CALL)
        rect(layer, x - 6, y - 6, 13, 13, 3, UI_ACCENT, opa, false);
    else if (action_for(s) == HOME_SETUP_WIFI)
        wifi(layer, x, y - UI_GAP, true, opa);
    else if (action_for(s) == HOME_OPEN_SETTINGS || action_for(s) == HOME_OPEN_SESSIONS) {
        line(layer, x - 5, y - 6, x + 5, y, UI_TEXT, UI_STROKE, opa);
        line(layer, x + 5, y, x - 5, y + 6, UI_TEXT, UI_STROKE, opa);
    } else if (action_for(s) == HOME_NO_ACTION) {
        arc(layer, x, y, 8, 25, 300, UI_TEXT_DIM, UI_STROKE, opa);
    }
    else {
        rect(layer, x - 4, y - 9, 8, 13, 4, UI_TEXT, opa, false);
        arc(layer, x, y, 8, 0, 180, UI_TEXT, UI_STROKE, opa);
        line(layer, x, y + 8, x, y + 11, UI_TEXT, UI_STROKE, opa);
    }
}

static void button_styles_init(void)
{
    if (styles_ready) return;
    styles_ready = true;
    static const lv_style_prop_t transition_props[] = {LV_STYLE_BG_COLOR, LV_STYLE_TRANSLATE_Y, 0};
    lv_style_transition_dsc_init(&press_transition, transition_props, lv_anim_path_ease_out,
                                 UI_PRESS_MS, 0, NULL);
    lv_style_init(&button_base);
    lv_style_set_bg_color(&button_base, UI_SURFACE);
    lv_style_set_bg_opa(&button_base, LV_OPA_COVER);
    lv_style_set_radius(&button_base, UI_RADIUS);
    lv_style_set_border_width(&button_base, 1);
    lv_style_set_border_color(&button_base, UI_BORDER);
    lv_style_set_shadow_width(&button_base, 0);
    lv_style_set_pad_all(&button_base, 0);
    lv_style_set_transition(&button_base, &press_transition);
    lv_style_init(&button_pressed);
    lv_style_set_bg_color(&button_pressed, UI_SURFACE_PRESSED);
    lv_style_set_translate_y(&button_pressed, 1);
    lv_style_init(&button_disabled);
    lv_style_set_opa(&button_disabled, LV_OPA_40);
    lv_style_init(&button_active);
    lv_style_set_border_color(&button_active, UI_ACCENT);
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t color, int y)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, color, 0);
    lv_label_set_text(o, text);
    lv_obj_set_width(o, UI_SIZE);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(o, 0, y);
    return o;
}

static void clicked(lv_event_t *event)
{
    home_screen_t *s = lv_event_get_user_data(event);
    if (s->snapshot.action_enabled && s->on_action)
        s->on_action(action_for(s), s->context);
}

/* Adapted from lvgl-ui/assets/templates/action_button.c; single shared anatomy. */
static lv_obj_t *make_action_button(home_screen_t *s, int y)
{
    button_styles_init();
    lv_obj_t *b = lv_button_create(s->root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, UI_CENTER - UI_BUTTON_W / 2, y);
    lv_obj_set_size(b, UI_BUTTON_W, UI_BUTTON_H);
    lv_obj_set_ext_click_area(b, UI_TOUCH_EXT);
    lv_obj_add_style(b, &button_base, 0);
    lv_obj_add_style(b, &button_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &button_disabled, LV_STATE_DISABLED);
    lv_obj_add_style(b, &button_active, LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, clicked, LV_EVENT_CLICKED, s);
    s->button_icon = plain(b, UI_BUTTON_ICON_X, UI_BUTTON_ICON_Y,
                            UI_BUTTON_ICON_SIZE, UI_BUTTON_ICON_SIZE);
    lv_obj_add_event_cb(s->button_icon, draw_action_icon, LV_EVENT_DRAW_MAIN, s);
    s->button_label = lv_label_create(b);
    lv_obj_remove_style_all(s->button_label);
    lv_obj_set_style_text_font(s->button_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s->button_label, UI_TEXT, 0);
    lv_obj_set_pos(s->button_label, UI_BUTTON_LABEL_X, UI_BUTTON_LABEL_Y);
    return b;
}

static void motion_exec(void *var, int32_t value)
{
    home_screen_t *s = var;
    if (s->motion == value) return;
    const uint32_t at = lv_tick_get();
    if (at - s->motion_at < UI_MOTION_FRAME_MS) return;
    s->motion_at = at;
    s->motion = value;
    lv_obj_invalidate(s->hero); /* Only the bounded centre is animated. */
}

static void animate(home_screen_t *s)
{
    lv_anim_delete(s, motion_exec);
    s->motion = 0;
    if (!s->visible || (!active(s) && s->snapshot.state != HOME_BUSY)) return;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, s); lv_anim_set_exec_cb(&a, motion_exec);
    lv_anim_set_values(&a, 0, 1023); lv_anim_set_duration(&a, UI_MOTION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

home_screen_t *home_screen_create(lv_obj_t *parent, home_action_cb_t on_action, void *context)
{
    home_screen_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->on_action = on_action; s->context = context;
    s->visible = true;
    s->root = plain(parent, 0, 0, UI_SIZE, UI_SIZE);
    lv_obj_set_style_bg_color(s->root, UI_BG, 0);
    lv_obj_set_style_bg_opa(s->root, LV_OPA_COVER, 0);
    s->header = plain(s->root, 0, UI_HEADER_Y, UI_SIZE, UI_HEADER_H);
    lv_obj_set_style_bg_color(s->header, UI_BG, 0);
    lv_obj_set_style_bg_opa(s->header, LV_OPA_COVER, 0);
    lv_obj_t *handle = plain(s->header, UI_CENTER - UI_HANDLE_W / 2, UI_HANDLE_Y,
                              UI_HANDLE_W, UI_HANDLE_H);
    lv_obj_set_style_bg_color(handle, UI_BORDER, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    s->status = plain(s->header, UI_STATUS_X, UI_STATUS_Y - UI_HEADER_Y, UI_STATUS_W, UI_STATUS_H);
    lv_obj_add_event_cb(s->status, draw_status, LV_EVENT_DRAW_MAIN, s);
    s->battery = label(s->header, "", UI_FONT_SMALL, UI_TEXT_DIM, UI_STATUS_Y - UI_HEADER_Y + 3);
    lv_obj_set_width(s->battery, LV_SIZE_CONTENT);
    lv_obj_set_x(s->battery, UI_BATTERY_LABEL_X);
    lv_obj_t *brand = label(s->header, "JARVIS", UI_FONT_SMALL, UI_TEXT_DIM, UI_BRAND_Y - UI_HEADER_Y);
    lv_obj_set_style_text_letter_space(brand, UI_BRAND_TRACKING, 0);
    s->hero = plain(s->root, UI_ORBIT_X, UI_ORBIT_Y, UI_ORBIT_SIZE, UI_ORBIT_SIZE);
    lv_obj_add_event_cb(s->hero, draw_hero, LV_EVENT_DRAW_MAIN, s);
    s->code = label(s->root, "", UI_FONT_TITLE, UI_TEXT, UI_CODE_Y);
    lv_obj_set_width(s->code, UI_CODE_W);
    lv_obj_set_x(s->code, UI_CENTER - UI_CODE_W / 2);
    s->title = label(s->root, "", UI_FONT_TITLE, UI_TEXT, UI_ORBIT_TITLE_Y);
    s->hint = label(s->root, "", UI_FONT_SMALL, UI_TEXT_DIM, UI_ORBIT_HINT_Y);
    lv_obj_set_width(s->hint, UI_HINT_W);
    lv_obj_set_x(s->hint, UI_CENTER - UI_HINT_W / 2);
    s->button = make_action_button(s, UI_ORBIT_BUTTON_Y);
    home_snapshot_t initial = {.state = HOME_IDLE, .wifi_connected = true,
                              .action_enabled = true};
    home_screen_update(s, &initial);
    lv_obj_update_layout(s->root);
    lv_obj_invalidate(s->root);
    return s;
}

void home_screen_update(home_screen_t *s, const home_snapshot_t *snapshot)
{
    const bool state_changed = s->snapshot.state != snapshot->state;
    const home_action_t previous_action = action_for(s);
    const bool first = lv_label_get_text(s->title)[0] == '\0';
    const bool action_changed = s->snapshot.action_enabled != snapshot->action_enabled;
    const bool battery_changed = s->snapshot.battery_known != snapshot->battery_known ||
                                 s->snapshot.battery_percent != snapshot->battery_percent;
    const bool wifi_changed = s->snapshot.wifi_connected != snapshot->wifi_connected;
    const bool level_changed = s->snapshot.level != snapshot->level;
    const bool code_changed = strcmp(s->snapshot.activation_code, snapshot->activation_code) != 0;
    s->snapshot = *snapshot;
    static const char *titles[] = {"Ready", "Listening", "Listening", "Offline", "Connecting"};
    static const char *hints[] = {"Tap to begin", "Microphone is on", "Microphone is on",
                                 "Connect to get started", "Please wait"};
    const char *title = snapshot->title ? snapshot->title : titles[snapshot->state];
    const char *hint = snapshot->hint ? snapshot->hint : hints[snapshot->state];
    const char *caption = snapshot->action_label ? snapshot->action_label :
                          active(s) ? "End call" : snapshot->state == HOME_OFFLINE ? "Connect" : "Start call";
    if (strcmp(lv_label_get_text(s->title), title)) lv_label_set_text(s->title, title);
    if (strcmp(lv_label_get_text(s->hint), hint)) lv_label_set_text(s->hint, hint);
    if (strcmp(lv_label_get_text(s->button_label), caption)) {
        lv_label_set_text(s->button_label, caption);
        lv_obj_invalidate(s->button_icon);
    }
    lv_obj_set_state(s->button, LV_STATE_CHECKED, action_for(s) == HOME_END_CALL);
    if (state_changed || first) animate(s);
    if (code_changed || first) lv_label_set_text(s->code, snapshot->activation_code);
    if (battery_changed || first) {
        if (snapshot->battery_known) lv_label_set_text_fmt(s->battery, "%u%%", snapshot->battery_percent);
        else lv_label_set_text(s->battery, ""); /* Never invent a production battery level. */
    }
    if (previous_action != action_for(s) || action_changed || first) {
        lv_obj_set_state(s->button, LV_STATE_DISABLED, !snapshot->action_enabled);
        lv_obj_invalidate(s->button_icon);
    }
    if (wifi_changed || battery_changed || first) {
        lv_obj_update_layout(s->header);
        lv_obj_invalidate(s->header);
    }
    if (state_changed || level_changed || code_changed || first) lv_obj_invalidate(s->hero);
}

void home_screen_set_visible(home_screen_t *s, bool visible)
{
    if (s->visible == visible) return;
    s->visible = visible;
    animate(s);
}

lv_obj_t *home_screen_header(home_screen_t *s) { return s->header; }

lv_obj_t *home_screen_action_button(home_screen_t *s) { return s->button; }

void home_screen_destroy(home_screen_t *s)
{
    if (!s) return;
    lv_anim_delete(s, motion_exec);
    lv_obj_delete(s->root);
    free(s);
}
