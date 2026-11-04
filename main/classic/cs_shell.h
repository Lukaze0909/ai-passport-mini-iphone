// main/classic/cs_shell.h -- Classic system shell entry.
#pragma once

#include "esp_err.h"

// Call from app_main after bsp_display_init() and bsp_lvgl_init() succeeded.
// Starts NVS, audio, buttons, battery, networking and the shell task, then shows
// the lock screen. Release guidance runs inside the shell as the 释放 app.
esp_err_t cs_shell_start(void);

#include <stdbool.h>
#include <stdint.h>

// For the call app (cs_call.c): queue an event for cs_call_event() on the shell
// task, show a toast on any screen, and ask whether the call screen is showing.
void cs_shell_post_call(uint8_t event);
void cs_shell_toast(const char *text);
bool cs_shell_in_call_app(void);
// Screen on (from standby: the lock screen stays) and restart the idle count.
// Shell task only (cs_call_event runs there).
void cs_shell_wake(void);
// cs_call: the shared voice take changed (shell task).
void cs_shell_call_changed(void);
// cs_msgs: a message from the companion was stored (any task).
void cs_shell_post_msg(void);
// cs_memos: a memo preview arrived (any task).
void cs_shell_post_memo(void);
// cs_mines: a mine_* command was queued (any task).
void cs_shell_post_mines(void);
// cs_bingo: a bingo_card command arrived (any task).
void cs_shell_post_bingo(void);
