// Host-side renderer: runs the real main/rg/rg_ui.c on desktop LVGL 9.5.0 and
// dumps 240x320 RGB565 frames as PPM files. Developer tool, not part of the
// firmware or the validation gate. See tools/release_guidance/host_render.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "rg_content.h"
#include "rg_flow.h"
#include "rg_ui.h"

static uint16_t s_fb[RG_SCREEN_W * RG_SCREEN_H];
static uint16_t s_buf[RG_SCREEN_W * RG_SCREEN_H];
static uint32_t s_ms;

static uint32_t tick_cb(void) { return s_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    const uint16_t *src = (const uint16_t *)px;
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(&s_fb[y * RG_SCREEN_W + a->x1], src + (y - a->y1) * w, (size_t)w * 2);
    }
    lv_display_flush_ready(d);
}

static void dump(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", RG_SCREEN_W, RG_SCREEN_H);
    for (int i = 0; i < RG_SCREEN_W * RG_SCREEN_H; i++) {
        const uint16_t c = s_fb[i];
        const unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 31) * 255 / 31),
            (unsigned char)(((c >> 5) & 63) * 255 / 63),
            (unsigned char)((c & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void run_ms(uint32_t ms) {
    const uint32_t end = s_ms + ms;
    while (s_ms < end) {
        s_ms += 5;
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}

static void goto_sentence(rg_flow_t *f, int path, rg_sentence_id_t target, int focus) {
    rg_flow_init(f);
    if (target == RG_S_ENTRY) { f->entry_sel = (uint8_t)focus; return; }
    f->entry_sel = (uint8_t)path;
    rg_flow_handle(f, RG_ACT_CONFIRM);
    for (int guard = 0; guard < 40 && rg_flow_sentence(f) != target; guard++) {
        rg_flow_handle(f, RG_ACT_CONFIRM);
    }
    f->focus = (uint8_t)focus;
}

typedef struct {
    const char *name;
    bool day;
    rg_weather_t weather;
    bool rainbow_left;
    int path;
    rg_sentence_id_t sentence;
    int focus;
} shot_t;

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(RG_SCREEN_W, RG_SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_buf, NULL, sizeof s_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    printf("glyph audit missing=%d\n", rg_ui_check_glyphs());

    static const shot_t shots[] = {
        {"dusk_entry", false, RG_WEATHER_SUN, false, 0, RG_S_ENTRY, 0},
        {"dusk_entry_goal", false, RG_WEATHER_SUN, false, 0, RG_S_ENTRY, 1},
        {"dusk_feel1", false, RG_WEATHER_SUN, false, 0, RG_S_FEEL_1, 0},
        {"dusk_allow", false, RG_WEATHER_SUN, false, 0, RG_S_ALLOW, 0},
        {"dusk_letgo", false, RG_WEATHER_SUN, false, 0, RG_S_LETGO, 0},
        {"dusk_willing", false, RG_WEATHER_SUN, false, 0, RG_S_WILLING, 0},
        {"dusk_willing_not", false, RG_WEATHER_SUN, false, 0, RG_S_WILLING, 1},
        {"dusk_goal1_longest", false, RG_WEATHER_SUN, false, 1, RG_S_GOAL_1, 0},
        {"dusk_goal5", false, RG_WEATHER_SUN, false, 1, RG_S_GOAL_5, 0},
        {"dusk_loopa1", false, RG_WEATHER_SUN, false, 0, RG_S_LOOP_A_1, 0},
        {"dusk_loopb1", false, RG_WEATHER_SUN, false, 0, RG_S_LOOP_B_1, 0},
        {"dusk_loopb2_note", false, RG_WEATHER_SUN, false, 0, RG_S_LOOP_B_2, 0},
        {"dusk_loopb3", false, RG_WEATHER_SUN, false, 0, RG_S_LOOP_B_3, 0},
        {"morning_sun_allow", true, RG_WEATHER_SUN, false, 0, RG_S_ALLOW, 0},
        {"morning_rain_allow", true, RG_WEATHER_RAIN, false, 0, RG_S_ALLOW, 0},
        {"morning_rainbow_allow", true, RG_WEATHER_RAINBOW, false, 0, RG_S_ALLOW, 0},
        {"morning_rainbow_left_entry", true, RG_WEATHER_RAINBOW, true, 0, RG_S_ENTRY, 0},
        {"morning_leaves_willing", true, RG_WEATHER_LEAVES, false, 0, RG_S_WILLING, 0},
    };
    char path[512];
    for (size_t i = 0; i < sizeof shots / sizeof shots[0]; i++) {
        const shot_t *s = &shots[i];
        rg_flow_t flow;
        goto_sentence(&flow, s->path, s->sentence, s->focus);
        const rg_sky_t sky = {.day = s->day, .weather = s->weather,
                              .rainbow_left = s->rainbow_left, .seed = 12345};
        rg_ui_build(&sky, &flow);
        run_ms(2000);                       // let the sky animate a little
        snprintf(path, sizeof path, "%s/%s.ppm", out, s->name);
        dump(path);
    }
    // Motion strips: 24 frames, 4 animation ticks apart.
    static const struct { const char *name; bool day; rg_weather_t w; } anim[] = {
        {"anim_dusk", false, RG_WEATHER_SUN}, {"anim_rain", true, RG_WEATHER_RAIN},
        {"anim_leaves", true, RG_WEATHER_LEAVES}, {"anim_sun", true, RG_WEATHER_SUN}};
    for (size_t a = 0; a < sizeof anim / sizeof anim[0]; a++) {
        rg_flow_t flow;
        goto_sentence(&flow, 0, RG_S_ALLOW, 0);
        const rg_sky_t sky = {.day = anim[a].day, .weather = anim[a].w, .seed = 777};
        rg_ui_build(&sky, &flow);
        for (int f = 0; f < 24; f++) {
            run_ms(RG_FX_PERIOD_MS * 2);
            snprintf(path, sizeof path, "%s/%s_%02d.ppm", out, anim[a].name, f);
            dump(path);
        }
    }
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("lv_mem: total=%u used_pct=%u max_used=%u frag_pct=%u\n", (unsigned)mon.total_size,
           (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.frag_pct);
    return 0;
}
