// main/classic/cs_ui.h -- Classic shell LVGL screens.
//
// Every function here touches LVGL: call from the LVGL task or with
// bsp_lvgl_lock() held. The module has no ESP-IDF / FreeRTOS dependency so the
// same code renders on desktop LVGL (tools/classic/host_render.c). Live data
// comes from cs_view_get() / cs_view_weather(), implemented by the platform.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cs_companion.h"
#include "cs_logic.h"

typedef enum {
    CS_LINK_OFF = 0,      // Wi-Fi switch off
    CS_LINK_NO_CREDS,     // switch on but NVS has no network
    CS_LINK_CONNECTING,
    CS_LINK_HOME,         // connected to ssid0
    CS_LINK_HOTSPOT,      // connected to ssid1
} cs_link_t;

typedef struct {
    bool time_valid;
    int mon, mday, wday, hour, min;     // local time (CST-8), mon 1..12, wday 0 = Sunday
    uint8_t link;                       // cs_link_t
    bool mqtt;                          // broker connected (settings: "已连上")
    int batt;                           // 0..100, -1 unknown
    bool pomo_running;
    bool pomo_long;                     // H:MM:SS format (60-minute preset)
    uint32_t pomo_left_s, pomo_total_s;
    uint16_t pomo_permille;
    uint32_t wx_version;                // bumps when new weather arrives
    bool wx_valid;
    int wx_temp;
    int msgs_unread;                    // red count on the Messages icon (0 = none)
    bool bingo_done;                    // today's Bingo has a line: gold check on its icon
} cs_view_t;

void cs_view_get(cs_view_t *v);
bool cs_view_weather(cs_weather_t *out);

typedef enum {
    CS_SCR_NONE = 0,
    CS_SCR_LOCK,
    CS_SCR_HOME,
    CS_SCR_SETTINGS,
    CS_SCR_POMO_PICK,
    CS_SCR_POMO_RUN,
    CS_SCR_POMO_DONE,
    CS_SCR_WEATHER,
    CS_SCR_CALL,
    CS_SCR_MSGS,
    CS_SCR_NOTES,     // memo list
    CS_SCR_MEMO,      // new memo
    CS_SCR_MINES,     // mutual minesweeper (setup card or a board)
    CS_SCR_BINGO,     // the daily Bingo card
    CS_SCR_ASTRO,     // astro dice
    CS_SCR_ASTRO_HIST,   // astro dice: the last ten rolls
} cs_screen_t;

// Creates the periodic refresh timer. Call once.
void cs_ui_init(void);
// Builds and loads `s`, then deletes the previously active screen.
void cs_ui_show(cs_screen_t s);
// Forget this module's objects: another module (release guidance) is about to
// load its own screen and delete ours.
void cs_ui_detach(void);
cs_screen_t cs_ui_current(void);
// Re-reads the view now instead of waiting for the next tick.
void cs_ui_refresh(void);

void cs_ui_home_select(int sel);
// Settings: focus 0 = Wi-Fi switch, 1 = auto-lock; idle_choice indexes CS_IDLE_S.
void cs_ui_settings(int focus, int idle_choice);
void cs_ui_pomo_pick(int preset);

// "即将推出" alert on the top layer; hides itself after CS_ALERT_MS.
#define CS_ALERT_MS 1200
void cs_ui_alert(bool show);
bool cs_ui_alert_visible(void);
// Small toast under the status bar on the top layer (text copied).
void cs_ui_toast(const char *text, uint32_t ms);

typedef struct {
    uint8_t cursor;       // cs_call_stop_t
    bool enabled;         // broker connected
    bool recording;
    bool muted;
    bool speaker;
    char status[160];
} cs_call_view_t;
void cs_ui_call_update(const cs_call_view_t *cv);

// Messages (early-2010s skeuomorphic style thread). Bubbles are rebuilt only when gen changes;
// otherwise only the bottom bar and status line update.
#define CS_MSGS_VIEW 6
typedef struct {
    const cs_msg_t *msgs;   // oldest first; the last sits at the bottom
    int n;
    bool more_above;        // older messages exist above the first
    uint32_t gen;           // changes whenever msgs changes
    uint8_t bar;            // focused control: 0 录音, 1 发送, 2 删除
    uint8_t rec;            // 0 idle, 1 recording, 2 take ready (send / delete)
    bool enabled;           // broker connected
    char status[96];
} cs_msgs_view_t;
void cs_ui_msgs_update(const cs_msgs_view_t *v);

// Notes (early-2010s skeuomorphic style: leather bar, yellow ruled paper, red margin).
#define CS_NOTES_ROWS 9
typedef struct {
    int count;              // memos in the list
    int first;              // list index of rows[0]
    int cursor;             // -1 = the "+" button, else a list index
    uint8_t n;              // rows filled
    struct {
        char text[CS_MEMO_TEXT_MAX + 1];
        bool done;
    } rows[CS_NOTES_ROWS];
} cs_notes_view_t;
void cs_ui_notes_update(const cs_notes_view_t *v);

typedef enum {
    CS_MEMO_IDLE = 0, CS_MEMO_REC, CS_MEMO_TAKE, CS_MEMO_WAIT, CS_MEMO_PREVIEW, CS_MEMO_FAIL
} cs_memo_state_t;
typedef struct {
    uint8_t state;          // cs_memo_state_t
    uint8_t focus;          // 0 mic, 1 send, 2 trash
    uint8_t enabled;        // bit i: button i usable
    const char *text;       // preview text (CS_MEMO_PREVIEW), or the memo in detail
    char status[96];
    bool detail;            // reading a saved memo: no buttons, title "Memo"
    bool done;              // detail: the memo is checked
} cs_memo_view_t;
void cs_ui_memo_update(const cs_memo_view_t *v);

// Mines (对战扫雷). One screen: the setup card, or one of the two fields
// (hers in warm sand, the companion's in cool steel) with a cursor ring, the companion's last
// thought under the board, a status line and two buttons; the end screen is a
// blue hand-lettered sticky note on top.
typedef enum {
    CS_MCELL_SAND = 0,    // her field: nothing here
    CS_MCELL_FLAG,        // her field: one of her mines (red flag)
    CS_MCELL_FOOT,        // her field: the companion stepped here (footprints)
    CS_MCELL_BOOM,        // a mine went off here (either field)
    CS_MCELL_GREY,        // the companion's field: unopened
    CS_MCELL_GREEN,       // the companion's field: she stepped here safely
    CS_MCELL_WAIT,        // the companion's field: her step is waiting for the answer
    CS_MCELL_MINE,        // the companion's field: its mines, revealed at the end
    CS_MCELL_KINDS
} cs_mcell_t;
typedef enum { CS_MOVL_NONE = 0, CS_MOVL_DEAD, CS_MOVL_DEAD_AGAIN, CS_MOVL_WIN } cs_movl_t;
typedef enum { CS_MNOTE_PLAIN = 0, CS_MNOTE_COMPANION, CS_MNOTE_OK, CS_MNOTE_BAD } cs_mnote_t;
typedef struct {
    bool setup;                 // setup card instead of a board
    uint8_t setup_focus;        // 0 size, 1 who starts, 2 start
    char size_txt[16];          // "4x4 · 3"
    char first_txt[16];         // "You" / the companion name
    char score_txt[40];         // "You 2 : 3 <companion>"
    uint8_t n;                  // board side
    uint8_t field;              // 0 hers, 1 the companion's
    uint8_t cells[CS_MINES_MAX_CELLS];   // cs_mcell_t
    int8_t cursor;              // < n*n a cell; n*n button 0; n*n + 1 button 1
    const char *btn[2];
    uint8_t btn_on;             // bit i: button i usable
    uint8_t btn_warn;           // bit i: red (asks to confirm)
    char status[48];
    char note[128];             // up to two lines
    uint8_t note_kind;          // cs_mnote_t
    uint8_t overlay;            // cs_movl_t
    bool owner_sent;            // her layout went to the companion (a line on the end note)
} cs_mines_view_t;
void cs_ui_mines_update(const cs_mines_view_t *v);

// Bingo (the calendar icon): a blue notebook page. System-blue header with the
// title, the date (or a status) and a progress ring; ruled paper; white tiles
// with an empty circle (paler cream with a golden border for the fixed ones, a
// gold disc with a check on cream once done); Swap / size / companion buttons; the
// BINGO! sticky note (Mines' end-screen paper) on top.
typedef enum { CS_BOVL_NONE = 0, CS_BOVL_FIRST, CS_BOVL_AGAIN } cs_bovl_t;
typedef struct {
    const cs_bingo_card_t *card;   // NULL or n == 0: no card yet (msg shows instead)
    uint32_t gen;                  // changes when the labels or the size change
    int8_t cursor;                 // < n*n a cell; n*n + i button i
    bool swap;                     // picking a cell to swap (orange cursor)
    uint32_t busy;                 // cells waiting for the server ("...")
    const char *btn[3];
    uint8_t btn_on;                // bit i: button i usable
    uint8_t btn_warn;              // bit i: red (asks to confirm)
    char line2[48];                // under the title: the date or a status
    char msg[64];                  // no card: shown in place of the grid
    uint8_t pct;                   // ticked share, 0..100
    uint8_t overlay;               // cs_bovl_t
    const char *head;              // small line on the note ("<name> whispers:")
    const char *note;              // text on the note under BINGO!
} cs_bingo_view_t;
void cs_ui_bingo_update(const cs_bingo_view_t *v);

// Astro dice (占星骰子, the old Photos icon), round 11b "water": a lavender
// water photo full screen with a frosted title bar (the wizard hat = send to
// the companion on the left, the clock = History on the right) and a frosted Roll pill;
// the three results float on the water as big white symbols with a soft
// lavender glow, a small white name under each; a caption line under them.
typedef struct {
    bool have;                         // a roll to show (else three sparkles)
    uint32_t anim_ms;                  // ms into the roll (shimmer / surface); >= CS_ASTRO_ANIM_MS still
    uint8_t faces[CS_ASTRO_DICE];      // faces shown now (planet, sign, house - 1)
    uint8_t focus;                     // 0 Roll, 1 the hat (send to the companion), 2 the clock (History)
    bool sent;                         // this roll went to the companion (a gold check on the hat)
    char caption[40];                  // under the results: a hint or a status
} cs_astro_view_t;
void cs_ui_astro_update(const cs_astro_view_t *v);
// History: the last rolls, newest first, a time and the three results each.
typedef struct {
    uint8_t n;
    struct {
        char when[20];                 // "Today 21:52", "Oct 31 14:02", or an em dash
        uint8_t f[CS_ASTRO_DICE];
    } rows[CS_ASTRO_LOG];
} cs_astro_hist_view_t;
void cs_ui_astro_hist_update(const cs_astro_hist_view_t *v);

// Formats "10月31日 星期六" (empty when the clock is not valid).
void cs_ui_format_date(const cs_view_t *v, char *buf, int cap);
