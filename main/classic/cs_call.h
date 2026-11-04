// main/classic/cs_call.h -- 通话: walkie-talkie to the companion over MQTT (P2).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Event the shell posts once a second while the call screen is open.
#define CS_CALL_EV_POLL 1

void cs_call_init(void);
// Screen built by the shell (cs_ui_show(CS_SCR_CALL)); LVGL lock held.
void cs_call_enter(void);
// Leaving the call screen (LVGL lock held): an unfinished recording is cancelled.
void cs_call_exit(void);
// One key action from the shell task: UP / DOWN pressed, or OK tapped (released
// before the long press; a long press is the shell's "back"). Returns true
// when the user chose 结束 and the shell should go home.
bool cs_call_key(uint8_t btn);
// Recording, sending or playing a clip (the screen must not dim / lock).
bool cs_call_busy(void);

// One voice take shared by the call screen and the Messages app (shell task).
// voice_begin carries "app" so the gateway knows how to answer (msg = text).
typedef enum { CS_REC_IDLE = 0, CS_REC_ON, CS_REC_STOPPED } cs_rec_state_t;
cs_rec_state_t cs_call_rec_state(void);
uint32_t cs_call_rec_ms(void);
uint32_t cs_call_rec_clip(void);             // clip id of the current / last take (voice_begin "id")
const char *cs_call_rec_note(void);          // last status note ("已发送 ✓", "太短了…")
bool cs_call_rec_start(const char *app);     // false: refused, see the note
void cs_call_rec_stop(void);
bool cs_call_rec_finish(bool send);          // voice_send / voice_cancel of a stopped take
void cs_call_rec_abandon(void);              // leaving the app
// Internal events re-posted to the shell task via cs_shell_post_call().
void cs_call_event(uint8_t ev);
// MQTT "command" messages (called from the MQTT task).
void cs_call_on_command(const char *json, size_t len);
// MQTT "playchunk" message fragments (MQTT task), see cs_net_chunk_cb_t.
void cs_call_on_playchunk(const uint8_t *data, size_t len, size_t offset, size_t total);
