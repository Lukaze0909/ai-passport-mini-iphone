// main/classic/cs_bingo.h -- Bingo (生存 Bingo): the owner's daily card.
//
// The server rolls today's card (a 06:00 Beijing day; fixed cells plus random
// ones), auto-ticks some from NFC check-ins and keeps the companion's whisper hidden
// until her first line. The device caches the card in NVS, ticks offline and
// syncs each change; Swap (one unticked cell) and a new size need the server.
// Keys: UP / DOWN walk the cells then the three buttons (Swap, size, companion),
// wrapping; OK ticks / unticks. Long press OK closes the note or cancels a
// swap, else home.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cs_bingo_init(void);                // loads the cached card (shell start)
// MQTT task: a bingo_card command. False: not one of ours.
bool cs_bingo_on_command(const char *json, size_t len);

// Shell task.
void cs_bingo_enter(void);               // the app opened (LVGL lock held)
void cs_bingo_exit(void);
void cs_bingo_key(uint8_t btn);          // UP / DOWN / OK tap
bool cs_bingo_back(void);                // long press OK: closes the note / a swap (true), else false (home)
void cs_bingo_event(void);               // M_BINGO: apply the latest card
void cs_bingo_poll(uint32_t now_ms);     // sync, day change, note timers
bool cs_bingo_done_today(void);          // today's card has a line (home icon; any task)
