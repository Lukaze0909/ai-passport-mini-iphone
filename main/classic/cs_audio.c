// main/classic/cs_audio.c -- codec arbitration and the pomodoro chime.
#include "cs_audio.h"

#include <math.h>
#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cs_audio";

#define CHIME_RATE 16000
#define CHUNK 320              // 20 ms
#define PATTERN_MS 1800
#define TABLE_BITS 8

static SemaphoreHandle_t s_mtx;
static uint32_t s_rate;
static bool s_ok;
static TaskHandle_t s_chime_task;
static volatile bool s_chime_run, s_chime_active;
static int16_t s_sine[1 << TABLE_BITS];

bool cs_audio_ok(void) { return s_ok; }

bool cs_audio_acquire(uint32_t rate_hz, uint32_t timeout_ms) {
    if (!s_ok || !s_mtx) return false;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    if (s_rate != rate_hz) {
        const esp_err_t e = bsp_audio_set_format(rate_hz, 16, 1);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "set_format %u failed: %s", (unsigned)rate_hz, esp_err_to_name(e));
            s_rate = 0;
            xSemaphoreGive(s_mtx);
            return false;
        }
        s_rate = rate_hz;
    }
    return true;
}

void cs_audio_release(void) {
    if (s_mtx) xSemaphoreGive(s_mtx);
}

bool cs_audio_hook_acquire(uint32_t rate_hz) { return cs_audio_acquire(rate_hz, 60); }
void cs_audio_hook_release(void) { cs_audio_release(); }

void cs_audio_set_volume(uint8_t percent) {
    if (s_ok) bsp_audio_set_volume(percent);
}

// ---------------------------------------------------------------- chime
typedef struct {
    uint32_t start;        // sample offset in the pattern
    uint32_t inc;          // phase increment (Q32 of a table turn)
    float amp;
} voice_t;

static const struct { uint16_t ms; float hz; float amp; } NOTES[] = {
    {0, 1318.5f, 6500.f}, {0, 659.25f, 1800.f},
    {170, 1568.0f, 6000.f}, {170, 784.0f, 1600.f},
    {340, 2093.0f, 5200.f}, {340, 1046.5f, 1800.f},
};
#define NOTE_COUNT (sizeof NOTES / sizeof NOTES[0])

static void chime_task(void *arg) {
    (void)arg;
    static int16_t buf[CHUNK];
    voice_t v[NOTE_COUNT];
    for (size_t i = 0; i < NOTE_COUNT; i++) {
        v[i].start = (uint32_t)NOTES[i].ms * CHIME_RATE / 1000;
        v[i].inc = (uint32_t)((double)NOTES[i].hz * 4294967296.0 / CHIME_RATE);
        v[i].amp = NOTES[i].amp;
    }
    const float decay = expf(-1.0f / (0.38f * CHIME_RATE));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_chime_run) continue;
        if (!cs_audio_acquire(CHIME_RATE, 3000)) {
            ESP_LOGW(TAG, "chime: codec busy");
            s_chime_run = false;
            s_chime_active = false;
            continue;
        }
        const uint32_t pattern = PATTERN_MS * CHIME_RATE / 1000;
        for (int rep = 0; rep < CS_CHIME_REPEATS && s_chime_run; rep++) {
            uint32_t phase[NOTE_COUNT] = {0};
            float env[NOTE_COUNT];
            for (size_t i = 0; i < NOTE_COUNT; i++) env[i] = v[i].amp;
            for (uint32_t off = 0; off < pattern && s_chime_run; off += CHUNK) {
                for (int k = 0; k < CHUNK; k++) {
                    const uint32_t t = off + (uint32_t)k;
                    float acc = 0;
                    for (size_t i = 0; i < NOTE_COUNT; i++) {
                        if (t < v[i].start) continue;
                        acc += env[i] * s_sine[phase[i] >> (32 - TABLE_BITS)] * (1.0f / 32767.f);
                        phase[i] += v[i].inc;
                        env[i] *= decay;
                    }
                    if (acc > 32000.f) acc = 32000.f;
                    if (acc < -32000.f) acc = -32000.f;
                    buf[k] = (int16_t)acc;
                }
                if (bsp_audio_write(buf, sizeof buf) != ESP_OK) {
                    s_chime_run = false;
                    break;
                }
            }
        }
        memset(buf, 0, sizeof buf);
        (void)bsp_audio_write(buf, sizeof buf);   // flush the DMA with silence
        cs_audio_release();
        s_chime_run = false;
        s_chime_active = false;
    }
}

void cs_audio_chime_start(void) {
    if (!s_chime_task) return;
    s_chime_run = true;
    s_chime_active = true;
    xTaskNotifyGive(s_chime_task);
}

void cs_audio_chime_stop(void) { s_chime_run = false; }

bool cs_audio_chime_active(void) { return s_chime_active; }

esp_err_t cs_audio_init(bool codec_ok) {
    s_ok = codec_ok;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    for (int i = 0; i < (1 << TABLE_BITS); i++) {
        s_sine[i] = (int16_t)(32767.0f * sinf(6.2831853f * (float)i / (1 << TABLE_BITS)));
    }
    if (!s_chime_task && xTaskCreate(chime_task, "cs_chime", 3072, NULL, 6, &s_chime_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
