// main/classic/cs_bingo.c -- Bingo (生存 Bingo): the owner's daily card.
//
// Threads: the MQTT task parses a bingo_card command into a heap copy (only the
// newest is kept: a card is the whole state) and posts M_BINGO; everything else
// runs on the shell task. Merge rules, lines and the day boundary are in
// cs_logic (host tested). The card (with her unsynced changes) is cached in
// NVS "classic/bingo" (one blob, written on change); the size choice in
// "classic/bgsize".
//
// Events (passport/<user>/event, QoS 1, queued):
//   {"type":"bingo_get","n":4,"day":"2026-11-02"}        today's card, please (n: size for a new one)
//   {"type":"bingo_tick","day":D,"cell":"MAG","on":true,"seq":17}   her tick / untick
//   {"type":"bingo_reroll","day":D,"cell":"SUN","seq":18}           swap one unticked cell
//   {"type":"bingo_size","day":D,"n":5,"seq":19}         today's card again at another size
//   {"type":"bingo_bingo","day":D,"line":"row 2","cells":"MAG,SUN,VEG,TIDY"}   her first line
// Every change carries a new number; the server answers with the whole card and
// "ack" (the highest number it applied). Unacknowledged ticks are re-sent every
// 20 s while MQTT is up, and survive a reboot.
#include "cs_bingo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_logic.h"
#include "cs_net.h"
#include "cs_shell.h"
#include "cs_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_bingo";

#define NVS_NS "classic"
#define CLOCK_VALID_AFTER 1735689600u   // 2025-01-01
#define GET_EVERY_MS 60000u             // no card / an old one: ask again
#define PUSH_EVERY_MS 20000u            // unacknowledged ticks: send again
#define BUSY_MS 15000u                  // swap / size: no answer -> give up
#define STATUS_MS 4000u
#define AGAIN_MS 2500u                  // a later line's note closes itself

// MQTT task -> shell: the newest card (heap, ~340 bytes, freed by the shell).
static SemaphoreHandle_t s_mtx;
static cs_bingo_cmd_t *s_rx;

// Shell task.
static cs_bingo_card_t s_card;          // cached card (NVS), ~450 bytes
static volatile bool s_done_today;
static bool s_open, s_swap, s_size_armed, s_was_mqtt, s_bingo_out, s_stale;
static uint8_t s_size = 4;              // size for a new card
static uint8_t s_overlay;               // cs_bovl_t
static int s_cursor;
static uint32_t s_gen = 1, s_busy, s_busy_seq;
static uint32_t s_busy_until, s_status_until, s_overlay_until, s_next_get, s_next_push, s_next_bingo, s_last_get;
static char s_status[40];
static char s_today[CS_BINGO_DAY_LEN + 1];
static char s_toasted[CS_BINGO_DAY_LEN + 1];   // day whose first line was toasted (app closed)
static cs_bingo_view_t s_v;                    // ~150 bytes

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void tap(void) { rg_app_sound(RG_SOUND_TAP); }
static void confirm(void) { rg_app_sound(RG_SOUND_CONFIRM); }
static bool online(void) { return cs_net_status().mqtt; }
static int cells(void) { return s_card.n ? cs_bingo_cells(&s_card) : 0; }

// ---------------------------------------------------------------- NVS
static void save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "bingo", &s_card, sizeof s_card) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void save_size(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "bgsize", s_size) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void cs_bingo_init(void) {
    s_mtx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "bgsize", &v) == ESP_OK && (v == 4 || v == 5)) s_size = v;
    size_t len = sizeof s_card;
    if (nvs_get_blob(h, "bingo", &s_card, &len) != ESP_OK || len != sizeof s_card ||
        (s_card.n != 0 && s_card.n != 4 && s_card.n != 5)) {
        memset(&s_card, 0, sizeof s_card);
    }
    s_card.day[CS_BINGO_DAY_LEN] = '\0';
    s_card.whisper[CS_BINGO_WHISPER_MAX] = '\0';
    for (int i = 0; i < CS_BINGO_MAX_CELLS; i++) s_card.cells[i][CS_BINGO_LABEL_MAX] = '\0';
    nvs_close(h);
    ESP_LOGI(TAG, "cached card %s %ux%u: %d done, %d unsynced", s_card.day[0] ? s_card.day : "-", s_card.n,
             s_card.n, s_card.n ? cs_bingo_done_count(&s_card) : 0, cs_mines_count(s_card.pending));
}

static bool has(const char *s, size_t len, const char *what) {
    const size_t w = strlen(what);
    for (size_t i = 0; i + w <= len; i++) {
        if (memcmp(s + i, what, w) == 0) return true;
    }
    return false;
}

bool cs_bingo_on_command(const char *json, size_t len) {
    if (!has(json, len, "\"bingo_card\"")) return false;
    cs_bingo_cmd_t *m = malloc(sizeof *m);
    if (!m) {
        ESP_LOGW(TAG, "no memory for a card");
        return true;
    }
    if (!cs_bingo_cmd_parse(json, len, m)) {
        ESP_LOGW(TAG, "card rejected (%u bytes)", (unsigned)len);
        free(m);
        return true;
    }
    if (!s_mtx) {
        free(m);
        return true;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cs_bingo_cmd_t *old = s_rx;
    s_rx = m;
    xSemaphoreGive(s_mtx);
    free(old);
    cs_shell_post_bingo();
    return true;
}

// ---------------------------------------------------------------- events out
static bool send(const char *json) {
    const bool ok = cs_net_publish_event(json);
    ESP_LOGI(TAG, "%s %s", ok ? "sent" : "NOT sent", json);
    return ok;
}

static void send_get(void) {
    char j[80];
    snprintf(j, sizeof j, "{\"type\":\"bingo_get\",\"n\":%u,\"day\":\"%s\"}", s_size, s_card.day);
    send(j);
    s_last_get = now_ms();
}

static bool send_tick(int i) {
    char j[128];
    snprintf(j, sizeof j, "{\"type\":\"bingo_tick\",\"day\":\"%s\",\"cell\":\"%s\",\"on\":%s,\"seq\":%lu}",
             s_card.day, s_card.cells[i], (s_card.done >> i & 1u) ? "true" : "false",
             (unsigned long)s_card.pend_seq[i]);
    return send(j);
}

static void push_pending(void) {
    for (int i = 0; i < cells(); i++) {
        if ((s_card.pending >> i & 1u) && !send_tick(i)) return;
    }
}

static void send_bingo(void) {
    char name[12], list[CS_BINGO_MAX_N * (CS_BINGO_LABEL_MAX + 1) + 1] = "", j[160];
    uint32_t line;
    if (!cs_bingo_first_line(s_card.done, s_card.n, name, sizeof name, &line)) {
        s_bingo_out = false;   // unticked again before it went out
        return;
    }
    size_t o = 0;
    for (int i = 0; i < cells(); i++) {
        if (line >> i & 1u) o += (size_t)snprintf(list + o, sizeof list - o, "%s%s", o ? "," : "", s_card.cells[i]);
    }
    snprintf(j, sizeof j, "{\"type\":\"bingo_bingo\",\"day\":\"%s\",\"line\":\"%s\",\"cells\":\"%s\"}", s_card.day,
             name, list);
    send(j);
}

// ---------------------------------------------------------------- view
static void status(const char *t) {
    snprintf(s_status, sizeof s_status, "%s", t);
    s_status_until = now_ms() + STATUS_MS;
}

// "Mon Nov 2" from "2026-11-02".
static void day_text(const char *day, char *out, size_t cap) {
    static const char *const WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MO[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, m = 0, d = 0;
    for (int i = 0; i < 4; i++) y = y * 10 + (day[i] - '0');
    m = (day[5] - '0') * 10 + (day[6] - '0');
    d = (day[8] - '0') * 10 + (day[9] - '0');
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        snprintf(out, cap, "%s", day);
        return;
    }
    static const int T[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};   // Sakamoto
    const int yy = m < 3 ? y - 1 : y;
    const int wd = (yy + yy / 4 - yy / 100 + yy / 400 + T[m - 1] + d) % 7;
    snprintf(out, cap, "%s %s %d", WD[wd], MO[m - 1], d);
}

static bool fresh(void) { return s_card.n && !s_stale; }

// The Bingo screen draws straight from s_card (LVGL task): change it under the LVGL lock.
static bool card_lock(void) { return bsp_lvgl_lock(1000); }
static void card_unlock(bool locked) {
    if (locked) bsp_lvgl_unlock();
}

static void build_view(cs_bingo_view_t *v) {
    memset(v, 0, sizeof *v);
    const bool mq = online();
    const uint32_t now = now_ms();
    v->card = fresh() ? &s_card : NULL;
    v->gen = s_gen;
    v->cursor = (int8_t)s_cursor;
    v->swap = s_swap;
    v->busy = s_busy;
    v->overlay = s_overlay;
    const int n = v->card ? cells() : 0;
    if (n) v->pct = (uint8_t)((cs_bingo_done_count(&s_card) * 100 + n / 2) / n);
    // Buttons: Swap (needs the companion), size, companion (the whisper, after the first line).
    v->btn[0] = s_swap ? "Cancel" : "Swap";
    if (n && mq && !s_busy) v->btn_on |= 1;
    static char size_txt[2][12];
    const unsigned cur = n && s_card.n == 5 ? 5u : !n && s_size == 5 ? 5u : 4u, other = 9u - cur;
    snprintf(size_txt[0], sizeof size_txt[0], "%ux%u", cur, cur);
    snprintf(size_txt[1], sizeof size_txt[1], "%ux%u?", other, other);
    v->btn[1] = s_size_armed ? size_txt[1] : size_txt[0];
    if (!s_busy) v->btn_on |= 2;
    if (s_size_armed) v->btn_warn |= 2;
    v->btn[2] = CS_COMPANION_NAME;
    if (n && (s_card.celebrated || s_card.bingo)) v->btn_on |= 4;
    // The line under the title: a status, else the date and the count.
    if (s_status[0] && (int32_t)(s_status_until - now) > 0) {
        snprintf(v->line2, sizeof v->line2, "%s", s_status);
    } else if (s_swap) {
        snprintf(v->line2, sizeof v->line2, "Pick a cell to swap");
    } else if (s_size_armed) {
        snprintf(v->line2, sizeof v->line2, "OK again: new %ux%u card", other, other);
    } else if (s_busy) {
        snprintf(v->line2, sizeof v->line2, "Asking " CS_COMPANION_NAME "...");
    } else if (n) {
        char d[16];
        day_text(s_card.day, d, sizeof d);
        const int unsynced = cs_mines_count(s_card.pending);
        if (unsynced && !mq) snprintf(v->line2, sizeof v->line2, "%s · %d to sync", d, unsynced);
        else snprintf(v->line2, sizeof v->line2, "%s · %d of %d", d, cs_bingo_done_count(&s_card), n);
    } else if (s_today[0]) {
        day_text(s_today, v->line2, sizeof v->line2);
    }
    if (!n) {
        const cs_net_status_t ns = cs_net_status();
        snprintf(v->msg, sizeof v->msg, "%s",
                 mq ? "Asking " CS_COMPANION_NAME " for today's card..."
                 : ns.link == CS_LINK_HOME || ns.link == CS_LINK_HOTSPOT ? "Connecting to " CS_COMPANION_NAME "..."
                 : s_stale ? "New day! Today's card needs Wi-Fi."
                           : "Today's card comes from " CS_COMPANION_NAME ". Turn on Wi-Fi in Settings.");
    }
    if (s_overlay == CS_BOVL_FIRST) {
        v->head = CS_COMPANION_NAME " whispers:";
        v->note = s_card.whisper[0] ? s_card.whisper
                  : mq             ? "The note is on its way..."
                                   : "The note unlocks when you're online.";
    } else if (s_overlay == CS_BOVL_AGAIN) {
        static char again[24];
        snprintf(again, sizeof again, "%d lines!", cs_bingo_lines(s_card.done, s_card.n, NULL));
        v->head = "";
        v->note = again;
    }
}

static void refresh(void) {
    if (!s_open) return;
    build_view(&s_v);
    if (!bsp_lvgl_lock(500)) return;
    if (cs_ui_current() != CS_SCR_BINGO) cs_ui_show(CS_SCR_BINGO);
    cs_ui_bingo_update(&s_v);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- lines
// After any change: the first line opens the note with the companion's whisper (once a
// day); a further line flashes BINGO! again. With the app closed the screen
// wakes with a toast and the note waits for the next opening.
static void check_lines(void) {
    if (!fresh()) {
        s_done_today = false;
        return;
    }
    const int lines = cs_bingo_lines(s_card.done, s_card.n, NULL);
    s_done_today = lines > 0;
    bool dirty = false;
    if (lines > 0 && !s_card.celebrated) {
        if (!s_card.bingo && !s_bingo_out) {
            s_bingo_out = true;                    // tell the server: it answers with the whisper
            s_next_bingo = now_ms();
        }
        if (s_open) {
            s_card.celebrated = 1;
            s_card.lines_seen = (uint8_t)lines;
            s_overlay = CS_BOVL_FIRST;
            confirm();
            dirty = true;
            ESP_LOGI(TAG, "BINGO (%d line%s)", lines, lines > 1 ? "s" : "");
        } else if (strcmp(s_toasted, s_card.day) != 0) {
            memcpy(s_toasted, s_card.day, sizeof s_toasted);   // once; the note shows at the next opening
            cs_shell_wake();
            cs_shell_toast("BINGO! " CS_COMPANION_NAME " left you a note");
        }
    } else if (s_card.celebrated && lines > s_card.lines_seen) {
        s_card.lines_seen = (uint8_t)lines;
        if (s_open) {
            s_overlay = CS_BOVL_AGAIN;
            s_overlay_until = now_ms() + AGAIN_MS;
            confirm();
        }
        dirty = true;
    } else if (s_card.celebrated && lines < s_card.lines_seen) {
        s_card.lines_seen = (uint8_t)lines;
        dirty = true;
    }
    if (dirty) save();
}

// ---------------------------------------------------------------- keys
static void first_open_cell(void) {
    s_cursor = 0;
    for (int i = 0; i < cells(); i++) {
        if (!(s_card.done >> i & 1u)) {
            s_cursor = i;
            return;
        }
    }
}

static void cell_ok(int i) {
    const uint32_t bit = 1u << i;
    if (s_swap) {
        if (s_card.done & bit) {
            status("Ticked cells stay");
        } else if (s_card.fixed & bit) {
            status("That one is always on");
        } else if (!online()) {
            status("Swap needs Wi-Fi");
        } else if (s_card.pending) {
            push_pending();
            status("Syncing, try again");
        } else {
            char j[112];
            const uint32_t seq = ++s_card.seq;
            snprintf(j, sizeof j, "{\"type\":\"bingo_reroll\",\"day\":\"%s\",\"cell\":\"%s\",\"seq\":%lu}",
                     s_card.day, s_card.cells[i], (unsigned long)seq);
            if (send(j)) {
                s_busy = bit;
                s_busy_seq = seq;
                s_busy_until = now_ms() + BUSY_MS;
                s_swap = false;
                save();
                confirm();
                return;
            }
            status("Swap needs Wi-Fi");
        }
        tap();
        return;
    }
    if (s_busy & bit) return;
    const bool lk = card_lock();
    const uint32_t seq = cs_bingo_toggle(&s_card, i);
    card_unlock(lk);
    if (!seq) return;
    if (s_card.done & bit) confirm();
    else tap();
    save();
    if (online()) {
        send_tick(i);
        s_next_push = now_ms() + PUSH_EVERY_MS;
    }
    check_lines();
}

static void size_ok(void) {
    if (!s_size_armed) {
        s_size_armed = true;
        tap();
        return;
    }
    s_size_armed = false;
    const uint8_t next = (uint8_t)(9 - (s_card.n && fresh() ? s_card.n : s_size));
    if (!fresh()) {                                   // no card yet: the size of the next one
        s_size = next;
        save_size();
        confirm();
        if (online()) send_get();
        return;
    }
    if (!online()) {
        status("New size needs Wi-Fi");
        tap();
        return;
    }
    if (s_card.pending) {
        push_pending();
        status("Syncing, try again");
        tap();
        return;
    }
    char j[96];
    const uint32_t seq = ++s_card.seq;
    snprintf(j, sizeof j, "{\"type\":\"bingo_size\",\"day\":\"%s\",\"n\":%u,\"seq\":%lu}", s_card.day, next,
             (unsigned long)seq);
    if (!send(j)) {
        status("New size needs Wi-Fi");
        return;
    }
    s_size = next;
    save_size();
    save();
    s_busy = (1u << cells()) - 1u;
    s_busy_seq = seq;
    s_busy_until = now_ms() + BUSY_MS;
    confirm();
}

void cs_bingo_key(uint8_t btn) {
    if (!s_open) return;
    if (s_overlay != CS_BOVL_NONE) {                  // any key closes the note
        s_overlay = CS_BOVL_NONE;
        tap();
        refresh();
        return;
    }
    const int n = fresh() ? cells() : 0;
    if (btn != BSP_BTN_OK) {
        s_cursor = cs_mines_cursor_step(s_cursor, btn == BSP_BTN_DOWN ? 1 : -1, n + 3);
        s_size_armed = false;
        tap();
        refresh();
        return;
    }
    if (s_cursor < n) {
        cell_ok(s_cursor);
    } else if (s_cursor == n) {                       // Swap / Cancel
        if (s_swap) {
            s_swap = false;
            tap();
        } else if (!n || s_busy) {
            tap();
        } else if (!online()) {
            status("Swap needs Wi-Fi");
            tap();
        } else {
            s_swap = true;
            s_cursor = 0;
            for (int i = 0; i < n; i++) {             // first cell that can go
                if (!((s_card.done | s_card.fixed) >> i & 1u)) {
                    s_cursor = i;
                    break;
                }
            }
            tap();
        }
    } else if (s_cursor == n + 1) {
        if (!s_busy) size_ok();
    } else if (n && (s_card.celebrated || s_card.bingo)) {   // companion button: read the whisper again
        s_overlay = CS_BOVL_FIRST;
        tap();
    } else {
        status("Get a line first!");
        tap();
    }
    refresh();
}

bool cs_bingo_back(void) {
    if (!s_open) return false;
    if (s_overlay != CS_BOVL_NONE) {
        s_overlay = CS_BOVL_NONE;
    } else if (s_swap || s_size_armed) {
        s_swap = s_size_armed = false;
    } else {
        return false;
    }
    refresh();
    return true;
}

void cs_bingo_enter(void) {
    s_open = true;
    s_swap = s_size_armed = false;
    s_overlay = CS_BOVL_NONE;
    first_open_cell();
    if (online() && now_ms() - s_last_get > 3000) send_get();   // fresh ticks from the server
    check_lines();                                              // a line made while closed: the note now
    cs_ui_show(CS_SCR_BINGO);
    build_view(&s_v);
    cs_ui_bingo_update(&s_v);
}

void cs_bingo_exit(void) {
    s_open = false;
    s_swap = s_size_armed = false;
    s_overlay = CS_BOVL_NONE;
}

// ---------------------------------------------------------------- cards in
void cs_bingo_event(void) {
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    cs_bingo_cmd_t *m = s_rx;
    s_rx = NULL;
    xSemaphoreGive(s_mtx);
    if (!m) return;
    if (s_card.n && strcmp(m->day, s_card.day) < 0) {          // a late card for an earlier day
        ESP_LOGI(TAG, "card for %s ignored (have %s)", m->day, s_card.day);
        free(m);
        return;
    }
    const bool new_day = strcmp(m->day, s_card.day) != 0;
    const bool had_whisper = s_card.whisper[0] != '\0';
    if (s_busy_seq && m->ack >= s_busy_seq) s_busy = s_busy_seq = 0;
    const bool lk = card_lock();
    const bool layout = cs_bingo_merge(&s_card, m);
    card_unlock(lk);
    free(m);
    if (layout) {
        s_gen++;
        if (s_cursor >= cells() + 3) s_cursor = 0;
    }
    if (new_day) {
        s_bingo_out = false;
        s_overlay = CS_BOVL_NONE;
        s_swap = s_size_armed = false;
        first_open_cell();
    }
    if (s_card.bingo) s_bingo_out = false;
    if (s_card.n && s_card.n != s_size) {
        s_size = s_card.n;
        save_size();
    }
    s_stale = s_today[0] && strcmp(s_card.day, s_today) < 0;
    save();
    ESP_LOGI(TAG, "card %s %ux%u: %d done (auto 0x%lx), %d unsynced, bingo %d%s", s_card.day, s_card.n, s_card.n,
             cs_bingo_done_count(&s_card), (unsigned long)s_card.autod, cs_mines_count(s_card.pending), s_card.bingo,
             s_card.whisper[0] ? ", whisper in" : "");
    if (!had_whisper && s_card.whisper[0] && s_card.celebrated && s_overlay != CS_BOVL_FIRST) {
        status("New note: press " CS_COMPANION_NAME);
    }
    check_lines();
    refresh();
}

void cs_bingo_poll(uint32_t now) {
    const bool mq = online();
    if (mq && !s_was_mqtt) s_next_get = s_next_push = s_next_bingo = now;   // just connected
    s_was_mqtt = mq;
    bool changed = false;
    const time_t t = time(NULL);
    if (t >= (time_t)CLOCK_VALID_AFTER) {
        char d[CS_BINGO_DAY_LEN + 1];
        cs_bingo_card_day((uint32_t)t, d);
        if (strcmp(d, s_today) != 0) {
            memcpy(s_today, d, sizeof s_today);
            changed = true;
        }
    }
    const bool stale = s_card.n && s_today[0] && strcmp(s_card.day, s_today) < 0;
    if (stale != s_stale) {
        s_stale = stale;
        if (stale) {
            s_swap = s_size_armed = false;
            s_overlay = CS_BOVL_NONE;
            s_next_get = now;
            ESP_LOGI(TAG, "card %s is over (today %s)", s_card.day, s_today);
        }
        check_lines();
        changed = true;
    }
    if (mq && s_card.pending && (int32_t)(now - s_next_push) >= 0) {
        push_pending();
        s_next_push = now + PUSH_EVERY_MS;
    }
    if (mq && (!s_card.n || s_stale) && (int32_t)(now - s_next_get) >= 0) {
        send_get();
        s_next_get = now + GET_EVERY_MS;
    }
    if (mq && s_bingo_out && (int32_t)(now - s_next_bingo) >= 0) {
        send_bingo();
        s_next_bingo = now + PUSH_EVERY_MS;
    }
    if (s_busy && (int32_t)(now - s_busy_until) >= 0) {
        s_busy = s_busy_seq = 0;
        status("No answer from " CS_COMPANION_NAME);
        changed = true;
    }
    if (s_status[0] && (int32_t)(now - s_status_until) >= 0) {
        s_status[0] = '\0';
        changed = true;
    }
    if (s_overlay == CS_BOVL_AGAIN && (int32_t)(now - s_overlay_until) >= 0) {
        s_overlay = CS_BOVL_NONE;
        changed = true;
    }
    if (changed) refresh();
}

bool cs_bingo_done_today(void) { return s_done_today; }
