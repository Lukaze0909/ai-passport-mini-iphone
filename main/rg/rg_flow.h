// main/rg/rg_flow.h -- Release Guidance page flow (pure logic, host tested).
//
// Entry page -> feelings path (4) or goal path (8) -> loop A (4) / loop B (6)
// alternating forever. Nothing about answers is recorded; the only side effect
// reported to the caller is RG_FX_PATH_ENTERED for the standby statistics.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rg_content.h"

typedef enum {
    RG_ACT_NONE = 0,
    RG_ACT_PREV,        // UP
    RG_ACT_NEXT,        // DOWN
    RG_ACT_CONFIRM,     // OK short press
    RG_ACT_HOME,        // OK held ~1 s
    RG_ACT_HOLD_UP,     // UP held ~1 s (entry page: sound on/off)
    RG_ACT_HOLD_DOWN,   // DOWN held ~1 s (entry page: theme auto/dusk/morning)
} rg_action_t;

typedef enum {
    RG_PATH_FEEL = 0,
    RG_PATH_GOAL = 1,
} rg_path_t;

typedef enum {
    RG_FX_NONE          = 0,
    RG_FX_REDRAW_PAGE   = 1 << 0,   // sentence/page changed: rebuild text
    RG_FX_REDRAW_FOCUS  = 1 << 1,   // only the option focus changed
    RG_FX_PATH_ENTERED  = 1 << 2,   // count +1 for flow.path (entry -> path)
    RG_FX_SOUND_TAP     = 1 << 3,
    RG_FX_SOUND_CONFIRM = 1 << 4,
    RG_FX_TOGGLE_MUTE   = 1 << 5,
    RG_FX_CYCLE_THEME   = 1 << 6,
    RG_FX_LEAVE         = 1 << 7,   // OK held on the entry page: shell mode leaves the app
} rg_fx_t;

typedef struct {
    bool in_entry;          // true: entry page
    uint8_t entry_sel;      // 0 = 释放感受, 1 = 释放目标
    uint8_t path;           // rg_path_t of the running path
    uint32_t pos;           // linear position: path pages, then A,B,A,B...
    uint8_t focus;          // willing page: 0 = 愿意, 1 = 还不愿意
} rg_flow_t;

void rg_flow_init(rg_flow_t *flow);

// Applies one action and returns an rg_fx_t bit set.
uint32_t rg_flow_handle(rg_flow_t *flow, rg_action_t action);

// Sequence and index shown at a linear position of a path.
void rg_flow_locate(uint8_t path, uint32_t pos, rg_seq_id_t *seq, uint8_t *index);

// Sentence currently shown (RG_S_ENTRY on the entry page).
rg_sentence_id_t rg_flow_sentence(const rg_flow_t *flow);

// Sentence kind of the current page.
rg_kind_t rg_flow_kind(const rg_flow_t *flow);
