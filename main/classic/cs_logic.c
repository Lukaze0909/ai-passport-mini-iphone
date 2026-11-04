// main/classic/cs_logic.c -- Classic shell pure logic (host tested).
#include "cs_logic.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- keys
void cs_keys_init(cs_keys_t *k) {
    memset(k, 0, sizeof *k);
}

cs_keys_verdict_t cs_keys_feed(cs_keys_t *k, uint8_t key, uint8_t ev, uint32_t now_ms) {
    if (key >= CS_KEY_COUNT) return CS_KEYS_DROP;
    const uint8_t bit = (uint8_t)(1u << key);
    if (ev == CS_KEV_PRESS) {
        k->down_ms[key] = now_ms;
        k->live |= bit;
        k->spent &= (uint8_t)~bit;
        k->last = key;
        return CS_KEYS_PRESS;
    }
    if (k->spent & bit) return CS_KEYS_DROP;
    if (!(k->live & bit) || (ev != CS_KEV_LONG && ev != CS_KEV_RELEASE)) return CS_KEYS_OTHER;
    k->live &= (uint8_t)~bit;   // decided: the rest of this press is OTHER
    if (ev == CS_KEV_LONG) return CS_KEYS_LONG;
    // The BSP sends no LONG for a hold that starts inside its click window
    // right after a release (press, release, press-and-hold): time it here.
    return (uint32_t)(now_ms - k->down_ms[key]) >= CS_LONG_MS ? CS_KEYS_LONG : CS_KEYS_TAP;
}

void cs_keys_spend(cs_keys_t *k) {
    const uint8_t bit = (uint8_t)(1u << k->last);
    k->live &= (uint8_t)~bit;
    k->spent |= bit;
}

// ---------------------------------------------------------------- standby
const uint16_t CS_IDLE_S[CS_IDLE_CHOICES] = {30, 60, 120, 300, 0};

void cs_idle_init(cs_idle_t *t, uint32_t timeout_ms, uint32_t now_ms) {
    t->timeout_ms = timeout_ms;
    cs_idle_wake(t, now_ms);
}

void cs_idle_wake(cs_idle_t *t, uint32_t now_ms) {
    t->last_ms = now_ms;
    t->state = CS_SCREEN_ON;
}

void cs_idle_off(cs_idle_t *t) { t->state = CS_SCREEN_OFF; }

uint8_t cs_idle_tick(cs_idle_t *t, uint32_t now_ms, bool hold) {
    if (t->state == CS_SCREEN_OFF) return t->state;
    if (hold || t->timeout_ms == 0) {
        if (hold) t->last_ms = now_ms;
        t->state = CS_SCREEN_ON;
        return t->state;
    }
    const uint32_t idle = now_ms - t->last_ms;
    t->state = idle >= t->timeout_ms + CS_DIM_MS ? CS_SCREEN_OFF
             : idle >= t->timeout_ms           ? CS_SCREEN_DIM
                                               : CS_SCREEN_ON;
    return t->state;
}

// ---------------------------------------------------------------- home
static int home_step1(int sel, int dir) {
    const int n = CS_HOME_COUNT;
    const int s = (sel + (dir >= 0 ? 1 : -1)) % n;
    return s < 0 ? s + n : s;
}

int cs_home_step(int sel, int dir, uint32_t live) {
    live &= CS_HOME_ALL;
    int s = sel;
    for (int i = 0; i < CS_HOME_COUNT; i++) {
        s = home_step1(s, dir);
        if (!live || (live >> s & 1u)) return s;
    }
    return s;
}

// ---------------------------------------------------------------- pomodoro
const uint8_t CS_POMO_PRESET_MIN[CS_POMO_PRESET_COUNT] = {1, 3, 5, 15, 30, 45, 60};

void cs_pomo_start(cs_pomo_t *p, int64_t now_us, uint32_t total_s) {
    p->running = true;
    p->start_us = now_us;
    p->total_s = total_s;
}

void cs_pomo_stop(cs_pomo_t *p) { p->running = false; }

int64_t cs_pomo_remaining_us(const cs_pomo_t *p, int64_t now_us) {
    if (!p->running) return 0;
    int64_t left = (int64_t)p->total_s * 1000000 - (now_us - p->start_us);
    return left > 0 ? left : 0;
}

uint32_t cs_pomo_remaining_s(const cs_pomo_t *p, int64_t now_us) {
    const int64_t left = cs_pomo_remaining_us(p, now_us);
    return (uint32_t)((left + 999999) / 1000000);
}

uint16_t cs_pomo_progress_permille(const cs_pomo_t *p, int64_t now_us) {
    if (!p->running || p->total_s == 0) return 0;
    const int64_t total = (int64_t)p->total_s * 1000000;
    int64_t done = now_us - p->start_us;
    if (done < 0) done = 0;
    if (done > total) done = total;
    return (uint16_t)(done * 1000 / total);
}

int cs_pomo_format(uint32_t secs, bool long_format, char *buf, size_t cap) {
    const unsigned h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
    if (long_format) return snprintf(buf, cap, "%u:%02u:%02u", h, m, s);
    return snprintf(buf, cap, "%02u:%02u", h * 60 + m, s);
}

// ---------------------------------------------------------------- network
uint32_t cs_backoff_ms(unsigned attempt) {
    if (attempt >= 4) return 60000;
    const uint32_t ms = 5000u << attempt;   // 5 10 20 40
    return ms > 60000 ? 60000 : ms;
}

// ---------------------------------------------------------------- weather
cs_wx_icon_t cs_wx_icon_from_name(const char *name) {
    static const char *const NAMES[CS_WX_ICON_COUNT] = {
        "sunny", "cloudy", "overcast", "rain", "snow", "fog", "night"};
    if (!name) return CS_WX_CLOUDY;
    for (int i = 0; i < CS_WX_ICON_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) return (cs_wx_icon_t)i;
    }
    return CS_WX_CLOUDY;
}

static const struct { const char *level; cs_aqi_style_t style; } AQI_LEVELS[] = {
    {"优", {0x4CB84C, 0xFFFFFF}},
    {"良", {0xF5D23C, 0x5A3C00}},
    {"轻度污染", {0xF2922E, 0xFFFFFF}},
    {"中度污染", {0xE0413A, 0xFFFFFF}},
    {"重度污染", {0x8E44AD, 0xFFFFFF}},
    {"严重污染", {0x7E1F2A, 0xFFFFFF}},
};
#define AQI_LEVEL_COUNT (sizeof AQI_LEVELS / sizeof AQI_LEVELS[0])

const char *cs_aqi_level_for(int aqi) {
    if (aqi < 0) return "";
    static const int LIMIT[AQI_LEVEL_COUNT - 1] = {50, 100, 150, 200, 300};
    for (size_t i = 0; i < AQI_LEVEL_COUNT - 1; i++) {
        if (aqi <= LIMIT[i]) return AQI_LEVELS[i].level;
    }
    return AQI_LEVELS[AQI_LEVEL_COUNT - 1].level;
}

cs_aqi_style_t cs_aqi_style(const char *level, int aqi) {
    if (level && level[0]) {
        for (size_t i = 0; i < AQI_LEVEL_COUNT; i++) {
            if (strcmp(level, AQI_LEVELS[i].level) == 0) return AQI_LEVELS[i].style;
        }
    }
    if (aqi >= 0) return cs_aqi_style(cs_aqi_level_for(aqi), -1);
    const cs_aqi_style_t grey = {0x9AA0A8, 0xFFFFFF};
    return grey;
}

// ---------------------------------------------------------------- JSON
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int csj_parse(const char *js, size_t len, csj_tok_t *t, int max) {
    int n = 0;
    int stack[16];          // indices of open containers
    int depth = 0;
    for (size_t p = 0; p < len; p++) {
        const char c = js[p];
        if (is_space(c) || c == ',' || c == ':') continue;
        if (c == '{' || c == '[') {
            if (n >= max || depth >= 16) return -1;
            // A container inside an object is always a value (keys are strings).
            if (depth && t[stack[depth - 1]].type == CSJ_ARR) t[stack[depth - 1]].size++;
            t[n] =(csj_tok_t){c == '{' ? CSJ_OBJ : CSJ_ARR, (int)p, -1, 0};
            stack[depth++] = n++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (!depth) return -1;
            csj_tok_t *open = &t[stack[--depth]];
            if ((c == '}') != (open->type == CSJ_OBJ)) return -1;
            open->end = (int)p + 1;
            continue;
        }
        if (n >= max) return -1;
        if (c == '"') {
            size_t q = p + 1;
            while (q < len && js[q] != '"') {
                if (js[q] == '\\') q++;
                q++;
            }
            if (q >= len) return -1;
            t[n] = (csj_tok_t){CSJ_STR, (int)p + 1, (int)q, 0};
            p = q;
        } else {
            size_t q = p;
            while (q < len && !is_space(js[q]) && js[q] != ',' && js[q] != ']' && js[q] != '}' &&
                   js[q] != ':') {
                q++;
            }
            t[n] = (csj_tok_t){CSJ_PRIM, (int)p, (int)q, 0};
            p = q - 1;
        }
        // Object members: count keys only (a key token is followed by its value).
        if (depth) {
            csj_tok_t *parent = &t[stack[depth - 1]];
            if (parent->type == CSJ_ARR) {
                parent->size++;
            } else {
                // In an object, tokens alternate key, value. A token is a key when the
                // previous direct child count is even.
                int direct = 0;
                for (int j = stack[depth - 1] + 1; j < n; j = csj_next(t, n, j)) direct++;
                if ((direct & 1) == 0) parent->size++;
            }
        }
        n++;
    }
    if (depth) return -1;
    return n;
}

int csj_next(const csj_tok_t *t, int n, int i) {
    if (t[i].type != CSJ_OBJ && t[i].type != CSJ_ARR) return i + 1;
    int j = i + 1;
    while (j < n && (t[i].end < 0 || t[j].start < t[i].end)) j++;
    return j;
}

int csj_get(const char *js, const csj_tok_t *t, int n, int obj, const char *key) {
    if (obj < 0 || obj >= n || t[obj].type != CSJ_OBJ) return -1;
    const size_t klen = strlen(key);
    int j = obj + 1;
    for (int k = 0; k < t[obj].size && j < n; k++) {
        const int v = j + 1;
        if (v >= n) return -1;
        if (t[j].type == CSJ_STR && (size_t)(t[j].end - t[j].start) == klen &&
            memcmp(js + t[j].start, key, klen) == 0) {
            return v;
        }
        j = csj_next(t, n, v);
    }
    return -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_u4(const char *s, int avail, uint32_t *out) {
    if (avail < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const int h = hexval(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

static int utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool csj_str(const char *js, const csj_tok_t *t, int i, char *out, size_t cap) {
    if (!cap) return false;
    out[0] = '\0';
    if (i < 0 || t[i].type != CSJ_STR) return false;
    size_t o = 0;
    for (int p = t[i].start; p < t[i].end;) {
        char tmp[4];
        int n = 0;
        if (js[p] == '\\' && p + 1 < t[i].end) {
            const char e = js[p + 1];
            p += 2;
            switch (e) {
            case 'n': tmp[0] = '\n'; n = 1; break;
            case 't': tmp[0] = '\t'; n = 1; break;
            case 'r': tmp[0] = '\r'; n = 1; break;
            case 'b': tmp[0] = '\b'; n = 1; break;
            case 'f': tmp[0] = '\f'; n = 1; break;
            case 'u': {
                uint32_t cp;
                if (!read_u4(js + p, t[i].end - p, &cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t[i].end && js[p] == '\\' &&
                    js[p + 1] == 'u') {
                    uint32_t lo;
                    if (read_u4(js + p + 2, t[i].end - p - 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                n = utf8_put(cp, tmp);
                break;
            }
            default: tmp[0] = e; n = 1; break;
            }
        } else {
            // Copy one whole UTF-8 sequence so truncation never splits a character.
            const unsigned char c = (unsigned char)js[p];
            n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            if (p + n > t[i].end) n = t[i].end - p;
            memcpy(tmp, js + p, (size_t)n);
            p += n;
        }
        if (o + (size_t)n >= cap) break;
        memcpy(out + o, tmp, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';
    return true;
}

bool csj_int(const char *js, const csj_tok_t *t, int i, int *out) {
    if (i < 0 || (t[i].type != CSJ_PRIM && t[i].type != CSJ_STR)) return false;
    int p = t[i].start;
    const int end = t[i].end;
    bool neg = false;
    if (p < end && (js[p] == '-' || js[p] == '+')) neg = js[p++] == '-';
    if (p >= end || js[p] < '0' || js[p] > '9') return false;
    long v = 0;
    while (p < end && js[p] >= '0' && js[p] <= '9') {
        v = v * 10 + (js[p++] - '0');
        if (v > 1000000000L) return false;
    }
    if (p < end && js[p] == '.' && p + 1 < end && js[p + 1] >= '5' && js[p + 1] <= '9') v++;
    *out = (int)(neg ? -v : v);
    return true;
}

// ---------------------------------------------------------------- weather parse
#define WX_TOKENS 80

bool cs_weather_parse(const char *json, size_t len, cs_weather_t *out) {
    csj_tok_t t[WX_TOKENS];
    const int n = csj_parse(json, len, t, WX_TOKENS);
    memset(out, 0, sizeof *out);
    out->aqi = -1;
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    if (!csj_int(json, t, csj_get(json, t, n, 0, "temp"), &out->temp)) return false;
    csj_str(json, t, csj_get(json, t, n, 0, "city"), out->city, sizeof out->city);
    csj_str(json, t, csj_get(json, t, n, 0, "cond"), out->cond, sizeof out->cond);
    csj_str(json, t, csj_get(json, t, n, 0, "icon"), out->icon, sizeof out->icon);
    csj_str(json, t, csj_get(json, t, n, 0, "aqi_level"), out->aqi_level, sizeof out->aqi_level);
    csj_str(json, t, csj_get(json, t, n, 0, "updated"), out->updated, sizeof out->updated);
    if (!csj_int(json, t, csj_get(json, t, n, 0, "hi"), &out->hi)) out->hi = out->temp;
    if (!csj_int(json, t, csj_get(json, t, n, 0, "lo"), &out->lo)) out->lo = out->temp;
    if (!csj_int(json, t, csj_get(json, t, n, 0, "aqi"), &out->aqi)) out->aqi = -1;
    if (!out->aqi_level[0] && out->aqi >= 0) {
        snprintf(out->aqi_level, sizeof out->aqi_level, "%s", cs_aqi_level_for(out->aqi));
    }
    const int days = csj_get(json, t, n, 0, "days");
    if (days >= 0 && t[days].type == CSJ_ARR) {
        int j = days + 1;
        for (int k = 0; k < t[days].size && j < n; k++, j = csj_next(t, n, j)) {
            if (out->ndays >= CS_WX_DAYS) break;
            if (t[j].type != CSJ_OBJ) continue;
            cs_wx_day_t *d = &out->days[out->ndays];
            csj_str(json, t, csj_get(json, t, n, j, "d"), d->d, sizeof d->d);
            csj_str(json, t, csj_get(json, t, n, j, "icon"), d->icon, sizeof d->icon);
            if (!csj_int(json, t, csj_get(json, t, n, j, "hi"), &d->hi)) continue;
            if (!csj_int(json, t, csj_get(json, t, n, j, "lo"), &d->lo)) d->lo = d->hi;
            out->ndays++;
        }
    }
    out->valid = true;
    return true;
}

// ---------------------------------------------------------------- call
int cs_call_step(int cur, int dir) {
    int s = (cur + (dir >= 0 ? 1 : -1)) % CS_CALL_STOPS;
    return s < 0 ? s + CS_CALL_STOPS : s;
}

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767};
static const int8_t IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

void cs_adpcm_init(cs_adpcm_state_t *s) {
    s->predictor = 0;
    s->index = 0;
}

static void ima_update(cs_adpcm_state_t *s, uint8_t code) {
    const int step = IMA_STEP[s->index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int pred = s->predictor + ((code & 8) ? -diff : diff);
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    s->predictor = (int16_t)pred;
    int idx = s->index + IMA_INDEX[code];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    s->index = (uint8_t)idx;
}

static uint8_t ima_encode_one(cs_adpcm_state_t *s, int16_t sample) {
    const int step = IMA_STEP[s->index];
    int diff = (int)sample - s->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int temp = step;
    if (diff >= temp) { code |= 4; diff -= temp; }
    temp >>= 1;
    if (diff >= temp) { code |= 2; diff -= temp; }
    temp >>= 1;
    if (diff >= temp) code |= 1;
    ima_update(s, code);
    return code;
}

size_t cs_adpcm_encode(cs_adpcm_state_t *s, const int16_t *pcm, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        const uint8_t lo = ima_encode_one(s, pcm[i]);
        const uint8_t hi = ima_encode_one(s, pcm[i + 1]);
        out[o++] = (uint8_t)(lo | (hi << 4));
    }
    return o;
}

size_t cs_adpcm_decode(cs_adpcm_state_t *s, const uint8_t *in, size_t bytes, int16_t *pcm) {
    size_t o = 0;
    for (size_t i = 0; i < bytes; i++) {
        ima_update(s, in[i] & 0x0F);
        pcm[o++] = s->predictor;
        ima_update(s, in[i] >> 4);
        pcm[o++] = s->predictor;
    }
    return o;
}

void cs_audio_hdr_pack(uint8_t out[CS_AUDIO_HDR_BYTES], uint32_t clip_id, uint16_t seq,
                       uint16_t flags, const cs_adpcm_state_t *state) {
    const uint16_t pred = (uint16_t)state->predictor;
    out[0] = (uint8_t)clip_id;
    out[1] = (uint8_t)(clip_id >> 8);
    out[2] = (uint8_t)(clip_id >> 16);
    out[3] = (uint8_t)(clip_id >> 24);
    out[4] = (uint8_t)seq;
    out[5] = (uint8_t)(seq >> 8);
    out[6] = (uint8_t)flags;
    out[7] = (uint8_t)(flags >> 8);
    out[8] = (uint8_t)pred;
    out[9] = (uint8_t)(pred >> 8);
    out[10] = state->index;
    out[11] = 0;
}

bool cs_play_cmd_parse(const char *json, size_t len, cs_play_cmd_t *out) {
    csj_tok_t t[24];
    const int n = csj_parse(json, len, t, 24);
    memset(out, 0, sizeof *out);
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    char op[12];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "op"), op, sizeof op) || strcmp(op, "play") != 0) {
        return false;
    }
    const int v = csj_get(json, t, n, 0, "via");
    if (v >= 0) {
        char via[8];
        if (!csj_str(json, t, v, via, sizeof via)) return false;
        if (strcmp(via, "mqtt") == 0) out->via = CS_PLAY_MQTT;
        else if (strcmp(via, "http") != 0) return false;
    }
    if (out->via == CS_PLAY_HTTP) {
        if (!csj_str(json, t, csj_get(json, t, n, 0, "url"), out->url, sizeof out->url)) return false;
        if (strncmp(out->url, "https://", 8) != 0) return false;
        // A truncated URL would fetch the wrong file: reject instead.
        const int u = csj_get(json, t, n, 0, "url");
        if ((size_t)(t[u].end - t[u].start) >= sizeof out->url) return false;
    } else {
        int chunks = 0, bytes = 0;
        if (!csj_int(json, t, csj_get(json, t, n, 0, "chunks"), &chunks) || chunks < 1 || chunks > 65535) {
            return false;
        }
        out->chunks = (uint16_t)chunks;
        if (csj_int(json, t, csj_get(json, t, n, 0, "bytes"), &bytes) && bytes > 0) out->bytes = (uint32_t)bytes;
    }
    const int f = csj_get(json, t, n, 0, "fmt");
    if (f >= 0) {
        char fmt[8];
        if (!csj_str(json, t, f, fmt, sizeof fmt)) return false;
        if (strcmp(fmt, "adpcm") == 0) out->fmt = CS_PLAY_ADPCM;
        else if (strcmp(fmt, "pcm") != 0) return false;   // unknown format: never play noise
    }
    csj_str(json, t, csj_get(json, t, n, 0, "id"), out->id, sizeof out->id);
    // A chunk session is keyed by the id's first 8 characters and needs ADPCM.
    if (out->via == CS_PLAY_MQTT && (strlen(out->id) < 8 || out->fmt != CS_PLAY_ADPCM)) return false;
    int txt = csj_get(json, t, n, 0, "text");
    if (txt < 0) txt = csj_get(json, t, n, 0, "caption");
    csj_str(json, t, txt, out->text, sizeof out->text);
    return true;
}

void cs_adpcm_stream_init(cs_adpcm_stream_t *d) { memset(d, 0, sizeof *d); }

int cs_adpcm_stream_feed(cs_adpcm_stream_t *d, const uint8_t *in, size_t len, size_t *used,
                         int16_t *pcm, size_t max_samples) {
    size_t u = 0, o = 0;
    max_samples &= ~(size_t)1;
    while (u < len) {
        if (d->data_left == 0) {
            d->hdr[d->hdr_have++] = in[u++];
            if (d->hdr_have < CS_PLAY_BLOCK_HDR) continue;
            d->hdr_have = 0;
            if (d->hdr[2] > 88) {
                *used = u;
                return -1;
            }
            d->st.predictor = (int16_t)(uint16_t)(d->hdr[0] | (d->hdr[1] << 8));
            d->st.index = d->hdr[2];
            d->data_left = CS_PLAY_BLOCK_DATA;
            continue;
        }
        size_t n = (max_samples - o) / 2;
        if (n == 0) break;
        if (n > len - u) n = len - u;
        if (n > d->data_left) n = d->data_left;
        o += cs_adpcm_decode(&d->st, in + u, n, pcm + o);
        u += n;
        d->data_left = (uint16_t)(d->data_left - n);
    }
    *used = u;
    return (int)o;
}

size_t cs_block_asm_add(cs_block_asm_t *a, const uint8_t *in, size_t len) {
    size_t n = CS_PLAY_BLOCK_BYTES - a->have;
    if (n > len) n = len;
    memcpy(a->buf + a->have, in, n);
    a->have = (uint16_t)(a->have + n);
    return n;
}

// ---------------------------------------------------------------- playchunk session
void cs_chunk_sess_init(cs_chunk_sess_t *s, const char *id, uint16_t total) {
    memcpy(s->id, id, sizeof s->id);
    s->total = total;
    s->next = 0;
    s->blocks = 0;
}

int cs_chunk_accept(cs_chunk_sess_t *s, const uint8_t *hdr, size_t len, size_t msg_len) {
    if (len < CS_CHUNK_HDR || msg_len < len || memcmp(hdr, s->id, sizeof s->id) != 0) return -1;
    const uint16_t seq = (uint16_t)(hdr[8] | (hdr[9] << 8));
    if (seq < s->next || seq >= s->total) return -1;
    // Only the clip's last chunk may be short: every other one tells the size.
    if (seq + 1 < s->total) {
        const size_t blocks = (msg_len - CS_CHUNK_HDR + CS_PLAY_BLOCK_BYTES - 1) / CS_PLAY_BLOCK_BYTES;
        if (blocks >= 1 && blocks <= 255) s->blocks = (uint8_t)blocks;
    }
    const int missing = seq - s->next;
    s->next = (uint16_t)(seq + 1);
    return missing * (s->blocks ? s->blocks : CS_CHUNK_BLOCKS_DEFAULT);
}

bool cs_chunk_complete(const cs_chunk_sess_t *s) { return s->next >= s->total; }

// ---------------------------------------------------------------- messages
// UTF-8 text -> printable ASCII: tabs / newlines become spaces, any other
// character (one per code point) '?'. Writes at most max chars plus the NUL.
static void ascii_reduce(const char *raw, char *out, size_t max) {
    size_t o = 0;
    for (size_t i = 0; raw[i] && o < max;) {
        const unsigned char ch = (unsigned char)raw[i];
        if (ch < 0x80) {
            out[o++] = ch == '\t' || ch == '\n' || ch == '\r' ? ' ' : (ch < 0x20 || ch == 0x7F) ? '?' : (char)ch;
            i++;
        } else {
            out[o++] = '?';
            i++;
            while ((unsigned char)raw[i] >= 0x80 && ((unsigned char)raw[i] & 0xC0) == 0x80) i++;
        }
    }
    out[o] = '\0';
}

// Unix seconds / clip ids exceed csj_int's 10^9 guard: an unsigned 32-bit number.
static bool csj_u32(const char *js, const csj_tok_t *t, int k, uint32_t *out) {
    if (k < 0 || t[k].type != CSJ_PRIM) return false;
    uint64_t v = 0;
    int p = t[k].start;
    while (p < t[k].end && js[p] >= '0' && js[p] <= '9' && v <= 0xFFFFFFFFu) v = v * 10 + (uint64_t)(js[p++] - '0');
    if (p == t[k].start || p != t[k].end || v > 0xFFFFFFFFu) return false;
    *out = (uint32_t)v;
    return true;
}

bool cs_msg_cmd_parse(const char *json, size_t len, cs_msg_cmd_t *out) {
    csj_tok_t t[16];
    const int n = csj_parse(json, len, t, 16);
    memset(out, 0, sizeof *out);
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    char op[8];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "op"), op, sizeof op) || strcmp(op, "msg") != 0) return false;
    // Unescaped UTF-8 first (up to 4 bytes per character), then reduce to ASCII.
    char raw[CS_MSG_TEXT_MAX * 4 + 1];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "text"), raw, sizeof raw)) return false;
    ascii_reduce(raw, out->text, CS_MSG_TEXT_MAX);
    csj_str(json, t, csj_get(json, t, n, 0, "id"), out->id, sizeof out->id);
    (void)csj_u32(json, t, csj_get(json, t, n, 0, "ts"), &out->ts);
    return true;
}

bool cs_memo_cmd_parse(const char *json, size_t len, cs_memo_cmd_t *out) {
    csj_tok_t t[12];
    const int n = csj_parse(json, len, t, 12);
    memset(out, 0, sizeof *out);
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    char op[8];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "op"), op, sizeof op) || strcmp(op, "memo") != 0) return false;
    if (!csj_u32(json, t, csj_get(json, t, n, 0, "clip"), &out->clip)) return false;
    char raw[CS_MEMO_TEXT_MAX * 4 + 1];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "text"), raw, sizeof raw)) return false;
    ascii_reduce(raw, out->text, CS_MEMO_TEXT_MAX);
    return true;
}

bool cs_memo_visible(uint8_t flags, uint32_t created, uint32_t done_at, uint32_t now) {
    if (now == 0) return true;
    if (flags & CS_MEMO_DONE) {
        if (done_at == 0) return true;
        const uint32_t day_done = (done_at + CS_TZ_OFFSET_S) / 86400u;
        const uint32_t day_now = (now + CS_TZ_OFFSET_S) / 86400u;
        return day_now <= day_done;
    }
    return created == 0 || now < created || now - created < CS_MEMO_TTL_S;
}

#define STORE_MAGIC 0x5A4Du       // "MZ"
#define STORE_HDR 20u

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t rec_len(uint16_t text_len) { return ((STORE_HDR + text_len + 3u) & ~3u) + 4u; }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// Reads and checks the record at off. Returns its length, 0 if there is none.
static uint32_t rec_read(const cs_store_t *s, uint32_t off, uint8_t *buf, uint32_t *seq) {
    const uint32_t end = (off / CS_STORE_SECTOR + 1) * CS_STORE_SECTOR;
    if (off + STORE_HDR + 4 > end || !s->read(s->ctx, off, buf, STORE_HDR)) return 0;
    if ((buf[0] | buf[1] << 8) != STORE_MAGIC) return 0;
    const uint16_t tl = (uint16_t)(buf[4] | buf[5] << 8);
    if (tl > CS_MSG_TEXT_MAX) return 0;
    const uint32_t rl = rec_len(tl);
    if (off + rl > end || !s->read(s->ctx, off + STORE_HDR, buf + STORE_HDR, rl - STORE_HDR)) return 0;
    if (fnv1a(2166136261u, buf, rl - 4) != rd32(buf + rl - 4)) return 0;
    *seq = rd32(buf + 8);
    return rl;
}

static void store_keep(cs_store_t *s, uint32_t *seqs, uint32_t seq, uint32_t off) {
    // Insert in seq order; keep the newest CS_STORE_KEEP.
    int pos = s->n;
    while (pos > 0 && seqs[pos - 1] > seq) pos--;
    if (s->n == CS_STORE_KEEP) {
        if (pos == 0) return;                 // older than everything kept
        memmove(&s->idx[0], &s->idx[1], (size_t)(pos - 1) * sizeof s->idx[0]);
        memmove(&seqs[0], &seqs[1], (size_t)(pos - 1) * sizeof seqs[0]);
        pos--;
    } else {
        memmove(&s->idx[pos + 1], &s->idx[pos], (size_t)(s->n - pos) * sizeof s->idx[0]);
        memmove(&seqs[pos + 1], &seqs[pos], (size_t)(s->n - pos) * sizeof seqs[0]);
        s->n++;
    }
    s->idx[pos] = off;
    seqs[pos] = seq;
}

bool cs_store_open(cs_store_t *s) {
    if (s->size < 2 * CS_STORE_SECTOR || s->size % CS_STORE_SECTOR) return false;
    uint8_t buf[STORE_HDR + CS_MSG_TEXT_MAX + 8];
    uint32_t seqs[CS_STORE_KEEP];
    uint32_t best = 0, best_end = 0;
    bool any = false;
    s->n = 0;
    for (uint32_t sec = 0; sec < s->size; sec += CS_STORE_SECTOR) {
        uint32_t off = sec, seq, rl;
        while ((rl = rec_read(s, off, buf, &seq)) != 0) {
            store_keep(s, seqs, seq, off);
            if (!any || seq > best) {
                best = seq;
                best_end = off + rl;
                any = true;
            }
            off += rl;
        }
    }
    s->seq = any ? best + 1 : 1;
    s->wr = any ? best_end % s->size : 0;
    // The rest of the write sector must still be erased; if a torn record (or
    // junk) sits there, continue in the next sector instead.
    if (s->wr % CS_STORE_SECTOR) {
        const uint32_t end = (s->wr / CS_STORE_SECTOR + 1) * CS_STORE_SECTOR;
        for (uint32_t off = s->wr; off < end; off += 64) {
            const uint32_t k = end - off < 64 ? end - off : 64;
            if (!s->read(s->ctx, off, buf, k)) return false;
            bool clean = true;
            for (uint32_t i = 0; i < k; i++) clean &= buf[i] == 0xFF;
            if (!clean) {
                s->wr = end % s->size;
                break;
            }
        }
    }
    return true;
}

bool cs_store_add(cs_store_t *s, const cs_msg_t *m) {
    size_t tl = 0;
    while (tl < CS_MSG_TEXT_MAX && m->text[tl]) tl++;
    const uint32_t rl = rec_len((uint16_t)tl);
    if (s->wr % CS_STORE_SECTOR + rl > CS_STORE_SECTOR) {
        s->wr = (s->wr / CS_STORE_SECTOR + 1) * CS_STORE_SECTOR % s->size;
    }
    if (s->wr % CS_STORE_SECTOR == 0) {
        // Entering a sector: it holds the oldest records (or nothing). Erase it.
        if (!s->erase(s->ctx, s->wr)) return false;
        uint8_t k = 0;
        for (uint8_t i = 0; i < s->n; i++) {
            if (s->idx[i] / CS_STORE_SECTOR != s->wr / CS_STORE_SECTOR) s->idx[k++] = s->idx[i];
        }
        s->n = k;
    }
    uint8_t buf[STORE_HDR + CS_MSG_TEXT_MAX + 8];
    memset(buf, 0, sizeof buf);
    buf[0] = (uint8_t)STORE_MAGIC;
    buf[1] = (uint8_t)(STORE_MAGIC >> 8);
    buf[2] = m->out ? 1 : 0;
    buf[4] = (uint8_t)tl;
    buf[5] = (uint8_t)(tl >> 8);
    buf[6] = (uint8_t)m->dur_s;
    buf[7] = (uint8_t)(m->dur_s >> 8);
    wr32(buf + 8, s->seq);
    wr32(buf + 12, m->ts);
    wr32(buf + 16, m->aux);
    memcpy(buf + STORE_HDR, m->text, tl);
    wr32(buf + rl - 4, fnv1a(2166136261u, buf, rl - 4));
    if (!s->write(s->ctx, s->wr, buf, rl)) return false;
    if (s->n == CS_STORE_KEEP) {
        memmove(&s->idx[0], &s->idx[1], (CS_STORE_KEEP - 1) * sizeof s->idx[0]);
        s->n--;
    }
    s->idx[s->n++] = s->wr;
    s->seq++;
    s->wr = (s->wr + rl) % s->size;
    return true;
}

bool cs_store_get(const cs_store_t *s, int i, cs_msg_t *out) {
    if (i < 0 || i >= s->n) return false;
    uint8_t buf[STORE_HDR + CS_MSG_TEXT_MAX + 8];
    uint32_t seq;
    if (!rec_read(s, s->idx[i], buf, &seq)) return false;
    const uint16_t tl = (uint16_t)(buf[4] | buf[5] << 8);
    out->out = buf[2];
    out->dur_s = (uint16_t)(buf[6] | buf[7] << 8);
    out->ts = rd32(buf + 12);
    out->aux = rd32(buf + 16);
    memcpy(out->text, buf + STORE_HDR, tl);
    out->text[tl] = '\0';
    return true;
}


// ---------------------------------------------------------------- SHA-256
static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror32(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

static void sha_block(cs_sha256_t *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], d0 = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
        const uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & d0) ^ (b & d0));
        h = g; g = f; f = e; e = d + t1; d = d0; d0 = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d0; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void cs_sha256_init(cs_sha256_t *c) {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, H0, sizeof H0);
    c->len = 0;
    c->used = 0;
}

void cs_sha256_update(cs_sha256_t *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len--) {
        c->buf[c->used++] = *p++;
        if (c->used == 64) {
            sha_block(c, c->buf);
            c->used = 0;
        }
    }
}

void cs_sha256_final(cs_sha256_t *c, uint8_t out[32]) {
    const uint64_t bits = c->len * 8;
    const uint8_t pad = 0x80, zero = 0;
    cs_sha256_update(c, &pad, 1);
    while (c->used != 56) cs_sha256_update(c, &zero, 1);
    uint8_t L[8];
    for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    cs_sha256_update(c, L, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

// ---------------------------------------------------------------- mines
const uint8_t CS_MINES_N[CS_MINES_SIZES] = {3, 4, 5};
const uint8_t CS_MINES_M[CS_MINES_SIZES] = {2, 3, 4};

int cs_mines_cells(const cs_mines_t *g) { return g->n * g->n; }

int cs_mines_count(uint32_t mask) {
    int k = 0;
    for (; mask; mask &= mask - 1) k++;
    return k;
}

bool cs_mines_cell_name(int cell, int n, char *buf, size_t cap) {
    if (cap < 3 || n < 1 || n > CS_MINES_MAX_N || cell < 0 || cell >= n * n) return false;
    buf[0] = (char)('A' + cell / n);
    buf[1] = (char)('1' + cell % n);
    buf[2] = '\0';
    return true;
}

int cs_mines_cell_parse(const char *s, int n) {
    if (!s || n < 1 || n > CS_MINES_MAX_N) return -1;
    while (*s == ' ') s++;
    if (!s[0] || !s[1]) return -1;   // "A": do not read past the end
    char r = s[0];
    if (r >= 'a' && r <= 'z') r = (char)(r - 'a' + 'A');
    const char c = s[1];
    const char *e = s + 2;
    while (*e == ' ') e++;
    if (*e || r < 'A' || r >= 'A' + n || c < '1' || c >= '1' + n) return -1;
    return (r - 'A') * n + (c - '1');
}

int cs_mines_cursor_step(int cur, int dir, int stops) {
    if (stops <= 0) return 0;
    const int s = (cur + (dir >= 0 ? 1 : -1)) % stops;
    return s < 0 ? s + stops : s;
}

void cs_mines_new(cs_mines_t *g, int size, bool companion_first, uint32_t game) {
    memset(g, 0, sizeof *g);
    if (size < 0 || size >= CS_MINES_SIZES) size = CS_MINES_DEFAULT_SIZE;
    g->game = game;
    g->n = CS_MINES_N[size];
    g->m = CS_MINES_M[size];
    g->phase = CS_MINES_PLACE;
    g->companion_first = companion_first;
    g->pending = -1;
    g->boom = -1;
}

static bool cell_ok(const cs_mines_t *g, int cell) { return cell >= 0 && cell < cs_mines_cells(g); }

bool cs_mines_toggle(cs_mines_t *g, int cell) {
    if (g->phase != CS_MINES_PLACE || !cell_ok(g, cell)) return false;
    const uint32_t bit = 1u << cell;
    if (!(g->mines & bit) && cs_mines_count(g->mines) >= g->m) return false;
    g->mines ^= bit;
    return true;
}

static void mines_start(cs_mines_t *g) { g->phase = g->companion_first ? CS_MINES_COMPANION_TURN : CS_MINES_MY_TURN; }

bool cs_mines_go(cs_mines_t *g) {
    if (g->phase != CS_MINES_PLACE || cs_mines_count(g->mines) != g->m) return false;
    if (g->commit[0]) mines_start(g);
    else g->phase = CS_MINES_READY;
    return true;
}

bool cs_mines_commit(cs_mines_t *g, const char *hex) {
    if (g->phase != CS_MINES_PLACE && g->phase != CS_MINES_READY) return false;
    char low[CS_MINES_COMMIT_LEN + 1];
    for (int i = 0; i < CS_MINES_COMMIT_LEN; i++) {
        char ch = hex[i];
        if (ch >= 'A' && ch <= 'F') ch = (char)(ch - 'A' + 'a');
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
        low[i] = ch;
    }
    if (hex[CS_MINES_COMMIT_LEN]) return false;
    low[CS_MINES_COMMIT_LEN] = '\0';
    if (g->commit[0]) return strcmp(g->commit, low) == 0;   // a re-delivery; never replaced
    memcpy(g->commit, low, sizeof low);
    if (g->phase == CS_MINES_READY) mines_start(g);
    return true;
}

bool cs_mines_step(cs_mines_t *g, int cell) {
    if (g->phase != CS_MINES_MY_TURN || !cell_ok(g, cell) || (g->my_steps >> cell & 1u)) return false;
    g->pending = (int8_t)cell;
    g->phase = CS_MINES_MY_WAIT;
    return true;
}

void cs_mines_unwait(cs_mines_t *g) {
    if (g->phase != CS_MINES_MY_WAIT) return;
    g->pending = -1;
    g->phase = CS_MINES_MY_TURN;
}

bool cs_mines_result(cs_mines_t *g, int cell, bool boom) {
    if (g->phase != CS_MINES_MY_WAIT || cell != g->pending) return false;
    g->pending = -1;
    if (boom) {
        g->boom = (int8_t)cell;
        g->winner = CS_MINES_COMPANION_WON;
        g->phase = CS_MINES_OVER;
        return true;
    }
    g->my_steps |= 1u << cell;
    if (cs_mines_count(g->my_steps) >= cs_mines_cells(g) - g->m) {
        g->winner = CS_MINES_OWNER_WON;
        g->phase = CS_MINES_OVER;
    } else {
        g->phase = CS_MINES_COMPANION_TURN;
    }
    return true;
}

int cs_mines_companion_step(cs_mines_t *g, int cell) {
    if (!cell_ok(g, cell) || g->phase < CS_MINES_MY_TURN) return -1;
    const uint32_t bit = 1u << cell;
    if (g->companion_steps & bit) return -2;
    if (g->phase != CS_MINES_COMPANION_TURN) return -1;
    g->companion_steps |= bit;
    if (g->mines & bit) {
        g->boom = (int8_t)cell;
        g->winner = CS_MINES_OWNER_WON;
        g->phase = CS_MINES_OVER;
        return 1;
    }
    if (cs_mines_count(g->companion_steps) >= cs_mines_cells(g) - g->m) {
        g->winner = CS_MINES_COMPANION_WON;
        g->phase = CS_MINES_OVER;
    } else {
        g->phase = CS_MINES_MY_TURN;
    }
    return 0;
}

int cs_mines_layout_parse(const char *layout, int n, uint32_t *mask) {
    *mask = 0;
    int count = 0;
    const char *p = layout;
    while (*p) {
        char cell[8];
        size_t k = 0;
        while (*p && *p != ',') {
            if (k + 1 >= sizeof cell) return -1;
            cell[k++] = *p++;
        }
        cell[k] = '\0';
        if (*p == ',') p++;
        const int c = cs_mines_cell_parse(cell, n);
        if (c < 0 || (*mask >> c & 1u)) return -1;
        *mask |= 1u << c;
        count++;
    }
    return count;
}

bool cs_mines_layout_str(uint32_t mask, int n, char *buf, size_t cap) {
    if (!cap) return false;
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < n * n && i < CS_MINES_MAX_CELLS; i++) {
        if (!(mask >> i & 1u)) continue;
        char c[4];
        if (!cs_mines_cell_name(i, n, c, sizeof c)) return false;
        const int w = snprintf(buf + o, cap - o, "%s%s", o ? "," : "", c);
        if (w < 0 || (size_t)w >= cap - o) {
            buf[o] = '\0';
            return false;
        }
        o += (size_t)w;
    }
    return true;
}

bool cs_mines_may_reveal_mine(const cs_mines_t *g) {
    if (g->phase == CS_MINES_OVER) return true;
    if (g->phase < CS_MINES_MY_TURN) return false;
    return (g->my_steps | g->companion_steps) != 0 || g->pending >= 0;
}

void cs_mines_commit_of(const char *layout, const char *salt, char *out) {
    cs_sha256_t c;
    uint8_t d[32];
    cs_sha256_init(&c);
    cs_sha256_update(&c, layout, strlen(layout));
    cs_sha256_update(&c, salt, strlen(salt));
    cs_sha256_final(&c, d);
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < CS_MINES_COMMIT_LEN / 2; i++) {
        out[2 * i] = HEX[d[i] >> 4];
        out[2 * i + 1] = HEX[d[i] & 15];
    }
    out[CS_MINES_COMMIT_LEN] = '\0';
}

bool cs_mines_reveal(cs_mines_t *g, const char *layout, const char *salt) {
    if (g->phase != CS_MINES_OVER) return false;
    char want[CS_MINES_COMMIT_LEN + 1];
    cs_mines_commit_of(layout, salt, want);
    uint32_t mask;
    const int count = cs_mines_layout_parse(layout, g->n, &mask);
    g->companion_mines = count > 0 ? mask : 0;
    bool ok = g->commit[0] && strcmp(want, g->commit) == 0 && count == g->m;
    ok = ok && (mask & g->my_steps) == 0;                      // every "safe" answer was safe
    if (g->winner == CS_MINES_COMPANION_WON && g->boom >= 0) {
        ok = ok && (mask >> g->boom & 1u);                      // her fatal cell was a mine
    }
    g->reveal = ok ? CS_MINES_VERIFIED : CS_MINES_MISMATCH;
    return true;
}

bool cs_mines_cmd_parse(const char *json, size_t len, cs_mines_cmd_t *out) {
    csj_tok_t t[20];
    const int n = csj_parse(json, len, t, 20);
    memset(out, 0, sizeof *out);
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    char op[24];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "op"), op, sizeof op)) return false;
    if (strcmp(op, "mine_commit") == 0) out->op = CS_MCMD_COMMIT;
    else if (strcmp(op, "mine_result") == 0) out->op = CS_MCMD_RESULT;
    else if (strcmp(op, "mine_companion_step") == 0) out->op = CS_MCMD_COMPANION_STEP;
    else if (strcmp(op, "mine_reveal") == 0) out->op = CS_MCMD_REVEAL;
    else return false;
    if (!csj_u32(json, t, csj_get(json, t, n, 0, "game"), &out->game)) return false;
    if (out->op == CS_MCMD_COMMIT) {
        char c[24];
        if (!csj_str(json, t, csj_get(json, t, n, 0, "commit"), c, sizeof c) || strlen(c) != CS_MINES_COMMIT_LEN) {
            return false;
        }
        memcpy(out->commit, c, sizeof out->commit);
        return true;
    }
    if (out->op == CS_MCMD_REVEAL) {
        return csj_str(json, t, csj_get(json, t, n, 0, "layout"), out->layout, sizeof out->layout) &&
               csj_str(json, t, csj_get(json, t, n, 0, "salt"), out->salt, sizeof out->salt) &&
               strlen(out->salt) < sizeof out->salt - 1;
    }
    if (out->op == CS_MCMD_RESULT) {
        const int b = csj_get(json, t, n, 0, "boom");
        if (b < 0 || t[b].type != CSJ_PRIM) return false;
        const int bl = t[b].end - t[b].start;
        if (bl == 4 && memcmp(json + t[b].start, "true", 4) == 0) out->boom = true;
        else if (!(bl == 5 && memcmp(json + t[b].start, "false", 5) == 0)) return false;
    } else {
        char raw[CS_MINES_NOTE_MAX * 4 + 1];
        if (csj_str(json, t, csj_get(json, t, n, 0, "note"), raw, sizeof raw)) ascii_reduce(raw, out->note, CS_MINES_NOTE_MAX);
    }
    char c[8];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "cell"), c, sizeof c) || strlen(c) > 3) return false;
    memcpy(out->cell, c, strlen(c) + 1);
    return true;
}

// ---------------------------------------------------------------- bingo
// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

void cs_bingo_card_day(uint32_t t, char *out) {
    const uint32_t local = t + CS_TZ_OFFSET_S - CS_BINGO_DAY_START_S;
    int y;
    unsigned m, d;
    civil_from_days((int32_t)(local / 86400u), &y, &m, &d);
    snprintf(out, CS_BINGO_DAY_LEN + 1, "%04u-%02u-%02u", (unsigned)y % 10000u, m % 100u, d % 100u);
}

int cs_bingo_cells(const cs_bingo_card_t *c) { return c->n * c->n; }

// Line masks for side n: n rows, n columns, the diagonal, the anti-diagonal.
static int bingo_line_masks(int n, uint32_t *masks) {
    int k = 0;
    for (int r = 0; r < n; r++) {
        uint32_t row = 0, col = 0;
        for (int i = 0; i < n; i++) {
            row |= 1u << (r * n + i);
            col |= 1u << (i * n + r);
        }
        masks[k++] = row;
        masks[n + r] = col;
    }
    k = 2 * n;
    uint32_t dg = 0, an = 0;
    for (int i = 0; i < n; i++) {
        dg |= 1u << (i * n + i);
        an |= 1u << (i * n + n - 1 - i);
    }
    masks[k++] = dg;
    masks[k++] = an;
    return k;
}

int cs_bingo_lines(uint32_t done, int n, uint32_t *cells) {
    if (cells) *cells = 0;
    if (n < 2 || n > CS_BINGO_MAX_N) return 0;
    uint32_t masks[2 * CS_BINGO_MAX_N + 2];
    const int k = bingo_line_masks(n, masks);
    int lines = 0;
    for (int i = 0; i < k; i++) {
        if ((done & masks[i]) != masks[i]) continue;
        lines++;
        if (cells) *cells |= masks[i];
    }
    return lines;
}

bool cs_bingo_first_line(uint32_t done, int n, char *name, size_t cap, uint32_t *cells) {
    if (n < 2 || n > CS_BINGO_MAX_N) return false;
    uint32_t masks[2 * CS_BINGO_MAX_N + 2];
    const int k = bingo_line_masks(n, masks);
    for (int i = 0; i < k; i++) {
        if ((done & masks[i]) != masks[i]) continue;
        if (i < n) snprintf(name, cap, "row %d", i + 1);
        else if (i < 2 * n) snprintf(name, cap, "col %d", i - n + 1);
        else snprintf(name, cap, "%s", i == 2 * n ? "diag" : "anti");
        if (cells) *cells = masks[i];
        return true;
    }
    return false;
}

uint32_t cs_bingo_toggle(cs_bingo_card_t *c, int cell) {
    if (c->n < 2 || cell < 0 || cell >= cs_bingo_cells(c)) return 0;
    const uint32_t bit = 1u << cell;
    c->done ^= bit;
    c->autod &= ~bit;
    c->pending |= bit;
    c->pend_seq[cell] = ++c->seq;
    return c->seq;
}

int cs_bingo_done_count(const cs_bingo_card_t *c) {
    const int cells = cs_bingo_cells(c);
    return cs_mines_count(c->done & (cells >= 32 ? 0xFFFFFFFFu : (1u << cells) - 1u));
}

bool cs_bingo_merge(cs_bingo_card_t *c, const cs_bingo_cmd_t *m) {
    const int cells = m->n * m->n;
    const uint32_t all = (1u << cells) - 1u;
    if (strcmp(c->day, m->day) != 0 || c->n == 0) {
        const uint32_t seq = c->seq;
        memset(c, 0, sizeof *c);
        memcpy(c->day, m->day, sizeof c->day);
        c->n = m->n;
        c->done = m->done & all;
        c->autod = m->autod & all;
        c->fixed = m->fixed & all;
        c->bingo = m->bingo;
        memcpy(c->cells, m->cells, sizeof c->cells);
        memcpy(c->whisper, m->whisper, sizeof c->whisper);
        c->seq = seq > m->ack ? seq : m->ack;
        return true;
    }
    // Same day. A new size drops all her pending changes; a swapped cell drops its own.
    bool changed = c->n != m->n;
    uint32_t pending = changed ? 0 : c->pending;
    for (int i = 0; i < cells && c->n == m->n; i++) {
        if (strcmp(c->cells[i], m->cells[i]) == 0) continue;
        pending &= ~(1u << i);
        changed = true;
    }
    for (int i = 0; i < cells; i++) {
        if ((pending >> i & 1u) && c->pend_seq[i] <= m->ack) pending &= ~(1u << i);
    }
    const uint32_t mine = c->done;
    c->n = m->n;
    c->done = ((m->done & ~pending) | (mine & pending)) & all;
    c->autod = m->autod & ~pending & all;
    c->fixed = m->fixed & all;
    c->pending = pending & all;
    for (int i = 0; i < CS_BINGO_MAX_CELLS; i++) {
        if (!(c->pending >> i & 1u)) c->pend_seq[i] = 0;
    }
    c->bingo = c->bingo || m->bingo;
    memcpy(c->cells, m->cells, sizeof c->cells);
    if (m->whisper[0]) memcpy(c->whisper, m->whisper, sizeof c->whisper);
    if (m->ack > c->seq) c->seq = m->ack;
    const int lines = cs_bingo_lines(c->done, c->n, NULL);
    if (lines < c->lines_seen) c->lines_seen = (uint8_t)lines;
    return changed;
}

static bool bingo_label_ok(const char *s, size_t len) {
    if (len == 0 || len > CS_BINGO_LABEL_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        const char ch = s[i];
        if (ch < 0x20 || ch > 0x7E || ch == ',' || ch == '"' || ch == '\\') return false;
    }
    return true;
}

bool cs_bingo_cmd_parse(const char *json, size_t len, cs_bingo_cmd_t *out) {
    csj_tok_t t[24];
    const int n = csj_parse(json, len, t, 24);
    memset(out, 0, sizeof *out);
    if (n < 1 || t[0].type != CSJ_OBJ) return false;
    char op[16];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "op"), op, sizeof op) || strcmp(op, "bingo_card") != 0) return false;
    if (!csj_str(json, t, csj_get(json, t, n, 0, "day"), out->day, sizeof out->day) ||
        strlen(out->day) != CS_BINGO_DAY_LEN) {
        return false;
    }
    int side;
    if (!csj_int(json, t, csj_get(json, t, n, 0, "n"), &side) || side < 4 || side > CS_BINGO_MAX_N) return false;
    out->n = (uint8_t)side;
    const int cells = side * side;
    const uint32_t all = (1u << cells) - 1u;
    if (!csj_u32(json, t, csj_get(json, t, n, 0, "done"), &out->done)) return false;
    (void)csj_u32(json, t, csj_get(json, t, n, 0, "auto"), &out->autod);
    (void)csj_u32(json, t, csj_get(json, t, n, 0, "fixed"), &out->fixed);
    (void)csj_u32(json, t, csj_get(json, t, n, 0, "ack"), &out->ack);
    out->done &= all;
    out->autod &= all;
    out->fixed &= all;
    const int b = csj_get(json, t, n, 0, "bingo");
    out->bingo = b >= 0 && t[b].type == CSJ_PRIM && t[b].end - t[b].start == 4 && memcmp(json + t[b].start, "true", 4) == 0;
    // "MAG,UP<10,...": exactly n*n distinct labels.
    char list[CS_BINGO_MAX_CELLS * (CS_BINGO_LABEL_MAX + 1) + 8];
    if (!csj_str(json, t, csj_get(json, t, n, 0, "cells"), list, sizeof list) ||
        strlen(list) >= sizeof list - 1) {
        return false;
    }
    int k = 0;
    for (const char *p = list;; k++) {
        const char *e = strchr(p, ',');
        const size_t l = e ? (size_t)(e - p) : strlen(p);
        if (k >= cells || !bingo_label_ok(p, l)) return false;
        memcpy(out->cells[k], p, l);
        out->cells[k][l] = '\0';
        for (int j = 0; j < k; j++) {
            if (strcmp(out->cells[j], out->cells[k]) == 0) return false;   // duplicate label
        }
        if (!e) break;
        p = e + 1;
    }
    if (k + 1 != cells) return false;
    char raw[CS_BINGO_WHISPER_MAX * 4 + 1];
    if (out->bingo && csj_str(json, t, csj_get(json, t, n, 0, "whisper"), raw, sizeof raw)) {
        ascii_reduce(raw, out->whisper, CS_BINGO_WHISPER_MAX);
    }
    return true;
}

// ---------------------------------------------------------------- astro dice
const char *const CS_ASTRO_PLANETS[CS_ASTRO_FACES] = {
    "Sun", "Moon", "Mercury", "Venus", "Mars", "Jupiter",
    "Saturn", "Uranus", "Neptune", "Pluto", "North Node", "South Node",
};
const char *const CS_ASTRO_SIGNS[CS_ASTRO_FACES] = {
    "Aries", "Taurus", "Gemini", "Cancer", "Leo", "Virgo",
    "Libra", "Scorpio", "Sagittarius", "Capricorn", "Aquarius", "Pisces",
};
const uint16_t CS_ASTRO_SETTLE_MS[CS_ASTRO_DICE] = {500, 750, 1000};

int cs_astro_face(const uint8_t *b, size_t n, size_t *used) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] < CS_ASTRO_ACCEPT) {
            if (used) *used = i + 1;
            return b[i] % CS_ASTRO_FACES;
        }
    }
    if (used) *used = n;
    return -1;
}

bool cs_astro_roll(const uint8_t *b, size_t n, uint8_t faces[CS_ASTRO_DICE]) {
    size_t at = 0;
    for (int d = 0; d < CS_ASTRO_DICE; d++) {
        size_t used;
        const int f = cs_astro_face(b + at, n - at, &used);
        if (f < 0) return false;
        faces[d] = (uint8_t)f;
        at += used;
    }
    return true;
}

int cs_astro_event_json(const uint8_t faces[CS_ASTRO_DICE], char *out, size_t cap) {
    if (faces[0] >= CS_ASTRO_FACES || faces[1] >= CS_ASTRO_FACES || faces[2] >= CS_ASTRO_FACES) return -1;
    const int n = snprintf(out, cap, "{\"type\":\"astro_roll\",\"planet\":\"%s\",\"sign\":\"%s\",\"house\":%d}",
                           CS_ASTRO_PLANETS[faces[0]], CS_ASTRO_SIGNS[faces[1]], faces[2] + 1);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

uint8_t cs_astro_anim(uint32_t elapsed, const uint8_t faces[CS_ASTRO_DICE], uint8_t seed,
                      uint8_t shown[CS_ASTRO_DICE]) {
    static const uint8_t STEP[CS_ASTRO_DICE] = {5, 7, 11};   // coprime to 12: every face comes by
    const uint32_t frame = elapsed / CS_ASTRO_FRAME_MS;
    uint8_t done = 0;
    for (int d = 0; d < CS_ASTRO_DICE; d++) {
        if (elapsed >= CS_ASTRO_SETTLE_MS[d]) {
            shown[d] = faces[d];
            done |= (uint8_t)(1u << d);
        } else {
            shown[d] = (uint8_t)((seed + d * 4u + frame * STEP[d]) % CS_ASTRO_FACES);
        }
    }
    return done;
}

void cs_astro_fx(uint32_t elapsed, int die, cs_astro_fx_t *out) {
    static const int8_t WOB[8] = {0, 2, 3, 2, 0, -2, -3, -2};
    static const uint8_t SHIM[6] = {80, 105, 135, 160, 135, 105};
    const uint32_t settle = die >= 0 && die < CS_ASTRO_DICE ? CS_ASTRO_SETTLE_MS[die] : 0;
    const uint32_t t = elapsed + (uint32_t)die * 137u;     // each die out of step with the others
    if (elapsed < settle) {
        out->opa = SHIM[(t / 45u) % 6u];
        out->dx = WOB[(t / 40u) % 8u];
        out->dy = (int8_t)(WOB[(t / 60u + 2u) % 8u] / 2);
        out->ghost = 70;
        out->glow = 255;
    } else if (elapsed < settle + CS_ASTRO_SURFACE_MS) {
        const uint32_t k = elapsed - settle;               // 0 .. SURFACE_MS - 1
        out->opa = (uint8_t)(160u + (95u * k) / CS_ASTRO_SURFACE_MS);
        out->dx = (int8_t)(WOB[(t / 40u) % 8u] * (int)(CS_ASTRO_SURFACE_MS - k) / (int)CS_ASTRO_SURFACE_MS);
        out->dy = 0;
        out->ghost = (uint8_t)(70u - (70u * k) / CS_ASTRO_SURFACE_MS);
        out->glow = 255;
    } else {
        out->opa = 255;
        out->dx = out->dy = 0;
        out->ghost = 0;
        out->glow = 255;
    }
}

void cs_astro_log_push(cs_astro_log_t log[CS_ASTRO_LOG], uint8_t *n, uint32_t t, const uint8_t f[CS_ASTRO_DICE]) {
    memmove(&log[1], &log[0], sizeof log[0] * (CS_ASTRO_LOG - 1));
    memset(&log[0], 0, sizeof log[0]);
    log[0].t = t;
    memcpy(log[0].f, f, CS_ASTRO_DICE);
    if (*n < CS_ASTRO_LOG) (*n)++;
}

uint8_t cs_astro_log_clean(cs_astro_log_t log[CS_ASTRO_LOG], uint8_t n) {
    uint8_t k = 0;
    if (n > CS_ASTRO_LOG) n = CS_ASTRO_LOG;
    for (uint8_t i = 0; i < n; i++) {
        if (log[i].f[0] < CS_ASTRO_FACES && log[i].f[1] < CS_ASTRO_FACES && log[i].f[2] < CS_ASTRO_FACES) {
            if (k != i) log[k] = log[i];
            k++;
        }
    }
    return k;
}

void cs_astro_when(uint32_t t, uint32_t now, char *out, size_t cap) {
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!t) {
        snprintf(out, cap, "—");
        return;
    }
    const uint32_t lt = t + 8u * 3600u;                      // Beijing, no DST
    const uint32_t day = lt / 86400u, sec = lt % 86400u;
    const unsigned hh = sec / 3600u, mm = sec / 60u % 60u;
    if (now) {
        const uint32_t today = (now + 8u * 3600u) / 86400u;
        if (day == today) {
            snprintf(out, cap, "Today %02u:%02u", hh, mm);
            return;
        }
        if (day + 1u == today) {
            snprintf(out, cap, "Yesterday %02u:%02u", hh, mm);
            return;
        }
    }
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    const int64_t z = (int64_t)day + 719468;
    const int64_t era = z / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, cap, "%s %u %02u:%02u", MON[m - 1], d, hh, mm);
}
