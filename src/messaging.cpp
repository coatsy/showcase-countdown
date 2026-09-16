#include "messaging.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <atomic>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>
#include <string.h>

#include "env_config.h"
#include "firmware_ota.h"
#include "jingles.h"
#include "mqtt_transport.h"
#include "espnow_relay.h"

namespace messaging {
namespace {

constexpr const char* FW_VERSION = FIRMWARE_VERSION;
constexpr size_t TOPIC_MAX = 64;
constexpr size_t PAYLOAD_MAX = 768;
constexpr uint32_t DEFAULT_TTL_MS = 15000;
constexpr uint32_t MAX_TTL_MS = 60000;

struct Inbound {
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
    uint32_t admitted;
};

char id[8] = {0};
char topicPrefix[40] = {0};  // showcase/dev/<id>
char stateTopic[48] = {0};
esp_mqtt_client_handle_t client = nullptr;
QueueHandle_t inbound = nullptr;
std::atomic<bool> isConnected{false};
struct Delivery { char id[65]{}; uint32_t at = 0; } deliveries[32];
unsigned deliveryNext = 0;

const bool ENABLED = MQTT_URI[0] != '\0' || ESPNOW_RELAY_ENABLED;

relay_core::Result admitRadio(relay_core::Topic topic, const char* payload) {
    if (messaging::mqttConnected()) return relay_core::Direct;
    JsonDocument doc;
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) return relay_core::Invalid;
    if (!doc["_delivery_id"].isNull() &&
        (!doc["_delivery_id"].is<const char*>() || strlen(doc["_delivery_id"]) > 64))
        return relay_core::Invalid;
    static const char* names[] = {"", "cmd/display", "cmd/audio", "cmd/led", "cmd/config"};
    Inbound msg{};
    snprintf(msg.topic, sizeof(msg.topic), "%s", names[topic]);
    snprintf(msg.payload, sizeof(msg.payload), "%s", payload);
    msg.admitted = millis();
    return inbound && xQueueSend(inbound, &msg, 0) == pdTRUE ? relay_core::Accepted : relay_core::Busy;
}

// Runs on the MQTT task. Copy and queue; never render from here.
void onEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
    auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
        case MQTT_EVENT_CONNECTED: {
            isConnected = true;
            char filter[TOPIC_MAX];
            snprintf(filter, sizeof(filter), "%s/cmd/#", topicPrefix);
            esp_mqtt_client_subscribe(client, filter, 1);
            esp_mqtt_client_subscribe(client, "showcase/all/cmd/#", 1);
            Serial.println("  mqtt    : connected");
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            isConnected = false;
            Serial.println("  mqtt    : disconnected");
            break;
        case MQTT_EVENT_DATA: {
            // Fragmented payloads are larger than anything the room sends.
            if (event->total_data_len != event->data_len) {
                break;
            }
            if (event->topic_len >= TOPIC_MAX || event->data_len >= PAYLOAD_MAX) {
                break;
            }
            Inbound msg;
            memcpy(msg.topic, event->topic, event->topic_len);
            msg.topic[event->topic_len] = '\0';
            memcpy(msg.payload, event->data, event->data_len);
            msg.payload[event->data_len] = '\0';
            msg.admitted = millis();
            if (xQueueSend(inbound, &msg, 0) != pdTRUE) Serial.println("mqtt command queue full");
            break;
        }
        default:
            break;
    }
}

const char* commandName(const char* topic) {
    const char* p = strrchr(topic, '/');
    return p ? p + 1 : topic;
}

Kind parseKind(const char* kind) {
    if (kind == nullptr) return Kind::Self;
    if (strcmp(kind, "dm") == 0) return Kind::Dm;
    if (strcmp(kind, "shout") == 0) return Kind::Shout;
    if (strcmp(kind, "organiser") == 0 || strcmp(kind, "organizer") == 0) return Kind::Organiser;
    return Kind::Self;
}

void copyString(char* dst, size_t size, const char* src) {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#RRGGBB" or "RRGGBB". Named colours like red, blue, skyblue, orange and
// off are also accepted. Anything else yields white so a typo is visible.
uint32_t parseColor(const char* text) {
    if (text == nullptr) return 0xFFFFFF;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    if (*text == '#') ++text;

    const size_t length = strlen(text);
    if (length == 0) return 0xFFFFFF;

    // Accept simple colour names in a lowercase-friendly form.
    static const struct {
        const char* name;
        uint32_t value;
    } named[] = {
        {"black", 0x000000}, {"off", 0x000000}, {"red", 0xFF0000}, {"green", 0x00FF00},
        {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF},
        {"white", 0xFFFFFF}, {"orange", 0xFFA500}, {"grey", 0x808080}, {"gray", 0x808080},
        {"skyblue", 0x87CEEB}, {"pink", 0xFFC0CB},
    };

    char lowered[32] = {0};
    size_t loweredLen = 0;
    for (; *text != '\0' && loweredLen + 1 < sizeof(lowered); ++text) {
        lowered[loweredLen++] = static_cast<char>(tolower(static_cast<unsigned char>(*text)));
    }
    lowered[loweredLen] = '\0';
    for (const auto& candidate : named) {
        if (strcmp(candidate.name, lowered) == 0) {
            return candidate.value;
        }
    }

    if (length != 6) return 0xFFFFFF;
    uint32_t value = 0;
    for (int i = 0; i < 6; ++i) {
        const int nibble = hexNibble(text[i]);
        if (nibble < 0) return 0xFFFFFF;
        value = (value << 4) | static_cast<uint32_t>(nibble);
    }
    return value;
}

LedMode parseLedMode(const char* mode) {
    if (mode == nullptr) return LedMode::Solid;
    if (strcmp(mode, "blink") == 0) return LedMode::Blink;
    if (strcmp(mode, "breathe") == 0) return LedMode::Breathe;
    if (strcmp(mode, "off") == 0) return LedMode::Off;
    if (strcmp(mode, "snake") == 0) return LedMode::Snake;
    if (strcmp(mode, "ping") == 0) return LedMode::Ping;
    if (strcmp(mode, "rainbow") == 0) return LedMode::Rainbow;
    // Accepted without the separator too, since callers routinely lower-case
    // "RollingRainbow" without inserting one.
    if (strcmp(mode, "rolling_rainbow") == 0 || strcmp(mode, "rollingrainbow") == 0) {
        return LedMode::RollingRainbow;
    }
    return LedMode::Solid;
}

void dispatch(const Inbound& msg, const Handlers& h) {
    JsonDocument doc;
    if (deserializeJson(doc, msg.payload) != DeserializationError::Ok) {
        Serial.printf("  mqtt    : bad json on %s\n", msg.topic);
        return;
    }
    const char* command = commandName(msg.topic);
    const char* delivery = doc["_delivery_id"] | "";
    if (strlen(delivery) > 64 || (!doc["_delivery_id"].isNull() && !doc["_delivery_id"].is<const char*>())) {
        Serial.println("messaging invalid delivery id"); return;
    }
    const uint32_t age = millis() - msg.admitted;
    const uint32_t ttl = doc["ttl_s"] | 15U;
    const uint32_t lifetime = ttl == 0 ? DEFAULT_TTL_MS : (ttl > 60 ? MAX_TTL_MS : ttl * 1000U);
    if (strcmp(command, "config") && age >= lifetime) {
        Serial.println("messaging expired queued command"); return;
    }
    if (*delivery) {
        for (const auto& seen : deliveries)
            if (!strcmp(seen.id, delivery) && millis() - seen.at < 120000) return;
        auto& seen = deliveries[deliveryNext++ % 32];
        copyString(seen.id, sizeof(seen.id), delivery); seen.at = millis();
    }
    auto onDisplay = h.onDisplay;
    auto onConfig = h.onConfig;

    if (strcmp(command, "audio") == 0 && h.onAudio) {
        Audio a = {};
        const char* jingle = doc["jingle"] | static_cast<const char*>(nullptr);
        const char* notes = jingles::find(jingle);
        if (notes != nullptr) {
            copyString(a.jingle, sizeof(a.jingle), jingle);
        } else {
            notes = doc["notes"] | static_cast<const char*>(nullptr);
        }
        if (notes == nullptr || notes[0] == '\0') {
            Serial.printf("  mqtt    : audio without notes or known jingle\n");
            return;
        }
        copyString(a.notes, sizeof(a.notes), notes);
        a.volume = doc["volume"] | 0;
        h.onAudio(a);
    } else if (strcmp(command, "led") == 0 && h.onLed) {
        Led l = {};
        l.color = parseColor(doc["color"] | static_cast<const char*>(nullptr));
        l.mode = parseLedMode(doc["mode"] | "solid");
        l.periodMs = doc["period_ms"] | 600;
        if (l.periodMs < 50) l.periodMs = 50;
        l.ttlMs = lifetime - age;
        h.onLed(l);
    } else if (strcmp(command, "display") == 0 && onDisplay) {
        Display d = {};
        copyString(d.text, sizeof(d.text), doc["text"] | "");
        copyString(d.from, sizeof(d.from), doc["from"] | "");
        d.kind = parseKind(doc["kind"] | "self");
        d.ttlMs = lifetime - age;
        d.priority = doc["priority"] | 0;
        if (d.text[0] != '\0') {
            onDisplay(d);
        }
    } else if (strcmp(command, "config") == 0 && onConfig) {
        Config c = {};
        c.hasTeam = !doc["team"].isNull();
        copyString(c.team, sizeof(c.team), doc["team"] | "");
        c.brightness = doc["brightness"] | 0;
        c.hasLocked = !doc["locked"].isNull();
        c.locked = doc["locked"] | false;
        onConfig(c);
    } else {
        Serial.printf("  mqtt    : unhandled command '%s'\n", command);
    }
}

}  // namespace

bool enabled() { return ENABLED; }
bool mqttConnected() { return isConnected.load() && WiFi.status() == WL_CONNECTED; }
bool connected() { return mqttConnected() || espnow_relay::ready(); }
const char* deviceId() { return id; }

void begin(const char* deviceId) {
    if (!ENABLED || inbound != nullptr) {
        return;
    }
    copyString(id, sizeof(id), deviceId);
    snprintf(topicPrefix, sizeof(topicPrefix), "showcase/dev/%s", id);
    snprintf(stateTopic, sizeof(stateTopic), "%s/state", topicPrefix);
    inbound = xQueueCreate(4, sizeof(Inbound));
    if (!inbound) { Serial.println("messaging queue allocation FAILED"); return; }
    espnow_relay::setReceiver(admitRadio);
    if (MQTT_URI[0] == '\0') return;

    static char clientId[24];
    snprintf(clientId, sizeof(clientId), "stick-%s", id);

    esp_mqtt_client_config_t cfg = {};
    configureMqttBroker(cfg);
// IDF 5 regrouped the flat client config into nested sub-structs.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.credentials.client_id = clientId;
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = stateTopic;
    cfg.session.last_will.msg = "{\"online\":false}";
    cfg.session.last_will.retain = 1;
    cfg.session.last_will.qos = 1;
    cfg.network.reconnect_timeout_ms = ESPNOW_RELAY_ENABLED ? 1000 : 5000;
    cfg.buffer.size = 1024;
#else
    cfg.client_id = clientId;
    cfg.keepalive = 30;
    cfg.lwt_topic = stateTopic;
    cfg.lwt_msg = "{\"online\":false}";
    cfg.lwt_retain = 1;
    cfg.lwt_qos = 1;
    cfg.reconnect_timeout_ms = ESPNOW_RELAY_ENABLED ? 1000 : 5000;
    cfg.buffer_size = 1024;
#endif

    client = esp_mqtt_client_init(&cfg);
    if (!client) { Serial.println("mqtt allocation FAILED"); return; }
    esp_err_t err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
    if (err == ESP_OK) err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        Serial.printf("  mqtt    : start FAILED (%d)\n", err);
        return;
    }
    Serial.printf("  mqtt    : %s as %s\n", MQTT_URI, clientId);
}

void poll(const Handlers& handlers) {
    if (!ENABLED || inbound == nullptr) {
        return;
    }
    Inbound msg;
    if (xQueueReceive(inbound, &msg, 0) == pdTRUE) {
        dispatch(msg, handlers);
    }
}

void publishState(const Status& status) {
    if (!ENABLED || !connected()) {
        return;
    }
    JsonDocument doc;
    doc["online"] = true;
    doc["id"] = id;
    doc["voice"] = status.voice;
    doc["battery"] = status.battery;
    doc["ntp"] = status.ntp;
    doc["fired"] = status.fired;
    doc["code"] = status.code;
    doc["team"] = status.team ? status.team : "";
    doc["uptime_s"] = millis() / 1000;
    doc["heap"] = ESP.getFreeHeap();
    doc["fw"] = FW_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["ota"] = firmware_ota::ready();
#if CONFIG_IDF_TARGET_ESP32S3
    doc["chip"] = "esp32s3";
#else
    doc["chip"] = "esp32";
#endif
    static const String imageMd5 = ESP.getSketchMD5();
    doc["image_md5"] = imageMd5;
    doc["transport"] = mqttConnected() ? "mqtt" : "espnow";
    doc["time_source"] = espnow_relay::timeSource();
    char out[512];
    const size_t n = serializeJson(doc, out, sizeof(out));
    if (mqttConnected()) {
        if (esp_mqtt_client_enqueue(client, stateTopic, out, n, 1, 1, true) < 0)
            Serial.println("mqtt state enqueue FAILED");
    } else espnow_relay::send(relay_core::State, out);
}

void publishButton(char button, const char* action) {
    if (!ENABLED || !connected()) {
        if (ENABLED) Serial.println("messaging button dropped: no transport");
        return;
    }
    char topic[TOPIC_MAX];
    snprintf(topic, sizeof(topic), "%s/event/button", topicPrefix);
    char out[64];
    const int n = snprintf(out, sizeof(out), "{\"button\":\"%c\",\"action\":\"%s\"}", button, action);
    if (mqttConnected()) {
        if (esp_mqtt_client_enqueue(client, topic, out, n, 0, 0, true) < 0)
            Serial.println("mqtt button enqueue FAILED");
    } else espnow_relay::send(relay_core::Button, out);
}

}  // namespace messaging
