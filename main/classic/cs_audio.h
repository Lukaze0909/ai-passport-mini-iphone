// main/classic/cs_audio.h -- codec arbitration and the pomodoro chime.
//
// Every user of the ES8311 (release-guidance key sounds, chime, call recording
// and playback) takes the audio mutex first; acquire switches the sample rate
// when needed, so bsp_audio_set_format() is never called with PCM in flight.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t cs_audio_init(bool codec_ok);
bool cs_audio_ok(void);
bool cs_audio_acquire(uint32_t rate_hz, uint32_t timeout_ms);
void cs_audio_release(void);

// Hook for rg_app_set_audio_hooks(): short wait, never blocks a key for long.
bool cs_audio_hook_acquire(uint32_t rate_hz);
void cs_audio_hook_release(void);

// Alarm chime: a short three-note bell pattern, CS_CHIME_REPEATS times or until
// stopped. Plays regardless of the key-sound mute.
#define CS_CHIME_REPEATS 6
void cs_audio_chime_start(void);
void cs_audio_chime_stop(void);
bool cs_audio_chime_active(void);

// Output volume helpers (call speaker toggle); restore resets to the default.
#define CS_AUDIO_VOLUME_NORMAL 60
#define CS_AUDIO_VOLUME_LOUD 85
void cs_audio_set_volume(uint8_t percent);
