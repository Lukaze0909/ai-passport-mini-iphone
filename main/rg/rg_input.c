// main/rg/rg_input.c -- Release Guidance key timing (pure logic, host tested).
#include "rg_input.h"

void rg_input_init(rg_input_t *in) {
    for (int i = 0; i < RG_KEY_COUNT; i++) in->press_ms[i] = 0;
    in->hold_key = RG_KEY_NONE;
    in->hold_deadline = 0;
    in->ok_armed = false;
}

int rg_input_event(rg_input_t *in, rg_key_t key, rg_key_event_t ev, uint32_t now_ms,
                   rg_action_t out[2]) {
    if (key >= RG_KEY_COUNT) return 0;
    switch (ev) {
    case RG_KEV_PRESS:
        in->press_ms[key] = now_ms;
        in->hold_key = RG_KEY_NONE;   // any new press cancels a pending hold check
        if (key == RG_KEY_OK) {
            in->ok_armed = true;
            return 0;
        }
        out[0] = key == RG_KEY_UP ? RG_ACT_PREV : RG_ACT_NEXT;
        return 1;
    case RG_KEV_CLICK:
    case RG_KEV_DOUBLE:
        if (key != RG_KEY_OK || !in->ok_armed) return 0;
        in->ok_armed = false;
        out[0] = RG_ACT_CONFIRM;
        if (ev == RG_KEV_CLICK) return 1;
        out[1] = RG_ACT_CONFIRM;
        return 2;
    case RG_KEV_LONG:
        if (key == RG_KEY_OK) in->ok_armed = false;   // a long press never confirms
        in->hold_key = (uint8_t)key;
        {
            // If the PRESS event was lost, press_ms is stale; LONG fires ~500ms
            // after the press, so use that as the base instead.
            uint32_t base = in->press_ms[key];
            if (base == 0 || (uint32_t)(now_ms - base) > RG_HOLD_MS) base = now_ms - 500;
            in->hold_deadline = base + RG_HOLD_MS;
        }
        return 0;
    default:
        return 0;
    }
}

bool rg_input_pending(const rg_input_t *in, uint32_t *deadline) {
    if (in->hold_key == RG_KEY_NONE) return false;
    *deadline = in->hold_deadline;
    return true;
}

rg_action_t rg_input_poll(rg_input_t *in, uint32_t now_ms, rg_key_t held) {
    if (in->hold_key == RG_KEY_NONE) return RG_ACT_NONE;
    if ((int32_t)(now_ms - in->hold_deadline) < 0) return RG_ACT_NONE;
    const rg_key_t key = (rg_key_t)in->hold_key;
    in->hold_key = RG_KEY_NONE;
    if (held != key) return RG_ACT_NONE;   // released between 500 and 999 ms
    if (key == RG_KEY_OK) {
        in->ok_armed = false;              // the release must not confirm
        return RG_ACT_HOME;
    }
    return key == RG_KEY_UP ? RG_ACT_HOLD_UP : RG_ACT_HOLD_DOWN;
}

rg_key_t rg_input_key_from_mv(int mv, const uint16_t windows[RG_KEY_COUNT][2]) {
    if (mv < 0) return RG_KEY_NONE;
    for (int i = 0; i < RG_KEY_COUNT; i++) {
        if (mv >= windows[i][0] && mv < windows[i][1]) return (rg_key_t)i;
    }
    return RG_KEY_NONE;
}
