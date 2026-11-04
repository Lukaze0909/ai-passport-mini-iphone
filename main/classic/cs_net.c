// main/classic/cs_net.c -- Wi-Fi STA (home, then hotspot), SNTP and MQTT over TLS.
//
// One task owns the whole lifecycle so start/stop never race:
//   wait for "enabled" -> read NVS -> Wi-Fi init/start -> try ssid0 then ssid1
//   (12 s each) -> on success SNTP + MQTT, status every 60 s -> on disconnect
//   tear MQTT/SNTP down and retry; after a failed round wait 5 s doubling to
//   60 s -> when disabled stop + deinit Wi-Fi and free its memory.
// Secrets are never logged (only whether each key is present).
#include "cs_net.h"

#include <stdio.h>
#include <string.h>

#include "cs_logic.h"
#include "cs_ui.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"

static const char *TAG = "cs_net";

#define B_EN BIT0
#define B_IP BIT1
#define B_DISC BIT2
#define B_MQTT_UP BIT3
#define B_CHANGE BIT4

#define CONNECT_TIMEOUT_MS 12000
#define STATUS_PERIOD_MS 60000
#define RX_MAX 1536

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_mqtt_mtx;
static esp_mqtt_client_handle_t s_client;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static volatile uint8_t s_link = CS_LINK_OFF;
static volatile bool s_mqtt;
static volatile int8_t s_rssi;
static bool s_creds_known, s_have_creds;
static cs_net_msg_cb_t s_on_weather, s_on_command;
static cs_net_chunk_cb_t s_on_chunk;
static cs_net_batt_cb_t s_batt;
static char s_boot[9];

static char s_ssid[2][33], s_pass[2][65];
static char s_uri[128], s_user[64], s_mpass[128];
static char s_t_status[96], s_t_event[96], s_t_audio[96], s_t_weather[96], s_t_cmd[96], s_t_chunk[96];

static char s_rx[RX_MAX + 1];
static size_t s_rx_len;
static int s_rx_topic;   // 1 weather, 2 command, 3 playchunk, 0 other

// Uplink audio chunks waiting for PUBACK. An ack can be processed by the MQTT
// task before the publisher has recorded the msg id, so recent unknown acks are
// remembered too (event acks land there as well and age out). A slot also
// frees itself after INFLIGHT_MAX_MS: esp-mqtt keeps retrying the message, but
// with MQTT_REPORT_DELETED_MESSAGES off a lost ack would otherwise hold the
// slot until the next disconnect.
#define EARLY_ACKS 8
#define INFLIGHT_MAX_MS 3000
static portMUX_TYPE s_fl_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_fl_id[CS_NET_AUDIO_INFLIGHT];
static uint32_t s_fl_ms[CS_NET_AUDIO_INFLIGHT];
static int s_fl_n;
static cs_net_audio_stats_t s_ast;   // under s_fl_mux
static volatile bool s_fast;   // Wi-Fi modem sleep off (cs_net_set_fast)

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static int s_early[EARLY_ACKS];
static unsigned s_early_pos;

static void inflight_reset(void) {
    taskENTER_CRITICAL(&s_fl_mux);
    s_fl_n = 0;
    for (int i = 0; i < EARLY_ACKS; i++) s_early[i] = -1;
    taskEXIT_CRITICAL(&s_fl_mux);
}

static int inflight_count(void) {
    const uint32_t now = now_ms();
    int expired = 0;
    taskENTER_CRITICAL(&s_fl_mux);
    for (int i = 0; i < s_fl_n;) {
        if ((uint32_t)(now - s_fl_ms[i]) >= INFLIGHT_MAX_MS) {
            s_fl_n--;
            s_fl_id[i] = s_fl_id[s_fl_n];
            s_fl_ms[i] = s_fl_ms[s_fl_n];
            expired++;
            s_ast.expired++;
        } else {
            i++;
        }
    }
    const int n = s_fl_n;
    taskEXIT_CRITICAL(&s_fl_mux);
    if (expired) ESP_LOGW(TAG, "audio: %d chunk(s) unacked for %d s, slot freed", expired, INFLIGHT_MAX_MS / 1000);
    return n;
}

static void inflight_add(int id) {
    taskENTER_CRITICAL(&s_fl_mux);
    bool acked = false;
    for (int i = 0; i < EARLY_ACKS; i++) {
        if (s_early[i] == id) {
            s_early[i] = -1;
            acked = true;
            break;
        }
    }
    if (acked) {
        s_ast.acked++;
        s_ast.last_ack_ms = now_ms();
    } else if (s_fl_n < CS_NET_AUDIO_INFLIGHT) {
        s_fl_id[s_fl_n] = id;
        s_fl_ms[s_fl_n++] = now_ms();
    }
    taskEXIT_CRITICAL(&s_fl_mux);
}

// MQTT task: PUBLISHED (acked) or DELETED (outbox gave up) for msg id.
static void inflight_done(int id) {
    taskENTER_CRITICAL(&s_fl_mux);
    bool found = false;
    for (int i = 0; i < s_fl_n; i++) {
        if (s_fl_id[i] == id) {
            s_fl_n--;
            s_fl_id[i] = s_fl_id[s_fl_n];
            s_fl_ms[i] = s_fl_ms[s_fl_n];
            found = true;
            s_ast.acked++;
            s_ast.last_ack_ms = now_ms();
            break;
        }
    }
    if (!found) s_early[s_early_pos++ % EARLY_ACKS] = id;
    taskEXIT_CRITICAL(&s_fl_mux);
}

void cs_net_log_heap(const char *what) {
    ESP_LOGI(TAG, "heap %s: free %u, largest %u, min %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static bool enabled(void) { return (xEventGroupGetBits(s_ev) & B_EN) != 0; }

// ---------------------------------------------------------------- NVS
static bool get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    size_t len = cap;
    out[0] = '\0';
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

static bool load_creds(void) {
    nvs_handle_t h;
    bool any = false;
    memset(s_ssid, 0, sizeof s_ssid);
    memset(s_pass, 0, sizeof s_pass);
    s_uri[0] = s_user[0] = s_mpass[0] = '\0';
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
        const bool h0 = get_str(h, "ssid0", s_ssid[0], sizeof s_ssid[0]);
        get_str(h, "pass0", s_pass[0], sizeof s_pass[0]);
        const bool h1 = get_str(h, "ssid1", s_ssid[1], sizeof s_ssid[1]);
        get_str(h, "pass1", s_pass[1], sizeof s_pass[1]);
        const bool hu = get_str(h, "mqtt_uri", s_uri, sizeof s_uri);
        const bool hn = get_str(h, "mqtt_user", s_user, sizeof s_user);
        const bool hp = get_str(h, "mqtt_pass", s_mpass, sizeof s_mpass);
        nvs_close(h);
        any = h0 || h1;
        ESP_LOGI(TAG, "nvs net: home=%d hotspot=%d mqtt_uri=%d mqtt_user=%d mqtt_pass=%d", h0, h1, hu, hn, hp);
    } else {
        ESP_LOGW(TAG, "nvs namespace 'net' missing: no network configured");
    }
    if (s_user[0]) {
        snprintf(s_t_status, sizeof s_t_status, "passport/%s/status", s_user);
        snprintf(s_t_event, sizeof s_t_event, "passport/%s/event", s_user);
        snprintf(s_t_audio, sizeof s_t_audio, "passport/%s/audio", s_user);
        snprintf(s_t_weather, sizeof s_t_weather, "passport/%s/weather", s_user);
        snprintf(s_t_cmd, sizeof s_t_cmd, "passport/%s/command", s_user);
        snprintf(s_t_chunk, sizeof s_t_chunk, "passport/%s/playchunk", s_user);
    }
    s_creds_known = true;
    s_have_creds = any;
    return any;
}

bool cs_net_have_credentials(void) {
    if (!s_creds_known) {
        // Peek without keeping secrets around longer than needed.
        nvs_handle_t h;
        bool any = false;
        if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
            size_t len = 0;
            any = (nvs_get_str(h, "ssid0", NULL, &len) == ESP_OK && len > 1) ||
                  (nvs_get_str(h, "ssid1", NULL, &len) == ESP_OK && len > 1);
            nvs_close(h);
        }
        return any;
    }
    return s_have_creds;
}

// ---------------------------------------------------------------- events
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const wifi_event_sta_disconnected_t *d = data;
    ESP_LOGI(TAG, "wifi disconnected (reason %d)", d ? d->reason : -1);
    xEventGroupSetBits(s_ev, B_DISC);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    xEventGroupSetBits(s_ev, B_IP);
}

// ---------------------------------------------------------------- MQTT
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt = true;
        esp_mqtt_client_subscribe(e->client, s_t_weather, 1);
        esp_mqtt_client_subscribe(e->client, s_t_cmd, 1);
        esp_mqtt_client_subscribe(e->client, s_t_chunk, 1);
        xEventGroupSetBits(s_ev, B_MQTT_UP);
        cs_net_log_heap("after TLS/MQTT connect");
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_mqtt) ESP_LOGW(TAG, "mqtt disconnected");
        s_mqtt = false;
        inflight_reset();
        break;
    case MQTT_EVENT_PUBLISHED:
    case MQTT_EVENT_DELETED:
        inflight_done(e->msg_id);
        break;
    case MQTT_EVENT_DATA:
        if (e->current_data_offset == 0) {
            s_rx_len = 0;
            s_rx_topic = 0;
            if (e->topic_len == (int)strlen(s_t_weather) && memcmp(e->topic, s_t_weather, e->topic_len) == 0) {
                s_rx_topic = 1;
            } else if (e->topic_len == (int)strlen(s_t_cmd) && memcmp(e->topic, s_t_cmd, e->topic_len) == 0) {
                s_rx_topic = 2;
            } else if (e->topic_len == (int)strlen(s_t_chunk) && memcmp(e->topic, s_t_chunk, e->topic_len) == 0) {
                s_rx_topic = 3;
            }
        }
        if (s_rx_topic == 3) {   // streamed straight through, never copied whole
            if (s_on_chunk && e->data_len > 0) {
                s_on_chunk((const uint8_t *)e->data, (size_t)e->data_len, (size_t)e->current_data_offset,
                           (size_t)e->total_data_len);
            }
        } else if (s_rx_topic && e->total_data_len <= RX_MAX) {
            memcpy(s_rx + e->current_data_offset, e->data, (size_t)e->data_len);
            s_rx_len = (size_t)(e->current_data_offset + e->data_len);
            if (s_rx_len == (size_t)e->total_data_len) {
                s_rx[s_rx_len] = '\0';
                if (s_rx_topic == 1 && s_on_weather) s_on_weather(s_rx, s_rx_len);
                if (s_rx_topic == 2 && s_on_command) s_on_command(s_rx, s_rx_len);
            }
        } else if (s_rx_topic && e->current_data_offset == 0) {
            ESP_LOGW(TAG, "message too large (%d bytes), ignored", e->total_data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle) {
            ESP_LOGW(TAG, "mqtt error type %d, tls 0x%x, sock errno %d, refused %d",
                     e->error_handle->error_type, (unsigned)e->error_handle->esp_tls_last_esp_err,
                     e->error_handle->esp_transport_sock_errno, e->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static void mqtt_start(void) {
    if (!s_uri[0] || !s_user[0]) {
        ESP_LOGW(TAG, "mqtt not configured (mqtt_uri / mqtt_user missing)");
        return;
    }
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = s_user,
        .credentials.client_id = s_user,
        .credentials.authentication.password = s_mpass,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 8000,
        // esp-mqtt re-sends a QoS 1 message whose PUBACK is not back within this
        // time, on the same TCP connection, which already delivers it. The 1 s
        // default doubled the uplink on a slow trans-Pacific path just when it
        // was congested.
        .session.message_retransmit_timeout = 8000,
        .buffer.size = 2048,
        .buffer.out_size = 2048,       // audio packet: 12 + 1500 bytes + topic
        .task.stack_size = 6144,
    };
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "mqtt init failed");
        return;
    }
    esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    xSemaphoreTake(s_mqtt_mtx, portMAX_DELAY);
    s_client = c;
    xSemaphoreGive(s_mqtt_mtx);
    if (esp_mqtt_client_start(c) != ESP_OK) ESP_LOGE(TAG, "mqtt start failed");
}

static void mqtt_stop(void) {
    xSemaphoreTake(s_mqtt_mtx, portMAX_DELAY);
    esp_mqtt_client_handle_t c = s_client;
    s_client = NULL;
    s_mqtt = false;
    xSemaphoreGive(s_mqtt_mtx);
    if (c) {
        esp_mqtt_client_stop(c);
        esp_mqtt_client_destroy(c);
    }
}

// queue = true: hand the message to the MQTT task's outbox and return at once
// (callers may hold the LVGL lock); false: send now, blocking up to the network
// timeout (the audio sender wants that back-pressure). Returns the msg id
// (0 for QoS 0) or -1.
static int publish(const char *topic, const void *data, size_t len, int qos, bool queue) {
    int id = -1;
    if (xSemaphoreTake(s_mqtt_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    if (s_client && s_mqtt) {
        id = queue ? esp_mqtt_client_enqueue(s_client, topic, data, (int)len, qos, 0, true)
                   : esp_mqtt_client_publish(s_client, topic, data, (int)len, qos, 0);
    }
    xSemaphoreGive(s_mqtt_mtx);
    return id;
}

bool cs_net_publish_event(const char *json) { return publish(s_t_event, json, strlen(json), 1, true) >= 0; }
bool cs_net_publish_event_now(const char *json) { return publish(s_t_event, json, strlen(json), 1, false) >= 0; }

bool cs_net_publish_audio(const void *data, size_t len) {
    const uint32_t t0 = now_ms();
    uint32_t waited = 0;
    while (inflight_count() >= CS_NET_AUDIO_INFLIGHT) {
        waited = now_ms() - t0;
        if (waited >= 1000 || !s_mqtt) {
            ESP_LOGW(TAG, "audio: %d chunks unacked for 1 s, chunk dropped", CS_NET_AUDIO_INFLIGHT);
            taskENTER_CRITICAL(&s_fl_mux);
            s_ast.refused++;
            if (waited > s_ast.max_wait_ms) s_ast.max_wait_ms = waited;
            taskEXIT_CRITICAL(&s_fl_mux);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const uint32_t t1 = now_ms();
    const int id = publish(s_t_audio, data, len, 1, false);
    taskENTER_CRITICAL(&s_fl_mux);
    if (t1 - t0 > s_ast.max_wait_ms) s_ast.max_wait_ms = t1 - t0;
    if (id >= 0) {
        if (!s_ast.first_ms) s_ast.first_ms = t1;
        s_ast.published++;
        s_ast.bytes += (uint32_t)len;
    }
    taskEXIT_CRITICAL(&s_fl_mux);
    if (id > 0) inflight_add(id);
    return id >= 0;
}

void cs_net_audio_stats_reset(void) {
    taskENTER_CRITICAL(&s_fl_mux);
    memset(&s_ast, 0, sizeof s_ast);
    taskEXIT_CRITICAL(&s_fl_mux);
}

void cs_net_audio_stats(cs_net_audio_stats_t *out) {
    const int n = inflight_count();   // also frees expired slots
    taskENTER_CRITICAL(&s_fl_mux);
    *out = s_ast;
    taskEXIT_CRITICAL(&s_fl_mux);
    out->inflight = (uint8_t)n;
}

bool cs_net_fast(void) { return s_fast; }

static void publish_status(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    char js[160];
    snprintf(js, sizeof js, "{\"boot\":\"%s\",\"fw\":\"classic-1\",\"net\":\"%s\",\"rssi\":%d,\"batt\":%d,\"heap\":%u}",
             s_boot, s_link == CS_LINK_HOTSPOT ? "hotspot" : "home", s_rssi, s_batt ? s_batt() : -1,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (publish(s_t_status, js, strlen(js), 0, false) < 0) ESP_LOGW(TAG, "status publish failed");
}

// ---------------------------------------------------------------- Wi-Fi
static esp_err_t wifi_up(void) {
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
        return e;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, &s_h_ip);
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e == ESP_OK && s_fast) cs_net_set_fast(true);
    cs_net_log_heap("after wifi start");
    return e;
}

// Default STA power save (modem sleep) wakes only for beacons: the AP holds
// downlink frames meanwhile, which adds up to a beacon interval to every TCP
// round trip. Off while voice is streaming. BLE is never started by the shell,
// so Wi-Fi / BT coexistence does not forbid WIFI_PS_NONE.
void cs_net_set_fast(bool on) {
    s_fast = on;
    const esp_err_t e = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (e == ESP_OK) ESP_LOGI(TAG, "wifi power save %s", on ? "off (voice)" : "on");
    else if (e != ESP_ERR_WIFI_NOT_INIT && e != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi power save: %s", esp_err_to_name(e));
    }
}

static void wifi_down(void) {
    if (s_h_wifi) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_h_wifi);
    if (s_h_ip) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
    s_h_wifi = s_h_ip = NULL;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    cs_net_log_heap("after wifi off");
}

static bool try_connect(int idx) {
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, s_ssid[idx], strnlen(s_ssid[idx], sizeof wc.sta.ssid));
    memcpy(wc.sta.password, s_pass[idx], strnlen(s_pass[idx], sizeof wc.sta.password));
    wc.sta.threshold.authmode = s_pass[idx][0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, B_IP | B_DISC);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    ESP_LOGI(TAG, "connecting to %s network", idx ? "hotspot" : "home");
    const EventBits_t b = xEventGroupWaitBits(s_ev, B_IP | B_DISC | B_CHANGE, pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (b & B_IP) return true;
    esp_wifi_disconnect();
    // After a timeout the STA_DISCONNECTED event arrives late; wait for it here
    // so it cannot land after the next attempt cleared B_DISC and fail it at once.
    if (!(b & B_DISC)) xEventGroupWaitBits(s_ev, B_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    xEventGroupClearBits(s_ev, B_DISC);
    return false;
}

// Sleeps up to ms; returns early when the switch changes.
static void wait_change(uint32_t ms) {
    xEventGroupWaitBits(s_ev, B_CHANGE, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
}

static void net_task(void *arg) {
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_ev, B_EN, pdFALSE, pdTRUE, portMAX_DELAY);
        xEventGroupClearBits(s_ev, B_CHANGE);
        if (!load_creds()) {
            s_link = CS_LINK_NO_CREDS;
            while (enabled()) wait_change(60000);
            s_link = CS_LINK_OFF;
            continue;
        }
        s_link = CS_LINK_CONNECTING;
        if (wifi_up() != ESP_OK) {
            ESP_LOGE(TAG, "wifi start failed");
            wifi_down();
            wait_change(5000);
            continue;
        }
        unsigned attempt = 0;
        while (enabled()) {
            int got = -1;
            for (int idx = 0; idx < 2 && got < 0 && enabled(); idx++) {
                if (s_ssid[idx][0] && try_connect(idx)) got = idx;
            }
            if (!enabled()) break;
            if (got < 0) {
                const uint32_t ms = cs_backoff_ms(attempt++);
                ESP_LOGI(TAG, "no network, retry in %u s", (unsigned)(ms / 1000));
                wait_change(ms);
                continue;
            }
            attempt = 0;
            s_link = got ? CS_LINK_HOTSPOT : CS_LINK_HOME;
            cs_net_log_heap("after wifi got ip");
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
                2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "ntp.aliyun.com"));
            const bool sntp = esp_netif_sntp_init(&sc) == ESP_OK;
            mqtt_start();
            int64_t next_status = 0;
            for (;;) {
                int64_t now = esp_timer_get_time() / 1000;
                const uint32_t wait = next_status && next_status > now ? (uint32_t)(next_status - now)
                                                                       : STATUS_PERIOD_MS;
                const EventBits_t b = xEventGroupWaitBits(s_ev, B_DISC | B_CHANGE | B_MQTT_UP, pdTRUE,
                                                          pdFALSE, pdMS_TO_TICKS(wait));
                if (!enabled() || (b & B_DISC)) break;
                now = esp_timer_get_time() / 1000;
                if ((b & B_MQTT_UP) || (next_status && now >= next_status)) {
                    if (s_mqtt) publish_status();
                    next_status = now + STATUS_PERIOD_MS;
                }
            }
            mqtt_stop();
            if (sntp) esp_netif_sntp_deinit();
            s_link = enabled() ? CS_LINK_CONNECTING : CS_LINK_OFF;
        }
        wifi_down();
        s_link = CS_LINK_OFF;
    }
}

// ---------------------------------------------------------------- API
esp_err_t cs_net_init(cs_net_msg_cb_t on_weather, cs_net_msg_cb_t on_command, cs_net_chunk_cb_t on_chunk,
                      cs_net_batt_cb_t batt) {
    s_on_weather = on_weather;
    s_on_command = on_command;
    s_on_chunk = on_chunk;
    inflight_reset();
    s_batt = batt;
    snprintf(s_boot, sizeof s_boot, "%08lx", (unsigned long)esp_random());
    s_ev = xEventGroupCreate();
    s_mqtt_mtx = xSemaphoreCreateMutex();
    if (!s_ev || !s_mqtt_mtx) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    if (xTaskCreate(net_task, "cs_net", 5120, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void cs_net_enable(bool on) {
    if (!s_ev) return;
    if (on) xEventGroupSetBits(s_ev, B_EN | B_CHANGE);
    else {
        xEventGroupClearBits(s_ev, B_EN);
        xEventGroupSetBits(s_ev, B_CHANGE);
    }
}

cs_net_status_t cs_net_status(void) {
    cs_net_status_t st = {.link = s_link, .mqtt = s_mqtt, .rssi = s_rssi};
    return st;
}
