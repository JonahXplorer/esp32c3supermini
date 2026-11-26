#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    HOME_IDLE, HOME_LISTENING, HOME_SPEAKING, HOME_OFFLINE, HOME_BUSY
} home_visual_state_t;
typedef enum {
    HOME_START_CALL, HOME_END_CALL, HOME_SETUP_WIFI,
    HOME_OPEN_SETTINGS, HOME_OPEN_SESSIONS, HOME_NO_ACTION
} home_action_t;

/* Presentation data only. Preview supplies mocks; the application owns guards. */
typedef struct {
    home_visual_state_t state;
    bool wifi_connected;
    bool battery_known;
    uint8_t battery_percent;
    uint8_t level;
    bool action_enabled;
    /* Optional application copy; NULL keeps the standalone design defaults. */
    const char *title;
    const char *hint;
    const char *action_label;
    home_action_t action;
    char activation_code[7];
} home_snapshot_t;
typedef void (*home_action_cb_t)(home_action_t action, void *context);
typedef struct home_screen home_screen_t;

home_screen_t *home_screen_create(lv_obj_t *parent, home_action_cb_t on_action, void *context);
void home_screen_update(home_screen_t *screen, const home_snapshot_t *snapshot);
void home_screen_destroy(home_screen_t *screen);
void home_screen_set_visible(home_screen_t *screen, bool visible);
lv_obj_t *home_screen_header(home_screen_t *screen);
/* Exposed for deterministic native rendering of real button states. */
lv_obj_t *home_screen_action_button(home_screen_t *screen);
