// main/classic/cs_mines.c -- Mines (对战扫雷): two-field mutual minesweeper with the companion.
//
// Threads: the MQTT task parses commands into a small ring (cs_mines_on_command)
// and posts M_MINES; everything else runs on the shell task. Game rules and
// the commit check are in cs_logic (host tested). Nothing is stored in flash:
// the game, the score and the losing streak live in RAM until a reboot.
//
// Events (passport/<user>/event, QoS 1, queued):
//   {"type":"mine_new","game":G,"size":4,"mines":3,"first":"owner"|"companion"}
//   {"type":"mine_ready","game":G}                      her mines are hidden
//   {"type":"mine_step","game":G,"cell":"B2"}           her step on the companion's field
//   {"type":"mine_companion_result","game":G,"cell":"C3","boom":false,"winner":""|"owner"|"companion"}
//   {"type":"mine_quit","game":G}
//   {"type":"mine_owner_reveal","game":G,"layout":"B3,C2"}  her mines, once per game
// Her layout leaves the device only after the game: at the end (win or loss)
// or when she quits after at least one step; during play only the result of
// each of the companion's steps goes out. A failed send is retried every 10 s.
#include "cs_mines.h"

#include <stdio.h>
#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_logic.h"
#include "cs_net.h"
#include "cs_shell.h"
#include "cs_ui.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_mines";

#define RX_SLOTS 4
#define ANSWER_TIMEOUT_MS 10000     // her step: no answer -> she may step again
#define FLIP_BACK_MS 2500           // after the companion's step: her field, then back to the companion's
#define OVERLAY_DELAY_MS 1300       // the fatal cell shows before the end note
#define SLOW_MS (3u * 60u * 1000u)  // "<name> is slow..." and the screen may sleep again

// MQTT task -> shell.
static SemaphoreHandle_t s_mtx;
static cs_mines_cmd_t s_rx[RX_SLOTS];
static uint8_t s_rx_head, s_rx_n;

// Shell task.
static cs_mines_t s_g;               // phase IDLE = setup card
static bool s_open;
static uint8_t s_size = CS_MINES_DEFAULT_SIZE;
static bool s_companion_first;
static uint8_t s_setup_focus = 2;    // Start
static int s_cursor;
static uint8_t s_field;              // 0 hers, 1 the companion's
static bool s_quit_armed;
static uint8_t s_overlay;            // cs_movl_t showing
static uint8_t s_overlay_next;       // shown at s_overlay_at
static uint32_t s_overlay_at, s_flip_at, s_answer_until, s_wait_since;
static uint16_t s_score_me, s_score_companion;
static uint8_t s_streak;             // her losses in a row (DEAD AGAIN!)
static char s_note[CS_MINES_NOTE_MAX + 1];
static char s_status_extra[24];      // one-off status ("Offline", "No answer")
static uint32_t s_rev_game;          // game whose layout was queued for the companion (0 = none)
static char s_rev_layout[16];        // "A1,B2,C3,D4" at most
static bool s_rev_pending, s_rev_sent;
static uint32_t s_rev_retry;
static cs_mines_view_t s_v;          // ~250 bytes

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void tap(void) { rg_app_sound(RG_SOUND_TAP); }
static void confirm(void) { rg_app_sound(RG_SOUND_CONFIRM); }

void cs_mines_init(void) {
    s_mtx = xSemaphoreCreateMutex();
    s_g.phase = CS_MINES_IDLE;
}

bool cs_mines_on_command(const char *json, size_t len) {
    static cs_mines_cmd_t cmd;   // MQTT task only
    if (!cs_mines_cmd_parse(json, len, &cmd)) return false;
    if (!s_mtx) return true;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_rx_n == RX_SLOTS) {    // full: the oldest goes
        s_rx_head = (uint8_t)((s_rx_head + 1) % RX_SLOTS);
        s_rx_n--;
    }
    s_rx[(s_rx_head + s_rx_n) % RX_SLOTS] = cmd;
    s_rx_n++;
    xSemaphoreGive(s_mtx);
    cs_shell_post_mines();
    return true;
}

// ---------------------------------------------------------------- events out
static bool send(const char *json) {
    const bool ok = cs_net_publish_event(json);
    ESP_LOGI(TAG, "%s %s", ok ? "sent" : "NOT sent", json);
    return ok;
}

static bool send_cell(const char *type, int cell) {
    char c[4], j[96];
    cs_mines_cell_name(cell, s_g.n, c, sizeof c);
    snprintf(j, sizeof j, "{\"type\":\"%s\",\"game\":%lu,\"cell\":\"%s\"}", type, (unsigned long)s_g.game, c);
    return send(j);
}

static void send_simple(const char *type) {
    char j[64];
    snprintf(j, sizeof j, "{\"type\":\"%s\",\"game\":%lu}", type, (unsigned long)s_g.game);
    send(j);
}

static void send_companion_result(int cell, bool boom) {
    char c[4], j[128];
    cs_mines_cell_name(cell, s_g.n, c, sizeof c);
    const char *w = s_g.phase != CS_MINES_OVER ? "" : s_g.winner == CS_MINES_OWNER_WON ? "owner" : "companion";
    snprintf(j, sizeof j, "{\"type\":\"mine_companion_result\",\"game\":%lu,\"cell\":\"%s\",\"boom\":%s,\"winner\":\"%s\"}",
             (unsigned long)s_g.game, c, boom ? "true" : "false", w);
    send(j);
}

// Her layout to the companion, after the game only (see the header comment).
static void owner_reveal_try(void) {
    char j[112];
    snprintf(j, sizeof j, "{\"type\":\"mine_owner_reveal\",\"game\":%lu,\"layout\":\"%s\"}",
             (unsigned long)s_rev_game, s_rev_layout);
    s_rev_sent = send(j);
    s_rev_pending = !s_rev_sent;
    s_rev_retry = now_ms() + 10000;
}

static void owner_reveal(void) {
    if (!cs_mines_may_reveal_mine(&s_g) || s_rev_game == s_g.game) return;   // once per game
    s_rev_game = s_g.game;
    cs_mines_layout_str(s_g.mines, s_g.n, s_rev_layout, sizeof s_rev_layout);
    owner_reveal_try();
}

// ---------------------------------------------------------------- view
static void build_view(void) {
    cs_mines_view_t *v = &s_v;
    memset(v, 0, sizeof *v);
    v->overlay = s_overlay;
    v->owner_sent = s_rev_sent && s_rev_game == s_g.game;
    if (s_g.phase == CS_MINES_IDLE) {
        v->setup = true;
        v->setup_focus = s_setup_focus;
        snprintf(v->size_txt, sizeof v->size_txt, "%ux%u · %u", CS_MINES_N[s_size], CS_MINES_N[s_size],
                 CS_MINES_M[s_size]);
        snprintf(v->first_txt, sizeof v->first_txt, "%s", s_companion_first ? CS_COMPANION_NAME : "You");
        snprintf(v->score_txt, sizeof v->score_txt, "You %u : %u " CS_COMPANION_NAME, s_score_me, s_score_companion);
        return;
    }
    const cs_mines_t *g = &s_g;
    const int cells = cs_mines_cells(g);
    v->n = g->n;
    v->field = s_field;
    v->cursor = (int8_t)s_cursor;
    for (int i = 0; i < cells; i++) {
        const uint32_t bit = 1u << i;
        uint8_t k;
        if (s_field == 0) {
            k = (g->mines & g->companion_steps & bit) ? CS_MCELL_BOOM : (g->mines & bit) ? CS_MCELL_FLAG
              : (g->companion_steps & bit) ? CS_MCELL_FOOT : CS_MCELL_SAND;
        } else {
            k = (g->my_steps & bit) ? CS_MCELL_GREEN : g->pending == i ? CS_MCELL_WAIT
              : (g->winner == CS_MINES_COMPANION_WON && g->boom == i) ? CS_MCELL_BOOM
              : (g->companion_mines & bit) ? CS_MCELL_MINE : CS_MCELL_GREY;
        }
        v->cells[i] = k;
    }
    const uint32_t now = now_ms();
    const bool slow = now - s_wait_since >= SLOW_MS;
    switch (g->phase) {
    case CS_MINES_PLACE:
        v->btn[0] = "Go";
        v->btn_on = cs_mines_count(g->mines) == g->m ? 1 : 0;
        snprintf(v->status, sizeof v->status, "%d/%u hidden", cs_mines_count(g->mines), g->m);
        break;
    case CS_MINES_READY:
        v->btn[0] = "Go";
        snprintf(v->status, sizeof v->status, "%s", slow ? CS_COMPANION_NAME " is slow..." : CS_COMPANION_NAME " hiding...");
        break;
    case CS_MINES_MY_TURN:
    case CS_MINES_MY_WAIT:
        v->btn[0] = "Flip";
        v->btn_on = 1;
        snprintf(v->status, sizeof v->status, "%s", g->phase == CS_MINES_MY_WAIT ? "..." : "Your turn");
        break;
    case CS_MINES_COMPANION_TURN:
        v->btn[0] = "Flip";
        v->btn_on = 1;
        snprintf(v->status, sizeof v->status, "%s", slow ? CS_COMPANION_NAME " is slow..." : CS_COMPANION_NAME " thinking...");
        break;
    default:   // OVER
        v->btn[0] = "Flip";
        v->btn_on = 1;
        snprintf(v->status, sizeof v->status, "%s", g->winner == CS_MINES_OWNER_WON ? "You won!" : "You lost");
        break;
    }
    if (s_status_extra[0] && g->phase != CS_MINES_OVER) snprintf(v->status, sizeof v->status, "%s", s_status_extra);
    if (g->phase == CS_MINES_OVER) {
        v->btn[1] = "Again";
        v->btn_on |= 2;
    } else {
        v->btn[1] = s_quit_armed ? "Sure?" : "Quit";
        v->btn_on |= 2;
        v->btn_warn = s_quit_armed ? 2 : 0;
    }
    // Under the board: the commit, the companion's latest thought, or the reveal check.
    if (g->phase == CS_MINES_OVER && g->reveal != CS_MINES_UNREVEALED) {
        char list[24] = "";
        size_t o = 0;
        for (int i = 0; i < cells && o + 4 < sizeof list; i++) {
            if (!(g->companion_mines >> i & 1u)) continue;
            char c[4];
            cs_mines_cell_name(i, g->n, c, sizeof c);
            o += (size_t)snprintf(list + o, sizeof list - o, "%s%s", o ? " " : "", c);
        }
        const bool ok = g->reveal == CS_MINES_VERIFIED;
        snprintf(v->note, sizeof v->note, CS_COMPANION_NAME "'s mines: %s  %s\nsha256 %s", list[0] ? list : "?",
                 ok ? "verified ✓" : "MISMATCH!", g->commit);
        v->note_kind = ok ? CS_MNOTE_OK : CS_MNOTE_BAD;
    } else if (g->phase == CS_MINES_OVER) {
        snprintf(v->note, sizeof v->note, "Waiting for " CS_COMPANION_NAME "'s reveal...\nsha256 %s", g->commit);
    } else if (s_note[0]) {
        snprintf(v->note, sizeof v->note, "\"%s\"", s_note);
        v->note_kind = CS_MNOTE_COMPANION;
    } else if (g->commit[0]) {
        snprintf(v->note, sizeof v->note, CS_COMPANION_NAME "'s mines are locked in.\nsha256 %s", g->commit);
    } else {
        snprintf(v->note, sizeof v->note, CS_COMPANION_NAME " is choosing mines...");
    }
}

static void refresh(void) {
    if (!s_open) return;
    if (!bsp_lvgl_lock(500)) return;
    if (cs_ui_current() != CS_SCR_MINES) cs_ui_show(CS_SCR_MINES);
    build_view();
    cs_ui_mines_update(&s_v);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- game flow
static void began_waiting(void) { s_wait_since = now_ms(); }

static void enter_play(void) {
    // Whoever moves first: she looks at the field she will act on / be stepped on.
    s_field = s_g.phase == CS_MINES_MY_TURN ? 1 : 0;
    if (s_g.phase == CS_MINES_COMPANION_TURN) began_waiting();
    ESP_LOGI(TAG, "game %lu: play starts, %s first", (unsigned long)s_g.game, s_g.companion_first ? "companion" : "owner");
}

static void game_over(uint32_t delay_ms) {
    if (s_g.winner == CS_MINES_COMPANION_WON) {
        s_score_companion++;
        s_streak++;
        s_overlay_next = s_streak >= 2 ? CS_MOVL_DEAD_AGAIN : CS_MOVL_DEAD;
    } else {
        s_score_me++;
        s_streak = 0;
        s_overlay_next = CS_MOVL_WIN;
    }
    s_overlay_at = now_ms() + delay_ms;
    s_flip_at = 0;
    s_quit_armed = false;
    owner_reveal();
    ESP_LOGI(TAG, "game %lu over: %s won (%u : %u)", (unsigned long)s_g.game,
             s_g.winner == CS_MINES_OWNER_WON ? "owner" : "companion", s_score_me, s_score_companion);
}

static void start_game(void) {
    if (!cs_net_status().mqtt) {
        cs_shell_toast(CS_COMPANION_NAME " offline");
        return;
    }
    cs_mines_new(&s_g, s_size, s_companion_first, esp_random() | 1u);
    char j[128];
    snprintf(j, sizeof j, "{\"type\":\"mine_new\",\"game\":%lu,\"size\":%u,\"mines\":%u,\"first\":\"%s\"}",
             (unsigned long)s_g.game, s_g.n, s_g.m, s_companion_first ? "companion" : "owner");
    if (!send(j)) {
        s_g.phase = CS_MINES_IDLE;
        cs_shell_toast(CS_COMPANION_NAME " offline");
        return;
    }
    confirm();
    s_cursor = 0;
    s_field = 0;
    s_note[0] = s_status_extra[0] = '\0';
    s_quit_armed = false;
    s_overlay = s_overlay_next = CS_MOVL_NONE;
    s_overlay_at = s_flip_at = 0;
    began_waiting();
}

static void setup_key(uint8_t btn) {
    if (btn != BSP_BTN_OK) {
        s_setup_focus = (uint8_t)cs_mines_cursor_step(s_setup_focus, btn == BSP_BTN_DOWN ? 1 : -1, 3);
        tap();
        return;
    }
    if (s_setup_focus == 0) {
        s_size = (uint8_t)((s_size + 1) % CS_MINES_SIZES);
        tap();
    } else if (s_setup_focus == 1) {
        s_companion_first = !s_companion_first;
        tap();
    } else {
        start_game();
    }
}

static void cell_ok(int cell) {
    switch (s_g.phase) {
    case CS_MINES_PLACE:
        if (s_field != 0) {
            s_field = 0;
        } else if (cs_mines_toggle(&s_g, cell)) {
            tap();
        }
        return;
    case CS_MINES_MY_TURN:
        if (s_field == 0) {               // nothing to do on her own field: go to the other
            s_field = 1;
            tap();
            return;
        }
        if (!cs_mines_step(&s_g, cell)) return;
        if (!send_cell("mine_step", cell)) {
            cs_mines_unwait(&s_g);
            snprintf(s_status_extra, sizeof s_status_extra, "Offline");
            return;
        }
        s_status_extra[0] = '\0';
        s_answer_until = now_ms() + ANSWER_TIMEOUT_MS;
        tap();
        return;
    default:
        return;
    }
}

static void board_key(uint8_t btn) {
    const int cells = cs_mines_cells(&s_g);
    if (btn != BSP_BTN_OK) {
        s_cursor = cs_mines_cursor_step(s_cursor, btn == BSP_BTN_DOWN ? 1 : -1, cells + 2);
        s_quit_armed = false;
        tap();
        return;
    }
    if (s_cursor < cells) {
        cell_ok(s_cursor);
        return;
    }
    if (s_cursor == cells) {                          // Go / Flip
        if (s_g.phase == CS_MINES_PLACE) {
            if (!cs_mines_go(&s_g)) return;
            send_simple("mine_ready");
            confirm();
            if (s_g.phase == CS_MINES_READY) began_waiting();
            else enter_play();
            s_cursor = 0;
        } else if (s_g.phase != CS_MINES_READY) {
            s_field ^= 1;
            s_flip_at = 0;
            tap();
        }
        return;
    }
    if (s_g.phase == CS_MINES_OVER) {                 // Again
        s_g.phase = CS_MINES_IDLE;
        s_setup_focus = 2;
        s_overlay = s_overlay_next = CS_MOVL_NONE;
        confirm();
        return;
    }
    if (!s_quit_armed) {                              // Quit: OK twice
        s_quit_armed = true;
        tap();
        return;
    }
    send_simple("mine_quit");
    owner_reveal();                                   // only if a step was made
    ESP_LOGI(TAG, "game %lu quit", (unsigned long)s_g.game);
    s_g.phase = CS_MINES_IDLE;
    s_quit_armed = false;
    s_setup_focus = 2;
    confirm();
}

void cs_mines_key(uint8_t btn) {
    if (!s_open) return;
    if (s_overlay_next != CS_MOVL_NONE) {             // a key during the pause: the end note now
        s_overlay = s_overlay_next;
        s_overlay_next = CS_MOVL_NONE;
    } else if (s_overlay != CS_MOVL_NONE) {           // any key closes the end note: the boards
        s_overlay = CS_MOVL_NONE;
        s_field = 1;                                  // the companion's field, with the reveal
        tap();
    } else if (s_flip_at) {                           // her field after its step: back to its field
        s_flip_at = 0;
        s_field = 1;
        tap();
    } else if (s_g.phase == CS_MINES_IDLE) {
        setup_key(btn);
    } else {
        board_key(btn);
    }
    refresh();
}

bool cs_mines_back(void) {
    if (!s_open || s_overlay == CS_MOVL_NONE) return false;
    s_overlay = CS_MOVL_NONE;
    s_field = 1;
    refresh();
    return true;
}

void cs_mines_enter(void) {
    s_open = true;
    cs_ui_show(CS_SCR_MINES);
    build_view();
    cs_ui_mines_update(&s_v);
}

void cs_mines_exit(void) {
    s_open = false;
    s_quit_armed = false;
    s_flip_at = 0;
}

// ---------------------------------------------------------------- commands in
static void apply(const cs_mines_cmd_t *c) {
    if (s_g.phase == CS_MINES_IDLE || c->game != s_g.game) {
        ESP_LOGI(TAG, "command %u for game %lu ignored (current %lu)", c->op, (unsigned long)c->game,
                 (unsigned long)s_g.game);
        return;
    }
    const int cell = c->cell[0] ? cs_mines_cell_parse(c->cell, s_g.n) : -1;
    switch (c->op) {
    case CS_MCMD_COMMIT: {
        const uint8_t was = s_g.phase;
        if (!cs_mines_commit(&s_g, c->commit)) {
            ESP_LOGW(TAG, "commit refused");
            return;
        }
        ESP_LOGI(TAG, "commit %s", s_g.commit);
        if (was == CS_MINES_READY && s_g.phase != CS_MINES_READY) enter_play();
        return;
    }
    case CS_MCMD_RESULT:
        if (!cs_mines_result(&s_g, cell, c->boom)) {
            ESP_LOGI(TAG, "result for %s ignored", c->cell);
            return;
        }
        s_answer_until = 0;
        s_note[0] = '\0';
        s_field = 1;
        if (s_g.phase == CS_MINES_OVER) game_over(c->boom ? OVERLAY_DELAY_MS : 600);
        else began_waiting();
        if (!s_open && s_g.phase == CS_MINES_OVER) {
            cs_shell_wake();
            cs_shell_toast(s_g.winner == CS_MINES_OWNER_WON ? "Mines: you won!" : "Mines: DEAD!");
        }
        return;
    case CS_MCMD_COMPANION_STEP: {
        const int r = cs_mines_companion_step(&s_g, cell);
        if (r == -1) {
            ESP_LOGW(TAG, "companion step %s refused (phase %u)", c->cell, s_g.phase);
            return;
        }
        if (r == -2) {                                // re-delivery / re-send: answer again
            send_companion_result(cell, (s_g.mines >> cell) & 1u);
            return;
        }
        send_companion_result(cell, r == 1);
        snprintf(s_note, sizeof s_note, "%s", c->note);
        s_field = 0;                                  // show where its step landed
        s_status_extra[0] = '\0';
        if (s_g.phase == CS_MINES_OVER) game_over(r == 1 ? OVERLAY_DELAY_MS : 900);
        else s_flip_at = s_open ? now_ms() + FLIP_BACK_MS : 0;
        if (s_open) {
            tap();
        } else {
            char t[40];
            snprintf(t, sizeof t, "Mines: " CS_COMPANION_NAME " stepped %s", c->cell);
            cs_shell_wake();
            cs_shell_toast(t);
        }
        return;
    }
    case CS_MCMD_REVEAL:
        if (!cs_mines_reveal(&s_g, c->layout, c->salt)) return;
        ESP_LOGI(TAG, "reveal %s: %s", c->layout, s_g.reveal == CS_MINES_VERIFIED ? "verified" : "MISMATCH");
        return;
    default:
        return;
    }
}

void cs_mines_event(void) {
    for (;;) {
        cs_mines_cmd_t c;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        const bool have = s_rx_n > 0;
        if (have) {
            c = s_rx[s_rx_head];
            s_rx_head = (uint8_t)((s_rx_head + 1) % RX_SLOTS);
            s_rx_n--;
        }
        xSemaphoreGive(s_mtx);
        if (!have) break;
        apply(&c);
    }
    refresh();
}

void cs_mines_poll(uint32_t now) {
    bool changed = false;
    if (s_g.phase == CS_MINES_MY_WAIT && s_answer_until && (int32_t)(now - s_answer_until) >= 0) {
        ESP_LOGW(TAG, "no answer for %d in %d s", s_g.pending, ANSWER_TIMEOUT_MS / 1000);
        s_cursor = s_g.pending;                       // OK again re-sends the same cell
        cs_mines_unwait(&s_g);
        s_answer_until = 0;
        snprintf(s_status_extra, sizeof s_status_extra, "No answer");
        changed = true;
    }
    if (s_flip_at && (int32_t)(now - s_flip_at) >= 0) {
        s_flip_at = 0;
        s_field = 1;
        changed = true;
    }
    if (s_overlay_next && (int32_t)(now - s_overlay_at) >= 0 && s_open) {
        s_overlay = s_overlay_next;
        s_overlay_next = CS_MOVL_NONE;
        confirm();
        changed = true;
    }
    if (s_rev_pending && (int32_t)(now - s_rev_retry) >= 0) {
        if (cs_net_status().mqtt) owner_reveal_try();
        else s_rev_retry = now + 10000;
        changed = changed || s_rev_sent;
    }
    // The waiting line turns "slow" once.
    static bool was_slow;
    const bool waiting = s_g.phase == CS_MINES_READY || s_g.phase == CS_MINES_COMPANION_TURN;
    const bool slow = waiting && now - s_wait_since >= SLOW_MS;
    if (slow != was_slow) {
        was_slow = slow;
        changed = true;
    }
    if (changed) refresh();
}

bool cs_mines_hold(uint32_t now) {
    if (!s_open) return false;
    if (s_g.phase == CS_MINES_MY_WAIT || s_overlay_next) return true;
    return (s_g.phase == CS_MINES_READY || s_g.phase == CS_MINES_COMPANION_TURN) && now - s_wait_since < SLOW_MS;
}
