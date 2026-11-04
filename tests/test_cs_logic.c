// Host tests for the classic shell pure logic: long-press key filter, home grid,
// pomodoro math, Wi-Fi backoff, weather JSON / AQI colours, call cursor,
// IMA ADPCM against a Python reference vector, audio chunk header.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cs_adpcm_vectors.h"
#include "cs_logic.h"

#define P CS_KEV_PRESS
#define C CS_KEV_CLICK
#define D CS_KEV_DOUBLE
#define L CS_KEV_LONG
#define R CS_KEV_RELEASE

static void test_keys(void) {
    cs_keys_t k;
    cs_keys_init(&k);
    // Short OK press (BSP: PRESS, RELEASE, CLICK ~190 ms later): TAP on release.
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 1000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 1120) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_OK, C, 1310) == CS_KEYS_OTHER);
    // Long OK press (PRESS, LONG at 500 ms, RELEASE): BACK once, never a TAP.
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 2000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, L, 2500) == CS_KEYS_LONG);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 3200) == CS_KEYS_OTHER);
    // Two quick presses are two taps (PRESS, RELEASE, PRESS, RELEASE, DOUBLE).
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 5000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 5080) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 5150) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 5230) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_OK, D, 5420) == CS_KEYS_OTHER);
    // Tap then press-and-hold inside the click window: the BSP sends no LONG,
    // so the release decides by the held time (boundary: 499 tap, 500 back).
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 6000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 6080) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 6150) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 6650) == CS_KEYS_LONG);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 7000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 7499) == CS_KEYS_TAP);
    // UP / DOWN are timed the same way: tap on release, LONG once, CLICK OTHER.
    assert(cs_keys_feed(&k, CS_KEY_UP, P, 8000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_UP, L, 8500) == CS_KEYS_LONG);
    assert(cs_keys_feed(&k, CS_KEY_UP, R, 8600) == CS_KEYS_OTHER);
    assert(cs_keys_feed(&k, CS_KEY_UP, P, 8700) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_UP, R, 8790) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_DOWN, P, 8800) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_DOWN, R, 8850) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_DOWN, C, 9040) == CS_KEYS_OTHER);
    // Keys are tracked apart: a spent UP press does not touch OK's state.
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 9100) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 9150) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_UP, P, 9200) == CS_KEYS_PRESS);
    cs_keys_spend(&k);
    assert(cs_keys_feed(&k, CS_KEY_UP, L, 9700) == CS_KEYS_DROP);
    assert(cs_keys_feed(&k, CS_KEY_OK, C, 9710) == CS_KEYS_OTHER);
    assert(cs_keys_feed(&k, CS_KEY_UP, R, 9800) == CS_KEYS_DROP);
    assert(cs_keys_feed(&k, CS_KEY_UP, P, 9900) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_UP, R, 9950) == CS_KEYS_TAP);
    // A spent OK press (used on PRESS, or its screen went away): release, LONG,
    // CLICK all dropped, even with another key in between ...
    cs_keys_init(&k);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 9000) == CS_KEYS_PRESS);
    cs_keys_spend(&k);
    assert(cs_keys_feed(&k, CS_KEY_OK, L, 9500) == CS_KEYS_DROP);
    assert(cs_keys_feed(&k, CS_KEY_UP, P, 9550) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 9600) == CS_KEYS_DROP);
    assert(cs_keys_feed(&k, CS_KEY_OK, C, 9800) == CS_KEYS_DROP);
    cs_keys_init(&k);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 9900) == CS_KEYS_PRESS);
    cs_keys_spend(&k);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 9950) == CS_KEYS_DROP);
    // ... until the next OK press, which is live again.
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 11000) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 11100) == CS_KEYS_TAP);
    // A release or LONG without a seen PRESS (lost event) acts on nothing.
    cs_keys_init(&k);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 12000) == CS_KEYS_OTHER);
    assert(cs_keys_feed(&k, CS_KEY_OK, L, 12100) == CS_KEYS_OTHER);
    // Timestamp wrap-around.
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 0xFFFFFF00u) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 0x00000010u) == CS_KEYS_TAP);
    assert(cs_keys_feed(&k, CS_KEY_OK, P, 0xFFFFFF00u) == CS_KEYS_PRESS);
    assert(cs_keys_feed(&k, CS_KEY_OK, R, 0x00000100u) == CS_KEYS_LONG);
    // Out-of-range key ignored.
    assert(cs_keys_feed(&k, 7, P, 1) == CS_KEYS_DROP);
}

static void test_idle(void) {
    assert(CS_IDLE_S[CS_IDLE_DEFAULT] == 60 && CS_IDLE_S[CS_IDLE_CHOICES - 1] == 0);
    cs_idle_t t;
    cs_idle_init(&t, 60000, 1000);
    assert(cs_idle_tick(&t, 1000, false) == CS_SCREEN_ON);
    assert(cs_idle_tick(&t, 60999, false) == CS_SCREEN_ON);
    assert(cs_idle_tick(&t, 61000, false) == CS_SCREEN_DIM);      // 60 s idle: dim
    assert(cs_idle_tick(&t, 70999, false) == CS_SCREEN_DIM);
    assert(cs_idle_tick(&t, 71000, false) == CS_SCREEN_OFF);      // +10 s: off
    assert(cs_idle_tick(&t, 72000, true) == CS_SCREEN_OFF);       // off stays off, even with hold
    cs_idle_wake(&t, 80000);
    assert(cs_idle_tick(&t, 80000, false) == CS_SCREEN_ON);
    // A key while dimmed brightens and restarts the count.
    assert(cs_idle_tick(&t, 140000, false) == CS_SCREEN_DIM);
    cs_idle_wake(&t, 141000);
    assert(cs_idle_tick(&t, 200999, false) == CS_SCREEN_ON);
    // Hold (voice, alarm, release session) keeps it on and restarts the count,
    // also from dim.
    assert(cs_idle_tick(&t, 201000, false) == CS_SCREEN_DIM);
    assert(cs_idle_tick(&t, 205000, true) == CS_SCREEN_ON);
    assert(cs_idle_tick(&t, 264999, false) == CS_SCREEN_ON);
    assert(cs_idle_tick(&t, 265000, false) == CS_SCREEN_DIM);
    // Manual standby.
    cs_idle_off(&t);
    assert(cs_idle_tick(&t, 265001, false) == CS_SCREEN_OFF);
    // Never: always on.
    cs_idle_init(&t, 0, 0);
    assert(cs_idle_tick(&t, 0xFFFFFFF0u, false) == CS_SCREEN_ON);
    // Wrap-around of the millisecond clock.
    cs_idle_init(&t, 30000, 0xFFFFF000u);
    assert(cs_idle_tick(&t, 0x00005000u, false) == CS_SCREEN_ON);  // 24.6 s
    assert(cs_idle_tick(&t, 0x00007000u, false) == CS_SCREEN_DIM); // 32.8 s
}

static void test_home(void) {
    assert(CS_HOME_COUNT == 14 && CS_HOME_GRID_COUNT == 10);
    // Every icon live: plain reading order, wrapping.
    assert(cs_home_step(0, +1, CS_HOME_ALL) == 1);
    assert(cs_home_step(13, +1, CS_HOME_ALL) == 0);
    assert(cs_home_step(0, -1, CS_HOME_ALL) == 13);
    assert(cs_home_step(CS_HOME_SETTINGS, +1, CS_HOME_ALL) == CS_HOME_MINES);
    assert(cs_home_step(CS_HOME_MINES, +1, CS_HOME_ALL) == CS_HOME_CALL);    // grid -> dock in reading order
    assert(cs_home_step(5, +1, 0) == 6 && cs_home_step(0, -1, 0) == 13);    // no live icon: plain step
    int s = CS_HOME_RELEASE;
    for (int i = 0; i < CS_HOME_COUNT; i++) s = cs_home_step(s, +1, CS_HOME_ALL);
    assert(s == CS_HOME_RELEASE);
    // Placeholders skipped both ways; the dock's only app (Call) wraps to Messages.
    const uint32_t live = CS_HOME_ALL & ~CS_HOME_PLACEHOLDERS;
    assert(!(CS_HOME_PLACEHOLDERS >> CS_HOME_ASTRO & 1u) && CS_HOME_ASTRO == 2);   // Astro took the Photos slot
    assert(cs_home_step(CS_HOME_CALENDAR, +1, live) == CS_HOME_ASTRO);
    assert(cs_home_step(CS_HOME_ASTRO, +1, live) == CS_HOME_WEATHER);
    assert(cs_home_step(CS_HOME_WEATHER, -1, live) == CS_HOME_ASTRO);
    assert(cs_home_step(CS_HOME_ASTRO, -1, live) == CS_HOME_CALENDAR);
    assert(cs_home_step(CS_HOME_NOTES, +1, live) == CS_HOME_SETTINGS);      // over Camera
    assert(cs_home_step(CS_HOME_SETTINGS, -1, live) == CS_HOME_NOTES);
    assert(cs_home_step(CS_HOME_MINES, +1, live) == CS_HOME_CALL);
    assert(cs_home_step(CS_HOME_CALL, +1, live) == CS_HOME_MESSAGES);       // over Mail, Garden, Music
    assert(cs_home_step(CS_HOME_MESSAGES, -1, live) == CS_HOME_CALL);
    // A full lap visits each live icon once and only those.
    uint32_t seen = 0;
    s = CS_HOME_RELEASE;
    for (int i = 0; i < 10; i++) {
        s = cs_home_step(s, +1, live);
        assert(live >> s & 1u);
        assert(!(seen >> s & 1u));
        seen |= 1u << s;
    }
    assert(seen == live && s == CS_HOME_RELEASE);
    // Starting on a placeholder (not reachable, but safe): the next live one.
    assert(cs_home_step(CS_HOME_MAIL, +1, live) == CS_HOME_MESSAGES);
    assert(cs_home_step(CS_HOME_MAIL, -1, live) == CS_HOME_CALL);
}

static void test_pomo(void) {
    static const uint8_t expect[] = {1, 3, 5, 15, 30, 45, 60};
    assert(memcmp(CS_POMO_PRESET_MIN, expect, sizeof expect) == 0);
    assert(CS_POMO_PRESET_MIN[CS_POMO_DEFAULT_PRESET] == 15);
    char b[16];
    assert(cs_pomo_format(900, false, b, sizeof b) == 5 && strcmp(b, "15:00") == 0);
    cs_pomo_format(754, false, b, sizeof b);
    assert(strcmp(b, "12:34") == 0);
    cs_pomo_format(0, false, b, sizeof b);
    assert(strcmp(b, "00:00") == 0);
    cs_pomo_format(3600, true, b, sizeof b);
    assert(strcmp(b, "1:00:00") == 0);
    cs_pomo_format(3599, true, b, sizeof b);
    assert(strcmp(b, "0:59:59") == 0);
    cs_pomo_format(59, true, b, sizeof b);
    assert(strcmp(b, "0:00:59") == 0);

    cs_pomo_t p = {0};
    assert(cs_pomo_remaining_s(&p, 123) == 0);
    assert(cs_pomo_progress_permille(&p, 123) == 0);
    const int64_t t0 = 5000000000LL;            // esp_timer microseconds (not wall clock)
    cs_pomo_start(&p, t0, 15 * 60);
    assert(cs_pomo_remaining_s(&p, t0) == 900);
    assert(cs_pomo_remaining_s(&p, t0 + 1) == 900);          // rounds up: first second shows 15:00
    assert(cs_pomo_remaining_s(&p, t0 + 1000000) == 899);
    assert(cs_pomo_remaining_s(&p, t0 + 899999999) == 1);
    assert(cs_pomo_remaining_s(&p, t0 + 900000000) == 0);
    assert(cs_pomo_remaining_s(&p, t0 + 990000000) == 0);    // never negative
    assert(cs_pomo_remaining_us(&p, t0 + 300000000) == 600000000);
    assert(cs_pomo_progress_permille(&p, t0) == 0);
    assert(cs_pomo_progress_permille(&p, t0 + 450000000) == 500);
    assert(cs_pomo_progress_permille(&p, t0 + 2000000000LL) == 1000);
    assert(cs_pomo_progress_permille(&p, t0 - 5) == 0);
    cs_pomo_stop(&p);
    assert(cs_pomo_remaining_s(&p, t0) == 0);
}

static void test_backoff(void) {
    assert(cs_backoff_ms(0) == 5000);
    assert(cs_backoff_ms(1) == 10000);
    assert(cs_backoff_ms(2) == 20000);
    assert(cs_backoff_ms(3) == 40000);
    assert(cs_backoff_ms(4) == 60000);
    assert(cs_backoff_ms(5) == 60000);
    assert(cs_backoff_ms(1000) == 60000);
}

static const char WX[] =
    "{\"city\":\"上海\",\"temp\":17,\"hi\":19,\"lo\":13,\"cond\":\"多云\",\"icon\":\"cloudy\","
    "\"aqi\":62,\"aqi_level\":\"良\",\"updated\":\"22:55\",\"days\":[{\"d\":\"周日\",\"icon\":"
    "\"sunny\",\"hi\":20,\"lo\":12},{\"d\":\"周一\",\"icon\":\"cloudy\",\"hi\":18,\"lo\":13},"
    "{\"d\":\"周二\",\"icon\":\"rain\",\"hi\":16,\"lo\":12}]}";

static void test_weather(void) {
    cs_weather_t w;
    assert(cs_weather_parse(WX, strlen(WX), &w));
    assert(w.valid && w.temp == 17 && w.hi == 19 && w.lo == 13 && w.aqi == 62);
    assert(strcmp(w.city, "上海") == 0 && strcmp(w.cond, "多云") == 0);
    assert(strcmp(w.icon, "cloudy") == 0 && strcmp(w.aqi_level, "良") == 0);
    assert(strcmp(w.updated, "22:55") == 0);
    assert(w.ndays == 3);
    assert(strcmp(w.days[0].d, "周日") == 0 && strcmp(w.days[0].icon, "sunny") == 0);
    assert(w.days[0].hi == 20 && w.days[0].lo == 12);
    assert(strcmp(w.days[2].icon, "rain") == 0 && w.days[2].hi == 16);

    // Python json.dumps default (ensure_ascii) escapes CJK; key order differs; floats.
    const char *esc = "{ \"temp\": -2.6, \"city\": \"\\u5317\\u4eac\", \"aqi\": 180,"
                      " \"days\": [], \"extra\": {\"x\": [1, {\"y\": 2}]}, \"icon\": \"snow\" }";
    assert(cs_weather_parse(esc, strlen(esc), &w));
    assert(w.temp == -3 && strcmp(w.city, "北京") == 0 && w.ndays == 0);
    assert(w.hi == -3 && w.lo == -3);                       // defaults to temp
    assert(strcmp(w.aqi_level, "中度污染") == 0);            // derived from the number
    assert(cs_wx_icon_from_name(w.icon) == CS_WX_SNOW);

    // Nested "temp" inside another object must not be picked up at top level.
    const char *nested = "{\"x\":{\"temp\":5},\"city\":\"a\"}";
    assert(!cs_weather_parse(nested, strlen(nested), &w));
    assert(!cs_weather_parse("", 0, &w));
    assert(!cs_weather_parse("{\"temp\":", 8, &w));
    assert(!cs_weather_parse("[1,2]", 5, &w));
    // Long city truncated on a UTF-8 boundary.
    const char *longc = "{\"temp\":1,\"city\":\"上海上海上海上海上海上海上海上海上海\"}";
    assert(cs_weather_parse(longc, strlen(longc), &w));
    assert(strlen(w.city) == 21 && strncmp(w.city, "上海上海上海上", 21) == 0);

    assert(cs_wx_icon_from_name("sunny") == CS_WX_SUNNY);
    assert(cs_wx_icon_from_name("overcast") == CS_WX_OVERCAST);
    assert(cs_wx_icon_from_name("rain") == CS_WX_RAIN);
    assert(cs_wx_icon_from_name("fog") == CS_WX_FOG);
    assert(cs_wx_icon_from_name("night") == CS_WX_NIGHT);
    assert(cs_wx_icon_from_name("tornado") == CS_WX_CLOUDY);
    assert(cs_wx_icon_from_name(NULL) == CS_WX_CLOUDY);
}

static void test_aqi(void) {
    assert(cs_aqi_style("优", -1).bg == 0x4CB84C);          // green
    assert(cs_aqi_style("良", -1).bg == 0xF5D23C);          // yellow
    assert(cs_aqi_style("轻度污染", -1).bg == 0xF2922E);    // orange
    assert(cs_aqi_style("中度污染", -1).bg == 0xE0413A);    // red
    assert(cs_aqi_style("重度污染", -1).bg == 0x8E44AD);    // purple
    assert(cs_aqi_style("严重污染", -1).bg == 0x7E1F2A);    // maroon
    assert(cs_aqi_style("良", -1).text == 0x5A3C00);        // dark text on yellow
    assert(cs_aqi_style("", 30).bg == 0x4CB84C);
    assert(cs_aqi_style("??", 120).bg == 0xF2922E);
    assert(cs_aqi_style(NULL, 400).bg == 0x7E1F2A);
    assert(cs_aqi_style("", -1).bg == 0x9AA0A8);
    assert(strcmp(cs_aqi_level_for(50), "优") == 0);
    assert(strcmp(cs_aqi_level_for(51), "良") == 0);
    assert(strcmp(cs_aqi_level_for(300), "重度污染") == 0);
    assert(strcmp(cs_aqi_level_for(301), "严重污染") == 0);
}

static void test_json(void) {
    csj_tok_t t[16];
    const char *js = "{\"a\":[1,2,{\"b\":\"x\\\"y\"}],\"c\":true}";
    const int n = csj_parse(js, strlen(js), t, 16);
    assert(n == 10);
    assert(t[0].type == CSJ_OBJ && t[0].size == 2);
    const int a = csj_get(js, t, n, 0, "a");
    assert(a == 2 && t[a].type == CSJ_ARR && t[a].size == 3);
    assert(csj_next(t, n, a) == 8);
    char s[8];
    const int obj = 5;
    assert(t[obj].type == CSJ_OBJ && t[obj].size == 1);
    assert(csj_str(js, t, csj_get(js, t, n, obj, "b"), s, sizeof s) && strcmp(s, "x\"y") == 0);
    assert(csj_get(js, t, n, 0, "c") == 9);
    assert(csj_get(js, t, n, 0, "b") == -1);
    assert(csj_parse("{\"a\":1", 6, t, 16) < 0);
    assert(csj_parse("{\"a\":1]", 7, t, 16) < 0);
    assert(csj_parse("[1,2,3,4,5]", 11, t, 3) < 0);   // too many tokens
    char u[8];
    const char *sur = "[\"\\ud83d\\ude00\"]";             // surrogate pair -> 4-byte UTF-8
    assert(csj_parse(sur, strlen(sur), t, 16) == 2);
    assert(csj_str(sur, t, 1, u, sizeof u) && strcmp(u, "\xF0\x9F\x98\x80") == 0);
}

static void test_call_and_audio(void) {
    assert(cs_call_step(CS_CALL_TALK, +1) == CS_CALL_SEND);   // Send left of Delete
    assert(cs_call_step(CS_CALL_SPEAKER, +1) == CS_CALL_END);
    assert(cs_call_step(CS_CALL_END, +1) == CS_CALL_TALK);
    assert(cs_call_step(CS_CALL_TALK, -1) == CS_CALL_END);

    // Whole clip in one go vs. the Python reference.
    uint8_t out[sizeof CS_ADPCM_OUT];
    cs_adpcm_state_t st;
    cs_adpcm_init(&st);
    assert(cs_adpcm_encode(&st, CS_ADPCM_IN, 512, out) == 256);
    assert(memcmp(out, CS_ADPCM_OUT, sizeof out) == 0);
    assert(st.predictor == CS_ADPCM_END_PRED && st.index == CS_ADPCM_END_INDEX);

    // Two chunks: the second chunk's header carries the mid-clip state and that
    // chunk decodes on its own from the header alone.
    cs_adpcm_init(&st);
    cs_adpcm_encode(&st, CS_ADPCM_IN, 256, out);
    assert(st.predictor == CS_ADPCM_MID_PRED && st.index == CS_ADPCM_MID_INDEX);
    uint8_t h[CS_AUDIO_HDR_BYTES];
    cs_audio_hdr_pack(h, 0xA1B2C3D4u, 0x0102, CS_AUDIO_FLAG_LAST, &st);
    const uint8_t expect_head[8] = {0xD4, 0xC3, 0xB2, 0xA1, 0x02, 0x01, 0x01, 0x00};
    assert(memcmp(h, expect_head, 8) == 0);
    assert((int16_t)(h[8] | (h[9] << 8)) == CS_ADPCM_MID_PRED);
    assert(h[10] == CS_ADPCM_MID_INDEX && h[11] == 0);
    cs_adpcm_encode(&st, CS_ADPCM_IN + 256, 256, out + 128);
    assert(memcmp(out, CS_ADPCM_OUT, 256) == 0);
    cs_adpcm_state_t dec = {.predictor = (int16_t)(h[8] | (h[9] << 8)), .index = h[10]};
    int16_t pcm_a[256], pcm_b[256];
    cs_adpcm_decode(&dec, out + 128, 128, pcm_a);
    cs_adpcm_state_t dec_full;
    cs_adpcm_init(&dec_full);
    int16_t all[512];
    cs_adpcm_decode(&dec_full, out, 256, all);
    memcpy(pcm_b, all + 256, sizeof pcm_b);
    assert(memcmp(pcm_a, pcm_b, sizeof pcm_a) == 0);
    // Decoded signal tracks the input (sanity, outside the clipping runs).
    long err = 0;
    for (int i = 0; i < 280; i++) err += (all[i] > CS_ADPCM_IN[i] ? all[i] - CS_ADPCM_IN[i] : CS_ADPCM_IN[i] - all[i]);
    assert(err / 280 < 1500);

    // Negative header values (predictor is signed).
    cs_adpcm_state_t neg = {.predictor = -2, .index = 88};
    cs_audio_hdr_pack(h, 1, 0, 0, &neg);
    assert(h[8] == 0xFE && h[9] == 0xFF && h[10] == 88 && h[6] == 0);

    cs_play_cmd_t cmd;
    const char *play = "{\"op\":\"play\",\"url\":\"https://example.com/passport-audio/"
                       "0123456789abcdef0123456789abcdef0123456789abcdef\",\"id\":\"r1\","
                       "\"text\":\"\\u665a\\u5b89\"}";
    assert(cs_play_cmd_parse(play, strlen(play), &cmd));
    assert(strcmp(cmd.id, "r1") == 0 && strcmp(cmd.text, "晚安") == 0);
    assert(strncmp(cmd.url, "https://example.com/passport-audio/0123", 39) == 0);
    assert(!cs_play_cmd_parse("{\"op\":\"stop\"}", 13, &cmd));
    const char *http = "{\"op\":\"play\",\"url\":\"http://x/y\"}";
    assert(!cs_play_cmd_parse(http, strlen(http), &cmd));
    assert(cmd.fmt == CS_PLAY_PCM);
    // "fmt": absent / "pcm" -> PCM, "adpcm" -> ADPCM, anything else rejected.
    // "caption" stands in for a missing "text".
    assert(cs_play_cmd_parse(play, strlen(play), &cmd) && cmd.fmt == CS_PLAY_PCM);
    const char *ad = "{\"op\":\"play\",\"url\":\"https://h/a\",\"id\":\"r2\",\"caption\":\"hi\","
                     "\"fmt\":\"adpcm\"}";
    assert(cs_play_cmd_parse(ad, strlen(ad), &cmd));
    assert(cmd.fmt == CS_PLAY_ADPCM && strcmp(cmd.text, "hi") == 0 && strcmp(cmd.id, "r2") == 0);
    const char *pc = "{\"op\":\"play\",\"url\":\"https://h/a\",\"fmt\":\"pcm\",\"text\":\"t\",\"caption\":\"c\"}";
    assert(cs_play_cmd_parse(pc, strlen(pc), &cmd) && cmd.fmt == CS_PLAY_PCM && strcmp(cmd.text, "t") == 0);
    const char *bad = "{\"op\":\"play\",\"url\":\"https://h/a\",\"fmt\":\"opus\"}";
    assert(!cs_play_cmd_parse(bad, strlen(bad), &cmd));
    assert(cs_play_cmd_parse(play, strlen(play), &cmd) && cmd.via == CS_PLAY_HTTP);
    // via: mqtt needs no url, needs adpcm, an id of >= 8 chars and 1..65535 chunks.
    const char *mq = "{\"op\":\"play\",\"id\":\"0123456789ab\",\"text\":\"\u665a\u5b89\",\"fmt\":\"adpcm\","
                     "\"via\":\"mqtt\",\"chunks\":13,\"bytes\":46440}";
    assert(cs_play_cmd_parse(mq, strlen(mq), &cmd));
    assert(cmd.via == CS_PLAY_MQTT && cmd.fmt == CS_PLAY_ADPCM && cmd.chunks == 13 && cmd.bytes == 46440);
    assert(strcmp(cmd.id, "0123456789ab") == 0 && strcmp(cmd.text, "晚安") == 0 && cmd.url[0] == 0);
    const char *mq_pcm = "{\"op\":\"play\",\"id\":\"0123456789ab\",\"via\":\"mqtt\",\"chunks\":2}";
    assert(!cs_play_cmd_parse(mq_pcm, strlen(mq_pcm), &cmd));
    const char *mq_id = "{\"op\":\"play\",\"id\":\"0123\",\"fmt\":\"adpcm\",\"via\":\"mqtt\",\"chunks\":2}";
    assert(!cs_play_cmd_parse(mq_id, strlen(mq_id), &cmd));
    const char *mq_n0 = "{\"op\":\"play\",\"id\":\"0123456789ab\",\"fmt\":\"adpcm\",\"via\":\"mqtt\",\"chunks\":0}";
    assert(!cs_play_cmd_parse(mq_n0, strlen(mq_n0), &cmd));
    const char *mq_nn = "{\"op\":\"play\",\"id\":\"0123456789ab\",\"fmt\":\"adpcm\",\"via\":\"mqtt\"}";
    assert(!cs_play_cmd_parse(mq_nn, strlen(mq_nn), &cmd));
    const char *via_x = "{\"op\":\"play\",\"url\":\"https://h/a\",\"via\":\"ftp\"}";
    assert(!cs_play_cmd_parse(via_x, strlen(via_x), &cmd));
    const char *via_h = "{\"op\":\"play\",\"url\":\"https://h/a\",\"via\":\"http\"}";
    assert(cs_play_cmd_parse(via_h, strlen(via_h), &cmd) && cmd.via == CS_PLAY_HTTP);
}

static void chunk_hdr(uint8_t h[CS_CHUNK_HDR], const char *id, uint16_t seq, uint16_t total) {
    memcpy(h, id, 8);
    h[8] = (uint8_t)seq;
    h[9] = (uint8_t)(seq >> 8);
    h[10] = (uint8_t)total;
    h[11] = (uint8_t)(total >> 8);
}

static void test_playchunk(void) {
    cs_chunk_sess_t s;
    uint8_t h[CS_CHUNK_HDR];
    const size_t c3 = CS_CHUNK_HDR + 3 * CS_PLAY_BLOCK_BYTES, c7 = CS_CHUNK_HDR + 7 * CS_PLAY_BLOCK_BYTES;
    assert(c3 == 1560 && c7 == 3624);
    cs_chunk_sess_init(&s, "0123456789ab", 6);
    assert(!cs_chunk_complete(&s));
    chunk_hdr(h, "01234567", 0, 6);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == 0 && s.blocks == 3);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == -1);          // duplicate
    chunk_hdr(h, "deadbeef", 1, 6);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == -1);          // other clip
    chunk_hdr(h, "01234567", 1, 6);
    assert(cs_chunk_accept(&s, h, CS_CHUNK_HDR - 1, c3) == -1);  // too short
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == 0);
    chunk_hdr(h, "01234567", 3, 6);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == 3);           // seq 2 skipped: 3 blocks of silence
    chunk_hdr(h, "01234567", 2, 6);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == -1);          // late: older than expected
    chunk_hdr(h, "01234567", 6, 6);
    assert(cs_chunk_accept(&s, h, sizeof h, c3) == -1);          // beyond the clip
    // The short last chunk does not change the learned size: seq 4 skipped -> 3 blocks.
    chunk_hdr(h, "01234567", 5, 9);                              // header total is not trusted
    assert(cs_chunk_accept(&s, h, sizeof h, CS_CHUNK_HDR + 100) == 3 && s.blocks == 3);
    assert(cs_chunk_complete(&s));
    // 7-block chunks (fragmented on the wire: only the first fragment is checked).
    cs_chunk_sess_init(&s, "abcdefgh", 400);
    chunk_hdr(h, "abcdefgh", 0, 400);
    assert(cs_chunk_accept(&s, h, sizeof h, c7) == 0 && s.blocks == 7);
    chunk_hdr(h, "abcdefgh", 300, 400);                          // seq above 255
    assert(cs_chunk_accept(&s, h, sizeof h, c7) == 299 * 7 && s.next == 301);
    // A gap before any full chunk was seen uses the default size; a lone last
    // chunk with a skip before it too.
    cs_chunk_sess_init(&s, "abcdefgh", 2);
    chunk_hdr(h, "abcdefgh", 1, 2);
    assert(cs_chunk_accept(&s, h, sizeof h, CS_CHUNK_HDR + 600) == CS_CHUNK_BLOCKS_DEFAULT);
    assert(s.blocks == 0 && cs_chunk_complete(&s));
    // A gap reported by a full chunk is sized by that chunk.
    cs_chunk_sess_init(&s, "abcdefgh", 9);
    chunk_hdr(h, "abcdefgh", 2, 9);
    assert(cs_chunk_accept(&s, h, sizeof h, CS_CHUNK_HDR + 5 * CS_PLAY_BLOCK_BYTES) == 10);
    // msg_len shorter than what was handed over is malformed.
    chunk_hdr(h, "abcdefgh", 3, 9);
    assert(cs_chunk_accept(&s, h, sizeof h, CS_CHUNK_HDR - 2) == -1);

    // Block assembler: blocks come out whole whatever the slicing.
    static uint8_t body[3 * CS_PLAY_BLOCK_BYTES + 40];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 7 + 3);
    cs_block_asm_t a = {.have = 0};
    size_t pos = 0, blocks = 0, step = 1;
    while (pos < sizeof body) {
        size_t len = step < sizeof body - pos ? step : sizeof body - pos;
        step = step * 3 + 1;
        if (step > 900) step = 5;
        while (len) {
            const size_t n = cs_block_asm_add(&a, body + pos, len);
            pos += n;
            len -= n;
            if (a.have == CS_PLAY_BLOCK_BYTES) {
                assert(memcmp(a.buf, body + blocks * CS_PLAY_BLOCK_BYTES, CS_PLAY_BLOCK_BYTES) == 0);
                blocks++;
                a.have = 0;
            }
        }
    }
    assert(blocks == 3 && a.have == 40);
    assert(memcmp(a.buf, body + 3 * CS_PLAY_BLOCK_BYTES, 40) == 0);
    assert(cs_block_asm_add(&a, body, 0) == 0);
    // The all-zero block the device inserts for a missing chunk decodes to silence.
    static const uint8_t silent[CS_PLAY_BLOCK_BYTES];
    int16_t pcm[2 * CS_PLAY_BLOCK_DATA];
    cs_adpcm_stream_t d;
    cs_adpcm_stream_init(&d);
    size_t used;
    assert(cs_adpcm_stream_feed(&d, silent, sizeof silent, &used, pcm, 2 * CS_PLAY_BLOCK_DATA) ==
           2 * CS_PLAY_BLOCK_DATA);
    for (int i = 0; i < 2 * CS_PLAY_BLOCK_DATA; i++) assert(pcm[i] == 0);
}

// Downlink ADPCM blocks: encode a known signal block by block (header = encoder
// state before the block, as the gateway does), then decode the body sliced in
// awkward pieces; the output must equal the encoder's own reconstruction.
#define BLK_SAMPLES (CS_PLAY_BLOCK_DATA * 2)
#define DL_SAMPLES (2 * BLK_SAMPLES + 300)     // two full blocks + a short last one
static void test_downlink_adpcm(void) {
    static int16_t src[DL_SAMPLES], ref[DL_SAMPLES], got[DL_SAMPLES + 8];
    static uint8_t body[3 * (CS_PLAY_BLOCK_HDR + CS_PLAY_BLOCK_DATA)];
    // Deterministic test signal: two integer triangle waves plus an LCG hiss,
    // with full-scale bursts to drive the step index to the top.
    uint32_t lcg = 12345;
    for (int i = 0; i < DL_SAMPLES; i++) {
        lcg = lcg * 1103515245u + 12345u;
        const int a = (i * 37) % 400, b = (i * 5) % 1000;
        int v = (a < 200 ? a : 400 - a) * 90 - 9000 + (b < 500 ? b : 1000 - b) * 20 - 5000 +
                (int)((lcg >> 16) & 0x3FF) - 512;
        if (i % 700 > 650) v = (i & 8) ? 32767 : -32768;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        src[i] = (int16_t)v;
    }
    cs_adpcm_state_t enc;
    cs_adpcm_init(&enc);
    size_t blen = 0;
    int s = 0;
    while (s < DL_SAMPLES) {
        const int n = DL_SAMPLES - s < BLK_SAMPLES ? DL_SAMPLES - s : BLK_SAMPLES;
        body[blen++] = (uint8_t)enc.predictor;
        body[blen++] = (uint8_t)((uint16_t)enc.predictor >> 8);
        body[blen++] = enc.index;
        body[blen++] = 0;
        cs_adpcm_state_t probe = enc;          // the encoder's reconstruction of this block
        const size_t bytes = cs_adpcm_encode(&enc, src + s, (size_t)n, body + blen);
        cs_adpcm_decode(&probe, body + blen, bytes, ref + s);
        assert(probe.predictor == enc.predictor && probe.index == enc.index);
        blen += bytes;
        s += n;
    }
    assert(blen == 2 * (CS_PLAY_BLOCK_HDR + CS_PLAY_BLOCK_DATA) + CS_PLAY_BLOCK_HDR + 150);
    // Decode with odd slice sizes and a small output buffer.
    static const size_t SLICE[] = {1, 3, 160, 7, 516, 2, 61};
    static const size_t MAXS[] = {320, 50, 7, 1024};
    for (size_t m = 0; m < sizeof MAXS / sizeof MAXS[0]; m++) {
        cs_adpcm_stream_t d;
        cs_adpcm_stream_init(&d);
        size_t pos = 0, o = 0, k = 0;
        while (pos < blen) {
            size_t len = SLICE[k++ % (sizeof SLICE / sizeof SLICE[0])];
            if (len > blen - pos) len = blen - pos;
            size_t used = 0;
            while (used < len) {
                size_t u;
                const int g = cs_adpcm_stream_feed(&d, body + pos + used, len - used, &u, got + o, MAXS[m]);
                assert(g >= 0 && g <= (int)(MAXS[m] & ~(size_t)1));
                used += u;
                o += (size_t)g;
                assert(o <= DL_SAMPLES);
            }
            pos += len;
        }
        assert(o == DL_SAMPLES);
        assert(memcmp(got, ref, sizeof ref) == 0);
    }
    // The decoded signal tracks the source.
    long err = 0;
    for (int i = 0; i < DL_SAMPLES; i++) err += got[i] > src[i] ? got[i] - src[i] : src[i] - got[i];
    assert(err / DL_SAMPLES < 1500);
    // Blocks are independent: the second block alone decodes to the same samples.
    cs_adpcm_stream_t d;
    cs_adpcm_stream_init(&d);
    size_t u;
    const size_t blk = CS_PLAY_BLOCK_HDR + CS_PLAY_BLOCK_DATA;
    assert(cs_adpcm_stream_feed(&d, body + blk, blk, &u, got, BLK_SAMPLES) == BLK_SAMPLES && u == blk);
    assert(memcmp(got, ref + BLK_SAMPLES, BLK_SAMPLES * sizeof got[0]) == 0);
    // Tied to the Python reference vector: one block of CS_ADPCM_OUT from the zero state.
    static uint8_t one[CS_PLAY_BLOCK_HDR + sizeof CS_ADPCM_OUT];
    memset(one, 0, CS_PLAY_BLOCK_HDR);
    memcpy(one + CS_PLAY_BLOCK_HDR, CS_ADPCM_OUT, sizeof CS_ADPCM_OUT);
    cs_adpcm_state_t z;
    cs_adpcm_init(&z);
    static int16_t whole[2 * sizeof CS_ADPCM_OUT];
    cs_adpcm_decode(&z, CS_ADPCM_OUT, sizeof CS_ADPCM_OUT, whole);
    cs_adpcm_stream_init(&d);
    assert(cs_adpcm_stream_feed(&d, one, sizeof one, &u, got, DL_SAMPLES) == (int)(2 * sizeof CS_ADPCM_OUT));
    assert(memcmp(got, whole, sizeof whole) == 0);
    // Negative predictor in the header; corrupt step index stops the stream.
    const uint8_t neg[6] = {0xFE, 0xFF, 88, 0, 0x00, 0x00};
    cs_adpcm_stream_init(&d);
    assert(cs_adpcm_stream_feed(&d, neg, 4, &u, got, 2) == 0 && u == 4);
    assert(d.st.predictor == -2 && d.st.index == 88 && d.data_left == CS_PLAY_BLOCK_DATA);
    const uint8_t corrupt[5] = {0, 0, 89, 0, 0x77};
    cs_adpcm_stream_init(&d);
    assert(cs_adpcm_stream_feed(&d, corrupt, sizeof corrupt, &u, got, 16) == -1);
}

// RAM "flash": erase sets 0xFF, write may only clear bits (like NOR flash).
#define FL_SIZE (4 * CS_STORE_SECTOR)
static uint8_t s_fl[FL_SIZE];
static int s_fl_erases, s_fl_fail_write_at = -1;
static bool fl_read(void *c, uint32_t off, void *b, size_t n) {
    (void)c;
    assert(off + n <= FL_SIZE);
    memcpy(b, s_fl + off, n);
    return true;
}
static bool fl_write(void *c, uint32_t off, const void *b, size_t n) {
    (void)c;
    assert(off + n <= FL_SIZE && off / CS_STORE_SECTOR == (off + n - 1) / CS_STORE_SECTOR);
    const uint8_t *p = b;
    size_t k = n;
    if (s_fl_fail_write_at >= 0) {           // power loss: only part of the record lands
        k = (size_t)s_fl_fail_write_at;
        s_fl_fail_write_at = -1;
    }
    for (size_t i = 0; i < k; i++) {
        assert((s_fl[off + i] & p[i]) == p[i]);   // never set a bit without an erase
        s_fl[off + i] &= p[i];
    }
    return k == n;
}
static bool fl_erase(void *c, uint32_t off) {
    (void)c;
    assert(off % CS_STORE_SECTOR == 0 && off < FL_SIZE);
    memset(s_fl + off, 0xFF, CS_STORE_SECTOR);
    s_fl_erases++;
    return true;
}
static void store_open(cs_store_t *s) {
    memset(s, 0, sizeof *s);
    s->read = fl_read;
    s->write = fl_write;
    s->erase = fl_erase;
    s->size = FL_SIZE;
    assert(cs_store_open(s));
}
static void msg_make(cs_msg_t *m, int i, bool long_text) {
    memset(m, 0, sizeof *m);
    m->out = (uint8_t)(i & 1);
    m->dur_s = (uint16_t)i;
    m->ts = 1761900000u + (uint32_t)i;
    m->aux = 0xA5000000u + (uint32_t)i;
    if (long_text) {
        for (int k = 0; k < CS_MSG_TEXT_MAX; k++) m->text[k] = (char)('a' + (i + k) % 26);
    } else {
        snprintf(m->text, sizeof m->text, "msg %d", i);
    }
}

static void test_messages(void) {
    cs_msg_cmd_t c;
    const char *j = "{\"op\":\"msg\",\"id\":\"0123456789ab\",\"text\":\"Hi \\u732b!\\nSleep well\\t:)\",\"ts\":1761990000}";
    assert(cs_msg_cmd_parse(j, strlen(j), &c));
    assert(strcmp(c.text, "Hi ?! Sleep well :)") == 0);   // CJK -> '?', newline / tab -> space
    assert(strcmp(c.id, "0123456789ab") == 0 && c.ts == 1761990000u);
    assert(!cs_msg_cmd_parse("{\"op\":\"play\",\"text\":\"x\"}", 26, &c));
    assert(!cs_msg_cmd_parse("{\"op\":\"msg\"}", 12, &c));                 // text required
    static char big[1200];
    int o = snprintf(big, sizeof big, "{\"op\":\"msg\",\"text\":\"");
    for (int i = 0; i < 400; i++) big[o++] = (char)('A' + i % 26);
    o += snprintf(big + o, sizeof big - (size_t)o, "\"}");
    assert(cs_msg_cmd_parse(big, (size_t)o, &c) && strlen(c.text) == CS_MSG_TEXT_MAX && c.ts == 0);

    // Empty (erased) partition, and junk: both open empty.
    cs_store_t s;
    cs_msg_t m, g;
    memset(s_fl, 0xFF, sizeof s_fl);
    store_open(&s);
    assert(s.n == 0 && s.seq == 1 && s.wr == 0);
    memset(s_fl, 0x5A, sizeof s_fl);
    store_open(&s);
    assert(s.n == 0);
    // Add, read back, survive a reopen.
    for (int i = 0; i < 10; i++) {
        msg_make(&m, i, false);
        assert(cs_store_add(&s, &m));
    }
    assert(s.n == 10);
    store_open(&s);
    assert(s.n == 10 && s.seq == 11);
    for (int i = 0; i < 10; i++) {
        msg_make(&m, i, false);
        assert(cs_store_get(&s, i, &g));
        assert(strcmp(g.text, m.text) == 0 && g.out == m.out && g.dur_s == m.dur_s && g.ts == m.ts);
        assert(g.aux == m.aux);
    }
    // Many long messages: wraps the 4-sector log several times; keeps the newest 50.
    for (int i = 10; i < 200; i++) {
        msg_make(&m, i, true);
        assert(cs_store_add(&s, &m));
        assert(s.n <= CS_STORE_KEEP);
    }
    // 304-byte records, 13 per sector, 3 full sectors besides the one being written:
    // at least 39 survive; the index holds what is really there.
    assert(s.n >= 39 && s.n <= CS_STORE_KEEP);
    for (int i = 0; i < s.n; i++) {
        msg_make(&m, 200 - s.n + i, true);
        assert(cs_store_get(&s, i, &g) && strcmp(g.text, m.text) == 0);
    }
    const int kept = s.n;
    store_open(&s);
    assert(s.n == kept && s.seq == 201);
    msg_make(&m, 199, true);
    assert(cs_store_get(&s, s.n - 1, &g) && strcmp(g.text, m.text) == 0);
    // Short messages: exactly the newest 50 are kept.
    for (int i = 200; i < 300; i++) {
        msg_make(&m, i, false);
        assert(cs_store_add(&s, &m));
    }
    assert(s.n == CS_STORE_KEEP);
    store_open(&s);
    assert(s.n == CS_STORE_KEEP);
    for (int i = 0; i < CS_STORE_KEEP; i++) {
        msg_make(&m, 250 + i, false);
        assert(cs_store_get(&s, i, &g) && strcmp(g.text, m.text) == 0);
    }
    // Power loss mid-record: the torn record is ignored, the next write goes to
    // a fresh sector, nothing older is lost.
    msg_make(&m, 300, true);
    s_fl_fail_write_at = 40;
    assert(!cs_store_add(&s, &m));
    store_open(&s);
    assert(s.n == CS_STORE_KEEP);
    msg_make(&g, 299, false);
    cs_msg_t last;
    assert(cs_store_get(&s, s.n - 1, &last) && strcmp(last.text, g.text) == 0);
    assert(s.wr % CS_STORE_SECTOR == 0);
    msg_make(&m, 301, false);
    assert(cs_store_add(&s, &m));
    store_open(&s);
    assert(cs_store_get(&s, s.n - 1, &g) && strcmp(g.text, "msg 301") == 0);
    // A bad checksum also ends that sector's scan.
    const uint32_t at = s.idx[s.n - 1];
    s_fl[at + 20] ^= 0x01;                   // flip a text bit of the newest record
    store_open(&s);
    assert(cs_store_get(&s, s.n - 1, &g) && strcmp(g.text, "msg 299") == 0);
}

static void test_memos(void) {
    cs_memo_cmd_t c;
    const char *j = "{\"op\":\"memo\",\"clip\":4012345678,\"text\":\"Buy oat milk\\u2014and eggs\"}";
    assert(cs_memo_cmd_parse(j, strlen(j), &c));
    assert(c.clip == 4012345678u && strcmp(c.text, "Buy oat milk?and eggs") == 0);
    assert(!cs_memo_cmd_parse("{\"op\":\"memo\",\"text\":\"x\"}", 27, &c));             // clip required
    const char *neg = "{\"op\":\"memo\",\"clip\":-3,\"text\":\"x\"}";
    assert(!cs_memo_cmd_parse(neg, strlen(neg), &c));
    const char *msg = "{\"op\":\"msg\",\"clip\":1,\"text\":\"x\"}";
    assert(!cs_memo_cmd_parse(msg, strlen(msg), &c));
    static char big[700];
    int o = snprintf(big, sizeof big, "{\"op\":\"memo\",\"clip\":7,\"text\":\"");
    for (int i = 0; i < 300; i++) big[o++] = 'm';
    o += snprintf(big + o, sizeof big - (size_t)o, "\"}");
    assert(cs_memo_cmd_parse(big, (size_t)o, &c) && strlen(c.text) == CS_MEMO_TEXT_MAX);

    // Expiry. 2025-11-01 10:00 CST = 02:00 UTC.
    const uint32_t t = 1761962400u;
    assert(cs_memo_visible(0, t, 0, 0));                                  // clock unknown: keep
    assert(cs_memo_visible(0, t, 0, t + CS_MEMO_TTL_S - 1));              // unchecked: a week
    assert(!cs_memo_visible(0, t, 0, t + CS_MEMO_TTL_S));
    assert(cs_memo_visible(0, 0, 0, t + 100u * 86400u));                  // created unknown: keep
    // Checked at 10:00: still there at 23:59 CST, gone at 00:00 CST the next day.
    assert(cs_memo_visible(CS_MEMO_DONE, t - 3600, t, t + 14 * 3600 - 1));
    assert(!cs_memo_visible(CS_MEMO_DONE, t - 3600, t, t + 14 * 3600));
    // Checked at 23:30 CST: gone half an hour later (the next calendar day).
    const uint32_t late = t + 13 * 3600 + 1800;
    assert(cs_memo_visible(CS_MEMO_DONE, t, late, late + 1799));
    assert(!cs_memo_visible(CS_MEMO_DONE, t, late, late + 1800));
    assert(cs_memo_visible(CS_MEMO_DONE, t, 0, t + 30u * 86400u));        // checked time unknown: keep
}

static void hex32(const uint8_t d[32], char *out) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_sha256(void) {
    cs_sha256_t c;
    uint8_t d[32];
    char h[65];
    cs_sha256_init(&c);
    cs_sha256_final(&c, d);
    hex32(d, h);
    assert(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    cs_sha256_init(&c);
    cs_sha256_update(&c, "abc", 3);
    cs_sha256_final(&c, d);
    hex32(d, h);
    assert(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    // Two blocks, fed in odd slices.
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    cs_sha256_init(&c);
    for (size_t i = 0; i < strlen(m); i += 7) cs_sha256_update(&c, m + i, strlen(m) - i < 7 ? strlen(m) - i : 7);
    cs_sha256_final(&c, d);
    hex32(d, h);
    assert(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    // Exactly 55 / 56 / 64 bytes (padding edges): 'a' repeated.
    static const struct { int n; const char *h; } A[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    };
    for (size_t k = 0; k < sizeof A / sizeof A[0]; k++) {
        char buf[64];
        memset(buf, 'a', sizeof buf);
        cs_sha256_init(&c);
        cs_sha256_update(&c, buf, (size_t)A[k].n);
        cs_sha256_final(&c, d);
        hex32(d, h);
        assert(strcmp(h, A[k].h) == 0);
    }
}

static void test_mines(void) {
    char b[8];
    // Names and parsing: rows A.., columns 1...
    assert(cs_mines_cell_name(0, 4, b, sizeof b) && strcmp(b, "A1") == 0);
    assert(cs_mines_cell_name(5, 4, b, sizeof b) && strcmp(b, "B2") == 0);
    assert(cs_mines_cell_name(24, 5, b, sizeof b) && strcmp(b, "E5") == 0);
    assert(!cs_mines_cell_name(16, 4, b, sizeof b) && !cs_mines_cell_name(-1, 4, b, sizeof b));
    assert(cs_mines_cell_parse("B2", 4) == 5 && cs_mines_cell_parse(" c3 ", 3) == 8);
    assert(cs_mines_cell_parse("D1", 3) == -1 && cs_mines_cell_parse("A4", 3) == -1);
    assert(cs_mines_cell_parse("A", 3) == -1 && cs_mines_cell_parse("A12", 5) == -1 && cs_mines_cell_parse("", 3) == -1);
    // Cursor wraps both ways (16 cells + 2 buttons).
    assert(cs_mines_cursor_step(17, +1, 18) == 0 && cs_mines_cursor_step(0, -1, 18) == 17);
    assert(cs_mines_cursor_step(4, +1, 18) == 5 && cs_mines_cursor_step(5, -1, 18) == 4);

    // Layout strings.
    uint32_t mask;
    assert(cs_mines_layout_parse("A2,C1", 3, &mask) == 2 && mask == (1u << 1 | 1u << 6));
    assert(cs_mines_layout_parse("a2, c1", 3, &mask) == 2);
    assert(cs_mines_layout_parse("A2,A2", 3, &mask) == -1);
    assert(cs_mines_layout_parse("A2,,C1", 3, &mask) == -1);
    assert(cs_mines_layout_parse("A2,D1", 3, &mask) == -1);
    assert(cs_mines_layout_parse("", 3, &mask) == 0);

    // Commit = first 16 hex of sha256(layout + salt) (matches Python hashlib).
    char cm[17];
    cs_mines_commit_of("A2,C1", "5f1c", cm);
    assert(strcmp(cm, "c2f0952c64b081da") == 0);

    // A 4x4 game, owner first. Placing: at most 3 mines, toggling off works.
    cs_mines_t g;
    cs_mines_new(&g, CS_MINES_DEFAULT_SIZE, false, 77);
    assert(g.n == 4 && g.m == 3 && g.phase == CS_MINES_PLACE && g.pending == -1 && g.boom == -1);
    assert(!cs_mines_go(&g));                                  // none placed yet
    assert(cs_mines_toggle(&g, 0) && cs_mines_toggle(&g, 5) && cs_mines_toggle(&g, 10));
    assert(!cs_mines_toggle(&g, 15));                          // a fourth is refused
    assert(cs_mines_toggle(&g, 10) && cs_mines_toggle(&g, 15));// move one
    assert(g.mines == (1u << 0 | 1u << 5 | 1u << 15));
    assert(!cs_mines_step(&g, 1));                             // not playing yet
    assert(cs_mines_companion_step(&g, 1) == -1);
    assert(cs_mines_go(&g) && g.phase == CS_MINES_READY);      // no commit yet
    assert(!cs_mines_toggle(&g, 1));                           // mines are fixed now
    assert(!cs_mines_commit(&g, "12345"));                     // too short
    assert(!cs_mines_commit(&g, "0123456789abcdeg"));          // not hex
    assert(cs_mines_commit(&g, "0123456789ABCDEF") && strcmp(g.commit, "0123456789abcdef") == 0);
    assert(g.phase == CS_MINES_MY_TURN);
    assert(!cs_mines_commit(&g, "ffffffffffffffff"));          // never replaced once playing
    // Her step: wait for the answer; a stray / wrong-cell answer is ignored.
    assert(!cs_mines_result(&g, 2, false));
    assert(cs_mines_step(&g, 2) && g.phase == CS_MINES_MY_WAIT && g.pending == 2);
    assert(!cs_mines_step(&g, 3));
    assert(cs_mines_companion_step(&g, 1) == -1);              // not the companion's turn
    assert(!cs_mines_result(&g, 3, false));
    cs_mines_unwait(&g);                                       // timeout: step again
    assert(g.phase == CS_MINES_MY_TURN && g.pending == -1);
    assert(cs_mines_step(&g, 2) && cs_mines_result(&g, 2, false));
    assert(g.phase == CS_MINES_COMPANION_TURN && g.my_steps == 1u << 2);
    assert(!cs_mines_result(&g, 2, false));                    // a re-delivery changes nothing
    assert(!cs_mines_step(&g, 3));                             // the companion's turn
    // The companion steps: judged on the device.
    assert(cs_mines_companion_step(&g, 16) == -1);             // off the field
    assert(cs_mines_companion_step(&g, 1) == 0 && g.phase == CS_MINES_MY_TURN);
    assert(cs_mines_companion_step(&g, 1) == -2);              // re-delivery: answer again
    assert(!cs_mines_step(&g, 2));                             // already open
    assert(cs_mines_step(&g, 3) && cs_mines_result(&g, 3, false) && g.phase == CS_MINES_COMPANION_TURN);
    assert(cs_mines_companion_step(&g, 5) == 1);               // her mine: the companion loses
    assert(g.phase == CS_MINES_OVER && g.winner == CS_MINES_OWNER_WON && g.boom == 5);
    assert(cs_mines_companion_step(&g, 5) == -2 && cs_mines_companion_step(&g, 6) == -1);

    // Reveal: hash and consistency with the answers she got (safe at A3, A4).
    cs_mines_t r = g;
    cs_mines_commit_of("B1,C3,D4", "aa", r.commit);
    assert(cs_mines_reveal(&r, "B1,C3,D4", "aa") && r.reveal == CS_MINES_VERIFIED);
    assert(r.companion_mines == (1u << 4 | 1u << 10 | 1u << 15));
    r = g;
    cs_mines_commit_of("B1,C3,D4", "aa", r.commit);
    assert(cs_mines_reveal(&r, "B1,C3,D4", "ab") && r.reveal == CS_MINES_MISMATCH);   // other salt
    r = g;
    cs_mines_commit_of("A3,C3,D4", "aa", r.commit);                // hash fine, but A3 was "safe"
    assert(cs_mines_reveal(&r, "A3,C3,D4", "aa") && r.reveal == CS_MINES_MISMATCH);
    r = g;
    cs_mines_commit_of("B1,C3", "aa", r.commit);                   // too few mines
    assert(cs_mines_reveal(&r, "B1,C3", "aa") && r.reveal == CS_MINES_MISMATCH);
    cs_mines_t early;
    cs_mines_new(&early, 0, false, 1);
    assert(!cs_mines_reveal(&early, "A1,A2", "x"));                // only at the end

    // 3x3, the companion first; commit arrives while she is still placing. She steps on
    // a mine -> the companion won; the reveal must hold that cell.
    cs_mines_new(&g, 0, true, 9);
    assert(g.n == 3 && g.m == 2);
    assert(cs_mines_commit(&g, "00000000000000aa") && g.phase == CS_MINES_PLACE);
    assert(cs_mines_toggle(&g, 4) && cs_mines_toggle(&g, 8) && cs_mines_go(&g));
    assert(g.phase == CS_MINES_COMPANION_TURN);
    assert(cs_mines_companion_step(&g, 0) == 0 && g.phase == CS_MINES_MY_TURN);
    assert(cs_mines_step(&g, 1) && cs_mines_result(&g, 1, true));
    assert(g.phase == CS_MINES_OVER && g.winner == CS_MINES_COMPANION_WON && g.boom == 1);
    r = g;
    cs_mines_commit_of("A2,C3", "s", r.commit);
    assert(cs_mines_reveal(&r, "A2,C3", "s") && r.reveal == CS_MINES_VERIFIED);
    r = g;
    cs_mines_commit_of("A3,C3", "s", r.commit);                    // her fatal A2 not a mine
    assert(cs_mines_reveal(&r, "A3,C3", "s") && r.reveal == CS_MINES_MISMATCH);

    // Clearing wins: the companion clears her 7 safe cells of a 3x3.
    cs_mines_new(&g, 0, true, 10);
    assert(cs_mines_toggle(&g, 0) && cs_mines_toggle(&g, 1) && cs_mines_commit(&g, "1111111111111111"));
    assert(cs_mines_go(&g) && g.phase == CS_MINES_COMPANION_TURN);
    int her_safe = 0;
    for (int c = 2; c < 9; c++) {
        assert(g.phase == CS_MINES_COMPANION_TURN);
        assert(cs_mines_companion_step(&g, c) == 0);
        if (g.phase == CS_MINES_OVER) break;
        assert(cs_mines_step(&g, her_safe + 2) && cs_mines_result(&g, her_safe + 2, false));
        her_safe++;
    }
    assert(g.phase == CS_MINES_OVER && g.winner == CS_MINES_COMPANION_WON && g.boom == -1);
    assert(cs_mines_count(g.companion_steps) == 7 && her_safe == 6);
    // ...and she wins by clearing the companion's 7.
    cs_mines_new(&g, 0, false, 11);
    assert(cs_mines_toggle(&g, 0) && cs_mines_toggle(&g, 1) && cs_mines_commit(&g, "1111111111111111"));
    assert(cs_mines_go(&g));
    for (int c = 0; c < 7; c++) {
        assert(cs_mines_step(&g, c) && cs_mines_result(&g, c, false));
        if (g.phase == CS_MINES_OVER) break;
        assert(cs_mines_companion_step(&g, c + 2) == 0);
    }
    assert(g.winner == CS_MINES_OWNER_WON && cs_mines_count(g.my_steps) == 7);

    // Her layout as a string, and when it may leave the device.
    char ls[24];
    assert(cs_mines_layout_str(1u << 15 | 1u << 0 | 1u << 5, 4, ls, sizeof ls) && strcmp(ls, "A1,B2,D4") == 0);
    assert(cs_mines_layout_str(0, 4, ls, sizeof ls) && ls[0] == '\0');
    assert(!cs_mines_layout_str(1u << 0 | 1u << 1 | 1u << 2, 3, ls, 6) && strcmp(ls, "A1,A2") == 0);   // too small: cut at a cell
    assert(cs_mines_may_reveal_mine(&g));                          // over
    cs_mines_t q;
    cs_mines_new(&q, 0, false, 12);
    assert(!cs_mines_may_reveal_mine(&q));                         // placing
    assert(cs_mines_toggle(&q, 0) && cs_mines_toggle(&q, 1) && cs_mines_go(&q));
    assert(!cs_mines_may_reveal_mine(&q));                         // ready, no commit
    assert(cs_mines_commit(&q, "1111111111111111") && q.phase == CS_MINES_MY_TURN);
    assert(!cs_mines_may_reveal_mine(&q));                         // playing, no step yet: quit hides it
    assert(cs_mines_step(&q, 4));
    assert(cs_mines_may_reveal_mine(&q));                          // a step is out
    cs_mines_unwait(&q);
    assert(!cs_mines_may_reveal_mine(&q));
    assert(cs_mines_step(&q, 4) && cs_mines_result(&q, 4, false));
    assert(cs_mines_may_reveal_mine(&q));                          // one move made

    // Commands.
    cs_mines_cmd_t mc;
    const char *j1 = "{\"op\":\"mine_commit\",\"game\":3000000000,\"commit\":\"0123456789abcdef\"}";
    assert(cs_mines_cmd_parse(j1, strlen(j1), &mc) && mc.op == CS_MCMD_COMMIT && mc.game == 3000000000u);
    assert(strcmp(mc.commit, "0123456789abcdef") == 0);
    const char *j1b = "{\"op\":\"mine_commit\",\"game\":3,\"commit\":\"0123456789abcdef0\"}";
    assert(!cs_mines_cmd_parse(j1b, strlen(j1b), &mc));
    const char *j2 = "{\"op\":\"mine_result\",\"game\":3,\"cell\":\"B2\",\"boom\":true}";
    assert(cs_mines_cmd_parse(j2, strlen(j2), &mc) && mc.op == CS_MCMD_RESULT && mc.boom && strcmp(mc.cell, "B2") == 0);
    const char *j2b = "{\"op\":\"mine_result\",\"game\":3,\"cell\":\"B2\",\"boom\":\"no\"}";
    assert(!cs_mines_cmd_parse(j2b, strlen(j2b), &mc));
    const char *j2c = "{\"op\":\"mine_result\",\"game\":3,\"cell\":\"B2\",\"boom\":false}";
    assert(cs_mines_cmd_parse(j2c, strlen(j2c), &mc) && !mc.boom);
    const char *j3 = "{\"op\":\"mine_companion_step\",\"game\":3,\"cell\":\"C3\",\"note\":\"Corners\\u2026 C3.\"}";
    assert(cs_mines_cmd_parse(j3, strlen(j3), &mc) && mc.op == CS_MCMD_COMPANION_STEP);
    assert(strcmp(mc.cell, "C3") == 0 && strcmp(mc.note, "Corners? C3.") == 0);
    const char *j3b = "{\"op\":\"mine_companion_step\",\"game\":3,\"cell\":\"C3\"}"; // note optional
    assert(cs_mines_cmd_parse(j3b, strlen(j3b), &mc) && mc.note[0] == '\0');
    static char big[400];
    int o = snprintf(big, sizeof big, "{\"op\":\"mine_companion_step\",\"game\":3,\"cell\":\"A1\",\"note\":\"");
    for (int i = 0; i < 90; i++) big[o++] = 'x';
    o += snprintf(big + o, sizeof big - (size_t)o, "\"}");
    assert(cs_mines_cmd_parse(big, (size_t)o, &mc) && strlen(mc.note) == CS_MINES_NOTE_MAX);
    const char *j4 = "{\"op\":\"mine_reveal\",\"game\":3,\"layout\":\"A2,C1\",\"salt\":\"5f1c\"}";
    assert(cs_mines_cmd_parse(j4, strlen(j4), &mc) && mc.op == CS_MCMD_REVEAL);
    assert(strcmp(mc.layout, "A2,C1") == 0 && strcmp(mc.salt, "5f1c") == 0);
    const char *j5 = "{\"op\":\"mine_reveal\",\"layout\":\"A2,C1\",\"salt\":\"5f1c\"}";   // game required
    assert(!cs_mines_cmd_parse(j5, strlen(j5), &mc));
    const char *j6 = "{\"op\":\"msg\",\"game\":3,\"text\":\"hi\"}";
    assert(!cs_mines_cmd_parse(j6, strlen(j6), &mc));
}

static void bingo_cmd(cs_bingo_cmd_t *m, const char *json) {
    assert(cs_bingo_cmd_parse(json, strlen(json), m));
}

static void test_bingo(void) {
    char day[16];
    // Card day: 06:00 Beijing (22:00 UTC the day before) is the boundary.
    cs_bingo_card_day(1793570399u, day);   // 2026-11-02 05:59:59 +08:00
    assert(strcmp(day, "2026-11-01") == 0);
    cs_bingo_card_day(1793570400u, day);   // 2026-11-02 06:00:00 +08:00
    assert(strcmp(day, "2026-11-02") == 0);
    cs_bingo_card_day(1793631599u, day);   // 2026-11-02 22:59:59 +08:00
    assert(strcmp(day, "2026-11-02") == 0);
    cs_bingo_card_day(1793642400u, day);   // 2026-11-03 02:00 +08:00: still the 2nd's card
    assert(strcmp(day, "2026-11-02") == 0);
    cs_bingo_card_day(1803852000u, day);   // 2027-03-01 06:00 +08:00 (after Feb 28)
    assert(strcmp(day, "2027-03-01") == 0);
    cs_bingo_card_day(1803851999u, day);
    assert(strcmp(day, "2027-02-28") == 0);
    cs_bingo_card_day(1830204000u, day);   // 2027-12-31 06:00 +08:00 -> 2028 leap year next
    assert(strcmp(day, "2027-12-31") == 0);

    // Lines on 4x4: rows, columns, both diagonals.
    uint32_t cells = 0;
    char name[12];
    assert(cs_bingo_lines(0, 4, &cells) == 0 && cells == 0);
    assert(!cs_bingo_first_line(0x0E77u, 4, name, sizeof name, &cells) && cs_bingo_lines(0x0E77u, 4, NULL) == 0);   // 9 cells, no line
    assert(cs_bingo_lines(0x00F0u, 4, &cells) == 1 && cells == 0x00F0u);
    assert(cs_bingo_first_line(0x00F0u, 4, name, sizeof name, &cells) && strcmp(name, "row 2") == 0);
    assert(cs_bingo_lines(0x2222u, 4, NULL) == 1);
    assert(cs_bingo_first_line(0x2222u, 4, name, sizeof name, &cells) && strcmp(name, "col 2") == 0 &&
           cells == 0x2222u);
    assert(cs_bingo_lines(0x8421u, 4, NULL) == 1);                    // 0, 5, 10, 15
    assert(cs_bingo_first_line(0x8421u, 4, name, sizeof name, NULL) && strcmp(name, "diag") == 0);
    assert(cs_bingo_lines(0x1248u, 4, NULL) == 1);                    // 3, 6, 9, 12
    assert(cs_bingo_first_line(0x1248u, 4, name, sizeof name, NULL) && strcmp(name, "anti") == 0);
    assert(cs_bingo_lines(0x00F0u | 0x2222u, 4, &cells) == 2 && cells == (0x00F0u | 0x2222u));
    assert(cs_bingo_lines(0xFFFFu, 4, NULL) == 10);
    assert(cs_bingo_lines(0x000Fu, 5, NULL) == 0);                    // 4 of a 5-cell row
    // 5x5: row 5 (cells 20..24), column 5 (4, 9, 14, 19, 24), both diagonals.
    assert(cs_bingo_lines(0x1F00000u, 5, NULL) == 1);
    assert(cs_bingo_first_line(0x1F00000u, 5, name, sizeof name, NULL) && strcmp(name, "row 5") == 0);
    assert(cs_bingo_first_line(0x1084210u, 5, name, sizeof name, NULL) && strcmp(name, "col 5") == 0);
    assert(cs_bingo_lines(0x1041041u, 5, NULL) == 1);                 // 0, 6, 12, 18, 24
    assert(cs_bingo_first_line(0x0111110u, 5, name, sizeof name, NULL) && strcmp(name, "anti") == 0);
    assert(cs_bingo_lines(0x1FFFFFFu, 5, NULL) == 12);
    assert(cs_bingo_lines(0x1FFFFFFu, 5, NULL) == 12);
    assert(cs_bingo_lines(0xFFFFu, 3, NULL) == 8);                    // any side 2..5 works

    // Card command.
    static cs_bingo_cmd_t m;
    static const char C16[] =
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"MAG,UP<10,MEALS,BED<1,5 CUPS,VEG,"
        "NO ICE,BATH,SUN,WALK,READ,TIDY,LESS SCR,GAIN BK,FAM,DIARY\",\"done\":3,\"auto\":2,\"fixed\":15,"
        "\"ack\":4,\"bingo\":false,\"whisper\":\"secret\"}";
    bingo_cmd(&m, C16);
    assert(m.n == 4 && m.done == 3 && m.autod == 2 && m.fixed == 15 && m.ack == 4 && !m.bingo);
    assert(strcmp(m.cells[1], "UP<10") == 0 && strcmp(m.cells[12], "LESS SCR") == 0 && strcmp(m.cells[15], "DIARY") == 0);
    assert(m.whisper[0] == '\0');                                     // only once bingo is true
    const char *bad[] = {
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C\",\"done\":0}",   // too few
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,A\",\"done\":0}",  // duplicate
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q\",\"done\":0}",  // too many
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,TOOLONG12\",\"done\":0}",
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,\",\"done\":0}",  // empty
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":3,\"cells\":\"A,B,C,D,E,F,G,H,I\",\"done\":0}",  // size
        "{\"op\":\"bingo_card\",\"day\":\"11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P\",\"done\":0}",
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P\"}",  // no done
        "{\"op\":\"mine_commit\",\"game\":1,\"commit\":\"0123456789abcdef\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(!cs_bingo_cmd_parse(bad[i], strlen(bad[i]), &m));

    // Cache: first card, her offline ticks win until acknowledged.
    static cs_bingo_card_t c;
    memset(&c, 0, sizeof c);
    bingo_cmd(&m, C16);
    assert(cs_bingo_merge(&c, &m));
    assert(strcmp(c.day, "2026-11-02") == 0 && c.done == 3 && c.seq == 4 && cs_bingo_done_count(&c) == 2);
    assert(cs_bingo_toggle(&c, 2) == 5 && cs_bingo_toggle(&c, 0) == 6);   // tick MEALS, untick MAG
    assert(c.done == 6 && c.pending == 5 && c.pend_seq[2] == 5 && c.pend_seq[0] == 6);
    assert(cs_bingo_toggle(&c, 16) == 0 && cs_bingo_toggle(&c, -1) == 0);
    // An older card (ack 4, an auto tick of cell 3) arrives: her two changes stay.
    static const char C16b[] =
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"MAG,UP<10,MEALS,BED<1,5 CUPS,VEG,"
        "NO ICE,BATH,SUN,WALK,READ,TIDY,LESS SCR,GAIN BK,FAM,DIARY\",\"done\":11,\"auto\":10,\"fixed\":15,\"ack\":4}";
    bingo_cmd(&m, C16b);
    assert(!cs_bingo_merge(&c, &m));
    assert(c.done == (2u | 4u | 8u) && c.pending == 5 && c.seq == 6);
    // ack 5: MEALS is in; MAG still hers (pending), even though the server says ticked.
    m.ack = 5;
    m.done = 1u | 2u | 4u | 8u;
    assert(!cs_bingo_merge(&c, &m));
    assert(c.done == (2u | 4u | 8u) && c.pending == 1 && c.pend_seq[2] == 0);
    m.ack = 6;
    m.done = 2u | 4u | 8u;
    assert(!cs_bingo_merge(&c, &m) && c.pending == 0 && c.done == 14);
    // A swap of cell 9 (WALK -> SWIM) while cell 9 had a pending change: that change goes.
    assert(cs_bingo_toggle(&c, 9) == 7 && cs_bingo_toggle(&c, 10) == 8);
    snprintf(m.cells[9], sizeof m.cells[9], "SWIM");
    assert(cs_bingo_merge(&c, &m));
    assert(c.pending == (1u << 10) && !(c.done >> 9 & 1u) && (c.done >> 10 & 1u) && strcmp(c.cells[9], "SWIM") == 0);
    // Lines: row 1 complete with the server's view plus her pending tick.
    c.celebrated = 1;
    c.lines_seen = 1;
    m.ack = 8;
    m.done = 0;
    assert(!cs_bingo_merge(&c, &m));
    assert(c.done == 0 && c.lines_seen == 0);                         // unticked by the server: no line left
    // Bingo with the whisper; a later card without the whisper keeps it.
    static const char C16w[] =
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":4,\"cells\":\"MAG,UP<10,MEALS,BED<1,5 CUPS,VEG,"
        "NO ICE,BATH,SUN,SWIM,READ,TIDY,LESS SCR,GAIN BK,FAM,DIARY\",\"done\":15,\"ack\":8,\"bingo\":true,"
        "\"whisper\":\"You did it, \\u732b! Proud of you.\"}";
    bingo_cmd(&m, C16w);
    assert(m.bingo && strcmp(m.whisper, "You did it, ?! Proud of you.") == 0);
    assert(!cs_bingo_merge(&c, &m) && c.bingo && strcmp(c.whisper, "You did it, ?! Proud of you.") == 0);
    m.whisper[0] = '\0';
    assert(!cs_bingo_merge(&c, &m) && c.whisper[0] != '\0');
    // A new size the same day: every pending change drops, labels replaced.
    assert(cs_bingo_toggle(&c, 5) == 9);
    static const char C25[] =
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-02\",\"n\":5,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,"
        "U,V,W,X,MAG\",\"done\":16777216,\"ack\":8}";
    bingo_cmd(&m, C25);
    assert(cs_bingo_merge(&c, &m));
    assert(c.n == 5 && c.pending == 0 && c.done == (1u << 24) && c.seq == 9 && c.celebrated == 1 && c.bingo);
    // The next day replaces everything; the change counter never goes back.
    static const char D2[] =
        "{\"op\":\"bingo_card\",\"day\":\"2026-11-03\",\"n\":4,\"cells\":\"A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P\","
        "\"done\":0,\"ack\":3}";
    assert(cs_bingo_toggle(&c, 1) == 10);
    bingo_cmd(&m, D2);
    assert(cs_bingo_merge(&c, &m));
    assert(c.n == 4 && c.pending == 0 && c.done == 0 && c.seq == 10 && !c.celebrated && !c.bingo &&
           c.whisper[0] == '\0' && strcmp(c.day, "2026-11-03") == 0);
    // A device whose cache was wiped learns the counter from the server.
    memset(&c, 0, sizeof c);
    m.ack = 40;
    assert(cs_bingo_merge(&c, &m) && c.seq == 40 && cs_bingo_toggle(&c, 0) == 41);
}

static void test_astro(void) {
    // Name tables: 12 each, none empty, no duplicates (within and across).
    const char *all[2 * CS_ASTRO_FACES];
    for (int i = 0; i < CS_ASTRO_FACES; i++) {
        all[i] = CS_ASTRO_PLANETS[i];
        all[CS_ASTRO_FACES + i] = CS_ASTRO_SIGNS[i];
    }
    for (int i = 0; i < 2 * CS_ASTRO_FACES; i++) {
        assert(all[i] && all[i][0] && strlen(all[i]) <= 11);
        for (const char *c = all[i]; *c; c++) assert(*c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\');
        for (int j = 0; j < i; j++) assert(strcmp(all[i], all[j]) != 0);
    }
    assert(strcmp(CS_ASTRO_PLANETS[0], "Sun") == 0 && strcmp(CS_ASTRO_PLANETS[3], "Venus") == 0 &&
           strcmp(CS_ASTRO_PLANETS[11], "South Node") == 0);
    assert(strcmp(CS_ASTRO_SIGNS[0], "Aries") == 0 && strcmp(CS_ASTRO_SIGNS[7], "Scorpio") == 0 &&
           strcmp(CS_ASTRO_SIGNS[11], "Pisces") == 0);
    // Rejection sampling: bytes >= 240 are skipped, the rest map by % 12.
    size_t used = 99;
    const uint8_t a[] = {0, 11, 12, 239};
    assert(cs_astro_face(a, 1, &used) == 0 && used == 1);
    assert(cs_astro_face(a + 1, 1, &used) == 11 && used == 1);
    assert(cs_astro_face(a + 2, 1, &used) == 0 && used == 1);
    assert(cs_astro_face(a + 3, 1, &used) == 11 && used == 1);   // 239 = 19 * 12 + 11, the last accepted
    const uint8_t rej[] = {240, 255, 251, 7};
    assert(cs_astro_face(rej, 4, &used) == 7 && used == 4);      // three rejected, then 7
    assert(cs_astro_face(rej, 3, &used) == -1 && used == 3);     // all rejected
    assert(cs_astro_face(rej, 0, &used) == -1 && used == 0);
    assert(cs_astro_face(a, 4, NULL) == 0);
    // Every accepted byte value: each face exactly 20 times (unbiased), every rejected one refused.
    int count[CS_ASTRO_FACES] = {0};
    for (int v = 0; v < 256; v++) {
        const uint8_t b = (uint8_t)v;
        const int f = cs_astro_face(&b, 1, NULL);
        if (v >= 240) {
            assert(f == -1);
        } else {
            assert(f == v % 12);
            count[f]++;
        }
    }
    for (int i = 0; i < CS_ASTRO_FACES; i++) assert(count[i] == 20);
    // Three dice from one stream: planet, sign, house - 1, rejections skipped in between.
    uint8_t faces[3];
    const uint8_t s1[] = {3, 245, 31, 254, 255, 6};               // Venus, 31 % 12 = 7 Scorpio, house 7
    assert(cs_astro_roll(s1, sizeof s1, faces) && faces[0] == 3 && faces[1] == 7 && faces[2] == 6);
    const uint8_t s2[] = {200, 240, 240};                         // 200 % 12 = 8, then nothing usable
    assert(!cs_astro_roll(s2, sizeof s2, faces));
    const uint8_t s3[] = {12, 24, 36};
    assert(cs_astro_roll(s3, sizeof s3, faces) && faces[0] == 0 && faces[1] == 0 && faces[2] == 0);
    // The event.
    char j[128];
    const uint8_t f1[3] = {3, 7, 6};
    assert(cs_astro_event_json(f1, j, sizeof j) > 0);
    assert(strcmp(j, "{\"type\":\"astro_roll\",\"planet\":\"Venus\",\"sign\":\"Scorpio\",\"house\":7}") == 0);
    const uint8_t f2[3] = {10, 8, 11};
    assert(cs_astro_event_json(f2, j, sizeof j) > 0);
    assert(strcmp(j, "{\"type\":\"astro_roll\",\"planet\":\"North Node\",\"sign\":\"Sagittarius\",\"house\":12}") == 0);
    const uint8_t bad[3] = {12, 0, 0};
    assert(cs_astro_event_json(bad, j, sizeof j) == -1);
    assert(cs_astro_event_json(f1, j, 20) == -1);                 // too small
    // Animation: flickers (faces change frame to frame), stops one by one on the result.
    uint8_t shown[3], prev[3];
    assert(cs_astro_anim(0, f1, 5, shown) == 0);
    memcpy(prev, shown, 3);
    assert(cs_astro_anim(CS_ASTRO_FRAME_MS, f1, 5, shown) == 0);
    for (int d = 0; d < 3; d++) assert(shown[d] != prev[d] && shown[d] < 12);
    assert(cs_astro_anim(CS_ASTRO_SETTLE_MS[0], f1, 5, shown) == 1 && shown[0] == 3);
    assert(cs_astro_anim(CS_ASTRO_SETTLE_MS[1], f1, 5, shown) == 3 && shown[1] == 7);
    assert(cs_astro_anim(CS_ASTRO_SETTLE_MS[2] - 1, f1, 5, shown) == 3);
    assert(cs_astro_anim(CS_ASTRO_SETTLE_MS[2], f1, 5, shown) == 7 && memcmp(shown, f1, 3) == 0);
    assert(cs_astro_anim(60000, f1, 200, shown) == 7 && memcmp(shown, f1, 3) == 0);
    assert(CS_ASTRO_SETTLE_MS[0] < CS_ASTRO_SETTLE_MS[1] && CS_ASTRO_SETTLE_MS[1] < CS_ASTRO_SETTLE_MS[2] &&
           CS_ASTRO_SETTLE_MS[2] <= 1200);
    // The last die flickers through all 12 faces before it stops (12 frames < its 1 s).
    uint32_t seen = 0;
    for (uint32_t f = 0; f < 12; f++) {
        const uint8_t z[3] = {0, 0, 0};
        assert(!(cs_astro_anim(f * CS_ASTRO_FRAME_MS, z, 0, shown) & 4));
        seen |= 1u << shown[2];
    }
    assert(seen == 0xFFFu);
    // Water shimmer: dim and wobbling before a die settles, sharp and still after it surfaced.
    assert(CS_ASTRO_ANIM_MS == CS_ASTRO_SETTLE_MS[2] + CS_ASTRO_SURFACE_MS && CS_ASTRO_ANIM_MS <= 1300);
    cs_astro_fx_t fx;
    for (int d = 0; d < 3; d++) {
        int moved = 0, dimmest = 255, brightest = 0;
        for (uint32_t t = 0; t < CS_ASTRO_SETTLE_MS[d]; t += 10) {
            cs_astro_fx(t, d, &fx);
            assert(fx.opa >= 70 && fx.opa <= 170 && fx.ghost > 0 && fx.glow == 255);
            assert(fx.dx >= -3 && fx.dx <= 3 && fx.dy >= -2 && fx.dy <= 2);
            moved |= fx.dx != 0;
            if (fx.opa < dimmest) dimmest = fx.opa;
            if (fx.opa > brightest) brightest = fx.opa;
        }
        assert(moved && brightest - dimmest >= 50);                    // it really shimmers
        int last = 0;
        for (uint32_t t = CS_ASTRO_SETTLE_MS[d]; t < CS_ASTRO_SETTLE_MS[d] + CS_ASTRO_SURFACE_MS; t += 10) {
            cs_astro_fx(t, d, &fx);
            assert(fx.opa >= last && fx.opa >= 160 && fx.dy == 0);    // surfacing: only gets sharper
            last = fx.opa;
        }
        cs_astro_fx(CS_ASTRO_SETTLE_MS[d] + CS_ASTRO_SURFACE_MS, d, &fx);
        assert(fx.opa == 255 && fx.dx == 0 && fx.dy == 0 && fx.ghost == 0);
        cs_astro_fx(60000, d, &fx);
        assert(fx.opa == 255 && fx.dx == 0 && fx.ghost == 0);
    }
    // History log: newest first, ten kept, bad entries dropped.
    cs_astro_log_t log[CS_ASTRO_LOG];
    memset(log, 0, sizeof log);
    uint8_t n = 0;
    for (uint32_t i = 0; i < 13; i++) {
        const uint8_t f[3] = {(uint8_t)(i % 12), (uint8_t)((i + 1) % 12), (uint8_t)((i + 2) % 12)};
        cs_astro_log_push(log, &n, 1000 + i, f);
    }
    assert(n == 10 && log[0].t == 1012 && log[0].f[0] == 0 && log[9].t == 1003 && log[9].f[2] == 5);
    assert(sizeof(cs_astro_log_t) == 8);
    log[3].f[1] = 12;
    log[7].f[0] = 200;
    n = cs_astro_log_clean(log, n);
    assert(n == 8 && log[3].t == 1008 && log[6].t == 1004 && log[7].t == 1003);
    assert(cs_astro_log_clean(log, 250) <= CS_ASTRO_LOG);
    // Times in Beijing: 2026-11-02 21:52 CST = 13:52 UTC = 1793627520.
    char w[24];
    const uint32_t now = 1793627520u;
    cs_astro_when(now, now, w, sizeof w);
    assert(strcmp(w, "Today 21:52") == 0);
    cs_astro_when(now - 45 * 3600 - 47 * 60, now, w, sizeof w);    // 2026-11-01 00:05
    assert(strcmp(w, "Yesterday 00:05") == 0);
    cs_astro_when(now - 2 * 86400 - 7 * 3600 - 50 * 60, now, w, sizeof w);
    assert(strcmp(w, "Oct 31 14:02") == 0);
    cs_astro_when(now - 21 * 3600 - 52 * 60 - 1, now, w, sizeof w);   // 1 s before Beijing midnight
    assert(strcmp(w, "Yesterday 23:59") == 0);
    cs_astro_when(now, 0, w, sizeof w);                             // clock not set now: the date
    assert(strcmp(w, "Nov 2 21:52") == 0);
    cs_astro_when(0, now, w, sizeof w);
    assert(strcmp(w, "\u2014") == 0);
    cs_astro_when(951782400u, 0, w, sizeof w);                      // 2000-02-29 00:00 UTC = 08:00 CST
    assert(strcmp(w, "Feb 29 08:00") == 0);
}

int main(void) {
    test_keys();
    test_idle();
    test_home();
    test_pomo();
    test_backoff();
    test_weather();
    test_aqi();
    test_json();
    test_call_and_audio();
    test_downlink_adpcm();
    test_playchunk();
    test_messages();
    test_memos();
    test_sha256();
    test_mines();
    test_bingo();
    test_astro();
    puts("test_cs_logic: PASS");
    return 0;
}
