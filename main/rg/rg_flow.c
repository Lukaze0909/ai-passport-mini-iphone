// main/rg/rg_flow.c -- Release Guidance page flow (pure logic, host tested).
#include "rg_flow.h"

#include <stddef.h>

static uint8_t path_len(uint8_t path) {
    return RG_SEQS[path == RG_PATH_GOAL ? RG_SEQ_GOAL : RG_SEQ_FEEL].count;
}

static uint32_t loop_len(void) {
    return (uint32_t)RG_SEQS[RG_SEQ_LOOP_A].count + RG_SEQS[RG_SEQ_LOOP_B].count;
}

void rg_flow_init(rg_flow_t *flow) {
    flow->in_entry = true;
    flow->entry_sel = 0;
    flow->path = RG_PATH_FEEL;
    flow->pos = 0;
    flow->focus = 0;
}

void rg_flow_locate(uint8_t path, uint32_t pos, rg_seq_id_t *seq, uint8_t *index) {
    const uint32_t first = path_len(path);
    if (pos < first) {
        *seq = path == RG_PATH_GOAL ? RG_SEQ_GOAL : RG_SEQ_FEEL;
        *index = (uint8_t)pos;
        return;
    }
    const uint32_t r = (pos - first) % loop_len();
    const uint32_t a = RG_SEQS[RG_SEQ_LOOP_A].count;
    if (r < a) {
        *seq = RG_SEQ_LOOP_A;
        *index = (uint8_t)r;
    } else {
        *seq = RG_SEQ_LOOP_B;
        *index = (uint8_t)(r - a);
    }
}

rg_sentence_id_t rg_flow_sentence(const rg_flow_t *flow) {
    if (flow->in_entry) return RG_S_ENTRY;
    rg_seq_id_t seq;
    uint8_t index;
    rg_flow_locate(flow->path, flow->pos, &seq, &index);
    return (rg_sentence_id_t)RG_SEQS[seq].items[index];
}

rg_kind_t rg_flow_kind(const rg_flow_t *flow) {
    return (rg_kind_t)RG_SENTENCES[rg_flow_sentence(flow)].kind;
}

// Keep pos bounded without changing what UP/DOWN show: once two full loops
// have passed, step back one loop (the page and its predecessor are identical).
static void advance(rg_flow_t *flow) {
    flow->pos++;
    const uint32_t limit = path_len(flow->path) + 2 * loop_len();
    if (flow->pos >= limit) flow->pos -= loop_len();
    flow->focus = 0;
}

static uint32_t handle_entry(rg_flow_t *flow, rg_action_t action) {
    switch (action) {
    case RG_ACT_PREV:
        if (flow->entry_sel == 0) return RG_FX_NONE;
        flow->entry_sel = 0;
        return RG_FX_REDRAW_FOCUS | RG_FX_SOUND_TAP;
    case RG_ACT_NEXT:
        if (flow->entry_sel == 1) return RG_FX_NONE;
        flow->entry_sel = 1;
        return RG_FX_REDRAW_FOCUS | RG_FX_SOUND_TAP;
    case RG_ACT_CONFIRM:
        flow->in_entry = false;
        flow->path = flow->entry_sel ? RG_PATH_GOAL : RG_PATH_FEEL;
        flow->pos = 0;
        flow->focus = 0;
        return RG_FX_REDRAW_PAGE | RG_FX_PATH_ENTERED | RG_FX_SOUND_CONFIRM;
    case RG_ACT_HOLD_UP:
        return RG_FX_TOGGLE_MUTE;
    case RG_ACT_HOLD_DOWN:
        return RG_FX_CYCLE_THEME;
    case RG_ACT_HOME:
        return RG_FX_LEAVE;             // already at step 1: one more level up
    default:
        return RG_FX_NONE;
    }
}

uint32_t rg_flow_handle(rg_flow_t *flow, rg_action_t action) {
    if (flow->in_entry) return handle_entry(flow, action);

    if (action == RG_ACT_HOME) {
        flow->in_entry = true;          // entry_sel keeps the last choice
        flow->focus = 0;
        return RG_FX_REDRAW_PAGE | RG_FX_SOUND_TAP;
    }

    const bool willing = rg_flow_kind(flow) == RG_KIND_WILLING;
    switch (action) {
    case RG_ACT_PREV:
        if (willing) {
            if (flow->focus == 0) return RG_FX_NONE;
            flow->focus = 0;
            return RG_FX_REDRAW_FOCUS | RG_FX_SOUND_TAP;
        }
        if (flow->pos == 0) return RG_FX_NONE;     // first sentence: stay
        flow->pos--;
        flow->focus = 0;
        return RG_FX_REDRAW_PAGE | RG_FX_SOUND_TAP;
    case RG_ACT_NEXT:
        if (willing) {
            if (flow->focus == 1) return RG_FX_NONE;
            flow->focus = 1;
            return RG_FX_REDRAW_FOCUS | RG_FX_SOUND_TAP;
        }
        advance(flow);
        return RG_FX_REDRAW_PAGE | RG_FX_SOUND_TAP;
    case RG_ACT_CONFIRM:
        // Both willing answers continue identically; nothing is recorded.
        advance(flow);
        return RG_FX_REDRAW_PAGE | RG_FX_SOUND_CONFIRM;
    default:
        return RG_FX_NONE;
    }
}
