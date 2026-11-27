#include "screen.h"
#include "screen_conversations.h"
#include "screen_port.h"
#include "screen_widgets.h"
#include "home_presenter.h"
#include "ui_button.h"
#include "ui_theme.h"

#include "board.h"
#include "lvgl.h"
#include "servo_radio.h"

#if defined(SUGARAT_NATIVE_AMOLED_UI)
#include "esp_timer.h"
#include "playback.h"
static int64_t next_poll_us;
#endif

enum { SECONDARY_PAGE_SIZE = 360 };
_Static_assert(UI_SIZE == SCREEN_SIZE, "Orbit geometry must match the panel");
static home_screen_t *orbit_home;

typedef enum {
    PAGE_HOME, PAGE_SHADE, PAGE_SETTINGS, PAGE_MANUAL, PAGE_NETWORK, PAGE_VOLUME, PAGE_BRIGHTNESS, PAGE_SESSIONS, PAGE_COUNT
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

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *home_grab;
static lv_obj_t *receiver_status;
static lv_obj_t *targets[2];
static lv_obj_t *light_button;
static lv_obj_t *light_label;
static lv_obj_t *angle_caption;
static adjustment_t angle = {.action = SCREEN_SET_ANGLE};
static adjustment_t speed = {.action = SCREEN_SET_SPEED};
static lv_obj_t *wifi_status;
static lv_obj_t *wifi_setup;
static lv_obj_t *voice_server_buttons[2];
static lv_obj_t *voice_server_detail;
static lv_obj_t *voice_server_confirmation;
static lv_obj_t *voice_server_confirmation_title;
static bool voice_server_pending_china;

static page_t active_page;
static screen_action_t action;
static screen_state_t displayed;
static bool has_state;
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
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
#else
    lv_obj_t *object = ui_button_create(parent, width, height, UI_DENSE_TOUCH_EXT);
#endif
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

static int page_origin(page_t page)
{
    return page == PAGE_HOME ? 0 : (SCREEN_SIZE - SECONDARY_PAGE_SIZE) / 2;
}

static void show_page(page_t page)
{
    if (page == active_page) return;
    if (active_page == PAGE_SESSIONS) screen_conversations_open(false);
    if (active_page == PAGE_NETWORK) screen_set_hidden(voice_server_confirmation, true);
    lv_anim_delete(pages[active_page], slide_y);
    screen_set_hidden(pages[active_page], true);
    active_page = page;
    if (page == PAGE_SESSIONS) screen_conversations_open(true);
    screen_set_hidden(pages[page], false);
    screen_set_hidden(home_grab, page == PAGE_HOME);
    lv_obj_set_y(pages[page], page_origin(page));
    home_screen_set_visible(orbit_home, page == PAGE_HOME);
    if (page == PAGE_SHADE) {
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, pages[page]);
        lv_anim_set_exec_cb(&animation, slide_y);
        lv_anim_set_values(&animation, -SCREEN_SIZE, page_origin(page));
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
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    lv_obj_t *back_button = screen_button(page, 40, 40, LV_SYMBOL_LEFT, NULL);
    lv_obj_set_pos(back_button, 66, 40);
#else
    lv_obj_t *back_button = screen_button(page, 48, 48, LV_SYMBOL_LEFT, NULL);
    lv_obj_set_pos(back_button, 66, 42);
#endif
    lv_obj_set_style_bg_color(back_button, lv_color_hex(PANEL), 0);
    lv_obj_add_event_cb(back_button, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)back);
    lv_obj_t *label = screen_label(page, title, &lv_font_montserrat_16, TEXT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 15, 52);
}

static lv_obj_t *menu_row(lv_obj_t *page, int y, int height, const char *symbol, const char *title, uint32_t tint)
{
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    lv_obj_t *row = screen_button(page, 242, height, "", NULL);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(PANEL), 0);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_t *tile = screen_surface(row, 38, 38, 12, tint);
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_center(screen_label(tile, symbol, &lv_font_montserrat_16, BG));
    lv_obj_align(screen_label(row, title, &lv_font_montserrat_16, TEXT), LV_ALIGN_LEFT_MID, 58, 0);
#else
    (void)tint;
    lv_obj_t *row = screen_button(page, UI_MENU_ROW_W, height, "", NULL);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(screen_label(row, symbol, &lv_font_montserrat_16, MUTED), LV_ALIGN_LEFT_MID, UI_PAD, 0);
    lv_obj_align(screen_label(row, title, &lv_font_montserrat_16, TEXT), LV_ALIGN_LEFT_MID, UI_MENU_LABEL_X, 0);
#endif
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
        lv_label_set_text_fmt(control->readout, "%d" UI_ANGLE_UNIT, value * SERVO_ANGLE_INCREMENT);
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
    lv_obj_t *card = screen_surface(page, width, UI_ADJUSTMENT_H, 20, PANEL);
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
    lv_obj_align(receiver_status, LV_ALIGN_TOP_MID, 0, UI_RECEIVER_STATUS_Y);

    lv_obj_t *selector = screen_surface(page, 254, 54, 27, PANEL);
    lv_obj_align(selector, LV_ALIGN_TOP_MID, 0, UI_SERVO_SELECTOR_Y);
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

    angle_caption = build_adjustment(page, &angle, UI_SERVO_ANGLE_Y, 274, "", TEAL);
    build_adjustment(page, &speed, UI_SERVO_SPEED_Y, 250, "Travel time / 90" UI_ANGLE_UNIT, LILAC);
    lv_slider_set_range(speed.slider, 0, (SERVO_SPEED_MAX - SERVO_SPEED_MIN) / SERVO_SPEED_INCREMENT);

    light_button = screen_button(page, 134, 42, "Light", &light_label);
    lv_obj_align(light_button, LV_ALIGN_TOP_MID, 0, UI_SERVO_LIGHT_Y);
    lv_obj_add_event_cb(light_button, light_clicked, LV_EVENT_CLICKED, NULL);
}

static void orbit_home_action(home_action_t selected, void *context)
{
    (void)context;
    switch (selected) {
    case HOME_START_CALL: action = (screen_action_t){.kind = SCREEN_START_CALL}; break;
    case HOME_END_CALL: action = (screen_action_t){.kind = SCREEN_END_CALL}; break;
    case HOME_SETUP_WIFI: action = (screen_action_t){.kind = SCREEN_SETUP_WIFI}; break;
    case HOME_OPEN_SETTINGS: show_page(PAGE_SETTINGS); break;
    case HOME_OPEN_SESSIONS: show_page(PAGE_SESSIONS); break;
    default: break;
    }
}

static void build_home(lv_obj_t *page)
{
    orbit_home = home_screen_create(page, orbit_home_action, NULL);
    if (!orbit_home) return;
    lv_obj_t *header = home_screen_header(orbit_home);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(header, pull_down, LV_EVENT_ALL, NULL);
}

static void choose_voice_server(lv_event_t *event)
{
    const bool china = (uintptr_t)lv_event_get_user_data(event) != 0;
    if (china == displayed.settings.voice_china) return;

    voice_server_pending_china = china;
    lv_label_set_text(voice_server_confirmation_title, china ? "Use China server?" : "Use overseas server?");
    screen_set_hidden(voice_server_confirmation, false);
}

static void close_voice_server_confirmation(lv_event_t *event)
{
    screen_set_hidden(voice_server_confirmation, true);
    if (lv_event_get_user_data(event)) {
        action = (screen_action_t){.kind = SCREEN_SET_VOICE_SERVER, .value = voice_server_pending_china};
    }
}

static void build_network(lv_obj_t *page)
{
    enum {
        STATUS_Y = 98, SETUP_Y = 128, SERVER_TITLE_Y = 196, SERVER_OPTIONS_Y = 222,
        SERVER_DETAIL_Y = 282, OPTION_WIDTH = 116, OPTION_OFFSET = 62, BUTTON_HEIGHT = 48,
        DIALOG_WIDTH = 242, DIALOG_HEIGHT = 170, DIALOG_BUTTON_WIDTH = 100,
    };
    static const screen_action_t setup = {.kind = SCREEN_SETUP_WIFI};
    static const char *const regions[] = {"Overseas", "China"};
    build_header(page, "Network", PAGE_SETTINGS);
    wifi_status = screen_label(page, "", &lv_font_montserrat_16, TEXT);
    lv_obj_set_width(wifi_status, 250);
    lv_obj_set_style_text_align(wifi_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(wifi_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);
    wifi_setup = screen_button(page, 170, BUTTON_HEIGHT, "Set up Wi-Fi", NULL);
    lv_obj_set_style_bg_color(wifi_setup, lv_color_hex(PANEL), 0);
    lv_obj_align(wifi_setup, LV_ALIGN_TOP_MID, 0, SETUP_Y);
    lv_obj_add_event_cb(wifi_setup, command_clicked, LV_EVENT_CLICKED, (void *)&setup);

    lv_obj_t *label = screen_label(page, "Voice server", &lv_font_montserrat_16, TEXT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, SERVER_TITLE_Y);
    for (uintptr_t i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        lv_obj_t *button = screen_button(page, OPTION_WIDTH, BUTTON_HEIGHT, regions[i], NULL);
        voice_server_buttons[i] = button;
        lv_obj_align(button, LV_ALIGN_TOP_MID, i == 0 ? -OPTION_OFFSET : OPTION_OFFSET, SERVER_OPTIONS_Y);
        lv_obj_set_style_bg_color(button, lv_color_hex(PANEL), 0);
        lv_obj_set_style_bg_color(button, lv_color_mix(lv_color_hex(TEAL), lv_color_hex(PANEL), LV_OPA_20),
                                  LV_STATE_CHECKED);
        lv_obj_set_style_border_width(button, 2, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(TEAL), LV_STATE_CHECKED);
        lv_obj_add_event_cb(button, choose_voice_server, LV_EVENT_CLICKED, (void *)i);
    }
    voice_server_detail = screen_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(voice_server_detail, LV_ALIGN_TOP_MID, 0, SERVER_DETAIL_Y);

    voice_server_confirmation = screen_surface(page, lv_pct(100), lv_pct(100), 0, BG);
    lv_obj_add_flag(voice_server_confirmation, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(voice_server_confirmation, LV_OPA_80, 0);
    lv_obj_t *card = screen_surface(voice_server_confirmation, DIALOG_WIDTH, DIALOG_HEIGHT, 26, PANEL);
    lv_obj_center(card);
    voice_server_confirmation_title = screen_label(card, "", &lv_font_montserrat_16, TEXT);
    lv_obj_align(voice_server_confirmation_title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_t *detail = screen_label(card, "Device will restart", &lv_font_montserrat_14, MUTED);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *cancel = screen_button(card, DIALOG_BUTTON_WIDTH, BUTTON_HEIGHT, "Cancel", NULL);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(EDGE), 0);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, -55, -20);
    lv_obj_add_event_cb(cancel, close_voice_server_confirmation, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_label;
    lv_obj_t *restart = screen_button(card, DIALOG_BUTTON_WIDTH, BUTTON_HEIGHT, "Restart", &restart_label);
    lv_obj_set_style_bg_color(restart, lv_color_hex(TEAL), 0);
    lv_obj_set_style_text_color(restart_label, lv_color_hex(INK), 0);
    lv_obj_align(restart, LV_ALIGN_BOTTOM_MID, 55, -20);
    lv_obj_add_event_cb(restart, close_voice_server_confirmation, LV_EVENT_CLICKED, restart);
    screen_set_hidden(voice_server_confirmation, true);
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
    lv_obj_t *sound = menu_row(page, UI_MENU_START_Y, UI_MENU_ROW_H, LV_SYMBOL_VOLUME_MAX, "Volume", TEAL);
    lv_obj_add_event_cb(sound, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_VOLUME);
    lv_obj_t *light = menu_row(page, UI_MENU_START_Y + UI_MENU_STEP_Y, UI_MENU_ROW_H, LV_SYMBOL_EYE_OPEN, "Brightness", AMBER);
    lv_obj_add_event_cb(light, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_BRIGHTNESS);
    lv_obj_t *sessions = menu_row(page, UI_MENU_START_Y + 2 * UI_MENU_STEP_Y, UI_MENU_ROW_H, LV_SYMBOL_EDIT, "Sessions", LILAC);
    lv_obj_add_event_cb(sessions, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_SESSIONS);
    lv_obj_t *manual = menu_row(page, UI_MENU_START_Y + 3 * UI_MENU_STEP_Y, UI_MENU_ROW_H, LV_SYMBOL_LIST, "Manual control", TEAL);
    lv_obj_add_event_cb(manual, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_MANUAL);
    lv_obj_t *network = menu_row(page, UI_MENU_START_Y + 4 * UI_MENU_STEP_Y, UI_MENU_ROW_H, LV_SYMBOL_WIFI, "Network", LILAC);
    lv_obj_add_event_cb(network, navigate, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_NETWORK);

    build_network(pages[PAGE_NETWORK]);
    build_level_page(pages[PAGE_VOLUME], &volume, 0, "Volume", LV_SYMBOL_VOLUME_MAX, TEAL);
    build_level_page(pages[PAGE_BRIGHTNESS], &brightness, SUGARAT_BRIGHTNESS_MIN,
                     "Brightness", LV_SYMBOL_EYE_OPEN, UI_LEVEL_BRIGHTNESS_RGB);
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
    lv_obj_set_style_bg_color(root, UI_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    for (int i = 0; i < PAGE_COUNT; ++i) {
        const int size = i == PAGE_HOME ? SCREEN_SIZE : SECONDARY_PAGE_SIZE;
        pages[i] = screen_surface(root, size, size, 0, BG);
        lv_obj_set_pos(pages[i], page_origin((page_t)i), page_origin((page_t)i));
        screen_set_hidden(pages[i], i != PAGE_HOME);
    }
    build_home(pages[PAGE_HOME]);
    if (!orbit_home) return ESP_ERR_NO_MEM;
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
        lv_label_set_text_fmt(angle_caption, "B angle (0-%u" UI_ANGLE_UNIT ")", (unsigned)view->angle_limit_deg);
        lv_slider_set_range(angle.slider, 0, view->angle_limit_deg / SERVO_ANGLE_INCREMENT);
    }
    if (!has_state || view->light != previous->light || view->online != previous->online ||
        view->pending != previous->pending || touch_changed) {
        const bool known = view->online && view->light != SERVO_LIGHT_UNKNOWN;
        const bool on = known && view->light == SERVO_LIGHT_ON;
        lv_label_set_text(light_label, known ? (on ? "Light on" : "Light off") : "Light");
#if defined(SUGARAT_NATIVE_AMOLED_UI)
        lv_obj_set_style_bg_color(light_button, lv_color_hex(on ? AMBER : PANEL), 0);
        lv_obj_set_style_text_color(light_label, lv_color_hex(on ? INK : TEXT), 0);
#else
        lv_obj_set_state(light_button, LV_STATE_CHECKED, on);
#endif
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
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    const unsigned playback = voice_playback_level();
#else
    const unsigned playback = state->voice.speaking;
#endif
    const home_snapshot_t home = home_presenter_snapshot(state, playback);
    home_screen_update(orbit_home, &home);
    if (!has_state || state->wifi_connected != displayed.wifi_connected ||
        state->wifi_setup_active != displayed.wifi_setup_active ||
        state->voice.can_end != displayed.voice.can_end || state->voice.phase != displayed.voice.phase ||
        state->settings.voice_china != displayed.settings.voice_china ||
        state->voice_server_save_failed != displayed.voice_server_save_failed) {
        const bool call_active = state->voice.can_end || state->voice.phase == SUGARAT_VOICE_OPENING;
        lv_label_set_text(wifi_status, state->wifi_setup_active ? "Join the setup hotspot" :
                          state->wifi_connected ? "Wi-Fi connected" : "Wi-Fi disconnected");
        screen_mark_enabled(wifi_setup, !state->wifi_setup_active && !call_active);
        const bool can_switch = !sugarat_voice_call_busy(&state->voice);
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_state(voice_server_buttons[i], LV_STATE_CHECKED, i == state->settings.voice_china);
            screen_mark_enabled(voice_server_buttons[i], can_switch);
        }
        if (!can_switch) screen_set_hidden(voice_server_confirmation, true);
        lv_label_set_text(voice_server_detail, !can_switch ? "End call to switch servers" :
                          state->voice_server_save_failed ? "Couldn't save server" :
                          sugarat_voice_server_host(state->settings.voice_china));
    }
    displayed = *state;
    has_state = true;
}

screen_action_t screen_poll(void)
{
#if defined(SUGARAT_NATIVE_AMOLED_UI)
    uint32_t wait_ms = lv_timer_handler();
    if (wait_ms > 15) wait_ms = 15; // Keep state polling and ESP-NOW responsive, including idle screens.
    if (wait_ms == 0) wait_ms = 1;
    next_poll_us = esp_timer_get_time() + (int64_t)wait_ms * 1000;
#else
    lv_timer_handler();
#endif
    const screen_action_t selected = action;
    action = (screen_action_t){0};
    return selected;
}

#if defined(SUGARAT_NATIVE_AMOLED_UI)
void screen_wait(void)
{
    screen_port_wait_until(next_poll_us);
}
#endif

bool screen_manual_is_open(void)
{
    return active_page == PAGE_MANUAL;
}
