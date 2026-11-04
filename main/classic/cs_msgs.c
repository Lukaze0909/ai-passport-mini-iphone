// main/classic/cs_msgs.c -- 留言 (Messages): an SMS-style thread with the companion.
//
// Threads: the MQTT task stores incoming messages (cs_msgs_on_command) and
// posts M_MSG to the shell; everything else runs on the shell task. The log is
// shared under s_mtx. Flash: partition "msgs" (data, subtype 0x40, 64 KB). If
// the partition table on the device predates it, history is kept in 8 KB of
// RAM instead (lost on reboot) and a warning is logged.
#include "cs_msgs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_call.h"
#include "cs_logic.h"
#include "cs_net.h"
#include "cs_shell.h"
#include "cs_ui.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_msgs";

#define PART_SUBTYPE 0x40
#define RAM_LOG_BYTES (2 * CS_STORE_SECTOR)
#define CLOCK_VALID_AFTER 1735689600    // 2025-01-01T00:00:00Z
#define RECENT_IDS 4

static SemaphoreHandle_t s_mtx;
static cs_store_t s_store;
static bool s_ok;
static const esp_partition_t *s_part;
static uint8_t *s_ram;
static char s_recent[RECENT_IDS][24];     // de-duplicates QoS 1 re-deliveries (MQTT task)
static unsigned s_recent_pos;
static char s_last_text[CS_MSG_TEXT_MAX + 1];   // newest incoming, for the toast

// Shell task.
static volatile int s_unread;
static bool s_open;
static int s_scroll;                      // newest messages skipped (0 = bottom)
static uint8_t s_bar;                     // 0 录音, 1 发送, 2 删除
static cs_msg_t s_view[CS_MSGS_VIEW];

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

void cs_msgs_init(void) {
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, "msgs");
    if (s_part && s_part->size >= 2 * CS_STORE_SECTOR) {
        s_store.read = part_read;
        s_store.write = part_write;
        s_store.erase = part_erase;
        s_store.size = (uint32_t)(s_part->size / CS_STORE_SECTOR * CS_STORE_SECTOR);
    } else {
        ESP_LOGW(TAG, "no \"msgs\" partition (flash the new partition table): history kept in RAM only");
        s_ram = malloc(RAM_LOG_BYTES);
        if (!s_ram) return;
        memset(s_ram, 0xFF, RAM_LOG_BYTES);
        s_store.read = ram_read;
        s_store.write = ram_write;
        s_store.erase = ram_erase;
        s_store.size = RAM_LOG_BYTES;
    }
    s_ok = cs_store_open(&s_store);
    ESP_LOGI(TAG, "message log: %s, %u kept, next seq %lu", s_ok ? "open" : "FAILED", (unsigned)s_store.n,
             (unsigned long)s_store.seq);
}

static bool store_add(const cs_msg_t *m) {
    if (!s_ok) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const bool ok = cs_store_add(&s_store, m);
    xSemaphoreGive(s_mtx);
    if (!ok) ESP_LOGW(TAG, "message not stored");
    return ok;
}

bool cs_msgs_on_command(const char *json, size_t len) {
    static cs_msg_cmd_t cmd;   // MQTT task only (~300 bytes off its stack)
    if (!cs_msg_cmd_parse(json, len, &cmd)) return false;
    if (cmd.id[0]) {
        for (int i = 0; i < RECENT_IDS; i++) {
            if (strcmp(s_recent[i], cmd.id) == 0) {
                ESP_LOGI(TAG, "msg %s again: ignored", cmd.id);
                return true;
            }
        }
        snprintf(s_recent[s_recent_pos++ % RECENT_IDS], sizeof s_recent[0], "%s", cmd.id);
    }
    static cs_msg_t m;
    memset(&m, 0, sizeof m);
    m.ts = cmd.ts ? cmd.ts : now_unix();
    snprintf(m.text, sizeof m.text, "%s", cmd.text);
    if (!store_add(&m)) return true;
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        snprintf(s_last_text, sizeof s_last_text, "%s", m.text);
        xSemaphoreGive(s_mtx);
    }
    ESP_LOGI(TAG, "msg %s from the companion: %u chars", cmd.id, (unsigned)strlen(m.text));
    cs_shell_post_msg();
    return true;
}

// ---------------------------------------------------------------- view
static void fmt_secs(char *b, size_t cap, uint32_t ms) {
    const unsigned s = (unsigned)(ms / 1000);
    snprintf(b, cap, "%u:%02u", s / 60, s % 60);
}

static void view_show(bool lvgl_held) {
    cs_msgs_view_t v;
    memset(&v, 0, sizeof v);
    int k = 0;
    if (s_ok) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        const int n = s_store.n;
        if (s_scroll > n - 1) s_scroll = n > 0 ? n - 1 : 0;
        const int end = n - 1 - s_scroll;
        int start = end - CS_MSGS_VIEW + 1;
        if (start < 0) start = 0;
        for (int i = start; i <= end && k < CS_MSGS_VIEW; i++) {
            if (cs_store_get(&s_store, i, &s_view[k])) k++;
        }
        v.more_above = start > 0;
        v.gen = (s_store.seq << 8) ^ (uint32_t)s_scroll;
        xSemaphoreGive(s_mtx);
    }
    v.msgs = s_view;
    v.n = k;
    v.bar = s_bar;
    v.rec = (uint8_t)cs_call_rec_state();
    v.enabled = cs_net_status().mqtt;
    char t[12];
    fmt_secs(t, sizeof t, cs_call_rec_ms());
    if (v.rec == CS_REC_ON) snprintf(v.status, sizeof v.status, "正在说… %s", t);
    else if (v.rec == CS_REC_STOPPED) snprintf(v.status, sizeof v.status, "说完了 · %s — 发送 还是 删除？", t);
    else snprintf(v.status, sizeof v.status, "%s", cs_call_rec_note());
    if (lvgl_held) {
        cs_ui_msgs_update(&v);
    } else if (bsp_lvgl_lock(500)) {
        cs_ui_msgs_update(&v);
        bsp_lvgl_unlock();
    }
}

static void tap(void) { rg_app_sound(RG_SOUND_TAP); }

void cs_msgs_enter(void) {
    s_open = true;
    s_unread = 0;
    s_scroll = 0;
    s_bar = cs_call_rec_state() == CS_REC_STOPPED ? 1 : 0;
    view_show(true);
}

void cs_msgs_exit(void) {
    s_open = false;
    cs_call_rec_abandon();
}

void cs_msgs_key(uint8_t btn) {
    const cs_rec_state_t st = cs_call_rec_state();
    if (st == CS_REC_ON) {                       // OK stops; no key sounds while recording
        if (btn == BSP_BTN_OK) cs_call_rec_stop();
    } else if (st == CS_REC_STOPPED) {
        if (btn != BSP_BTN_OK) {
            s_bar = s_bar == 1 ? 2 : 1;
            tap();
        } else {
            const uint32_t ms = cs_call_rec_ms();
            const bool send = s_bar != 2;
            if (cs_call_rec_finish(send) && send) {
                static cs_msg_t m;
                memset(&m, 0, sizeof m);
                m.out = 1;
                m.dur_s = (uint16_t)((ms + 500) / 1000);
                m.ts = now_unix();
                store_add(&m);
            }
            s_bar = 0;
            s_scroll = 0;
        }
    } else if (btn == BSP_BTN_UP) {              // idle: UP older, DOWN newer, OK record
        if (s_ok && s_scroll + 1 < s_store.n) {
            s_scroll++;
            tap();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_scroll > 0) {
            s_scroll--;
            tap();
        }
    } else {
        s_bar = 0;
        if (cs_call_rec_start("msg")) s_scroll = 0;
    }
    view_show(false);
}

void cs_msgs_changed(void) {
    if (!s_open) return;
    const cs_rec_state_t st = cs_call_rec_state();
    if (st == CS_REC_STOPPED && s_bar == 0) s_bar = 1;   // the take is ready: Send focused
    if (st == CS_REC_IDLE) s_bar = 0;
    view_show(false);
}

bool cs_msgs_incoming(char *preview, size_t cap) {
    if (s_open) {
        s_scroll = 0;
        view_show(false);
        return true;
    }
    s_unread++;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(preview, cap, "%s: %s", CS_COMPANION_NAME, s_last_text);
    xSemaphoreGive(s_mtx);
    return false;
}

int cs_msgs_unread(void) { return s_unread; }
