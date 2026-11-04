// main/rg/rg_ui.h -- Release Guidance LVGL screens and sky animation.
// Every function here touches LVGL: call from the LVGL task or with bsp_lvgl_lock().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rg_flow.h"
#include "rg_theme.h"

typedef struct {
    bool day;
    rg_weather_t weather;
    bool rainbow_left;
    uint32_t seed;          // animation jitter seed
} rg_sky_t;

// Builds (or rebuilds after a theme change) the whole screen and shows `flow`.
void rg_ui_build(const rg_sky_t *sky, const rg_flow_t *flow);

// Rebuilds the text of the current page.
void rg_ui_show_page(const rg_flow_t *flow);

// Updates option colours / dot only (entry and willing pages).
void rg_ui_show_focus(const rg_flow_t *flow);

// Shows a faint status word near the bottom for about a second.
void rg_ui_flash_status(const char *word);

// Deletes the animation / status timers and forgets every object (shell mode:
// the next screen load deletes the objects themselves).
void rg_ui_teardown(void);

// Logs every glyph of the fixed copy that is missing from its font; returns the count.
int rg_ui_check_glyphs(void);
