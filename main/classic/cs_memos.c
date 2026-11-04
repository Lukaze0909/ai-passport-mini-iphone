// main/classic/cs_memos.c -- Notes (闪念胶囊): one-line English memos.
//
// Threads: the MQTT task parses previews (cs_memos_on_command) and posts
// M_MEMO; everything else runs on the shell task. Flash: partition "memos"
// (data, subtype 0x41, 32 KB), one record per memo state (cs_logic.h). If the
// partition table predates it, memos live in 8 KB of RAM (lost on reboot).
#include "cs_memos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_call.h"
#include "cs_logic.h"
#include "cs_shell.h"
#include "cs_ui.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_memos";

#define PART_SUBTYPE 0x41
#define RAM_LOG_BYTES (2 * CS_STORE_SECTOR)
#define CLOCK_VALID_AFTER 1735689600    // 2025-01-01T00:00:00Z
#define PREVIEW_TIMEOUT_MS 20000
#define MAX_ITEMS CS_STORE_KEEP

static SemaphoreHandle_t s_mtx;         // the log and s_rx_*
static cs_store_t s_store;
static bool s_ok;
static const esp_partition_t *s_part;
static uint8_t *s_ram;
static uint16_t s_next_id;

// MQTT task -> shell: the latest preview.
static uint32_t s_rx_clip;
static char s_rx_text[CS_MEMO_TEXT_MAX + 1];

// Shell task.
typedef struct {
    uint16_t id;
    uint8_t rec;         // log index of its newest record
    uint8_t done;
} item_t;
static item_t s_items[MAX_ITEMS];
static int s_count;
static bool s_open, s_new;               // app open / on the NEW page
static int s_cursor = -1;                // LIST: -1 = "+", else item index
static uint8_t s_focus;                  // NEW: 0 mic, 1 send, 2 trash
static uint8_t s_state;                  // NEW: cs_memo_state_t (own states; REC / TAKE from cs_call)
static uint32_t s_pending_clip, s_wait_until;
static char s_preview[CS_MEMO_TEXT_MAX + 1];
static cs_msg_t s_tmp;                   // record scratch (~300 bytes)
static cs_notes_view_t s_nv;             // ~1.1 KB

// ---------------------------------------------------------------- storage
static bool part_read(void *c, uint32_t off, void *b, size_t n) {
    (void)c;
    return esp_partition_read(s_part, off, b, n) == ESP_OK;
}
static bool part_write(void *c, uint32_t off, const void *b, size_t n) {
    (void)c;
    return esp_partition_write(s_part, off, b, n) == ESP_OK;
}
static bool part_erase(void *c, uint32_t off) {
    (void)c;
    return esp_partition_erase_range(s_part, off, CS_STORE_SECTOR) == ESP_OK;
}
static bool ram_read(void *c, uint32_t off, void *b, size_t n) {
    (void)c;
    memcpy(b, s_ram + off, n);
    return true;
}
static bool ram_write(void *c, uint32_t off, const void *b, size_t n) {
    (void)c;
    memcpy(s_ram + off, b, n);
    return true;
}
static bool ram_erase(void *c, uint32_t off) {
    (void)c;
    memset(s_ram + off, 0xFF, CS_STORE_SECTOR);
    return true;
}

static uint32_t now_unix(void) {
    const time_t t = time(NULL);
    return t >= CLOCK_VALID_AFTER ? (uint32_t)t : 0;
}

void cs_memos_init(void) {
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, "memos");
    if (s_part && s_part->size >= 2 * CS_STORE_SECTOR) {
        s_store.read = part_read;
        s_store.write = part_write;
        s_store.erase = part_erase;
        s_store.size = (uint32_t)(s_part->size / CS_STORE_SECTOR * CS_STORE_SECTOR);
    } else {
        ESP_LOGW(TAG, "no \"memos\" partition (flash the new partition table): memos kept in RAM only");
        s_ram = malloc(RAM_LOG_BYTES);
        if (!s_ram) return;
        memset(s_ram, 0xFF, RAM_LOG_BYTES);
        s_store.read = ram_read;
        s_store.write = ram_write;
        s_store.erase = ram_erase;
        s_store.size = RAM_LOG_BYTES;
    }
    s_ok = cs_store_open(&s_store);
    for (int i = 0; s_ok && i < s_store.n; i++) {
        if (cs_store_get(&s_store, i, &s_tmp) && (uint16_t)(s_tmp.dur_s + 1) > s_next_id) {
            s_next_id = (uint16_t)(s_tmp.dur_s + 1);
        }
    }
    ESP_LOGI(TAG, "memo log: %s, %u records, next id %u", s_ok ? "open" : "FAILED", (unsigned)s_store.n,
             (unsigned)s_next_id);
}

static bool store_add(const cs_msg_t *m) {
    if (!s_ok) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool ok = cs_store_add(&s_store, m);
    xSemaphoreGive(s_mtx);
    if (!ok) ESP_LOGW(TAG, "memo not stored");
    return ok;
}

// Newest record per id, still visible, newest memo (highest id) first.
static void list_build(void) {
    s_count = 0;
    if (!s_ok) return;
    const uint32_t now = now_unix();
    uint16_t seen[MAX_ITEMS];
    int ns = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = s_store.n - 1; i >= 0; i--) {
        if (!cs_store_get(&s_store, i, &s_tmp)) continue;
        const uint16_t id = s_tmp.dur_s;
        bool dup = false;
        for (int k = 0; k < ns && !dup; k++) dup = seen[k] == id;
        if (dup) continue;
        seen[ns++] = id;
        if (!cs_memo_visible(s_tmp.out, s_tmp.ts, s_tmp.aux, now)) continue;
        s_items[s_count++] = (item_t){.id = id, .rec = (uint8_t)i, .done = (uint8_t)(s_tmp.out & CS_MEMO_DONE)};
    }
    xSemaphoreGive(s_mtx);
    // Newest first by id (ids grow with creation; a toggle keeps the id).
    for (int i = 1; i < s_count; i++) {
        const item_t x = s_items[i];
        int j = i;
        while (j > 0 && (int16_t)(s_items[j - 1].id - x.id) < 0) {
            s_items[j] = s_items[j - 1];
            j--;
        }
        s_items[j] = x;
    }
}

bool cs_memos_on_command(const char *json, size_t len) {
    static cs_memo_cmd_t cmd;   // MQTT task only
    if (!cs_memo_cmd_parse(json, len, &cmd)) return false;
    if (!s_mtx) return true;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_rx_clip = cmd.clip;
    snprintf(s_rx_text, sizeof s_rx_text, "%s", cmd.text);
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "memo preview for clip %lu: %u chars", (unsigned long)cmd.clip, (unsigned)strlen(cmd.text));
    cs_shell_post_memo();
    return true;
}

// ---------------------------------------------------------------- views
static void fmt_secs(char *b, size_t cap, uint32_t ms) {
    const unsigned s = (unsigned)(ms / 1000);
    snprintf(b, cap, "%u:%02u", s / 60, s % 60);
}

static void list_show(void) {
    if (s_cursor >= s_count) s_cursor = s_count - 1;
    memset(&s_nv, 0, sizeof s_nv);
    s_nv.count = s_count;
    s_nv.cursor = s_cursor;
    int first = 0;
    if (s_cursor >= CS_NOTES_ROWS) first = s_cursor - CS_NOTES_ROWS + 1;
    s_nv.first = first;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = first; i < s_count && s_nv.n < CS_NOTES_ROWS; i++) {
        if (!cs_store_get(&s_store, s_items[i].rec, &s_tmp)) s_tmp.text[0] = '\0';
        // Memo records hold at most CS_MEMO_TEXT_MAX characters; copy bounded anyway.
        char *dst = s_nv.rows[s_nv.n].text;
        size_t k = 0;
        while (k < CS_MEMO_TEXT_MAX && s_tmp.text[k]) {
            dst[k] = s_tmp.text[k];
            k++;
        }
        dst[k] = '\0';
        s_nv.rows[s_nv.n].done = s_items[i].done != 0;
        s_nv.n++;
    }
    xSemaphoreGive(s_mtx);
    cs_ui_notes_update(&s_nv);
}

// NEW state, with recording / take coming from the shared voice take.
static uint8_t new_state(void) {
    const cs_rec_state_t r = cs_call_rec_state();
    if (r == CS_REC_ON) return CS_MEMO_REC;
    if (r == CS_REC_STOPPED) return CS_MEMO_TAKE;
    return s_state;
}

static uint8_t enabled_for(uint8_t st) {
    switch (st) {
    case CS_MEMO_IDLE: return 1u << 0;
    case CS_MEMO_REC: return 1u << 0;
    case CS_MEMO_TAKE: return 1u << 1 | 1u << 2;
    case CS_MEMO_WAIT: return 1u << 2;
    case CS_MEMO_PREVIEW: return 1u << 1 | 1u << 2;
    default: return 1u << 0 | 1u << 2;          // FAIL: record again or drop
    }
}

static void new_show(void) {
    cs_memo_view_t v;
    memset(&v, 0, sizeof v);
    v.state = new_state();
    v.enabled = enabled_for(v.state);
    if (!(v.enabled & (1u << s_focus))) {       // focus the first usable button (Send before Trash)
        s_focus = (v.enabled & 2) ? 1 : (v.enabled & 1) ? 0 : 2;
    }
    v.focus = s_focus;
    v.text = s_preview;
    char t[12];
    fmt_secs(t, sizeof t, cs_call_rec_ms());
    if (v.state == CS_MEMO_REC) snprintf(v.status, sizeof v.status, "正在说… %s", t);
    else if (v.state == CS_MEMO_TAKE) snprintf(v.status, sizeof v.status, "说完了 · %s — 发送 还是 删除？", t);
    else if (v.state == CS_MEMO_IDLE) snprintf(v.status, sizeof v.status, "%s", cs_call_rec_note());
    cs_ui_memo_update(&v);
}

static void show(cs_screen_t want, bool lvgl_held) {
    if (!lvgl_held && !bsp_lvgl_lock(500)) return;
    if (cs_ui_current() != want) cs_ui_show(want);
    if (want == CS_SCR_NOTES) list_show();
    else new_show();
    if (!lvgl_held) bsp_lvgl_unlock();
}

static void refresh(void) {
    if (s_open) show(s_new ? CS_SCR_MEMO : CS_SCR_NOTES, false);
}

static void tap(void) { rg_app_sound(RG_SOUND_TAP); }

// ---------------------------------------------------------------- app
void cs_memos_enter(void) {
    s_open = true;
    s_new = false;
    list_build();
    s_cursor = s_count > 0 ? 0 : -1;
    show(CS_SCR_NOTES, true);
}

static void new_reset(void) {
    s_state = CS_MEMO_IDLE;
    s_pending_clip = 0;
    s_preview[0] = '\0';
    s_focus = 0;
}

void cs_memos_exit(void) {
    s_open = false;
    s_new = false;
    cs_call_rec_abandon();
    new_reset();
}

bool cs_memos_back(void) {
    if (!s_open || !s_new) return false;
    cs_call_rec_abandon();
    new_reset();
    s_new = false;
    list_build();
    refresh();
    return true;
}

static void toggle(int idx) {
    if (idx < 0 || idx >= s_count) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool ok = cs_store_get(&s_store, s_items[idx].rec, &s_tmp);
    xSemaphoreGive(s_mtx);
    if (!ok) return;
    s_tmp.out ^= CS_MEMO_DONE;
    s_tmp.aux = (s_tmp.out & CS_MEMO_DONE) ? now_unix() : 0;
    const uint16_t id = s_items[idx].id;
    if (!store_add(&s_tmp)) return;
    list_build();
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id == id) s_cursor = i;
    }
}

static void save_preview(void) {
    memset(&s_tmp, 0, sizeof s_tmp);
    s_tmp.dur_s = s_next_id++;
    s_tmp.ts = now_unix();
    snprintf(s_tmp.text, sizeof s_tmp.text, "%s", s_preview);
    if (store_add(&s_tmp)) ESP_LOGI(TAG, "memo %u saved", (unsigned)s_tmp.dur_s);
}

static void list_key(uint8_t btn) {
    if (btn == BSP_BTN_UP) {
        if (s_cursor > -1) {
            s_cursor--;
            tap();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_cursor + 1 < s_count) {
            s_cursor++;
            tap();
        }
    } else if (s_cursor < 0) {
        s_new = true;
        new_reset();
        rg_app_sound(RG_SOUND_CONFIRM);
    } else {
        toggle(s_cursor);
        tap();
    }
}

static void new_key(uint8_t btn) {
    const uint8_t st = new_state();
    const uint8_t en = enabled_for(st);
    if (btn != BSP_BTN_OK) {
        if (st == CS_MEMO_REC) return;            // no key sounds while recording
        for (int k = 0; k < 3; k++) {             // next usable button
            s_focus = (uint8_t)((s_focus + (btn == BSP_BTN_DOWN ? 1 : 2)) % 3);
            if (en & (1u << s_focus)) break;
        }
        tap();
        return;
    }
    switch (st) {
    case CS_MEMO_REC:
        cs_call_rec_stop();
        break;
    case CS_MEMO_TAKE:
        if (s_focus == 1) {
            if (cs_call_rec_finish(true)) {
                s_pending_clip = cs_call_rec_clip();
                s_wait_until = (uint32_t)(esp_timer_get_time() / 1000) + PREVIEW_TIMEOUT_MS;
                s_state = CS_MEMO_WAIT;
                ESP_LOGI(TAG, "waiting for the preview of clip %lu", (unsigned long)s_pending_clip);
            }
        } else {
            cs_call_rec_finish(false);
            new_reset();
        }
        break;
    case CS_MEMO_PREVIEW:
        if (s_focus == 1) {
            save_preview();
            new_reset();
            s_new = false;
            list_build();
            s_cursor = s_count > 0 ? 0 : -1;      // the new memo on top
            rg_app_sound(RG_SOUND_CONFIRM);
        } else {
            new_reset();
            tap();
        }
        break;
    case CS_MEMO_WAIT:
    case CS_MEMO_FAIL:
        if (s_focus == 0 && st == CS_MEMO_FAIL) {
            new_reset();
            cs_call_rec_start("memo");
        } else {
            new_reset();                          // drop it (a late preview is ignored)
            tap();
        }
        break;
    default:                                      // IDLE: OK on the mic records
        cs_call_rec_start("memo");
        break;
    }
}

void cs_memos_key(uint8_t btn) {
    if (!s_open) return;
    if (s_new) new_key(btn);
    else list_key(btn);
    refresh();
}

void cs_memos_changed(void) {
    if (s_open && s_new) refresh();
}

void cs_memos_preview(void) {
    uint32_t clip;
    char text[CS_MEMO_TEXT_MAX + 1];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    clip = s_rx_clip;
    snprintf(text, sizeof text, "%s", s_rx_text);
    xSemaphoreGive(s_mtx);
    if (!s_open || !s_new || s_state != CS_MEMO_WAIT || clip != s_pending_clip) {
        ESP_LOGI(TAG, "preview for clip %lu ignored", (unsigned long)clip);
        return;
    }
    snprintf(s_preview, sizeof s_preview, "%s", text);
    s_state = CS_MEMO_PREVIEW;
    s_focus = 1;
    refresh();
}

void cs_memos_poll(uint32_t now_ms) {
    if (!s_open || !s_new || s_state != CS_MEMO_WAIT) return;
    if ((int32_t)(now_ms - s_wait_until) < 0) return;
    ESP_LOGW(TAG, "no preview for clip %lu in %d s", (unsigned long)s_pending_clip, PREVIEW_TIMEOUT_MS / 1000);
    s_state = CS_MEMO_FAIL;
    s_pending_clip = 0;
    refresh();
}

bool cs_memos_busy(void) { return s_open && s_new && s_state == CS_MEMO_WAIT; }
