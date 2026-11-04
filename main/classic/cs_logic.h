// main/classic/cs_logic.h -- Classic shell pure logic (host tested, no ESP-IDF / LVGL).
//
// Key filter: the shell sees every BSP button event first. Each key reports a
// short press on release (CS_KEYS_TAP) or a long press (CS_KEYS_LONG,
// CS_LONG_MS), never both, so a press that turns into a long press never also
// fires the short action (OK long = back, UP long = sound on / off). Release
// comes straight from the BSP (debounce only, ~10 ms), unlike CLICK, which
// waits ~190 ms for a possible second click.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- keys
#define CS_LONG_MS 500u    // = BSP_BTN_LONG_PRESS_MS (checked in cs_shell.c)

typedef enum { CS_KEY_UP = 0, CS_KEY_DOWN, CS_KEY_OK, CS_KEY_COUNT } cs_key_t;      // = bsp_btn_t
typedef enum {                                                                       // = bsp_btn_ev_t
    CS_KEV_PRESS = 0, CS_KEV_CLICK, CS_KEV_DOUBLE, CS_KEV_LONG, CS_KEV_RELEASE
} cs_kev_t;

typedef enum {
    CS_KEYS_DROP = 0,      // trailing event of a spent press: nobody sees it
    CS_KEYS_PRESS,         // the key went down
    CS_KEYS_TAP,           // released before CS_LONG_MS: the short action
    CS_KEYS_LONG,          // held to CS_LONG_MS (once per press)
    CS_KEYS_OTHER,         // anything else (raw timing for release guidance only)
} cs_keys_verdict_t;

typedef struct {
    uint32_t down_ms[CS_KEY_COUNT];
    uint8_t live;          // bit per key: down and not decided yet
    uint8_t spent;         // bit per key: drop trailing events until that key's next press
    uint8_t last;          // key of the latest press
} cs_keys_t;

void cs_keys_init(cs_keys_t *k);
cs_keys_verdict_t cs_keys_feed(cs_keys_t *k, uint8_t key, uint8_t ev, uint32_t now_ms);
// The press in progress (the latest one) was used up on PRESS (lock, home,
// alarm, alert) or its screen went away: its release / LONG / CLICK / DOUBLE
// are dropped.
void cs_keys_spend(cs_keys_t *k);

// ---------------------------------------------------------------- standby
// No key for the chosen time: dim to CS_DIM_PERCENT, then CS_DIM_MS later lock
// and switch the backlight off. "hold" (voice, alarm, a release session) keeps
// the screen on and restarts the count. Choice 4 = never.
#define CS_IDLE_CHOICES 5
#define CS_IDLE_DEFAULT 1                 // 1 minute
#define CS_DIM_MS 10000u
#define CS_DIM_PERCENT 30
extern const uint16_t CS_IDLE_S[CS_IDLE_CHOICES];   // 30 60 120 300 0
typedef enum { CS_SCREEN_ON = 0, CS_SCREEN_DIM, CS_SCREEN_OFF } cs_screen_state_t;
typedef struct {
    uint32_t last_ms;      // last activity
    uint32_t timeout_ms;   // 0 = never
    uint8_t state;         // cs_screen_state_t
} cs_idle_t;
void cs_idle_init(cs_idle_t *t, uint32_t timeout_ms, uint32_t now_ms);
// Key or wake-up event: screen on, count restarts.
void cs_idle_wake(cs_idle_t *t, uint32_t now_ms);
// Manual standby: off now (stays off until cs_idle_wake).
void cs_idle_off(cs_idle_t *t);
// New state for the current time; an off screen stays off.
uint8_t cs_idle_tick(cs_idle_t *t, uint32_t now_ms, bool hold);

// ---------------------------------------------------------------- home grid
// 10 grid icons (4 columns) followed by 4 dock icons, in reading order.
typedef enum {
    CS_HOME_MESSAGES = 0, CS_HOME_CALENDAR, CS_HOME_ASTRO, CS_HOME_WEATHER,   // ASTRO: the old Photos slot
    CS_HOME_RELEASE, CS_HOME_CLOCK, CS_HOME_NOTES, CS_HOME_CAMERA,
    CS_HOME_SETTINGS, CS_HOME_MINES,
    CS_HOME_CALL, CS_HOME_MAIL, CS_HOME_GARDEN, CS_HOME_MUSIC,
    CS_HOME_COUNT
} cs_home_item_t;
#define CS_HOME_GRID_COUNT 10
// Icons that open nothing yet (an alert): drawn slightly dimmed, the cursor skips them.
#define CS_HOME_PLACEHOLDERS                                                                       \
    ((1u << CS_HOME_CAMERA) | (1u << CS_HOME_MAIL) | (1u << CS_HOME_GARDEN) | (1u << CS_HOME_MUSIC))
#define CS_HOME_ALL ((1u << CS_HOME_COUNT) - 1u)

// The next item (dir +1 DOWN / -1 UP, reading order, wrapping) whose bit is set
// in `live` (bit i = item i opens an app); no live item: plain step.
int cs_home_step(int sel, int dir, uint32_t live);

// ---------------------------------------------------------------- pomodoro
#define CS_POMO_PRESET_COUNT 7
extern const uint8_t CS_POMO_PRESET_MIN[CS_POMO_PRESET_COUNT];   // 1 3 5 15 30 45 60
#define CS_POMO_DEFAULT_PRESET 3                                  // 15 min

typedef struct {
    bool running;
    int64_t start_us;      // monotonic (esp_timer) time of start
    uint32_t total_s;
} cs_pomo_t;

void cs_pomo_start(cs_pomo_t *p, int64_t now_us, uint32_t total_s);
void cs_pomo_stop(cs_pomo_t *p);
// Whole seconds left, rounded up (shows 15:00 for the first second); 0 once done.
uint32_t cs_pomo_remaining_s(const cs_pomo_t *p, int64_t now_us);
// Microseconds until zero (0 when done / not running).
int64_t cs_pomo_remaining_us(const cs_pomo_t *p, int64_t now_us);
// Elapsed share 0..1000.
uint16_t cs_pomo_progress_permille(const cs_pomo_t *p, int64_t now_us);
// "MM:SS", or "H:MM:SS" when long_format (the 60-minute preset). Returns length.
int cs_pomo_format(uint32_t secs, bool long_format, char *buf, size_t cap);

// ---------------------------------------------------------------- network
// Wi-Fi retry wait after a full round of failed attempts: 5 s doubling to 60 s.
uint32_t cs_backoff_ms(unsigned attempt);

// ---------------------------------------------------------------- weather
typedef enum {
    CS_WX_SUNNY = 0, CS_WX_CLOUDY, CS_WX_OVERCAST, CS_WX_RAIN, CS_WX_SNOW, CS_WX_FOG,
    CS_WX_NIGHT, CS_WX_ICON_COUNT
} cs_wx_icon_t;

cs_wx_icon_t cs_wx_icon_from_name(const char *name);   // unknown -> cloudy

typedef struct {
    uint32_t bg;     // pill colour 0xRRGGBB
    uint32_t text;   // label colour on the pill
} cs_aqi_style_t;

// By level text (优 良 轻度污染 中度污染 重度污染 严重污染); falls back to the
// AQI number when the level is empty or unknown; aqi < 0 and no level -> grey.
cs_aqi_style_t cs_aqi_style(const char *level, int aqi);
// Level text for an AQI number (used when the message has no level).
const char *cs_aqi_level_for(int aqi);

#define CS_WX_DAYS 3
typedef struct {
    char d[16];
    char icon[12];
    int hi, lo;
} cs_wx_day_t;

typedef struct {
    bool valid;
    char city[24];
    char cond[24];
    char icon[12];
    char aqi_level[16];
    char updated[12];
    int temp, hi, lo, aqi;     // aqi -1 when absent
    uint8_t ndays;
    cs_wx_day_t days[CS_WX_DAYS];
} cs_weather_t;

// Parses the retained weather JSON. Needs at least "temp"; other fields optional.
bool cs_weather_parse(const char *json, size_t len, cs_weather_t *out);

// ---------------------------------------------------------------- tiny JSON
// jsmn-style tokenizer: tokens in document order, no allocation.
typedef enum { CSJ_UNDEF = 0, CSJ_OBJ, CSJ_ARR, CSJ_STR, CSJ_PRIM } csj_type_t;
typedef struct {
    uint8_t type;
    int start, end;   // byte offsets [start, end) (strings exclude quotes)
    int size;         // children: object = key count, array = item count
} csj_tok_t;

// Returns the token count, or -1 on malformed input / too many tokens.
int csj_parse(const char *js, size_t len, csj_tok_t *t, int max);
// Index of the first token after the subtree rooted at i.
int csj_next(const csj_tok_t *t, int n, int i);
// Value token of `key` in object token `obj`, or -1.
int csj_get(const char *js, const csj_tok_t *t, int n, int obj, const char *key);
// String value with JSON unescaping (\uXXXX to UTF-8); truncates on a UTF-8 boundary.
bool csj_str(const char *js, const csj_tok_t *t, int i, char *out, size_t cap);
// Number (rounded to nearest integer).
bool csj_int(const char *js, const csj_tok_t *t, int i, int *out);

// ---------------------------------------------------------------- call (P2)
// Cells in the call panel: 0 说话 1 发送 2 删除 / 3 静音 4 免提 (5 empty) then 结束.
// Send sits left of Delete (used more); the cursor walks them in this order.
typedef enum {
    CS_CALL_TALK = 0, CS_CALL_SEND, CS_CALL_DELETE, CS_CALL_MUTE, CS_CALL_SPEAKER,
    CS_CALL_END, CS_CALL_STOPS
} cs_call_stop_t;
int cs_call_step(int cur, int dir);   // wraps over the 6 stops

// IMA ADPCM, 4 bit, standard step / index tables, low nibble first.
typedef struct {
    int16_t predictor;
    uint8_t index;
} cs_adpcm_state_t;
void cs_adpcm_init(cs_adpcm_state_t *s);
// Encodes n samples (n even) into n / 2 bytes; returns the byte count.
size_t cs_adpcm_encode(cs_adpcm_state_t *s, const int16_t *pcm, size_t n, uint8_t *out);
// Decoder (used by the host test round trip only).
size_t cs_adpcm_decode(cs_adpcm_state_t *s, const uint8_t *in, size_t bytes, int16_t *pcm);

// Binary MQTT "audio" chunk header, 12 bytes little-endian:
// u32 clip_id, u16 seq, u16 flags (bit0 = last), i16 predictor, u8 step_index, u8 0.
// predictor / step_index are the encoder state before this chunk's first sample,
// so every chunk decodes on its own.
#define CS_AUDIO_HDR_BYTES 12
#define CS_AUDIO_FLAG_LAST 1u
void cs_audio_hdr_pack(uint8_t out[CS_AUDIO_HDR_BYTES], uint32_t clip_id, uint16_t seq,
                       uint16_t flags, const cs_adpcm_state_t *state);

// Play command: {"op":"play","url":"...","id":"...","text":"...","fmt":"adpcm"}.
// "caption" is accepted when "text" is absent. "fmt" absent or "pcm": raw s16le;
// "adpcm": the block stream below; any other value rejects the command.
// "via" absent or "http": fetch "url" (https only). "via":"mqtt": no url; the
// body arrives as "chunks" playchunk messages (needs "fmt":"adpcm", an id of at
// least 8 characters and 1 <= chunks <= 65535; "bytes" is informational).
typedef enum { CS_PLAY_PCM = 0, CS_PLAY_ADPCM } cs_play_fmt_t;
typedef enum { CS_PLAY_HTTP = 0, CS_PLAY_MQTT } cs_play_via_t;
typedef struct {
    char url[160];
    char id[48];
    char text[96];
    uint8_t fmt;       // cs_play_fmt_t
    uint8_t via;       // cs_play_via_t
    uint16_t chunks;   // via mqtt
    uint32_t bytes;    // via mqtt, 0 if absent
} cs_play_cmd_t;
bool cs_play_cmd_parse(const char *json, size_t len, cs_play_cmd_t *out);

// Downlink "adpcm" body (16 kHz mono): blocks of a 4-byte header
// [i16 LE predictor][u8 step index 0..88][u8 0] + 512 bytes IMA ADPCM
// (1024 samples, low nibble first, same tables as the uplink encoder). Each
// block decodes on its own from its header; the last block may be shorter
// (n data bytes -> 2n samples). The decoder takes the body in any slicing.
#define CS_PLAY_BLOCK_HDR 4
#define CS_PLAY_BLOCK_DATA 512
typedef struct {
    cs_adpcm_state_t st;
    uint16_t data_left;    // data bytes left in the current block (0: reading a header)
    uint8_t hdr_have;
    uint8_t hdr[CS_PLAY_BLOCK_HDR];
} cs_adpcm_stream_t;
void cs_adpcm_stream_init(cs_adpcm_stream_t *d);
// Consumes bytes from in[0..len) (*used set) and writes at most max_samples
// samples (rounded down to even); returns the sample count, or -1 on a header
// with a step index above 88 (the stream is corrupt: stop playing).
int cs_adpcm_stream_feed(cs_adpcm_stream_t *d, const uint8_t *in, size_t len, size_t *used,
                         int16_t *pcm, size_t max_samples);

// Collects one whole block (header + 512 data bytes) from arbitrary slices, so a
// writer only ever hands complete blocks to the playback buffer.
#define CS_PLAY_BLOCK_BYTES (CS_PLAY_BLOCK_HDR + CS_PLAY_BLOCK_DATA)
typedef struct {
    uint8_t buf[CS_PLAY_BLOCK_BYTES];
    uint16_t have;
} cs_block_asm_t;
// Takes bytes until the block is whole (have == CS_PLAY_BLOCK_BYTES); returns
// how many were taken. The caller empties it (have = 0) after using the block.
size_t cs_block_asm_add(cs_block_asm_t *a, const uint8_t *in, size_t len);

// Playchunk message (passport/<user>/playchunk): 12-byte header, little endian:
// char id[8] (first 8 characters of the play id), u16 seq 0..N-1, u16 total N;
// then whole 516-byte blocks, any number per chunk (the server sends 3 so a
// chunk fits the 2048-byte MQTT RX buffer unfragmented; 7 also works). The
// last chunk may hold fewer and its last block may be short.
#define CS_CHUNK_HDR 12
#define CS_CHUNK_BLOCKS_DEFAULT 3   // gap silence before any full chunk was seen
typedef struct {
    char id[8];
    uint16_t total;    // from the play command
    uint16_t next;     // next expected seq
    uint8_t blocks;    // blocks per chunk, learned from the latest non-final chunk (0: none yet)
} cs_chunk_sess_t;
void cs_chunk_sess_init(cs_chunk_sess_t *s, const char *id, uint16_t total);
// Checks a chunk's first bytes; msg_len is the whole message payload length.
// Returns -1 to drop the chunk (shorter than the header, other id, duplicate /
// older seq, seq >= total), otherwise how many blocks of silence stand in for
// the chunks skipped before it (skipped chunks x blocks per chunk, learned from
// the body length of non-final chunks); the session then expects seq + 1. The
// header's own total is not used.
int cs_chunk_accept(cs_chunk_sess_t *s, const uint8_t *hdr, size_t len, size_t msg_len);
bool cs_chunk_complete(const cs_chunk_sess_t *s);   // the last chunk was accepted

// ---------------------------------------------------------------- messages
// One SMS-style message. The companion's are English text (printable ASCII only: the
// device has no CJK body font for them); the owner's are voice clips shown as
// a duration.
#define CS_MSG_TEXT_MAX 280
typedef struct {
    uint32_t ts;            // unix seconds, 0 = unknown
    uint16_t dur_s;         // voice clip length (outgoing)
    uint8_t out;            // 1 = from the owner (voice), 0 = from the companion (text)
    uint32_t aux;           // spare (memos: when it was checked, unix seconds)
    char text[CS_MSG_TEXT_MAX + 1];
} cs_msg_t;

// {"op":"msg","id":"<hex>","text":"...","ts":<unix>}: text required, cut to
// CS_MSG_TEXT_MAX; tabs / newlines become spaces, anything outside 0x20..0x7E
// becomes '?' (one per character). id (kept for de-duplication) and ts optional.
typedef struct {
    char id[24];
    uint32_t ts;
    char text[CS_MSG_TEXT_MAX + 1];
} cs_msg_cmd_t;
bool cs_msg_cmd_parse(const char *json, size_t len, cs_msg_cmd_t *out);

// Message log in a raw flash partition: records appended in 4 KB sectors, the
// oldest sector erased when the log wraps, the newest CS_STORE_KEEP indexed.
// Record: u16 magic, u8 out, u8 0, u16 text_len, u16 dur_s, u32 seq, u32 ts,
// u32 aux, text, zero padding to 4 bytes, u32 FNV-1a of everything before it. A record
// never crosses a sector; a torn record (power loss) ends its sector's scan.
#define CS_STORE_SECTOR 4096u
#define CS_STORE_KEEP 50
typedef struct {
    bool (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    bool (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    bool (*erase)(void *ctx, uint32_t off);   // the sector starting at off
    void *ctx;
    uint32_t size;                 // bytes, a multiple of CS_STORE_SECTOR (>= 2 sectors)
    uint32_t idx[CS_STORE_KEEP];   // record offsets, oldest first
    uint8_t n;
    uint32_t seq;                  // next record's sequence number
    uint32_t wr;                   // next write offset
} cs_store_t;
// Scans the partition (set the callbacks, ctx and size first).
bool cs_store_open(cs_store_t *s);
bool cs_store_add(cs_store_t *s, const cs_msg_t *m);
// i = 0 is the oldest kept message.
bool cs_store_get(const cs_store_t *s, int i, cs_msg_t *out);

// ---------------------------------------------------------------- memos (闪念胶囊)
// One-line English memos in their own log (cs_store, partition "memos"). A
// record is a whole memo state; the newest record per id wins (a check toggle
// appends a new record). Mapping onto cs_msg_t: dur_s = id, out = flags,
// ts = created, aux = checked at, text = the memo.
#define CS_MEMO_TEXT_MAX 120
#define CS_MEMO_DONE 0x01
#define CS_MEMO_TTL_S (7u * 86400u)        // unchecked memos live a week
#define CS_TZ_OFFSET_S (8 * 3600)          // the device runs on CST-8

// Server preview for a recorded memo: {"op":"memo","clip":<u32>,"text":"..."}.
// Text reduced to printable ASCII like messages, cut to CS_MEMO_TEXT_MAX.
typedef struct {
    uint32_t clip;
    char text[CS_MEMO_TEXT_MAX + 1];
} cs_memo_cmd_t;
bool cs_memo_cmd_parse(const char *json, size_t len, cs_memo_cmd_t *out);

// Still shown at `now` (0 = clock unknown: nothing expires). A checked memo
// goes the next local calendar day after it was checked; an unchecked one
// CS_MEMO_TTL_S after it was created (never if created was unknown).
bool cs_memo_visible(uint8_t flags, uint32_t created, uint32_t done_at, uint32_t now);

// ---------------------------------------------------------------- SHA-256
// Small portable SHA-256 (the mines commit check; host tested against FIPS
// 180-2 vectors).
typedef struct {
    uint32_t h[8];
    uint64_t len;          // bytes hashed so far
    uint8_t buf[64];
    uint8_t used;
} cs_sha256_t;
void cs_sha256_init(cs_sha256_t *c);
void cs_sha256_update(cs_sha256_t *c, const void *data, size_t len);
void cs_sha256_final(cs_sha256_t *c, uint8_t out[32]);

// ---------------------------------------------------------------- mines (对战扫雷)
// Two fields, one per player, no neighbour numbers. The owner hides her mines
// on the device (they never leave it); the companion's layout lives on the server,
// committed before play as the first 16 hex digits of sha256(layout + salt)
// and revealed at the end. First to step on the other's mine loses; clearing
// every safe cell of the other's field wins. Cells: row-major index; names
// rows A.. and columns 1.. ("B2" = row 1, column 1).
#define CS_MINES_SIZES 3
#define CS_MINES_DEFAULT_SIZE 1            // 4x4, 3 mines
#define CS_MINES_MAX_N 5
#define CS_MINES_MAX_CELLS (CS_MINES_MAX_N * CS_MINES_MAX_N)
#define CS_MINES_NOTE_MAX 60               // the companion's one-line thought (ASCII)
#define CS_MINES_COMMIT_LEN 16
extern const uint8_t CS_MINES_N[CS_MINES_SIZES];   // 3 4 5
extern const uint8_t CS_MINES_M[CS_MINES_SIZES];   // 2 3 4

typedef enum {
    CS_MINES_IDLE = 0,     // no game (setup screen)
    CS_MINES_PLACE,        // the owner hides her mines; the companion's commit may arrive meanwhile
    CS_MINES_READY,        // her mines are set; waiting for the companion's commit
    CS_MINES_MY_TURN,      // she steps on the companion's field
    CS_MINES_MY_WAIT,      // her step is out; waiting for the companion's answer
    CS_MINES_COMPANION_TURN, // waiting for the companion's step on her field
    CS_MINES_OVER,
} cs_mines_phase_t;
typedef enum { CS_MINES_NOBODY = 0, CS_MINES_OWNER_WON, CS_MINES_COMPANION_WON } cs_mines_winner_t;
typedef enum { CS_MINES_UNREVEALED = 0, CS_MINES_VERIFIED, CS_MINES_MISMATCH } cs_mines_reveal_t;

typedef struct {
    uint32_t game;         // id chosen by the device, echoed by the server
    uint8_t n, m;          // side, mines per field
    uint8_t phase;         // cs_mines_phase_t
    uint8_t winner;        // cs_mines_winner_t
    uint8_t companion_first;
    uint8_t reveal;        // cs_mines_reveal_t
    int8_t pending;        // MY_WAIT: her cell in flight, else -1
    int8_t boom;           // the cell that ended the game on a mine, else -1
    uint32_t mines;        // her mines (bit per cell)
    uint32_t companion_steps; // the companion's footprints on her field
    uint32_t my_steps;     // her safe steps on the companion's field
    uint32_t companion_mines; // the companion's layout once revealed
    char commit[CS_MINES_COMMIT_LEN + 1];
} cs_mines_t;

int cs_mines_cells(const cs_mines_t *g);           // n * n
int cs_mines_count(uint32_t mask);                 // set bits
// "B2" for cell 5 of a 4x4 field (buf >= 4). False when out of range.
bool cs_mines_cell_name(int cell, int n, char *buf, size_t cap);
// Cell index for "B2" / "b2" (spaces around allowed), -1 if invalid for side n.
int cs_mines_cell_parse(const char *s, int n);
// Cursor over `stops` positions, wrapping (dir +1 next, -1 previous).
int cs_mines_cursor_step(int cur, int dir, int stops);

// New game in PLACE (size index into CS_MINES_N / CS_MINES_M).
void cs_mines_new(cs_mines_t *g, int size, bool companion_first, uint32_t game);
// PLACE: OK on a cell adds / removes a mine (no more than m). True if changed.
bool cs_mines_toggle(cs_mines_t *g, int cell);
// PLACE with all m mines down: READY, or straight into play when the commit is
// already here. True if accepted.
bool cs_mines_go(cs_mines_t *g);
// The companion's commit (16 hex digits, stored lower case). Accepted in PLACE / READY
// while none is set (the same one again is fine); READY then starts play.
bool cs_mines_commit(cs_mines_t *g, const char *hex);
// MY_TURN: her step on an unopened cell of the companion's field -> MY_WAIT.
bool cs_mines_step(cs_mines_t *g, int cell);
// MY_WAIT: no answer in time; back to MY_TURN (she may step again).
void cs_mines_unwait(cs_mines_t *g);
// The companion's answer for her pending cell. True if it matched and was applied:
// boom -> OVER (the companion won); her last safe cell -> OVER (she won); else COMPANION_TURN.
bool cs_mines_result(cs_mines_t *g, int cell, bool boom);
// The companion's step on her field, judged here. Returns 1 boom (she won), 0 safe
// (the companion won if it was her last safe cell, else MY_TURN), -2 a cell the companion had
// already stepped on (a re-delivery: answer again, nothing changes), -1
// refused (not its turn / invalid cell).
int cs_mines_companion_step(cs_mines_t *g, int cell);
// Reveal at OVER: layout ("A2,C1": m distinct cells) and salt as sent. Sets
// companion_mines and reveal = VERIFIED when sha256(layout + salt) starts with the
// commit and the layout agrees with every answer she got (no safe step on a
// mine, her fatal cell is a mine), else MISMATCH. False if not at OVER.
bool cs_mines_reveal(cs_mines_t *g, const char *layout, const char *salt);
// First 16 hex digits of sha256(layout + salt) (out >= 17 bytes).
void cs_mines_commit_of(const char *layout, const char *salt, char *out);
// Cell mask to "B3,C2,C4" (ascending, comma separated); false if it does not fit.
bool cs_mines_layout_str(uint32_t mask, int n, char *buf, size_t cap);
// Her layout may leave the device: the game is over, or she quits after at
// least one step by either side (a quit before any step reveals nothing).
bool cs_mines_may_reveal_mine(const cs_mines_t *g);
// Layout string to a cell mask; -1 if malformed / repeated cells, else the count.
int cs_mines_layout_parse(const char *layout, int n, uint32_t *mask);

// Commands from the server (passport/<user>/command), all with "game":<u32>:
//   {"op":"mine_commit","game":G,"commit":"<16 hex>"}
//   {"op":"mine_result","game":G,"cell":"B2","boom":false}
//   {"op":"mine_companion_step","game":G,"cell":"C3","note":"<= 60 ASCII chars"}
//   {"op":"mine_reveal","game":G,"layout":"A2,C1","salt":"<hex>"}
typedef enum { CS_MCMD_COMMIT = 1, CS_MCMD_RESULT, CS_MCMD_COMPANION_STEP, CS_MCMD_REVEAL } cs_mines_op_t;
typedef struct {
    uint8_t op;            // cs_mines_op_t
    bool boom;
    uint32_t game;
    char cell[4];
    char commit[CS_MINES_COMMIT_LEN + 1];
    char note[CS_MINES_NOTE_MAX + 1];
    char layout[40];
    char salt[65];
} cs_mines_cmd_t;
bool cs_mines_cmd_parse(const char *json, size_t len, cs_mines_cmd_t *out);

// ---------------------------------------------------------------- bingo (生存 Bingo)
// A daily card of short ASCII labels ("MAG", "5 CUPS", "UP<10"), 4x4 or 5x5.
// The server rolls it (fixed cells + random ones from a pool), auto-ticks some
// from NFC check-ins and keeps the companion's hidden whisper until the first line.
// The device caches the card, lets the owner tick / untick offline and sends
// each change with a sequence number; the server echoes the highest number
// it applied ("ack"), so her unacknowledged changes win over older cards.
// Cells: row-major. A card day runs from 06:00 to 06:00 Beijing time.
#define CS_BINGO_MAX_N 5
#define CS_BINGO_MAX_CELLS (CS_BINGO_MAX_N * CS_BINGO_MAX_N)
#define CS_BINGO_LABEL_MAX 8
#define CS_BINGO_WHISPER_MAX 80
#define CS_BINGO_DAY_LEN 10                // "2026-11-02"
#define CS_BINGO_DAY_START_S (6 * 3600)    // local 06:00

typedef struct {
    char day[CS_BINGO_DAY_LEN + 1];        // "" = no card yet
    uint8_t n;                             // 4 or 5 (0 = no card)
    uint8_t bingo;                         // the server saw the first line (whisper may be set)
    uint8_t celebrated;                    // the first BINGO! note was shown on the device
    uint8_t lines_seen;                    // complete lines already celebrated
    uint32_t done;                         // ticked cells (bit per cell)
    uint32_t autod;                        // ticked by the server from check-ins
    uint32_t fixed;                        // on the card every day (never swapped)
    uint32_t pending;                      // her changes the server has not acknowledged
    uint32_t seq;                          // number of her latest change
    uint32_t pend_seq[CS_BINGO_MAX_CELLS]; // change number per pending cell
    char cells[CS_BINGO_MAX_CELLS][CS_BINGO_LABEL_MAX + 1];
    char whisper[CS_BINGO_WHISPER_MAX + 1];
} cs_bingo_card_t;

// {"op":"bingo_card","day":"2026-11-02","n":4,"cells":"MAG,UP<10,...","done":5,
//  "auto":1,"fixed":3,"ack":17,"bingo":false[,"whisper":"..."]}
// cells: n*n distinct labels, comma separated, 1..8 printable ASCII each
// (no comma, quote or backslash). whisper: reduced to printable ASCII, cut to 80.
typedef struct {
    char day[CS_BINGO_DAY_LEN + 1];
    uint8_t n;
    bool bingo;
    uint32_t done, autod, fixed, ack;
    char cells[CS_BINGO_MAX_CELLS][CS_BINGO_LABEL_MAX + 1];
    char whisper[CS_BINGO_WHISPER_MAX + 1];
} cs_bingo_cmd_t;
bool cs_bingo_cmd_parse(const char *json, size_t len, cs_bingo_cmd_t *out);

// Card day for unix time t (06:00 Beijing boundary): "2026-11-02". out >= 11 bytes.
void cs_bingo_card_day(uint32_t t, char *out);
int cs_bingo_cells(const cs_bingo_card_t *c);    // n * n
// Complete lines (rows, columns, both diagonals) in `done` on an n x n card;
// *cells (optional) gets the union of their cells.
int cs_bingo_lines(uint32_t done, int n, uint32_t *cells);
// The first complete line: "row 2" / "col 3" / "diag" / "anti" (rows / columns
// from 1); false if none. Its cells go to *cells (optional).
bool cs_bingo_first_line(uint32_t done, int n, char *name, size_t cap, uint32_t *cells);
// Her tick / untick: flips the cell, marks it pending under a new change
// number (returned; 0 = refused: no card / bad cell).
uint32_t cs_bingo_toggle(cs_bingo_card_t *c, int cell);
// A server card into the cached one. Another day replaces it (her pending
// changes for the old day are dropped). The same day: changes up to `ack`
// stop being pending; the rest of hers win over the server's ticks; a cell
// whose label changed (swap / new size) loses its pending change. The change
// counter never goes below ack. Returns true when the labels / size changed.
bool cs_bingo_merge(cs_bingo_card_t *c, const cs_bingo_cmd_t *m);
// "3/16" style counts.
int cs_bingo_done_count(const cs_bingo_card_t *c);

// ---------------------------------------------------------------- astro dice
// Three twelve-sided dice: planet, sign, house. Faces come from random bytes
// (the hardware RNG on the device) by rejection sampling: a byte >= 240 is
// skipped and the rest map by % 12, so every face has exactly 20 of the 240
// accepted values (no modulo bias).
#define CS_ASTRO_FACES 12
#define CS_ASTRO_ACCEPT 240u            // 20 * 12: bytes below this are used
typedef enum { CS_ASTRO_PLANET = 0, CS_ASTRO_SIGN, CS_ASTRO_HOUSE, CS_ASTRO_DICE } cs_astro_die_t;
// English names (planets: Sun .. Pluto, North Node, South Node; signs: Aries ..
// Pisces), in the order of the symbol images in cs_astro_art.
extern const char *const CS_ASTRO_PLANETS[CS_ASTRO_FACES];
extern const char *const CS_ASTRO_SIGNS[CS_ASTRO_FACES];
// One face from b[0..n): the first byte < 240, % 12. *used (optional) = bytes
// consumed, including the rejected ones; -1 when all n were rejected.
int cs_astro_face(const uint8_t *b, size_t n, size_t *used);
// Three faces (planet, sign, house - 1) from one byte stream; false if it ran out.
bool cs_astro_roll(const uint8_t *b, size_t n, uint8_t faces[CS_ASTRO_DICE]);
// {"type":"astro_roll","planet":"Venus","sign":"Scorpio","house":7}; length or -1.
int cs_astro_event_json(const uint8_t faces[CS_ASTRO_DICE], char *out, size_t cap);
// Rolling animation: the results shimmer like reflections on water, cycling
// through faces, and surface one by one at CS_ASTRO_SETTLE_MS[i] after the
// press, on the real result. `shown` gets each die's face `elapsed` ms in;
// returns the mask of dice already settled (7 = all).
#define CS_ASTRO_FRAME_MS 70u
#define CS_ASTRO_SURFACE_MS 200u          // settled -> fully sharp
#define CS_ASTRO_ANIM_MS (1000u + CS_ASTRO_SURFACE_MS)
extern const uint16_t CS_ASTRO_SETTLE_MS[CS_ASTRO_DICE];
uint8_t cs_astro_anim(uint32_t elapsed, const uint8_t faces[CS_ASTRO_DICE], uint8_t seed,
                      uint8_t shown[CS_ASTRO_DICE]);
// How die `die` looks `elapsed` ms into a roll: opacity of the symbol, its
// wobble offset, a faint mirrored ghost (reflection) and the glow. Before it
// settles: dim, wobbling, ghost on; while surfacing: the wobble dies out and
// it sharpens to full; after: still and sharp (opa 255, no offset, no ghost).
typedef struct {
    uint8_t opa, ghost, glow;
    int8_t dx, dy;
} cs_astro_fx_t;
void cs_astro_fx(uint32_t elapsed, int die, cs_astro_fx_t *out);

// History: the last CS_ASTRO_LOG rolls, newest first, kept in NVS (one blob).
#define CS_ASTRO_LOG 10
typedef struct {
    uint32_t t;                       // unix seconds; 0 = the clock was not set
    uint8_t f[CS_ASTRO_DICE];         // planet, sign, house - 1
    uint8_t pad;
} cs_astro_log_t;
// Adds a roll at the front (the oldest falls off); *n counts up to CS_ASTRO_LOG.
void cs_astro_log_push(cs_astro_log_t log[CS_ASTRO_LOG], uint8_t *n, uint32_t t, const uint8_t f[CS_ASTRO_DICE]);
// Drops entries with a face out of range (a bad NVS blob); returns the new count.
uint8_t cs_astro_log_clean(cs_astro_log_t log[CS_ASTRO_LOG], uint8_t n);
// "Today 21:52" / "Yesterday 23:05" / "Oct 31 14:02" (Beijing time) for t seen
// at `now` (0 = clock not set: dates only); t == 0: an em dash.
void cs_astro_when(uint32_t t, uint32_t now, char *out, size_t cap);
