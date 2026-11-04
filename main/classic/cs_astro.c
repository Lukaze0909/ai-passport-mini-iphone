// main/classic/cs_astro.c -- Astro dice (占星骰子): planet, sign and house.
//
// Everything runs on the shell task. A roll is decided the moment OK is
// released: random bytes from esp_random() feed cs_astro_roll() (bytes >= 240
// rejected, the rest % 12: no modulo bias, host tested). The animation only
// shows faces on the way there (cs_astro_anim / cs_astro_fx) and stops on the
// real result.
//
// Randomness: esp_random() is the true hardware RNG while the radio runs
// (Wi-Fi on). With Wi-Fi off ESP-IDF calls its output pseudo-random; the
// documented fix, bootloader_random_enable(), takes over the SAR ADC, which
// this board reads for its keys (and the battery), so it is not used. Each word
// is instead XORed with a hash of the key-release time in CPU cycles (human
// timing jitter): with Wi-Fi on that changes nothing (a uniform word XOR an
// independent value stays uniform), with it off it adds real entropy. The log
// line of every roll says which case it was.
//
// History: the last ten rolls (time + faces, 8 bytes each) in NVS
// "classic/astro" (one blob, written after each roll).
//
// Event (passport/<user>/event, QoS 1, queued), only when she presses the hat:
//   {"type":"astro_roll","planet":"Venus","sign":"Scorpio","house":7}
#include "cs_astro.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_logic.h"
#include "cs_net.h"
#include "cs_ui.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "rg_app.h"
#include "rg_sound_data.h"

static const char *TAG = "cs_astro";

#define NVS_NS "classic"
#define CLOCK_VALID_AFTER 1735689600u   // 2025-01-01
#define STATUS_MS 4000u

typedef struct {
    uint8_t n, pad[3];
    cs_astro_log_t log[CS_ASTRO_LOG];
} astro_blob_t;                         // 84 bytes in NVS

static bool s_open, s_hist, s_rolling, s_sent, s_have;
static uint8_t s_focus;                               // 0 Roll, 1 the hat, 2 the clock
static uint8_t s_faces[CS_ASTRO_DICE], s_shown[CS_ASTRO_DICE], s_settled, s_seed;
static uint32_t s_t0, s_anim, s_roll_time, s_status_until, s_rolls;
static char s_status[40];
static astro_blob_t s_blob;
static cs_astro_view_t s_v;                           // ~56 bytes
static cs_astro_hist_view_t s_hv;                     // ~241 bytes

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void tap(void) { rg_app_sound(RG_SOUND_TAP); }
static void confirm(void) { rg_app_sound(RG_SOUND_CONFIRM); }

static uint32_t wall_now(void) {
    const time_t t = time(NULL);
    return t >= (time_t)CLOCK_VALID_AFTER ? (uint32_t)t : 0;
}

static bool radio_on(void) {
    wifi_mode_t m;
    return esp_wifi_get_mode(&m) == ESP_OK && m != WIFI_MODE_NULL;
}

static uint32_t mix32(uint32_t x) {   // a 32-bit finaliser (lowbias32)
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static void random_bytes(uint8_t *b, size_t n, uint32_t jitter) {
    for (size_t i = 0; i < n; i += 4) {
        const uint32_t w = esp_random() ^ mix32(jitter + (uint32_t)i * 0x9E3779B9u);
        const size_t k = n - i < 4 ? n - i : 4;
        memcpy(b + i, &w, k);
    }
}

static void status(const char *t) {
    snprintf(s_status, sizeof s_status, "%s", t);
    s_status_until = now_ms() + STATUS_MS;
}

// ---------------------------------------------------------------- NVS
void cs_astro_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_blob;
    if (nvs_get_blob(h, "astro", &s_blob, &len) != ESP_OK || len != sizeof s_blob) memset(&s_blob, 0, sizeof s_blob);
    nvs_close(h);
    s_blob.n = cs_astro_log_clean(s_blob.log, s_blob.n);
    ESP_LOGI(TAG, "%u rolls in the history", s_blob.n);
}

static void save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "astro", &s_blob, sizeof s_blob) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- view
static void build_view(cs_astro_view_t *v) {
    memset(v, 0, sizeof *v);
    v->have = s_have;
    v->anim_ms = s_rolling ? s_anim : CS_ASTRO_ANIM_MS;
    memcpy(v->faces, s_rolling ? s_shown : s_faces, sizeof v->faces);
    v->focus = s_focus;
    v->sent = s_have && !s_rolling && s_sent;
    if (s_status[0] && (int32_t)(s_status_until - now_ms()) > 0) {
        snprintf(v->caption, sizeof v->caption, "%s", s_status);
    } else if (s_rolling) {
        v->caption[0] = '\0';
    } else if (s_focus == 1) {
        snprintf(v->caption, sizeof v->caption, s_have ? "Ask " CS_COMPANION_NAME " to read it"
                                                       : "Roll first, then ask " CS_COMPANION_NAME);
    } else if (s_focus == 2) {
        snprintf(v->caption, sizeof v->caption, "History");
    } else if (!s_have) {
        snprintf(v->caption, sizeof v->caption, "Press OK to roll");
    }
}

static void build_hist(cs_astro_hist_view_t *v) {
    memset(v, 0, sizeof *v);
    const uint32_t now = wall_now();
    v->n = s_blob.n;
    for (int i = 0; i < s_blob.n; i++) {
        cs_astro_when(s_blob.log[i].t, now, v->rows[i].when, sizeof v->rows[i].when);
        memcpy(v->rows[i].f, s_blob.log[i].f, CS_ASTRO_DICE);
    }
}

static void refresh(void) {
    if (!s_open) return;
    if (!bsp_lvgl_lock(500)) return;
    if (s_hist) {
        build_hist(&s_hv);
        if (cs_ui_current() != CS_SCR_ASTRO_HIST) cs_ui_show(CS_SCR_ASTRO_HIST);
        cs_ui_astro_hist_update(&s_hv);
    } else {
        build_view(&s_v);
        if (cs_ui_current() != CS_SCR_ASTRO) cs_ui_show(CS_SCR_ASTRO);
        cs_ui_astro_update(&s_v);
    }
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- actions
static void roll(void) {
    const uint32_t jitter = esp_cpu_get_cycle_count() ^ (uint32_t)esp_timer_get_time();
    uint8_t b[16];
    random_bytes(b, sizeof b, jitter);
    while (!cs_astro_roll(b, sizeof b, s_faces)) {   // > 13 of 16 bytes rejected: about 1 in 10^14
        random_bytes(b, sizeof b, jitter ^ 0xA5A5A5A5u);
    }
    s_seed = b[15];
    s_rolling = s_have = true;
    s_sent = false;
    s_settled = 0;
    s_t0 = now_ms();
    s_anim = 0;
    s_roll_time = wall_now();
    s_rolls++;
    s_status[0] = '\0';
    cs_astro_anim(0, s_faces, s_seed, s_shown);
    ESP_LOGI(TAG, "roll %lu: %s / %s / house %d (rng: %s)", (unsigned long)s_rolls,
             CS_ASTRO_PLANETS[s_faces[0]], CS_ASTRO_SIGNS[s_faces[1]], s_faces[2] + 1,
             radio_on() ? "hardware, radio on" : "hardware + key timing, radio off");
}

static void settle(void) {
    s_rolling = false;
    s_settled = 7;
    cs_astro_log_push(s_blob.log, &s_blob.n, s_roll_time, s_faces);
    save();
}

static void ask_companion(void) {
    if (s_rolling) return;                           // a moment: the dice are still on the water
    if (!s_have) {
        status("Roll the dice first");
        tap();
        return;
    }
    if (s_sent) {
        status("Already with " CS_COMPANION_NAME);
        tap();
        return;
    }
    if (!cs_net_status().mqtt) {
        status(CS_COMPANION_NAME " needs Wi-Fi (Settings)");
        tap();
        return;
    }
    char j[112];
    if (cs_astro_event_json(s_faces, j, sizeof j) < 0) return;
    const bool ok = cs_net_publish_event(j);
    ESP_LOGI(TAG, "%s %s", ok ? "sent" : "NOT sent", j);
    if (!ok) {
        status("Could not reach " CS_COMPANION_NAME);
        tap();
        return;
    }
    s_sent = true;
    status("Sent to " CS_COMPANION_NAME " ✓");
    confirm();
}

void cs_astro_key(uint8_t btn) {
    if (!s_open) return;
    if (s_hist) {                                    // History: OK goes back
        if (btn == BSP_BTN_OK) {
            s_hist = false;
            tap();
            refresh();
        }
        return;
    }
    if (btn != BSP_BTN_OK) {                         // Roll -> hat -> clock (DOWN), wrapping
        s_focus = (uint8_t)((s_focus + (btn == BSP_BTN_DOWN ? 1 : 2)) % 3);
        s_status[0] = '\0';
        tap();
    } else if (s_focus == 1) {
        ask_companion();
    } else if (s_focus == 2) {
        if (s_rolling) settle();
        s_hist = true;
        tap();
    } else if (!s_rolling) {
        roll();
        tap();
    }
    refresh();
}

bool cs_astro_back(void) {
    if (!s_open || !s_hist) return false;
    s_hist = false;
    refresh();
    return true;
}

void cs_astro_poll(uint32_t now) {
    if (!s_open) return;
    bool changed = false;
    if (s_rolling) {
        const uint32_t el = now - s_t0;
        uint8_t shown[CS_ASTRO_DICE];
        const uint8_t done = cs_astro_anim(el, s_faces, s_seed, shown);
        if (done != s_settled) {
            tap();                                   // a result surfaces: a soft note
            s_settled = done;
        }
        memcpy(s_shown, shown, sizeof shown);
        s_anim = el;
        changed = true;
        if (el >= CS_ASTRO_ANIM_MS) settle();
    }
    if (s_status[0] && (int32_t)(now - s_status_until) >= 0) {
        s_status[0] = '\0';
        changed = true;
    }
    if (changed && !s_hist) refresh();
}

uint32_t cs_astro_poll_ms(void) {
    if (!s_open) return 0;
    if (s_rolling) return 30u;
    return s_status[0] ? 250u : 0u;
}

void cs_astro_enter(void) {
    s_open = true;
    s_hist = false;
    s_focus = 0;
    s_status[0] = '\0';
    if (s_rolling) settle();                         // left mid-roll: the result stands
    cs_ui_show(CS_SCR_ASTRO);
    build_view(&s_v);
    cs_ui_astro_update(&s_v);
}

void cs_astro_exit(void) {
    s_open = false;
    s_hist = false;
    if (s_rolling) settle();
}
