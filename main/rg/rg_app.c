// main/rg/rg_app.c -- Release Guidance (释放引导): tasks, input, sound, NVS.
//
// Threads:
//   button callback (esp_timer task)  -> only timestamps and enqueues the event
//   rg_input task (prio 5)            -> key timing, page flow, UI under bsp_lvgl_lock()
//   rg_worker task (prio 6)           -> key sounds (bsp_audio_write) and NVS saves
//   LVGL task                          -> rendering and the sky animation timer
// Shell mode (classic shell): rg_app_init() runs the shared part once, then the
// shell forwards keys with rg_app_key() / rg_app_poll() from its own task and
// builds / frees the UI with rg_app_enter() / rg_app_exit(). The worker task
// also plays the shell's key sounds (rg_app_sound()).
// Nothing about the user's answers is stored. NVS keeps only: sound on/off,
// theme mode, last weather / sound bank (to vary them), and per-path entry counts
// for the future standby module.
#include "rg_app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rg_content.h"
#include "rg_flow.h"
#include "rg_input.h"
#include "rg_sound_data.h"
#include "rg_theme.h"
#include "rg_ui.h"
#include "sdkconfig.h"

#ifndef CONFIG_RG_SOUND_VOLUME
#define CONFIG_RG_SOUND_VOLUME 60
#endif

static const char *TAG = "rg_app";

_Static_assert(BSP_BTN_COUNT == RG_KEY_COUNT, "key table must match the BSP ladder");
_Static_assert((int)BSP_BTN_UP == (int)RG_KEY_UP && (int)BSP_BTN_DOWN == (int)RG_KEY_DOWN &&
               (int)BSP_BTN_OK == (int)RG_KEY_OK, "rg_key_t mirrors bsp_btn_t");
_Static_assert((int)BSP_BTN_PRESS == (int)RG_KEV_PRESS && (int)BSP_BTN_CLICK == (int)RG_KEV_CLICK &&
               (int)BSP_BTN_DOUBLE == (int)RG_KEV_DOUBLE && (int)BSP_BTN_LONG == (int)RG_KEV_LONG,
               "rg_key_event_t mirrors bsp_btn_ev_t");

#define NVS_NS "rg_guide"
#define KEY_QUEUE_DEPTH 8
#define JOB_QUEUE_DEPTH 8
#define AUDIO_CHUNK 480                 // 20 ms at 24 kHz
#define CLOCK_VALID_AFTER 1735689600    // 2025-01-01T00:00:00Z

typedef struct {
    uint8_t btn;
    uint8_t ev;
    uint32_t ms;
} key_msg_t;

typedef enum { JOB_SOUND = 1, JOB_SAVE_SETTINGS, JOB_SAVE_COUNT } job_type_t;

typedef struct {
    uint8_t type;
    uint8_t arg;        // sound kind / path
    uint32_t value;     // count value for JOB_SAVE_COUNT
} job_t;

static QueueHandle_t s_keyq, s_jobq;
static volatile bool s_ready;
static bool s_audio_ok, s_nvs_ok;
static rg_flow_t s_flow;
static rg_input_t s_input;
static rg_sky_t s_sky;
static struct {
    uint8_t muted, theme_mode, weather, bank;
    uint32_t count[2];
} s_set;

static rg_audio_acquire_fn s_acquire;
static rg_audio_release_fn s_release;
static bool s_core_ready, s_glyphs_checked;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- NVS
static void nvs_load(void) {
    s_set.weather = s_set.bank = 0xFF;
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // first boot: defaults
    nvs_get_u8(h, "muted", &s_set.muted);
    nvs_get_u8(h, "theme", &s_set.theme_mode);
    nvs_get_u8(h, "weather", &s_set.weather);
    nvs_get_u8(h, "bank", &s_set.bank);
    nvs_get_u32(h, "n_feel", &s_set.count[RG_PATH_FEEL]);
    nvs_get_u32(h, "n_goal", &s_set.count[RG_PATH_GOAL]);
    nvs_close(h);
    if (s_set.theme_mode >= RG_THEME_MODE_COUNT) s_set.theme_mode = RG_THEME_MODE_AUTO;
    s_set.muted = s_set.muted ? 1 : 0;
}

static void nvs_save(const job_t *job) {
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = ESP_OK;
    if (job->type == JOB_SAVE_SETTINGS) {
        e = nvs_set_u8(h, "muted", s_set.muted);
        if (e == ESP_OK) e = nvs_set_u8(h, "theme", s_set.theme_mode);
        if (e == ESP_OK) e = nvs_set_u8(h, "weather", s_set.weather);
        if (e == ESP_OK) e = nvs_set_u8(h, "bank", s_set.bank);
    } else {
        e = nvs_set_u32(h, job->arg == RG_PATH_GOAL ? "n_goal" : "n_feel", job->value);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(e));
}

// ---------------------------------------------------------------- worker
static void play_clip(uint8_t kind) {
    static const int16_t silence[AUDIO_CHUNK];
    const rg_sound_clip_t *clip = &RG_SOUND_CLIPS[s_set.bank % RG_SOUND_BANK_COUNT][kind];
    uint32_t off = 0;
    while (off < clip->samples) {
        const uint32_t n = clip->samples - off < AUDIO_CHUNK ? clip->samples - off : AUDIO_CHUNK;
        if (bsp_audio_write(clip->pcm + off, n * sizeof(int16_t)) != ESP_OK) return;
        off += n;
        job_t next;
        if (xQueuePeek(s_jobq, &next, 0) == pdTRUE && next.type == JOB_SOUND) return;  // newer key wins
    }
    (void)bsp_audio_write(silence, sizeof silence);   // flush DMA with zeros
}

// ---- unlock click (shell, leaving the lock screen), synthesized here.
// Two resonant noise transients 7 ms apart, "ka-da": a quieter bright one
// (~3.4 kHz) then a fuller lower one (~2.2 kHz). Each is LCG noise with a
// 0.35 ms envelope through a two-pole resonator ringing ~2 ms, plus a little
// of the raw noise for the edge; silent by ~20 ms, 30 ms in all. The peak is matched to the
// current bank's tap sound. Deterministic (fixed seed).
#define CLICK_MS 30
#define CLICK_CHUNK 240                 // 10 ms per write
typedef struct {
    float a1, a2, y1, y2, env, edec, amp;
    uint32_t at;
} click_voice_t;
typedef struct {
    click_voice_t v[2];
    uint32_t n, lcg;
} click_t;

static void click_init(click_t *c) {
    static const struct { float hz, ring_ms, at_ms, amp; } V[2] = {
        {3400.0f, 1.6f, 0.0f, 0.55f},
        {2200.0f, 2.4f, 7.0f, 1.0f},
    };
    const float fs = (float)RG_SOUND_RATE_HZ;
    c->n = 0;
    c->lcg = 0x2545F491u;
    for (int i = 0; i < 2; i++) {
        const float r = expf(-1000.0f / (V[i].ring_ms * fs));
        c->v[i] = (click_voice_t){
            .a1 = 2.0f * r * cosf(2.0f * (float)M_PI * V[i].hz / fs),
            .a2 = -r * r,
            .env = 1.0f,
            .edec = expf(-1000.0f / (0.35f * fs)),
            .amp = V[i].amp,
            .at = (uint32_t)(V[i].at_ms * fs / 1000.0f),
        };
    }
}

static float click_next(click_t *c) {
    c->lcg = c->lcg * 1664525u + 1013904223u;
    const float noise = (float)(int32_t)c->lcg * (1.0f / 2147483648.0f);
    float out = 0.0f;
    for (int i = 0; i < 2; i++) {
        click_voice_t *v = &c->v[i];
        if (c->n < v->at) continue;
        const float x = noise * v->env * v->amp;
        v->env *= v->edec;
        const float y = x + v->a1 * v->y1 + v->a2 * v->y2;
        v->y2 = v->y1;
        v->y1 = y;
        out += y + 0.25f * x;
    }
    c->n++;
    return out;
}

static void play_unlock(void) {
    static int16_t buf[CLICK_CHUNK];
    const uint32_t total = (uint32_t)RG_SOUND_RATE_HZ * CLICK_MS / 1000;
    click_t c;
    float peak = 1e-6f;
    click_init(&c);
    for (uint32_t i = 0; i < total; i++) {
        const float a = fabsf(click_next(&c));
        if (a > peak) peak = a;
    }
    const rg_sound_clip_t *tap = &RG_SOUND_CLIPS[s_set.bank % RG_SOUND_BANK_COUNT][RG_SOUND_TAP];
    int ref = 0;
    for (uint32_t i = 0; i < tap->samples; i++) {
        const int a = abs(tap->pcm[i]);
        if (a > ref) ref = a;
    }
    const float scale = (float)(ref ? ref : 12000) / peak;
    click_init(&c);
    for (uint32_t off = 0; off < total; off += CLICK_CHUNK) {
        const uint32_t n = total - off < CLICK_CHUNK ? total - off : CLICK_CHUNK;
        for (uint32_t i = 0; i < n; i++) {
            float s = click_next(&c) * scale;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            buf[i] = (int16_t)s;
        }
        if (bsp_audio_write(buf, n * sizeof buf[0]) != ESP_OK) return;
        job_t next;
        if (xQueuePeek(s_jobq, &next, 0) == pdTRUE && next.type == JOB_SOUND) return;   // newer key wins
    }
    memset(buf, 0, sizeof buf);
    (void)bsp_audio_write(buf, sizeof buf);   // flush DMA with zeros
}

static void worker_task(void *arg) {
    (void)arg;
    // With audio hooks (shell mode) the format is set per sound by the hook, which
    // also serialises the codec with the shell's chime / call audio.
    bool audio = s_audio_ok &&
                 (s_acquire || bsp_audio_set_format(RG_SOUND_RATE_HZ, 16, 1) == ESP_OK);
    if (audio) bsp_audio_set_volume(CONFIG_RG_SOUND_VOLUME);
    else ESP_LOGW(TAG, "audio unavailable: running silently");
    job_t job;
    for (;;) {
        if (xQueueReceive(s_jobq, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.type == JOB_SOUND) {
            if (!audio) continue;
            if (s_acquire && !s_acquire(RG_SOUND_RATE_HZ)) continue;   // codec busy: skip the tick
            if (job.arg == RG_APP_SOUND_UNLOCK) play_unlock();
            else play_clip(job.arg);
            if (s_release) s_release();
        } else {
            nvs_save(&job);
        }
    }
}

static void post(job_type_t type, uint8_t arg, uint32_t value) {
    const job_t job = {.type = (uint8_t)type, .arg = arg, .value = value};
    if (xQueueSend(s_jobq, &job, 0) != pdTRUE) ESP_LOGW(TAG, "job queue full (%d)", type);
}

static void sound(rg_sound_kind_t kind) {
    if (!s_set.muted) post(JOB_SOUND, (uint8_t)kind, 0);
}

// ---------------------------------------------------------------- theme
static bool clock_hour(int *hour) {
    const time_t t = time(NULL);
    if (t < CLOCK_VALID_AFTER) return false;   // no RTC / network time on this board
    struct tm tm;
    localtime_r(&t, &tm);
    *hour = tm.tm_hour;
    return true;
}

static bool compute_day(void) {
    int hour = 0;
    const bool valid = clock_hour(&hour);
    return rg_theme_is_day((rg_theme_mode_t)s_set.theme_mode, valid, hour);
}

// ---------------------------------------------------------------- input
static rg_key_t sample_held(void) {
    static const uint16_t windows[RG_KEY_COUNT][2] = BSP_BTN_MV_TABLE;
    int votes[RG_KEY_COUNT] = {0};
    for (int i = 0; i < 3; i++) {
        const rg_key_t k = rg_input_key_from_mv(bsp_button_read_mv(), windows);
        if (k != RG_KEY_NONE) votes[k]++;
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(3));
    }
    for (int k = 0; k < RG_KEY_COUNT; k++) {
        if (votes[k] >= 2) return (rg_key_t)k;
    }
    return RG_KEY_NONE;
}

// Returns true for RG_FX_LEAVE (only the shell acts on it; standalone ignores it).
// Returns the status word to flash.
static const char *toggle_mute(void) {
    s_set.muted ^= 1;
    sound(RG_SOUND_TAP);                       // audible only when turning sound on
    post(JOB_SAVE_SETTINGS, 0, 0);
    return RG_STATUS_WORDS[s_set.muted ? RG_STATUS_MUTED : RG_STATUS_SOUND];
}

static bool apply(rg_action_t action) {
    const uint32_t fx = rg_flow_handle(&s_flow, action);
    if (fx == RG_FX_NONE) return false;
    if (fx & RG_FX_LEAVE) return true;

    if (fx & RG_FX_SOUND_CONFIRM) sound(RG_SOUND_CONFIRM);
    else if (fx & RG_FX_SOUND_TAP) sound(RG_SOUND_TAP);

    const char *status = NULL;
    bool rebuild = false;
    if (fx & RG_FX_TOGGLE_MUTE) status = toggle_mute();
    if (fx & RG_FX_CYCLE_THEME) {
        s_set.theme_mode = (uint8_t)rg_theme_next_mode((rg_theme_mode_t)s_set.theme_mode);
        static const uint8_t WORD[RG_THEME_MODE_COUNT] = {
            RG_STATUS_AUTO, RG_STATUS_DUSK, RG_STATUS_MORNING};
        status = RG_STATUS_WORDS[WORD[s_set.theme_mode]];
        const bool day = compute_day();
        rebuild = day != s_sky.day;
        s_sky.day = day;
        sound(RG_SOUND_TAP);
        post(JOB_SAVE_SETTINGS, 0, 0);
    }
    if (fx & RG_FX_PATH_ENTERED) {
        const uint8_t path = s_flow.path;
        s_set.count[path]++;
        post(JOB_SAVE_COUNT, path, s_set.count[path]);
    }

    if (!bsp_lvgl_lock(500)) {
        ESP_LOGW(TAG, "LVGL lock timeout; page update dropped");
        return false;
    }
    if (rebuild) rg_ui_build(&s_sky, &s_flow);
    else if (fx & RG_FX_REDRAW_PAGE) rg_ui_show_page(&s_flow);
    else if (fx & RG_FX_REDRAW_FOCUS) rg_ui_show_focus(&s_flow);
    if (status) rg_ui_flash_status(status);
    bsp_lvgl_unlock();
    return false;
}

static void input_task(void *arg) {
    (void)arg;
    key_msg_t msg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        uint32_t deadline;
        if (rg_input_pending(&s_input, &deadline)) {
            const int32_t left = (int32_t)(deadline - now_ms());
            wait = left <= 0 ? 0 : pdMS_TO_TICKS(left) + 1;
        }
        if (xQueueReceive(s_keyq, &msg, wait) == pdTRUE) {
            rg_action_t actions[2];
            const int n = rg_input_event(&s_input, (rg_key_t)msg.btn, (rg_key_event_t)msg.ev,
                                         msg.ms, actions);
            for (int i = 0; i < n; i++) (void)apply(actions[i]);
            continue;
        }
        if (rg_input_pending(&s_input, &deadline) && (int32_t)(now_ms() - deadline) >= 0) {
            const rg_action_t a = rg_input_poll(&s_input, now_ms(), sample_held());
            if (a != RG_ACT_NONE) (void)apply(a);
        }
    }
}

// Button callbacks run in the shared esp_timer task: timestamp and enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_ready || !s_keyq) return;
    const key_msg_t msg = {.btn = (uint8_t)btn, .ev = (uint8_t)ev, .ms = now_ms()};
    (void)xQueueSend(s_keyq, &msg, 0);
}

// ---------------------------------------------------------------- start
// Shared by both modes: NVS, sound bank, audio, queues and the worker task.
static esp_err_t core_init(void) {
    if (s_core_ready) return ESP_OK;
    setenv("TZ", "CST-8", 1);   // Asia/Shanghai, used only if the clock is ever set
    tzset();

    esp_err_t e = nvs_flash_init();
    s_nvs_ok = e == ESP_OK;
    if (!s_nvs_ok) {
        // Do not erase: other data may live there. Run without persistence.
        ESP_LOGW(TAG, "NVS unavailable (%s): settings will not persist", esp_err_to_name(e));
    }
    nvs_load();

    // Vary the sound bank on every power-up, never repeating the last one.
    s_set.bank = rg_pick_other(esp_random(), RG_SOUND_BANK_COUNT, s_set.bank);

    s_audio_ok = bsp_audio_init() == ESP_OK;
    rg_flow_init(&s_flow);
    rg_input_init(&s_input);

    s_keyq = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(key_msg_t));
    s_jobq = xQueueCreate(JOB_QUEUE_DEPTH, sizeof(job_t));
    if (!s_keyq || !s_jobq) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker_task, "rg_worker", 4096, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_core_ready = true;
    return ESP_OK;
}

// New weather (never the last one) and day / night for this entry.
static void pick_sky(void) {
    const uint32_t r1 = esp_random(), r2 = esp_random();
    s_set.weather = rg_pick_other(r1, RG_WEATHER_COUNT, s_set.weather);
    s_sky.weather = (rg_weather_t)s_set.weather;
    s_sky.rainbow_left = (r1 >> 16) & 1;
    s_sky.seed = r1 ^ r2;
    s_sky.day = compute_day();
}

// Builds the UI for the current flow. Caller holds the LVGL lock.
static void build_ui_locked(void) {
    const int missing = s_glyphs_checked ? 0 : rg_ui_check_glyphs();
    s_glyphs_checked = true;
    rg_ui_build(&s_sky, &s_flow);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "lvgl pool after build: used %u%%, max_used %u, frag %u%%",
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.frag_pct);
    if (missing) ESP_LOGE(TAG, "%d glyphs missing from the fixed copy", missing);
}

esp_err_t rg_app_start(void) {
    esp_err_t e = core_init();
    if (e != ESP_OK) return e;
    pick_sky();
    post(JOB_SAVE_SETTINGS, 0, 0);    // remember this power-up's weather / bank

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    build_ui_locked();
    bsp_lvgl_unlock();

    if (xTaskCreate(input_task, "rg_input", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    e = bsp_button_init(on_key, NULL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ready: day=%d weather=%d bank=%d muted=%d theme=%d audio=%d nvs=%d",
             s_sky.day, s_sky.weather, s_set.bank, s_set.muted, s_set.theme_mode,
             s_audio_ok, s_nvs_ok);
    return ESP_OK;
}

// ---------------------------------------------------------------- shell mode
void rg_app_set_audio_hooks(rg_audio_acquire_fn acquire, rg_audio_release_fn release) {
    s_acquire = acquire;
    s_release = release;
}

esp_err_t rg_app_init(void) {
    esp_err_t e = core_init();
    if (e == ESP_OK) post(JOB_SAVE_SETTINGS, 0, 0);
    return e;
}

void rg_app_enter(void) {
    // Each entry is a fresh start: entry page, new weather, current day / night.
    pick_sky();
    rg_flow_init(&s_flow);
    rg_input_init(&s_input);
    post(JOB_SAVE_SETTINGS, 0, 0);
    build_ui_locked();
    ESP_LOGI(TAG, "entered: day=%d weather=%d muted=%d theme=%d",
             s_sky.day, s_sky.weather, s_set.muted, s_set.theme_mode);
}

void rg_app_exit(void) {
    rg_input_init(&s_input);      // drop any pending hold check
    rg_ui_teardown();
}

bool rg_app_key(uint8_t btn, uint8_t ev, uint32_t ms) {
    rg_action_t actions[2];
    const int n = rg_input_event(&s_input, (rg_key_t)btn, (rg_key_event_t)ev, ms, actions);
    for (int i = 0; i < n; i++) {
        if (apply(actions[i])) return true;
    }
    return false;
}

bool rg_app_deadline(uint32_t *ms) { return rg_input_pending(&s_input, ms); }

bool rg_app_poll(uint32_t now) {
    uint32_t deadline;
    if (!rg_input_pending(&s_input, &deadline) || (int32_t)(now - deadline) < 0) return false;
    const rg_action_t a = rg_input_poll(&s_input, now, sample_held());
    return a != RG_ACT_NONE && apply(a);
}

void rg_app_sound(int kind) {
    if (s_core_ready && ((kind >= 0 && kind < RG_SOUND_KIND_COUNT) || kind == RG_APP_SOUND_UNLOCK)) {
        sound((rg_sound_kind_t)kind);
    }
}

bool rg_app_muted(void) { return s_set.muted != 0; }

bool rg_app_in_session(void) { return !s_flow.in_entry; }

const char *rg_app_sound_word(void) {
    return RG_STATUS_WORDS[s_set.muted ? RG_STATUS_MUTED : RG_STATUS_SOUND];
}

bool rg_app_toggle_mute(bool in_app) {
    if (!s_core_ready) return s_set.muted != 0;
    const char *status = toggle_mute();
    if (in_app && bsp_lvgl_lock(500)) {
        rg_ui_flash_status(status);
        bsp_lvgl_unlock();
    }
    return s_set.muted != 0;
}
