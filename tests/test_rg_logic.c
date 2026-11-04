// Host tests for the Release Guidance pure logic: copy table, flow, key timing,
// theme choice and layout helpers. No ESP-IDF or LVGL needed.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rg_content.h"
#include "rg_flow.h"
#include "rg_input.h"
#include "rg_theme.h"

// Work-order section 4, copied independently of the generator.
static const char *const WO_FEEL[] = {
    "现在感受一下，此时此刻是哪种想要浮上心头？",
    "能允许它存在吗？",
    "能允许自己轻松放开这个紧抓不放的想要吗？",
    "愿意现在放开它吗？",
};
static const char *const WO_GOAL[] = {
    "心里想着你的目标，感受一下这个目标勾起了什么想要？",
    "能允许它存在吗？",
    "能允许自己轻松放开这个紧抓不放的想要吗？",
    "愿意现在放开它吗？",
    "如果目标实现不了，会勾起什么想要？",
    "能允许它存在吗？",
    "能允许自己轻松放开这个紧抓不放的想要吗？",
    "愿意现在放开它吗？",
};
static const char *const WO_A[] = {
    "现在还有什么想要？",
    "能允许它存在吗？",
    "能允许自己轻松放开这个紧抓不放的想要吗？",
    "愿意现在放开它吗？",
};
static const char *const WO_B[] = {
    "能允许感受浮现上来吗？",
    "没有感受说明感受被压抑了，只是允许感受浮现出来。",
    "现在是哪种想要？",
    "能允许它存在吗？",
    "能允许自己轻松放开这个紧抓不放的想要吗？",
    "愿意现在放开它吗？",
};

static void joined(rg_sentence_id_t id, char *buf, size_t cap) {
    buf[0] = '\0';
    const rg_sentence_t *s = &RG_SENTENCES[id];
    for (int l = 0; l < s->line_count; l++) {
        const rg_line_t *line = &RG_LINES[s->first_line + l];
        for (int k = 0; k < line->seg_count; k++) {
            assert(strlen(buf) + strlen(RG_SEGS[line->first_seg + k].text) < cap);
            strcat(buf, RG_SEGS[line->first_seg + k].text);
        }
    }
}

static void check_seq(rg_seq_id_t seq, const char *const *expected, size_t n) {
    assert(RG_SEQS[seq].count == n);
    char buf[256];
    for (size_t i = 0; i < n; i++) {
        rg_sentence_id_t id = (rg_sentence_id_t)RG_SEQS[seq].items[i];
        assert(strcmp(RG_SENTENCES[id].source, expected[i]) == 0);
        joined(id, buf, sizeof buf);
        assert(strcmp(buf, expected[i]) == 0);
    }
}

static void test_content(void) {
    check_seq(RG_SEQ_FEEL, WO_FEEL, 4);
    check_seq(RG_SEQ_GOAL, WO_GOAL, 8);
    check_seq(RG_SEQ_LOOP_A, WO_A, 4);
    check_seq(RG_SEQ_LOOP_B, WO_B, 6);
    char buf[256];
    joined(RG_S_ENTRY, buf, sizeof buf);
    assert(strcmp(buf, "仅仅通过释放获得一切。") == 0);
    assert(strcmp(RG_ENTRY_OPTIONS[0], "释放感受") == 0);
    assert(strcmp(RG_ENTRY_OPTIONS[1], "释放目标") == 0);
    assert(strcmp(RG_WILLING_OPTIONS[0], "愿意") == 0);
    assert(strcmp(RG_WILLING_OPTIONS[1], "还不愿意") == 0);

    for (int id = 0; id < RG_SENTENCE_COUNT; id++) {
        const rg_sentence_t *s = &RG_SENTENCES[id];
        int emph = 0;
        int prev_baseline = -1;
        for (int l = 0; l < s->line_count; l++) {
            const rg_line_t *line = &RG_LINES[s->first_line + l];
            char text[128] = "";
            for (int k = 0; k < line->seg_count; k++) {
                strcat(text, RG_SEGS[line->first_seg + k].text);
                emph += RG_SEGS[line->first_seg + k].emph;
            }
            assert(strcmp(text, "吗？") != 0);          // never an orphan 吗？
            assert(strcmp(text, "？") != 0);
            assert(line->baseline > prev_baseline);
            assert(line->baseline > 40 && line->baseline < 290);
            prev_baseline = line->baseline;
        }
        if (s->kind == RG_KIND_NOTE) assert(emph == 0);
        else assert(emph == 1);                        // exactly one keyword
    }
    // The explanation sentence is the only note and has no question mark.
    assert(RG_SENTENCES[RG_S_LOOP_B_2].kind == RG_KIND_NOTE);
    assert(strstr(RG_SENTENCES[RG_S_LOOP_B_2].source, "？") == NULL);
    // Longest sentence uses the 22 px slot.
    const rg_line_t *g = &RG_LINES[RG_SENTENCES[RG_S_GOAL_1].first_line];
    assert(RG_FONT_PX[RG_SEGS[g->first_seg].font] == 22);
}

static rg_sentence_id_t walk(rg_flow_t *f, rg_action_t a) {
    rg_flow_handle(f, a);
    return rg_flow_sentence(f);
}

static void test_flow(void) {
    rg_flow_t f;
    rg_flow_init(&f);
    assert(rg_flow_sentence(&f) == RG_S_ENTRY);
    assert(rg_flow_handle(&f, RG_ACT_PREV) == RG_FX_NONE);      // already top item
    assert(rg_flow_handle(&f, RG_ACT_NEXT) & RG_FX_REDRAW_FOCUS);
    assert(f.entry_sel == 1);
    assert(rg_flow_handle(&f, RG_ACT_HOME) == RG_FX_LEAVE);     // hold on step 1: leave the app
    assert(f.in_entry && f.entry_sel == 1);
    assert(rg_flow_handle(&f, RG_ACT_HOLD_UP) == RG_FX_TOGGLE_MUTE);
    assert(rg_flow_handle(&f, RG_ACT_HOLD_DOWN) == RG_FX_CYCLE_THEME);
    rg_flow_handle(&f, RG_ACT_PREV);

    // Feelings path, counted once on entry.
    uint32_t fx = rg_flow_handle(&f, RG_ACT_CONFIRM);
    assert(fx & RG_FX_PATH_ENTERED);
    assert(fx & RG_FX_SOUND_CONFIRM);
    assert(f.path == RG_PATH_FEEL);
    assert(rg_flow_sentence(&f) == RG_S_FEEL_1);
    assert(rg_flow_handle(&f, RG_ACT_PREV) == RG_FX_NONE);      // first sentence stays
    assert(walk(&f, RG_ACT_NEXT) == RG_S_ALLOW);
    assert(walk(&f, RG_ACT_CONFIRM) == RG_S_LETGO);
    assert(walk(&f, RG_ACT_PREV) == RG_S_ALLOW);
    assert(walk(&f, RG_ACT_NEXT) == RG_S_LETGO);
    assert(walk(&f, RG_ACT_NEXT) == RG_S_WILLING);
    // Willing page: UP/DOWN move focus only, both answers continue the same way.
    assert(rg_flow_handle(&f, RG_ACT_NEXT) == (RG_FX_REDRAW_FOCUS | RG_FX_SOUND_TAP));
    assert(f.focus == 1);
    assert(rg_flow_handle(&f, RG_ACT_NEXT) == RG_FX_NONE);
    assert(rg_flow_sentence(&f) == RG_S_WILLING);
    fx = rg_flow_handle(&f, RG_ACT_CONFIRM);
    assert(!(fx & RG_FX_PATH_ENTERED));
    assert(rg_flow_sentence(&f) == RG_S_LOOP_A_1);
    assert(f.focus == 0);

    // A -> B -> A ... forever.
    const rg_sentence_id_t a_then_b[] = {
        RG_S_LOOP_A_1, RG_S_ALLOW, RG_S_LETGO, RG_S_WILLING,
        RG_S_LOOP_B_1, RG_S_LOOP_B_2, RG_S_LOOP_B_3, RG_S_ALLOW, RG_S_LETGO, RG_S_WILLING,
    };
    for (int round = 0; round < 25; round++) {
        for (int i = 0; i < 10; i++) {
            assert(rg_flow_sentence(&f) == a_then_b[i]);
            rg_flow_handle(&f, RG_ACT_CONFIRM);
        }
    }
    assert(rg_flow_sentence(&f) == RG_S_LOOP_A_1);
    assert(f.pos < 4 + 2 * 10);                                  // stays bounded
    // UP from loop A's first sentence returns to loop B's last.
    assert(walk(&f, RG_ACT_PREV) == RG_S_WILLING);

    // Long-press return keeps the last entry choice and is not counted.
    fx = rg_flow_handle(&f, RG_ACT_HOME);
    assert(fx & RG_FX_REDRAW_PAGE);
    assert(!(fx & RG_FX_PATH_ENTERED));
    assert(f.in_entry);

    // Goal path: 8 sentences then loop A; UP from loop A goes back into the path.
    rg_flow_handle(&f, RG_ACT_NEXT);
    fx = rg_flow_handle(&f, RG_ACT_CONFIRM);
    assert(fx & RG_FX_PATH_ENTERED);
    assert(f.path == RG_PATH_GOAL);
    const rg_sentence_id_t goal[] = {
        RG_S_GOAL_1, RG_S_ALLOW, RG_S_LETGO, RG_S_WILLING,
        RG_S_GOAL_5, RG_S_ALLOW, RG_S_LETGO, RG_S_WILLING, RG_S_LOOP_A_1,
    };
    for (int i = 0; i < 9; i++) {
        assert(rg_flow_sentence(&f) == goal[i]);
        if (i < 8) rg_flow_handle(&f, RG_ACT_CONFIRM);
    }
    assert(walk(&f, RG_ACT_PREV) == RG_S_WILLING);
    assert(walk(&f, RG_ACT_CONFIRM) == RG_S_LOOP_A_1);
    assert(rg_flow_kind(&f) == RG_KIND_QUESTION);
}

static void test_input(void) {
    rg_input_t in;
    rg_action_t a[2];
    uint32_t deadline;
    rg_input_init(&in);

    // UP/DOWN act on press.
    assert(rg_input_event(&in, RG_KEY_UP, RG_KEV_PRESS, 10, a) == 1 && a[0] == RG_ACT_PREV);
    assert(rg_input_event(&in, RG_KEY_UP, RG_KEV_CLICK, 300, a) == 0);
    assert(rg_input_event(&in, RG_KEY_DOWN, RG_KEV_PRESS, 400, a) == 1 && a[0] == RG_ACT_NEXT);
    assert(rg_input_event(&in, RG_KEY_DOWN, RG_KEV_DOUBLE, 700, a) == 0);

    // OK click confirms; double click confirms twice.
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 1000, a) == 0);
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_CLICK, 1250, a) == 1 && a[0] == RG_ACT_CONFIRM);
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_CLICK, 1260, a) == 0);   // needs a new press
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 2000, a);
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 2120, a);
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_DOUBLE, 2400, a) == 2);
    assert(a[0] == RG_ACT_CONFIRM && a[1] == RG_ACT_CONFIRM);

    // OK held 1 s returns home; the release afterwards never confirms.
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 3000, a);
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_LONG, 3500, a) == 0);
    assert(rg_input_pending(&in, &deadline) && deadline == 4000);
    assert(rg_input_poll(&in, 3999, RG_KEY_OK) == RG_ACT_NONE);           // too early
    assert(rg_input_pending(&in, &deadline));
    assert(rg_input_poll(&in, 4000, RG_KEY_OK) == RG_ACT_HOME);
    assert(!rg_input_pending(&in, &deadline));
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_CLICK, 4300, a) == 0);

    // Released between 500 and 999 ms: nothing at all.
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 5000, a);
    rg_input_event(&in, RG_KEY_OK, RG_KEV_LONG, 5500, a);
    assert(rg_input_poll(&in, 6000, RG_KEY_NONE) == RG_ACT_NONE);
    assert(rg_input_event(&in, RG_KEY_OK, RG_KEV_CLICK, 6100, a) == 0);
    assert(!rg_input_pending(&in, &deadline));

    // Re-press during a pending check restarts timing instead of firing early.
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 7000, a);
    rg_input_event(&in, RG_KEY_OK, RG_KEV_LONG, 7500, a);
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 7900, a);
    assert(!rg_input_pending(&in, &deadline));
    assert(rg_input_poll(&in, 8000, RG_KEY_OK) == RG_ACT_NONE);

    // UP / DOWN holds for the entry-page settings; wrong key held -> nothing.
    rg_input_event(&in, RG_KEY_UP, RG_KEV_PRESS, 9000, a);
    rg_input_event(&in, RG_KEY_UP, RG_KEV_LONG, 9500, a);
    assert(rg_input_poll(&in, 10000, RG_KEY_UP) == RG_ACT_HOLD_UP);
    rg_input_event(&in, RG_KEY_DOWN, RG_KEV_PRESS, 11000, a);
    rg_input_event(&in, RG_KEY_DOWN, RG_KEV_LONG, 11500, a);
    assert(rg_input_poll(&in, 12000, RG_KEY_UP) == RG_ACT_NONE);
    rg_input_event(&in, RG_KEY_DOWN, RG_KEV_PRESS, 13000, a);
    rg_input_event(&in, RG_KEY_DOWN, RG_KEV_LONG, 13500, a);
    assert(rg_input_poll(&in, 14000, RG_KEY_DOWN) == RG_ACT_HOLD_DOWN);

    // Millisecond counter wrap-around.
    rg_input_event(&in, RG_KEY_OK, RG_KEV_PRESS, 0xFFFFFF00u, a);
    rg_input_event(&in, RG_KEY_OK, RG_KEV_LONG, 0xFFFFFF00u + 500, a);
    assert(rg_input_poll(&in, 0xFFFFFF00u + 900, RG_KEY_OK) == RG_ACT_NONE);
    assert(rg_input_poll(&in, 0xFFFFFF00u + 1000, RG_KEY_OK) == RG_ACT_HOME);

    // ADC windows match the BSP ladder table order.
    const uint16_t windows[RG_KEY_COUNT][2] = {{0, 150}, {150, 447}, {447, 1900}};
    assert(rg_input_key_from_mv(0, windows) == RG_KEY_UP);
    assert(rg_input_key_from_mv(150, windows) == RG_KEY_DOWN);
    assert(rg_input_key_from_mv(1200, windows) == RG_KEY_OK);
    assert(rg_input_key_from_mv(3300, windows) == RG_KEY_NONE);
    assert(rg_input_key_from_mv(-1, windows) == RG_KEY_NONE);
}

static void test_theme_and_layout(void) {
    assert(!rg_theme_is_day(RG_THEME_MODE_AUTO, false, 12));       // no clock: dusk
    assert(rg_theme_is_day(RG_THEME_MODE_AUTO, true, 6));
    assert(rg_theme_is_day(RG_THEME_MODE_AUTO, true, 17));
    assert(!rg_theme_is_day(RG_THEME_MODE_AUTO, true, 18));
    assert(!rg_theme_is_day(RG_THEME_MODE_AUTO, true, 2));
    assert(rg_theme_is_day(RG_THEME_MODE_MORNING, false, 0));
    assert(!rg_theme_is_day(RG_THEME_MODE_DUSK, true, 12));
    assert(rg_theme_palette(false, RG_WEATHER_RAIN) == RG_PALETTE_DUSK);
    assert(rg_theme_palette(true, RG_WEATHER_RAIN) == RG_PALETTE_RAIN);
    assert(rg_theme_palette(true, RG_WEATHER_SUN) == RG_PALETTE_MORNING);
    assert(rg_theme_next_mode(RG_THEME_MODE_MORNING) == RG_THEME_MODE_AUTO);

    for (uint32_t r = 0; r < 200; r++) {
        for (uint8_t last = 0; last < 4; last++) {
            uint8_t p = rg_pick_other(r * 2654435761u, 4, last);
            assert(p < 4 && p != last);
        }
        assert(rg_pick_other(r, 3, 0xFF) < 3);
    }
    int seen[4] = {0};
    for (uint32_t r = 0; r < 300; r++) seen[rg_pick_other(r, 4, 1)]++;
    assert(seen[1] == 0 && seen[0] > 0 && seen[2] > 0 && seen[3] > 0);
    uint32_t st = 0;
    assert(rg_rand_next(&st) != 0 && st != 0);

    const int16_t w[3] = {75, 56, 25};
    int16_t xs[3];
    assert(rg_layout_center(w, 3, 240, xs) == 156);
    assert(xs[0] == 42 && xs[1] == 117 && xs[2] == 173);
    assert(rg_layout_label_top(160, 25, 3) == 138);

    assert(rg_star_opa(-32767, 10) == 89);
    assert(rg_star_opa(32767, 10) == 255);
    assert(rg_star_opa(32767, 7) == 178);
    assert(rg_star_opa(32767, 14) == 255);
    assert(rg_star_px(9) == 1 && rg_star_px(12) == 2);
}

int main(void) {
    test_content();
    test_flow();
    test_input();
    test_theme_and_layout();
    puts("test_rg_logic: PASS");
    return 0;
}
