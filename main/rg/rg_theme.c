// main/rg/rg_theme.c -- theme / weather / sound-bank choice and layout helpers.
#include "rg_theme.h"

bool rg_theme_is_day(rg_theme_mode_t mode, bool clock_valid, int local_hour) {
    if (mode == RG_THEME_MODE_DUSK) return false;
    if (mode == RG_THEME_MODE_MORNING) return true;
    if (!clock_valid) return false;               // no trustworthy time: dusk
    return local_hour >= RG_DAY_START_HOUR && local_hour < RG_DAY_END_HOUR;
}

rg_palette_id_t rg_theme_palette(bool day, rg_weather_t weather) {
    if (!day) return RG_PALETTE_DUSK;
    return weather == RG_WEATHER_RAIN ? RG_PALETTE_RAIN : RG_PALETTE_MORNING;
}

rg_theme_mode_t rg_theme_next_mode(rg_theme_mode_t mode) {
    return (rg_theme_mode_t)((mode + 1) % RG_THEME_MODE_COUNT);
}

uint8_t rg_pick_other(uint32_t rnd, uint8_t count, uint8_t last) {
    if (count <= 1) return 0;
    if (last >= count) return (uint8_t)(rnd % count);
    uint8_t pick = (uint8_t)(rnd % (uint32_t)(count - 1));
    return pick >= last ? (uint8_t)(pick + 1) : pick;
}

uint32_t rg_rand_next(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int rg_layout_center(const int16_t *widths, int count, int screen_w, int16_t *xs) {
    int total = 0;
    for (int i = 0; i < count; i++) total += widths[i];
    int x = (screen_w - total) / 2;
    for (int i = 0; i < count; i++) {
        xs[i] = (int16_t)x;
        x += widths[i];
    }
    return total;
}

uint8_t rg_star_opa(int32_t sine, uint8_t size10) {
    if (sine < -32767) sine = -32767;
    if (sine > 32767) sine = 32767;
    // 0.35 + 0.65 * (sin + 1) / 2, in 0..255
    int32_t base = 89 + (166 * (sine + 32767)) / 65534;
    int32_t scaled = size10 < 11 ? base * size10 / 10 : base * (size10 - 6) / 8;
    if (scaled > 255) scaled = 255;
    if (scaled < 0) scaled = 0;
    return (uint8_t)scaled;
}

int rg_star_px(uint8_t size10) {
    return size10 < 11 ? 1 : 2;
}
