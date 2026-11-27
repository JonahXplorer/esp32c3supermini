#include "ui_button.h"
#include "ui_theme.h"

/* Shared states adapted from the lvgl-ui action button template. */
static lv_style_t base, pressed, disabled, checked;
static lv_style_transition_dsc_t transition;
static bool initialized;

static void init_styles(void)
{
    if (initialized) return;
    initialized = true;
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR, LV_STYLE_TRANSLATE_Y, 0};
    lv_style_transition_dsc_init(&transition, props, lv_anim_path_ease_out, UI_PRESS_MS, 0, NULL);
    lv_style_init(&base);
    lv_style_set_bg_color(&base, UI_SURFACE);
    lv_style_set_bg_opa(&base, LV_OPA_COVER);
    lv_style_set_radius(&base, UI_RADIUS);
    lv_style_set_border_width(&base, 1);
    lv_style_set_border_color(&base, UI_BORDER);
    lv_style_set_pad_all(&base, 0);
    lv_style_set_transition(&base, &transition);
    lv_style_init(&pressed);
    lv_style_set_bg_color(&pressed, UI_SURFACE_PRESSED);
    lv_style_set_translate_y(&pressed, 1);
    lv_style_init(&disabled);
    lv_style_set_opa(&disabled, LV_OPA_40);
    lv_style_init(&checked);
    lv_style_set_border_color(&checked, UI_ACCENT);
}

lv_obj_t *ui_button_create(lv_obj_t *parent, int width, int height, int touch_margin)
{
    init_styles();
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, height);
    lv_obj_set_ext_click_area(button, touch_margin);
    lv_obj_add_style(button, &base, 0);
    lv_obj_add_style(button, &pressed, LV_STATE_PRESSED);
    lv_obj_add_style(button, &disabled, LV_STATE_DISABLED);
    lv_obj_add_style(button, &checked, LV_STATE_CHECKED);
    return button;
}
