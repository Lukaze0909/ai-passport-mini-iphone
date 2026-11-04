// main/classic/cs_call.c -- 通话: walkie-talkie to the companion over MQTT.
//
// Recording (说话): cs_rec task reads 20 ms of 16 kHz mono from the ES8311,
// IMA-ADPCM encodes it straight into one of six 1512-byte packet buffers
// (12-byte header + up to 1500 bytes = 187 ms) and hands full packets to
// cs_send, which publishes them QoS 1 on passport/<user>/audio (up to 8
// waiting for PUBACK, cs_net.c). Each header carries the encoder state before
// its first sample, so a lost chunk does not desync later ones. Only these six
// buffers exist: a clip is never held in memory. If the network stalls for
// longer than the pool and the in-flight window last (~1.1 s + ~1.5 s),
// frames are dropped. cs_send writes voice_begin / voice_end itself, in line
// with the chunks, and logs one stats line per clip once the acks are in.
//
// Playback: cs_play drains a stream buffer to the speaker, 16 kHz mono, on any
// screen (toast) and regardless of the key-sound mute. Two sources ("via"):
//   http (default): the command waits until nothing plays, then cs_http (HTTPS
//          GET, CA bundle) fills the buffer. A read error mid-stream ends the
//          download; what is already buffered still plays.
//   mqtt:  the session opens in the MQTT task as the command arrives (so chunk 0,
//          sent right behind it, is not missed), with no HTTPS client at all.
//          Playchunk fragments are cut into whole 516-byte blocks and written
//          to the largest buffer the heap allows (28 KB down to 16 KB); when it
//          is full the MQTT task waits for space (up to 300 ms per block,
//          back-pressure through TCP; after one timeout the rest of that chunk
//          is dropped; it never waits while there is room). A skipped seq
//          becomes silence of one chunk's length (blocks per chunk learned
//          from the body length); the clip ends after the last chunk or 5 s
//          without one. Starts at ~3 s buffered (or the whole clip), resumes an
//          underrun at ~1.5 s. Refused while recording or while another clip
//          plays. Wi-Fi modem sleep is off while a clip plays or the call
//          screen is open. One stats line per clip (arrival rate).
// Two body formats ("fmt"):
//   pcm (http only): raw s16le; 8 KB buffer, starts once 4 KB (~0.13 s) are in.
//   adpcm: 516-byte IMA ADPCM blocks (cs_logic.h), 4x less data; http: 12 KB
//          buffer, starts at 8 KB (~1 s), on an underrun mid-clip pauses (I2S
//          plays silence) until 4 KB are buffered again or the source is done,
//          instead of dribbling out partial frames.
//
// Key handling and UI run on the shell task (cs_call_key / cs_call_event); the
// worker tasks talk back only through cs_shell_post_call().
#include "cs_call.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "cs_audio.h"
#include "cs_logic.h"
#include "cs_net.h"
#include "cs_shell.h"
#include "cs_ui.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "cs_call";

#define RATE 16000
#define FRAME 320                   // samples per I2S read (20 ms)
#define CHUNK_BYTES 1500            // ADPCM payload per packet (187 ms)
#define PKT_BYTES (CS_AUDIO_HDR_BYTES + CHUNK_BYTES)
#define POOL 6                      // ~1.1 s of audio waiting for the sender
#define ACK_DRAIN_MS 3000           // after the last chunk: wait this long for PUBACKs
#define MAX_REC_MS 60000
#define PLAY_SB_BYTES 8192
#define PLAY_START_BYTES 4096
#define ADPCM_SB_BYTES 12288
#define ADPCM_START_BYTES 8192      // ~1 s of compressed audio
#define ADPCM_RESUME_BYTES 4096     // ~0.5 s after an underrun
#define ADPCM_IN_BYTES (FRAME / 2)  // one 20 ms frame of ADPCM per read
#define ADPCM_BYTES_PER_S 8063      // 516 bytes per 1024 samples at 16 kHz
#define MQTT_START_BYTES (3 * ADPCM_BYTES_PER_S)        // ~3 s
#define MQTT_RESUME_BYTES (3 * ADPCM_BYTES_PER_S / 2)   // ~1.5 s
#define FEED_IDLE_MS 5000           // no chunk for this long: play what is buffered, end
#define FEED_BLOCK_WAIT_MS 300

enum {
    EV_POLL = CS_CALL_EV_POLL,   // shell: once a second while the call screen is open
    EV_REC_TICK,      // cs_rec: one more second recorded
    EV_REC_DONE,      // cs_rec: stopped (by key, 60 s limit or error)
    EV_PLAY_CMD,      // MQTT: a play command is pending
    EV_PLAY_START,
    EV_PLAY_END,
    EV_PLAY_FAIL,
};

typedef enum { ST_IDLE = 0, ST_REC, ST_STOPPED } state_t;

typedef struct {
    uint16_t len;     // total bytes incl. header
    bool last;
    uint8_t data[PKT_BYTES];
} pkt_t;

// ---- shared state
static pkt_t s_pool[POOL];
static QueueHandle_t s_free_q, s_send_q;     // indices into s_pool
static SemaphoreHandle_t s_clip_sent;        // given by cs_send after voice_end
// Set before xTaskCreate, cleared as the task's last action: a new take is
// refused while the previous clip's tasks are still draining.
static volatile bool s_rec_alive, s_send_alive;
static volatile bool s_rec_run;
static volatile uint32_t s_rec_ms;
static volatile uint32_t s_clip_id;
static volatile uint16_t s_chunks_sent;
static volatile uint16_t s_dropped;
static char s_clip_app[8] = "call";      // voice_begin "app": call / msg
static volatile uint16_t s_last_seq;     // seq of the clip's last chunk
static volatile uint16_t s_seq_gaps;     // seqs skipped for dropped frames
static volatile uint32_t s_rec_frames;

static SemaphoreHandle_t s_cmd_mtx;
static cs_play_cmd_t s_pending, s_playing;
static volatile bool s_have_pending, s_play_busy, s_http_done, s_play_abort;
static StreamBufferHandle_t s_sb;

// ---- MQTT playback feed. The MQTT task writes s_sb only while s_feed_open,
// holding s_feed_mtx; cs_play closes the feed under the same mutex before it
// deletes the buffer.
static SemaphoreHandle_t s_feed_mtx;
static bool s_feed_open;
static volatile bool s_feed_done;            // last chunk in, timed out or closed
static volatile uint32_t s_feed_last_ms;     // last accepted chunk (or session start)
static cs_chunk_sess_t s_sess;               // MQTT task
static cs_block_asm_t s_asm;                 // MQTT task
static bool s_chunk_take, s_feed_slow;       // MQTT task: current message accepted / congested
static unsigned s_feed_dropped;              // blocks dropped (buffer full), MQTT task
static size_t s_sb_bytes;                    // size of the current play buffer
static uint32_t s_cmd_ms;                    // play command arrival (MQTT clip)
static volatile uint32_t s_first_ms;         // first accepted chunk (0: none yet)
static volatile uint32_t s_rx_bytes;         // payload bytes of accepted chunks
static volatile uint16_t s_rx_chunks;
static volatile unsigned s_gap_blocks, s_pauses;
static volatile bool s_call_open;            // call screen shown (shell task)
static bool s_fast_on;                       // last value given to cs_net_set_fast

// ---- shell-task state
static state_t s_state;
static uint8_t s_cursor;
static bool s_muted, s_speaker;
static char s_status[160];
static char s_caption[96];
static bool s_showing_play;

static void ui_update(void);

// ---------------------------------------------------------------- events out
static void publish_event(const char *json) {
    if (!cs_net_publish_event(json)) ESP_LOGW(TAG, "event publish failed: %s", json);
}

// ---------------------------------------------------------------- recording
#define SEND_QUIT 0xFF   // tells cs_send there is no last packet coming

static void update_fast(void);

// One clip per task instance (created by start_recording, deletes itself), so
// its stack only exists while recording.
static void rec_task(void *arg) {
    (void)arg;
    static int16_t pcm[FRAME];
    {
        if (!cs_audio_acquire(RATE, 1000)) {
            ESP_LOGW(TAG, "codec busy, recording not started");
            s_rec_run = false;
            const uint8_t quit = SEND_QUIT;
            xQueueSend(s_send_q, &quit, portMAX_DELAY);
            cs_shell_post_call(EV_REC_DONE);
            s_rec_alive = false;
            update_fast();
            vTaskDelete(NULL);
        }
        cs_adpcm_state_t st;
        cs_adpcm_init(&st);                 // state reset at each clip start
        uint16_t seq = 0;
        int cur = -1;                       // pool index being filled
        size_t fill = 0;
        uint32_t samples = 0, next_tick = RATE;
        bool dropping = false;
        s_dropped = 0;
        s_seq_gaps = 0;
        while (s_rec_run) {
            if (bsp_audio_read(pcm, sizeof pcm) != ESP_OK) {
                ESP_LOGW(TAG, "mic read failed");
                break;
            }
            size_t off = 0;
            while (off < FRAME) {
                if (cur < 0) {
                    uint8_t idx;
                    if (xQueueReceive(s_free_q, &idx, 0) == pdTRUE) {
                        dropping = false;
                        cur = idx;
                        cs_audio_hdr_pack(s_pool[cur].data, s_clip_id, seq, 0, &st);
                        fill = 0;
                    } else {
                        // Network stalled and the pool is full: keep the encoder
                        // running (state stays valid) but drop these samples.
                        uint8_t scratch[FRAME / 2];
                        cs_adpcm_encode(&st, pcm + off, FRAME - off, scratch);
                        s_dropped++;
                        if (!dropping) {
                            seq++;                // leave a visible gap in the sequence
                            s_seq_gaps++;
                        }
                        dropping = true;
                        break;
                    }
                }
                size_t n = (CHUNK_BYTES - fill) * 2;
                if (n > FRAME - off) n = FRAME - off;
                fill += cs_adpcm_encode(&st, pcm + off, n, s_pool[cur].data + CS_AUDIO_HDR_BYTES + fill);
                off += n;
                if (fill == CHUNK_BYTES) {
                    s_pool[cur].len = PKT_BYTES;
                    s_pool[cur].last = false;
                    const uint8_t idx = (uint8_t)cur;
                    xQueueSend(s_send_q, &idx, portMAX_DELAY);
                    cur = -1;
                    seq++;
                }
            }
            samples += FRAME;
            s_rec_frames = samples / FRAME;
            s_rec_ms = samples / (RATE / 1000);
            if (samples >= next_tick) {
                next_tick += RATE;
                cs_shell_post_call(EV_REC_TICK);
            }
            if (s_rec_ms >= MAX_REC_MS) break;
        }
        s_rec_run = false;
        cs_audio_release();
        // Final packet (possibly empty) carries the "last" flag.
        if (cur < 0) {
            uint8_t idx;
            if (xQueueReceive(s_free_q, &idx, pdMS_TO_TICKS(2000)) == pdTRUE) {
                cur = idx;
                cs_audio_hdr_pack(s_pool[cur].data, s_clip_id, seq, 0, &st);
                fill = 0;
            }
        }
        s_last_seq = seq;
        if (cur >= 0) {
            s_pool[cur].data[6] = CS_AUDIO_FLAG_LAST;
            s_pool[cur].len = (uint16_t)(CS_AUDIO_HDR_BYTES + fill);
            s_pool[cur].last = true;
            const uint8_t idx = (uint8_t)cur;
            xQueueSend(s_send_q, &idx, portMAX_DELAY);
        } else {
            const uint8_t quit = SEND_QUIT;
            xQueueSend(s_send_q, &quit, portMAX_DELAY);
        }
        if (s_dropped) ESP_LOGW(TAG, "%u frames dropped (network slow)", (unsigned)s_dropped);
        // Stay "recording" until cs_send has published the last chunk and
        // voice_end, so a new take cannot reuse the clip id / counters early.
        if (cur >= 0 && xSemaphoreTake(s_clip_sent, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(TAG, "last chunk not confirmed sent");
        }
        cs_shell_post_call(EV_REC_DONE);
    }
    s_rec_alive = false;
    update_fast();
    vTaskDelete(NULL);
}

// chunks = chunks handed to MQTT (QoS 1: all of them arrive unless the
// connection drops); last_seq / gaps / drop_ms explain any seq numbers missing.
static void publish_voice_end(void) {
    char js[160];
    snprintf(js, sizeof js,
             "{\"type\":\"voice_end\",\"id\":%lu,\"chunks\":%u,\"ms\":%lu,\"last_seq\":%u,\"gaps\":%u,\"drop_ms\":%u}",
             (unsigned long)s_clip_id, (unsigned)s_chunks_sent, (unsigned long)s_rec_ms, (unsigned)s_last_seq,
             (unsigned)s_seq_gaps, (unsigned)s_dropped * (FRAME * 1000 / RATE));
    if (!cs_net_publish_event_now(js)) ESP_LOGW(TAG, "event publish failed: %s", js);
}

// One line per recorded clip, after its PUBACKs (or ACK_DRAIN_MS).
static void log_uplink_stats(void) {
    cs_net_audio_stats_t a;
    for (uint32_t waited = 0;; waited += 50) {
        cs_net_audio_stats(&a);
        if (!a.inflight || waited >= ACK_DRAIN_MS) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const uint32_t span = a.first_ms && a.last_ack_ms > a.first_ms ? a.last_ack_ms - a.first_ms : 0;
    ESP_LOGI(TAG, "uplink clip %lu: %lu ms, frames %lu recorded / %u dropped (%u seq gaps), chunks %u published / "
             "%u acked / %u unacked / %u expired / %u refused, max slot wait %lu ms, %lu bytes, "
             "first publish to last ack %lu ms (%lu B/s), power save %s",
             (unsigned long)s_clip_id, (unsigned long)s_rec_ms, (unsigned long)s_rec_frames, (unsigned)s_dropped,
             (unsigned)s_seq_gaps, (unsigned)a.published, (unsigned)a.acked, (unsigned)a.inflight,
             (unsigned)a.expired, (unsigned)a.refused, (unsigned long)a.max_wait_ms, (unsigned long)a.bytes,
             (unsigned long)span, (unsigned long)(span ? (uint64_t)a.bytes * 1000 / span : 0),
             cs_net_fast() ? "off" : "ON");
}

// Lives for one clip: exits after the last packet (or SEND_QUIT).
static void send_task(void *arg) {
    (void)arg;
    cs_net_audio_stats_reset();
    {
        char js[112];
        snprintf(js, sizeof js,
                 "{\"type\":\"voice_begin\",\"id\":%lu,\"codec\":\"ima_adpcm\",\"rate\":16000,\"app\":\"%s\"}",
                 (unsigned long)s_clip_id, s_clip_app);
        if (!cs_net_publish_event_now(js)) ESP_LOGW(TAG, "event publish failed: %s", js);
    }
    bool chunks = false;
    for (;;) {
        uint8_t idx;
        if (xQueueReceive(s_send_q, &idx, portMAX_DELAY) != pdTRUE) continue;
        if (idx == SEND_QUIT) {        // no last packet: still close the clip (best effort)
            publish_voice_end();
            break;
        }
        pkt_t *p = &s_pool[idx];
        chunks = true;
        if (cs_net_publish_audio(p->data, p->len)) s_chunks_sent++;
        else ESP_LOGW(TAG, "audio chunk %u not sent", (unsigned)(p->data[4] | (p->data[5] << 8)));
        const bool last = p->last;
        xQueueSend(s_free_q, &idx, portMAX_DELAY);
        if (last) {
            publish_voice_end();
            cs_net_log_heap("after clip");
            xSemaphoreGive(s_clip_sent);
            break;
        }
    }
    if (chunks) log_uplink_stats();   // waits for the last PUBACKs (power save stays off)
    s_send_alive = false;
    update_fast();
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- playback
static void http_task(void *arg) {
    (void)arg;
    bool ok = false;
    esp_http_client_config_t cfg = {
        .url = s_playing.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && esp_http_client_open(c, 0) == ESP_OK) {
        const int64_t len = esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        cs_net_log_heap("audio https open");
        if (status == 200) {
            static char buf[1024];
            ok = true;
            int total = 0;
            while (!s_play_abort) {
                const int n = esp_http_client_read(c, buf, sizeof buf);
                if (n < 0) { ok = false; break; }
                if (n == 0) break;
                total += n;
                size_t sent = 0;
                while (sent < (size_t)n && !s_play_abort) {
                    sent += xStreamBufferSend(s_sb, buf + sent, (size_t)n - sent, pdMS_TO_TICKS(500));
                }
            }
            ESP_LOGI(TAG, "audio fetched: %d bytes (content-length %lld)", total, (long long)len);
        } else {
            ESP_LOGW(TAG, "audio GET status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "audio GET failed to open");
    }
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    // A failed or cut download ends the source; cs_play still plays what is buffered.
    if (!ok) ESP_LOGW(TAG, "audio download ended early");
    s_http_done = true;
    vTaskDelete(NULL);
}

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Stops the MQTT task writing s_sb (it may be mid-push; abort makes that short).
static void feed_close(void) {
    xSemaphoreTake(s_feed_mtx, portMAX_DELAY);
    s_feed_open = false;
    s_feed_done = true;
    xSemaphoreGive(s_feed_mtx);
}

// True once nothing more will be written to s_sb.
static bool src_done(void) {
    if (s_playing.via != CS_PLAY_MQTT) return s_http_done;
    if (!s_feed_done && (uint32_t)(ms_now() - s_feed_last_ms) > FEED_IDLE_MS) {
        ESP_LOGW(TAG, "no chunk for %d s: playing what is buffered", FEED_IDLE_MS / 1000);
        feed_close();
    }
    return s_feed_done;
}

// Waits until `bytes` are buffered, the source is done or playback is aborted.
static void wait_buffered(size_t bytes) {
    while (!src_done() && !s_play_abort && xStreamBufferBytesAvailable(s_sb) < bytes) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Wi-Fi modem sleep off while the call screen is open or a clip plays.
// Called from the shell, MQTT and play tasks; s_cmd_mtx orders them.
static void update_fast(void) {
    if (!s_cmd_mtx) return;
    xSemaphoreTake(s_cmd_mtx, portMAX_DELAY);
    const bool want = s_call_open || s_play_busy || s_rec_alive || s_send_alive;
    if (want != s_fast_on) {
        s_fast_on = want;
        cs_net_set_fast(want);
    }
    xSemaphoreGive(s_cmd_mtx);
}

// One line per MQTT clip: how fast the chunks really arrived.
static void log_clip_stats(void) {
    const uint32_t last = s_feed_last_ms, first = s_first_ms;
    const uint32_t span = first ? last - first : 0;
    const uint32_t since_cmd = last - s_cmd_ms;
    ESP_LOGI(TAG, "clip %s: %u/%u chunks, %lu bytes, first chunk +%lu ms, last +%lu ms after the command, "
             "%lu B/s over the chunks (%lu B/s from the command; playback needs %d), "
             "%u pause(s), %u silent block(s) for gaps, %u block(s) dropped, buffer %u",
             s_playing.id, (unsigned)s_rx_chunks, (unsigned)s_sess.total, (unsigned long)s_rx_bytes,
             (unsigned long)(first ? first - s_cmd_ms : 0), (unsigned long)since_cmd,
             (unsigned long)(span ? (uint64_t)s_rx_bytes * 1000 / span : 0),
             (unsigned long)(since_cmd ? (uint64_t)s_rx_bytes * 1000 / since_cmd : 0), ADPCM_BYTES_PER_S,
             (unsigned)s_pauses, (unsigned)s_gap_blocks, s_feed_dropped, (unsigned)s_sb_bytes);
}

// Raw s16le: write whole samples as they arrive.
static bool play_pcm(int16_t *out) {
    bool played = false;
    size_t have = 0;
    uint8_t *bytes = (uint8_t *)out;
    while (!s_play_abort) {
        const size_t n = xStreamBufferReceive(s_sb, bytes + have, FRAME * sizeof *out - have, pdMS_TO_TICKS(200));
        have += n;
        if (n == 0 && src_done() && xStreamBufferIsEmpty(s_sb)) break;
        const size_t whole = have & ~(size_t)1;
        if (whole == FRAME * sizeof *out || (n == 0 && whole)) {
            if (bsp_audio_write(out, whole) != ESP_OK) break;
            played = true;
            if (have > whole) bytes[0] = bytes[whole];
            have -= whole;
        }
    }
    return played;
}

// ADPCM blocks: decode one 20 ms frame at a time; pause on underrun until
// `resume` bytes are buffered again.
static bool play_adpcm(int16_t *out, size_t resume) {
    static uint8_t in[ADPCM_IN_BYTES];
    cs_adpcm_stream_t dec;
    cs_adpcm_stream_init(&dec);
    bool played = false;
    unsigned underruns = 0;
    while (!s_play_abort) {
        if (!src_done() && xStreamBufferBytesAvailable(s_sb) < sizeof in) {
            underruns++;
            wait_buffered(resume);
            continue;
        }
        const size_t n = xStreamBufferReceive(s_sb, in, sizeof in, pdMS_TO_TICKS(200));
        if (n == 0) {
            if (src_done() && xStreamBufferIsEmpty(s_sb)) break;
            continue;
        }
        size_t used = 0;
        while (used < n && !s_play_abort) {
            size_t u;
            const int got = cs_adpcm_stream_feed(&dec, in + used, n - used, &u, out, FRAME);
            if (got < 0) {
                ESP_LOGW(TAG, "bad ADPCM block header: playback stopped");
                return played;
            }
            used += u;
            if (got > 0) {
                if (bsp_audio_write(out, (size_t)got * sizeof *out) != ESP_OK) return played;
                played = true;
            }
        }
    }
    if (underruns) ESP_LOGW(TAG, "playback paused %u time(s) waiting for data", underruns);
    s_pauses = underruns;
    return played;
}

// One playback per task instance (created with cs_http by maybe_start_play).
static void play_task(void *arg) {
    (void)arg;
    static int16_t out[FRAME];
    {
        bool played = false;
        const bool adpcm = s_playing.fmt == CS_PLAY_ADPCM;
        const bool mqtt = s_playing.via == CS_PLAY_MQTT;
        if (cs_audio_acquire(RATE, 3000)) {
            cs_audio_set_volume(s_speaker ? CS_AUDIO_VOLUME_LOUD : CS_AUDIO_VOLUME_NORMAL);
            // Pre-buffer so network jitter does not stutter the start.
            // MQTT: ~3 s (or as much as the buffer holds, minus room for one chunk).
            const size_t room = s_sb_bytes > 4096 ? s_sb_bytes - 4096 : s_sb_bytes / 2;
            const size_t start = mqtt ? (MQTT_START_BYTES < room ? MQTT_START_BYTES : room)
                                      : adpcm ? ADPCM_START_BYTES : PLAY_START_BYTES;
            const size_t resume = mqtt ? (MQTT_RESUME_BYTES < room ? MQTT_RESUME_BYTES : room) : ADPCM_RESUME_BYTES;
            wait_buffered(start);
            if (!s_play_abort) cs_shell_post_call(EV_PLAY_START);
            played = adpcm ? play_adpcm(out, resume) : play_pcm(out);
            memset(out, 0, sizeof out);
            (void)bsp_audio_write(out, sizeof out);
            cs_audio_release();
        }
        s_play_abort = true;   // stops the writer if it is still running (e.g. codec busy)
        if (mqtt) {
            feed_close();
            log_clip_stats();
        } else {
            while (!s_http_done) vTaskDelay(pdMS_TO_TICKS(20));   // reader owns s_sb until done
        }
        vStreamBufferDelete(s_sb);
        s_sb = NULL;
        s_play_busy = false;
        update_fast();
        cs_shell_post_call(played ? EV_PLAY_END : EV_PLAY_FAIL);
    }
    vTaskDelete(NULL);
}

// Shell task: start the pending command if nothing else holds the codec.
static void maybe_start_play(void) {
    if (s_state == ST_REC || s_rec_run) return;
    xSemaphoreTake(s_cmd_mtx, portMAX_DELAY);
    const bool have = s_have_pending && !s_play_busy;
    if (have) {
        s_playing = s_pending;
        s_have_pending = false;
        s_play_busy = true;      // claimed: the MQTT task cannot start a chunk clip now
    }
    xSemaphoreGive(s_cmd_mtx);
    if (!have) return;
    s_sb_bytes = s_playing.fmt == CS_PLAY_ADPCM ? ADPCM_SB_BYTES : PLAY_SB_BYTES;
    s_sb = xStreamBufferCreate(s_sb_bytes, 1);
    if (!s_sb) {
        ESP_LOGE(TAG, "no memory for the play buffer");
        cs_net_log_heap("play alloc failed");
        s_play_busy = false;
        return;
    }
    s_pauses = 0;
    update_fast();
    s_play_abort = false;
    s_http_done = false;
    // Output task first: if the reader cannot start, nothing else is running yet.
    if (xTaskCreate(play_task, "cs_play", 3072, NULL, 7, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the play task");
        vStreamBufferDelete(s_sb);
        s_sb = NULL;
        s_play_busy = false;
        update_fast();
        return;
    }
    snprintf(s_caption, sizeof s_caption, "%s", s_playing.text);
    ESP_LOGI(TAG, "play id=%s fmt=%s", s_playing.id, s_playing.fmt == CS_PLAY_ADPCM ? "adpcm" : "pcm");
    if (xTaskCreate(http_task, "cs_http", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the https task");
        s_play_abort = true;      // play_task sees abort + done, frees s_sb, posts FAIL
        s_http_done = true;
    }
}

// MQTT task: a via=mqtt command opens the chunk session at once.
static void start_mqtt_play(const cs_play_cmd_t *cmd) {
    if (!s_feed_mtx) return;
    xSemaphoreTake(s_cmd_mtx, portMAX_DELAY);
    const bool busy = s_play_busy || s_rec_run || s_rec_alive;
    if (!busy) s_play_busy = true;
    xSemaphoreGive(s_cmd_mtx);
    if (busy) {
        ESP_LOGW(TAG, "play id=%s refused: %s", cmd->id, s_rec_run || s_rec_alive ? "recording" : "another clip playing");
        return;
    }
    s_cmd_ms = ms_now();
    s_playing = *cmd;
    // As much buffer as the heap allows: 28 KB holds 3 s plus a chunk in flight.
    // Keep >= 24 KB free and the block 8 KB larger, for TLS records, Wi-Fi RX
    // and a 16 KB TCP window of queued segments.
    static const size_t SIZES[] = {28672, 24576, 20480, 16384};
    s_sb = NULL;
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0] && !s_sb; i++) {
        const bool last = i + 1 == sizeof SIZES / sizeof SIZES[0];
        if (!last && (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < SIZES[i] + 8192 ||
                      heap_caps_get_free_size(MALLOC_CAP_8BIT) < SIZES[i] + 24576)) {
            continue;
        }
        s_sb_bytes = SIZES[i];
        s_sb = xStreamBufferCreate(s_sb_bytes, 1);
    }
    if (!s_sb) {
        ESP_LOGE(TAG, "no memory for the play buffer");
        cs_net_log_heap("play alloc failed");
        s_play_busy = false;
        return;
    }
    s_first_ms = 0;
    s_rx_bytes = 0;
    s_rx_chunks = 0;
    s_gap_blocks = 0;
    s_pauses = 0;
    update_fast();
    s_play_abort = false;
    s_http_done = true;           // no HTTPS reader in this mode
    s_feed_done = false;
    s_feed_last_ms = ms_now();
    s_feed_dropped = 0;
    s_chunk_take = false;
    s_asm.have = 0;
    cs_chunk_sess_init(&s_sess, cmd->id, cmd->chunks);
    xSemaphoreTake(s_feed_mtx, portMAX_DELAY);
    s_feed_open = true;
    xSemaphoreGive(s_feed_mtx);
    snprintf(s_caption, sizeof s_caption, "%s", cmd->text);
    ESP_LOGI(TAG, "play id=%s fmt=adpcm via=mqtt chunks=%u bytes=%lu buffer=%u", cmd->id, (unsigned)cmd->chunks,
             (unsigned long)cmd->bytes, (unsigned)s_sb_bytes);
    cs_net_log_heap("audio mqtt open");
    if (xTaskCreate(play_task, "cs_play", 3072, NULL, 7, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the play task");
        feed_close();
        vStreamBufferDelete(s_sb);
        s_sb = NULL;
        s_play_busy = false;
        update_fast();
    }
}

// MQTT task, s_feed_mtx held: writes one whole (or the clip's short last) block.
// Waits for space up to FEED_BLOCK_WAIT_MS, once per chunk; then drops.
static void feed_push(const uint8_t *b, size_t n) {
    for (int waited = 0; xStreamBufferSpacesAvailable(s_sb) < n; waited += 10) {
        if (s_play_abort || s_feed_slow || waited >= FEED_BLOCK_WAIT_MS) {
            s_feed_slow = true;
            s_feed_dropped++;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    (void)xStreamBufferSend(s_sb, b, n, 0);   // single writer: the space is there
}

void cs_call_on_playchunk(const uint8_t *data, size_t len, size_t offset, size_t total) {
    static const uint8_t SILENT[CS_PLAY_BLOCK_BYTES];   // header 0/0 + zero codes = silence
    const bool last_fragment = offset + len >= total;
    if (!s_feed_mtx) return;
    xSemaphoreTake(s_feed_mtx, portMAX_DELAY);
    if (!s_feed_open || s_play_abort) {
        s_chunk_take = false;
        xSemaphoreGive(s_feed_mtx);
        return;
    }
    if (offset == 0) {
        const int gap = cs_chunk_accept(&s_sess, data, len, total);
        s_chunk_take = gap >= 0;
        if (!s_chunk_take) {
            xSemaphoreGive(s_feed_mtx);
            return;
        }
        s_feed_slow = false;
        s_feed_last_ms = ms_now();
        if (!s_first_ms) s_first_ms = s_feed_last_ms;
        s_rx_chunks++;
        s_rx_bytes += (uint32_t)total;
        s_asm.have = 0;           // a half-built block of a cut-off chunk is discarded
        if (gap) {
            ESP_LOGW(TAG, "chunk(s) missing before seq %u: %d silent block(s)", (unsigned)(s_sess.next - 1), gap);
            s_gap_blocks += (unsigned)gap;
        }
        for (int i = 0; i < gap && !s_feed_slow; i++) feed_push(SILENT, sizeof SILENT);
        data += CS_CHUNK_HDR;
        len -= CS_CHUNK_HDR;
    } else if (!s_chunk_take) {
        xSemaphoreGive(s_feed_mtx);
        return;
    }
    while (len) {
        const size_t n = cs_block_asm_add(&s_asm, data, len);
        data += n;
        len -= n;
        if (s_asm.have == CS_PLAY_BLOCK_BYTES) {
            feed_push(s_asm.buf, CS_PLAY_BLOCK_BYTES);
            s_asm.have = 0;
        }
    }
    if (last_fragment) {
        const bool clip_end = cs_chunk_complete(&s_sess);
        if (s_asm.have > CS_PLAY_BLOCK_HDR) {
            // Only the clip's last block may be short; pad any other to keep alignment.
            if (!clip_end) memset(s_asm.buf + s_asm.have, 0, CS_PLAY_BLOCK_BYTES - s_asm.have);
            feed_push(s_asm.buf, clip_end ? s_asm.have : CS_PLAY_BLOCK_BYTES);
        }
        s_asm.have = 0;
        s_chunk_take = false;
        s_feed_last_ms = ms_now();
        if (clip_end) {
            s_feed_open = false;
            s_feed_done = true;
            if (s_feed_dropped) ESP_LOGW(TAG, "%u block(s) dropped: playback buffer full", s_feed_dropped);
            ESP_LOGI(TAG, "all %u chunks received", (unsigned)s_sess.total);
        }
    }
    xSemaphoreGive(s_feed_mtx);
}

void cs_call_on_command(const char *json, size_t len) {
    cs_play_cmd_t cmd;
    if (!cs_play_cmd_parse(json, len, &cmd)) {
        ESP_LOGW(TAG, "command ignored (%u bytes)", (unsigned)len);
        return;
    }
    if (cmd.via == CS_PLAY_MQTT) {
        start_mqtt_play(&cmd);
        return;
    }
    xSemaphoreTake(s_cmd_mtx, portMAX_DELAY);
    s_pending = cmd;
    s_have_pending = true;      // a newer command replaces one not yet started
    xSemaphoreGive(s_cmd_mtx);
    cs_shell_post_call(EV_PLAY_CMD);
}

// ---------------------------------------------------------------- shell side
static bool enabled(void) { return cs_net_status().mqtt; }

static void fmt_secs(char *b, size_t cap, uint32_t ms) {
    const unsigned s = (unsigned)(ms / 1000);
    snprintf(b, cap, "%u:%02u", s / 60, s % 60);
}

static void set_status(void) {
    char t[12];
    if (s_state == ST_REC) {
        fmt_secs(t, sizeof t, s_rec_ms);
        snprintf(s_status, sizeof s_status, "正在说… %s", t);
    } else if (s_showing_play) {
        if (s_caption[0]) snprintf(s_status, sizeof s_status, CS_COMPANION_NAME " 正在说… %s", s_caption);
        else snprintf(s_status, sizeof s_status, CS_COMPANION_NAME " 正在说…");
    } else if (!enabled()) {
        const uint8_t link = cs_net_status().link;
        snprintf(s_status, sizeof s_status, "%s",
                 link == CS_LINK_OFF        ? "先在设置里打开 Wi-Fi"
                 : link == CS_LINK_NO_CREDS ? "未配置网络"
                 : link == CS_LINK_CONNECTING ? "Wi-Fi 连接中…"
                                             : "正在连接 " CS_COMPANION_NAME "…");
    } else if (s_state == ST_STOPPED) {
        fmt_secs(t, sizeof t, s_rec_ms);
        snprintf(s_status, sizeof s_status, "说完了 · %s — 发送 还是 删除？", t);
    } else if (!s_status[0]) {
        snprintf(s_status, sizeof s_status, "对讲中");
    }
}

static void ui_update(void) {
    if (!cs_shell_in_call_app()) return;
    cs_call_view_t v = {.cursor = s_cursor, .enabled = enabled(), .recording = s_state == ST_REC,
                        .muted = s_muted, .speaker = s_speaker};
    set_status();
    snprintf(v.status, sizeof v.status, "%s", s_status);
    if (!bsp_lvgl_lock(300)) return;
    cs_ui_call_update(&v);
    bsp_lvgl_unlock();
}

static void cancel_clip(void) {
    char js[64];
    snprintf(js, sizeof js, "{\"type\":\"voice_cancel\",\"id\":%lu}", (unsigned long)s_clip_id);
    publish_event(js);
}

static void start_recording(const char *app) {
    if (s_rec_alive || s_send_alive) {       // previous take still being sent
        snprintf(s_status, sizeof s_status, "等一下…");
        return;
    }
    snprintf(s_clip_app, sizeof s_clip_app, "%s", app);
    if (s_state == ST_STOPPED) cancel_clip();   // a new take replaces an unsent one
    s_state = ST_IDLE;
    s_clip_id = esp_random();
    s_rec_ms = 0;
    s_rec_frames = 0;
    s_chunks_sent = 0;
    s_last_seq = 0;
    xSemaphoreTake(s_clip_sent, 0);   // clear a stale give
    s_send_alive = true;              // cs_send writes voice_begin first
    update_fast();
    if (xTaskCreate(send_task, "cs_send", 5120, NULL, 5, NULL) != pdPASS) {
        s_send_alive = false;
        update_fast();
        cancel_clip();
        snprintf(s_status, sizeof s_status, "内存不够，录不了");
        cs_net_log_heap("send task alloc failed");
        return;
    }
    s_state = ST_REC;
    s_rec_run = true;
    s_status[0] = '\0';
    s_rec_alive = true;
    if (xTaskCreate(rec_task, "cs_rec", 4096, NULL, 7, NULL) != pdPASS) {
        s_rec_alive = false;
        const uint8_t quit = SEND_QUIT;
        xQueueSend(s_send_q, &quit, portMAX_DELAY);
        s_rec_run = false;
        s_state = ST_IDLE;
        cancel_clip();
        snprintf(s_status, sizeof s_status, "内存不够，录不了");
        cs_net_log_heap("rec task alloc failed");
        return;
    }
    ESP_LOGI(TAG, "recording clip %lu", (unsigned long)s_clip_id);
}

static void stop_recording(void) {
    s_rec_run = false;   // cs_rec finishes the frame, sends the last packet, posts EV_REC_DONE
}

static void finish_clip(bool send) {
    char js[64];
    snprintf(js, sizeof js, "{\"type\":\"%s\",\"id\":%lu}", send ? "voice_send" : "voice_cancel",
             (unsigned long)s_clip_id);
    publish_event(js);
    s_state = ST_IDLE;
    snprintf(s_status, sizeof s_status, "%s", send ? "已发送 ✓" : "已删除");
}

bool cs_call_key(uint8_t btn) {
    if (btn != BSP_BTN_OK) {
        s_cursor = (uint8_t)cs_call_step(s_cursor, btn == BSP_BTN_DOWN ? +1 : -1);
        ui_update();
        return false;
    }
    if (s_state == ST_REC) {            // OK on any cell stops the recording
        stop_recording();
        return false;
    }
    switch (s_cursor) {
    case CS_CALL_TALK:
        if (!enabled()) break;
        if (s_muted) {
            snprintf(s_status, sizeof s_status, "静音中，先关掉静音");
            break;
        }
        if (s_play_busy) {
            snprintf(s_status, sizeof s_status, "等 " CS_COMPANION_NAME " 说完");
            break;
        }
        start_recording("call");
        break;
    case CS_CALL_DELETE:
        if (s_state == ST_STOPPED) finish_clip(false);
        break;
    case CS_CALL_SEND:
        if (s_state == ST_STOPPED && enabled()) finish_clip(true);
        break;
    case CS_CALL_MUTE:
        s_muted = !s_muted;
        s_status[0] = '\0';
        break;
    case CS_CALL_SPEAKER:
        s_speaker = !s_speaker;
        if (s_play_busy) cs_audio_set_volume(s_speaker ? CS_AUDIO_VOLUME_LOUD : CS_AUDIO_VOLUME_NORMAL);
        break;
    case CS_CALL_END:
        return true;
    default:
        break;
    }
    ui_update();
    return false;
}

void cs_call_event(uint8_t ev) {
    switch (ev) {
    case EV_REC_DONE:
        if (s_state != ST_REC) {          // left the screen mid-take: already cancelled
            maybe_start_play();
            break;
        }
        s_state = s_rec_ms >= 300 ? ST_STOPPED : ST_IDLE;
        if (s_state == ST_IDLE) {
            // Too short (or the codec was busy): drop it on the server too.
            char js[64];
            snprintf(js, sizeof js, "{\"type\":\"voice_cancel\",\"id\":%lu}", (unsigned long)s_clip_id);
            publish_event(js);
            snprintf(s_status, sizeof s_status, "太短了，再说一次");
        }
        maybe_start_play();
        break;
    case EV_PLAY_CMD:
        maybe_start_play();
        break;
    case EV_PLAY_START:
        s_showing_play = true;
        cs_shell_wake();   // from standby: the lock screen with the toast; plays at once
        if (!cs_shell_in_call_app()) cs_shell_toast(CS_COMPANION_NAME " 来消息了");
        break;
    case EV_PLAY_END:
    case EV_PLAY_FAIL:
        s_showing_play = false;
        if (ev == EV_PLAY_FAIL) snprintf(s_status, sizeof s_status, "没收到 " CS_COMPANION_NAME " 的声音");
        else s_status[0] = '\0';
        if (!cs_shell_in_call_app()) cs_audio_set_volume(CS_AUDIO_VOLUME_NORMAL);
        maybe_start_play();   // a command that arrived meanwhile
        break;
    default:
        break;
    }
    ui_update();
    cs_shell_call_changed();   // the Messages app shows the same take
}

void cs_call_enter(void) {
    s_call_open = true;
    update_fast();
    s_cursor = CS_CALL_TALK;
    if (s_state == ST_IDLE) s_status[0] = '\0';
    cs_call_view_t v = {.cursor = s_cursor, .enabled = enabled(), .recording = s_state == ST_REC,
                        .muted = s_muted, .speaker = s_speaker};
    set_status();
    snprintf(v.status, sizeof v.status, "%s", s_status);
    cs_ui_call_update(&v);   // LVGL lock already held by the shell
}

// ---------------------------------------------------------------- shared take
// The Messages app records through the same machinery (one take at a time).
cs_rec_state_t cs_call_rec_state(void) {
    return s_state == ST_REC ? CS_REC_ON : s_state == ST_STOPPED ? CS_REC_STOPPED : CS_REC_IDLE;
}

uint32_t cs_call_rec_ms(void) { return s_rec_ms; }

uint32_t cs_call_rec_clip(void) { return s_clip_id; }

const char *cs_call_rec_note(void) { return s_status; }

bool cs_call_rec_start(const char *app) {
    if (!enabled()) {
        snprintf(s_status, sizeof s_status, "先在设置里打开 Wi-Fi");
        return false;
    }
    if (s_play_busy) {
        snprintf(s_status, sizeof s_status, "等 " CS_COMPANION_NAME " 说完");
        return false;
    }
    start_recording(app);
    return s_state == ST_REC;
}

void cs_call_rec_stop(void) {
    if (s_state == ST_REC) stop_recording();
}

bool cs_call_rec_finish(bool send) {
    if (s_state != ST_STOPPED) return false;
    if (send && !enabled()) {
        snprintf(s_status, sizeof s_status, "先在设置里打开 Wi-Fi");
        return false;
    }
    finish_clip(send);
    return true;
}

// Leaving mid-recording cancels the take; an unsent clip is dropped too.
void cs_call_rec_abandon(void) {
    if (s_state == ST_REC) stop_recording();
    if (s_state != ST_IDLE) {
        char js[64];
        snprintf(js, sizeof js, "{\"type\":\"voice_cancel\",\"id\":%lu}", (unsigned long)s_clip_id);
        publish_event(js);
    }
    s_state = ST_IDLE;
    s_status[0] = '\0';
}

void cs_call_exit(void) {
    cs_call_rec_abandon();
    if (!s_play_busy) cs_audio_set_volume(CS_AUDIO_VOLUME_NORMAL);
    s_call_open = false;
    update_fast();
}

bool cs_call_busy(void) { return s_rec_alive || s_send_alive || s_play_busy; }

void cs_call_init(void) {
    s_cmd_mtx = xSemaphoreCreateMutex();
    s_feed_mtx = xSemaphoreCreateMutex();
    s_clip_sent = xSemaphoreCreateBinary();
    s_free_q = xQueueCreate(POOL, sizeof(uint8_t));
    s_send_q = xQueueCreate(POOL, sizeof(uint8_t));
    if (!s_cmd_mtx || !s_feed_mtx || !s_clip_sent || !s_free_q || !s_send_q) {
        ESP_LOGE(TAG, "init: out of memory");
        return;
    }
    for (uint8_t i = 0; i < POOL; i++) xQueueSend(s_free_q, &i, 0);
    // Worker tasks are created per clip / per playback to keep idle RAM free.
}
