// main/classic/cs_astro.h -- Astro dice (占星骰子): planet, sign and house.
//
// Three twelve-sided dice rolled with the hardware RNG (rejection sampling,
// cs_logic). Main screen keys: UP / DOWN move the focus between Roll, the
// wizard hat (top left) and the clock (top right); OK on Roll rolls (the
// results shimmer on the water for about a second and surface one by one, a
// soft note each); OK on the hat sends the roll to the companion ({"type":"astro_roll",
// ...} on the event topic); OK on the clock opens History (the last ten rolls,
// kept in NVS "classic/astro"). OK or a long press closes History; a long
// press on the main screen goes home.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void cs_astro_init(void);                // loads the history (shell start)
// Shell task.
void cs_astro_enter(void);               // the app opened (LVGL lock held)
void cs_astro_exit(void);
void cs_astro_key(uint8_t btn);          // UP / DOWN / OK tap
bool cs_astro_back(void);                // long press OK: History -> the dice (true), else false (home)
void cs_astro_poll(uint32_t now_ms);     // animation frames, caption timeout
// While the results move the shell should poll at least this often (ms); 0 = idle.
uint32_t cs_astro_poll_ms(void);
