#include "WifiManager.h"
#include "WebServer.h"
#include "WebSocket.h"
#include "TimeUtils.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define PROGMEM
#include "index_html_gz.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define MAX_DURATION_ON    5 * 3600 * 1000  // 5 hours
#define PIN_BUTTON         14
#define PIN_RELAY          27
#define PIN_REFILL_RELAY   25
#define PIN_BLUE_LED       26

static bool last_button_state = true;
static unsigned long last_button_time = 0;
static unsigned long last_led_toggle_time = 0;
static bool led_state = false;
static std::string ws_msg;

static uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static void app_log(const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    ESP_LOGI("PoolTimers", "%s", message);
}

struct ScheduleConfig {
    bool enabled = false;
    uint8_t days_mask = 0x00;       // bit0=Sun ... bit6=Sat
    uint16_t start_minute = 8 * 60; // 08:00
    uint16_t duration_min = 120;    // 2h
};


// -------------------- Channel --------------------
// Groups all per-relay state so filter and refill share one code path.
struct Channel {
    enum Kind { FILTER, REFILL };

    Kind kind;
    const char *prefs_ns;   // NVS namespace
    const char *label;      // for log messages
    uint8_t pin;            // relay GPIO
    bool output_state = false;
    ScheduleConfig cfg;
    uint32_t manual_duration_sec = MAX_DURATION_ON / 1000;
    unsigned long switch_off_time = 0;
    long last_schedule_local_day = -1;
    long last_schedule_checked_sec = -1;

    Channel(Kind k, const char *ns, const char *lbl, uint8_t p)
        : kind(k), prefs_ns(ns), label(lbl), pin(p) {}
};

static WebSocket ws("/ws");
static std::map<int, Channel *> ws_channels;

static Channel channels[] = {
    { Channel::FILTER, "schedule", "Filter", PIN_RELAY },
    { Channel::REFILL, "refill",   "Refill", PIN_REFILL_RELAY }
};
static constexpr size_t CHANNEL_COUNT = sizeof(channels) / sizeof(channels[0]);
static Channel &filter_ch = channels[Channel::FILTER];
static Channel &refill_ch = channels[Channel::REFILL];

// Forward declarations
void sendChannelState(Channel &ch, unsigned long now);
void sendChannelStateToClient(Channel &ch, int client_id, unsigned long now);
int64_t now_utc_sec();
int find_char(const char *str, char c);
void set_channel_output(Channel &ch, bool on);

static WebServer wifiServer([] {
    WebServer::Options options;
    options.max_uri_handlers = 20;
    options.root_page_gzip = index_html_gz;
    options.root_page_gzip_size = index_html_gz_len;
    return options;
}());
static WifiManager wifiManager(wifiServer);

// -------------------- IO Setup --------------------
void setup_io() {
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << PIN_RELAY) | (1ULL << PIN_REFILL_RELAY) | (1ULL << PIN_BLUE_LED);
    outputs.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&outputs));
    for (Channel &channel : channels) {
        ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(channel.pin), 0));
    }
    ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(PIN_BLUE_LED), 0));

    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << PIN_BUTTON;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
}

void set_channel_output(Channel &ch, bool on) {
    if (ch.output_state == on) return;
    ch.output_state = on;
    gpio_set_level(static_cast<gpio_num_t>(ch.pin), on ? 1 : 0);
    app_log("%s state changed: %s", ch.label, on ? "ON" : "OFF");
}

void stopExpiredChannel(Channel &ch, unsigned long now) {
    if (!ch.switch_off_time || now <= ch.switch_off_time) return;
    ch.switch_off_time = 0;
    set_channel_output(ch, false);
    sendChannelState(ch, now);
    app_log("%s switched off due to timeout", ch.label);
}

bool minute_in_wrapped_range(int minute, int start, int end_exclusive) {
    if (start < end_exclusive) return minute >= start && minute < end_exclusive;
    return minute >= start || minute < end_exclusive;
}

bool schedule_active_at_minute(const ScheduleConfig &cfg, int week_minute) {
    if (!cfg.enabled || cfg.days_mask == 0) return false;
    for (int d = 0; d < 7; d++) {
        if (((cfg.days_mask >> d) & 0x01) == 0) continue;
        int start = d * 1440 + (int)cfg.start_minute;
        int end_exclusive = start + (int)cfg.duration_min;
        int m = week_minute;
        if (end_exclusive <= 10080) {
            if (m >= start && m < end_exclusive) return true;
        } else {
            int wrapped_end = end_exclusive - 10080;
            if (minute_in_wrapped_range(m, start, wrapped_end)) return true;
        }
    }
    return false;
}

// -------------------- Schedule persistence --------------------
void load_channel_schedule(Channel &ch) {
    nvs_handle_t handle;
    if (nvs_open(ch.prefs_ns, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t enabled = 0;
        uint8_t days = 0;
        uint32_t start = 8 * 60;
        uint32_t duration = 120;
        uint32_t manual_duration = MAX_DURATION_ON / 1000;
        nvs_get_u8(handle, "enabled", &enabled);
        nvs_get_u8(handle, "days", &days);
        nvs_get_u32(handle, "start", &start);
        nvs_get_u32(handle, "dur", &duration);
        nvs_get_u32(handle, "manual_dur", &manual_duration);
        nvs_close(handle);
        ch.cfg.enabled = enabled != 0;
        ch.cfg.days_mask = days;
        ch.cfg.start_minute = static_cast<uint16_t>(start);
        ch.cfg.duration_min = static_cast<uint16_t>(duration);
        ch.manual_duration_sec = manual_duration;
    }
    if (ch.cfg.start_minute > 1439) ch.cfg.start_minute = 0;
    if (ch.cfg.duration_min == 0)   ch.cfg.duration_min = 1;
    if (ch.cfg.duration_min > 720)  ch.cfg.duration_min = 720;
    if (ch.manual_duration_sec == 0) ch.manual_duration_sec = MAX_DURATION_ON / 1000;
    if (ch.manual_duration_sec > 12 * 3600) ch.manual_duration_sec = 12 * 3600;
}

void save_channel_schedule(Channel &ch) {
    nvs_handle_t handle;
    if (nvs_open(ch.prefs_ns, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "enabled", ch.cfg.enabled ? 1 : 0);
    nvs_set_u8(handle, "days", ch.cfg.days_mask);
    nvs_set_u32(handle, "start", ch.cfg.start_minute);
    nvs_set_u32(handle, "dur", ch.cfg.duration_min);
    nvs_set_u32(handle, "manual_dur", ch.manual_duration_sec);
    nvs_commit(handle);
    nvs_close(handle);
}

// -------------------- Channel helpers --------------------
static std::string current_wifi_ip() {
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (sta_netif == nullptr || esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) return "";
    char ip[16] = {};
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    return ip;
}

static void buildStateJson(Channel &ch, unsigned long now) {
    auto local_sec = now_local_sec();
    const auto wifi = wifiManager.status();
    const std::string wifi_ssid = wifi.connected ? wifi.ssid : "";
    const std::string wifi_ip = wifi.connected ? current_wifi_ip() : "";
    cJSON *state = cJSON_CreateObject();
    if (state == nullptr) return;
    const auto remaining = (ch.switch_off_time > now) ? (ch.switch_off_time - now) / 1000 : 0;
    const auto uptime_seconds = now / 1000;
    cJSON_AddStringToObject(state, "remaining_time", std::to_string(remaining).c_str());
    cJSON_AddStringToObject(state, "manual_duration_sec", std::to_string(ch.manual_duration_sec).c_str());
    cJSON_AddStringToObject(state, "uptime", std::to_string(uptime_seconds).c_str());
    cJSON_AddNumberToObject(state, "uptimeSeconds", uptime_seconds);
    cJSON_AddStringToObject(state, "wifi_ssid", wifi_ssid.c_str());
    cJSON_AddStringToObject(state, "wifi_ip", wifi_ip.c_str());
    cJSON_AddStringToObject(state, "ssid", wifi_ssid.c_str());
    cJSON_AddStringToObject(state, "ip", wifi_ip.c_str());
    cJSON_AddStringToObject(state, "schedule_enabled", ch.cfg.enabled ? "1" : "0");
    cJSON_AddStringToObject(state, "schedule_days", std::to_string(ch.cfg.days_mask).c_str());
    cJSON_AddStringToObject(state, "schedule_start", std::to_string(ch.cfg.start_minute).c_str());
    cJSON_AddStringToObject(state, "schedule_duration", std::to_string(ch.cfg.duration_min).c_str());
    cJSON_AddStringToObject(state, "time_synced", now_utc_sec() >= 0 ? "1" : "0");
    cJSON_AddStringToObject(state, "tz_offset_min", std::to_string(timezone_offset_minutes()).c_str());
    cJSON_AddStringToObject(state, "local_epoch", std::to_string(local_sec).c_str());
    char *serialized = cJSON_PrintUnformatted(state);
    if (serialized != nullptr) {
        ws_msg.assign(serialized);
        cJSON_free(serialized);
    }
    cJSON_Delete(state);
}

void sendChannelStateToClient(Channel &ch, int client_id, unsigned long now) {
    buildStateJson(ch, now);
    ws.send(client_id, ws_msg);
}

void sendChannelState(Channel &ch, unsigned long now) {
    buildStateJson(ch, now);
    for (const auto &entry : ws_channels) {
        if (entry.second == &ch) ws.send(entry.first, ws_msg);
    }
}


int find_char(const char *str, char c) {
    for (int i = 0; str[i]; i++) if (str[i] == c) return i;
    return -1;
}

void handleChannelMessage(Channel &ch, const char *msg) {
    if ((ch.kind == Channel::FILTER && strncmp(msg, "filter:", 7) == 0) ||
        (ch.kind == Channel::REFILL && strncmp(msg, "refill:", 7) == 0)) {
        msg += 7;
    }
    int colon = find_char(msg, ':');
    if (colon < 0) { app_log("%s unsupported command: %s", ch.label, msg); return; }

    if (strncmp(msg, "set_duration:", 13) == 0) {
        auto now = millis();
        const char *p = msg + colon + 1;
        long dur = atol(p);
        if (dur < 0) dur = 0;
        dur = std::min(12L * 3600L, dur);
        ch.switch_off_time = 0;
        set_channel_output(ch, false);
        if (dur > 0) {
            ch.manual_duration_sec = (uint32_t)dur;
            save_channel_schedule(ch);
            ch.switch_off_time = now + dur * 1000;
            set_channel_output(ch, true);
        }
        app_log("%s duration_on: %ld", ch.label, dur);
        sendChannelState(ch, now);
        return;
    }

    if (strncmp(msg, "set_manual_duration:", 20) == 0) {
        const char *payload = msg + 20;
        long dur = atol(payload);
        if (dur <= 0) dur = MAX_DURATION_ON / 1000;
        ch.manual_duration_sec = (uint32_t)std::min(12L * 3600L, dur);
        save_channel_schedule(ch);
        app_log("%s manual duration saved: %lu", ch.label, (unsigned long)ch.manual_duration_sec);
        sendChannelState(ch, millis());
        return;
    }

    if (strncmp(msg, "get_state:", 10) == 0) {
        sendChannelState(ch, millis());
        return;
    }

    if (strncmp(msg, "sync_time:", 10) == 0) {
        const char *payload = msg + 10;
        int p1 = find_char(payload, ':');
        if (p1 < 0) return;
        char epoch_buf[24] = {0};
        memcpy(epoch_buf, payload, std::min((int)sizeof(epoch_buf) - 1, p1));
        int64_t epoch_utc = atoll(epoch_buf);
        int32_t tz_min = atoi(payload + p1 + 1);
        if (now_utc_sec() >= 0 && timezone_offset_minutes() == tz_min) return;
        bool updated = sync_time(epoch_utc, tz_min);
        if (updated) {
            // Broadcast updated time to both channels.
            auto now = millis();
            for (Channel &channel : channels) sendChannelState(channel, now);
        }
        return;
    }

    if (strncmp(msg, "set_schedule:", 13) == 0) {
        const char *payload = msg + 13;
        int c1 = find_char(payload, ':');           if (c1 < 0) return;
        int c2 = find_char(payload + c1 + 1, ':'); if (c2 < 0) return; c2 += c1 + 1;
        int c3 = find_char(payload + c2 + 1, ':'); if (c3 < 0) return; c3 += c2 + 1;

        char enabled_buf[8] = {0}, days_buf[8] = {0}, start_buf[8] = {0};
        memcpy(enabled_buf, payload,          std::min((int)sizeof(enabled_buf) - 1, c1));
        memcpy(days_buf,    payload + c1 + 1, std::min((int)sizeof(days_buf)    - 1, c2 - c1 - 1));
        memcpy(start_buf,   payload + c2 + 1, std::min((int)sizeof(start_buf)   - 1, c3 - c2 - 1));

        ScheduleConfig proposed;
        proposed.enabled      = atoi(enabled_buf) != 0;
        proposed.days_mask    = (uint8_t)(atoi(days_buf) & 0x7F);
        proposed.start_minute = (uint16_t)std::max(0, std::min(1439, atoi(start_buf)));
        proposed.duration_min = (uint16_t)std::max(1, std::min(720,  atoi(payload + c3 + 1)));

        ch.cfg = proposed;
        ch.last_schedule_local_day = -1;
        save_channel_schedule(ch);
        app_log("%s schedule saved: enabled=%d days=%u start=%u duration=%u",
            ch.label, ch.cfg.enabled ? 1 : 0, ch.cfg.days_mask, ch.cfg.start_minute, ch.cfg.duration_min);
        for (const auto &entry : ws_channels) {
            if (entry.second == &ch) ws.send(entry.first, "notice:Schedule saved");
        }
        sendChannelState(ch, millis());
        return;
    }

    app_log("%s unsupported command: %s", ch.label, msg);
}

void check_channel_schedule(Channel &ch, unsigned long now_ms) {
    if (!ch.cfg.enabled || now_utc_sec() < 0) return;
    auto local_sec = now_local_sec();
    if (local_sec < 0) return;
    if (local_sec == ch.last_schedule_checked_sec) return;
    ch.last_schedule_checked_sec = (long)local_sec;

    long sec_of_day = (long)(local_sec % 86400);
    if (sec_of_day < 0) sec_of_day += 86400;
    int day_of_week = (int)(((local_sec / 86400) + 4) % 7);  // 1970-01-01 = Thu(4)
    if (day_of_week < 0) day_of_week += 7;
    long local_day    = (long)(local_sec / 86400);
    long minute_of_day = sec_of_day / 60;

    if (((ch.cfg.days_mask >> day_of_week) & 0x01) == 0) return;
    if (minute_of_day != ch.cfg.start_minute) return;
    if ((sec_of_day % 60) != 0) return;
    if (ch.last_schedule_local_day == local_day) return;

    ch.last_schedule_local_day = local_day;
    ch.switch_off_time = now_ms + (unsigned long)ch.cfg.duration_min * 60UL * 1000UL;
    set_channel_output(ch, true);
    app_log("%s schedule trigger: dow=%d start_min=%u duration_min=%u",
        ch.label, day_of_week, ch.cfg.start_minute, ch.cfg.duration_min);
    sendChannelState(ch, now_ms);
}

// -------------------- WebSocket events --------------------
void handleWebSocketText(int client_id, const std::string &message) {
    if (message == "channel:filter") {
        ws_channels[client_id] = &filter_ch;
        sendChannelStateToClient(filter_ch, client_id, millis());
        ws.send(client_id, "channel_ready:filter");
        return;
    }
    if (message == "channel:refill") {
        ws_channels[client_id] = &refill_ch;
        sendChannelStateToClient(refill_ch, client_id, millis());
        ws.send(client_id, "channel_ready:refill");
        return;
    }
    auto channel = ws_channels.find(client_id);
    if (channel == ws_channels.end() || channel->second == nullptr || message.size() >= 128) return;
    app_log("WS[%s]: '%s'", channel->second->label, message.c_str());
    handleChannelMessage(*channel->second, message.c_str());
}

// -------------------- Web Server --------------------
esp_err_t refill_page_handler(httpd_req_t *request) {
    char etag[sizeof(index_html_gz_etag)] = {};
    if (httpd_req_get_hdr_value_str(request, "If-None-Match", etag, sizeof(etag)) == ESP_OK &&
        strcmp(etag, index_html_gz_etag) == 0) {
        httpd_resp_set_status(request, "304 Not Modified");
        return httpd_resp_send(request, nullptr, 0);
    }
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "ETag", index_html_gz_etag);
    return httpd_resp_send(request, reinterpret_cast<const char *>(index_html_gz), index_html_gz_len);
}

esp_err_t restart_handler(httpd_req_t *request) {
    httpd_resp_sendstr(request, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

void register_app_routes() {
    ws.onConnect([](int client_id) { ws_channels[client_id] = nullptr; });
    ws.onDisconnect([](int client_id) { ws_channels.erase(client_id); });
    ws.onText(handleWebSocketText);
    ESP_ERROR_CHECK(ws.registerWith(wifiServer));

    httpd_uri_t refill_route = {};
    refill_route.uri = "/refill";
    refill_route.method = HTTP_GET;
    refill_route.handler = refill_page_handler;
    ESP_ERROR_CHECK(wifiServer.registerRoute(refill_route));

    httpd_uri_t refill_slash_route = {};
    refill_slash_route.uri = "/refill/";
    refill_slash_route.method = HTTP_GET;
    refill_slash_route.handler = refill_page_handler;
    ESP_ERROR_CHECK(wifiServer.registerRoute(refill_slash_route));

    httpd_uri_t restart_route = {};
    restart_route.uri = "/restart";
    restart_route.method = HTTP_GET;
    restart_route.handler = restart_handler;
    ESP_ERROR_CHECK(wifiServer.registerRoute(restart_route));

    ESP_ERROR_CHECK(wifiServer.registerDeferredRoutes());
}

// -------------------- Main Loop --------------------
extern "C" void app_main(void) {
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    setup_io();
    for (Channel &channel : channels) load_channel_schedule(channel);
    load_timezone();
    wifiManager.initializeNetworkStack();
    wifiManager.setWifiStatusHandler([](const WifiManager::WifiStatus &status) {
        const std::string ip = status.connected ? current_wifi_ip() : "";
        ESP_LOGI("PoolTimers", "Wi-Fi %s: SSID=%s IP=%s",
                 status.connected ? "connected" : "disconnected",
                 status.ssid.c_str(), ip.c_str());
    });
    wifiManager.begin();
    register_app_routes();

    uint32_t last_wifi_poll = 0;
    bool wifi_connected = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const auto now = millis();
        if (now - last_wifi_poll >= 1000) {
            ensure_ntp_sync();
            wifi_connected = wifiManager.status().connected;
            last_wifi_poll = now;
        }
        for (Channel &channel : channels) check_channel_schedule(channel, now);

        const bool button = gpio_get_level(static_cast<gpio_num_t>(PIN_BUTTON)) != 0;
        if (!button && last_button_state && now - last_button_time > 200) {
            last_button_time = now;
            const bool is_on = filter_ch.switch_off_time > 0;
            filter_ch.switch_off_time = is_on ? 0 : now + MAX_DURATION_ON;
            set_channel_output(filter_ch, !is_on);
            sendChannelState(filter_ch, now);
            app_log("Button pressed: toggled switch");
        }
        last_button_state = button;

        for (Channel &channel : channels) stopExpiredChannel(channel, now);

        if (!wifi_connected) {
            if (now - last_led_toggle_time >= 500) {
                led_state = !led_state;
                gpio_set_level(static_cast<gpio_num_t>(PIN_BLUE_LED), led_state);
                last_led_toggle_time = now;
            }
        } else {
            gpio_set_level(static_cast<gpio_num_t>(PIN_BLUE_LED), 0);
        }
    }
}
