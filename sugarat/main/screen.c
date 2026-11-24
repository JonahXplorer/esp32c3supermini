#include "screen.h"
#include "screen_port.h"

#include "lvgl.h"
#include "servo_radio.h"
#include <string.h>

enum {
    BG = 0x10131F,
    PANEL = 0x222838,
    EDGE = 0x343D50,
    TRACK = 0x414A5C,
    TEXT = 0xF3F6FC,
    MUTED = 0xB4BDCF,
    TEAL = 0x69E4CF,
    INK = 0x102F2C,
    LILAC = 0xB5A6FF,
    AMBER = 0xFFCA89,
    RED = 0xFF9EAA,
};

typedef enum { PAGE_SERVOS, PAGE_JARVIS, PAGE_COUNT } page_t;

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
    [SUGARAT_VOICE_BOOT]         = {"Starting Jarvis", "Getting voice ready", LILAC},
    [SUGARAT_VOICE_SETUP]        = {"Set up Wi-Fi", "Join Sugarat-Jarvis Wi-Fi\non your phone", AMBER},
    [SUGARAT_VOICE_JOINING]      = {"Joining Wi-Fi", "Connecting to your network", AMBER},
    [SUGARAT_VOICE_ACTIVATE]     = {"Activate device", "Enter this code at bot.sugarat.ai", AMBER},
    [SUGARAT_VOICE_BIND_AGENT]   = {"Choose an agent", "Bind a voice agent in Sugarat", AMBER},
    [SUGARAT_VOICE_READY]        = {"Say \"Jarvis\"", "Or tap to start a call", TEAL},
    [SUGARAT_VOICE_OPENING]      = {"Opening a call", "You can start speaking", LILAC},
    [SUGARAT_VOICE_IN_CALL]      = {"In conversation", "Speak to your voice agent", LILAC},
    [SUGARAT_VOICE_RECONNECTING] = {"Reconnecting", "Restoring your conversation", AMBER},
    [SUGARAT_VOICE_ENDING]       = {"Ending the call", "Returning to wake-word mode", LILAC},
    [SUGARAT_VOICE_ERROR]        = {"Voice unavailable", "Restart Sugarat to try again", RED},
    [SUGARAT_VOICE_RETRY]        = {"Call interrupted", "Tap or say \"Jarvis\" to retry", AMBER},
};

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *tabs[PAGE_COUNT];
static lv_obj_t *tab_text[PAGE_COUNT];
static lv_obj_t *voice_dot;
static lv_obj_t *receiver_status;
static lv_obj_t *targets[2];
static lv_obj_t *light_button;
static lv_obj_t *light_label;
static lv_obj_t *angle_caption;
static adjustment_t angle = {.action = SCREEN_SET_ANGLE};
static adjustment_t speed = {.action = SCREEN_SET_SPEED};
static lv_obj_t *voice_ring;
static lv_obj_t *voice_core;
static lv_obj_t *waveform[5];
static lv_obj_t *voice_title;
static lv_obj_t *voice_detail;
static lv_obj_t *voice_end;
static lv_obj_t *quick_voice;

static page_t active_page;
static screen_action_t action;
static screen_state_t displayed;
static bool has_state;

static lv_obj_t *surface(lv_obj_t *parent, int width, int height, int radius, uint32_t color)
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

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *button(lv_obj_t *parent, int width, int height, const char *text, lv_obj_t **label)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *title = text_label(object, text, &lv_font_montserrat_16, TEXT);
    lv_obj_center(title);
    if (label) {
        *label = title;
    }
    return object;
}

static void set_hidden(lv_obj_t *object, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_page(page_t page)
{
    active_page = page;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        const bool selected = i == page;
        set_hidden(pages[i], !selected);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(selected ? TRACK : PANEL), 0);
        lv_obj_set_style_text_color(tab_text[i], lv_color_hex(selected ? TEXT : MUTED), 0);
    }
}

static void tab_clicked(lv_event_t *event)
{
    show_page((page_t)(uintptr_t)lv_event_get_user_data(event));
}

static void swipe(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_LEFT && active_page == PAGE_SERVOS) {
        lv_indev_wait_release(input);
        show_page(PAGE_JARVIS);
    } else if (direction == LV_DIR_RIGHT && active_page == PAGE_JARVIS) {
        lv_indev_wait_release(input);
        show_page(PAGE_SERVOS);
    }
}

static void command_clicked(lv_event_t *event)
{
    const screen_action_t *command = lv_event_get_user_data(event);
    action = *command;
}

static void light_clicked(lv_event_t *event)
{
    action = (screen_action_t){
        .kind = SCREEN_SET_LIGHT,
        .value = displayed.light == SERVO_LIGHT_ON ? SERVO_LIGHT_OFF : SERVO_LIGHT_ON,
    };
}

static void mark_enabled(lv_obj_t *object, bool enabled)
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
    lv_obj_t *card = surface(page, width, 60, 20, PANEL);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(EDGE), 0);

    lv_obj_t *label = text_label(card, caption, &lv_font_montserrat_14, MUTED);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 13, 11);
    control->readout = text_label(card, "", &lv_font_montserrat_24, TEXT);
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
    receiver_status = text_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(receiver_status, LV_ALIGN_TOP_MID, 0, 89);

    lv_obj_t *selector = surface(page, 254, 54, 27, PANEL);
    lv_obj_align(selector, LV_ALIGN_TOP_MID, 0, 117);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *label;
        targets[i] = button(selector, 119, 46, i == 0 ? "Position A" : "Position B", &label);
        lv_obj_set_pos(targets[i], 4 + i * 127, 4);
        lv_obj_set_style_bg_color(targets[i], lv_color_hex(PANEL), 0);
        lv_obj_set_style_bg_color(targets[i], lv_color_hex(TEAL), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(targets[i], lv_color_hex(INK), LV_STATE_CHECKED);
        // Inherit the checked foreground from the button.
        lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
        lv_obj_set_style_text_color(targets[i], lv_color_hex(TEXT), 0);
        lv_obj_add_event_cb(targets[i], command_clicked, LV_EVENT_CLICKED, (void *)&target_commands[i]);
    }

    angle_caption = build_adjustment(page, &angle, 178, 274, "", TEAL);
    build_adjustment(page, &speed, 244, 246, "Travel time / 90°", LILAC);
    lv_slider_set_range(speed.slider, 0, (SERVO_SPEED_MAX - SERVO_SPEED_MIN) / SERVO_SPEED_INCREMENT);

    light_button = button(page, 94, 34, "Light", &light_label);
    lv_obj_align(light_button, LV_ALIGN_TOP_MID, 0, 306);
    lv_obj_add_event_cb(light_button, light_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *quick_label;
    quick_voice = button(page, 110, 34, "", &quick_label);
    lv_obj_set_style_text_font(quick_label, &lv_font_montserrat_14, 0);
    lv_obj_align(quick_voice, LV_ALIGN_TOP_MID, 50, 306);
    lv_obj_set_style_bg_color(quick_voice, lv_color_hex(PANEL), 0);
    lv_obj_add_event_cb(quick_voice, tab_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)PAGE_JARVIS);
}

static void build_voice(lv_obj_t *page)
{
    static const screen_action_t start_command = {.kind = SCREEN_START_CALL};
    static const screen_action_t end_command = {.kind = SCREEN_END_CALL};
    voice_ring = surface(page, 110, 110, LV_RADIUS_CIRCLE, BG);
    lv_obj_align(voice_ring, LV_ALIGN_TOP_MID, 0, 87);
    lv_obj_set_style_border_width(voice_ring, 1, 0);
    lv_obj_add_event_cb(voice_ring, command_clicked, LV_EVENT_CLICKED, (void *)&start_command);

    voice_core = surface(voice_ring, 88, 88, LV_RADIUS_CIRCLE, PANEL);
    lv_obj_center(voice_core);
    const int heights[] = {14, 26, 38, 26, 14};
    for (int i = 0; i < 5; ++i) {
        waveform[i] = surface(voice_core, 5, heights[i], LV_RADIUS_CIRCLE, LILAC);
        lv_obj_align(waveform[i], LV_ALIGN_CENTER, (i - 2) * 11, 0);
    }

    voice_title = text_label(page, "", &lv_font_montserrat_24, TEXT);
    lv_obj_align(voice_title, LV_ALIGN_TOP_MID, 0, 206);
    voice_detail = text_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_set_style_text_align(voice_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(voice_detail, 242);
    lv_label_set_long_mode(voice_detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(voice_detail, LV_ALIGN_TOP_MID, 0, 255);
    voice_end = button(page, 152, 42, "End call", NULL);
    lv_obj_align(voice_end, LV_ALIGN_TOP_MID, 0, 291);
    lv_obj_set_style_bg_color(voice_end, lv_color_hex(0x672839), 0);
    lv_obj_add_event_cb(voice_end, command_clicked, LV_EVENT_CLICKED, (void *)&end_command);
}

static void build_navigation(lv_obj_t *root)
{
    lv_obj_t *tray = surface(root, 208, 40, LV_RADIUS_CIRCLE, PANEL);
    lv_obj_align(tray, LV_ALIGN_TOP_MID, 0, 33);
    const char *names[] = {"Servos", "Jarvis"};
    for (int i = 0; i < PAGE_COUNT; ++i) {
        tabs[i] = button(tray, 98, 34, names[i], &tab_text[i]);
        lv_obj_set_pos(tabs[i], 3 + i * 104, 3);
        lv_obj_add_event_cb(tabs[i], tab_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_align(tab_text[PAGE_JARVIS], LV_ALIGN_CENTER, -4, 0);
    voice_dot = surface(tabs[PAGE_JARVIS], 6, 6, LV_RADIUS_CIRCLE, LILAC);
    lv_obj_align(voice_dot, LV_ALIGN_RIGHT_MID, -10, 0);
}

esp_err_t screen_init(bool touch_ready)
{
    esp_err_t err = screen_port_init(touch_ready);
    if (err != ESP_OK) {
        return err;
    }
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root, swipe, LV_EVENT_GESTURE, NULL);

    for (int i = 0; i < PAGE_COUNT; ++i) {
        pages[i] = surface(root, LV_PCT(100), LV_PCT(100), 0, BG);
        lv_obj_set_style_bg_opa(pages[i], LV_OPA_TRANSP, 0);
    }
    build_servos(pages[PAGE_SERVOS]);
    build_voice(pages[PAGE_JARVIS]);
    build_navigation(root);
    show_page(PAGE_SERVOS);
    return ESP_OK;
}

static void show_receiver(const screen_state_t *state)
{
    uint32_t tint = state->online ? TEAL : AMBER;
    if (!state->touch_ready) {
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
    mark_enabled(control->slider, enabled);
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
    if (voice->phase == SUGARAT_VOICE_READY || voice->phase == SUGARAT_VOICE_RETRY) {
        lv_obj_add_flag(voice_ring, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(voice_ring, LV_OBJ_FLAG_CLICKABLE);
    }
    const lv_color_t tint = lv_color_hex(copy->tint);
    lv_obj_set_style_bg_color(voice_dot, tint, 0);
    lv_obj_set_style_border_color(voice_ring, lv_color_mix(tint, lv_color_hex(BG), LV_OPA_60), 0);
    lv_obj_set_style_bg_color(voice_core, lv_color_mix(tint, lv_color_hex(PANEL), LV_OPA_30), 0);
    for (int i = 0; i < 5; ++i) {
        lv_obj_set_style_bg_color(waveform[i], tint, 0);
    }
    lv_obj_set_style_text_font(voice_title, activation ? &lv_font_montserrat_40 : &lv_font_montserrat_24, 0);
    lv_label_set_text(voice_title, activation ? voice->activation_code : copy->title);
    lv_label_set_text(voice_detail, copy->detail);
    set_hidden(voice_end, !voice->can_end);

    const char *shortcut = voice->can_end ? "Voice call" : activation ? "Activate Jarvis" :
                           voice->phase == SUGARAT_VOICE_SETUP ? "Set up Wi-Fi" : NULL;
    if (shortcut) {
        lv_label_set_text(lv_obj_get_child(quick_voice, 0), shortcut);
    }
    set_hidden(quick_voice, shortcut == NULL);
    lv_obj_align(light_button, LV_ALIGN_TOP_MID, shortcut ? -60 : 0, 306);
}

void screen_update(const screen_state_t *state)
{
    const bool enabled = state->online && !state->pending && state->touch_ready;
    if (!has_state || state->online != displayed.online || state->pending != displayed.pending ||
        state->touch_ready != displayed.touch_ready || state->receiver_channel != displayed.receiver_channel ||
        state->position != displayed.position ||
        state->moving != displayed.moving) {
        show_receiver(state);
        for (int i = 0; i < 2; ++i) {
            if (state->online && state->position == i) {
                lv_obj_add_state(targets[i], LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(targets[i], LV_STATE_CHECKED);
            }
            mark_enabled(targets[i], enabled);
        }
    }
    if (!has_state || state->angle_limit_deg != displayed.angle_limit_deg) {
        lv_label_set_text_fmt(angle_caption, "B angle (0-%u°)", (unsigned)state->angle_limit_deg);
        lv_slider_set_range(angle.slider, 0, state->angle_limit_deg / SERVO_ANGLE_INCREMENT);
    }
    if (!has_state || state->light != displayed.light || state->online != displayed.online ||
        state->pending != displayed.pending || state->touch_ready != displayed.touch_ready) {
        const bool known = state->online && state->light != SERVO_LIGHT_UNKNOWN;
        const bool on = known && state->light == SERVO_LIGHT_ON;
        lv_label_set_text(light_label, known ? (on ? "Light on" : "Light off") : "Light");
        lv_obj_set_style_bg_color(light_button, lv_color_hex(on ? AMBER : PANEL), 0);
        lv_obj_set_style_text_color(light_label, lv_color_hex(on ? INK : TEXT), 0);
        mark_enabled(light_button, enabled && known);
    }
    sync_adjustment(&angle, state->angle_deg / SERVO_ANGLE_INCREMENT, enabled, state->pending);
    sync_adjustment(&speed, (state->speed - SERVO_SPEED_MIN) / SERVO_SPEED_INCREMENT, enabled, state->pending);

    if (!has_state || state->voice.phase != displayed.voice.phase || state->voice.can_end != displayed.voice.can_end ||
        strcmp(state->voice.activation_code, displayed.voice.activation_code) != 0) {
        show_voice(&state->voice);
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
