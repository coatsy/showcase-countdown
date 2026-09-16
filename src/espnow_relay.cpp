#include "espnow_relay.h"
#include "env_config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/md.h>
#include <sys/time.h>

namespace espnow_relay {
namespace {
using namespace relay_core;
Engine engine;
uint8_t key[32], mac[6], bridgeMac[6]{};
const uint8_t ALL[6] = {255,255,255,255,255,255};
bool started = false, isBridge = false, haveBridge = false;
const char* clockSource = "unknown";
uint32_t lastHello = 0, lastBeacon = 0, lastErrors = 0;
uint32_t hostTimeAt = 0;
uint32_t bridgeSeenAt = 0;
uint32_t clockSyncAt = 0;
bool haveHostTime = false;
Result (*receiver)(Topic, const char*) = nullptr;
struct Packet { uint8_t mac[6]; Frame frame; };
QueueHandle_t packets = nullptr;
portMUX_TYPE dropMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t callbackDrops = 0;
struct Peer { bool used = false; uint8_t mac[6]{}; uint32_t seen = 0; } peers[32];

const char* topicName(Topic topic) {
    static const char* names[] = {"", "cmd/display", "cmd/audio", "cmd/led", "cmd/config",
                                 "state", "event/button", "time"};
    return topic >= Display && topic <= Time ? names[topic] : "";
}
Topic parseTopic(const char* topic) {
    for (unsigned i = Display; i <= Button; ++i)
        if (!strcmp(topicName(static_cast<Topic>(i)), topic)) return static_cast<Topic>(i);
    return static_cast<Topic>(0);
}
void deviceId(const uint8_t* address, char* out) {
    snprintf(out, 7, "%02x%02x%02x", address[3], address[4], address[5]);
}
void output(JsonDocument& doc) { serializeJson(doc, Serial); Serial.println(); }
void result(uint32_t token, const uint8_t* dst, Result status) {
    if (!isBridge) {
        if (status != Accepted) Serial.printf("relay uplink rejected status=%u\n", status);
        return;
    }
    char id[7]; deviceId(dst, id);
    JsonDocument doc;
    doc["relay"] = 2; doc["type"] = "ack"; doc["token"] = token;
    doc["id"] = id; doc["ok"] = status == Accepted;
    static const char* errors[] = {"", "queue_full", "invalid", "direct_mqtt", "timeout"};
    if (status != Accepted) doc["error"] = errors[status];
    output(doc);
}
void serialError(uint32_t token, const char* id, const char* error) {
    JsonDocument doc;
    doc["relay"] = 2; doc["type"] = "ack"; doc["token"] = token;
    doc["id"] = id; doc["ok"] = false; doc["error"] = error; output(doc);
}
void sign(const void* bytes, size_t n, uint8_t* out) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof(key),
                    static_cast<const uint8_t*>(bytes), n, out);
}
bool transmit(const Frame& f) {
    return esp_now_send(ALL, reinterpret_cast<const uint8_t*>(&f), sizeof(f)) == ESP_OK;
}
void capture(const uint8_t* source, const uint8_t* bytes, int n) {
    if (n != sizeof(Frame) || !packets) return;
    Packet p; memcpy(p.mac, source, 6); memcpy(&p.frame, bytes, sizeof(Frame));
    if (xQueueSend(packets, &p, 0) != pdTRUE) {
        portENTER_CRITICAL(&dropMux); ++callbackDrops; portEXIT_CRITICAL(&dropMux);
    }
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onReceive(const esp_now_recv_info_t* info, const uint8_t* bytes, int n) {
    capture(info->src_addr, bytes, n);
}
#else
void onReceive(const uint8_t* source, const uint8_t* bytes, int n) { capture(source, bytes, n); }
#endif
bool saneEpoch(int64_t epoch) {
    return epoch >= 1700000000LL && epoch <= 4102444800LL &&
        llabs(epoch - static_cast<int64_t>(EVENT_EPOCH_UTC)) <= 366LL * 86400;
}
Result incoming(const Frame& frame, const char* payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) return Invalid;
    const auto topic = static_cast<Topic>(frame.topic);
    if (!isBridge) {
        if (topic == Time) {
            if (haveBridge && !same(bridgeMac, frame.src)) return Invalid;
            if (!doc["epoch"].is<int64_t>()) return Invalid;
            const int64_t epoch = doc["epoch"];
            if (epoch != 0 && !saneEpoch(epoch)) return Invalid;
            memcpy(bridgeMac, frame.src, 6); haveBridge = true;
            bridgeSeenAt = millis();
            if (epoch == 0) return Accepted; // authenticated discovery, not a clock claim
            const int64_t now = time(nullptr);
            if (!strcmp(clockSource, "ntp") || (!strcmp(clockSource, "espnow") && epoch <= now)) return Accepted;
            timeval tv{static_cast<time_t>(epoch), 0};
            settimeofday(&tv, nullptr); clockSource = "espnow";
            clockSyncAt = millis();
            return Accepted;
        }
        if (!haveBridge || !same(bridgeMac, frame.src) || topic < Display || topic > Config)
            return Invalid;
        return receiver ? receiver(topic, payload) : Busy;
    }
    if (topic != State && topic != Button) return Invalid;
    Peer* slot = nullptr;
    char id[7]; deviceId(frame.src, id);
    for (auto& p : peers) if (p.used && same(p.mac, frame.src)) { slot = &p; break; }
    // Two physical devices cannot claim the same logical id in the active peer set.
    for (auto& p : peers) if (p.used && !same(p.mac, frame.src) &&
        !memcmp(p.mac + 3, frame.src + 3, 3) && !elapsed(millis(), p.seen, 45000)) return Invalid;
    if (!slot) for (auto& p : peers) if (!p.used || elapsed(millis(), p.seen, 45000)) { slot = &p; break; }
    if (!slot) return Busy;
    if (topic == State) {
        const char* claimed = doc["id"] | "";
        if (strcmp(claimed, id)) return Invalid;
    }
    slot->used = true; memcpy(slot->mac, frame.src, 6); slot->seen = millis();
    JsonDocument out;
    out["relay"] = 2; out["type"] = "rx"; out["id"] = id; out["topic"] = topicName(topic);
    out["payload"] = doc.as<JsonObject>(); out["session"] = frame.session; out["seq"] = frame.seq;
    output(out);
    return Accepted;
}
void hello() {
    char id[7]; deviceId(mac, id);
    JsonDocument doc;
    doc["relay"] = 2; doc["type"] = "hello"; doc["bridge_id"] = id; output(doc);
}
} // namespace

bool enabled() { return ESPNOW_RELAY_ENABLED; }
bool ready() { return started && (isBridge || (haveBridge && !elapsed(millis(), bridgeSeenAt, 45000))); }
const char* timeSource() { return clockSource; }
uint32_t timeSyncMs() { return clockSyncAt; }
void setTimeSource(const char* source) { clockSource = source; }
void setReceiver(Result (*fn)(Topic, const char*)) { receiver = fn; }
void begin(bool bridge) {
    if (!enabled() || started) return;
    isBridge = bridge;
    const char* text = ESPNOW_RELAY_KEY;
    if (strlen(text) != 64) { Serial.println("relay invalid key; disabled"); return; }
    for (unsigned i = 0; i < 32; ++i) {
        char pair[] = {text[i * 2], text[i * 2 + 1], 0};
        key[i] = strtoul(pair, nullptr, 16);
    }
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    packets = xQueueCreate(32, sizeof(Packet));
    if (!packets || esp_now_init() != ESP_OK) { Serial.println("relay init FAILED"); return; }
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, ALL, 6); peer.channel = 0; peer.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peer) != ESP_OK || esp_now_register_recv_cb(onReceive) != ESP_OK) {
        Serial.println("relay peer/callback init FAILED"); return;
    }
    engine.begin(mac, EVENT_EPOCH_UTC, esp_random(), sign, transmit, incoming, result);
    started = true;
    if (isBridge) { hello(); lastHello = millis(); }
    Serial.printf("relay ready channel=%u\n", WiFi.channel());
}
bool send(Topic topic, const char* payload) {
    if (!ready()) { Serial.println("relay no bridge yet; uplink not queued"); return false; }
    if (!engine.enqueue(bridgeMac, topic, payload, 0, millis())) {
        Serial.println("relay uplink queue full/invalid"); return false;
    }
    return true;
}
void poll() {
    if (!started) return;
    Packet p;
    for (unsigned i = 0; i < 16 && xQueueReceive(packets, &p, 0) == pdTRUE; ++i)
        engine.input(p.mac, &p.frame, sizeof(p.frame), millis());
    engine.tick(millis());
    if (isBridge && elapsed(millis(), lastHello, 10000)) { lastHello = millis(); hello(); }
    if (isBridge && (lastBeacon == 0 || elapsed(millis(), lastBeacon, 10000))) {
        lastBeacon = millis();
        char body[48];
        const int64_t epoch = haveHostTime && !elapsed(millis(), hostTimeAt, 30000) ? time(nullptr) : 0;
        snprintf(body, sizeof(body), "{\"epoch\":%lld}", static_cast<long long>(epoch));
        if (!engine.enqueue(ALL, Time, body, 0, millis())) Serial.println("relay beacon queue full");
    }
    if (elapsed(millis(), lastErrors, 10000)) {
        lastErrors = millis();
        portENTER_CRITICAL(&dropMux); const auto drops = callbackDrops; portEXIT_CRITICAL(&dropMux);
        if (drops || engine.invalidFrames || engine.queueFull || engine.sendErrors)
            Serial.printf("relay counters rx_drop=%lu invalid=%lu queue_full=%lu send_error=%lu\n",
                static_cast<unsigned long>(drops), static_cast<unsigned long>(engine.invalidFrames),
                static_cast<unsigned long>(engine.queueFull), static_cast<unsigned long>(engine.sendErrors));
    }
}
void serialLine(const char* line) {
    if (!isBridge || !started) return;
    JsonDocument doc;
    if (strlen(line) > 4096 || deserializeJson(doc, line) || !doc.is<JsonObject>() ||
        !doc["relay"].is<int>() || doc["relay"].as<int>() != 2) {
        Serial.println("relay invalid serial JSON/version"); return;
    }
    const char* type = doc["type"] | "";
    if (!strcmp(type, "time")) {
        if (!doc["epoch"].is<int64_t>()) { Serial.println("relay invalid time"); return; }
        const int64_t epoch = doc["epoch"];
        if (!saneEpoch(epoch) || (haveHostTime && epoch < time(nullptr))) {
            Serial.println("relay rejected unreasonable/regressing host time"); return;
        }
        timeval tv{static_cast<time_t>(epoch), 0}; settimeofday(&tv, nullptr);
        haveHostTime = true; hostTimeAt = millis(); lastBeacon = 0; return;
    }
    if (strcmp(type, "tx")) { Serial.println("relay unknown serial type"); return; }
    const char* id = doc["id"] | "";
    const uint32_t token = doc["token"] | uint32_t(0);
    const auto topic = parseTopic(doc["topic"] | "");
    if (!doc["token"].is<uint32_t>() || topic < Display || topic > Config ||
        !doc["payload"].is<JsonObject>() || measureJson(doc["payload"]) > MAX_PAYLOAD) {
        serialError(token, id, "invalid_topic_payload_token"); return;
    }
    const bool all = !strcmp(id, "all");
    if (!all && (strlen(id) != 6 || strspn(id, "0123456789abcdef") != 6)) {
        serialError(token, id, "invalid_destination"); return;
    }
    char payload[MAX_PAYLOAD + 1]; serializeJson(doc["payload"], payload, sizeof(payload));
    unsigned matched = 0;
    for (const auto& p : peers) if (p.used && !elapsed(millis(), p.seen, 45000)) {
        char peerId[7]; deviceId(p.mac, peerId);
        if (!all && strcmp(id, peerId)) continue;
        ++matched;
        if (!engine.enqueue(p.mac, topic, payload, token, millis())) serialError(token, peerId, "queue_full");
    }
    if (!matched) serialError(token, id, "unknown_peer");
}
} // namespace espnow_relay
