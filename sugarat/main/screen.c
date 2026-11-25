#include "screen.h"
#include "screen_conversations.h"
#include "screen_port.h"
#include "screen_widgets.h"

#include "board.h"
#include "lvgl.h"
#include "servo_radio.h"
#include <string.h>

enum { WAVE_BARS = 9 };

typedef enum {
    PAGE_HOME, PAGE_SHADE, PAGE_SETTINGS, PAGE_MANUAL, PAGE_WIFI, PAGE_VOLUME, PAGE_BRIGHTNESS, PAGE_SESSIONS, PAGE_COUNT
} page_t;

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *readout;
    screen_action_kind_t action;
    bool dragging;
} level_control_t;

static level_control_t volume = {.action = SCREEN_SET_VOLUME};
static level_control_t brightness = {.action = SCREEN_SET_BRIGHTNESS};

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *readout;
    screen_action_kind_t action;
    int confirmed;
    bool dragging;
    bool awaiting;
} adjustment_t;

typedef struct {
    const char *title;
    const char *detail;
    uint32_t tint;
} voice_copy_t;

static const voice_copy_t voice_copy[] = {
    [SUGARAT_VOICE_BOOT]         = {"Starting Jarvis", "", LILAC},
    [SUGARAT_VOICE_SETUP]        = {"Set up Wi-Fi", "Join Sugarat-Jarvis on your phone", AMBER},
    [SUGARAT_VOICE_JOINING]      = {"Joining Wi-Fi", "", AMBER},
    [SUGARAT_VOICE_PLATFORM_CONNECTING] = {"Connecting", "Contacting the platform", LILAC},
    [SUGARAT_VOICE_PLATFORM_RETRYING]   = {"Platform unreachable", "Retrying", AMBER},
    [SUGARAT_VOICE_ACTIVATE]     = {"Activate device", "Enter this code at bot.sugarat.ai", AMBER},
    [SUGARAT_VOICE_BIND_AGENT]   = {"Choose an agent", "Bind a voice agent in Sugarat", AMBER},
    [SUGARAT_VOICE_PREPARING]    = {"Connecting voice", "Waiting for the voice server", LILAC},
    [SUGARAT_VOICE_READY]        = {"Say \"Jarvis\"", "Or tap the waveform", TEAL},
    [SUGARAT_VOICE_OPENING]      = {"Opening a call", "You can start speaking", LILAC},
    [SUGARAT_VOICE_IN_CALL]      = {"In conversation", "Listening", TEAL},
    [SUGARAT_VOICE_RECONNECTING] = {"Reconnecting voice", "Restoring your conversation", AMBER},
    [SUGARAT_VOICE_ENDING]       = {"Ending the call", "", LILAC},
    [SUGARAT_VOICE_ERROR]        = {"Voice unavailable", "Restart Sugarat to try again", RED},
    [SUGARAT_VOICE_RETRY]        = {"Call interrupted", "Restoring the voice connection", AMBER},
    [SUGARAT_VOICE_NO_SESSION]   = {"No voice session", "Create one in Settings", LILAC},
};

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *home_grab;
static lv_obj_t *receiver_status;
static lv_obj_t *targets[2];
static lv_obj_t *light_button;
static lv_obj_t *light_label;
static lv_obj_t *angle_caption;
static adjustment_t angle = {.action = SCREEN_SET_ANGLE};
static adjustment_t speed = {.action = SCREEN_SET_SPEED};
static lv_obj_t *voice_ring;
static lv_obj_t *voice_core;
static lv_obj_t *voice_orbit[2];
static lv_obj_t *waveform[WAVE_BARS];
static lv_obj_t *voice_title;
static lv_obj_t *voice_detail;
static lv_obj_t *voice_end;
static lv_obj_t *wifi_status;
static lv_obj_t *wifi_setup;

static page_t active_page;
static screen_action_t action;
static screen_state_t displayed;
static bool has_state;
static uint32_t wave_updated;
static int32_t pull_start_y;
static bool pulling;
static lv_point_t home_swipe_start;

lv_obj_t *screen_surface(lv_obj_t *parent, int width, int height, int radius, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

lv_obj_t *screen_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *screen_button(lv_obj_t *parent, int width, int height, const char *text, lv_obj_t **label)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *title = screen_label(object, text, &lv_font_montserrat_16, TEXT);
    lv_obj_center(title);
    if (label) {
        *label = title;
    }
    return object;
}

void screen_set_hidden(lv_obj_t *object, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void slide_y(void *object, int32_t y)
{
    lv_obj_set_y(object, y);
}

static void show_page(page_t page)
{
    if (page == active_page) return;
    if (active_page == PAGE_SESSIONS) screen_conversations_open(false);
    lv_anim_delete(pages[active_page], slide_y);
    screen_set_hidden(pages[active_page], true);
    active_page = page;
    if (page == PAGE_SESSIONS) screen_conversations_open(true);
    screen_set_hidden(pages[page], false);
    screen_set_hidden(home_grab, page == PAGE_HOME);
    lv_obj_set_y(pages[page], 0);
    if (page == PAGE_SHADE) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, pages[page]);
        lv_anim_set_exec_cb(&animation, slide_y);
        lv_anim_set_values(&animation, -SCREEN_SIZE, 0);
        lv_anim_set_duration(&animation, 180);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_start(&animation);
    }
}

static void navigate(lv_event_t *event)
{
    show_page((page_t)(uintptr_t)lv_event_get_user_data(event));
}

static void pull_down(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        pull_start_y = point.y;
        pulling = true;
    } else if (code == LV_EVENT_PRESSING && pulling && point.y - pull_start_y >= 36) {
        pulling = false;
        lv_indev_wait_release(input);
        show_page(PAGE_SHADE);
    } else if (code == LV_EVENT_CLICKED) {
        show_page(PAGE_SHADE);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        pulling = false;
    }
}

static void dismiss_shade(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input = lv_indev_active();
    if (input && lv_indev_get_gesture_dir(input) == LV_DIR_TOP) {
        lv_indev_wait_release(input);
        show_page(PAGE_HOME);
    }
}

static void swipe_home(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (!input) return;

    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        home_swipe_start = point;
        return;
    }

    const int32_t dy = home_swipe_start.y - point.y;
    const int32_t dx = point.x - home_swipe_start.x;
    if (dy >= 48 && dy > 2 * dx && dy > -2 * dx) {
        lv_indev_wait_release(input);
        show_page(PAGE_HOME);
    }
}

static void build_header(lv_obj_t *page, const char *title, page_t back)
{
    lv_obj_t *back_button = screen_button(page, 40, 40, LV_SYMBOL_LEFT, NULL);
    lv_obj_set_pos(back_button, 66, 40);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(PANEL), 0);
    lv_obj_add_event_cb(back_button, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)back);
    lv_obj_t *label = screen_label(page, title, &lv_font_montserrat_16, TEXT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 15, 52);
}

static lv_obj_t *menu_row(lv_obj_t *page, int y, int height, const char *symbol, const char *title, uint32_t tint)
{
    lv_obj_t *row = screen_button(page, 242, height, "", NULL);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(PANEL), 0);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_t *tile = screen_surface(row, 38, 38, 12, tint);
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_center(screen_label(tile, symbol, &lv_font_montserrat_16, BG));
    lv_obj_align(screen_label(row, title, &lv_font_montserrat_16, TEXT), LV_ALIGN_LEFT_MID, 58, 0);
    lv_obj_align(screen_label(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, MUTED), LV_ALIGN_RIGHT_MID, -14, 0);
    return row;
}

static void command_clicked(lv_event_t *event)
{
    const screen_action_t *command = lv_event_get_user_data(event);
    action = *command;
}

static void level_input(lv_event_t *event)
{
    level_control_t *control = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        control->dragging = true;
    } else if (control->dragging && (code == LV_EVENT_VALUE_CHANGED ||
                                     code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {
        const int value = lv_slider_get_value(control->slider);
        lv_label_set_text_fmt(control->readout, "%d%%", value);
        control->dragging = code == LV_EVENT_VALUE_CHANGED;
        action = (screen_action_t){.kind = control->action, .value = value, .preview = control->dragging};
    }
}

static void build_level_page(lv_obj_t *page, level_control_t *control, int minimum,
                             const char *title, const char *symbol, uint32_t accent)
{
    build_header(page, title, PAGE_SETTINGS);
    control->slider = lv_slider_create(page);
    lv_obj_t *slider = control->slider;
    lv_obj_set_size(slider, 80, 176);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 99);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_ext_click_area(slider, 10);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(slider, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_slider_set_range(slider, minimum, 100);
    lv_obj_add_event_cb(slider, level_input, LV_EVENT_ALL, control);

    lv_obj_t *icon = screen_surface(page, 32, 32, LV_RADIUS_CIRCLE, BG);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 229);
    lv_obj_center(screen_label(icon, symbol, &lv_font_montserrat_16, TEXT));
    control->readout = screen_label(page, "", &lv_font_montserrat_24, TEXT);
    lv_obj_align(control->readout, LV_ALIGN_TOP_MID, 0, 292);
}

static void sync_level(level_control_t *control, int value)
{
    if (!control->dragging &&
        (!has_state || lv_slider_get_value(control->slider) != value)) {
        lv_slider_set_value(control->slider, value, LV_ANIM_OFF);
        lv_label_set_text_fmt(control->readout, "%d%%", value);
    }
}

static void light_clicked(lv_event_t *event)
{
    action = (screen_action_t){
        .kind = SCREEN_SET_LIGHT,
        .value = displayed.manual.light == SERVO_LIGHT_ON ? SERVO_LIGHT_OFF : SERVO_LIGHT_ON,
    };
}

void screen_mark_enabled(lv_obj_t *object, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(object, LV_STATE_DISABLED);
    }
}

static void readout(adjustment_t *control, int value)
{
    if (control->action == SCREEN_SET_ANGLE) {
        lv_label_set_text_fmt(control->readout, "%d°", value * SERVO_ANGLE_INCREMENT);
    } else {
        const unsigned us_per_step = SERVO_SPEED_MIN + value * SERVO_SPEED_INCREMENT;
        const unsigned centiseconds = (1000u + us_per_step - 1) / us_per_step;
        lv_label_set_text_fmt(control->readout, "%u.%02u s", centiseconds / 100, centiseconds % 100);
    }
}

static void slider_input(lv_event_t *event)
{
    adjustment_t *control = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        control->dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        readout(control, lv_slider_get_value(control->slider));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        const int value = lv_slider_get_value(control->slider);
        if (code == LV_EVENT_RELEASED && control->dragging && value != control->confirmed) {
            const int setting = control->action == SCREEN_SET_ANGLE ? value * SERVO_ANGLE_INCREMENT
                               : SERVO_SPEED_MIN + value * SERVO_SPEED_INCREMENT;
            action = (screen_action_t){.kind = control->action, .value = setting};
            control->awaiting = true;
        } else {
            lv_slider_set_value(control->slider, control->confirmed, LV_ANIM_OFF);
            readout(control, control->confirmed);
        }
        control->dragging = false;
    }
}

static lv_obj_t *build_adjustment(lv_obj_t *page, adjustment_t *control, int y,
                                 int width, const char *caption, uint32_t tint)
{
    lv_obj_t *card = screen_surface(page, width, 60, 20, PANEL);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(EDGE), 0);

    lv_obj_t *label = screen_label(card, caption, &lv_font_montserrat_14, MUTED);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 13, 11);
    control->readout = screen_label(card, "", &lv_font_montserrat_24, TEXT);
    lv_obj_align(control->readout, LV_ALIGN_TOP_RIGHT, -13, 2);

    lv_obj_t *slider = lv_slider_create(card);
    control->slider = slider;
    lv_obj_set_size(slider, width - 34, 10);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 39);
    lv_obj_set_ext_click_area(slider, 9);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(tint), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(tint), LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(slider, lv_color_hex(MUTED), LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_input, LV_EVENT_ALL, control);
    return label;
}

static void build_servos(lv_obj_t *page)
{
    static const screen_action_t target_commands[] = {
        {.kind = SCREEN_SET_TARGET, .value = 0},
        {.kind = SCREEN_SET_TARGET, .value = 1},
    };
    build_header(page, "Manual control", PAGE_SETTINGS);
    receiver_status = screen_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(receiver_status, LV_ALIGN_TOP_MID, 0, 81);

    lv_obj_t *selector = screen_surface(page, 254, 54, 27, PANEL);
    lv_obj_align(selector, LV_ALIGN_TOP_MID, 0, 105);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *label;
        targets[i] = screen_button(selector, 119, 46, i == 0 ? "Position A" : "Position B", &label);
        lv_obj_set_pos(targets[i], 4 + i * 127, 4);
        lv_obj_set_style_bg_color(targets[i], lv_color_hex(PANEL), 0);
        lv_obj_set_style_bg_color(targets[i], lv_color_hex(TEAL), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(targets[i], lv_color_hex(INK), LV_STATE_CHECKED);
        // Inherit the checked foreground from the button.
        lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
        lv_obj_set_style_text_color(targets[i], lv_color_hex(TEXT), 0);
        lv_obj_add_event_cb(targets[i], command_clicked, LV_EVENT_CLICKED, (void *)&target_commands[i]);
    }

    angle_caption = build_adjustment(page, &angle, 165, 274, "", TEAL);
    build_adjustment(page, &speed, 228, 250, "Travel time / 90°", LILAC);
    lv_slider_set_range(speed.slider, 0, (SERVO_SPEED_MAX - SERVO_SPEED_MIN) / SERVO_SPEED_INCREMENT);

    light_button = screen_button(page, 134, 42, "Light", &light_label);
    lv_obj_align(light_button, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_add_event_cb(light_button, light_clicked, LV_EVENT_CLICKED, NULL);

}

static lv_obj_t *orbit(lv_obj_t *parent, int size, int start, int end)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, start, end);
    lv_obj_set_style_arc_width(arc, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(EDGE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    return arc;
}

static void build_home(lv_obj_t *page)
{
    static const screen_action_t start_command = {.kind = SCREEN_START_CALL};
    static const screen_action_t end_command = {.kind = SCREEN_END_CALL};

    // The grab area starts at the top and extends until the drag threshold.
    lv_obj_t *grab = screen_surface(page, 200, 78, 0, BG);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(grab, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(grab, pull_down, LV_EVENT_ALL, NULL);
    lv_obj_t *handle = screen_surface(grab, 42, 4, 2, TRACK);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 27);

    voice_ring = screen_surface(page, 170, 170, LV_RADIUS_CIRCLE, BG);
    lv_obj_align(voice_ring, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_add_event_cb(voice_ring, command_clicked, LV_EVENT_CLICKED, (void *)&start_command);
    voice_orbit[0] = orbit(voice_ring, 168, 202, 302);
    voice_orbit[1] = orbit(voice_ring, 154, 16, 139);

    voice_core = screen_surface(voice_ring, 132, 132, LV_RADIUS_CIRCLE, PANEL);
    lv_obj_center(voice_core);
    lv_obj_set_style_border_width(voice_core, 1, 0);
    lv_obj_set_style_border_color(voice_core, lv_color_hex(EDGE), 0);
    lv_obj_set_style_bg_grad_dir(voice_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(voice_core, lv_color_hex(BG), 0);
    for (int i = 0; i < WAVE_BARS; ++i) {
        waveform[i] = screen_surface(voice_core, 5, 8, LV_RADIUS_CIRCLE, TEAL);
        lv_obj_align(waveform[i], LV_ALIGN_CENTER, (i - WAVE_BARS / 2) * 11, 0);
    }

    voice_title = screen_label(page, "", &lv_font_montserrat_24, TEXT);
    lv_obj_align(voice_title, LV_ALIGN_TOP_MID, 0, 247);
    voice_detail = screen_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_align(voice_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(voice_detail, 232);
    lv_label_set_long_mode(voice_detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(voice_detail, LV_ALIGN_TOP_MID, 0, 281);
    voice_end = screen_button(page, 120, 37, LV_SYMBOL_CLOSE "   End call", NULL);
    lv_obj_align(voice_end, LV_ALIGN_TOP_MID, 0, 304);
    lv_obj_set_style_bg_color(voice_end, lv_color_hex(0x4C202B), 0);
    lv_obj_add_event_cb(voice_end, command_clicked, LV_EVENT_CLICKED, (void *)&end_command);
}

static void build_settings(void)
{
    lv_obj_t *shade = pages[PAGE_SHADE];
    lv_obj_add_flag(shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(shade, dismiss_shade, LV_EVENT_GESTURE, NULL);
    lv_obj_t *title = screen_label(shade, "Control center", &lv_font_montserrat_24, TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 77);
    lv_obj_t *settings = menu_row(shade, 137, 58, LV_SYMBOL_SETTINGS, "Settings", LILAC);
    lv_obj_add_event_cb(settings, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_SETTINGS);
    lv_obj_t *done = screen_button(shade, 90, 42, "Done", NULL);
    lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 245);
    lv_obj_set_style_bg_color(done, lv_color_hex(PANEL), 0);
    lv_obj_add_event_cb(done, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_HOME);

    lv_obj_t *page = pages[PAGE_SETTINGS];
    build_header(page, "Settings", PAGE_SHADE);
    lv_obj_t *sound = menu_row(page, 88, 43, LV_SYMBOL_VOLUME_MAX, "Volume", TEAL);
    lv_obj_add_event_cb(sound, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_VOLUME);
    lv_obj_t *light = menu_row(page, 135, 43, LV_SYMBOL_EYE_OPEN, "Brightness", AMBER);
    lv_obj_add_event_cb(light, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_BRIGHTNESS);
    lv_obj_t *sessions = menu_row(page, 182, 43, LV_SYMBOL_EDIT, "Sessions", LILAC);
    lv_obj_add_event_cb(sessions, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_SESSIONS);
    lv_obj_t *manual = menu_row(page, 229, 43, LV_SYMBOL_LIST, "Manual control", TEAL);
    lv_obj_add_event_cb(manual, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_MANUAL);
    lv_obj_t *network = menu_row(page, 276, 43, LV_SYMBOL_WIFI, "Wi-Fi", LILAC);
    lv_obj_add_event_cb(network, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_WIFI);

    static const screen_action_t setup = {.kind = SCREEN_SETUP_WIFI};
    page = pages[PAGE_WIFI];
    build_header(page, "Wi-Fi", PAGE_SETTINGS);
    wifi_status = screen_label(page, "", &lv_font_montserrat_16, TEXT);
    lv_obj_set_width(wifi_status, 220);
    lv_obj_set_style_text_align(wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(wifi_status, LV_ALIGN_TOP_MID, 0, 117);
    wifi_setup = screen_button(page, 170, 48, "Set up Wi-Fi", NULL);
    lv_obj_set_style_bg_color(wifi_setup, lv_color_hex(PANEL), 0);
    lv_obj_align(wifi_setup, LV_ALIGN_TOP_MID, 0, 215);
    lv_obj_add_event_cb(wifi_setup, command_clicked, LV_EVENT_CLICKED, (void *)&setup);
    build_level_page(pages[PAGE_VOLUME], &volume, 0, "Volume", LV_SYMBOL_VOLUME_MAX, TEAL);
    build_level_page(pages[PAGE_BRIGHTNESS], &brightness, SUGARAT_BRIGHTNESS_MIN,
                     "Brightness", LV_SYMBOL_EYE_OPEN, AMBER);
    build_header(pages[PAGE_SESSIONS], "Sessions", PAGE_SETTINGS);
    screen_conversations_build(pages[PAGE_SESSIONS]);
}

esp_err_t screen_init(bool touch_ready)
{
    const esp_err_t err = screen_port_init(touch_ready);
    if (err != ESP_OK) return err;
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        pages[i] = screen_surface(root, SCREEN_SIZE, SCREEN_SIZE, 0, BG);
        screen_set_hidden(pages[i], i != PAGE_HOME);
    }
    build_home(pages[PAGE_HOME]);
    build_settings();
    build_servos(pages[PAGE_MANUAL]);

    home_grab = screen_surface(root, 160, 30, 0, BG);
    lv_obj_align(home_grab, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(home_grab, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(home_grab, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(home_grab, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(home_grab, swipe_home, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(home_grab, swipe_home, LV_EVENT_PRESSING, NULL);
    lv_obj_t *home_indicator = screen_surface(home_grab, 40, 4, LV_RADIUS_CIRCLE, MUTED);
    lv_obj_align(home_indicator, LV_ALIGN_BOTTOM_MID, 0, -9);
    screen_set_hidden(home_grab, true);
    return ESP_OK;
}

static void show_receiver(const manual_control_view_t *state, bool touch_ready)
{
    uint32_t tint = state->online ? TEAL : AMBER;
    if (!touch_ready) {
        lv_label_set_text(receiver_status, "Touchscreen unavailable");
        tint = RED;
    } else if (!state->online) {
        lv_label_set_text(receiver_status, "Finding receiver...");
    } else if (state->pending) {
        lv_label_set_text(receiver_status, "Sending to receiver...");
        tint = MUTED;
    } else if (state->moving) {
        lv_label_set_text_fmt(receiver_status, "Moving to %c", 'A' + state->position);
    } else if (state->receiver_channel != 0) {
        lv_label_set_text_fmt(receiver_status, "Receiver connected  /  ch %u", (unsigned)state->receiver_channel);
    } else {
        lv_label_set_text(receiver_status, "Receiver connected");
    }
    lv_obj_set_style_text_color(receiver_status, lv_color_hex(tint), 0);
}

static void sync_adjustment(adjustment_t *control, int confirmed, bool enabled, bool waiting)
{
    control->confirmed = confirmed;
    screen_mark_enabled(control->slider, enabled);
    if (!enabled) {
        control->dragging = false;
    }
    control->awaiting &= waiting;
    if (!control->dragging && !control->awaiting &&
        (!has_state || lv_slider_get_value(control->slider) != confirmed)) {
        lv_slider_set_value(control->slider, confirmed, LV_ANIM_OFF);
        readout(control, confirmed);
    }
}

static void show_voice(const sugarat_voice_view_t *voice)
{
    const voice_copy_t *copy = &voice_copy[voice->phase];
    const bool activation = voice->phase == SUGARAT_VOICE_ACTIVATE;
    if (voice->phase == SUGARAT_VOICE_READY) {
        lv_obj_add_flag(voice_ring, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(voice_ring, LV_OBJ_FLAG_CLICKABLE);
    }
    const lv_color_t tint = lv_color_hex(voice->speaking ? LILAC : copy->tint);
    lv_obj_set_style_bg_color(voice_core, lv_color_mix(tint, lv_color_hex(BG), LV_OPA_20), 0);
    lv_obj_set_style_arc_color(voice_orbit[0], tint, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(voice_orbit[1], lv_color_mix(tint, lv_color_hex(EDGE), LV_OPA_50), LV_PART_INDICATOR);
    for (int i = 0; i < WAVE_BARS; ++i) {
        lv_obj_set_style_bg_color(waveform[i], tint, 0);
    }
    lv_obj_set_style_text_font(voice_title, activation ? &lv_font_montserrat_40 : &lv_font_montserrat_24, 0);
    lv_obj_align(voice_title, LV_ALIGN_TOP_MID, 0, activation ? 232 : 247);
    lv_label_set_text(voice_title, activation ? voice->activation_code : copy->title);
    lv_label_set_text(voice_detail, voice->phase == SUGARAT_VOICE_IN_CALL && voice->speaking
                                   ? "Jarvis is speaking" : copy->detail);
    screen_set_hidden(voice_end, !voice->can_end);
}

static void animate_wave(const sugarat_voice_view_t *voice)
{
    const uint32_t tick = lv_tick_get();
    if (tick - wave_updated < 90) return;
    wave_updated = tick;
    const unsigned cycle = tick / 90;
    for (int i = 0; i < WAVE_BARS; ++i) {
        const int offset = i - WAVE_BARS / 2;
        const unsigned distance = (unsigned)(offset < 0 ? -offset : offset);
        const unsigned step = (cycle + i * 3) % 14;
        const unsigned pulse = step < 7 ? step : 14 - step;
        unsigned height = 6 + (5 - distance) * pulse / 4;
        if (voice->phase == SUGARAT_VOICE_IN_CALL) {
            height += ((unsigned)voice->level * (5 - distance) * (pulse + 3)) / 80;
        }
        if (lv_obj_get_height(waveform[i]) != (int32_t)height) lv_obj_set_height(waveform[i], height);
    }
}

static void update_manual(const screen_state_t *state)
{
    const manual_control_view_t *view = &state->manual;
    const manual_control_view_t *previous = &displayed.manual;
    const bool touch_changed = state->touch_ready != displayed.touch_ready;
    const bool enabled = view->online && !view->pending && state->touch_ready;
    if (!has_state || view->online != previous->online || view->pending != previous->pending || touch_changed ||
        view->receiver_channel != previous->receiver_channel || view->position != previous->position ||
        view->moving != previous->moving) {
        show_receiver(view, state->touch_ready);
        for (int i = 0; i < 2; ++i) {
            if (view->online && view->position == i) lv_obj_add_state(targets[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(targets[i], LV_STATE_CHECKED);
            screen_mark_enabled(targets[i], enabled);
        }
    }
    if (!has_state || view->angle_limit_deg != previous->angle_limit_deg) {
        lv_label_set_text_fmt(angle_caption, "B angle (0-%u°)", (unsigned)view->angle_limit_deg);
        lv_slider_set_range(angle.slider, 0, view->angle_limit_deg / SERVO_ANGLE_INCREMENT);
    }
    if (!has_state || view->light != previous->light || view->online != previous->online ||
        view->pending != previous->pending || touch_changed) {
        const bool known = view->online && view->light != SERVO_LIGHT_UNKNOWN;
        const bool on = known && view->light == SERVO_LIGHT_ON;
        lv_label_set_text(light_label, known ? (on ? "Light on" : "Light off") : "Light");
        lv_obj_set_style_bg_color(light_button, lv_color_hex(on ? AMBER : PANEL), 0);
        lv_obj_set_style_text_color(light_label, lv_color_hex(on ? INK : TEXT), 0);
        screen_mark_enabled(light_button, enabled && known);
    }
    sync_adjustment(&angle, view->angle_deg / SERVO_ANGLE_INCREMENT, enabled, view->pending);
    sync_adjustment(&speed, (view->speed - SERVO_SPEED_MIN) / SERVO_SPEED_INCREMENT, enabled, view->pending);
}

void screen_update(const screen_state_t *state)
{
    update_manual(state);
    if (active_page == PAGE_SESSIONS) screen_conversations_update(&state->voice);
    sync_level(&volume, state->settings.volume);
    sync_level(&brightness, state->settings.brightness);
    if (!has_state || state->voice.phase != displayed.voice.phase || state->voice.can_end != displayed.voice.can_end ||
        state->voice.speaking != displayed.voice.speaking ||
        strcmp(state->voice.activation_code, displayed.voice.activation_code) != 0) {
        show_voice(&state->voice);
    }
    if (active_page == PAGE_HOME) animate_wave(&state->voice);
    if (!has_state || state->wifi_connected != displayed.wifi_connected ||
        state->wifi_setup_active != displayed.wifi_setup_active ||
        state->voice.can_end != displayed.voice.can_end || state->voice.phase != displayed.voice.phase) {
        const bool call_active = state->voice.can_end || state->voice.phase == SUGARAT_VOICE_OPENING;
        lv_label_set_text(wifi_status, state->wifi_setup_active ? "Join Sugarat-Jarvis on your phone" :
                          call_active ? "Voice call active" : state->wifi_connected ? "Connected" : "Disconnected");
        screen_mark_enabled(wifi_setup, !state->wifi_setup_active && !call_active);
    }
    displayed = *state;
    has_state = true;
}

screen_action_t screen_poll(void)
{
    lv_timer_handler();
    const screen_action_t selected = action;
    action = (screen_action_t){0};
    return selected;
}

bool screen_manual_is_open(void)
{
    return active_page == PAGE_MANUAL;
}
