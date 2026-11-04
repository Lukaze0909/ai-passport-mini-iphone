// Host-side renderer: runs the real main/classic/cs_ui.c on desktop LVGL 9.5.0
// with fake view data and dumps 240x320 frames as PPM. Developer tool only (not
// part of validate.sh); it says nothing about the panel, timing or memory.
// See tools/classic/host_render.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cs_ui.h"
#include "lvgl.h"

#define W 240
#define H 320
static uint16_t s_fb[W * H];
static uint16_t s_buf[W * H];
static uint32_t s_ms;
static cs_view_t s_view;
static cs_weather_t s_wx;

void cs_view_get(cs_view_t *v) { *v = s_view; }
bool cs_view_weather(cs_weather_t *out) { *out = s_wx; return s_wx.valid; }

static uint32_t tick_cb(void) { return s_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)px;
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++) memcpy(&s_fb[y * W + a->x1], src + (y - a->y1) * w, (size_t)w * 2);
    lv_display_flush_ready(d);
}

static void dump(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = s_fb[i];
        // Same corner mask as the firmware (30 px radius, outside = black).
        const int x = i % W, y = i / W;
        int dx = x < 30 ? 30 - x : x > W - 31 ? x - (W - 31) : 0;
        int dy = y < 30 ? 30 - y : y > H - 31 ? y - (H - 31) : 0;
        const int out = dx * dx + dy * dy > 30 * 30;
        unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31),
                                (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                (unsigned char)((c & 31) * 255 / 31)};
        if (out) rgb[0] = rgb[1] = rgb[2] = 0;
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void pool(const char *what) {
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    printf("lvgl pool %s: used %u B (%u%%), max_used %u\n", what, (unsigned)(m.total_size - m.free_size),
           (unsigned)m.used_pct, (unsigned)m.max_used);
    fflush(stdout);
}

static void run(uint32_t ms) {
    const uint32_t end = s_ms + ms;
    while (s_ms < end) { s_ms += 5; lv_timer_handler(); }
    lv_refr_now(NULL);
}

static const char WX[] =
    "{\"city\":\"上海\",\"temp\":17,\"hi\":19,\"lo\":13,\"cond\":\"多云\",\"icon\":\"cloudy\","
    "\"aqi\":62,\"aqi_level\":\"良\",\"updated\":\"22:55\",\"days\":[{\"d\":\"周日\",\"icon\":"
    "\"sunny\",\"hi\":20,\"lo\":12},{\"d\":\"周一\",\"icon\":\"cloudy\",\"hi\":18,\"lo\":13},"
    "{\"d\":\"周二\",\"icon\":\"rain\",\"hi\":16,\"lo\":12}]}";

static cs_msg_t sample(uint32_t ts, bool out, uint16_t dur, const char *text) {
    cs_msg_t m;
    memset(&m, 0, sizeof m);
    m.ts = ts;
    m.out = out;
    m.dur_s = dur;
    snprintf(m.text, sizeof m.text, "%s", text);
    return m;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "CST-8", 1);   // the device's zone (time stamps in the Messages thread)
    tzset();
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(W, H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, s_buf, NULL, sizeof s_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    s_view = (cs_view_t){.time_valid = true, .mon = 10, .mday = 31, .wday = 6, .hour = 22, .min = 47,
                         .link = CS_LINK_HOME, .mqtt = true, .batt = 87};
    cs_weather_parse(WX, strlen(WX), &s_wx);
    s_view.wx_valid = true;
    s_view.wx_temp = 17;
    s_view.wx_version = 1;
    cs_ui_init();

    cs_ui_show(CS_SCR_LOCK); run(300); dump(out, "01_lock");
    s_view.pomo_running = true; s_view.pomo_left_s = 754; s_view.pomo_total_s = 900; s_view.pomo_permille = 162;
    cs_ui_home_select(CS_HOME_RELEASE); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "02_home");
    cs_ui_home_select(CS_HOME_CALL); run(300); dump(out, "03_home_dock");
    cs_ui_alert(true); run(300); dump(out, "04_alert");
    cs_ui_alert(false);
    cs_ui_show(CS_SCR_SETTINGS); run(300); dump(out, "05_settings");
    s_view.link = CS_LINK_OFF; s_view.mqtt = false; run(300); dump(out, "06_settings_off");
    s_view.link = CS_LINK_NO_CREDS; run(300); dump(out, "07_settings_nocreds");
    s_view.link = CS_LINK_HOME; s_view.mqtt = true;
    cs_ui_pomo_pick(3); cs_ui_show(CS_SCR_POMO_PICK); run(300); dump(out, "08_pomo_pick");
    cs_ui_pomo_pick(0); run(300); dump(out, "09_pomo_pick_first");
    cs_ui_show(CS_SCR_POMO_RUN); run(300); dump(out, "10_pomo_run");
    s_view.pomo_left_s = 3600; s_view.pomo_total_s = 3600; s_view.pomo_long = true; s_view.pomo_permille = 0;
    run(300); dump(out, "11_pomo_run_60");
    cs_ui_show(CS_SCR_POMO_DONE); run(300); dump(out, "12_pomo_done");
    s_view.pomo_running = false;
    cs_ui_show(CS_SCR_WEATHER); run(300); dump(out, "13_weather");
    s_wx.valid = false; s_view.wx_valid = false; s_view.wx_version = 2; s_view.link = CS_LINK_OFF;
    run(300); dump(out, "14_weather_offline");
    s_view.link = CS_LINK_HOME; run(300); dump(out, "15_weather_waiting");
    cs_ui_show(CS_SCR_CALL);
    cs_call_view_t cv = {.cursor = CS_CALL_TALK, .enabled = true, .recording = true};
    snprintf(cv.status, sizeof cv.status, "正在说… 0:07");
    cs_ui_call_update(&cv); run(300); dump(out, "16_call_rec");
    cv.recording = false; cv.cursor = CS_CALL_SEND;
    snprintf(cv.status, sizeof cv.status, "说完了 · 0:07 — 发送 还是 删除？");
    cs_ui_call_update(&cv); run(300); dump(out, "17_call_stopped");
    cv.cursor = CS_CALL_END; cv.speaker = true;
    snprintf(cv.status, sizeof cv.status, CS_COMPANION_NAME " 正在说… 今天过得怎么样？");
    cs_ui_call_update(&cv); cs_ui_toast(CS_COMPANION_NAME " 来消息了", 2000); run(300); dump(out, "18_call_end_toast");
    cv.enabled = false; cv.cursor = CS_CALL_TALK; cv.speaker = false;
    snprintf(cv.status, sizeof cv.status, "先在设置里打开 Wi-Fi");
    cs_ui_call_update(&cv); run(2600); dump(out, "19_call_disabled");
    s_view.time_valid = false; s_view.link = CS_LINK_CONNECTING; s_view.batt = 12;
    cs_ui_show(CS_SCR_LOCK); run(300); dump(out, "20_lock_nosync");
    s_view.time_valid = true; s_view.link = CS_LINK_HOME; s_view.batt = 87;
    // Messages: a thread both ways, time stamps between groups (2025-11-01, CST).
    const uint32_t t0 = 1761969600u;   // 12:00
    const cs_msg_t thread[] = {
        sample(t0, false, 0, "Good morning! Light rain after lunch, so maybe take the small umbrella."),
        sample(t0 + 95, true, 7, ""),
        sample(t0 + 140, false, 0, "Got it. I'll remind you at 12:30. Have a nice walk!"),
        sample(t0 + 7300, true, 12, ""),
        sample(t0 + 7360, false, 0, "Haha, that sounds like the cat again. Did she steal the sock?"),
        sample(t0 + 7420, true, 4, ""),
    };
    cs_msgs_view_t mv = {.msgs = thread, .n = 6, .more_above = true, .gen = 1, .bar = 0, .rec = 0,
                         .enabled = true};
    cs_ui_show(CS_SCR_MSGS); cs_ui_msgs_update(&mv); run(300); dump(out, "21_messages");
    mv.rec = 2; mv.bar = 1;
    snprintf(mv.status, sizeof mv.status, "说完了 · 0:06 — 发送 还是 删除？");
    cs_ui_msgs_update(&mv); run(300); dump(out, "22_messages_take");
    mv.rec = 1; mv.bar = 0;
    snprintf(mv.status, sizeof mv.status, "正在说… 0:03");
    cs_ui_msgs_update(&mv); run(300); dump(out, "23_messages_rec");
    const cs_msg_t one[] = {
        sample(t0 + 9000, false, 0,
               "Long one to check wrapping: the device keeps the newest fifty messages in flash, so a "
               "reboot does not lose them. Scroll with UP and DOWN; hold OK to go back."),
    };
    mv = (cs_msgs_view_t){.msgs = one, .n = 1, .gen = 2, .enabled = true};
    cs_ui_msgs_update(&mv); run(300); dump(out, "24_messages_long");
    s_view.msgs_unread = 3;
    cs_ui_home_select(CS_HOME_MESSAGES); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "25_home_unread");
    cs_ui_toast(CS_COMPANION_NAME ": Haha, that sounds like the cat again. Did she steal the sock?", 2000);
    run(300); dump(out, "26_home_unread_toast");
    s_view.msgs_unread = 0;
    run(2200);   // let the toast expire
    // Notes: the list (some checked), the "+" focus, and the new-memo states.
    static cs_notes_view_t nv;
    static const struct { const char *t; bool done; } memos[] = {
        {"Call the vet about the cat's booster", false},
        {"Buy oat milk and eggs", true},
        {"Back up the trip photos to the cloud folder before Sunday night, the big ones too", false},
        {"Water the basil", true},
        {"Book the dentist", false},
        {"Return the library book", false},
    };
    memset(&nv, 0, sizeof nv);
    nv.count = 6;
    nv.n = 6;
    nv.cursor = 2;
    for (int i = 0; i < 6; i++) {
        snprintf(nv.rows[i].text, sizeof nv.rows[i].text, "%s", memos[i].t);
        nv.rows[i].done = memos[i].done;
    }
    cs_ui_show(CS_SCR_NOTES); cs_ui_notes_update(&nv); run(300); dump(out, "27_notes_list");
    nv.cursor = -1;
    cs_ui_notes_update(&nv); run(300); dump(out, "28_notes_plus");
    cs_memo_view_t mv2 = {.state = CS_MEMO_REC, .focus = 0, .enabled = 1, .text = ""};
    snprintf(mv2.status, sizeof mv2.status, "正在说… 0:04");
    cs_ui_show(CS_SCR_MEMO); cs_ui_memo_update(&mv2); run(300); dump(out, "29_memo_rec");
    mv2 = (cs_memo_view_t){.state = CS_MEMO_WAIT, .focus = 2, .enabled = 4, .text = ""};
    cs_ui_memo_update(&mv2); run(300); dump(out, "30_memo_wait");
    mv2 = (cs_memo_view_t){.state = CS_MEMO_PREVIEW, .focus = 1, .enabled = 6,
                           .text = "Pick up the dry cleaning on the way home tomorrow."};
    cs_ui_memo_update(&mv2); run(300); dump(out, "31_memo_preview");
    mv2 = (cs_memo_view_t){.state = CS_MEMO_FAIL, .focus = 0, .enabled = 5, .text = ""};
    cs_ui_memo_update(&mv2); run(300); dump(out, "32_memo_fail"); pool("32_memo_fail");
    // Mines: home icon, setup card, both fields mid-game, the end notes, the reveal.
    cs_ui_home_select(CS_HOME_MINES); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "33_home_mines");
    static cs_mines_view_t gv;
    memset(&gv, 0, sizeof gv);
    gv.setup = true;
    gv.setup_focus = 2;
    snprintf(gv.size_txt, sizeof gv.size_txt, "4x4 · 3");
    snprintf(gv.first_txt, sizeof gv.first_txt, "You");
    snprintf(gv.score_txt, sizeof gv.score_txt, "You 2 : 3 " CS_COMPANION_NAME);
    cs_ui_show(CS_SCR_MINES); cs_ui_mines_update(&gv); run(300); dump(out, "34_mines_setup"); pool("34_mines_setup");
    gv.setup_focus = 0;
    cs_ui_mines_update(&gv); run(300); dump(out, "35_mines_setup_size");
    memset(&gv, 0, sizeof gv);
    gv.n = 4;
    gv.field = 0;
    gv.cursor = 6;
    gv.cells[0] = CS_MCELL_FLAG; gv.cells[6] = CS_MCELL_FLAG; gv.cells[15] = CS_MCELL_FLAG;
    gv.cells[5] = CS_MCELL_FOOT; gv.cells[9] = CS_MCELL_FOOT;
    gv.btn[0] = "Flip"; gv.btn[1] = "Quit"; gv.btn_on = 3;
    snprintf(gv.status, sizeof gv.status, "Your turn");
    snprintf(gv.note, sizeof gv.note, "\"She avoided corners last time... so B2. Brave of me.\"");
    gv.note_kind = CS_MNOTE_COMPANION;
    cs_ui_mines_update(&gv); run(300); dump(out, "36_mines_her_field"); pool("36_mines_her_field");
    gv.field = 1;
    for (int i = 0; i < 16; i++) gv.cells[i] = CS_MCELL_GREY;
    gv.cells[2] = CS_MCELL_GREEN; gv.cells[7] = CS_MCELL_GREEN; gv.cells[12] = CS_MCELL_WAIT;
    gv.cursor = 16;
    snprintf(gv.status, sizeof gv.status, "...");
    cs_ui_mines_update(&gv); run(300); dump(out, "37_mines_companion_field"); pool("37_mines_companion_field");
    memset(&gv, 0, sizeof gv);
    gv.n = 5;
    gv.field = 0;
    gv.cursor = 12;
    gv.cells[3] = CS_MCELL_FLAG; gv.cells[8] = CS_MCELL_FLAG; gv.cells[20] = CS_MCELL_FLAG;
    gv.cells[12] = CS_MCELL_FLAG;
    gv.btn[0] = "Go"; gv.btn[1] = "Sure?"; gv.btn_on = 3; gv.btn_warn = 2;
    snprintf(gv.status, sizeof gv.status, "4/4 hidden");
    snprintf(gv.note, sizeof gv.note, CS_COMPANION_NAME "'s mines are locked in.\nsha256 3fa91b2c0d4e5f67");
    cs_ui_mines_update(&gv); run(300); dump(out, "38_mines_place5");
    pool("5x5 place");
    memset(&gv, 0, sizeof gv);
    gv.n = 3;
    gv.field = 1;
    gv.cursor = 9;
    for (int i = 0; i < 9; i++) gv.cells[i] = CS_MCELL_GREY;
    gv.cells[0] = CS_MCELL_GREEN; gv.cells[1] = CS_MCELL_BOOM; gv.cells[8] = CS_MCELL_MINE;
    gv.btn[0] = "Flip"; gv.btn[1] = "Again"; gv.btn_on = 3;
    snprintf(gv.status, sizeof gv.status, "You lost");
    snprintf(gv.note, sizeof gv.note, CS_COMPANION_NAME "'s mines: A2 C3  verified ✓\nsha256 3fa91b2c0d4e5f67");
    gv.note_kind = CS_MNOTE_OK;
    gv.overlay = CS_MOVL_DEAD;
    gv.owner_sent = true;
    cs_ui_mines_update(&gv); run(300); dump(out, "39_mines_dead"); pool("39_mines_dead");
    gv.overlay = CS_MOVL_DEAD_AGAIN;
    cs_ui_mines_update(&gv); run(300); dump(out, "40_mines_dead_again");
    gv.overlay = CS_MOVL_WIN;
    cs_ui_mines_update(&gv); run(300); dump(out, "41_mines_win");
    gv.overlay = CS_MOVL_NONE;
    cs_ui_mines_update(&gv); run(300); dump(out, "42_mines_reveal"); pool("42_mines_reveal");
    snprintf(gv.note, sizeof gv.note, CS_COMPANION_NAME "'s mines: A2 C3  MISMATCH!\nsha256 3fa91b2c0d4e5f67");
    gv.note_kind = CS_MNOTE_BAD;
    cs_ui_mines_update(&gv); run(300); dump(out, "43_mines_mismatch");
    // Bingo: home icon with today's check, 4x4 mid-day, 5x5 swapping, the notes, no card.
    s_view.bingo_done = true;
    cs_ui_home_select(CS_HOME_CALENDAR); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "44_home_bingo");
    s_view.bingo_done = false;
    static cs_bingo_card_t bc;
    static const char *const L16[16] = {"5 CUPS", "MAG", "SUN", "LESS SCR", "UP<10", "TIDY", "GAIN BK", "MEALS",
                                        "YAY ME", "STRCH", "BED<1", "LNDRY", "FLOOR", "3 GOOD", "GAME OK", "FAM"};
    memset(&bc, 0, sizeof bc);
    snprintf(bc.day, sizeof bc.day, "2026-11-02");
    bc.n = 4;
    for (int i = 0; i < 16; i++) snprintf(bc.cells[i], sizeof bc.cells[i], "%s", L16[i]);
    bc.fixed = (1u << 1) | (1u << 4) | (1u << 7) | (1u << 10);
    bc.done = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 12) | (1u << 2);
    bc.autod = 1u << 4;
    static cs_bingo_view_t bv;
    memset(&bv, 0, sizeof bv);
    bv.card = &bc;
    bv.gen = 1;
    bv.cursor = 6;
    bv.btn[0] = "Swap"; bv.btn[1] = "4x4"; bv.btn[2] = CS_COMPANION_NAME; bv.btn_on = 3;
    snprintf(bv.line2, sizeof bv.line2, "Mon Nov 2 · 5 of 16");
    bv.pct = 31;
    cs_ui_show(CS_SCR_BINGO); cs_ui_bingo_update(&bv); run(300); dump(out, "45_bingo_4x4"); pool("45_bingo_4x4");
    static const char *const L25[25] = {"MAG", "5 CUPS", "VEG", "NO ICE", "MEALS", "UP<10", "BATH", "HAIR",
                                        "FOOT", "BED<1", "SLEPT", "EYES", "OUT", "SUN", "SWIM", "BOX", "WALK",
                                        "STRCH", "NO SIT", "DISH", "TRASH", "LNDRY", "TIDY", "FLOOR", "PARCL"};
    memset(&bc, 0, sizeof bc);
    snprintf(bc.day, sizeof bc.day, "2026-11-02");
    bc.n = 5;
    for (int i = 0; i < 25; i++) snprintf(bc.cells[i], sizeof bc.cells[i], "%s", L25[i]);
    bc.fixed = 1u | (1u << 4) | (1u << 5) | (1u << 9);
    bc.done = 1u | (1u << 5) | (1u << 12) | (1u << 6) | (1u << 18) | (1u << 24) | (1u << 20);
    bv.gen = 2;
    bv.cursor = 13;
    bv.swap = true;
    bv.busy = 1u << 22;
    bv.btn[0] = "Cancel"; bv.btn[1] = "5x5"; bv.btn_on = 3;
    snprintf(bv.line2, sizeof bv.line2, "Pick a cell to swap");
    bv.pct = 28;
    cs_ui_bingo_update(&bv); run(300); dump(out, "46_bingo_5x5_swap"); pool("46_bingo_5x5_swap");
    bv.swap = false;
    bv.busy = 0;
    bc.done |= (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);   // row 1
    bv.cursor = 3;
    bv.btn[0] = "Swap"; bv.btn_on = 7;
    snprintf(bv.line2, sizeof bv.line2, "Mon Nov 2 · 11 of 25");
    bv.pct = 44;
    bv.overlay = CS_BOVL_FIRST;
    bv.head = CS_COMPANION_NAME " whispers:";
    bv.note = "Look at you go! Water, sun, and a whole row. Nicely done.";
    cs_ui_bingo_update(&bv); run(300); dump(out, "47_bingo_first"); pool("47_bingo_first");
    bv.note = "The note is on its way...";
    cs_ui_bingo_update(&bv); run(300); dump(out, "48_bingo_waiting");
    bv.overlay = CS_BOVL_AGAIN;
    bv.head = "";
    bv.note = "2 lines!";
    cs_ui_bingo_update(&bv); run(300); dump(out, "49_bingo_again");
    bv.overlay = CS_BOVL_NONE;
    cs_ui_bingo_update(&bv); run(300); dump(out, "50_bingo_5x5_line"); pool("50_bingo_5x5_line");
    memset(&bv, 0, sizeof bv);
    bv.cursor = 1;
    bv.btn[0] = "Swap"; bv.btn[1] = "4x4"; bv.btn[2] = CS_COMPANION_NAME; bv.btn_on = 2;
    snprintf(bv.line2, sizeof bv.line2, "Mon Nov 2");
    snprintf(bv.msg, sizeof bv.msg, "New day! Today's card needs Wi-Fi.");
    cs_ui_show(CS_SCR_BINGO); cs_ui_bingo_update(&bv); run(300); dump(out, "51_bingo_nocard");
    // 4x4 with the first column done (the approved mockup's second state), focus on the companion.
    memset(&bc, 0, sizeof bc);
    snprintf(bc.day, sizeof bc.day, "2026-11-02");
    bc.n = 4;
    for (int i = 0; i < 16; i++) snprintf(bc.cells[i], sizeof bc.cells[i], "%s", L16[i]);
    bc.fixed = (1u << 1) | (1u << 4) | (1u << 7) | (1u << 10);
    bc.done = 1u | (1u << 4) | (1u << 8) | (1u << 12) | (1u << 1) | (1u << 5);
    memset(&bv, 0, sizeof bv);
    bv.card = &bc;
    bv.gen = 3;
    bv.cursor = 16 + 2;
    bv.btn[0] = "Swap"; bv.btn[1] = "4x4"; bv.btn[2] = CS_COMPANION_NAME; bv.btn_on = 7;
    snprintf(bv.line2, sizeof bv.line2, "Mon Nov 2 · 6 of 16");
    bv.pct = 38;
    cs_ui_show(CS_SCR_BINGO); cs_ui_bingo_update(&bv); run(300); dump(out, "52_bingo_4x4_line"); pool("52_bingo_4x4_line");
    // Home: placeholders dimmed (the cursor skips them); the cursor on Weather and on Settings.
    cs_ui_home_select(CS_HOME_WEATHER); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "53_home_weather"); pool("53_home_weather");
    cs_ui_home_select(CS_HOME_SETTINGS); run(300); dump(out, "54_home_settings");
    // Astro dice (11b water): home icon, idle, shimmer, surfacing, result, sent, long names, History.
    cs_ui_home_select(CS_HOME_ASTRO); cs_ui_show(CS_SCR_HOME); run(300); dump(out, "55_home_astro");
    static cs_astro_view_t av;
    memset(&av, 0, sizeof av);
    av.anim_ms = CS_ASTRO_ANIM_MS;
    snprintf(av.caption, sizeof av.caption, "Press OK to roll");
    cs_ui_show(CS_SCR_ASTRO); cs_ui_astro_update(&av); run(300); dump(out, "56_astro_idle"); pool("56_astro_idle");
    av.have = true;
    av.caption[0] = '\0';
    av.anim_ms = 300;
    av.faces[0] = 5; av.faces[1] = 2; av.faces[2] = 9;
    cs_ui_astro_update(&av); run(100); dump(out, "57_astro_rolling");
    av.anim_ms = 820;
    av.faces[0] = 3; av.faces[1] = 7; av.faces[2] = 1;
    cs_ui_astro_update(&av); run(100); dump(out, "61_astro_surfacing");
    av.anim_ms = CS_ASTRO_ANIM_MS;
    av.faces[2] = 6;
    cs_ui_astro_update(&av); run(300); dump(out, "58_astro_result"); pool("58_astro_result");
    av.focus = 1;
    av.sent = true;
    snprintf(av.caption, sizeof av.caption, "Sent to " CS_COMPANION_NAME " ✓");
    cs_ui_astro_update(&av); run(300); dump(out, "59_astro_sent");
    av.focus = 2;
    av.sent = false;
    av.faces[0] = 10; av.faces[1] = 8; av.faces[2] = 11;
    snprintf(av.caption, sizeof av.caption, "History");
    cs_ui_astro_update(&av); run(300); dump(out, "60_astro_long");
    static cs_astro_hist_view_t hv;
    static const struct { const char *w; uint8_t f[3]; } HROWS[10] = {
        {"Today 21:52", {3, 7, 6}}, {"Today 21:40", {1, 11, 3}}, {"Today 20:13", {6, 9, 11}},
        {"Yesterday 23:05", {2, 9, 7}}, {"Yesterday 19:20", {0, 4, 4}}, {"Oct 31 14:02", {4, 0, 0}},
        {"Oct 30 09:11", {5, 8, 8}}, {"Oct 29 22:40", {8, 11, 11}}, {"Oct 29 08:03", {9, 1, 1}},
        {"\u2014", {11, 5, 9}},
    };
    memset(&hv, 0, sizeof hv);
    hv.n = 10;
    for (int i = 0; i < 10; i++) {
        snprintf(hv.rows[i].when, sizeof hv.rows[i].when, "%s", HROWS[i].w);
        memcpy(hv.rows[i].f, HROWS[i].f, 3);
    }
    cs_ui_show(CS_SCR_ASTRO_HIST); cs_ui_astro_hist_update(&hv); run(300); dump(out, "62_astro_history");
    pool("62_astro_history");
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("lvgl pool: used %u%%, max_used %u\n", (unsigned)mon.used_pct, (unsigned)mon.max_used);
    return 0;
}
