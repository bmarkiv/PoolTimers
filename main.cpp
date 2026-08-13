#include "../WifiManager/WifiManager.h"
#include "index_html_gz.h"

#define MAX_DURATION_ON    5 * 3600 * 1000  // 5 hours
#define PIN_BUTTON         14
#define PIN_RELAY          27
#define PIN_REFILL_RELAY   25
#define PIN_BLUE_LED       26

static unsigned long c = 0;
static bool last_button_state = HIGH;
static unsigned long last_button_time = 0;
static unsigned long last_led_toggle_time = 0;
static bool led_state = LOW;
static String ws_msg;

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
    bool output_state = LOW;
    ScheduleConfig cfg;
    uint32_t manual_duration_sec = MAX_DURATION_ON / 1000;
    unsigned long switch_off_time = 0;
    long last_schedule_local_day = -1;
    long last_schedule_checked_sec = -1;

    Channel(Kind k, const char *ns, const char *lbl, uint8_t p)
        : kind(k), prefs_ns(ns), label(lbl), pin(p) {}
};

static AsyncWebSocket ws("/ws");

static Channel channels[] = {
    { Channel::FILTER, "schedule", "Filter", PIN_RELAY },
    { Channel::REFILL, "refill",   "Refill", PIN_REFILL_RELAY }
};
static constexpr size_t CHANNEL_COUNT = sizeof(channels) / sizeof(channels[0]);
static Channel &filter_ch = channels[Channel::FILTER];
static Channel &refill_ch = channels[Channel::REFILL];

// Forward declarations
void sendChannelState(Channel &ch, unsigned long now);
void sendChannelStateToClient(Channel &ch, AsyncWebSocketClient *client, unsigned long now);
int64_t now_utc_sec();
int find_char(const char *str, char c);
void set_channel_output(Channel &ch, bool on);

AsyncWebServer server(80);
WifiManager::Options wifi_options;
WifiManager wifiManager(server, [] {
    WifiManager::Options options;
    options.enableStatusWebSocket = false;
    return options;
}());

// -------------------- IO Setup --------------------
void setup_io() {
    for (Channel &channel : channels) {
        pinMode(channel.pin, OUTPUT);
        digitalWrite(channel.pin, LOW);
    }
    pinMode(PIN_BLUE_LED,     OUTPUT); digitalWrite(PIN_BLUE_LED,     LOW);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

void set_channel_output(Channel &ch, bool on) {
    if (ch.output_state == on) return;
    ch.output_state = on;
    digitalWrite(ch.pin, on ? HIGH : LOW);
    app_log("%s state changed: %s", ch.label, on ? "ON" : "OFF");
}

void sendChannelStateToClients(Channel &ch, unsigned long now) {
    for (auto *client : ws.getClients()) {
        if (client->_tempObject == &ch) {
            sendChannelStateToClient(ch, client, now);
        }
    }
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
    Preferences p;
    p.begin(ch.prefs_ns, false);
    ch.cfg.enabled      = p.getBool("enabled", false);
    ch.cfg.days_mask    = (uint8_t)p.getUChar("days", 0x00);
    ch.cfg.start_minute = (uint16_t)p.getUInt("start", 8 * 60);
    ch.cfg.duration_min = (uint16_t)p.getUInt("dur", 120);
    ch.manual_duration_sec = p.getUInt("manual_dur", MAX_DURATION_ON / 1000);
    p.end();
    if (ch.cfg.start_minute > 1439) ch.cfg.start_minute = 0;
    if (ch.cfg.duration_min == 0)   ch.cfg.duration_min = 1;
    if (ch.cfg.duration_min > 720)  ch.cfg.duration_min = 720;
    if (ch.manual_duration_sec == 0) ch.manual_duration_sec = MAX_DURATION_ON / 1000;
    if (ch.manual_duration_sec > 12 * 3600) ch.manual_duration_sec = 12 * 3600;
}

void save_channel_schedule(Channel &ch) {
    Preferences p;
    p.begin(ch.prefs_ns, false);
    p.putBool("enabled", ch.cfg.enabled);
    p.putUChar("days",   ch.cfg.days_mask);
    p.putUInt("start",   ch.cfg.start_minute);
    p.putUInt("dur",     ch.cfg.duration_min);
    p.putUInt("manual_dur", ch.manual_duration_sec);
    p.end();
}

// -------------------- Channel helpers --------------------
static void buildStateJson(Channel &ch, unsigned long now) {
    auto local_sec = now_local_sec();
    String wifi_ssid = WiFi.SSID();
    String wifi_ip = WiFi.localIP().toString();
    ws_msg  = "{";
    ws_msg += "\"remaining_time\":\""     + String((ch.switch_off_time > now) ? (ch.switch_off_time - now) / 1000 : 0) + "\"";
    ws_msg += ",\"manual_duration_sec\":\"" + String(ch.manual_duration_sec)  + "\"";
    ws_msg += ",\"uptime\":\""            + String(now / 1000)                + "\"";
    ws_msg += ",\"uptimeSeconds\":"       + String((unsigned long)(now / 1000));
    ws_msg += ",\"wifi_ssid\":\""         + wifi_ssid                         + "\"";
    ws_msg += ",\"wifi_ip\":\""           + wifi_ip                           + "\"";
    ws_msg += ",\"ssid\":\""              + wifi_ssid                         + "\"";
    ws_msg += ",\"ip\":\""                + wifi_ip                           + "\"";
    ws_msg += ",\"schedule_enabled\":\""  + String(ch.cfg.enabled ? 1 : 0)    + "\"";
    ws_msg += ",\"schedule_days\":\""     + String(ch.cfg.days_mask)           + "\"";
    ws_msg += ",\"schedule_start\":\""    + String(ch.cfg.start_minute)        + "\"";
    ws_msg += ",\"schedule_duration\":\"" + String(ch.cfg.duration_min)        + "\"";
    ws_msg += ",\"time_synced\":\""       + String(utc_synced ? 1 : 0)         + "\"";
    ws_msg += ",\"tz_offset_min\":\""     + String(tz_offset_min)              + "\"";
    ws_msg += ",\"local_epoch\":\""       + String(local_sec)                  + "\"";
    ws_msg += "}";
}

void sendChannelStateToClient(Channel &ch, AsyncWebSocketClient *client, unsigned long now) {
    buildStateJson(ch, now);
    client->text(ws_msg);
}

void sendChannelState(Channel &ch, unsigned long now) {
    buildStateJson(ch, now);
    for (auto *client : ws.getClients()) {
        if (client->_tempObject == &ch) client->text(ws_msg);
    }
}


int find_char(const char *str, char c) {
    for (int i = 0; str[i]; i++) if (str[i] == c) return i;
    return -1;
}

void handleChannelMessage(Channel &ch, char *msg) {
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
        dur = min(12L * 3600L, dur);
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
        ch.manual_duration_sec = (uint32_t)min(12L * 3600L, dur);
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
        memcpy(epoch_buf, payload, min((int)sizeof(epoch_buf) - 1, p1));
        int64_t epoch_utc = atoll(epoch_buf);
        int32_t tz_min = atoi(payload + p1 + 1);
        if (ntp_time_valid && tz_offset_min == tz_min) return;
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
        memcpy(enabled_buf, payload,          min((int)sizeof(enabled_buf) - 1, c1));
        memcpy(days_buf,    payload + c1 + 1, min((int)sizeof(days_buf)    - 1, c2 - c1 - 1));
        memcpy(start_buf,   payload + c2 + 1, min((int)sizeof(start_buf)   - 1, c3 - c2 - 1));

        ScheduleConfig proposed;
        proposed.enabled      = atoi(enabled_buf) != 0;
        proposed.days_mask    = (uint8_t)(atoi(days_buf) & 0x7F);
        proposed.start_minute = (uint16_t)max(0, min(1439, atoi(start_buf)));
        proposed.duration_min = (uint16_t)max(1, min(720,  atoi(payload + c3 + 1)));

        ch.cfg = proposed;
        ch.last_schedule_local_day = -1;
        save_channel_schedule(ch);
        app_log("%s schedule saved: enabled=%d days=%u start=%u duration=%u",
            ch.label, ch.cfg.enabled ? 1 : 0, ch.cfg.days_mask, ch.cfg.start_minute, ch.cfg.duration_min);
        for (auto *client : ws.getClients()) {
            if (client->_tempObject == &ch) client->text("notice:Schedule saved");
        }
        sendChannelState(ch, millis());
        return;
    }

    app_log("%s unsupported command: %s", ch.label, msg);
}

void check_channel_schedule(Channel &ch, unsigned long now_ms) {
    if (!ch.cfg.enabled || !utc_synced) return;
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
void handleWebSocketMessage(Channel &ch, void *arg, char *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        char msg[128];
        len = min(len, sizeof(msg) - 1);
        memcpy(msg, data, len);
        msg[len] = 0;
        app_log("WS[%s]: '%s'", ch.label, msg);
        handleChannelMessage(ch, msg);
    } else {
        app_log("WS[%s] unsupported frame: final:%d index:%d len:%d opcode:%d",
            ch.label, info->final, info->index, info->len, info->opcode);
    }
}

void onWebSocketEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            client->_tempObject = nullptr;
            app_log("WS #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            app_log("WS #%u disconnected", client->id());
            break;
        case WS_EVT_DATA:
            if (arg == nullptr) break;
            {
                AwsFrameInfo *info = (AwsFrameInfo *)arg;
                if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) break;
                char msg[128];
                len = min(len, sizeof(msg) - 1);
                memcpy(msg, data, len);
                msg[len] = 0;
                if (strncmp(msg, "channel:filter", 14) == 0) {
                    client->_tempObject = &channels[Channel::FILTER];
                    sendChannelStateToClient(channels[Channel::FILTER], client, millis());
                    client->text("channel_ready:filter");
                } else if (strncmp(msg, "channel:refill", 14) == 0) {
                    client->_tempObject = &channels[Channel::REFILL];
                    sendChannelStateToClient(channels[Channel::REFILL], client, millis());
                    client->text("channel_ready:refill");
                } else if (client->_tempObject != nullptr) {
                    Channel &ch = *(Channel *)client->_tempObject;
                    handleChannelMessage(ch, msg);
                } else {
                    app_log("WS #%u has no channel", client->id());
                }
            }
            break;
        default: break;
    }
}

// -------------------- Web Server --------------------
void handleNotFound(AsyncWebServerRequest *request) {
    app_log("Not found: %s", request->url().c_str());
    request->send(404, "text/plain", "Not found");
}

void serve_index(AsyncWebServerRequest *request, const char *cache_control) {
    if (request->hasHeader("If-None-Match") &&
        request->header("If-None-Match") == String(index_html_gz_etag)) {
        request->send(304);
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse_P(
        200, "text/html", index_html_gz, index_html_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", cache_control);
    response->addHeader("ETag", index_html_gz_etag);
    request->send(response);
}

void start_web_server() {
    const char *refill_paths[] = { "/refill", "/refill/" };
    for (const char *path : refill_paths) {
        server.on(path, HTTP_GET, [](AsyncWebServerRequest *r) {
            serve_index(r, "no-store");
        });
    }
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "Restarting...");
        delay(500);
        ESP.restart();
    });
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
}

// -------------------- Main Loop --------------------
void setup(void) {
    ws_msg.reserve(256);
    setup_io();
    Serial.begin(115200);
    for (Channel &channel : channels) load_channel_schedule(channel);
    load_timezone();
    wifiManager.setHomeHandler([](AsyncWebServerRequest *request) {
        serve_index(request, "max-age=86400");
    });
    wifiManager.setNotFoundHandler(handleNotFound);
    wifiManager.setStatusHandler([]() {
        auto now = millis();
        for (Channel &channel : channels) sendChannelState(channel, now);
    });
    wifiManager.begin();
    start_web_server();
}

void loop(void) {
    delay(2);
    wifiManager.loop();
    c++;
    if (c % 100 == 0) {
        ws.cleanupClients();
    }

    auto now = millis();
    ensure_ntp_sync();
    for (Channel &channel : channels) check_channel_schedule(channel, now);

    bool button = digitalRead(PIN_BUTTON);
    if (button == LOW && last_button_state == HIGH && now - last_button_time > 200) {
        last_button_time = now;
        bool is_on = filter_ch.switch_off_time > 0;
        filter_ch.switch_off_time = is_on ? 0 : now + MAX_DURATION_ON;
        set_channel_output(filter_ch, !is_on);
        sendChannelState(filter_ch, now);
        app_log("Button pressed — toggled switch");
    }
    last_button_state = button;

    for (Channel &channel : channels) stopExpiredChannel(channel, now);

    if (WiFi.status() != WL_CONNECTED) {
        if (now - last_led_toggle_time >= 500) {
            led_state = !led_state;
            digitalWrite(PIN_BLUE_LED, led_state);
            last_led_toggle_time = now;
        }
    } else {
        digitalWrite(PIN_BLUE_LED, LOW);
    }
}
