// main/classic/cs_shell.c -- Classic system shell: lock, home, apps, routing.
//
// Threads:
//   button callback (esp_timer task) -> timestamps and enqueues only
//   pomodoro esp_timer callback      -> enqueues "done" only
//   cs_shell task (prio 5)           -> key filter, app logic, NVS, LVGL under bsp_lvgl_lock()
//   rg_worker (prio 6)               -> key sounds (release guidance sound bank) and rg NVS
//   cs_chime (prio 6)                -> alarm chime
//   cs_net (prio 4) + MQTT task      -> networking (cs_net.c)
//   LVGL task                        -> rendering; cs_ui's 250 ms timer reads cs_view_get()
// Release guidance runs inside this task's context as the 释放 app (rg_app_key /
// rg_app_poll), so there is exactly one consumer of key events.
#include "cs_shell.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "cs_astro.h"
#include "cs_audio.h"
#include "cs_bingo.h"
#include "cs_call.h"
#include "cs_logic.h"
#include "cs_memos.h"
#include "cs_mines.h"
#include "cs_msgs.h"
#include "cs_net.h"
#include "cs_ui.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_shell";

_Static_assert((int)BSP_BTN_UP == (int)CS_KEY_UP && (int)BSP_BTN_DOWN == (int)CS_KEY_DOWN &&
               (int)BSP_BTN_OK == (int)CS_KEY_OK, "cs_key_t mirrors bsp_btn_t");
_Static_assert((int)BSP_BTN_PRESS == (int)CS_KEV_PRESS && (int)BSP_BTN_CLICK == (int)CS_KEV_CLICK &&
               (int)BSP_BTN_DOUBLE == (int)CS_KEV_DOUBLE && (int)BSP_BTN_LONG == (int)CS_KEV_LONG &&
               (int)BSP_BTN_RELEASE == (int)CS_KEV_RELEASE, "cs_kev_t mirrors bsp_btn_ev_t");
_Static_assert(CS_LONG_MS == BSP_BTN_LONG_PRESS_MS, "shell long press = BSP long press");

// Staged features (spec priorities): P1 weather, P2 call.
#ifndef CS_HAVE_WEATHER
#define CS_HAVE_WEATHER 1
#endif
#ifndef CS_HAVE_CALL
#define CS_HAVE_CALL 1
#endif

// Home icons the cursor stops on: the apps (placeholders only show an alert).
#define HOME_LIVE                                                                   \
    (CS_HOME_ALL & ~CS_HOME_PLACEHOLDERS & ~(CS_HAVE_WEATHER ? 0u : 1u << CS_HOME_WEATHER) & \
     ~(CS_HAVE_CALL ? 0u : 1u << CS_HOME_CALL))

#define NVS_NS "classic"
#define QUEUE_DEPTH 12
#define CLOCK_VALID_AFTER 1735689600    // 2025-01-01T00:00:00Z
#define BATT_PERIOD_MS 30000
#define WX_JSON_MAX 1024

typedef enum { M_KEY = 1, M_POMO_DONE, M_WEATHER, M_CALL, M_MSG, M_MEMO, M_MINES, M_BINGO } msg_type_t;
typedef struct {
    uint8_t type, a, b;
    uint32_t ms;
} msg_t;

typedef enum { APP_LOCK = 0, APP_HOME, APP_RELEASE, APP_SETTINGS, APP_POMO, APP_WEATHER, APP_CALL, APP_MSGS, APP_NOTES, APP_MINES, APP_BINGO, APP_ASTRO } app_t;
typedef enum { POMO_PICK = 0, POMO_RUN, POMO_DONE } pomo_view_t;

static QueueHandle_t s_q;
static volatile bool s_ready;
static app_t s_app = APP_LOCK;
static cs_keys_t s_keys;
static int s_home_sel = CS_HOME_RELEASE;
static bool s_wifi_on;
static uint8_t s_preset = CS_POMO_DEFAULT_PRESET;
static pomo_view_t s_pomo_view;
static cs_pomo_t s_pomo;
static esp_timer_handle_t s_pomo_timer;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_batt = -1;
static bool s_batt_ok;
static SemaphoreHandle_t s_wx_mtx;
static cs_weather_t s_wx;
static volatile uint32_t s_wx_version;
static char s_wx_json[WX_JSON_MAX + 1];
static size_t s_wx_json_len;
// Standby. The power key is a hardware latch the firmware cannot read, so
// "lock now" is a long press of DOWN; otherwise the idle timer (Settings).
static cs_idle_t s_idle;
static uint8_t s_idle_choice = CS_IDLE_DEFAULT;   // index into CS_IDLE_S, NVS "idle"
static uint8_t s_screen = CS_SCREEN_ON;           // backlight state applied
static uint32_t s_standby_ms;
static int s_set_focus;                           // Settings row: 0 Wi-Fi, 1 auto-lock

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void post(msg_type_t type, uint8_t a, uint8_t b) {
    if (!s_q) return;
    const msg_t m = {.type = (uint8_t)type, .a = a, .b = b, .ms = now_ms()};
    if (xQueueSend(s_q, &m, 0) != pdTRUE) ESP_LOGW(TAG, "queue full (%d)", type);
}

static void tap(void) { rg_app_sound(RG_SOUND_TAP); }
static void confirm(void) { rg_app_sound(RG_SOUND_CONFIRM); }

// ---------------------------------------------------------------- NVS
static void nvs_set_u8_key(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, key, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // first boot: defaults
    uint8_t v;
    if (nvs_get_u8(h, "wifi", &v) == ESP_OK) s_wifi_on = v != 0;
    if (nvs_get_u8(h, "pomo", &v) == ESP_OK && v < CS_POMO_PRESET_COUNT) s_preset = v;
    if (nvs_get_u8(h, "idle", &v) == ESP_OK && v < CS_IDLE_CHOICES) s_idle_choice = v;
    size_t len = sizeof s_wx_json;
    if (nvs_get_str(h, "wx", s_wx_json, &len) == ESP_OK) {
        s_wx_json_len = strlen(s_wx_json);
        if (cs_weather_parse(s_wx_json, s_wx_json_len, &s_wx)) {
            s_wx_version++;
            ESP_LOGI(TAG, "weather cache loaded (%s %d°, updated %s)", s_wx.city, s_wx.temp, s_wx.updated);
        }
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- view (LVGL task)
void cs_view_get(cs_view_t *v) {
    memset(v, 0, sizeof *v);
    const time_t t = time(NULL);
    if (t >= CLOCK_VALID_AFTER) {
        struct tm tm;
        localtime_r(&t, &tm);
        v->time_valid = true;
        v->mon = tm.tm_mon + 1;
        v->mday = tm.tm_mday;
        v->wday = tm.tm_wday;
        v->hour = tm.tm_hour;
        v->min = tm.tm_min;
    }
    const cs_net_status_t ns = cs_net_status();
    if (!s_wifi_on) {
        v->link = CS_LINK_OFF;
    } else {
        v->link = ns.link == CS_LINK_OFF ? CS_LINK_CONNECTING : ns.link;
        v->mqtt = ns.mqtt;
    }
    v->batt = s_batt;
    cs_pomo_t p;
    taskENTER_CRITICAL(&s_mux);
    p = s_pomo;
    taskEXIT_CRITICAL(&s_mux);
    const int64_t now = esp_timer_get_time();
    v->pomo_running = p.running;
    v->pomo_total_s = p.total_s;
    v->pomo_long = p.total_s >= 3600;
    v->pomo_left_s = p.running ? cs_pomo_remaining_s(&p, now) : 0;
    v->pomo_permille = cs_pomo_progress_permille(&p, now);
    v->wx_version = s_wx_version;
    v->msgs_unread = cs_msgs_unread();
    v->bingo_done = cs_bingo_done_today();
    if (s_wx_mtx && xSemaphoreTake(s_wx_mtx, 0) == pdTRUE) {
        v->wx_valid = s_wx.valid;
        v->wx_temp = s_wx.temp;
        xSemaphoreGive(s_wx_mtx);
    }
}

bool cs_view_weather(cs_weather_t *out) {
    if (!s_wx_mtx || xSemaphoreTake(s_wx_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    *out = s_wx;
    xSemaphoreGive(s_wx_mtx);
    return out->valid;
}

// ---------------------------------------------------------------- network callbacks
static void on_weather(const char *data, size_t len) {
    cs_weather_t w;
    if (!cs_weather_parse(data, len, &w)) {
        ESP_LOGW(TAG, "weather JSON rejected (%u bytes)", (unsigned)len);
        return;
    }
    xSemaphoreTake(s_wx_mtx, portMAX_DELAY);
    s_wx = w;
    if (len <= WX_JSON_MAX) {
        memcpy(s_wx_json, data, len);
        s_wx_json[len] = '\0';
        s_wx_json_len = len;
    }
    xSemaphoreGive(s_wx_mtx);
    s_wx_version++;
    post(M_WEATHER, 0, 0);
    ESP_LOGI(TAG, "weather: %s %d° %s aqi %d (%s)", w.city, w.temp, w.icon, w.aqi, w.updated);
}

static void save_weather(void) {
    static char copy[WX_JSON_MAX + 1];
    xSemaphoreTake(s_wx_mtx, portMAX_DELAY);
    memcpy(copy, s_wx_json, s_wx_json_len + 1);
    xSemaphoreGive(s_wx_mtx);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "wx", copy) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static int battery_cb(void) { return s_batt; }

// ---------------------------------------------------------------- app switching
static cs_screen_t pomo_screen(void) {
    return s_pomo_view == POMO_RUN ? CS_SCR_POMO_RUN : s_pomo_view == POMO_DONE ? CS_SCR_POMO_DONE
                                                                                 : CS_SCR_POMO_PICK;
}

static void log_mem(const char *what) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "%s: lvgl used %u%% (max %u B, frag %u%%), heap free %u, largest %u", what,
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.frag_pct,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Caller holds the LVGL lock.
static void switch_app_locked(app_t next) {
    // An OK press still in progress belongs to the screen that is going away:
    // its release / hold must not act on the next one.
    cs_keys_spend(&s_keys);
    if (s_app == APP_RELEASE) rg_app_exit();
    if (s_app == APP_CALL && next != APP_CALL) cs_call_exit();
    if (s_app == APP_MSGS && next != APP_MSGS) cs_msgs_exit();
    if (s_app == APP_NOTES && next != APP_NOTES) cs_memos_exit();
    if (s_app == APP_MINES && next != APP_MINES) cs_mines_exit();
    if (s_app == APP_BINGO && next != APP_BINGO) cs_bingo_exit();
    if (s_app == APP_ASTRO && next != APP_ASTRO) cs_astro_exit();
    s_app = next;
    switch (next) {
    case APP_RELEASE:
        cs_ui_detach();
        rg_app_enter();
        break;
    case APP_LOCK: cs_ui_show(CS_SCR_LOCK); break;
    case APP_HOME:
        cs_ui_home_select(s_home_sel);
        cs_ui_show(CS_SCR_HOME);
        break;
    case APP_SETTINGS:
        s_set_focus = 0;
        cs_ui_settings(s_set_focus, s_idle_choice);
        cs_ui_show(CS_SCR_SETTINGS);
        break;
    case APP_POMO:
        cs_ui_pomo_pick(s_preset);
        cs_ui_show(pomo_screen());
        break;
    case APP_WEATHER: cs_ui_show(CS_SCR_WEATHER); break;
    case APP_CALL:
        cs_ui_show(CS_SCR_CALL);
        cs_call_enter();
        break;
    case APP_MSGS:
        cs_ui_show(CS_SCR_MSGS);
        cs_msgs_enter();
        break;
    case APP_NOTES:
        cs_memos_enter();   // builds its own screen
        break;
    case APP_MINES:
        cs_mines_enter();   // builds its own screen
        break;
    case APP_BINGO:
        cs_bingo_enter();   // builds its own screen
        break;
    case APP_ASTRO:
        cs_astro_enter();   // builds its own screen
        break;
    }
    log_mem("app switch");
}

static void switch_app(app_t next) {
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGW(TAG, "LVGL lock timeout; app switch dropped");
        return;
    }
    switch_app_locked(next);
    bsp_lvgl_unlock();
}

static void show_alert(void) {
    if (!bsp_lvgl_lock(500)) return;
    cs_ui_alert(true);
    bsp_lvgl_unlock();
}

static bool hide_alert_if_visible(void) {
    bool was = false;
    if (!bsp_lvgl_lock(500)) return false;
    was = cs_ui_alert_visible();
    if (was) cs_ui_alert(false);
    bsp_lvgl_unlock();
    return was;
}

// ---------------------------------------------------------------- standby
static void log_standby(const char *what, const char *why) {
    const int soc = s_batt_ok ? bsp_battery_soc() : -1;
    const int mv = s_batt_ok ? bsp_battery_mv() : -1;
    if (s_screen == CS_SCREEN_OFF) {
        ESP_LOGI(TAG, "standby enter (%s): battery %d%% %d mV", why, soc, mv);
    } else {
        ESP_LOGI(TAG, "standby exit (%s) after %lu s: battery %d%% %d mV", why,
                 (unsigned long)((now_ms() - s_standby_ms) / 1000), soc, mv);
    }
    (void)what;
}

// Applies a screen state: off locks first, then the backlight goes to 0
// (Wi-Fi / MQTT stay up; LVGL keeps its slow tick, nothing animates there).
static void screen_apply(uint8_t st, const char *why) {
    if (st == s_screen) return;
    const uint8_t was = s_screen;
    s_screen = st;
    if (st == CS_SCREEN_OFF) {
        if (s_app != APP_LOCK) switch_app(APP_LOCK);
        bsp_display_backlight(0);
        s_standby_ms = now_ms();
        log_standby("enter", why);
        return;
    }
    bsp_display_backlight(st == CS_SCREEN_DIM ? CS_DIM_PERCENT : 100);
    if (was == CS_SCREEN_OFF) log_standby("exit", why);
}

static void screen_wake(const char *why) {
    cs_idle_wake(&s_idle, now_ms());
    screen_apply(CS_SCREEN_ON, why);
}

// Never dim / lock during voice (recording, sending, playing), the alarm, or a
// release guidance session past its entry page (it is self-paced: no timers,
// and locking would lose the place).
static bool screen_hold(void) {
    return cs_call_busy() || cs_audio_chime_active() || cs_memos_busy() || cs_mines_hold(now_ms()) ||
           (s_app == APP_RELEASE && rg_app_in_session());
}

void cs_shell_wake(void) { screen_wake("voice"); }

void cs_shell_call_changed(void) {
    if (s_app == APP_MSGS) cs_msgs_changed();
    if (s_app == APP_NOTES) cs_memos_changed();
}

// A message from the companion was stored: the open thread shows it; elsewhere the
// Messages icon counts it, the screen wakes (from standby: the lock screen)
// and a toast shows the start of the text.
static void msg_arrived(void) {
    static char preview[64];
    if (cs_msgs_incoming(preview, sizeof preview)) return;
    screen_wake("message");
    tap();
    cs_shell_toast(preview);
    if (bsp_lvgl_lock(200)) {
        cs_ui_refresh();
        bsp_lvgl_unlock();
    }
}

// ---------------------------------------------------------------- pomodoro
static void pomo_timer_cb(void *arg) {
    (void)arg;
    post(M_POMO_DONE, 0, 0);
}

static void pomo_start(void) {
    const uint32_t secs = (uint32_t)CS_POMO_PRESET_MIN[s_preset] * 60;
    taskENTER_CRITICAL(&s_mux);
    cs_pomo_start(&s_pomo, esp_timer_get_time(), secs);
    taskEXIT_CRITICAL(&s_mux);
    esp_timer_stop(s_pomo_timer);
    esp_timer_start_once(s_pomo_timer, (uint64_t)secs * 1000000ULL);
    nvs_set_u8_key("pomo", s_preset);
    ESP_LOGI(TAG, "pomodoro started: %u min", (unsigned)CS_POMO_PRESET_MIN[s_preset]);
}

static void pomo_cancel(void) {
    esp_timer_stop(s_pomo_timer);
    taskENTER_CRITICAL(&s_mux);
    cs_pomo_stop(&s_pomo);
    taskEXIT_CRITICAL(&s_mux);
}

static void pomo_show(pomo_view_t v) {
    s_pomo_view = v;
    if (s_app != APP_POMO) return;
    if (!bsp_lvgl_lock(500)) return;
    cs_ui_pomo_pick(s_preset);
    cs_ui_show(pomo_screen());
    bsp_lvgl_unlock();
}

static void pomo_done(void) {
    taskENTER_CRITICAL(&s_mux);
    const bool was_running = s_pomo.running;
    cs_pomo_stop(&s_pomo);
    taskEXIT_CRITICAL(&s_mux);
    if (!was_running) return;   // cancelled just before the timer fired
    ESP_LOGI(TAG, "pomodoro done");
    screen_wake("alarm");
    s_pomo_view = POMO_DONE;
    // The alarm takes over the screen from any app (a recording in progress is stopped).
    if (s_app != APP_POMO) switch_app(APP_POMO);
    else pomo_show(POMO_DONE);
    cs_audio_chime_start();
}

static void pomo_key(uint8_t btn) {
    switch (s_pomo_view) {
    case POMO_PICK:
        if (btn == BSP_BTN_OK) {
            confirm();
            pomo_start();
            pomo_show(POMO_RUN);
            return;
        }
        {
            const int next = (int)s_preset + (btn == BSP_BTN_DOWN ? 1 : -1);
            if (next < 0 || next >= CS_POMO_PRESET_COUNT) return;   // wheel stops at the ends
            s_preset = (uint8_t)next;
            tap();
            if (bsp_lvgl_lock(500)) {
                cs_ui_pomo_pick(s_preset);
                bsp_lvgl_unlock();
            }
        }
        return;
    case POMO_RUN:
        if (btn == BSP_BTN_OK) {
            tap();
            pomo_cancel();
            pomo_show(POMO_PICK);
        }
        return;
    case POMO_DONE:
        cs_audio_chime_stop();
        pomo_show(POMO_PICK);
        return;
    }
}

// ---------------------------------------------------------------- apps
static void open_item(int item) {
    switch (item) {
    case CS_HOME_RELEASE: confirm(); switch_app(APP_RELEASE); return;
    case CS_HOME_MESSAGES: confirm(); switch_app(APP_MSGS); return;
    case CS_HOME_NOTES: confirm(); switch_app(APP_NOTES); return;
    case CS_HOME_MINES: confirm(); switch_app(APP_MINES); return;
    case CS_HOME_CALENDAR: confirm(); switch_app(APP_BINGO); return;   // the calendar icon is Bingo
    case CS_HOME_ASTRO: confirm(); switch_app(APP_ASTRO); return;      // the old Photos slot
    case CS_HOME_CLOCK: confirm(); switch_app(APP_POMO); return;
    case CS_HOME_SETTINGS: confirm(); switch_app(APP_SETTINGS); return;
#if CS_HAVE_WEATHER
    case CS_HOME_WEATHER: confirm(); switch_app(APP_WEATHER); return;
#endif
#if CS_HAVE_CALL
    case CS_HOME_CALL: confirm(); switch_app(APP_CALL); return;
#endif
    default:
        tap();
        show_alert();
        return;
    }
}

static void home_key(uint8_t btn) {
    if (btn == BSP_BTN_OK) {
        open_item(s_home_sel);
        return;
    }
    s_home_sel = cs_home_step(s_home_sel, btn == BSP_BTN_DOWN ? +1 : -1, HOME_LIVE);
    tap();
    if (bsp_lvgl_lock(500)) {
        cs_ui_home_select(s_home_sel);
        bsp_lvgl_unlock();
    }
}

static void settings_show(void) {
    if (bsp_lvgl_lock(500)) {
        cs_ui_settings(s_set_focus, s_idle_choice);
        bsp_lvgl_unlock();
    }
}

static void settings_key(uint8_t btn) {
    if (btn != BSP_BTN_OK) {               // UP / DOWN: the other row
        s_set_focus ^= 1;
        tap();
        settings_show();
        return;
    }
    if (s_set_focus == 1) {                // auto-lock: next choice
        s_idle_choice = (uint8_t)((s_idle_choice + 1) % CS_IDLE_CHOICES);
        s_idle.timeout_ms = (uint32_t)CS_IDLE_S[s_idle_choice] * 1000;
        cs_idle_wake(&s_idle, now_ms());
        tap();
        nvs_set_u8_key("idle", s_idle_choice);
        ESP_LOGI(TAG, "auto-lock after %u s (0 = never)", (unsigned)CS_IDLE_S[s_idle_choice]);
        settings_show();
        return;
    }
    s_wifi_on = !s_wifi_on;
    tap();
    nvs_set_u8_key("wifi", s_wifi_on);
    cs_net_enable(s_wifi_on);
    ESP_LOGI(TAG, "wifi switch %s", s_wifi_on ? "on" : "off");
    if (bsp_lvgl_lock(500)) {
        cs_ui_refresh();
        bsp_lvgl_unlock();
    }
}

static void go_home(void) {
    if (s_app == APP_HOME) return;
    confirm();
    switch_app(APP_HOME);
}

// Long press of UP on any screen (lock aside: its press already left for home)
// toggles key sounds, the one setting release guidance also uses; the alarm and
// the companion's voice play regardless. The toast shows release guidance's own status
// word in the shell's 14 px font (all of GB2312); inside release guidance its
// status line shows it instead.
static void toggle_sound(void) {
    const bool muted = rg_app_toggle_mute(s_app == APP_RELEASE);
    ESP_LOGI(TAG, "key sounds %s", muted ? "off" : "on");
    if (s_app != APP_RELEASE) cs_shell_toast(rg_app_sound_word());
}

// Long press of OK = back one level (every app sits directly under home).
// OK and UP act on release so a long press never fires the short action first;
// DOWN acts on press. Lock acts on press; home acts on press for OK and DOWN.
static void handle_key(uint8_t btn, uint8_t ev, uint32_t ms) {
    const cs_keys_verdict_t v = cs_keys_feed(&s_keys, btn, ev, ms);
    if (v == CS_KEYS_DROP) return;
    if (v == CS_KEYS_PRESS && s_screen != CS_SCREEN_ON) {
        // A dark or dimmed screen: this press only wakes it (off -> the lock
        // screen, unlock with the next press) and does nothing else.
        screen_wake("key");
        cs_keys_spend(&s_keys);
        return;
    }
    cs_idle_wake(&s_idle, now_ms());
    if (v == CS_KEYS_PRESS) {
        // Any key silences the alarm or closes the "coming soon" alert; that
        // press does nothing else (its release / hold are dropped).
        if (cs_audio_chime_active()) {
            cs_audio_chime_stop();
            cs_keys_spend(&s_keys);
            if (s_app == APP_POMO && s_pomo_view == POMO_DONE) pomo_show(POMO_PICK);
            return;
        }
        if (hide_alert_if_visible()) {
            cs_keys_spend(&s_keys);
            return;
        }
    }
    if (btn == BSP_BTN_UP && v == CS_KEYS_LONG) {
        toggle_sound();   // release guidance never sees this LONG (no second toggle there)
        return;
    }
    if (btn == BSP_BTN_DOWN && v == CS_KEYS_LONG && s_app != APP_RELEASE && s_app != APP_LOCK) {
        cs_idle_off(&s_idle);   // lock + screen off now (release guidance keeps DOWN hold = theme)
        screen_apply(CS_SCREEN_OFF, "key");
        return;
    }
    if (s_app == APP_RELEASE) {
        // Release guidance times its own keys from the raw events (OK hold
        // ~1 s = back to the entry page); the same hold on the entry page leaves.
        if (rg_app_key(btn, ev, ms)) go_home();
        return;
    }
    if (s_app == APP_LOCK) {
        if (v != CS_KEYS_PRESS) return;
        rg_app_sound(RG_APP_SOUND_UNLOCK);   // synthesized "ka-da", follows the key-sound mute
        switch_app(APP_HOME);     // spends the press: its release / hold stay on the lock screen
        return;
    }
    if (btn == BSP_BTN_OK && v == CS_KEYS_LONG) {
        if (s_app == APP_HOME) return;
        if (s_app == APP_NOTES && cs_memos_back()) return;   // NEW memo -> list, one level
        if (s_app == APP_MINES && cs_mines_back()) return;   // end note -> the boards
        if (s_app == APP_BINGO && cs_bingo_back()) return;   // note / swap -> the card
        if (s_app == APP_ASTRO && cs_astro_back()) return;   // History -> the dice
        if (s_app == APP_POMO && s_pomo_view == POMO_DONE) s_pomo_view = POMO_PICK;
        go_home();
        return;
    }
    // DOWN acts on release too now that its long press locks the screen.
    const bool on_press = btn == BSP_BTN_OK && s_app == APP_HOME;
    if (v != (on_press ? CS_KEYS_PRESS : CS_KEYS_TAP)) return;
    switch (s_app) {
    case APP_HOME:
        home_key(btn);
        if (btn == BSP_BTN_OK) cs_keys_spend(&s_keys);   // its release / hold must not reach the app it opened
        break;
    case APP_SETTINGS: settings_key(btn); break;
    case APP_POMO: pomo_key(btn); break;
    case APP_CALL:
        if (cs_call_key(btn)) go_home();
        break;
    case APP_MSGS: cs_msgs_key(btn); break;
    case APP_NOTES: cs_memos_key(btn); break;
    case APP_MINES: cs_mines_key(btn); break;
    case APP_BINGO: cs_bingo_key(btn); break;
    case APP_ASTRO: cs_astro_key(btn); break;
    default: break;   // weather: only the long press back
    }
}

// ---------------------------------------------------------------- task
static void handle(const msg_t *m) {
    switch (m->type) {
    case M_KEY: handle_key(m->a, m->b, m->ms); break;
    case M_POMO_DONE: pomo_done(); break;
    case M_WEATHER: save_weather(); break;
    case M_CALL: cs_call_event(m->a); break;
    case M_MSG: msg_arrived(); break;
    case M_MEMO: cs_memos_preview(); break;
    case M_MINES: cs_mines_event(); break;
    case M_BINGO: cs_bingo_event(); break;
    default: break;
    }
}

static void shell_task(void *arg) {
    (void)arg;
    uint32_t next_batt = 0, next_call_poll = 0;
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(1000);
        uint32_t deadline;
        if (s_app == APP_RELEASE && rg_app_deadline(&deadline)) {
            const int32_t left = (int32_t)(deadline - now_ms());
            wait = left <= 0 ? 0 : pdMS_TO_TICKS(left) + 1;
        }
        if (s_app == APP_ASTRO && cs_astro_poll_ms()) {   // dice rolling: animation frames
            const TickType_t t = pdMS_TO_TICKS(cs_astro_poll_ms());
            if (t < wait) wait = t ? t : 1;
        }
        msg_t m;
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) handle(&m);
        if (s_app == APP_RELEASE && rg_app_poll(now_ms())) go_home();
        const uint32_t now = now_ms();
        if (s_app == APP_CALL && (int32_t)(now - next_call_poll) >= 0) {
            next_call_poll = now + 1000;
            cs_call_event(CS_CALL_EV_POLL);   // broker up/down shows in the status line
        }
        if (s_batt_ok && (int32_t)(now - next_batt) >= 0) {
            next_batt = now + BATT_PERIOD_MS;
            s_batt = bsp_battery_soc();
        }
        if (s_app == APP_NOTES) cs_memos_poll(now_ms());
        cs_mines_poll(now_ms());   // also away from the app: the answer timeout
        cs_bingo_poll(now_ms());   // sync ticks, a new card day
        if (s_app == APP_ASTRO) cs_astro_poll(now_ms());
        screen_apply(cs_idle_tick(&s_idle, now_ms(), screen_hold()), "idle");
    }
}

// Button callbacks run in the shared esp_timer task: timestamp and enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_ready || !s_q) return;
    const msg_t m = {.type = M_KEY, .a = (uint8_t)btn, .b = (uint8_t)ev, .ms = now_ms()};
    (void)xQueueSend(s_q, &m, 0);
}

void cs_shell_post_call(uint8_t event) { post(M_CALL, event, 0); }
void cs_shell_post_msg(void) { post(M_MSG, 0, 0); }
void cs_shell_post_memo(void) { post(M_MEMO, 0, 0); }
void cs_shell_post_mines(void) { post(M_MINES, 0, 0); }
void cs_shell_post_bingo(void) { post(M_BINGO, 0, 0); }

// MQTT task: "msg" -> Messages, "memo" -> Notes, "mine_*" -> Mines, "bingo_card" -> Bingo,
// the rest (play) -> call module.
static void on_command(const char *data, size_t len) {
    if (cs_msgs_on_command(data, len) || cs_memos_on_command(data, len) || cs_mines_on_command(data, len) ||
        cs_bingo_on_command(data, len)) {
        return;
    }
    cs_call_on_command(data, len);
}

void cs_shell_toast(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    cs_ui_toast(text, 2500);
    bsp_lvgl_unlock();
}

bool cs_shell_in_call_app(void) { return s_app == APP_CALL; }

esp_err_t cs_shell_start(void) {
    cs_net_log_heap("shell start");
#if CONFIG_BT_ENABLED
    // The shell never starts BLE (only the BSP demo menu does): hand the BLE
    // controller's reserved RAM to the heap. Irreversible until reboot.
    const esp_err_t bt = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (bt != ESP_OK) ESP_LOGW(TAG, "BLE controller memory release: %s", esp_err_to_name(bt));
    cs_net_log_heap("after BLE controller memory release");
#endif
    // Release guidance core first: it initialises NVS, the codec and the sound worker.
    rg_app_set_audio_hooks(cs_audio_hook_acquire, cs_audio_hook_release);
    esp_err_t e = cs_audio_init(bsp_audio_init() == ESP_OK);
    if (e != ESP_OK) return e;
    e = rg_app_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "release guidance init: %s", esp_err_to_name(e));

    s_wx_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(QUEUE_DEPTH, sizeof(msg_t));
    if (!s_wx_mtx || !s_q) return ESP_ERR_NO_MEM;
    settings_load();
    cs_keys_init(&s_keys);
    cs_idle_init(&s_idle, (uint32_t)CS_IDLE_S[s_idle_choice] * 1000, now_ms());
    s_batt_ok = bsp_battery_init() == ESP_OK;
    if (s_batt_ok) s_batt = bsp_battery_soc();

    const esp_timer_create_args_t ta = {.callback = pomo_timer_cb, .name = "cs_pomo"};
    e = esp_timer_create(&ta, &s_pomo_timer);
    if (e != ESP_OK) return e;

    cs_msgs_init();
    cs_memos_init();
    cs_mines_init();
    cs_bingo_init();
    cs_astro_init();
    cs_call_init();
    e = cs_net_init(on_weather, on_command, cs_call_on_playchunk, battery_cb);
    if (e != ESP_OK) ESP_LOGE(TAG, "network init: %s", esp_err_to_name(e));

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    cs_ui_init();
    switch_app_locked(APP_LOCK);
    bsp_lvgl_unlock();

    if (xTaskCreate(shell_task, "cs_shell", 6144, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    e = bsp_button_init(on_key, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    if (s_wifi_on) cs_net_enable(true);
    ESP_LOGI(TAG, "ready: wifi=%d preset=%u batt=%d creds=%d", s_wifi_on, (unsigned)s_preset, s_batt,
             cs_net_have_credentials());
    cs_net_log_heap("shell ready");
    return ESP_OK;
}
