#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Wire format is little-endian (ESP32). No pointers or ABI-sized integers.
namespace relay_core {
constexpr size_t MAX_PAYLOAD = 767;
constexpr size_t CHUNK = 172;
constexpr uint8_t MAX_FRAGMENTS = 5;
constexpr uint32_t RETRY_MS = 350;
constexpr uint32_t EXPIRY_MS = 2500;
enum Topic : uint8_t { Display = 1, Audio, Led, Config, State, Button, Time };
enum Kind : uint8_t { Data = 1, Ack };
enum Result : uint8_t { Accepted = 0, Busy, Invalid, Direct, Expired };
#pragma pack(push, 1)
struct Frame {
    uint32_t magic;
    uint32_t event;
    uint8_t src[6], dst[6];
    uint32_t session, seq;
    uint16_t length;
    uint8_t topic, kind, fragment, count, result, reserved;
    uint8_t data[CHUNK];
    uint8_t tag[32];
};
#pragma pack(pop)
static_assert(sizeof(Frame) == 240, "ESP-NOW v1 frame bound");
inline bool same(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }
inline bool broadcast(const uint8_t* a) {
    static const uint8_t all[6] = {255,255,255,255,255,255};
    return same(a, all);
}
inline bool elapsed(uint32_t now, uint32_t start, uint32_t delay) { return now - start >= delay; }
using Sign = void (*)(const void*, size_t, uint8_t*);
using Send = bool (*)(const Frame&);
using Receive = Result (*)(const Frame&, const char*);
using Done = void (*)(uint32_t, const uint8_t*, Result);

class Engine {
    struct Assembly {
        bool used = false;
        Frame header{};
        uint32_t start = 0;
        uint8_t mask = 0;
        char payload[MAX_PAYLOAD + 1]{};
    } rx[8];
    struct Pending {
        bool used = false;
        Frame header{};
        char payload[MAX_PAYLOAD + 1]{};
        uint32_t token = 0, start = 0, last = 0, order = 0;
        uint8_t attempts = 0;
    } tx[16];
    struct Seen {
        bool used = false;
        uint8_t src[6]{};
        uint32_t session = 0, seq = 0, at = 0;
        Result result = Accepted;
    } seen[64];
    unsigned seenNext = 0;
    uint8_t mac[6]{};
    uint32_t event = 0, session = 0, sequence = 0, order = 0;
    Sign sign = nullptr;
    Send send = nullptr;
    Receive receive = nullptr;
    Done done = nullptr;
    void emit(Frame& f) { sign(&f, offsetof(Frame, tag), f.tag); if (!send(f)) ++sendErrors; }
    void acknowledge(const Frame& f, Result result) {
        if (f.topic == Time) return;
        Frame a{};
        a.magic = 0x32594c52; a.event = event;
        memcpy(a.src, mac, 6); memcpy(a.dst, f.src, 6);
        a.session = f.session; a.seq = f.seq; a.kind = Ack; a.result = result;
        emit(a);
    }
public:
    uint32_t invalidFrames = 0, sendErrors = 0, queueFull = 0;
    void begin(const uint8_t* address, uint32_t eventId, uint32_t boot, Sign s, Send out,
               Receive in, Done result) {
        memcpy(mac, address, 6); event = eventId; session = boot; sign = s;
        send = out; receive = in; done = result;
    }
    bool enqueue(const uint8_t* dst, Topic topic, const char* payload, uint32_t token,
                 uint32_t now) {
        const size_t n = strlen(payload);
        if (!n || n > MAX_PAYLOAD || topic < Display || topic > Time ||
            (broadcast(dst) && topic != Time)) return false;
        for (auto& t : tx) if (!t.used) {
            t = Pending{}; t.used = true; t.start = now; t.token = token; t.order = ++order;
            auto& f = t.header;
            f.magic = 0x32594c52; f.event = event; f.session = session; f.seq = ++sequence;
            memcpy(f.src, mac, 6); memcpy(f.dst, dst, 6);
            f.topic = topic; f.kind = Data; f.length = n; f.count = (n + CHUNK - 1) / CHUNK;
            memcpy(t.payload, payload, n + 1);
            return true;
        }
        ++queueFull;
        return false;
    }
    void input(const uint8_t* source, const void* bytes, size_t length, uint32_t now) {
        if (length != sizeof(Frame)) { ++invalidFrames; return; }
        Frame f; memcpy(&f, bytes, sizeof(f));
        uint8_t tag[32]; sign(&f, offsetof(Frame, tag), tag);
        uint8_t diff = 0;
        for (unsigned i = 0; i < 32; ++i) diff |= tag[i] ^ f.tag[i];
        if (diff || f.magic != 0x32594c52 || f.event != event || !same(source, f.src) ||
            same(f.src, mac) || (!same(f.dst, mac) && !(broadcast(f.dst) && f.topic == Time && f.kind == Data))) {
            ++invalidFrames; return;
        }
        if (f.kind == Ack) {
            if (f.result > Expired) { ++invalidFrames; return; }
            for (auto& t : tx) if (t.used && same(t.header.dst, f.src) &&
                t.header.session == f.session && t.header.seq == f.seq) {
                if (done) done(t.token, f.src, static_cast<Result>(f.result));
                t.used = false; return;
            }
            return;
        }
        if (f.kind != Data || f.topic < Display || f.topic > Time || !f.length ||
            f.length > MAX_PAYLOAD || f.count != (f.length + CHUNK - 1) / CHUNK ||
            f.fragment >= f.count || f.reserved || f.result) { ++invalidFrames; return; }
        for (const auto& d : seen) if (d.used && same(d.src, f.src) &&
            d.session == f.session && d.seq == f.seq && !elapsed(now, d.at, 120000)) {
            acknowledge(f, d.result); return;
        }
        Assembly* slot = nullptr;
        for (auto& a : rx) {
            if (a.used && elapsed(now, a.start, EXPIRY_MS)) a.used = false;
            if (a.used && same(a.header.src, f.src) && a.header.session == f.session && a.header.seq == f.seq) {
                slot = &a; break;
            }
        }
        if (!slot) for (auto& a : rx) if (!a.used) {
            a = Assembly{}; a.used = true; a.start = now; a.header = f; slot = &a; break;
        }
        if (!slot) { ++queueFull; acknowledge(f, Busy); return; }
        auto& a = *slot;
        if (a.header.length != f.length || a.header.topic != f.topic ||
            !same(a.header.dst, f.dst)) { ++invalidFrames; return; }
        const size_t offset = f.fragment * CHUNK;
        const size_t n = f.length - offset < CHUNK ? f.length - offset : CHUNK;
        memcpy(a.payload + offset, f.data, n);
        a.mask |= 1 << f.fragment;
        if (a.mask != (1 << f.count) - 1) return;
        a.payload[f.length] = 0;
        Result result = memchr(a.payload, 0, f.length) ? Invalid : receive(f, a.payload);
        auto& d = seen[seenNext++ % 64];
        d.used = true; memcpy(d.src, f.src, 6); d.session = f.session; d.seq = f.seq;
        d.at = now; d.result = result;
        a.used = false;
        acknowledge(f, result);
    }
    void tick(uint32_t now) {
        for (auto& t : tx) if (t.used) {
            bool earlier = false;
            for (const auto& other : tx) if (other.used && same(other.header.dst, t.header.dst) &&
                static_cast<int32_t>(other.order - t.order) < 0) earlier = true;
            if (earlier) continue;
            if (elapsed(now, t.start, EXPIRY_MS) || (t.attempts >= 4 && elapsed(now, t.last, RETRY_MS))) {
                if (done) done(t.token, t.header.dst, Expired);
                t.used = false; continue;
            }
            if (t.attempts && !elapsed(now, t.last, RETRY_MS)) continue;
            for (uint8_t i = 0; i < t.header.count; ++i) {
                Frame f = t.header; f.fragment = i;
                const size_t offset = i * CHUNK;
                const size_t n = f.length - offset < CHUNK ? f.length - offset : CHUNK;
                memcpy(f.data, t.payload + offset, n); emit(f);
            }
            ++t.attempts; t.last = now;
            if (t.header.topic == Time) t.used = false;
        }
    }
};

// No scans while parked. The driver channel hint does not prevent channel scans
// during a probe, so cap probes and suppress them around the finale.
struct Recovery {
    enum Phase { Probe, Parked, Online } phase = Probe;
    uint32_t since = 0, stableSince = 0;
    bool stable = false;
    bool update(uint32_t now, bool mqtt, bool canProbe, bool timingCritical) {
        const auto old = phase;
        if (phase == Online && !mqtt) { phase = Parked; since = now; stable = false; }
        if (phase == Probe) {
            if (mqtt) {
                if (!stable) { stable = true; stableSince = now; }
                if (elapsed(now, stableSince, 3000)) phase = Online;
            } else stable = false;
            if (phase == Probe && !stable && elapsed(now, since, 8000)) { phase = Parked; since = now; }
        } else if (phase == Parked && canProbe && !timingCritical && elapsed(now, since, 60000)) {
            phase = Probe; since = now; stable = false;
        }
        return old != phase;
    }
};
}  // namespace relay_core
