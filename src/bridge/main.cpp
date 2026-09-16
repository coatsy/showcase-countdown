// ESP-NOW bridge: a spare stick powered from the router's USB port. It joins
// the event WiFi, subscribes to showcase/bridge/cmd on the broker, and
// rebroadcasts each command as an ESP-NOW frame, so sticks whose MQTT session
// has dropped still get the room lock and the fire signal. Being associated
// with the AP puts it on the AP's channel, which is where the sticks listen.
// See docs/messaging.md, "ESP-NOW".
//
//   MQTT showcase/bridge/cmd, or a line on USB serial:
//     lock | unlock | fire <epoch>     -> broadcast three times, acked
//     ?                                -> status on serial
//
// Build and flash with: pio run -e bridge -t upload --upload-port <port>

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mqtt_client.h>
#include <string.h>

#include "env_config.h"
#include "../espnow_relay.h"
#include "../mqtt_transport.h"

namespace {

constexpr const char* MAGIC = "SHOWCASE1 ";
constexpr int REPEATS = 3;  // broadcast frames are unacknowledged; say it thrice
constexpr int REPEAT_GAP_MS = 25;
constexpr const char* CMD_TOPIC = "showcase/bridge/cmd";
constexpr const char* STATE_TOPIC = "showcase/bridge/state";
constexpr uint32_t STATE_MS = 10000;

const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const bool MQTT_ENABLED = MQTT_URI[0] != '\0';

uint32_t sent = 0;
char lastCommand[40] = "none";
char line[4097];
size_t lineLength = 0;
bool lineOverflow = false;
bool espNowReady = false;
esp_mqtt_client_handle_t mqtt = nullptr;
volatile bool mqttConnected = false;
uint32_t lastStateMs = 0;

// MQTT commands arrive on the client task; hand them to loop().
char pendingCommand[64];
volatile bool commandPending = false;

void draw() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.drawString("ESP-NOW BRIDGE", 4, 4);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(4, 26);
    M5.Display.printf("wifi    : %s\n", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "---");
    M5.Display.printf("channel : %d\n", WiFi.channel());
    M5.Display.printf("mqtt    : %s\n", mqttConnected ? "connected" : "---");
    M5.Display.printf("sent    : %lu\n", static_cast<unsigned long>(sent));
    M5.Display.printf("last    : %s\n", lastCommand);
}

void publishState() {
    if (!mqttConnected) {
        return;
    }
    char out[128];
    const int n = snprintf(out, sizeof(out), "{\"online\":true,\"sent\":%lu,\"last\":\"%s\",\"channel\":%d}",
                           static_cast<unsigned long>(sent), lastCommand, WiFi.channel());
    esp_mqtt_client_publish(mqtt, STATE_TOPIC, out, n, 1, 1);
}

void onSent(const uint8_t*, esp_now_send_status_t status) {
    Serial.printf("tx %s\n", status == ESP_NOW_SEND_SUCCESS ? "ok" : "FAIL");
}

void startEspNow() {
    if (espNowReady) {
        return;
    }
    if (esp_now_init() != ESP_OK) {
        Serial.println("err esp_now_init failed");
        return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;  // whatever channel the station is on
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    espNowReady = true;
}

bool broadcast(const char* body) {
    char frame[64];
    const int n = snprintf(frame, sizeof(frame), "%s%s", MAGIC, body);
    if (n <= 0 || n >= static_cast<int>(sizeof(frame)) || !espNowReady) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < REPEATS; ++i) {
        const esp_err_t err = esp_now_send(BROADCAST, reinterpret_cast<const uint8_t*>(frame), n);
        if (err != ESP_OK) {
            Serial.printf("err esp_now_send: %s\n", esp_err_to_name(err));
            ok = false;
        }
        delay(REPEAT_GAP_MS);
    }
    return ok;
}

void handle(const char* command, const char* source) {
    if (command[0] == '?' || command[0] == '\0') {
        Serial.printf("bridge channel=%d sent=%lu last=%s mqtt=%s\n", WiFi.channel(),
                      static_cast<unsigned long>(sent), lastCommand, mqttConnected ? "yes" : "no");
        return;
    }
    const bool known = strcmp(command, "lock") == 0 || strcmp(command, "unlock") == 0 ||
                       strncmp(command, "fire ", 5) == 0;
    if (!known) {
        Serial.printf("err unknown command from %s: %s\n", source, command);
        return;
    }
    const bool ok = broadcast(command);
    if (ok) {
        ++sent;
        strncpy(lastCommand, command, sizeof(lastCommand) - 1);
        lastCommand[sizeof(lastCommand) - 1] = '\0';
    }
    Serial.printf("%s %s (%s)\n", ok ? "ack" : "err", command, source);
    publishState();
    draw();
}

void onMqttEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
    auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
        case MQTT_EVENT_CONNECTED:
            mqttConnected = true;
            esp_mqtt_client_subscribe(mqtt, CMD_TOPIC, 1);
            Serial.println("mqtt connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqttConnected = false;
            Serial.println("mqtt disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (event->data_len > 0 && event->data_len < static_cast<int>(sizeof(pendingCommand))) {
                memcpy(pendingCommand, event->data, event->data_len);
                pendingCommand[event->data_len] = '\0';
                commandPending = true;
            }
            break;
        default:
            break;
    }
}

void startMqtt() {
    esp_mqtt_client_config_t cfg = {};
    configureMqttBroker(cfg);
    cfg.client_id = "showcase-bridge";
    cfg.keepalive = 30;
    cfg.lwt_topic = STATE_TOPIC;
    cfg.lwt_msg = "{\"online\":false}";
    cfg.lwt_retain = 1;
    cfg.lwt_qos = 1;
    cfg.reconnect_timeout_ms = 5000;
    mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, onMqttEvent, nullptr);
    esp_mqtt_client_start(mqtt);
    Serial.printf("mqtt %s\n", MQTT_URI);
}

}  // namespace

void setup() {
    Serial.setRxBufferSize(8192);
    Serial.begin(115200);
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(120);
    draw();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (espnow_relay::enabled()) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        espnow_relay::begin(true);
        espNowReady = espnow_relay::ready();
        draw();
        return; // USB backhaul never associates/scans, regardless of saved credentials.
    }
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(100);
    }
    // Power save fully off. With modem sleep on, esp_now_send wakes the radio
    // and the MQTT TCP socket is silently lost without a disconnect event.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi %s channel %d ip %s\n", WIFI_SSID, WiFi.channel(),
                      WiFi.localIP().toString().c_str());
    } else {
        // No AP: park on the configured channel so serial-driven frames still
        // reach sticks that parked there too.
        Serial.println("wifi TIMEOUT, parking on ESPNOW_CHANNEL");
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
    startEspNow();
    if (MQTT_ENABLED) {
        startMqtt();
    }
    Serial.printf("bridge ready channel=%d\n", WiFi.channel());
    draw();
}

void loop() {
    M5.update();
    espnow_relay::poll();
    if (commandPending) {
        commandPending = false;
        handle(pendingCommand, "mqtt");
    }
    // Limit per pass so a busy/malformed host cannot starve RF ACK/retry service.
    for (unsigned read = 0; read < 512 && Serial.available(); ++read) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n') {
            if (lineOverflow) Serial.println("relay serial line over 4096 bytes; discarded");
            else if (lineLength > 0) {
                line[lineLength] = '\0';
                if (espnow_relay::enabled() && line[0] == '{') espnow_relay::serialLine(line);
                else handle(line, "serial");
            }
            lineLength = 0;
            lineOverflow = false;
        } else if (c == '\r') {
            continue;
        } else if (!lineOverflow && lineLength + 1 < sizeof(line)) {
            line[lineLength++] = c;
        } else lineOverflow = true;
    }
    // Button A re-sends the last command, for a bench test without a host.
    if (!espnow_relay::enabled() && M5.BtnA.wasClicked() && strcmp(lastCommand, "none") != 0) {
        handle(lastCommand, "button");
    }
    if (millis() - lastStateMs >= STATE_MS) {
        lastStateMs = millis();
        publishState();
        draw();
    }
    delay(5);
}
