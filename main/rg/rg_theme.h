// main/rg/rg_theme.h -- theme / weather / sound-bank choice and layout helpers
// (pure logic, host tested).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rg_content.h"

typedef enum {
    RG_THEME_MODE_AUTO = 0,     // by local time when the clock is valid, else dusk
    RG_THEME_MODE_DUSK,
    RG_THEME_MODE_MORNING,
    RG_THEME_MODE_COUNT
} rg_theme_mode_t;

typedef enum {
    RG_WEATHER_SUN = 0,
    RG_WEATHER_RAIN,
    RG_WEATHER_RAINBOW,
    RG_WEATHER_LEAVES,
    RG_WEATHER_COUNT
} rg_weather_t;

// Daytime window in local (Asia/Shanghai) hours: [6, 18).
#define RG_DAY_START_HOUR 6
#define RG_DAY_END_HOUR 18

bool rg_theme_is_day(rg_theme_mode_t mode, bool clock_valid, int local_hour);
rg_palette_id_t rg_theme_palette(bool day, rg_weather_t weather);
rg_theme_mode_t rg_theme_next_mode(rg_theme_mode_t mode);

// Uniform pick in [0, count) that differs from `last` when last < count and count > 1.
uint8_t rg_pick_other(uint32_t rnd, uint8_t count, uint8_t last);

// Small deterministic generator for animation jitter (xorshift32, never 0).
uint32_t rg_rand_next(uint32_t *state);

// Centres a run of segment widths on the screen; writes each segment's left x
// and returns the total width.
int rg_layout_center(const int16_t *widths, int count, int screen_w, int16_t *xs);

// LVGL places a label by its top; text sits on top + line_height - base_line.
static inline int rg_layout_label_top(int baseline, int line_height, int base_line) {
    return baseline - (line_height - base_line);
}

// Star brightness 0..255 from the sine of its phase (-32767..32767): 35 %..100 %,
// then scaled by its nominal size (0.7..1.4 px drawn as 1 px or 2 px dots).
uint8_t rg_star_opa(int32_t sine, uint8_t size10);
int rg_star_px(uint8_t size10);
