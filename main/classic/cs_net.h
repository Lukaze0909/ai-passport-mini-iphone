// main/classic/cs_net.h -- Wi-Fi STA (home, then hotspot), SNTP and MQTT over TLS.
//
// Runs only while the settings switch is on; switching off stops and deinits
// Wi-Fi completely. Credentials and broker settings come from NVS namespace
// "net" (ssid0/pass0, ssid1/pass1, mqtt_uri, mqtt_user, mqtt_pass); nothing is
// compiled in. Topics live under passport/<mqtt_user>/.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t link;     // cs_link_t (cs_ui.h)
    bool mqtt;
    int8_t rssi;
} cs_net_status_t;

// Incoming retained weather JSON / command JSON (called from the MQTT task;
// copy what you need and return quickly).
typedef void (*cs_net_msg_cb_t)(const char *data, size_t len);
// One fragment of a playchunk message (MQTT task; must not block for long):
// offset / total are the fragment's place in the whole payload.
typedef void (*cs_net_chunk_cb_t)(const uint8_t *data, size_t len, size_t offset, size_t total);
// Battery percent for the status message (-1 unknown).
typedef int (*cs_net_batt_cb_t)(void);

esp_err_t cs_net_init(cs_net_msg_cb_t on_weather, cs_net_msg_cb_t on_command, cs_net_chunk_cb_t on_chunk,
                      cs_net_batt_cb_t batt);
void cs_net_enable(bool on);
cs_net_status_t cs_net_status(void);
bool cs_net_have_credentials(void);

// Publish under passport/<user>/<sub>. Return false when not connected.
bool cs_net_publish_event(const char *json);                     // QoS 1, queued
// QoS 1, written now from the caller's task (blocks up to the network timeout):
// keeps voice_begin / voice_end in order with the audio chunks. esp-mqtt sends
// queued messages only one per loop pass with up to 1 s polls in between.
bool cs_net_publish_event_now(const char *json);
// QoS 1, binary, sent at once. At most CS_NET_AUDIO_INFLIGHT chunks wait for
// their PUBACK (each holds a copy in the MQTT outbox, ~1.5 KB); when the window
// is full this waits up to 1 s for an ack, then gives up (returns false).
#define CS_NET_AUDIO_INFLIGHT 8
bool cs_net_publish_audio(const void *data, size_t len);
// Uplink audio counters since the last reset (one clip).
typedef struct {
    uint16_t published, acked, expired, refused;   // refused: no slot within 1 s
    uint32_t bytes;
    uint32_t max_wait_ms;                           // longest wait for a free slot
    uint32_t first_ms, last_ack_ms;                 // esp_timer ms; 0 = none
    uint8_t inflight;                               // still waiting for PUBACK
} cs_net_audio_stats_t;
void cs_net_audio_stats_reset(void);
void cs_net_audio_stats(cs_net_audio_stats_t *out);
bool cs_net_fast(void);   // modem sleep currently off

// Wi-Fi modem sleep off (true) while voice streams, back on (false) after;
// remembered across Wi-Fi restarts. Any task.
void cs_net_set_fast(bool on);

// Logs free heap and the largest free block with a tag.
void cs_net_log_heap(const char *what);
