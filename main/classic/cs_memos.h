// main/classic/cs_memos.h -- Notes (闪念胶囊): one-line English memos.
//
// LIST: memos newest first, OK toggles the check, "+" opens NEW. NEW: record
// a take (voice_begin "app":"memo"), the server answers with a one-sentence
// preview ({"op":"memo","clip":<id>,"text":...}), OK on Send saves it. Checked
// memos go the next local day, unchecked ones after 7 days. Log in the
// "memos" flash partition (cs_store_*).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cs_memos_init(void);
// MQTT task: a preview command. False: not a memo command.
bool cs_memos_on_command(const char *json, size_t len);

// Shell task.
void cs_memos_enter(void);           // the Notes app opened (LVGL lock held): LIST
void cs_memos_exit(void);            // leaving the app: a pending take / preview is dropped
void cs_memos_key(uint8_t btn);      // UP / DOWN / OK tap
bool cs_memos_back(void);            // long press OK: NEW -> LIST (true); on LIST false (go home)
void cs_memos_changed(void);         // the shared voice take changed
void cs_memos_preview(void);         // a preview was posted (M_MEMO)
void cs_memos_poll(uint32_t now_ms); // preview timeout
bool cs_memos_busy(void);            // waiting for a preview (keep the screen on)
