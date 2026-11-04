// main/classic/cs_ui.c -- Classic shell LVGL screens.
//
// Each screen is one full-screen RGB565 background from Flash (all static art
// and fixed text baked in by tools/classic/gen_assets.py) plus a few objects
// for what changes. One 250 ms lv_timer refreshes the status bar and the
// current screen, touching labels only when their text actually changes.
#include "cs_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cs_assets.h"
#include "cs_astro_art.h"
#include "cs_mines_art.h"
#include "lvgl.h"

LV_FONT_DECLARE(cs_font12);
LV_FONT_DECLARE(cs_font14);
LV_FONT_DECLARE(cs_font18);
LV_FONT_DECLARE(cs_num40);
LV_FONT_DECLARE(cs_num52);

#define SCR_W 240
#define SCR_H 320
#define TICK_MS 250

static cs_screen_t s_cur;
static lv_obj_t *s_scr;
static lv_timer_t *s_tick;
static uint32_t s_ticks;
static int s_home_sel = CS_HOME_RELEASE;
static int s_pomo_sel = CS_POMO_DEFAULT_PRESET;

// status bar
static lv_obj_t *s_name, *s_fan, *s_time, *s_batt;
static char s_time_txt[8];
static int s_batt_w = -1;
static int s_fan_state = -1;

// per-screen objects (only those of the current screen are non-NULL)
static lv_obj_t *s_o[24];
enum {   // indices into s_o, reused per screen
    LOCK_TIME = 0, LOCK_DATE,
    HOME_HALO = 0, HOME_WDAY, HOME_MDAY, HOME_TEMP, HOME_BADGE, HOME_BADGE_TXT, HOME_MSG_BADGE, HOME_MSG_TXT,
    SET_SWITCH = 0, SET_NET, SET_COMPANION, SET_IDLE_CARD, SET_IDLE_LBL, SET_IDLE_VAL, SET_RING,
    PICK_ROW0 = 0,   // 5 rows
    RUN_TIME = 0, RUN_SUB, RUN_BAR,
    WX_MSG = 0, WX_CITY, WX_DESC, WX_ICON, WX_TEMP, WX_AQI, WX_PILL, WX_PILL_TXT, WX_UPD,
    WX_ROW0,   // 4 per row x 3: day, icon, hi, lo
    CALL_STATUS = 0, CALL_OVL0 = 1, CALL_IMG0 = 7, CALL_DOT = 12, CALL_RING = 13,
    MSG_AREA = 0, MSG_MORE, MSG_STATUS, MSG_BTN0,   // 3 buttons
    NOTE_TITLE = 0, NOTE_PLUS, NOTE_HL, NOTE_TXT0, NOTE_CHK0 = NOTE_TXT0 + CS_NOTES_ROWS,   // list
    MEMO_PAGE = 0, MEMO_STATUS, MEMO_TITLE, MEMO_BTN0,                                                   // new (3 buttons)
    MN_TITLE = 0, MN_TITLE_SH, MN_SIZE, MN_BODY,                      // mines: chrome
    MN_SCORE, MN_SET_V0, MN_SET_V1, MN_SET_START, MN_SET_RING,        //   body: setup card
    MN_RING = MN_SCORE, MN_NOTE, MN_STATUS, MN_BTN0, MN_BTN1,         //   body: board
    MN_OVL = 12, MN_OVL_W0, MN_OVL_W1, MN_OVL_SENT,                   //   end screen
    HOME_BINGO = HOME_MSG_TXT + 1, HOME_BINGO_TXT,                    // home: Bingo check
    BG_TITLE_SH = 0, BG_TITLE, BG_LINE2, BG_ARC, BG_PCT, BG_BODY,     // bingo: chrome
    BG_BTN0, BG_BTN1, BG_BTN2,                                        //   bottom bar
    BG_MSG, BG_GRID,                                                  //   body: no card / the grid
    BG_OVL = 12, BG_OVL_W, BG_OVL_HEAD, BG_OVL_TXT,                   //   the BINGO! note
    AS_TITLE = 0, AS_TOP, AS_DICE, AS_CAP, AS_PILL, AS_ROWS,                  // astro dice / history
};
static uint32_t s_wx_version = 0xFFFFFFFF;
static int s_last_link = -1, s_last_mqtt = -1;

static lv_obj_t *s_alert;
static lv_timer_t *s_alert_timer;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// ---------------------------------------------------------------- helpers
static lv_obj_t *bare(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *image(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(img, x, y);
    return img;
}

static int label_top(const lv_font_t *f, int baseline) {
    return baseline - (f->line_height - f->base_line);
}

// Left-aligned label whose text sits on `baseline`.
static lv_obj_t *label_left(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int x, int baseline) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_label_set_text_static(l, "");
    lv_obj_set_pos(l, x, label_top(f, baseline));
    return l;
}

// Label centred on cx with the given box width.
static lv_obj_t *label_center(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int cx, int w,
                              int baseline) {
    lv_obj_t *l = label_left(parent, f, color, cx - w / 2, baseline);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    return l;
}

static void set_text(lv_obj_t *l, const char *t) {
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

static void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa, int r) {
    lv_obj_t *o = bare(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}

static const char *const WDAY_CN[7] = {"日", "一", "二", "三", "四", "五", "六"};

void cs_ui_format_date(const cs_view_t *v, char *buf, int cap) {
    if (!v->time_valid) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, (size_t)cap, "%d月%d日 星期%s", v->mon, v->mday, WDAY_CN[v->wday % 7]);
}

// ---------------------------------------------------------------- status bar
// The companion's name after the signal bars (cut off before the clock), then
// the Wi-Fi fan right behind it.
#define STATUS_NAME_MAX_W 44
static void status_build(void) {
    s_name = label_left(s_scr, &cs_font12, 0xFFFFFF, CS_NAME_X, CS_STATUS_BASE);
    lv_label_set_text_static(s_name, CS_COMPANION_NAME);
    lv_point_t sz;
    lv_text_get_size(&sz, CS_COMPANION_NAME, &cs_font12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > STATUS_NAME_MAX_W) {
        sz.x = STATUS_NAME_MAX_W;
        lv_obj_set_width(s_name, sz.x);
        lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_CLIP);
    }
    const int fan_x = CS_NAME_X + sz.x + 4;
    s_fan = image(s_scr, &cs_img_fan_off, fan_x > CS_FAN_X ? fan_x : CS_FAN_X, CS_FAN_Y);
    s_time = label_center(s_scr, &cs_font12, 0xFFFFFF, SCR_W / 2, 60, CS_STATUS_BASE);
    s_batt = rect(s_scr, CS_BAT_FILL_X, CS_BAT_FILL_Y, CS_BAT_FILL_W, CS_BAT_FILL_H, 0xFFFFFF,
                  LV_OPA_COVER, 0);
    s_time_txt[0] = '\0';
    s_batt_w = -1;
    s_fan_state = -1;
}

static void status_refresh(const cs_view_t *v) {
    if (!s_time) return;
    char t[8];
    if (v->time_valid) snprintf(t, sizeof t, "%02d:%02d", v->hour, v->min);
    else snprintf(t, sizeof t, "--:--");
    if (strcmp(t, s_time_txt) != 0) {
        snprintf(s_time_txt, sizeof s_time_txt, "%s", t);
        lv_label_set_text(s_time, t);
    }
    // Fan: grey when off, blinking while connecting, white when connected.
    int fan;
    if (v->link == CS_LINK_HOME || v->link == CS_LINK_HOTSPOT) fan = 1;
    else if (v->link == CS_LINK_CONNECTING) fan = (s_ticks / 2) & 1;
    else fan = 0;
    if (fan != s_fan_state) {
        s_fan_state = fan;
        lv_image_set_src(s_fan, fan ? &cs_img_fan_on : &cs_img_fan_off);
    }
    const int w = v->batt < 0 ? CS_BAT_FILL_W : (v->batt * CS_BAT_FILL_W + 50) / 100;
    if (w != s_batt_w) {
        s_batt_w = w;
        show(s_batt, w > 0);
        if (w > 0) lv_obj_set_width(s_batt, w);
        lv_obj_set_style_bg_color(s_batt, hex(v->batt >= 0 && v->batt < 20 ? 0xFF4A3A : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(s_batt, v->batt < 0 ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

// ---------------------------------------------------------------- lock
static void lock_build(void) {
    s_o[LOCK_TIME] = label_center(s_scr, &cs_num40, 0xFFFFFF, SCR_W / 2, 220, 58);
    s_o[LOCK_DATE] = label_center(s_scr, &cs_font14, 0xFFFFFF, SCR_W / 2, 220, 80);
}

static void lock_refresh(const cs_view_t *v) {
    char t[8], d[40];
    if (v->time_valid) snprintf(t, sizeof t, "%d:%02d", v->hour, v->min);
    else t[0] = '\0';
    cs_ui_format_date(v, d, sizeof d);
    set_text(s_o[LOCK_TIME], t);
    set_text(s_o[LOCK_DATE], d);
}

// ---------------------------------------------------------------- home
static void home_place_halo(void) {
    lv_obj_set_pos(s_o[HOME_HALO], CS_HOME_POS[s_home_sel][0] - CS_HALO_PAD,
                   CS_HOME_POS[s_home_sel][1] - CS_HALO_PAD);
}

// Placeholder icons (open nothing yet; the cursor skips them): a light veil.
static void home_dim_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = hex(0x000000);
    r.bg_opa = 48;
    r.radius = 9;
    for (int i = 0; i < CS_HOME_COUNT; i++) {
        if (!(CS_HOME_PLACEHOLDERS >> i & 1u)) continue;
        const lv_area_t a = {CS_HOME_POS[i][0], CS_HOME_POS[i][1], CS_HOME_POS[i][0] + CS_ICON - 1,
                             CS_HOME_POS[i][1] + CS_ICON - 1};
        lv_draw_rect(layer, &r, &a);
    }
}

static void home_build(void) {
    lv_obj_t *dim = bare(s_scr);
    lv_obj_set_size(dim, SCR_W, SCR_H);
    lv_obj_add_event_cb(dim, home_dim_cb, LV_EVENT_DRAW_MAIN, NULL);
    s_o[HOME_HALO] = image(s_scr, &cs_img_halo, 0, 0);
    home_place_halo();
    const int cx = CS_HOME_POS[CS_HOME_CALENDAR][0] + CS_ICON / 2;
    const int cy = CS_HOME_POS[CS_HOME_CALENDAR][1];
    s_o[HOME_WDAY] = label_center(s_scr, &cs_font12, 0xFFFFFF, cx, CS_ICON, cy + 11);
    s_o[HOME_MDAY] = label_center(s_scr, &cs_font18, 0x1E1E1E, cx, CS_ICON, cy + 35);
    const int wx = CS_HOME_POS[CS_HOME_WEATHER][0], wy = CS_HOME_POS[CS_HOME_WEATHER][1];
    s_o[HOME_TEMP] = label_center(s_scr, &cs_font12, 0xFFFFFF, wx + 28, 30, wy + 36);
    const int kx = CS_HOME_POS[CS_HOME_CLOCK][0], ky = CS_HOME_POS[CS_HOME_CLOCK][1];
    s_o[HOME_BADGE] = rect(s_scr, kx + 12, ky - 6, 38, 16, 0xE0302A, LV_OPA_COVER, 8);
    lv_obj_set_style_border_color(s_o[HOME_BADGE], hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_o[HOME_BADGE], 1, 0);
    s_o[HOME_BADGE_TXT] = label_center(s_o[HOME_BADGE], &cs_font12, 0xFFFFFF, 19, 38, 12);
    lv_obj_set_y(s_o[HOME_BADGE_TXT], label_top(&cs_font12, 12) - 1);
    // Unread messages: round red count on the Messages icon.
    const int mx = CS_HOME_POS[CS_HOME_MESSAGES][0], my = CS_HOME_POS[CS_HOME_MESSAGES][1];
    s_o[HOME_MSG_BADGE] = rect(s_scr, mx + CS_ICON - 14, my - 6, 20, 18, 0xE0302A, LV_OPA_COVER, 9);
    lv_obj_set_style_border_color(s_o[HOME_MSG_BADGE], hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_o[HOME_MSG_BADGE], 2, 0);
    s_o[HOME_MSG_TXT] = label_center(s_o[HOME_MSG_BADGE], &cs_font12, 0xFFFFFF, 10, 20, 13);
    lv_obj_set_y(s_o[HOME_MSG_TXT], label_top(&cs_font12, 13) - 2);
    show(s_o[HOME_MSG_BADGE], false);
    // Today's Bingo has a line: a gold check on the calendar (Bingo) icon.
    s_o[HOME_BINGO] = rect(s_scr, cx + CS_ICON / 2 - 14, cy - 6, 20, 18, 0xF5B91E, LV_OPA_COVER, 9);
    lv_obj_set_style_border_color(s_o[HOME_BINGO], hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_o[HOME_BINGO], 2, 0);
    s_o[HOME_BINGO_TXT] = label_center(s_o[HOME_BINGO], &cs_font12, 0xFFFFFF, 10, 20, 13);
    lv_obj_set_y(s_o[HOME_BINGO_TXT], label_top(&cs_font12, 13) - 2);
    lv_label_set_text_static(s_o[HOME_BINGO_TXT], "✓");
    show(s_o[HOME_BINGO], false);
}

static void home_refresh(const cs_view_t *v) {
    char b[24];
    if (v->time_valid) {
        snprintf(b, sizeof b, "周%s", WDAY_CN[v->wday % 7]);
        set_text(s_o[HOME_WDAY], b);
        snprintf(b, sizeof b, "%d", v->mday);
        set_text(s_o[HOME_MDAY], b);
    } else {
        set_text(s_o[HOME_WDAY], "");
        set_text(s_o[HOME_MDAY], "31");
    }
    if (v->wx_valid) snprintf(b, sizeof b, "%d°", v->wx_temp);
    else b[0] = '\0';
    set_text(s_o[HOME_TEMP], b);
    show(s_o[HOME_BINGO], v->bingo_done);
    show(s_o[HOME_MSG_BADGE], v->msgs_unread > 0);
    if (v->msgs_unread > 0) {
        snprintf(b, sizeof b, "%d", v->msgs_unread > 99 ? 99 : v->msgs_unread);
        set_text(s_o[HOME_MSG_TXT], b);
    }
    show(s_o[HOME_BADGE], v->pomo_running);
    if (v->pomo_running) {
        const uint32_t s = v->pomo_left_s;
        // Compact badge: minutes left (rounded up) under an hour, else H:MM.
        if (s >= 3600) snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
        else cs_pomo_format(s, false, b, sizeof b);
        set_text(s_o[HOME_BADGE_TXT], b);
    }
}

void cs_ui_home_select(int sel) {
    s_home_sel = sel;
    if (s_cur == CS_SCR_HOME && s_o[HOME_HALO]) home_place_halo();
}

// ---------------------------------------------------------------- settings
// Two rows: the Wi-Fi switch (card in the background art) and auto-lock (card
// drawn here, below the hint text). UP / DOWN move the blue focus ring.
#define SET_CARD_Y 238
static int s_set_focus, s_set_idle = CS_IDLE_DEFAULT;
static const char *const IDLE_TXT[CS_IDLE_CHOICES] = {"30 秒", "1 分钟", "2 分钟", "5 分钟", "永不"};

static void settings_apply(void) {
    lv_obj_set_y(s_o[SET_RING], s_set_focus ? SET_CARD_Y - 2 : 60);
    set_text(s_o[SET_IDLE_VAL], IDLE_TXT[s_set_idle < CS_IDLE_CHOICES ? s_set_idle : CS_IDLE_DEFAULT]);
}

static void settings_build(void) {
    s_o[SET_SWITCH] = image(s_scr, &cs_img_sw_off, CS_SW_X, CS_SW_Y);
    s_o[SET_NET] = label_left(s_scr, &cs_font14, 0x385487, 140, 140);
    s_o[SET_COMPANION] = label_left(s_scr, &cs_font14, 0x385487, 140, 172);
    lv_obj_t *name = label_left(s_scr, &cs_font14, 0x141418, 22, 172);   // the row's title
    lv_obj_set_width(name, 112);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text_static(name, CS_COMPANION_NAME);
    lv_obj_t *card = rect(s_scr, 10, SET_CARD_Y, 220, 40, 0xFFFFFF, LV_OPA_COVER, 9);
    lv_obj_set_style_border_color(card, hex(0xAAAEB6), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    s_o[SET_IDLE_CARD] = card;
    s_o[SET_IDLE_LBL] = label_left(s_scr, &cs_font14, 0x141418, 22, SET_CARD_Y + 25);
    lv_label_set_text_static(s_o[SET_IDLE_LBL], "自动锁屏");
    lv_obj_t *val = label_left(s_scr, &cs_font14, 0x385487, 120, SET_CARD_Y + 25);
    lv_obj_set_width(val, 98);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    s_o[SET_IDLE_VAL] = val;
    lv_obj_t *ring = rect(s_scr, 8, 60, 224, 44, 0x3478F6, LV_OPA_TRANSP, 11);
    lv_obj_set_style_border_color(ring, hex(0x3478F6), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    s_o[SET_RING] = ring;
    settings_apply();
    s_last_link = s_last_mqtt = -1;
}

void cs_ui_settings(int focus, int idle_choice) {
    s_set_focus = focus ? 1 : 0;
    s_set_idle = idle_choice;
    if (s_cur == CS_SCR_SETTINGS) settings_apply();
}

static void settings_refresh(const cs_view_t *v) {
    if (v->link == s_last_link && (int)v->mqtt == s_last_mqtt) return;
    s_last_link = v->link;
    s_last_mqtt = v->mqtt;
    lv_image_set_src(s_o[SET_SWITCH], v->link == CS_LINK_OFF ? &cs_img_sw_off : &cs_img_sw_on);
    static const char *const NET[] = {"关闭", "未配置网络", "连接中…", "家里 ✓", "热点 ✓"};
    set_text(s_o[SET_NET], NET[v->link < 5 ? v->link : 0]);
    const char *comp = v->mqtt ? "已连上"
                     : (v->link == CS_LINK_HOME || v->link == CS_LINK_HOTSPOT) ? "连接中…" : "未连上";
    set_text(s_o[SET_COMPANION], comp);
}

// ---------------------------------------------------------------- pomodoro
static void pick_build(void) {
    for (int r = 0; r < 5; r++) {
        const bool mid = r == 2;
        const lv_font_t *f = mid ? &cs_font18 : &cs_font14;
        const int cy = CS_POMO_CY + (r - 2) * CS_POMO_ROW;
        const int base = cy + (mid ? 7 : 5);
        s_o[PICK_ROW0 + r] = label_center(s_scr, f, 0x141418, SCR_W / 2, 200, base);
        if (!mid) lv_obj_set_style_text_opa(s_o[PICK_ROW0 + r], (r == 1 || r == 3) ? 150 : 90, 0);
    }
}

static void pick_refresh(void) {
    for (int r = 0; r < 5; r++) {
        const int idx = s_pomo_sel + r - 2;
        char b[24];
        if (idx >= 0 && idx < CS_POMO_PRESET_COUNT) snprintf(b, sizeof b, "%u 分钟", CS_POMO_PRESET_MIN[idx]);
        else b[0] = '\0';
        set_text(s_o[PICK_ROW0 + r], b);
    }
}

void cs_ui_pomo_pick(int preset) {
    s_pomo_sel = preset;
    if (s_cur == CS_SCR_POMO_PICK) pick_refresh();
}

static void run_build(void) {
    s_o[RUN_TIME] = label_center(s_scr, &cs_num52, 0xFFFFFF, SCR_W / 2, 230, 142);
    s_o[RUN_SUB] = label_center(s_scr, &cs_font14, 0xBEC2CA, SCR_W / 2, 220, 168);
    s_o[RUN_BAR] = rect(s_scr, CS_POMO_BAR_X, CS_POMO_BAR_Y, 0, CS_POMO_BAR_H, 0xF05A46, LV_OPA_COVER, 3);
    show(s_o[RUN_BAR], false);
}

static void run_refresh(const cs_view_t *v) {
    char b[40];
    cs_pomo_format(v->pomo_left_s, v->pomo_long, b, sizeof b);
    set_text(s_o[RUN_TIME], b);
    snprintf(b, sizeof b, "%u 分钟 · 专注中", (unsigned)(v->pomo_total_s / 60));
    set_text(s_o[RUN_SUB], b);
    const int w = CS_POMO_BAR_W * v->pomo_permille / 1000;
    show(s_o[RUN_BAR], w >= 2);
    if (w >= 2 && lv_obj_get_width(s_o[RUN_BAR]) != w) lv_obj_set_width(s_o[RUN_BAR], w);
}

// ---------------------------------------------------------------- weather
static void wx_build(void) {
    s_o[WX_MSG] = label_center(s_scr, &cs_font14, 0xFFFFFF, SCR_W / 2, 190, 110);
    lv_label_set_long_mode(s_o[WX_MSG], LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(s_o[WX_MSG], 6, 0);
    s_o[WX_CITY] = label_left(s_scr, &cs_font18, 0xFFFFFF, 22, 51);
    s_o[WX_DESC] = label_left(s_scr, &cs_font12, 0xE6EEFA, 22, 70);
    lv_obj_set_width(s_o[WX_DESC], 130);
    lv_label_set_long_mode(s_o[WX_DESC], LV_LABEL_LONG_MODE_DOTS);
    s_o[WX_ICON] = image(s_scr, CS_IMG_WX_BIG[CS_WX_CLOUDY], 146, 28);
    s_o[WX_TEMP] = label_center(s_scr, &cs_num40, 0xFFFFFF, 74, 120, 132);
    s_o[WX_AQI] = label_left(s_scr, &cs_font18, 0xFFFFFF, 32, 188);
    s_o[WX_PILL] = rect(s_scr, 136, 166, 72, 20, 0xF5D23C, LV_OPA_COVER, 10);
    s_o[WX_PILL_TXT] = label_center(s_o[WX_PILL], &cs_font12, 0x5A3C00, 36, 72, 15);
    for (int k = 0; k < CS_WX_DAYS; k++) {
        const int base = CS_WX_ROW0 + k * CS_WX_ROW_DY;
        s_o[WX_ROW0 + k * 4 + 0] = label_left(s_scr, &cs_font14, 0xFFFFFF, 26, base);
        s_o[WX_ROW0 + k * 4 + 1] = image(s_scr, CS_IMG_WX_SMALL[CS_WX_SUNNY], 120 - CS_WX_SMALL_W / 2,
                                         base - 5 - CS_WX_SMALL_H / 2);
        s_o[WX_ROW0 + k * 4 + 2] = label_left(s_scr, &cs_font14, 0xFFFFFF, 166, base);
        s_o[WX_ROW0 + k * 4 + 3] = label_left(s_scr, &cs_font14, 0xC8D7F0, 196, base);
    }
    s_o[WX_UPD] = label_center(s_scr, &cs_font12, 0xD2DEF0, SCR_W / 2, 200, 303);
    s_wx_version = 0xFFFFFFFF;
    s_last_link = -1;
}

static void wx_refresh(const cs_view_t *v) {
    const bool offline = v->link == CS_LINK_OFF || v->link == CS_LINK_NO_CREDS;
    if (v->wx_version == s_wx_version && (v->wx_valid || (int)offline == s_last_link)) return;
    s_wx_version = v->wx_version;
    s_last_link = offline;
    cs_weather_t w;
    const bool ok = cs_view_weather(&w) && w.valid;
    show(s_o[WX_MSG], !ok);
    for (int i = WX_CITY; i < WX_ROW0 + 4 * CS_WX_DAYS; i++) show(s_o[i], ok);
    if (!ok) {
        set_text(s_o[WX_MSG], offline ? "打开设置里的 Wi-Fi\n就能看到上海天气" : "正在获取…");
        return;
    }
    char b[96];
    set_text(s_o[WX_CITY], w.city[0] ? w.city : "上海");
    snprintf(b, sizeof b, "%s  最高 %d° 最低 %d°", w.cond, w.hi, w.lo);
    set_text(s_o[WX_DESC], b);
    lv_image_set_src(s_o[WX_ICON], CS_IMG_WX_BIG[cs_wx_icon_from_name(w.icon)]);
    snprintf(b, sizeof b, "%d°", w.temp);
    set_text(s_o[WX_TEMP], b);
    if (w.aqi >= 0) snprintf(b, sizeof b, "AQI %d", w.aqi);
    else snprintf(b, sizeof b, "AQI --");
    set_text(s_o[WX_AQI], b);
    const cs_aqi_style_t st = cs_aqi_style(w.aqi_level, w.aqi);
    lv_obj_set_style_bg_color(s_o[WX_PILL], hex(st.bg), 0);
    lv_obj_set_style_text_color(s_o[WX_PILL_TXT], hex(st.text), 0);
    set_text(s_o[WX_PILL_TXT], w.aqi_level[0] ? w.aqi_level : "--");
    show(s_o[WX_PILL], w.aqi >= 0 || w.aqi_level[0]);
    for (int k = 0; k < CS_WX_DAYS; k++) {
        const bool have = k < w.ndays;
        for (int j = 0; j < 4; j++) show(s_o[WX_ROW0 + k * 4 + j], have);
        if (!have) continue;
        set_text(s_o[WX_ROW0 + k * 4 + 0], w.days[k].d);
        lv_image_set_src(s_o[WX_ROW0 + k * 4 + 1], CS_IMG_WX_SMALL[cs_wx_icon_from_name(w.days[k].icon)]);
        snprintf(b, sizeof b, "%d°", w.days[k].hi);
        set_text(s_o[WX_ROW0 + k * 4 + 2], b);
        snprintf(b, sizeof b, "%d°", w.days[k].lo);
        set_text(s_o[WX_ROW0 + k * 4 + 3], b);
    }
    if (w.updated[0]) snprintf(b, sizeof b, "%s 更新", w.updated);
    else b[0] = '\0';
    set_text(s_o[WX_UPD], b);
}

// ---------------------------------------------------------------- call
static void cell_box(int i, int *x, int *y, int *w, int *h) {
    const int pw = CS_CALL_PX1 - CS_CALL_PX0, ph = CS_CALL_PY1 - CS_CALL_PY0;
    *x = CS_CALL_PX0 + pw * (i % 3) / 3;
    *y = CS_CALL_PY0 + ph * (i / 3) / 2;
    *w = pw / 3;
    *h = ph / 2;
}

static void call_build(void) {
    lv_obj_t *t = label_center(s_scr, &cs_font18, 0x000000, SCR_W / 2, 200, 41);   // title + shadow
    lv_obj_set_style_text_opa(t, 160, 0);
    lv_label_set_text_static(t, CS_COMPANION_NAME);
    t = label_center(s_scr, &cs_font18, 0xFFFFFF, SCR_W / 2, 200, 40);
    lv_label_set_text_static(t, CS_COMPANION_NAME);
    s_o[CALL_STATUS] = label_center(s_scr, &cs_font12, 0xE6ECF4, SCR_W / 2, 200, 62);
    // Long captions from the companion scroll instead of being cut off.
    lv_label_set_long_mode(s_o[CALL_STATUS], LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    for (int i = 0; i < 6; i++) {
        int x, y, w, h;
        cell_box(i, &x, &y, &w, &h);
        s_o[CALL_OVL0 + i] = rect(s_scr, x + 2, y + 2, w - 3, h - 3, 0x78BEFF, LV_OPA_TRANSP, i == 0 ? 6 : 0);
    }
    for (int i = 0; i < 5; i++) {
        int x, y, w, h;
        cell_box(i, &x, &y, &w, &h);
        s_o[CALL_IMG0 + i] = image(s_scr, CS_IMG_CELL[i], x + (w - CS_CELL_W) / 2, y + (h - CS_CELL_H) / 2);
    }
    int x, y, w, h;
    cell_box(0, &x, &y, &w, &h);
    s_o[CALL_DOT] = rect(s_scr, x + w / 2 + 8, y + 7, 6, 6, 0xFF3C32, LV_OPA_COVER, 3);
    show(s_o[CALL_DOT], false);
    s_o[CALL_RING] = rect(s_scr, CS_CALL_END_X0 - 3, CS_CALL_END_Y0 - 3, CS_CALL_END_X1 - CS_CALL_END_X0 + 6,
                          CS_CALL_END_Y1 - CS_CALL_END_Y0 + 6, 0, LV_OPA_TRANSP, 11);
    lv_obj_set_style_border_color(s_o[CALL_RING], hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_o[CALL_RING], 2, 0);
    lv_obj_set_style_border_opa(s_o[CALL_RING], LV_OPA_80, 0);
    show(s_o[CALL_RING], false);
}

void cs_ui_call_update(const cs_call_view_t *cv) {
    if (s_cur != CS_SCR_CALL) return;
    set_text(s_o[CALL_STATUS], cv->status);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *o = s_o[CALL_OVL0 + i];
        uint32_t color = 0x78BEFF;
        lv_opa_t opa = LV_OPA_TRANSP;
        const bool on = (i == CS_CALL_MUTE && cv->muted) || (i == CS_CALL_SPEAKER && cv->speaker);
        if (i == CS_CALL_TALK && cv->recording) { color = 0xDC3C32; opa = 160; }
        else if (i == cv->cursor && cv->cursor != CS_CALL_END) { opa = cv->enabled ? 110 : 60; }
        else if (on) { color = 0xFFFFFF; opa = 45; }
        lv_obj_set_style_bg_color(o, hex(color), 0);
        lv_obj_set_style_bg_opa(o, opa, 0);
    }
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_image_opa(s_o[CALL_IMG0 + i], cv->enabled ? LV_OPA_COVER : 90, 0);
    }
    show(s_o[CALL_DOT], cv->recording);
    show(s_o[CALL_RING], cv->cursor == CS_CALL_END);
}

// ---------------------------------------------------------------- core
static void tick_cb(lv_timer_t *t) {
    (void)t;
    s_ticks++;
    if (s_cur == CS_SCR_NONE) return;
    cs_view_t v;
    cs_view_get(&v);
    status_refresh(&v);
    switch (s_cur) {
    case CS_SCR_LOCK: lock_refresh(&v); break;
    case CS_SCR_HOME: home_refresh(&v); break;
    case CS_SCR_SETTINGS: settings_refresh(&v); break;
    case CS_SCR_POMO_RUN: run_refresh(&v); break;
    case CS_SCR_WEATHER: wx_refresh(&v); break;
    default: break;
    }
}

void cs_ui_init(void) {
    if (!s_tick) s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
}

// ---------------------------------------------------------------- messages
// Early-2010s skeuomorphic Messages: blue-grey backdrop, glossy grey nav bar with the contact,
// the companion's bubbles left in white-grey, the owner's right in SMS green, a small
// tail at the lower corner, time stamps between groups (> 15 min apart).
// Drawn entirely with LVGL (no background art). The companion's text is ASCII.
#define MSG_NAV_H 32
#define MSG_AREA_Y (CS_STATUS_H + MSG_NAV_H)
#define MSG_BAR_Y 254
#define MSG_BUBBLE_W 168
static uint32_t s_msgs_gen = 0xFFFFFFFF;
static const char *const MON_EN[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const MSG_BTN_TXT[3] = {"录音", "发送", "删除"};

static void grad(lv_obj_t *o, uint32_t top, uint32_t bottom) {
    lv_obj_set_style_bg_color(o, hex(top), 0);
    lv_obj_set_style_bg_grad_color(o, hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static void border(lv_obj_t *o, uint32_t color, lv_opa_t opa) {
    lv_obj_set_style_border_color(o, hex(color), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
}

// The status bar other screens have in their background art (the name, fan,
// clock and battery fill come from status_build()).
static void msgs_status_art(void) {
    rect(s_scr, 0, 0, SCR_W, CS_STATUS_H, 0x000000, LV_OPA_COVER, 0);
    for (int i = 0; i < 5; i++) {
        const int bh = 3 + i * 2;
        rect(s_scr, 18 + i * 4, CS_STATUS_BASE - bh, 3, bh, 0xFFFFFF, i < 4 ? 235 : 70, 0);
    }
    lv_obj_t *b = rect(s_scr, 194, 3, 21, 9, 0x000000, LV_OPA_TRANSP, 2);
    border(b, 0xFFFFFF, 230);
    rect(s_scr, 215, 6, 2, 3, 0xFFFFFF, 230, 0);
}

static void msgs_build(void) {
    grad(s_scr, 0xE1E6F2, 0xD5DCEB);          // thread backdrop ~#dbe1ef
    msgs_status_art();
    // Classic SMS steel blue, glossy: #bbc8d9 -> #8b9fb8 -> #5f799a (two halves:
    // a gradient here takes two colours).
    lv_obj_t *nav = rect(s_scr, 0, CS_STATUS_H, SCR_W, MSG_NAV_H / 2, 0, LV_OPA_COVER, 0);
    grad(nav, 0xBBC8D9, 0x8B9FB8);
    nav = rect(s_scr, 0, CS_STATUS_H + MSG_NAV_H / 2, SCR_W, MSG_NAV_H - MSG_NAV_H / 2, 0, LV_OPA_COVER, 0);
    grad(nav, 0x8B9FB8, 0x5F799A);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
    border(nav, 0x3F5677, LV_OPA_COVER);
    // White title with a dark 1 px shadow.
    lv_obj_t *shadow = label_center(s_scr, &cs_font18, 0x3B4E6A, SCR_W / 2, 120, CS_STATUS_H + 21);
    lv_label_set_text_static(shadow, CS_COMPANION_NAME);
    lv_obj_t *title = label_center(s_scr, &cs_font18, 0xFFFFFF, SCR_W / 2, 120, CS_STATUS_H + 22);
    lv_label_set_text_static(title, CS_COMPANION_NAME);
    lv_obj_t *area = bare(s_scr);   // children placed by msgs_layout, clipped here
    lv_obj_set_pos(area, 0, MSG_AREA_Y);
    lv_obj_set_size(area, SCR_W, MSG_BAR_Y - MSG_AREA_Y);
    s_o[MSG_AREA] = area;
    // "···" pill: older messages above (UP).
    lv_obj_t *more = rect(s_scr, SCR_W / 2 - 18, MSG_AREA_Y + 3, 36, 14, 0x7C8696, 150, 7);
    lv_obj_t *dots = label_center(more, &cs_font12, 0xFFFFFF, 18, 36, 11);
    lv_label_set_text_static(dots, "···");
    s_o[MSG_MORE] = more;
    lv_obj_t *bar = rect(s_scr, 0, MSG_BAR_Y, SCR_W, SCR_H - MSG_BAR_Y, 0, LV_OPA_COVER, 0);
    grad(bar, 0xEEF1F5, 0xB4BFCE);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    border(bar, 0x8893A6, LV_OPA_COVER);
    s_o[MSG_STATUS] = label_center(s_scr, &cs_font12, 0x33415A, SCR_W / 2, 196, MSG_BAR_Y + 15);
    lv_label_set_long_mode(s_o[MSG_STATUS], LV_LABEL_LONG_MODE_DOTS);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = rect(s_scr, 22 + i * 68, MSG_BAR_Y + 22, 60, 28, 0xFFFFFF, LV_OPA_COVER, 7);
        lv_obj_t *l = label_center(btn, &cs_font14, 0x2A3445, 30, 60, 19);
        lv_label_set_text_static(l, MSG_BTN_TXT[i]);
        s_o[MSG_BTN0 + i] = btn;
    }
    s_msgs_gen = 0xFFFFFFFF;
}

// Shared styles: the 32 KB LVGL pool cannot hold per-object style copies for
// a screen of bubbles (local styles ran it out in the host render).
static lv_style_t s_st_bub[2], s_st_tail[2], s_st_txt[2], s_st_time;
static bool s_st_ready;

// 10 x 14 microphone, alpha only, drawn in the outgoing text colour.
static const uint8_t MIC_A8[14][10] = {
    {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF},
    {0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF},
    {0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF},
    {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
    {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00},
};
static const lv_image_dsc_t MIC_IMG = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = 10, .h = 14, .stride = 10},
    .data_size = sizeof MIC_A8,
    .data = &MIC_A8[0][0],
};

static void msgs_styles(void) {
    if (s_st_ready) return;
    s_st_ready = true;
    // Outgoing: glossy lime around #8bcf3c (classic SMS style).
    static const uint32_t TOP[2] = {0xFFFFFF, 0xD3F2A6}, LOW[2] = {0xDCDFE4, 0x8BCF3C};
    static const uint32_t EDGE[2] = {0xA4AAB3, 0x5E9A22}, TEXT[2] = {0x141414, 0x1E3A0C};
    for (int o = 0; o < 2; o++) {
        lv_style_t *b = &s_st_bub[o];
        lv_style_init(b);
        lv_style_set_radius(b, 13);
        lv_style_set_pad_hor(b, 10);
        lv_style_set_pad_ver(b, 5);
        lv_style_set_pad_column(b, 6);
        lv_style_set_bg_opa(b, LV_OPA_COVER);
        lv_style_set_bg_color(b, hex(TOP[o]));
        lv_style_set_bg_grad_color(b, hex(LOW[o]));
        lv_style_set_bg_grad_dir(b, LV_GRAD_DIR_VER);
        lv_style_set_bg_main_stop(b, 40);
        lv_style_set_border_color(b, hex(EDGE[o]));
        lv_style_set_border_width(b, 1);
        lv_style_t *t = &s_st_tail[o];
        lv_style_init(t);
        lv_style_set_radius(t, LV_RADIUS_CIRCLE);
        lv_style_set_bg_opa(t, LV_OPA_COVER);
        lv_style_set_bg_color(t, hex(LOW[o]));
        lv_style_set_border_color(t, hex(EDGE[o]));
        lv_style_set_border_width(t, 1);
        lv_style_t *x = &s_st_txt[o];
        lv_style_init(x);
        lv_style_set_text_font(x, &cs_font14);
        lv_style_set_text_color(x, hex(TEXT[o]));
        lv_style_set_max_width(x, MSG_BUBBLE_W);
        lv_style_set_image_recolor(x, hex(TEXT[o]));
        lv_style_set_image_recolor_opa(x, LV_OPA_COVER);
    }
    lv_style_init(&s_st_time);
    lv_style_set_text_font(&s_st_time, &cs_font12);
    lv_style_set_text_color(&s_st_time, hex(0x6E7A8C));
    lv_style_set_text_align(&s_st_time, LV_TEXT_ALIGN_CENTER);
}

static lv_obj_t *msgs_time_label(lv_obj_t *area, uint32_t ts) {
    const time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);
    char b[24];
    snprintf(b, sizeof b, "%s %d  %02d:%02d", MON_EN[tm.tm_mon % 12], tm.tm_mday, tm.tm_hour, tm.tm_min);
    lv_obj_t *l = lv_label_create(area);
    lv_obj_add_style(l, &s_st_time, 0);
    lv_obj_set_width(l, SCR_W);
    lv_label_set_text(l, b);
    return l;
}

// One bubble (content sized, placed by the caller): text, or mic + length.
static lv_obj_t *msgs_bubble(lv_obj_t *area, const cs_msg_t *m) {
    const int o = m->out ? 1 : 0;
    lv_obj_t *b = bare(area);
    lv_obj_add_style(b, &s_st_bub[o], 0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (o) {
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *mic = lv_image_create(b);
        lv_obj_add_style(mic, &s_st_txt[1], 0);
        lv_image_set_src(mic, &MIC_IMG);
        char d[12];
        snprintf(d, sizeof d, "%u:%02u", (unsigned)(m->dur_s / 60), (unsigned)(m->dur_s % 60));
        lv_obj_t *l = lv_label_create(b);
        lv_obj_add_style(l, &s_st_txt[1], 0);
        lv_label_set_text(l, d);
    } else {
        lv_obj_t *l = lv_label_create(b);
        lv_obj_add_style(l, &s_st_txt[0], 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, m->text);
    }
    return b;
}

// Bottom-up: newest at the bottom; stop when the area is full (only bubbles
// that show are created). Returns true when older ones did not fit.
static bool msgs_layout(lv_obj_t *area, const cs_msgs_view_t *v) {
    int y = MSG_BAR_Y - MSG_AREA_Y - 8;          // bottom edge for the next item
    for (int i = v->n - 1; i >= 0; i--) {
        const cs_msg_t *m = &v->msgs[i];
        lv_obj_t *b = msgs_bubble(area, m);
        lv_obj_update_layout(b);
        const int w = lv_obj_get_width(b), h = lv_obj_get_height(b);
        if (y - h < 0 && i != v->n - 1) {        // the newest always shows (its top may clip)
            lv_obj_delete(b);
            return true;
        }
        y -= h;
        const int x = m->out ? SCR_W - 14 - w : 14;
        lv_obj_set_pos(b, x, y);
        // Tail: a small round curl at the lower corner on the sender's side.
        lv_obj_t *t = bare(area);
        lv_obj_add_style(t, &s_st_tail[m->out ? 1 : 0], 0);
        lv_obj_set_size(t, 10, 10);
        lv_obj_set_pos(t, m->out ? x + w - 6 : x - 4, y + h - 10);
        y -= 7;
        const bool stamp = m->ts && (i == 0 || !v->msgs[i - 1].ts || m->ts > v->msgs[i - 1].ts + 900);
        if (stamp && y > 14) {
            lv_obj_t *l = msgs_time_label(area, m->ts);
            lv_obj_update_layout(l);
            y -= lv_obj_get_height(l);
            lv_obj_set_pos(l, 0, y);
            y -= 5;
        }
        if (y < 0) return i > 0;
    }
    return false;
}

static void msgs_bar(const cs_msgs_view_t *v) {
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = s_o[MSG_BTN0 + i];
        lv_obj_t *l = lv_obj_get_child(btn, 0);
        const bool enabled = v->enabled && (i == 0 ? v->rec != 2 : v->rec == 2);
        const bool focused = enabled && i == v->bar;
        uint32_t top = 0xFDFEFF, low = 0xCDD4DE, text = 0x2A3445, edge = 0x8D97A6;
        if (i == 0 && v->rec == 1) {
            top = 0xF79A90; low = 0xC9302A; text = 0xFFFFFF; edge = 0x8E1F1A;
        } else if (focused) {
            top = 0x9CC4F6; low = 0x2E6AD0; text = 0xFFFFFF; edge = 0x1E4C9C;
        }
        grad(btn, top, low);
        border(btn, edge, LV_OPA_COVER);
        lv_obj_set_style_text_color(l, hex(text), 0);
        lv_obj_set_style_opa(btn, enabled || (i == 0 && v->rec == 1) ? LV_OPA_COVER : 110, 0);
    }
}

void cs_ui_msgs_update(const cs_msgs_view_t *v) {
    if (s_cur != CS_SCR_MSGS || !s_o[MSG_AREA]) return;
    if (v->gen != s_msgs_gen) {
        s_msgs_gen = v->gen;
        lv_obj_t *area = s_o[MSG_AREA];
        lv_obj_clean(area);
        msgs_styles();
        const bool cut = msgs_layout(area, v);
        show(s_o[MSG_MORE], v->more_above || cut);
        lv_obj_move_foreground(s_o[MSG_MORE]);
    }
    msgs_bar(v);
    set_text(s_o[MSG_STATUS], v->status);
}

// ---------------------------------------------------------------- notes
// Early-2010s skeuomorphic Notes: leather title bar, pale yellow ruled paper, a thin red margin.
// Memo text is ASCII in the 14 px font (no handwriting face on the device).
#define NOTE_BAR_H 32
#define NOTE_TOP (CS_STATUS_H + NOTE_BAR_H)
#define NOTE_ROW_H 28
#define NOTE_TEXT_X 34
#define NOTE_INK 0x3B2A1C
#define NOTE_INK_DONE 0x9C8A6A
#define NOTE_RED 0xC0453A

static const uint8_t MEMO_MIC_A8[24 * 24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x8F, 0xE3, 0xEE, 0xAD, 0x26, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xE8, 0x1A, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x4F, 0xFF, 0xF9, 0xFD, 0xFE, 0xF8, 0xFF, 0x8F, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x83, 0xFE, 0xFB, 0xFF, 0xFF, 0xFC, 0xFE, 0xC1, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x7F, 0xFF, 0xFB, 0xFF, 0xFF, 0xFC, 0xFF, 0xBD, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xFF, 0xFB, 0xFF, 0xFF, 0xFC, 0xFF, 0xBE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xFF, 0xFB, 0xFF, 0xFF, 0xFC, 0xFF, 0xBE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x7F, 0xFF, 0xFB, 0xFF, 0xFF, 0xFC, 0xFF, 0xBD, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x00, 0x84, 0xFE, 0xFB, 0xFF, 0xFF, 0xFC, 0xFE, 0xC1, 0x00, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0xFF, 0xF9, 0xFF, 0xFF, 0xFA, 0xFE, 0xA6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x0B, 0xBF, 0x8D, 0x00, 0x11, 0xDE, 0xFF, 0xFC, 0xFC, 0xFF, 0xFF, 0x36, 0x01, 0x59, 0xBD, 0x31, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x07, 0xF1, 0xE8, 0x05, 0x00, 0x31, 0xC6, 0xFF, 0xFF, 0xE0, 0x56, 0x00, 0x00, 0xA8, 0xFF, 0x2A, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x95, 0xFF, 0x5A, 0x00, 0x00, 0x03, 0x37, 0x42, 0x0E, 0x00, 0x00, 0x21, 0xF9, 0xD8, 0x03, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x01, 0x23, 0xEC, 0xED, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0xC7, 0xFF, 0x50, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x4F, 0xF6, 0xFF, 0x9B, 0x40, 0x17, 0x15, 0x33, 0x7B, 0xEE, 0xFF, 0x82, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x35, 0xD5, 0xFF, 0xFF, 0xF4, 0xEF, 0xFF, 0xFF, 0xEE, 0x60, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x08, 0x4E, 0x90, 0xF3, 0xFF, 0xA3, 0x60, 0x19, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xBB, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x3A, 0x42, 0x41, 0xCE, 0xF8, 0x4A, 0x40, 0x3F, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x49, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x73, 0x7B, 0x7B, 0x7D, 0x7D, 0x7B, 0x7A, 0x7B, 0x1F, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t MEMO_SEND_A8[24 * 24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x0F, 0x0F, 0x09, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x06, 0x4D, 0x9B, 0xD6, 0xF3, 0xF3, 0xE3, 0xAA, 0x5F, 0x16, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x46, 0xD1, 0xFF, 0xFF, 0xFF, 0xF3, 0xF3, 0xFF, 0xFF, 0xFF, 0xE8, 0x6C, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x01, 0x7F, 0xFF, 0xFF, 0xD0, 0x70, 0x34, 0x10, 0x10, 0x29, 0x5E, 0xB6, 0xFF, 0xFF, 0xA7, 0x13, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x7F, 0xFF, 0xF9, 0x6E, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xE1, 0xFF, 0xB9, 0x01, 0x03, 0x00, 0x00,
    0x00, 0x03, 0x00, 0x43, 0xFD, 0xF7, 0x3D, 0x00, 0x00, 0x04, 0x02, 0x01, 0x00, 0x01, 0x05, 0x02, 0x00, 0x19, 0xD7, 0xFF, 0x7D, 0x00, 0x03, 0x00,
    0x00, 0x01, 0x09, 0xD6, 0xFF, 0x6E, 0x00, 0x05, 0x01, 0x00, 0x01, 0x00, 0x2E, 0x04, 0x00, 0x02, 0x05, 0x00, 0x36, 0xF6, 0xF8, 0x29, 0x00, 0x02,
    0x04, 0x00, 0x4A, 0xFF, 0xD2, 0x03, 0x03, 0x01, 0x00, 0x00, 0x04, 0x00, 0x8F, 0xAD, 0x0C, 0x00, 0x02, 0x04, 0x00, 0x9A, 0xFF, 0x89, 0x00, 0x05,
    0x04, 0x00, 0x9B, 0xFF, 0x6E, 0x00, 0x04, 0x02, 0x02, 0x02, 0x06, 0x02, 0x7D, 0xFF, 0xC6, 0x11, 0x00, 0x06, 0x01, 0x32, 0xFF, 0xD8, 0x04, 0x01,
    0x01, 0x08, 0xDA, 0xFC, 0x30, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7A, 0xFD, 0xFF, 0xCA, 0x19, 0x00, 0x02, 0x0A, 0xE2, 0xFD, 0x28, 0x00,
    0x00, 0x10, 0xF6, 0xF3, 0x10, 0x00, 0x0D, 0xAF, 0xBB, 0xBA, 0xBC, 0xBB, 0xDD, 0xFF, 0xFB, 0xFF, 0xE5, 0x43, 0x01, 0x00, 0xB9, 0xFF, 0x40, 0x00,
    0x00, 0x10, 0xF5, 0xF2, 0x0F, 0x00, 0x14, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD, 0xFE, 0xFF, 0x66, 0x00, 0x00, 0xB8, 0xFF, 0x3E, 0x00,
    0x01, 0x0B, 0xE6, 0xF9, 0x24, 0x00, 0x06, 0x3A, 0x3E, 0x3E, 0x41, 0x3E, 0xA0, 0xFD, 0xFF, 0xEE, 0x45, 0x00, 0x02, 0x06, 0xD4, 0xFE, 0x32, 0x00,
    0x04, 0x00, 0xAC, 0xFF, 0x5B, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0xFF, 0xEC, 0x35, 0x00, 0x06, 0x01, 0x23, 0xFD, 0xE7, 0x09, 0x01,
    0x04, 0x00, 0x5F, 0xFF, 0xBA, 0x00, 0x03, 0x03, 0x03, 0x03, 0x07, 0x03, 0x8F, 0xD9, 0x2F, 0x00, 0x04, 0x04, 0x00, 0x7C, 0xFE, 0x9E, 0x00, 0x04,
    0x01, 0x01, 0x15, 0xEC, 0xFD, 0x45, 0x00, 0x05, 0x00, 0x00, 0x02, 0x00, 0x50, 0x1E, 0x00, 0x03, 0x03, 0x01, 0x17, 0xE8, 0xFF, 0x41, 0x00, 0x03,
    0x00, 0x03, 0x00, 0x69, 0xFF, 0xE2, 0x16, 0x00, 0x04, 0x02, 0x01, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0xB5, 0xFF, 0xA7, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x03, 0x00, 0xAA, 0xFF, 0xD5, 0x3A, 0x00, 0x00, 0x00, 0x02, 0x04, 0x01, 0x00, 0x00, 0x1A, 0xB4, 0xFF, 0xDC, 0x11, 0x02, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x11, 0xB7, 0xFF, 0xFB, 0x99, 0x38, 0x0A, 0x00, 0x00, 0x03, 0x28, 0x7B, 0xE7, 0xFF, 0xD8, 0x34, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x7E, 0xFA, 0xFF, 0xFF, 0xE3, 0xBC, 0xBC, 0xD6, 0xFF, 0xFF, 0xFF, 0xA4, 0x13, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x2C, 0x89, 0xD5, 0xFF, 0xFF, 0xFF, 0xFF, 0xE3, 0x9C, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x02, 0x27, 0x3D, 0x3D, 0x32, 0x09, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t MEMO_TRASH_A8[24 * 24] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x05, 0x05, 0x05, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x03, 0x01, 0x41, 0x80, 0x7E, 0x7F, 0x80, 0x60, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87, 0xFF, 0xFF, 0xFF, 0xFF, 0xCB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x0E, 0x0F, 0x0F, 0x13, 0x0F, 0x85, 0xFA, 0xF6, 0xF7, 0xFA, 0xBE, 0x0F, 0x11, 0x0F, 0x0F, 0x0F, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x10, 0xE3, 0xF3, 0xF3, 0xF4, 0xF4, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF4, 0xF4, 0xF4, 0xF2, 0xF3, 0x3E, 0x00, 0x03, 0x00,
    0x00, 0x01, 0x00, 0x10, 0xE3, 0xF1, 0xEE, 0xEE, 0xEE, 0xEC, 0xEA, 0xEA, 0xEA, 0xEA, 0xEB, 0xEE, 0xEF, 0xEE, 0xEE, 0xF3, 0x3E, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x0E, 0x17, 0x20, 0x1E, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1D, 0x1D, 0x20, 0x1B, 0x0F, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x76, 0xEA, 0xED, 0xF8, 0xFC, 0xF9, 0xFA, 0xFB, 0xF8, 0xFC, 0xF9, 0xF1, 0xEA, 0xB0, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x01, 0x53, 0xFF, 0xE2, 0xC3, 0xB1, 0xC0, 0xB9, 0xB4, 0xC3, 0xB1, 0xBF, 0xD2, 0xFF, 0x92, 0x01, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x3D, 0xFE, 0x71, 0x00, 0x75, 0x0B, 0x33, 0x5B, 0x00, 0x75, 0x10, 0x29, 0xFE, 0x7D, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x26, 0xFD, 0xAC, 0x05, 0xFF, 0x36, 0x8C, 0xE1, 0x02, 0xFF, 0x3B, 0x5F, 0xFF, 0x5D, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x0A, 0xFF, 0xBE, 0x00, 0xFB, 0x2F, 0x7E, 0xCC, 0x00, 0xFB, 0x31, 0x6D, 0xFF, 0x3B, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x01, 0x0B, 0xEC, 0xD8, 0x10, 0xFC, 0x31, 0x80, 0xCF, 0x00, 0xFF, 0x37, 0x8F, 0xFF, 0x32, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0xC2, 0xFC, 0x1B, 0xFA, 0x31, 0x80, 0xCF, 0x00, 0xFF, 0x38, 0xB5, 0xFF, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0xBE, 0xF9, 0x28, 0xF8, 0x32, 0x80, 0xCF, 0x00, 0xFD, 0x40, 0xBF, 0xF9, 0x0D, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x81, 0xFF, 0x4C, 0xF1, 0x33, 0x80, 0xCF, 0x00, 0xF8, 0x54, 0xEE, 0xC7, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xFF, 0x4F, 0xF0, 0x34, 0x80, 0xCE, 0x01, 0xF6, 0x56, 0xEE, 0xC7, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x4D, 0xFF, 0x85, 0xF2, 0x32, 0x81, 0xD4, 0x00, 0xF6, 0x80, 0xFE, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x3E, 0xFD, 0x87, 0x4A, 0x1E, 0x2E, 0x43, 0x10, 0x4B, 0x59, 0xFC, 0x7E, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2A, 0xFF, 0xFF, 0xEF, 0xFA, 0xF5, 0xF1, 0xFD, 0xEF, 0xF9, 0xFF, 0x64, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x09, 0xB0, 0xC1, 0xC5, 0xC3, 0xC4, 0xC5, 0xC3, 0xC6, 0xC1, 0xBE, 0x2C, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define A8_IMG(name) {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = 24, .h = 24, \
                                 .stride = 24}, .data_size = sizeof name##_A8, .data = name##_A8}
static const lv_image_dsc_t MEMO_ICON[3] = {A8_IMG(MEMO_MIC), A8_IMG(MEMO_SEND), A8_IMG(MEMO_TRASH)};

static lv_obj_t *notes_chrome(const char *title) {
    grad(s_scr, 0xFFF8CF, 0xF5EAAE);
    for (int y = NOTE_TOP + NOTE_ROW_H; y < SCR_H; y += NOTE_ROW_H) {
        rect(s_scr, 0, y, SCR_W, 1, 0xD8C68C, LV_OPA_COVER, 0);
    }
    rect(s_scr, 26, NOTE_TOP, 1, SCR_H - NOTE_TOP, 0xE09486, LV_OPA_COVER, 0);
    rect(s_scr, 28, NOTE_TOP, 1, SCR_H - NOTE_TOP, 0xE09486, 110, 0);
    msgs_status_art();
    lv_obj_t *bar = rect(s_scr, 0, CS_STATUS_H, SCR_W, NOTE_BAR_H, 0, LV_OPA_COVER, 0);
    grad(bar, 0xA8644F, 0x5A281E);
    lv_obj_set_style_bg_main_stop(bar, 70, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    border(bar, 0x2E100A, LV_OPA_COVER);
    lv_obj_t *t = label_center(s_scr, &cs_font18, 0xFFFFFF, SCR_W / 2, 140, CS_STATUS_H + 22);
    lv_label_set_text(t, title);
    return t;
}

static void notes_build(void) {
    s_o[NOTE_TITLE] = notes_chrome("Notes");
    lv_obj_t *hl = rect(s_scr, 27, NOTE_TOP + 1, SCR_W - 27, NOTE_ROW_H - 1, 0xEFD27E, 200, 0);
    show(hl, false);
    s_o[NOTE_HL] = hl;
    for (int r = 0; r < CS_NOTES_ROWS; r++) {
        const int y = NOTE_TOP + r * NOTE_ROW_H;
        lv_obj_t *t = label_left(s_scr, &cs_font14, NOTE_INK, NOTE_TEXT_X, y + 20);
        lv_obj_set_size(t, 166, lv_font_get_line_height(&cs_font14));   // one line, cut with "…"
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        s_o[NOTE_TXT0 + r] = t;
        lv_obj_t *c = label_left(s_scr, &cs_font18, 0x2E8B3A, 210, y + 21);
        lv_label_set_text_static(c, "✓");
        show(c, false);
        s_o[NOTE_CHK0 + r] = c;
    }
    lv_obj_t *plus = rect(s_scr, 194, CS_STATUS_H + 5, 32, 22, 0, LV_OPA_COVER, 5);
    grad(plus, 0x93503F, 0x4E2018);
    border(plus, 0x2A0E08, LV_OPA_COVER);
    lv_obj_t *pl = label_center(plus, &cs_font18, 0xFFFFFF, 16, 30, 18);
    lv_label_set_text_static(pl, "+");
    s_o[NOTE_PLUS] = plus;
}

void cs_ui_notes_update(const cs_notes_view_t *v) {
    if (s_cur != CS_SCR_NOTES || !s_o[NOTE_PLUS]) return;
    char t[24];
    if (v->count > 0) snprintf(t, sizeof t, "Notes (%d)", v->count);
    else snprintf(t, sizeof t, "Notes");
    set_text(s_o[NOTE_TITLE], t);
    for (int r = 0; r < CS_NOTES_ROWS; r++) {
        lv_obj_t *l = s_o[NOTE_TXT0 + r];
        const bool used = r < v->n;
        set_text(l, used ? v->rows[r].text : "");
        lv_obj_set_style_text_color(l, hex(used && v->rows[r].done ? NOTE_INK_DONE : NOTE_INK), 0);
        show(s_o[NOTE_CHK0 + r], used && v->rows[r].done);
    }
    const int row = v->cursor - v->first;
    show(s_o[NOTE_HL], v->cursor >= 0 && row >= 0 && row < CS_NOTES_ROWS);
    if (v->cursor >= 0) lv_obj_set_y(s_o[NOTE_HL], NOTE_TOP + row * NOTE_ROW_H + 1);
    // The "+" button: a light ring when it has the cursor.
    lv_obj_set_style_border_color(s_o[NOTE_PLUS], hex(v->cursor < 0 ? 0xFFE39A : 0x2A0E08), 0);
    lv_obj_set_style_border_width(s_o[NOTE_PLUS], v->cursor < 0 ? 2 : 1, 0);
}

static void memo_build(void) {
    s_o[MEMO_TITLE] = notes_chrome("New Memo");
    lv_obj_t *page = label_left(s_scr, &cs_font14, NOTE_INK, NOTE_TEXT_X, NOTE_TOP + 20);
    lv_obj_set_width(page, 194);
    lv_label_set_long_mode(page, LV_LABEL_LONG_MODE_WRAP);
    // Lines of text sit on the ruled lines.
    lv_obj_set_style_text_line_space(page, NOTE_ROW_H - lv_font_get_line_height(&cs_font14), 0);
    s_o[MEMO_PAGE] = page;
    s_o[MEMO_STATUS] = label_center(s_scr, &cs_font12, 0x8A5A40, SCR_W / 2, 196, 258);
    lv_label_set_long_mode(s_o[MEMO_STATUS], LV_LABEL_LONG_MODE_DOTS);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = rect(s_scr, 32 + i * 66, 266, 44, 34, 0xEDD796, LV_OPA_TRANSP, 8);
        lv_obj_t *img = lv_image_create(btn);
        lv_image_set_src(img, &MEMO_ICON[i]);
        lv_obj_set_style_image_recolor(img, hex(NOTE_RED), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_center(img);
        s_o[MEMO_BTN0 + i] = btn;
    }
}

void cs_ui_memo_update(const cs_memo_view_t *v) {
    if (s_cur != CS_SCR_MEMO || !s_o[MEMO_PAGE]) return;
    lv_obj_t *page = s_o[MEMO_PAGE];
    set_text(s_o[MEMO_TITLE], v->detail ? "Memo" : "New Memo");
    lv_obj_set_style_text_color(page, hex(v->detail && v->done ? NOTE_INK_DONE : NOTE_INK), 0);
    const bool big = v->state == CS_MEMO_WAIT || v->state == CS_MEMO_FAIL;
    lv_obj_set_style_text_font(page, big ? &cs_font18 : &cs_font14, 0);
    set_text(page, v->state == CS_MEMO_PREVIEW ? v->text : v->state == CS_MEMO_WAIT ? "…"
                   : v->state == CS_MEMO_FAIL ? "?" : "");
    set_text(s_o[MEMO_STATUS], v->status);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = s_o[MEMO_BTN0 + i];
        lv_obj_t *img = lv_obj_get_child(btn, 0);
        show(btn, !v->detail);
        const bool en = (v->enabled >> i) & 1;
        const bool rec = i == 0 && v->state == CS_MEMO_REC;
        const bool foc = en && i == v->focus;
        lv_obj_set_style_bg_color(btn, hex(rec ? 0xD9483B : 0xEDD796), 0);
        lv_obj_set_style_bg_opa(btn, rec || foc ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(btn, hex(rec ? 0x8E1F1A : 0xB0503E), 0);
        lv_obj_set_style_border_width(btn, foc || rec ? 2 : 0, 0);
        lv_obj_set_style_image_recolor(img, hex(rec ? 0xFFFFFF : NOTE_RED), 0);
        lv_obj_set_style_opa(btn, en ? LV_OPA_COVER : 90, 0);
    }
}

// ---------------------------------------------------------------- mines
// Graphite glossy nav bar; the body is the setup card or a board. Her field
// sits on grass with sand tiles, the companion's on steel with grey glass tiles. Cell
// markers are A8 glyphs drawn as the tile's background image (no extra
// objects); one gold ring is the cursor. Shared styles as for the bubbles.
#define MN_NAV_H 32
#define MN_TOP (CS_STATUS_H + MN_NAV_H)
#define MN_BOARD_Y 64
#define MN_NOTE_Y 254
#define MN_BAR_Y 288
static lv_obj_t *s_mcell[CS_MINES_MAX_CELLS];
static uint8_t s_mkind[CS_MINES_MAX_CELLS];
static int s_mn_built = -1;          // body built for: 0 setup card, n board side, -1 none
static int s_mn_field = -1, s_mn_ovl = -1, s_mn_cell = 0, s_mn_gap = 0, s_mn_x0 = 0;
static lv_style_t s_st_mc[CS_MCELL_KINDS], s_st_coord, s_st_btn[4];   // buttons: plain, focused, warn, off
static bool s_st_mc_ready;

static void mines_styles(void) {
    if (s_st_mc_ready) return;
    s_st_mc_ready = true;
    static const struct { uint32_t top, low, edge; const lv_image_dsc_t *img; uint32_t ink; } K[CS_MCELL_KINDS] = {
        [CS_MCELL_SAND] = {0xFFF7DC, 0xE3CF98, 0x9C8550, NULL, 0},
        [CS_MCELL_FLAG] = {0xFFF7DC, 0xE3CF98, 0x9C8550, &cs_mines_flag, 0xD8261C},
        [CS_MCELL_FOOT] = {0xFFF7DC, 0xE3CF98, 0x9C8550, &cs_mines_foot, 0x5A3A22},
        [CS_MCELL_BOOM] = {0xFF8A78, 0xC61E14, 0x7E0F09, &cs_mines_mine, 0x111214},
        [CS_MCELL_GREY] = {0xF6F8FB, 0xAEB7C4, 0x5E6878, NULL, 0},
        [CS_MCELL_GREEN] = {0xC8F59E, 0x4CAA2C, 0x2C6E14, NULL, 0},
        [CS_MCELL_WAIT] = {0xFFF6C0, 0xE9C94C, 0x8E7418, NULL, 0},
        [CS_MCELL_MINE] = {0xF6F8FB, 0xAEB7C4, 0x5E6878, &cs_mines_mine, 0x111214},
    };
    for (int k = 0; k < CS_MCELL_KINDS; k++) {
        lv_style_t *s = &s_st_mc[k];
        lv_style_init(s);
        lv_style_set_radius(s, 6);
        lv_style_set_bg_opa(s, LV_OPA_COVER);
        lv_style_set_bg_color(s, hex(K[k].top));
        lv_style_set_bg_grad_color(s, hex(K[k].low));
        lv_style_set_bg_grad_dir(s, LV_GRAD_DIR_VER);
        lv_style_set_border_color(s, hex(K[k].edge));
        lv_style_set_border_width(s, 1);
        if (K[k].img) {
            lv_style_set_bg_image_src(s, K[k].img);
            lv_style_set_bg_image_recolor(s, hex(K[k].ink));
            lv_style_set_bg_image_recolor_opa(s, LV_OPA_COVER);
        }
    }
    lv_style_init(&s_st_coord);                              // row letters / column numbers
    lv_style_set_text_font(&s_st_coord, &cs_font12);
    lv_style_set_text_color(&s_st_coord, hex(0xFFFFFF));
    lv_style_set_text_opa(&s_st_coord, 210);
    lv_style_set_text_align(&s_st_coord, LV_TEXT_ALIGN_CENTER);
    static const uint32_t BT[4][4] = {   // top, low, edge, text
        {0xFDFEFF, 0xCDD4DE, 0x8D97A6, 0x2A3445}, {0x9CC4F6, 0x2E6AD0, 0x1E4C9C, 0xFFFFFF},
        {0xF79A90, 0xC9302A, 0x8E1F1A, 0xFFFFFF}, {0xFDFEFF, 0xCDD4DE, 0x8D97A6, 0x2A3445},
    };
    for (int k = 0; k < 4; k++) {
        lv_style_t *b = &s_st_btn[k];
        lv_style_init(b);
        lv_style_set_radius(b, 7);
        lv_style_set_bg_opa(b, LV_OPA_COVER);
        lv_style_set_bg_color(b, hex(BT[k][0]));
        lv_style_set_bg_grad_color(b, hex(BT[k][1]));
        lv_style_set_bg_grad_dir(b, LV_GRAD_DIR_VER);
        lv_style_set_border_color(b, hex(BT[k][2]));
        lv_style_set_border_width(b, k == 1 || k == 2 ? 2 : 1);
        lv_style_set_text_color(b, hex(BT[k][3]));
        lv_style_set_text_align(b, LV_TEXT_ALIGN_CENTER);
        lv_style_set_opa(b, k == 3 ? 110 : LV_OPA_COVER);
    }
}

static void mines_field_bg(int field) {
    if (field == s_mn_field) return;
    s_mn_field = field;
    if (field == 0) grad(s_scr, 0x7DB35A, 0x4F8538);         // her field: grass
    else if (field == 1) grad(s_scr, 0x7F93AE, 0x4E6383);    // the companion's field: steel
    else grad(s_scr, 0xC5CCD6, 0xAEB7C3);                    // setup: pinstripe grey
}

static void mines_build(void) {
    s_mn_built = s_mn_field = s_mn_ovl = -1;
    memset(s_mcell, 0, sizeof s_mcell);
    mines_field_bg(2);
    msgs_status_art();
    lv_obj_t *nav = rect(s_scr, 0, CS_STATUS_H, SCR_W, MN_NAV_H / 2, 0, LV_OPA_COVER, 0);
    grad(nav, 0x8E959F, 0x5B626C);
    nav = rect(s_scr, 0, CS_STATUS_H + MN_NAV_H / 2, SCR_W, MN_NAV_H - MN_NAV_H / 2, 0, LV_OPA_COVER, 0);
    grad(nav, 0x474E58, 0x2C3138);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, 0);
    border(nav, 0x16191E, LV_OPA_COVER);
    s_o[MN_TITLE_SH] = label_center(s_scr, &cs_font18, 0x16191E, SCR_W / 2, 150, CS_STATUS_H + 21);
    s_o[MN_TITLE] = label_center(s_scr, &cs_font18, 0xFFFFFF, SCR_W / 2, 150, CS_STATUS_H + 22);
    s_o[MN_SIZE] = label_left(s_scr, &cs_font12, 0xD7DCE3, 196, CS_STATUS_H + 20);
    lv_obj_t *body = bare(s_scr);
    lv_obj_set_pos(body, 0, MN_TOP);
    lv_obj_set_size(body, SCR_W, SCR_H - MN_TOP);
    s_o[MN_BODY] = body;
}

// A button is one label: shared style (gradient, border, text colour), the
// text centred by padding the label to the button height.
static lv_obj_t *mines_button(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *f) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &s_st_btn[0], 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_pad_top(l, (h - lv_font_get_line_height(f)) / 2, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text_static(l, "");
    return l;
}

static void mines_button_style(lv_obj_t *btn, const char *txt, bool on, bool focused, bool warn) {
    const int k = warn ? 2 : focused && on ? 1 : on || focused ? 0 : 3;
    for (int i = 0; i < 4; i++) lv_obj_remove_style(btn, &s_st_btn[i], 0);
    lv_obj_add_style(btn, &s_st_btn[k], 0);
    if (focused && !on) lv_obj_set_style_border_width(btn, 2, 0);
    else lv_obj_remove_local_style_prop(btn, LV_STYLE_BORDER_WIDTH, 0);
    set_text(btn, txt ? txt : "");
}

// Setup card: score, size and who starts on a white card, a big Start button.
static void mines_build_setup(lv_obj_t *body) {
    s_o[MN_SCORE] = label_center(body, &cs_font18, 0x2A3445, SCR_W / 2, 220, 34);
    lv_obj_t *card = rect(body, 14, 50, 212, 88, 0xFFFFFF, LV_OPA_COVER, 9);
    border(card, 0xAAAEB6, LV_OPA_COVER);
    rect(card, 0, 43, 212, 1, 0xD5D9E0, LV_OPA_COVER, 0);
    static const char *const ROW[2] = {"Size", "First"};
    for (int r = 0; r < 2; r++) {
        lv_obj_t *k = label_left(card, &cs_font14, 0x141418, 12, 28 + r * 44);
        lv_label_set_text_static(k, ROW[r]);
        lv_obj_t *v = label_left(card, &cs_font14, 0x385487, 90, 28 + r * 44);
        lv_obj_set_width(v, 110);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        s_o[MN_SET_V0 + r] = v;
    }
    s_o[MN_SET_START] = mines_button(body, 44, 154, 152, 40, &cs_font18);
    lv_obj_t *ring = rect(body, 12, 48, 216, 46, 0x3478F6, LV_OPA_TRANSP, 11);
    lv_obj_set_style_border_color(ring, hex(0x3478F6), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    s_o[MN_SET_RING] = ring;
    lv_obj_t *hint = label_center(body, &cs_font12, 0x4C566C, SCR_W / 2, 200, 232);
    lv_label_set_text_static(hint, "No numbers. Pure luck.");
}

static void mines_build_board(lv_obj_t *body, int n) {
    static const uint8_t CELL[] = {54, 42, 34}, GAP[] = {6, 5, 4};
    const int k = n <= 3 ? 0 : n == 4 ? 1 : 2;
    s_mn_cell = CELL[k];
    s_mn_gap = GAP[k];
    const int bw = n * s_mn_cell + (n - 1) * s_mn_gap;
    s_mn_x0 = 12 + (228 - bw) / 2;
    const int y0 = MN_BOARD_Y - MN_TOP;
    // Coordinates in two labels: digits are equal width, so letter spacing
    // puts each column number over its column; one row letter per line.
    static const char *const COLS[] = {"123", "1234", "12345"}, *const ROWS[] = {"A\nB\nC", "A\nB\nC\nD", "A\nB\nC\nD\nE"};
    const int pitch = s_mn_cell + s_mn_gap;
    const int asc = cs_font12.line_height - cs_font12.base_line;   // baseline -> label top
    lv_font_glyph_dsc_t g;
    const int dw = lv_font_get_glyph_dsc(&cs_font12, &g, '1', '2') ? g.adv_w : 7;
    lv_obj_t *c = lv_label_create(body);
    lv_obj_add_style(c, &s_st_coord, 0);
    lv_obj_set_style_text_letter_space(c, pitch - dw, 0);
    lv_obj_set_width(c, LV_SIZE_CONTENT);
    lv_label_set_text_static(c, COLS[k]);
    lv_obj_set_pos(c, s_mn_x0 + s_mn_cell / 2 - dw / 2, y0 - 4 - asc);
    lv_obj_t *r = lv_label_create(body);
    lv_obj_add_style(r, &s_st_coord, 0);
    lv_obj_set_style_text_line_space(r, pitch - lv_font_get_line_height(&cs_font12), 0);
    lv_label_set_text_static(r, ROWS[k]);
    lv_obj_set_pos(r, s_mn_x0 - 21, y0 + s_mn_cell / 2 + 5 - asc);
    for (int i = 0; i < n * n; i++) {
        lv_obj_t *c = bare(body);
        lv_obj_set_pos(c, s_mn_x0 + (i % n) * (s_mn_cell + s_mn_gap), y0 + (i / n) * (s_mn_cell + s_mn_gap));
        lv_obj_set_size(c, s_mn_cell, s_mn_cell);
        lv_obj_add_style(c, &s_st_mc[CS_MCELL_SAND], 0);
        s_mcell[i] = c;
        s_mkind[i] = CS_MCELL_SAND;
    }
    lv_obj_t *ring = rect(body, 0, 0, s_mn_cell + 8, s_mn_cell + 8, 0, LV_OPA_TRANSP, 9);
    lv_obj_set_style_border_color(ring, hex(0xFFD23A), 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    s_o[MN_RING] = ring;
    lv_obj_t *note = label_left(body, &cs_font12, 0xFFFFFF, 16, MN_NOTE_Y - MN_TOP + 12);
    lv_obj_set_size(note, 208, 2 * lv_font_get_line_height(&cs_font12));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_DOTS);
    s_o[MN_NOTE] = note;
    lv_obj_t *bar = rect(body, 0, MN_BAR_Y - MN_TOP, SCR_W, SCR_H - MN_BAR_Y, 0, LV_OPA_COVER, 0);
    grad(bar, 0xEEF1F5, 0xB4BFCE);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    border(bar, 0x8893A6, LV_OPA_COVER);
    s_o[MN_STATUS] = label_left(bar, &cs_font12, 0x33415A, 22, 19);
    lv_obj_set_width(s_o[MN_STATUS], 94);
    lv_label_set_long_mode(s_o[MN_STATUS], LV_LABEL_LONG_MODE_DOTS);
    s_o[MN_BTN0] = mines_button(bar, 118, 4, 50, 24, &cs_font14);
    s_o[MN_BTN0 + 1] = mines_button(bar, 174, 4, 50, 24, &cs_font14);
}

// End screen: a sticky note with blue hand-lettered capitals over everything,
// built only while it shows (the 5x5 board leaves little of the LVGL pool).
static void mines_overlay(int ovl) {
    if (ovl == s_mn_ovl) return;
    s_mn_ovl = ovl;
    if (s_o[MN_OVL]) {
        lv_obj_delete(s_o[MN_OVL]);
        s_o[MN_OVL] = s_o[MN_OVL_W0] = s_o[MN_OVL_W1] = s_o[MN_OVL_SENT] = NULL;
    }
    if (ovl == CS_MOVL_NONE) return;
    lv_obj_t *dim = rect(s_scr, 0, 0, SCR_W, SCR_H, 0x000000, 150, 0);
    rect(dim, 28, 68, 196, 196, 0x000000, 90, 4);                   // shadow
    lv_obj_t *paper = rect(dim, 22, 62, 196, 196, 0, LV_OPA_COVER, 3);
    grad(paper, 0xFFF7A6, 0xF6E06A);
    rect(paper, 0, 0, 196, 14, 0xE9CF4E, 120, 0);                   // the glued strip
    for (int i = 0; i < 2; i++) {
        lv_obj_t *w = lv_image_create(paper);
        lv_obj_set_style_image_recolor(w, hex(0x2456C8), 0);
        lv_obj_set_style_image_recolor_opa(w, LV_OPA_COVER, 0);
        s_o[MN_OVL_W0 + i] = w;
    }
    lv_obj_t *sent = label_center(paper, &cs_font12, 0x2456C8, 98, 190, 186);
    lv_obj_set_style_text_opa(sent, 200, 0);
    s_o[MN_OVL_SENT] = sent;
    s_o[MN_OVL] = dim;
    const lv_image_dsc_t *w0 = ovl == CS_MOVL_WIN ? &cs_mines_you : &cs_mines_dead;
    const lv_image_dsc_t *w1 = ovl == CS_MOVL_WIN ? &cs_mines_win : ovl == CS_MOVL_DEAD_AGAIN ? &cs_mines_again : NULL;
    lv_image_set_src(s_o[MN_OVL_W0], w0);
    const int h0 = w0->header.h, h1 = w1 ? w1->header.h + 8 : 0;
    const int top = 6 + (196 - 6 - h0 - h1) / 2;
    lv_obj_set_pos(s_o[MN_OVL_W0], (196 - w0->header.w) / 2, top);
    show(s_o[MN_OVL_W1], w1 != NULL);
    if (w1) {
        lv_image_set_src(s_o[MN_OVL_W1], w1);
        lv_obj_set_pos(s_o[MN_OVL_W1], (196 - w1->header.w) / 2 + 6, top + h0 + 8);
    }
}

void cs_ui_mines_update(const cs_mines_view_t *v) {
    if (s_cur != CS_SCR_MINES || !s_o[MN_BODY]) return;
    mines_styles();
    const int want = v->setup ? 0 : v->n;
    if (want != s_mn_built) {
        lv_obj_clean(s_o[MN_BODY]);
        memset(s_mcell, 0, sizeof s_mcell);
        for (int i = MN_BODY + 1; i < MN_OVL; i++) s_o[i] = NULL;
        if (v->setup) mines_build_setup(s_o[MN_BODY]);
        else mines_build_board(s_o[MN_BODY], v->n);
        s_mn_built = want;
    }
    mines_field_bg(v->setup ? 2 : v->field);
    char b[8];
    if (v->setup) {
        set_text(s_o[MN_TITLE_SH], "Mines");
        set_text(s_o[MN_TITLE], "Mines");
        set_text(s_o[MN_SIZE], "");
        set_text(s_o[MN_SCORE], v->score_txt);
        set_text(s_o[MN_SET_V0], v->size_txt);
        set_text(s_o[MN_SET_V0 + 1], v->first_txt);
        mines_button_style(s_o[MN_SET_START], "Start", true, v->setup_focus == 2, false);
        show(s_o[MN_SET_RING], v->setup_focus < 2);
        lv_obj_set_y(s_o[MN_SET_RING], 48 + (v->setup_focus == 1 ? 44 : 0));
    } else {
        const char *t = v->field ? CS_COMPANION_NAME "'s field" : "Your field";
        set_text(s_o[MN_TITLE_SH], t);
        set_text(s_o[MN_TITLE], t);
        snprintf(b, sizeof b, "%dx%d", v->n, v->n);
        set_text(s_o[MN_SIZE], b);
        const int cells = v->n * v->n;
        for (int i = 0; i < cells; i++) {
            const uint8_t k = v->cells[i] < CS_MCELL_KINDS ? v->cells[i] : CS_MCELL_SAND;
            if (k == s_mkind[i]) continue;
            lv_obj_remove_style(s_mcell[i], &s_st_mc[s_mkind[i]], 0);
            lv_obj_add_style(s_mcell[i], &s_st_mc[k], 0);
            s_mkind[i] = k;
        }
        const bool on_cell = v->cursor >= 0 && v->cursor < cells;
        show(s_o[MN_RING], on_cell);
        if (on_cell) {
            const int pitch = s_mn_cell + s_mn_gap;
            lv_obj_set_pos(s_o[MN_RING], s_mn_x0 + (v->cursor % v->n) * pitch - 4,
                           MN_BOARD_Y - MN_TOP + (v->cursor / v->n) * pitch - 4);
        }
        static const uint32_t MN_INK[] = {0xFFFFFF, 0xFFF4C2, 0xD2FFB8, 0xFFC4BC};
        lv_obj_set_style_text_color(s_o[MN_NOTE], hex(MN_INK[v->note_kind < 4 ? v->note_kind : 0]), 0);
        set_text(s_o[MN_NOTE], v->note);
        set_text(s_o[MN_STATUS], v->status);
        for (int i = 0; i < 2; i++) {
            mines_button_style(s_o[MN_BTN0 + i], v->btn[i], (v->btn_on >> i) & 1, v->cursor == cells + i,
                               (v->btn_warn >> i) & 1);
        }
    }
    mines_overlay(v->overlay);
    set_text(s_o[MN_OVL_SENT], v->owner_sent ? "Sent your mines to " CS_COMPANION_NAME : "");
}

// ---------------------------------------------------------------- bingo
// A blue notebook page: near-white paper with faint blue rules every 12 px, a
// glossy system-blue header (two halves, like the other glossy bars) with a scalloped
// paper edge, "My Bingo", the date or a status and a white progress ring. The
// header, the rules and the scallops are ONE object drawn in a draw callback;
// the whole grid is another (25 label objects did not fit the 32 KB LVGL
// pool): white tiles with a light-blue border, an empty circle over the label;
// done = a gold disc with a white check on cream; fixed and not done = a paler
// cream with a golden border and an empty golden circle; the cursor = a thick
// system-blue border with a soft halo (orange while picking a cell to swap).
// The grid draws straight from the shell's card; the shell changes the card
// under the LVGL lock. Blue gradient buttons, the BINGO! sticky note on top.
#define BG_HEAD_H 46
#define BG_TOP (CS_STATUS_H + BG_HEAD_H)    // the paper edge (scallop centres)
#define BG_BAR_Y 290                        // the buttons
#define BG_PAPER 0xFAFCFF
#define BG_RULE 0xD2E1F3
#define BG_BLUE 0x1E6ED7                    // system blue
#define BG_SWAP 0xFF8A1A
enum { BCELL_OPEN = 0, BCELL_FIXED, BCELL_DONE, BCELL_KINDS };
static int s_bg_n = -1;                // body built for side n (0: the message), -1 none
static int s_bg_ovl = -1;
static struct {
    const cs_bingo_card_t *card;
    uint32_t gen, busy, done, fixed;
    int8_t cursor;
    bool swap;
} s_bg;                                // what the grid shows (LVGL task reads it while drawing)
static lv_style_t s_st_bb[4];          // buttons: on, warn, off; focus (outline)
static bool s_st_bb_ready;

static void bingo_geom(int n, int *cell, int *gap, int *bw) {
    *cell = n == 4 ? 50 : 42;
    *gap = n == 4 ? 5 : 3;
    *bw = n * *cell + (n - 1) * *gap;
}

static bool clipped_out(const lv_layer_t *layer, const lv_area_t *a) {
    const lv_area_t *c = &layer->_clip_area;
    return a->x2 < c->x1 || a->x1 > c->x2 || a->y2 < c->y1 || a->y1 > c->y2;
}

static void fill(lv_layer_t *layer, const lv_area_t *a, uint32_t color, lv_opa_t opa, int radius) {
    if (clipped_out(layer, a)) return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = hex(color);
    r.bg_opa = opa;
    r.radius = radius;
    lv_draw_rect(layer, &r, a);
}

static void vgrad(lv_layer_t *layer, const lv_area_t *a, uint32_t top, uint32_t low) {
    if (clipped_out(layer, a)) return;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.bg_color = hex(top);
    r.bg_grad.dir = LV_GRAD_DIR_VER;
    r.bg_grad.stops_count = 2;
    r.bg_grad.stops[0] = (lv_grad_stop_t){.color = hex(top), .opa = LV_OPA_COVER, .frac = 0};
    r.bg_grad.stops[1] = (lv_grad_stop_t){.color = hex(low), .opa = LV_OPA_COVER, .frac = 255};
    lv_draw_rect(layer, &r, a);
}

// Header halves, the gloss line, the scallops and the rules (paper = screen bg).
static void bingo_paper_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    const int mid = CS_STATUS_H + BG_HEAD_H / 2;
    for (int y = BG_TOP + 6; y < SCR_H; y += 12) fill(layer, &(lv_area_t){0, y, SCR_W - 1, y}, BG_RULE, 255, 0);
    vgrad(layer, &(lv_area_t){0, CS_STATUS_H, SCR_W - 1, mid - 1}, 0x74AEF2, 0x4C90E6);
    vgrad(layer, &(lv_area_t){0, mid, SCR_W - 1, BG_TOP - 1}, 0x3585E2, 0x1A62CD);
    fill(layer, &(lv_area_t){0, mid, SCR_W - 1, mid}, 0x9CC8F7, 170, 0);
    for (int x = 5; x < SCR_W; x += 10) {
        fill(layer, &(lv_area_t){x - 4, BG_TOP - 4, x + 4, BG_TOP + 4}, BG_PAPER, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

static void bingo_check(lv_layer_t *layer, int cx, int cy, int r) {
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = hex(0xFFFFFF);
    l.width = r >= 8 ? 3 : 2;
    l.round_start = l.round_end = 1;
    const int x0 = cx - r / 2, y0 = cy, xm = cx - r / 8, ym = cy + (r * 2) / 5, x1 = cx + (r * 11) / 20,
              y1 = cy - (r * 9) / 20;
    l.p1 = (lv_point_precise_t){x0, y0};
    l.p2 = (lv_point_precise_t){xm, ym};
    lv_draw_line(layer, &l);
    l.p1 = (lv_point_precise_t){xm, ym};
    l.p2 = (lv_point_precise_t){x1, y1};
    lv_draw_line(layer, &l);
}

static void bingo_draw_cb(lv_event_t *e) {
    const cs_bingo_card_t *c = s_bg.card;
    if (!c || (c->n != 4 && c->n != 5)) return;
    static const struct { uint32_t bg, edge, ring, ink; } K[BCELL_KINDS] = {
        [BCELL_OPEN] = {0xFFFFFF, 0x78AAEB, 0xC6D6EC, 0x5F6A7B},
        [BCELL_FIXED] = {0xFFFCEE, 0xE6BF52, 0xE9C46A, 0x8A6A22},
        [BCELL_DONE] = {0xFFF4D2, 0x78AAEB, 0xF5B91E, 0x8E5E0C},
    };
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    const int n = c->n;
    int cell, gap, bw;
    bingo_geom(n, &cell, &gap, &bw);
    const int cyo = n == 4 ? 12 : 9;            // circle centre below the tile top
    const int rr = n == 4 ? 9 : 6;              // done disc radius (the empty circle is 1 smaller)
    const int edge = n == 4 ? 2 : 1;            // 5x5: thinner, so "TRASH" (41 px) fits in 42
    for (int i = 0; i < n * n; i++) {
        const uint32_t bit = 1u << i;
        const int k = (c->done & bit) ? BCELL_DONE : (c->fixed & bit) ? BCELL_FIXED : BCELL_OPEN;
        lv_area_t t;
        t.x1 = a.x1 + 4 + (i % n) * (cell + gap);
        t.y1 = a.y1 + 4 + (i / n) * (cell + gap);
        t.x2 = t.x1 + cell - 1;
        t.y2 = t.y1 + cell - 1;
        if (clipped_out(layer, &t)) continue;
        const bool cur = i == s_bg.cursor;
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = n == 4 ? 9 : 7;
        r.bg_opa = LV_OPA_COVER;
        r.bg_color = hex(K[k].bg);
        r.border_color = hex(cur ? (s_bg.swap ? BG_SWAP : BG_BLUE) : K[k].edge);
        r.border_width = cur ? edge + 1 : edge;
        r.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &r, &t);
        // The circle: a gold disc with a check once done, else an empty ring.
        const int cx = (t.x1 + t.x2 + 1) / 2, cy = t.y1 + cyo;
        lv_draw_rect_dsc_init(&r);
        r.radius = LV_RADIUS_CIRCLE;
        if (k == BCELL_DONE) {
            r.bg_opa = LV_OPA_COVER;
            r.bg_color = hex(K[k].ring);
            const lv_area_t d = {cx - rr, cy - rr, cx + rr, cy + rr};
            lv_draw_rect(layer, &r, &d);
            bingo_check(layer, cx, cy, rr);
        } else {
            r.bg_opa = LV_OPA_TRANSP;
            r.border_color = hex(K[k].ring);
            r.border_width = n == 4 ? 2 : 1;
            r.border_opa = LV_OPA_COVER;
            const lv_area_t d = {cx - rr + 1, cy - rr + 1, cx + rr - 1, cy + rr - 1};
            lv_draw_rect(layer, &r, &d);
        }
        // The label under it, two words on two lines, centred (by the caps) in
        // the space between the circle and the bottom border.
        char txt[CS_BINGO_LABEL_MAX + 1];
        snprintf(txt, sizeof txt, "%s", (s_bg.busy & bit) ? "..." : c->cells[i]);
        char *sp = strchr(txt, ' ');
        if (sp) *sp = '\n';
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.text = txt;
        l.text_local = 1;
        l.font = &cs_font12;
        l.color = hex(K[k].ink);
        l.align = LV_TEXT_ALIGN_CENTER;
        l.line_space = -3;
        lv_point_t sz;
        lv_text_get_size(&sz, txt, l.font, 0, l.line_space, cell, LV_TEXT_FLAG_NONE);
        if (sz.x > cell - 2 * edge - 4) {         // a wide word: 1 px tighter so it clears the border
            l.letter_space = -1;
            lv_text_get_size(&sz, txt, l.font, -1, l.line_space, cell, LV_TEXT_FLAG_NONE);
        }
        const int pad = l.font->line_height - l.font->base_line - 9;   // above the caps (9 px tall)
        const int top = cy + rr + 1, bot = t.y2 - edge - 2;   // room for the cursor's border
        const int ty = (top + bot + 1) / 2 - (sz.y - pad - l.font->base_line) / 2 - pad;
        lv_area_t ta = {t.x1, ty, t.x2, ty + sz.y - 1};
        lv_draw_label(layer, &l, &ta);
    }
    if (s_bg.cursor >= 0 && s_bg.cursor < n * n) {   // soft halo round the cursor tile
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = n == 4 ? 12 : 10;
        r.bg_opa = LV_OPA_TRANSP;
        r.border_color = hex(s_bg.swap ? BG_SWAP : BG_BLUE);
        r.border_width = 2;
        r.border_opa = 110;
        lv_area_t t;
        t.x1 = a.x1 + 1 + (s_bg.cursor % n) * (cell + gap);
        t.y1 = a.y1 + 1 + (s_bg.cursor / n) * (cell + gap);
        t.x2 = t.x1 + cell + 5;
        t.y2 = t.y1 + cell + 5;
        lv_draw_rect(layer, &r, &t);
    }
}

static void bingo_styles(void) {
    if (s_st_bb_ready) return;
    s_st_bb_ready = true;
    static const uint32_t BT[3][4] = {   // top, low, edge, text: on, warn, off
        {0x6CABF3, 0x1D66D1, 0x1756B5, 0xFFFFFF},
        {0xF79A90, 0xC9302A, 0x8E1F1A, 0xFFFFFF},
        {0xEEF3FA, 0xD3DEEC, 0xB4C5DB, 0x9AABC2},
    };
    for (int k = 0; k < 3; k++) {
        lv_style_t *b = &s_st_bb[k];
        lv_style_init(b);
        lv_style_set_radius(b, 6);
        lv_style_set_bg_opa(b, LV_OPA_COVER);
        lv_style_set_bg_color(b, hex(BT[k][0]));
        lv_style_set_bg_grad_color(b, hex(BT[k][1]));
        lv_style_set_bg_grad_dir(b, LV_GRAD_DIR_VER);
        lv_style_set_border_color(b, hex(BT[k][2]));
        lv_style_set_border_width(b, 1);
        lv_style_set_text_color(b, hex(BT[k][3]));
        lv_style_set_text_align(b, LV_TEXT_ALIGN_CENTER);
    }
    lv_style_t *f = &s_st_bb[3];                         // focus: a navy ring just outside
    lv_style_init(f);
    lv_style_set_outline_width(f, 2);
    lv_style_set_outline_pad(f, 1);
    lv_style_set_outline_color(f, hex(0x0D3F8F));
    lv_style_set_outline_opa(f, LV_OPA_COVER);
}

// A button is one label: shared style, the text centred by padding the label.
static lv_obj_t *bingo_button(int x, int y, int w, int h) {
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_add_style(l, &s_st_bb[0], 0);
    lv_obj_set_style_text_font(l, &cs_font14, 0);
    lv_obj_set_style_pad_top(l, (h - lv_font_get_line_height(&cs_font14)) / 2, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text_static(l, "");
    return l;
}

static void bingo_button_style(lv_obj_t *btn, const char *txt, bool on, bool focused, bool warn) {
    const int k = warn ? 1 : on ? 0 : 2;
    for (int i = 0; i < 4; i++) lv_obj_remove_style(btn, &s_st_bb[i], 0);
    lv_obj_add_style(btn, &s_st_bb[k], 0);
    if (focused) lv_obj_add_style(btn, &s_st_bb[3], 0);
    set_text(btn, txt ? txt : "");
}

static void bingo_build(void) {
    s_bg_n = s_bg_ovl = -1;
    memset(&s_bg, 0, sizeof s_bg);
    bingo_styles();
    lv_obj_set_style_bg_color(s_scr, hex(BG_PAPER), 0);
    lv_obj_t *paper = bare(s_scr);                          // header, scallops, rules
    lv_obj_set_pos(paper, 0, CS_STATUS_H);
    lv_obj_set_size(paper, SCR_W, SCR_H - CS_STATUS_H);
    lv_obj_add_event_cb(paper, bingo_paper_cb, LV_EVENT_DRAW_MAIN, NULL);
    msgs_status_art();
    s_o[BG_TITLE_SH] = label_left(s_scr, &cs_font18, 0x1B58B8, 14, CS_STATUS_H + 21);
    s_o[BG_TITLE] = label_left(s_scr, &cs_font18, 0xFFFFFF, 14, CS_STATUS_H + 22);
    lv_label_set_text_static(s_o[BG_TITLE_SH], "My Bingo");
    lv_label_set_text_static(s_o[BG_TITLE], "My Bingo");
    s_o[BG_LINE2] = label_left(s_scr, &cs_font12, 0xE2EEFF, 14, CS_STATUS_H + 38);
    lv_obj_set_width(s_o[BG_LINE2], 166);
    lv_label_set_long_mode(s_o[BG_LINE2], LV_LABEL_LONG_MODE_DOTS);
    // Progress ring: translucent light-blue track, white arc from 12 o'clock.
    lv_obj_t *arc = lv_arc_create(s_scr);
    lv_obj_remove_style_all(arc);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(arc, 188, CS_STATUS_H + 4);
    lv_obj_set_size(arc, 38, 38);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, hex(0xA8CCF8), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, 160, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_arc_set_value(arc, 0);
    s_o[BG_ARC] = arc;
    s_o[BG_PCT] = label_center(s_scr, &cs_font12, 0xFFFFFF, 207, 38, CS_STATUS_H + 27);
    lv_obj_t *body = bare(s_scr);
    lv_obj_set_pos(body, 0, BG_TOP);
    lv_obj_set_size(body, SCR_W, BG_BAR_Y - BG_TOP);
    s_o[BG_BODY] = body;
    for (int i = 0; i < 3; i++) s_o[BG_BTN0 + i] = bingo_button(22 + i * 68, BG_BAR_Y, 60, 22);
}

// The BINGO! note: Mines' sticky paper (a little more golden), the word in
// system blue, a small head line and the whisper (or "2 lines!"). Built only
// while it shows.
static void bingo_overlay(int ovl) {
    if (ovl == s_bg_ovl) return;
    s_bg_ovl = ovl;
    if (s_o[BG_OVL]) {
        lv_obj_delete(s_o[BG_OVL]);
        s_o[BG_OVL] = s_o[BG_OVL_W] = s_o[BG_OVL_HEAD] = s_o[BG_OVL_TXT] = NULL;
    }
    if (ovl == CS_BOVL_NONE) return;
    lv_obj_t *dim = rect(s_scr, 0, 0, SCR_W, SCR_H, 0x0A1E3C, 140, 0);
    rect(dim, 28, 68, 196, 196, 0x000000, 90, 4);                   // shadow
    lv_obj_t *paper = rect(dim, 22, 62, 196, 196, 0, LV_OPA_COVER, 3);
    grad(paper, 0xFFF4A8, 0xF8D862);
    rect(paper, 0, 0, 196, 14, 0xF0C53C, 110, 0);                   // the glued strip
    lv_obj_t *w = image(paper, &cs_mines_bingo, (196 - cs_mines_bingo.header.w) / 2, 20);
    lv_obj_set_style_image_recolor(w, hex(BG_BLUE), 0);
    lv_obj_set_style_image_recolor_opa(w, LV_OPA_COVER, 0);
    const int y = 20 + cs_mines_bingo.header.h;
    lv_obj_t *head = label_center(paper, &cs_font12, 0x8E5E0C, 98, 180, y + 20);
    lv_obj_t *txt = label_left(paper, &cs_font14, 0x1A5CC0, 10, y + 42);
    lv_obj_set_size(txt, 176, 196 - 8 - label_top(&cs_font14, y + 42));
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_DOTS);
    s_o[BG_OVL] = dim;
    s_o[BG_OVL_W] = w;
    s_o[BG_OVL_HEAD] = head;
    s_o[BG_OVL_TXT] = txt;
}

void cs_ui_bingo_update(const cs_bingo_view_t *v) {
    if (s_cur != CS_SCR_BINGO || !s_o[BG_BODY]) return;
    const cs_bingo_card_t *c = v->card;
    const int n = c && (c->n == 4 || c->n == 5) ? c->n : 0;
    if (n != s_bg_n) {
        lv_obj_clean(s_o[BG_BODY]);
        s_o[BG_GRID] = s_o[BG_MSG] = NULL;
        if (n) {
            int cell, gap, bw;
            bingo_geom(n, &cell, &gap, &bw);
            lv_obj_t *g = bare(s_o[BG_BODY]);     // tiles + halo, 4 px margin for the halo
            lv_obj_set_pos(g, (SCR_W - bw) / 2 - 4, (BG_BAR_Y - BG_TOP - bw) / 2 - 4);
            lv_obj_set_size(g, bw + 8, bw + 8);
            lv_obj_add_event_cb(g, bingo_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
            s_o[BG_GRID] = g;
        } else {
            lv_obj_t *m = label_center(s_o[BG_BODY], &cs_font14, 0x4A5A75, SCR_W / 2, 200, 104);
            lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
            s_o[BG_MSG] = m;
        }
        s_bg_n = n;
    }
    if (n) {
        // Redraw the grid only when something it shows changed.
        if (s_bg.card != c || s_bg.gen != v->gen || s_bg.busy != v->busy || s_bg.done != c->done ||
            s_bg.fixed != c->fixed || s_bg.cursor != v->cursor || s_bg.swap != v->swap) {
            s_bg.card = c;
            s_bg.gen = v->gen;
            s_bg.busy = v->busy;
            s_bg.done = c->done;
            s_bg.fixed = c->fixed;
            s_bg.cursor = v->cursor;
            s_bg.swap = v->swap;
            lv_obj_invalidate(s_o[BG_GRID]);
        }
    } else {
        s_bg.card = NULL;
        set_text(s_o[BG_MSG], v->msg);
    }
    set_text(s_o[BG_LINE2], v->line2);
    char b[8];
    snprintf(b, sizeof b, "%u%%", (unsigned)(v->pct > 100 ? 100 : v->pct));
    set_text(s_o[BG_PCT], b);
    if (lv_arc_get_value(s_o[BG_ARC]) != v->pct) lv_arc_set_value(s_o[BG_ARC], v->pct);
    for (int i = 0; i < 3; i++) {
        bingo_button_style(s_o[BG_BTN0 + i], v->btn[i], (v->btn_on >> i) & 1, v->cursor == n * n + i,
                           (v->btn_warn >> i) & 1);
    }
    bingo_overlay(v->overlay);
    set_text(s_o[BG_OVL_HEAD], v->head ? v->head : "");
    set_text(s_o[BG_OVL_TXT], v->note ? v->note : "");
}

// ---------------------------------------------------------------- astro dice
// Round 11b "water": the owner's lavender water photo full screen (from flash,
// with the frosted title bar and the frosted Roll pill baked in). The three
// results float on the water with no boxes: big white planet / sign symbols
// and the house number, each over a soft lavender glow (A8 masks from
// cs_astro_art), a small white name under each. Title bar: the wizard hat
// (send the roll to the companion) top left, the clock (History) top right. While a
// roll runs the results shimmer like reflections (cs_astro_fx: opacity,
// wobble, a faint ghost) and surface one by one. History: the frosted panel
// (baked) with the last ten rolls. Everything that changes is drawn in a few
// custom-draw objects; the animation redraws only the results band.
#define AS_INK 0x584E8C
#define AS_INK2 0x463C7D                 // the history symbols
#define AS_GLOW 0x7864C8
#define AS_TGLOW 0x6E5ABE                // glow under small white text
#define AS_SYM_CY 146                    // symbol centre line (the band centred between bar and pill)
#define AS_NAME_TOP 174                  // name label top (cs_font12)
#define AS_CAP_TOP 214                   // caption top (cs_font14)
#define AS_HAT_CX 22
#define AS_CLOCK_CX 218
#define AS_BAR_CY 34
#define AS_ROW_H 24
static cs_astro_view_t s_as;            // what the objects show (LVGL task reads it while drawing)
static cs_astro_hist_view_t s_ah;
static const int16_t AS_CX[CS_ASTRO_DICE] = {48, 120, 192};

static int s_as_letter;                 // letter spacing for the next as_label calls

static void as_label(lv_layer_t *layer, const char *txt, const lv_font_t *f, uint32_t color, lv_opa_t opa,
                     int x1, int x2, int y, lv_text_align_t align) {
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.letter_space = s_as_letter;
    l.text = txt;
    l.text_local = 1;
    l.font = f;
    l.color = hex(color);
    l.opa = opa;
    l.align = align;
    lv_area_t a = {x1, y, x2, y + f->line_height - 1};
    if (!clipped_out(layer, &a)) lv_draw_label(layer, &l, &a);
}

// White text over a soft lavender glow: the text drawn faintly round itself first.
static void as_glow_label(lv_layer_t *layer, const char *txt, const lv_font_t *f, int x1, int x2, int y, lv_opa_t opa) {
    static const int8_t O[8][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    const lv_opa_t g = (lv_opa_t)((uint32_t)opa * 38 / 255);
    for (int i = 0; i < 8; i++) {
        as_label(layer, txt, f, AS_TGLOW, g, x1 + O[i][0], x2 + O[i][0], y + O[i][1], LV_TEXT_ALIGN_CENTER);
    }
    as_label(layer, txt, f, 0xFFFFFF, opa, x1, x2, y, LV_TEXT_ALIGN_CENTER);
}

static void as_image(lv_layer_t *layer, const lv_image_dsc_t *img, int x, int y, uint32_t color, lv_opa_t opa) {
    if (!opa) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = img;
    d.opa = opa;
    if (img->header.cf == LV_COLOR_FORMAT_A8) {
        d.recolor = hex(color);
        d.recolor_opa = LV_OPA_COVER;
    }
    lv_area_t a = {x, y, x + img->header.w - 1, y + img->header.h - 1};
    if (!clipped_out(layer, &a)) lv_draw_image(layer, &d, &a);
}

// A mask centred on (cx, cy) with its glow under it.
static void as_glyph(lv_layer_t *layer, const lv_image_dsc_t *m, const lv_image_dsc_t *g, int cx, int cy,
                     lv_opa_t opa, lv_opa_t glow) {
    const int x = cx - m->header.w / 2, y = cy - m->header.h / 2;
    if (g) as_image(layer, g, x - CS_ASTRO_GLOW_M, y - CS_ASTRO_GLOW_M, AS_GLOW, (lv_opa_t)((uint32_t)glow * opa / 255));
    as_image(layer, m, x, y, 0xFFFFFF, opa);
}

static void as_ring(lv_layer_t *layer, const lv_area_t *a, int radius) {
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = radius;
    r.bg_opa = LV_OPA_TRANSP;
    r.border_color = hex(AS_INK);
    r.border_width = 2;
    r.border_opa = 150;
    lv_draw_rect(layer, &r, a);
}

// Title bar: the hat (left) and the clock (right), their focus rings, the sent badge.
static void as_top_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    as_image(layer, &cs_astro_hat, AS_HAT_CX - cs_astro_hat.header.w / 2, AS_BAR_CY - cs_astro_hat.header.h / 2, 0,
             LV_OPA_COVER);
    as_image(layer, &cs_astro_clock, AS_CLOCK_CX - cs_astro_clock.header.w / 2,
             AS_BAR_CY - cs_astro_clock.header.h / 2, AS_INK, LV_OPA_COVER);
    if (s_as.focus == 1) as_ring(layer, &(lv_area_t){AS_HAT_CX - 16, AS_BAR_CY - 15, AS_HAT_CX + 16, AS_BAR_CY + 15}, 9);
    if (s_as.focus == 2) {
        as_ring(layer, &(lv_area_t){AS_CLOCK_CX - 15, AS_BAR_CY - 15, AS_CLOCK_CX + 15, AS_BAR_CY + 15}, LV_RADIUS_CIRCLE);
    }
    if (s_as.sent) {                                         // sent: a small gold check on the hat
        const int bx = AS_HAT_CX + 12, by = AS_BAR_CY - 9;
        fill(layer, &(lv_area_t){bx - 6, by - 6, bx + 6, by + 6}, 0xE6B43C, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        bingo_check(layer, bx, by, 5);
    }
}

// The three results on the water.
static void as_dice_cb(lv_event_t *e) {
    static const char *const EMPTY[CS_ASTRO_DICE] = {"Planet", "Sign", "House"};
    lv_layer_t *layer = lv_event_get_layer(e);
    const bool moving = s_as.have && s_as.anim_ms < CS_ASTRO_ANIM_MS;
    for (int i = 0; i < CS_ASTRO_DICE; i++) {
        const int cx = AS_CX[i];
        if (!s_as.have) {
            as_glyph(layer, &cs_astro_sparkle, &cs_astro_sparkle_g, cx, AS_SYM_CY, 150, 255);
            as_glow_label(layer, EMPTY[i], &cs_font12, cx - 36, cx + 36, AS_NAME_TOP, 200);
            continue;
        }
        cs_astro_fx_t fx = {.opa = 255, .glow = 255};
        if (moving) cs_astro_fx(s_as.anim_ms, i, &fx);
        const uint8_t f = s_as.faces[i] % CS_ASTRO_FACES;
        const lv_image_dsc_t *m, *g;
        if (i == CS_ASTRO_PLANET) {
            m = CS_ASTRO_IMG_PLANET[f];
            g = CS_ASTRO_IMG_PLANET_G[f];
        } else if (i == CS_ASTRO_SIGN) {
            m = CS_ASTRO_IMG_SIGN[f];
            g = CS_ASTRO_IMG_SIGN_G[f];
        } else {
            m = CS_ASTRO_IMG_HOUSE[f];
            g = CS_ASTRO_IMG_HOUSE_G[f];
        }
        if (fx.ghost) as_glyph(layer, m, NULL, cx - 2 * fx.dx, AS_SYM_CY + 4 - fx.dy, fx.ghost, 0);   // reflection
        as_glyph(layer, m, g, cx + fx.dx, AS_SYM_CY + fx.dy, fx.opa, fx.glow);
        const char *name = i == CS_ASTRO_PLANET ? CS_ASTRO_PLANETS[f] : i == CS_ASTRO_SIGN ? CS_ASTRO_SIGNS[f] : "House";
        lv_point_t sz;
        lv_text_get_size(&sz, name, &cs_font12, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        s_as_letter = sz.x > 56 ? -1 : 0;                    // North Node, Sagittarius: a little tighter
        as_glow_label(layer, name, &cs_font12, cx - 38, cx + 38, AS_NAME_TOP, fx.opa);
        s_as_letter = 0;
    }
}

static void as_caption_cb(lv_event_t *e) {
    if (!s_as.caption[0]) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    as_glow_label(layer, s_as.caption, &cs_font14, 10, SCR_W - 11, AS_CAP_TOP, LV_OPA_COVER);
}

static void as_pill_cb(lv_event_t *e) {
    if (s_as.focus != 0) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    as_ring(layer, &(lv_area_t){CS_ASTRO_PILL_X0 - 3, CS_ASTRO_PILL_Y0 - 3, CS_ASTRO_PILL_X1 + 3, CS_ASTRO_PILL_Y1 + 3},
            CS_ASTRO_PILL_R + 3);
}

static lv_obj_t *as_layer_obj(int x, int y, int w, int h, lv_event_cb_t cb) {
    lv_obj_t *o = bare(s_scr);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

static void as_title(const char *t) {
    s_o[AS_TITLE] = label_center(s_scr, &cs_font18, AS_INK, SCR_W / 2, 150, AS_BAR_CY + 7);
    lv_label_set_text_static(s_o[AS_TITLE], t);
}

static void astro_build(void) {
    memset(&s_as, 0, sizeof s_as);
    msgs_status_art();
    as_title("Astro Dice");
    s_o[AS_TOP] = as_layer_obj(0, CS_ASTRO_BAR_Y0, SCR_W, CS_ASTRO_BAR_Y1 - CS_ASTRO_BAR_Y0, as_top_cb);
    s_o[AS_DICE] = as_layer_obj(0, AS_SYM_CY - 41, SCR_W, AS_NAME_TOP + 20 - (AS_SYM_CY - 41), as_dice_cb);
    s_o[AS_CAP] = as_layer_obj(0, AS_CAP_TOP - 5, SCR_W, 26, as_caption_cb);
    s_o[AS_PILL] = as_layer_obj(CS_ASTRO_PILL_X0 - 6, CS_ASTRO_PILL_Y0 - 6, CS_ASTRO_PILL_X1 - CS_ASTRO_PILL_X0 + 13,
                                CS_ASTRO_PILL_Y1 - CS_ASTRO_PILL_Y0 + 13, as_pill_cb);
    lv_obj_t *roll = label_center(s_scr, &cs_font18, AS_INK, (CS_ASTRO_PILL_X0 + CS_ASTRO_PILL_X1) / 2, 90,
                                  (CS_ASTRO_PILL_Y0 + CS_ASTRO_PILL_Y1) / 2 + 7);
    lv_label_set_text_static(roll, "Roll");
}

void cs_ui_astro_update(const cs_astro_view_t *v) {
    if (s_cur != CS_SCR_ASTRO || !s_o[AS_DICE]) return;
    if (s_as.have != v->have || s_as.anim_ms != v->anim_ms || memcmp(s_as.faces, v->faces, sizeof v->faces) != 0) {
        lv_obj_invalidate(s_o[AS_DICE]);
    }
    if (s_as.focus != v->focus || s_as.sent != v->sent) {
        lv_obj_invalidate(s_o[AS_TOP]);
        lv_obj_invalidate(s_o[AS_PILL]);
    }
    if (strcmp(s_as.caption, v->caption) != 0) lv_obj_invalidate(s_o[AS_CAP]);
    s_as = *v;
}

// History: "< Back", the title, the frosted panel (baked) and ten rows.
static void as_back_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = hex(AS_INK);
    l.width = 2;
    l.round_start = l.round_end = 1;
    l.p1 = (lv_point_precise_t){16, AS_BAR_CY - 4};
    l.p2 = (lv_point_precise_t){12, AS_BAR_CY};
    lv_draw_line(layer, &l);
    l.p1 = (lv_point_precise_t){12, AS_BAR_CY};
    l.p2 = (lv_point_precise_t){16, AS_BAR_CY + 4};
    lv_draw_line(layer, &l);
    as_label(layer, "Back", &cs_font12, AS_INK, LV_OPA_COVER, 21, 80, AS_BAR_CY - 8, LV_TEXT_ALIGN_LEFT);
}

static void as_rows_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t o;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &o);
    if (!s_ah.n) {
        as_label(layer, "No rolls yet", &cs_font14, AS_INK, LV_OPA_COVER, o.x1, o.x2, o.y1 + 100, LV_TEXT_ALIGN_CENTER);
        return;
    }
    for (int i = 0; i < s_ah.n && i < CS_ASTRO_LOG; i++) {
        const int y = o.y1 + 4 + i * AS_ROW_H, cy = y + AS_ROW_H / 2;
        if (i) fill(layer, &(lv_area_t){o.x1 + 10, y, o.x2 - 10, y}, 0xFFFFFF, LV_OPA_COVER, 0);
        as_label(layer, s_ah.rows[i].when, &cs_font12, AS_INK, LV_OPA_COVER, o.x1 + 10, o.x1 + 128, cy - 9,
                 LV_TEXT_ALIGN_LEFT);
        const uint8_t *f = s_ah.rows[i].f;
        const lv_image_dsc_t *p = CS_ASTRO_IMG_PLANET_S[f[0] % CS_ASTRO_FACES];
        const lv_image_dsc_t *s = CS_ASTRO_IMG_SIGN_S[f[1] % CS_ASTRO_FACES];
        as_image(layer, p, o.x1 + 138 - p->header.w / 2, cy - p->header.h / 2, AS_INK2, LV_OPA_COVER);
        as_image(layer, s, o.x1 + 160 - s->header.w / 2, cy - s->header.h / 2, AS_INK2, LV_OPA_COVER);
        char h[4];
        snprintf(h, sizeof h, "%d", f[2] % CS_ASTRO_FACES + 1);
        as_label(layer, h, &cs_font14, AS_INK2, LV_OPA_COVER, o.x1 + 174, o.x2 - 6, cy - 11, LV_TEXT_ALIGN_LEFT);
    }
}

static void astro_hist_build(void) {
    memset(&s_ah, 0, sizeof s_ah);
    msgs_status_art();
    image(s_scr, &cs_astro_hist, CS_ASTRO_PANEL_X0, CS_ASTRO_PANEL_Y0);
    as_title("History");
    as_layer_obj(0, CS_ASTRO_BAR_Y0, 90, CS_ASTRO_BAR_Y1 - CS_ASTRO_BAR_Y0, as_back_cb);
    s_o[AS_ROWS] = as_layer_obj(CS_ASTRO_PANEL_X0, CS_ASTRO_PANEL_Y0, CS_ASTRO_PANEL_X1 - CS_ASTRO_PANEL_X0,
                                CS_ASTRO_PANEL_Y1 - CS_ASTRO_PANEL_Y0, as_rows_cb);
}

void cs_ui_astro_hist_update(const cs_astro_hist_view_t *v) {
    if (s_cur != CS_SCR_ASTRO_HIST || !s_o[AS_ROWS]) return;
    s_ah = *v;
    lv_obj_invalidate(s_o[AS_ROWS]);
}

void cs_ui_refresh(void) { tick_cb(NULL); }

cs_screen_t cs_ui_current(void) { return s_cur; }

void cs_ui_detach(void) {
    s_cur = CS_SCR_NONE;
    s_scr = NULL;
    s_name = s_fan = s_time = s_batt = NULL;
    memset(s_o, 0, sizeof s_o);
    memset(s_mcell, 0, sizeof s_mcell);
    s_mn_built = -1;
    s_bg_n = -1;
    s_bg.card = NULL;
}

void cs_ui_show(cs_screen_t s) {
    static const lv_image_dsc_t *const BG[] = {
        [CS_SCR_LOCK] = &cs_img_bg_lock, [CS_SCR_HOME] = &cs_img_bg_home,
        [CS_SCR_SETTINGS] = &cs_img_bg_settings, [CS_SCR_POMO_PICK] = &cs_img_bg_pomo_pick,
        [CS_SCR_POMO_RUN] = &cs_img_bg_pomo_run, [CS_SCR_POMO_DONE] = &cs_img_bg_pomo_done,
        [CS_SCR_WEATHER] = &cs_img_bg_weather, [CS_SCR_CALL] = &cs_img_bg_call,
        [CS_SCR_MSGS] = NULL,   // drawn with LVGL (msgs_build)
        [CS_SCR_NOTES] = NULL, [CS_SCR_MEMO] = NULL, [CS_SCR_MINES] = NULL, [CS_SCR_BINGO] = NULL,
        [CS_SCR_ASTRO] = &cs_astro_bg, [CS_SCR_ASTRO_HIST] = &cs_astro_bg,
    };
    if (s == CS_SCR_NONE) return;
    lv_obj_t *old = lv_screen_active();
    cs_ui_detach();
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_bg_color(s_scr, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    // Load the empty screen and free the old one before building children, so
    // the 32 KB LVGL pool never holds two populated screens at once.
    lv_screen_load(s_scr);
    if (old && old != s_scr) lv_obj_delete(old);
    if (BG[s]) image(s_scr, BG[s], 0, 0);
    switch (s) {
    case CS_SCR_LOCK: lock_build(); break;
    case CS_SCR_HOME: home_build(); break;
    case CS_SCR_SETTINGS: settings_build(); break;
    case CS_SCR_POMO_PICK: pick_build(); break;
    case CS_SCR_POMO_RUN: run_build(); break;
    case CS_SCR_WEATHER: wx_build(); break;
    case CS_SCR_CALL: call_build(); break;
    case CS_SCR_MSGS: msgs_build(); break;
    case CS_SCR_NOTES: notes_build(); break;
    case CS_SCR_MEMO: memo_build(); break;
    case CS_SCR_MINES: mines_build(); break;
    case CS_SCR_BINGO: bingo_build(); break;
    case CS_SCR_ASTRO: astro_build(); break;
    case CS_SCR_ASTRO_HIST: astro_hist_build(); break;
    default: break;
    }
    status_build();
    s_cur = s;
    if (s == CS_SCR_POMO_PICK) pick_refresh();
    cs_ui_refresh();
}

// ---------------------------------------------------------------- overlays
static void alert_hide_cb(lv_timer_t *t) {
    (void)t;
    s_alert_timer = NULL;   // one-shot timers delete themselves
    cs_ui_alert(false);
}

void cs_ui_alert(bool on) {
    if (on) {
        if (!s_alert) {
            s_alert = image(lv_layer_top(), &cs_img_alert, (SCR_W - CS_ALERT_W) / 2, 132);
        }
        if (s_alert_timer) {
            lv_timer_reset(s_alert_timer);
        } else {
            s_alert_timer = lv_timer_create(alert_hide_cb, CS_ALERT_MS, NULL);
            lv_timer_set_repeat_count(s_alert_timer, 1);
        }
        return;
    }
    if (s_alert_timer) {
        lv_timer_delete(s_alert_timer);
        s_alert_timer = NULL;
    }
    if (s_alert) {
        lv_obj_delete(s_alert);
        s_alert = NULL;
    }
}

bool cs_ui_alert_visible(void) { return s_alert != NULL; }

static void toast_hide_cb(lv_timer_t *t) {
    (void)t;
    s_toast_timer = NULL;
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

void cs_ui_toast(const char *text, uint32_t ms) {
    if (!s_toast) {
        s_toast = rect(lv_layer_top(), 30, 20, 180, 28, 0x101014, 215, 14);
        lv_obj_set_style_border_color(s_toast, hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_border_opa(s_toast, LV_OPA_40, 0);
        lv_obj_t *l = label_center(s_toast, &cs_font14, 0xFFFFFF, 90, 176, 19);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(lv_obj_get_child(s_toast, 0), text);
    if (s_toast_timer) {
        lv_timer_set_period(s_toast_timer, ms);
        lv_timer_reset(s_toast_timer);
    } else {
        s_toast_timer = lv_timer_create(toast_hide_cb, ms, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
}
