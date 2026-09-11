#include "messaging.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>
#include <string.h>

#include "env_config.h"
#include "jingles.h"

namespace messaging {
namespace {

constexpr const char* FW_VERSION = "0.3.0-dev";
constexpr size_t TOPIC_MAX = 64;
constexpr size_t PAYLOAD_MAX = 768;
constexpr uint32_t DEFAULT_TTL_MS = 15000;
constexpr uint32_t MAX_TTL_MS = 60000;

struct Inbound {
    char topic[TOPIC_MAX];
    char payload[PAYLOAD_MAX];
};

char id[8] = {0};
char topicPrefix[40] = {0};  // showcase/dev/<id>
char stateTopic[48] = {0};
esp_mqtt_client_handle_t client = nullptr;
QueueHandle_t inbound = nullptr;
volatile bool isConnected = false;

const bool ENABLED = MQTT_HOST[0] != '\0';

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
            xQueueSend(inbound, &msg, 0);  // drop rather than block the network task
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
        const uint32_t ttlS = doc["ttl_s"] | 0;
        l.ttlMs = ttlS ? ttlS * 1000UL : DEFAULT_TTL_MS;
        if (l.ttlMs > MAX_TTL_MS) l.ttlMs = MAX_TTL_MS;
        h.onLed(l);
    } else if (strcmp(command, "display") == 0 && onDisplay) {
        Display d = {};
        copyString(d.text, sizeof(d.text), doc["text"] | "");
        copyString(d.from, sizeof(d.from), doc["from"] | "");
        d.kind = parseKind(doc["kind"] | "self");
        const uint32_t ttlS = doc["ttl_s"] | 0;
        d.ttlMs = ttlS ? ttlS * 1000UL : DEFAULT_TTL_MS;
        if (d.ttlMs > MAX_TTL_MS) d.ttlMs = MAX_TTL_MS;
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
bool connected() { return isConnected; }
const char* deviceId() { return id; }

void begin(const char* deviceId) {
    if (!ENABLED || client != nullptr) {
        return;
    }
    copyString(id, sizeof(id), deviceId);
    snprintf(topicPrefix, sizeof(topicPrefix), "showcase/dev/%s", id);
    snprintf(stateTopic, sizeof(stateTopic), "%s/state", topicPrefix);
    inbound = xQueueCreate(4, sizeof(Inbound));

    static char uri[64];
    static char clientId[24];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
    snprintf(clientId, sizeof(clientId), "stick-%s", id);

    esp_mqtt_client_config_t cfg = {};
// IDF 5 regrouped the flat client config into nested sub-structs.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = clientId;
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = stateTopic;
    cfg.session.last_will.msg = "{\"online\":false}";
    cfg.session.last_will.retain = 1;
    cfg.session.last_will.qos = 1;
    cfg.network.reconnect_timeout_ms = 5000;
    cfg.buffer.size = 1024;
#else
    cfg.uri = uri;
    cfg.client_id = clientId;
    cfg.keepalive = 30;
    cfg.lwt_topic = stateTopic;
    cfg.lwt_msg = "{\"online\":false}";
    cfg.lwt_retain = 1;
    cfg.lwt_qos = 1;
    cfg.reconnect_timeout_ms = 5000;
    cfg.buffer_size = 1024;
#endif

    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onEvent, nullptr);
    esp_mqtt_client_start(client);
    Serial.printf("  mqtt    : %s as %s\n", uri, clientId);
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
    if (!ENABLED || !isConnected) {
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
    char out[256];
    const size_t n = serializeJson(doc, out, sizeof(out));
    esp_mqtt_client_publish(client, stateTopic, out, n, 1, 1);
}

void publishButton(char button, const char* action) {
    if (!ENABLED || !isConnected) {
        return;
    }
    char topic[TOPIC_MAX];
    snprintf(topic, sizeof(topic), "%s/event/button", topicPrefix);
    char out[64];
    const int n = snprintf(out, sizeof(out), "{\"button\":\"%c\",\"action\":\"%s\"}", button, action);
    esp_mqtt_client_publish(client, topic, out, n, 0, 0);
}

}  // namespace messaging
