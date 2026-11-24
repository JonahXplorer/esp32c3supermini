#include "screen_conversations.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "conversations.h"
#include "esp_attr.h"
#include "screen_widgets.h"

enum { LIST_WIDTH = 236, ROW_HEIGHT = 56, DELETE_FILL = 0x47232E };

typedef struct {
    lv_obj_t *choose;
    lv_obj_t *remove;
    lv_obj_t *title;
    lv_obj_t *badge;
} session_row_t;

static lv_obj_t *list, *status, *create, *previous, *next, *refresh;
static lv_obj_t *confirmation, *confirmation_title, *confirm_delete;
static session_row_t widgets[SUGARAT_CONVERSATIONS_PAGE_SIZE];
static EXT_RAM_BSS_ATTR conversations_result_t inbox, page_data;
static lv_font_t title_font;
static int agent_id;
static bool pending, needs_refresh, call_active;
static conversations_action_t requested_action;
static const char *notice;
static char active_id[sizeof(((sugarat_voice_view_t *)0)->conversation_id)];
static char delete_id[sizeof(active_id)];

static bool can_change(void)
{
    return agent_id > 0 && !pending && !call_active;
}

static void submit(conversations_action_t action, size_t offset, const char *id)
{
    if (!agent_id || pending || (action != CONVERSATIONS_LIST && call_active)) return;
    const esp_err_t err = conversations_request(agent_id, action, offset, id);
    pending = err == ESP_OK;
    requested_action = action;
    notice = pending ? NULL : "Sessions unavailable";
}

static const char *session_title(const agent_link_platform_conversation_t *session)
{
    return session->name[0] ? session->name : "Voice session";
}

static void choose_session(lv_event_t *event)
{
    const agent_link_platform_conversation_t *row = lv_event_get_user_data(event);
    if (strcmp(row->id, active_id) != 0) submit(CONVERSATIONS_SELECT, page_data.offset, row->id);
}

static void ask_delete(lv_event_t *event)
{
    if (!can_change()) return;
    const agent_link_platform_conversation_t *row = lv_event_get_user_data(event);
    strcpy(delete_id, row->id);
    lv_label_set_text(confirmation_title, session_title(row));
    screen_set_hidden(confirmation, false);
}

static void close_confirmation(lv_event_t *event)
{
    const bool confirmed = lv_event_get_user_data(event) != NULL;
    if (confirmed && can_change()) submit(CONVERSATIONS_DELETE, page_data.offset, delete_id);
    delete_id[0] = '\0';
    screen_set_hidden(confirmation, true);
}

static void list_button(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);
    if (button == create) {
        submit(CONVERSATIONS_CREATE, 0, NULL);
    } else if (button == previous && page_data.offset >= SUGARAT_CONVERSATIONS_PAGE_SIZE) {
        submit(CONVERSATIONS_LIST, page_data.offset - SUGARAT_CONVERSATIONS_PAGE_SIZE, NULL);
    } else if (button == next && page_data.has_more) {
        submit(CONVERSATIONS_LIST, page_data.offset + page_data.count, NULL);
    } else if (button == refresh) {
        submit(CONVERSATIONS_LIST, 0, NULL);
    }
}

static void rebuild_rows(void)
{
    lv_obj_clean(list);
    for (size_t i = 0; i < page_data.count; ++i) {
        agent_link_platform_conversation_t *session = &page_data.rows[i];
        session_row_t *row = &widgets[i];
        lv_obj_t *card = screen_surface(list, LIST_WIDTH, ROW_HEIGHT, 18, PANEL);

        row->choose = screen_button(card, LIST_WIDTH - 48, ROW_HEIGHT, session_title(session), &row->title);
        lv_obj_set_style_bg_color(row->choose, lv_color_hex(EDGE), 0);
        lv_obj_set_style_bg_opa(row->choose, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(row->choose, LV_OPA_TRANSP, LV_STATE_DISABLED);
        lv_obj_set_style_text_font(row->title, &title_font, 0);
        lv_obj_set_width(row->title, LIST_WIDTH - 64);
        lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
        lv_obj_align(row->title, LV_ALIGN_TOP_LEFT, 12, 9);
        row->badge = screen_label(row->choose, "", &lv_font_montserrat_14, MUTED);
        lv_obj_set_pos(row->badge, 12, 32);
        lv_obj_add_event_cb(row->choose, choose_session, LV_EVENT_CLICKED, session);

        lv_obj_t *icon;
        row->remove = screen_button(card, 44, 44, LV_SYMBOL_TRASH, &icon);
        lv_obj_set_style_bg_color(row->remove, lv_color_hex(DELETE_FILL), 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(RED), 0);
        lv_obj_align(row->remove, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(row->remove, ask_delete, LV_EVENT_CLICKED, session);
    }
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

static lv_obj_t *toolbar_button(lv_obj_t *page, const char *text, int x, int y, int size, uint32_t fill)
{
    lv_obj_t *control = screen_button(page, size, 42, text, NULL);
    lv_obj_align(control, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_bg_color(control, lv_color_hex(fill), 0);
    lv_obj_add_event_cb(control, list_button, LV_EVENT_CLICKED, NULL);
    return control;
}

void screen_conversations_build(lv_obj_t *page)
{
    title_font = lv_font_montserrat_16;
#if LV_FONT_SIMSUN_16_CJK
    title_font.fallback = &lv_font_simsun_16_cjk;
#endif
    create = toolbar_button(page, LV_SYMBOL_PLUS, 93, 39, 42, 0x23529A);
    list = screen_surface(page, LIST_WIDTH + 8, 174, 0, BG);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 91);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(list, lv_color_hex(MUTED), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_row(list, 7, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    status = screen_label(page, "", &lv_font_montserrat_14, MUTED);
    lv_obj_set_width(status, 230);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 267);
    previous = toolbar_button(page, LV_SYMBOL_LEFT, -58, 287, 52, PANEL);
    refresh = toolbar_button(page, LV_SYMBOL_REFRESH, 0, 287, 44, PANEL);
    next = toolbar_button(page, LV_SYMBOL_RIGHT, 58, 287, 52, PANEL);

    confirmation = screen_surface(page, SCREEN_SIZE, SCREEN_SIZE, 0, BG);
    lv_obj_add_flag(confirmation, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(confirmation, LV_OPA_80, 0);
    lv_obj_t *card = screen_surface(confirmation, 242, 196, 26, PANEL);
    lv_obj_center(card);
    lv_obj_t *heading = screen_label(card, "Delete session?", &lv_font_montserrat_24, TEXT);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 21);
    confirmation_title = screen_label(card, "", &title_font, TEXT);
    lv_obj_set_width(confirmation_title, 210);
    lv_label_set_long_mode(confirmation_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(confirmation_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(confirmation_title, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_t *detail = screen_label(card, "Removes it from Sugarat cloud", &lv_font_montserrat_14, MUTED);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_t *cancel = screen_button(card, 100, 44, "Cancel", NULL);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(EDGE), 0);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, -55, -20);
    lv_obj_add_event_cb(cancel, close_confirmation, LV_EVENT_CLICKED, NULL);
    lv_obj_t *delete_label;
    confirm_delete = screen_button(card, 100, 44, "Delete", &delete_label);
    lv_obj_set_style_bg_color(confirm_delete, lv_color_hex(DELETE_FILL), 0);
    lv_obj_set_style_text_color(delete_label, lv_color_hex(RED), 0);
    lv_obj_align(confirm_delete, LV_ALIGN_BOTTOM_MID, 55, -20);
    lv_obj_add_event_cb(confirm_delete, close_confirmation, LV_EVENT_CLICKED, confirmation);
    screen_set_hidden(confirmation, true);
}

void screen_conversations_open(bool open)
{
    needs_refresh = open;
    screen_set_hidden(confirmation, true);
}

static void update_result(void)
{
    if (!conversations_read(&inbox)) return;
    pending = false;
    if (inbox.agent_id != agent_id) return;

    notice = NULL;
    if (inbox.result == ESP_OK && inbox.has_page) {
        page_data = inbox;
        rebuild_rows();
    } else if (inbox.result != ESP_OK) {
        if (inbox.changed) {
            memset(&page_data, 0, sizeof(page_data));
            rebuild_rows();
            notice = "Saved. Tap refresh";
        } else {
            notice = inbox.result == ESP_ERR_NOT_ALLOWED ? "End the call to make changes" :
                     requested_action == CONVERSATIONS_CREATE ? "Couldn't create session" :
                     requested_action == CONVERSATIONS_DELETE ? "Couldn't delete session" :
                     requested_action == CONVERSATIONS_SELECT ? "Couldn't switch session" :
                     "Couldn't load sessions";
        }
    }
}

void screen_conversations_update(const sugarat_voice_view_t *voice)
{
    if (agent_id != voice->agent_id) {
        agent_id = voice->agent_id;
        memset(&page_data, 0, sizeof(page_data));
        notice = NULL;
        needs_refresh = true;
        screen_set_hidden(confirmation, true);
        rebuild_rows();
    }
    call_active = voice->can_end || voice->phase == SUGARAT_VOICE_OPENING || voice->phase == SUGARAT_VOICE_ENDING;
    memcpy(active_id, voice->conversation_id, sizeof(active_id));
    update_result();
    if (needs_refresh && agent_id > 0 && !pending) {
        needs_refresh = false;
        submit(CONVERSATIONS_LIST, 0, NULL);
    }

    const bool change_enabled = can_change();
    screen_mark_enabled(create, change_enabled);
    screen_mark_enabled(confirm_delete, change_enabled);
    screen_mark_enabled(previous, agent_id > 0 && !pending && page_data.offset > 0);
    screen_mark_enabled(next, agent_id > 0 && !pending && page_data.has_more);
    screen_mark_enabled(refresh, agent_id > 0 && !pending);
    for (size_t i = 0; i < page_data.count; ++i) {
        const agent_link_platform_conversation_t *session = &page_data.rows[i];
        const bool active = strcmp(session->id, active_id) == 0;
        const char *suffix = session->id + (strlen(session->id) > 8 ? strlen(session->id) - 8 : 0);
        session_row_t *row = &widgets[i];
        char label[32];
        if (active) {
            snprintf(label, sizeof(label), "%s  Current", LV_SYMBOL_OK);
        } else if (session->created_at[0] && session->name[0]) {
            snprintf(label, sizeof(label), "%.10s", session->created_at);
        } else {
            snprintf(label, sizeof(label), "%s", suffix);
        }
        if (strcmp(lv_label_get_text(row->badge), label) != 0) {
            lv_label_set_text(row->badge, label);
            lv_obj_set_style_text_color(row->badge, lv_color_hex(active ? TEAL : MUTED), 0);
        }
        screen_mark_enabled(row->choose, change_enabled && !active);
        screen_mark_enabled(row->remove, change_enabled);
    }
    const char *message = voice->phase == SUGARAT_VOICE_ERROR ? "Voice unavailable" :
                          !agent_id ? "Connect a voice agent first" :
                          pending ? (requested_action == CONVERSATIONS_CREATE ? "Creating session..." :
                                     requested_action == CONVERSATIONS_DELETE ? "Deleting session..." :
                                     requested_action == CONVERSATIONS_SELECT ? "Switching session..." : "Loading sessions...") :
                          notice ? notice : !page_data.count ? "No sessions. Tap + to create" : "";
    if (strcmp(lv_label_get_text(status), message) != 0) lv_label_set_text(status, message);
}
