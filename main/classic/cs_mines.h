// main/classic/cs_mines.h -- Mines (对战扫雷): two-field mutual minesweeper with the companion.
//
// SETUP: size (3x3/2, 4x4/3, 5x5/4) and who starts, Start sends mine_new.
// PLACE: the owner hides her mines on her own field (they never leave the
// device); the companion's commit (sha256 prefix of its layout + salt) arrives
// meanwhile; Go sends mine_ready. PLAY: she steps on the companion's field (the server
// answers safe / boom), the companion steps on hers (judged here, only the result goes
// out), one step each in turn. OVER: DEAD! / DEAD AGAIN! / YOU WIN! sticky
// note, then the reveal is checked against the commit. Keys: UP / DOWN walk
// the cells then the two buttons (wrapping), OK acts; button 0 is Go / Flip
// (switch fields), button 1 Quit (OK twice) / Again. Long press OK = home
// (the game stays; reopening resumes it).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cs_mines_init(void);
// MQTT task: a mine_* command. False: not one of ours.
bool cs_mines_on_command(const char *json, size_t len);

// Shell task.
void cs_mines_enter(void);            // the app opened (LVGL lock held)
void cs_mines_exit(void);             // leaving the app (the game stays)
void cs_mines_key(uint8_t btn);       // UP / DOWN / OK tap
bool cs_mines_back(void);             // long press OK: closes the end note (true), else false (home)
void cs_mines_event(void);            // M_MINES: apply queued commands
void cs_mines_poll(uint32_t now_ms);  // timers (auto flip, answer timeout)
bool cs_mines_hold(uint32_t now_ms);  // keep the screen on (waiting for the companion, not for ever)
