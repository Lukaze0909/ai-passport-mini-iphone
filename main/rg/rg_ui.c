// main/rg/rg_ui.c -- Release Guidance LVGL screens and sky animation.
//
// Layering (back to front): gradient screen background, rainbow / sun / moon,
// stars, three drifting clouds, rain or leaves, page text, right-edge hints,
// status word. No full-screen ARGB layers: sprites are small A8 / RGB565A8
// images in Flash; stars, rain and leaves are drawn by two custom-draw objects
// that only invalidate the few pixels that change. One lv_timer drives all
// motion at RG_FX_PERIOD_MS (80 ms, 12.5 fps).
#include "rg_ui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "rg_content.h"
#include "rg_images.h"
#include "sdkconfig.h"

#ifndef CONFIG_RG_RAIN_DROPS
#define CONFIG_RG_RAIN_DROPS RG_RAIN_DROPS_DEFAULT
#endif
#ifndef CONFIG_RG_LEAVES
#define CONFIG_RG_LEAVES RG_LEAVES_DEFAULT
#endif
#ifndef CONFIG_RG_PAGE_FADE_MS
#define CONFIG_RG_PAGE_FADE_MS 0
#endif

static const char *TAG = "rg_ui";

LV_FONT_DECLARE(rg_wenkai_note19);
LV_FONT_DECLARE(rg_wenkai_opt21);
LV_FONT_DECLARE(rg_wenkai_long22);
LV_FONT_DECLARE(rg_wenkai_q25);
LV_FONT_DECLARE(rg_wenkai_entry27);
LV_FONT_DECLARE(rg_wenkai_key28);
LV_FONT_DECLARE(rg_wenkai_key30);
LV_FONT_DECLARE(rg_wenkai_small13);

static const lv_font_t *const FONTS[RG_FONT_COUNT] = {
    [RG_FONT_NOTE19] = &rg_wenkai_note19,
    [RG_FONT_OPT21] = &rg_wenkai_opt21,
    [RG_FONT_LONG22] = &rg_wenkai_long22,
    [RG_FONT_Q25] = &rg_wenkai_q25,
    [RG_FONT_ENTRY27] = &rg_wenkai_entry27,
    [RG_FONT_KEY28] = &rg_wenkai_key28,
    [RG_FONT_KEY30] = &rg_wenkai_key30,
    [RG_FONT_SMALL13] = &rg_wenkai_small13,
};

#define RAIN_MAX 32
#define LEAF_MAX 6
#define RAIN_LEN_Y 7
#define RAIN_LEN_X 2
#define RAIN_COLOR 0x8296AA
#define RAIN_OPA 190
#define LEAF_COLOR_A 0xE8A040
#define LEAF_COLOR_B 0xD8822E
#define STAR_OPA_STEP 6
#define STATUS_SHOW_MS 1200

typedef struct { int16_t x_q4, y_q4; } rg_drop_t;
typedef struct { int16_t x_q4, y_q4; uint16_t angle, sway; } rg_leaf_t;

static rg_sky_t s_sky;
static const rg_palette_t *s_pal;
static lv_obj_t *s_scr, *s_page, *s_status, *s_stars, *s_front, *s_sun_rays;
static lv_obj_t *s_clouds[RG_CLOUD_COUNT];
static lv_obj_t *s_hints[3];
static lv_obj_t *s_opt[2], *s_dot;
static lv_point_precise_t s_underline[2];
static int16_t s_opt_baseline[2], s_opt_x[2];
static lv_timer_t *s_fx_timer, *s_status_timer;
static uint32_t s_frame;
static uint32_t s_cloud_q8[RG_CLOUD_COUNT];
static int16_t s_cloud_x[RG_CLOUD_COUNT];
static uint8_t s_star_opa[RG_STAR_COUNT];
static rg_drop_t s_drops[RAIN_MAX];
static rg_leaf_t s_leaves[LEAF_MAX];
static int s_drop_count, s_leaf_count;
static uint32_t s_rng;

// Up chevron, down chevron (absolute coordinates; the line objects sit at 0,0).
static const lv_point_precise_t HINT_UP[] = {
    {RG_HINT_X - 3, RG_HINT_Y_UP + 2}, {RG_HINT_X, RG_HINT_Y_UP - 1}, {RG_HINT_X + 3, RG_HINT_Y_UP + 2}};
static const lv_point_precise_t HINT_DOWN[] = {
    {RG_HINT_X - 3, RG_HINT_Y_DOWN - 1}, {RG_HINT_X, RG_HINT_Y_DOWN + 2}, {RG_HINT_X + 3, RG_HINT_Y_DOWN - 1}};

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static lv_obj_t *bare_obj(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *sprite(lv_obj_t *parent, const lv_image_dsc_t *src, int x, int y) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(img, x, y);
    return img;
}

static void tint(lv_obj_t *img, uint32_t color) {
    lv_obj_set_style_image_recolor(img, hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------- sky
static const lv_image_dsc_t *cloud_src(int i) {
    static const lv_image_dsc_t *const DUSK[RG_CLOUD_COUNT] = {
        &rg_img_cloud0_dusk, &rg_img_cloud1_dusk, &rg_img_cloud2_dusk};
    static const lv_image_dsc_t *const DAY[RG_CLOUD_COUNT] = {
        &rg_img_cloud0_morning, &rg_img_cloud1_morning, &rg_img_cloud2_morning};
    return s_sky.day ? DAY[i] : DUSK[i];
}

static int cloud_left(int i) {
    const int pivot = (int)cloud_src(i)->header.w / 2;
    const int drift = (int)(s_cloud_q8[i] >> 8);
    return ((RG_CLOUDS[i].x + drift) % RG_CLOUD_WRAP) - 40 - pivot;
}

static void star_area(int i, lv_area_t *a) {
    const int px = rg_star_px(RG_STARS[i].size10);
    a->x1 = RG_STARS[i].x;
    a->y1 = RG_STARS[i].y;
    a->x2 = RG_STARS[i].x + px - 1;
    a->y2 = RG_STARS[i].y + px - 1;
}

static uint8_t star_opa_now(int i) {
    const int speed = 5 + (i % 5);                       // degrees per frame
    const int angle = (int)((RG_STARS[i].phase + s_frame * (uint32_t)speed) % 360);
    return rg_star_opa(lv_trigo_sin((int16_t)angle), RG_STARS[i].size10);
}

static void stars_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = hex(s_pal->text);
    for (int i = 0; i < RG_STAR_COUNT; i++) {
        lv_area_t a;
        star_area(i, &a);
        d.bg_opa = s_star_opa[i];
        d.radius = rg_star_px(RG_STARS[i].size10) > 1 ? 1 : 0;
        lv_draw_rect(layer, &d, &a);
    }
}

static void drop_area(const rg_drop_t *dr, lv_area_t *a) {
    const int x = dr->x_q4 >> 4, y = dr->y_q4 >> 4;
    a->x1 = x - RAIN_LEN_X - 1;
    a->y1 = y - 1;
    a->x2 = x + 1;
    a->y2 = y + RAIN_LEN_Y + 1;
}

static void leaf_area(const rg_leaf_t *lf, lv_area_t *a) {
    const int x = lf->x_q4 >> 4, y = lf->y_q4 >> 4;
    a->x1 = x - 7;
    a->y1 = y - 7;
    a->x2 = x + 7;
    a->y2 = y + 7;
}

static void front_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_drop_count) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = hex(RAIN_COLOR);
        d.opa = RAIN_OPA;
        d.width = 1;
        for (int i = 0; i < s_drop_count; i++) {
            const int x = s_drops[i].x_q4 >> 4, y = s_drops[i].y_q4 >> 4;
            d.p1.x = x;
            d.p1.y = y;
            d.p2.x = x - RAIN_LEN_X;
            d.p2.y = y + RAIN_LEN_Y;
            lv_draw_line(layer, &d);
        }
    }
    for (int i = 0; i < s_leaf_count; i++) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &rg_img_leaf;
        d.rotation = (int32_t)s_leaves[i].angle * 10;
        d.pivot.x = RG_IMG_LEAF_PIVOT_X;
        d.pivot.y = RG_IMG_LEAF_PIVOT_Y;
        d.recolor = hex(i & 1 ? LEAF_COLOR_B : LEAF_COLOR_A);
        d.recolor_opa = LV_OPA_COVER;
        const int x = s_leaves[i].x_q4 >> 4, y = s_leaves[i].y_q4 >> 4;
        lv_area_t a = {x - RG_IMG_LEAF_PIVOT_X, y - RG_IMG_LEAF_PIVOT_Y,
                       x - RG_IMG_LEAF_PIVOT_X + rg_img_leaf.header.w - 1,
                       y - RG_IMG_LEAF_PIVOT_Y + rg_img_leaf.header.h - 1};
        lv_draw_image(layer, &d, &a);
    }
}

static void drop_reset(rg_drop_t *dr, bool anywhere) {
    dr->x_q4 = (int16_t)((10 + rg_rand_next(&s_rng) % (RG_SCREEN_W)) << 4);
    dr->y_q4 = (int16_t)((anywhere ? rg_rand_next(&s_rng) % RG_SCREEN_H : 0) << 4) - (12 << 4);
}

static void leaf_reset(rg_leaf_t *lf, bool anywhere) {
    lf->x_q4 = (int16_t)(((int)(rg_rand_next(&s_rng) % 200) - 40) << 4);
    lf->y_q4 = (int16_t)(((anywhere ? (int)(rg_rand_next(&s_rng) % 240) : 0) - 10) << 4);
    lf->angle = (uint16_t)(rg_rand_next(&s_rng) % 360);
    lf->sway = (uint16_t)(rg_rand_next(&s_rng) % 360);
}

static void fx_tick(lv_timer_t *t) {
    (void)t;
    s_frame++;
    // Clouds: ~3 px/s for the nearest, slower behind; wrap off one side, back on the other.
    static const uint16_t SPEED_Q8[RG_CLOUD_COUNT] = {64, 51, 38};
    for (int i = 0; i < RG_CLOUD_COUNT; i++) {
        if (!s_clouds[i]) continue;
        s_cloud_q8[i] += SPEED_Q8[i];
        const int x = cloud_left(i);
        if (x != s_cloud_x[i]) {
            s_cloud_x[i] = (int16_t)x;
            lv_obj_set_x(s_clouds[i], x);
        }
    }
    if (s_stars) {
        for (int i = 0; i < RG_STAR_COUNT; i++) {
            const uint8_t opa = star_opa_now(i);
            if (abs((int)opa - (int)s_star_opa[i]) < STAR_OPA_STEP) continue;
            s_star_opa[i] = opa;
            lv_area_t a;
            star_area(i, &a);
            lv_obj_invalidate_area(s_stars, &a);
        }
    }
    if (s_sun_rays && (s_frame & 1) == 0) {
        lv_image_set_rotation(s_sun_rays, (int32_t)((s_frame / 2) % 360) * 10);
    }
    if (s_front) {
        lv_area_t a;
        // One invalidated rectangle per drop (old and new position joined) keeps
        // the frame well under LVGL's 32-area limit, beyond which it redraws all.
        for (int i = 0; i < s_drop_count; i++) {
            lv_area_t b;
            drop_area(&s_drops[i], &a);
            s_drops[i].x_q4 -= 24;               // 1.5 px left per frame
            s_drops[i].y_q4 += 6 << 4;           // 6 px down per frame
            if ((s_drops[i].y_q4 >> 4) > RG_SCREEN_H) {
                lv_obj_invalidate_area(s_front, &a);
                drop_reset(&s_drops[i], false);
                drop_area(&s_drops[i], &a);
                lv_obj_invalidate_area(s_front, &a);
                continue;
            }
            drop_area(&s_drops[i], &b);
            a.x1 = LV_MIN(a.x1, b.x1);
            a.y1 = LV_MIN(a.y1, b.y1);
            a.x2 = LV_MAX(a.x2, b.x2);
            a.y2 = LV_MAX(a.y2, b.y2);
            lv_obj_invalidate_area(s_front, &a);
        }
        for (int i = 0; i < s_leaf_count; i++) {
            rg_leaf_t *lf = &s_leaves[i];
            leaf_area(lf, &a);
            lv_obj_invalidate_area(s_front, &a);   // leaves move < 2 px: areas overlap and merge
            lf->sway = (uint16_t)((lf->sway + 4) % 360);
            lf->x_q4 += (int16_t)(10 + lv_trigo_sin((int16_t)lf->sway) / 2048);
            lf->y_q4 += 13;                      // ~0.8 px down per frame
            lf->angle = (uint16_t)((lf->angle + 7) % 360);
            if ((lf->y_q4 >> 4) > RG_SCREEN_H + 8 || (lf->x_q4 >> 4) > RG_SCREEN_W + 8) {
                leaf_reset(lf, false);
            }
            leaf_area(lf, &a);
            lv_obj_invalidate_area(s_front, &a);
        }
    }
}

static void build_sky(void) {
    lv_obj_set_style_bg_color(s_scr, hex(s_pal->top), 0);
    lv_obj_set_style_bg_grad_color(s_scr, hex(s_pal->bottom), 0);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    // Dithered, gamma-shaped gradient: an 8 x 320 RGB565 tile repeated across the
    // screen (RGB565 banding is very visible on the pale morning sky otherwise).
    static const lv_image_dsc_t *const SKY[RG_PALETTE_COUNT] = {
        [RG_PALETTE_DUSK] = &rg_img_sky_dusk,
        [RG_PALETTE_MORNING] = &rg_img_sky_morning,
        [RG_PALETTE_RAIN] = &rg_img_sky_rain,
    };
    lv_obj_t *sky = sprite(s_scr, SKY[rg_theme_palette(s_sky.day, s_sky.weather)], 0, 0);
    lv_image_set_inner_align(sky, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_size(sky, RG_SCREEN_W, RG_SCREEN_H);

    s_stars = s_front = s_sun_rays = NULL;
    s_drop_count = s_leaf_count = 0;
    if (s_sky.day) {
        if (s_sky.weather == RG_WEATHER_RAINBOW) {
            const int cx = s_sky.rainbow_left ? RG_RAINBOW_X_LEFT : RG_RAINBOW_X_RIGHT;
            sprite(s_scr, &rg_img_rainbow, cx - RG_IMG_RAINBOW_PIVOT_X,
                   RG_RAINBOW_Y - RG_IMG_RAINBOW_PIVOT_Y);
        } else if (s_sky.weather == RG_WEATHER_SUN) {
            tint(sprite(s_scr, &rg_img_sun_body, RG_SUN_X - RG_IMG_SUN_BODY_PIVOT_X,
                        RG_SUN_Y - RG_IMG_SUN_BODY_PIVOT_Y), RG_SUN_COLOR);
            s_sun_rays = sprite(s_scr, &rg_img_sun_rays, RG_SUN_X - RG_IMG_SUN_RAYS_PIVOT_X,
                                RG_SUN_Y - RG_IMG_SUN_RAYS_PIVOT_Y);
            tint(s_sun_rays, RG_SUN_RAY_COLOR);
            lv_image_set_pivot(s_sun_rays, RG_IMG_SUN_RAYS_PIVOT_X, RG_IMG_SUN_RAYS_PIVOT_Y);
        }
    } else {
        tint(sprite(s_scr, &rg_img_moon, RG_MOON_X - RG_IMG_MOON_PIVOT_X,
                    RG_MOON_Y - RG_IMG_MOON_PIVOT_Y), RG_MOON_COLOR);
        s_stars = bare_obj(s_scr);
        lv_obj_set_size(s_stars, RG_SCREEN_W, 150);
        lv_obj_add_event_cb(s_stars, stars_draw, LV_EVENT_DRAW_MAIN, NULL);
        for (int i = 0; i < RG_STAR_COUNT; i++) s_star_opa[i] = star_opa_now(i);
    }
    for (int i = 0; i < RG_CLOUD_COUNT; i++) {
        s_cloud_x[i] = (int16_t)cloud_left(i);
        s_clouds[i] = sprite(s_scr, cloud_src(i), s_cloud_x[i],
                             RG_CLOUDS[i].y - (int)cloud_src(i)->header.h / 2);
    }
    if (s_sky.day && (s_sky.weather == RG_WEATHER_RAIN || s_sky.weather == RG_WEATHER_LEAVES)) {
        if (s_sky.weather == RG_WEATHER_RAIN) {
            s_drop_count = CONFIG_RG_RAIN_DROPS < RAIN_MAX ? CONFIG_RG_RAIN_DROPS : RAIN_MAX;
            for (int i = 0; i < s_drop_count; i++) drop_reset(&s_drops[i], true);
        } else {
            s_leaf_count = CONFIG_RG_LEAVES < LEAF_MAX ? CONFIG_RG_LEAVES : LEAF_MAX;
            for (int i = 0; i < s_leaf_count; i++) leaf_reset(&s_leaves[i], true);
        }
        if (s_drop_count || s_leaf_count) {
            s_front = bare_obj(s_scr);
            lv_obj_set_size(s_front, RG_SCREEN_W, RG_SCREEN_H);
            lv_obj_add_event_cb(s_front, front_draw, LV_EVENT_DRAW_MAIN, NULL);
        }
    }
}

// ---------------------------------------------------------------- text
static int text_width(const char *text, const lv_font_t *font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static lv_obj_t *label_at(const char *text, const lv_font_t *font, uint32_t color,
                          int x, int baseline) {
    lv_obj_t *l = lv_label_create(s_page);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_label_set_text_static(l, text);
    lv_obj_set_pos(l, x, rg_layout_label_top(baseline, font->line_height, font->base_line));
    return l;
}

static void draw_sentence(rg_sentence_id_t id) {
    const rg_sentence_t *s = &RG_SENTENCES[id];
    for (int l = 0; l < s->line_count; l++) {
        const rg_line_t *line = &RG_LINES[s->first_line + l];
        int16_t widths[8], xs[8];
        const int n = line->seg_count < 8 ? line->seg_count : 8;
        for (int k = 0; k < n; k++) {
            const rg_seg_t *seg = &RG_SEGS[line->first_seg + k];
            widths[k] = (int16_t)text_width(seg->text, FONTS[seg->font]);
        }
        rg_layout_center(widths, n, RG_SCREEN_W, xs);
        for (int k = 0; k < n; k++) {
            const rg_seg_t *seg = &RG_SEGS[line->first_seg + k];
            lv_obj_t *lbl = label_at(seg->text, FONTS[seg->font],
                                     seg->emph ? s_pal->accent : s_pal->text, xs[k], line->baseline);
            if (s->kind == RG_KIND_NOTE) lv_obj_set_style_text_opa(lbl, RG_NOTE_OPA, 0);
            if (!seg->emph) continue;
            s_underline[0].x = xs[k] + RG_UNDERLINE_INSET;
            s_underline[0].y = line->baseline + RG_UNDERLINE_DROP;
            s_underline[1].x = xs[k] + widths[k] - RG_UNDERLINE_INSET;
            s_underline[1].y = line->baseline + RG_UNDERLINE_DROP + RG_UNDERLINE_SLANT;
            lv_obj_t *u = lv_line_create(s_page);
            lv_line_set_points(u, s_underline, 2);
            lv_obj_set_style_line_width(u, RG_UNDERLINE_WIDTH, 0);
            lv_obj_set_style_line_color(u, hex(s_pal->accent), 0);
            lv_obj_set_style_line_opa(u, RG_UNDERLINE_OPA, 0);
            lv_obj_set_style_line_rounded(u, true, 0);
        }
    }
}

static void draw_options(const char *const opts[2], int b0, int b1) {
    const lv_font_t *font = FONTS[RG_OPTION_FONT];
    s_opt_baseline[0] = (int16_t)b0;
    s_opt_baseline[1] = (int16_t)b1;
    for (int i = 0; i < 2; i++) {
        const int w = text_width(opts[i], font);
        s_opt_x[i] = (int16_t)((RG_SCREEN_W - w) / 2);
        s_opt[i] = label_at(opts[i], font, s_pal->dim, s_opt_x[i], s_opt_baseline[i]);
    }
    s_dot = bare_obj(s_page);
    lv_obj_set_size(s_dot, 2 * RG_OPTION_DOT_RADIUS, 2 * RG_OPTION_DOT_RADIUS);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, hex(s_pal->accent), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
}

void rg_ui_show_focus(const rg_flow_t *flow) {
    if (!s_opt[0] || !s_dot) return;
    const int sel = flow->in_entry ? flow->entry_sel : flow->focus;
    const lv_font_t *font = FONTS[RG_OPTION_FONT];
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(s_opt[i], hex(i == sel ? s_pal->text : s_pal->dim), 0);
    }
    const int ascent = font->line_height - font->base_line;
    const int cy = s_opt_baseline[sel] - ascent * RG_OPTION_DOT_RISE_PCT / 100;
    // Use the stored x: lv_obj_get_x() is stale until LVGL's next layout pass.
    lv_obj_set_pos(s_dot, s_opt_x[sel] - RG_OPTION_DOT_GAP - RG_OPTION_DOT_RADIUS,
                   cy - RG_OPTION_DOT_RADIUS);
}

void rg_ui_show_page(const rg_flow_t *flow) {
    if (!s_page) return;
    lv_obj_clean(s_page);
    s_opt[0] = s_opt[1] = s_dot = NULL;
    const rg_sentence_id_t id = rg_flow_sentence(flow);
    const rg_kind_t kind = (rg_kind_t)RG_SENTENCES[id].kind;
    draw_sentence(id);
    if (kind == RG_KIND_ENTRY) {
        draw_options(RG_ENTRY_OPTIONS, RG_ENTRY_OPTION_BASELINE_0, RG_ENTRY_OPTION_BASELINE_1);
    } else if (kind == RG_KIND_WILLING) {
        draw_options(RG_WILLING_OPTIONS, RG_WILLING_OPTION_BASELINE_0, RG_WILLING_OPTION_BASELINE_1);
    }
    rg_ui_show_focus(flow);
    // Hints only where UP/DOWN/OK mean previous/next/next (as in the approved mockups).
    const bool hints = kind == RG_KIND_QUESTION || kind == RG_KIND_NOTE;
    for (int i = 0; i < 3; i++) {
        if (hints) lv_obj_remove_flag(s_hints[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_hints[i], LV_OBJ_FLAG_HIDDEN);
    }
    // No page fade: an opa animation on the full-screen container needs an
    // ARGB layer larger than the LVGL pool. Pages cut directly.
}

static void build_hints(void) {
    const lv_point_precise_t *pts[2] = {HINT_UP, HINT_DOWN};
    for (int i = 0; i < 2; i++) {
        s_hints[i] = lv_line_create(s_scr);
        lv_line_set_points(s_hints[i], pts[i], 3);
        lv_obj_set_style_line_width(s_hints[i], 1, 0);
        lv_obj_set_style_line_color(s_hints[i], hex(s_pal->faint), 0);
        lv_obj_set_style_line_rounded(s_hints[i], true, 0);
    }
    s_hints[2] = bare_obj(s_scr);
    lv_obj_set_size(s_hints[2], 2, 2);
    lv_obj_set_pos(s_hints[2], RG_HINT_X - 1, RG_HINT_Y_OK - 1);
    lv_obj_set_style_radius(s_hints[2], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_hints[2], hex(s_pal->faint), 0);
    lv_obj_set_style_bg_opa(s_hints[2], LV_OPA_COVER, 0);
}

static void status_hide(lv_timer_t *t) {
    (void)t;
    s_status_timer = NULL;          // one-shot timers delete themselves
    if (s_status) lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
}

void rg_ui_flash_status(const char *word) {
    if (!s_status) return;
    const lv_font_t *font = FONTS[RG_STATUS_FONT];
    lv_label_set_text_static(s_status, word);
    lv_obj_set_pos(s_status, (RG_SCREEN_W - text_width(word, font)) / 2,
                   rg_layout_label_top(RG_STATUS_BASELINE, font->line_height, font->base_line));
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    if (s_status_timer) {
        lv_timer_reset(s_status_timer);
    } else {
        s_status_timer = lv_timer_create(status_hide, STATUS_SHOW_MS, NULL);
        lv_timer_set_repeat_count(s_status_timer, 1);
    }
}

void rg_ui_build(const rg_sky_t *sky, const rg_flow_t *flow) {
    s_sky = *sky;
    s_rng = sky->seed ? sky->seed : 1;
    s_pal = &RG_PALETTES[rg_theme_palette(sky->day, sky->weather)];

    lv_obj_t *old = lv_screen_active();
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
    // Load the empty screen and free the old one before building children, so
    // the LVGL pool never holds two populated screens (e.g. weather -> 释放).
    // The fx timer cannot run until the caller releases the LVGL lock, and every
    // pointer it uses is reassigned below.
    lv_screen_load(s_scr);
    if (old && old != s_scr) lv_obj_delete(old);
    s_stars = s_front = s_sun_rays = s_page = s_status = s_dot = NULL;
    s_opt[0] = s_opt[1] = NULL;
    for (int i = 0; i < 3; i++) s_hints[i] = NULL;
    for (int i = 0; i < RG_CLOUD_COUNT; i++) s_clouds[i] = NULL;
    build_sky();

    s_page = bare_obj(s_scr);
    lv_obj_set_size(s_page, RG_SCREEN_W, RG_SCREEN_H);
    build_hints();
    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, FONTS[RG_STATUS_FONT], 0);
    lv_obj_set_style_text_color(s_status, hex(s_pal->faint), 0);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

    rg_ui_show_page(flow);

#if defined(CONFIG_RG_FX_ENABLE)
    if (!s_fx_timer) s_fx_timer = lv_timer_create(fx_tick, RG_FX_PERIOD_MS, NULL);
#endif
}

void rg_ui_teardown(void) {
    if (s_fx_timer) {
        lv_timer_delete(s_fx_timer);
        s_fx_timer = NULL;
    }
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
    s_scr = s_page = s_status = s_stars = s_front = s_sun_rays = s_dot = NULL;
    for (int i = 0; i < RG_CLOUD_COUNT; i++) s_clouds[i] = NULL;
    for (int i = 0; i < 3; i++) s_hints[i] = NULL;
    s_opt[0] = s_opt[1] = NULL;
    s_drop_count = s_leaf_count = 0;
}

// ---------------------------------------------------------------- glyph audit
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    int len;
    if (s[0] < 0x80) { cp = s[0]; len = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
    else { cp = s[0] & 0x07; len = 4; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

static int audit(const char *text, const lv_font_t *font) {
    int missing = 0;
    const char *p = text;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        lv_font_glyph_dsc_t g;
        memset(&g, 0, sizeof g);
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0) || g.is_placeholder) {
            ESP_LOGE(TAG, "missing glyph U+%04lX in %d px font", (unsigned long)cp,
                     font->line_height);
            missing++;
        }
    }
    return missing;
}

int rg_ui_check_glyphs(void) {
    int missing = 0;
    for (int id = 0; id < RG_SENTENCE_COUNT; id++) {
        const rg_sentence_t *s = &RG_SENTENCES[id];
        for (int l = 0; l < s->line_count; l++) {
            const rg_line_t *line = &RG_LINES[s->first_line + l];
            for (int k = 0; k < line->seg_count; k++) {
                const rg_seg_t *seg = &RG_SEGS[line->first_seg + k];
                missing += audit(seg->text, FONTS[seg->font]);
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        missing += audit(RG_ENTRY_OPTIONS[i], FONTS[RG_OPTION_FONT]);
        missing += audit(RG_WILLING_OPTIONS[i], FONTS[RG_OPTION_FONT]);
    }
    for (int i = 0; i <= RG_STATUS_MORNING; i++) missing += audit(RG_STATUS_WORDS[i], FONTS[RG_STATUS_FONT]);
    // Negative control: a character outside the subset must be reported missing.
    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof g);
    const bool control_found = lv_font_get_glyph_dsc(FONTS[RG_FONT_Q25], &g, 0x9F98, 0) && !g.is_placeholder;
    ESP_LOGI(TAG, "glyph audit: %d missing, negative control %s", missing,
             control_found ? "UNEXPECTEDLY PRESENT" : "absent (ok)");
    return missing;
}
