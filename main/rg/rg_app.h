// main/rg/rg_app.h -- Release Guidance (释放引导) application entry.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Standalone mode. Call from app_main after bsp_display_init() and
// bsp_lvgl_init() succeeded. Initializes buttons, audio and NVS itself (each
// optional: the app still runs without sound or persistence), builds the UI and
// starts its tasks. Returns after startup; the tasks keep running.
esp_err_t rg_app_start(void);

// ---- Shell mode: release guidance as one app inside the classic shell. ----
// Codec arbitration for the key-sound worker: acquire sets the sample rate and
// returns false when the codec is busy (the sound is skipped). Set before init.
typedef bool (*rg_audio_acquire_fn)(uint32_t rate_hz);
typedef void (*rg_audio_release_fn)(void);
void rg_app_set_audio_hooks(rg_audio_acquire_fn acquire, rg_audio_release_fn release);

// NVS, audio, sound bank and the worker task; no UI, no buttons. Call once.
esp_err_t rg_app_init(void);
// Build / free the UI. Call with bsp_lvgl_lock() held. enter loads its own
// screen and deletes the previously active one; exit deletes the animation and
// status timers (the next screen load deletes the objects).
void rg_app_enter(void);
void rg_app_exit(void);
// Key events and hold checks from the shell's input task (LVGL lock NOT held).
// key / poll return true when the user held OK ~1 s on the entry page: the
// shell should leave the app (on any other page the hold returns to the entry page).
bool rg_app_key(uint8_t btn, uint8_t ev, uint32_t ms);
bool rg_app_deadline(uint32_t *ms);
bool rg_app_poll(uint32_t now_ms);
// Key sounds for the shell (RG_SOUND_TAP / RG_SOUND_CONFIRM, or the unlock
// click below); silent when muted.
#define RG_APP_SOUND_UNLOCK 0x10   // synthesized "ka-da" (no recording), 30 ms
void rg_app_sound(int kind);
bool rg_app_muted(void);
// Past the entry page (a session in progress; shell mode).
bool rg_app_in_session(void);
// The one key-sound switch (NVS rg_guide/muted), shared with the shell: toggles,
// saves, ticks when turning sound on, and with in_app flashes the release
// guidance status word (call only while its UI is built). Returns muted.
bool rg_app_toggle_mute(bool in_app);
// The status word for the current setting ("有声" / "静音").
const char *rg_app_sound_word(void);
