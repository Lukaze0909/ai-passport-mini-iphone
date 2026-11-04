// main/rg/rg_input.h -- Release Guidance key timing (pure logic, host tested).
//
// BSP events: PRESS (down), CLICK (single, ~180 ms after release), DOUBLE,
// LONG (held 500 ms, BSP-wide threshold). RELEASE (BSP_BTN_RELEASE) is ignored here. The app owns its own ~1 s hold:
// on LONG it schedules a deadline at press + RG_HOLD_MS; at the deadline the
// caller samples whether the key is still held. Releasing between 500 and
// 999 ms therefore does nothing (and the BSP sends no CLICK after LONG).
//   UP / DOWN: act immediately on PRESS (previous / next).
//   OK: CLICK confirms, DOUBLE confirms twice; a confirm needs an OK PRESS seen
//       after the last hold, so the release after a 1 s return never confirms.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rg_flow.h"

#define RG_HOLD_MS 1000u

typedef enum {   // same order as bsp_btn_t
    RG_KEY_UP = 0,
    RG_KEY_DOWN,
    RG_KEY_OK,
    RG_KEY_COUNT,
    RG_KEY_NONE = 0xFF,
} rg_key_t;

typedef enum {   // same order as bsp_btn_ev_t
    RG_KEV_PRESS = 0,
    RG_KEV_CLICK,
    RG_KEV_DOUBLE,
    RG_KEV_LONG,
} rg_key_event_t;

typedef struct {
    uint32_t press_ms[RG_KEY_COUNT];
    uint8_t hold_key;          // rg_key_t or RG_KEY_NONE
    uint32_t hold_deadline;
    bool ok_armed;
} rg_input_t;

void rg_input_init(rg_input_t *in);

// Feeds one BSP event. Writes up to two actions; returns how many.
int rg_input_event(rg_input_t *in, rg_key_t key, rg_key_event_t ev, uint32_t now_ms,
                   rg_action_t out[2]);

// True and *deadline set while a hold check is pending.
bool rg_input_pending(const rg_input_t *in, uint32_t *deadline);

// Call once now >= deadline with the key currently held (RG_KEY_NONE if none).
// Returns the hold action or RG_ACT_NONE; the pending check is cleared.
rg_action_t rg_input_poll(rg_input_t *in, uint32_t now_ms, rg_key_t held);

// Maps an ADC reading to a key using half-open [lo, hi) windows (BSP table order).
rg_key_t rg_input_key_from_mv(int mv, const uint16_t windows[RG_KEY_COUNT][2]);
