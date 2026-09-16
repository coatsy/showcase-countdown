#include "../src/relay_core.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace relay_core;
std::vector<Frame> wire;
std::vector<std::string> received;
std::vector<Result> results;

// Deterministic test MAC; production uses mbedTLS HMAC-SHA256.
void sign(const void* data, size_t n, uint8_t* tag) {
    uint32_t hash = 2166136261;
    auto* p = static_cast<const uint8_t*>(data);
    while (n--) hash = (hash ^ *p++) * 16777619;
    for (unsigned i = 0; i < 32; ++i) tag[i] = hash >> ((i % 4) * 8);
}
bool send(const Frame& f) { wire.push_back(f); return true; }
Result receive(const Frame&, const char* p) { received.emplace_back(p); return Accepted; }
void done(uint32_t, const uint8_t*, Result r) { results.push_back(r); }
const uint8_t A[6] = {1,2,3,4,5,6}, B[6] = {7,8,9,10,11,12};

int main() {
    Engine a, b;
    a.begin(A, 1789617600, 123, sign, send, receive, done);
    b.begin(B, 1789617600, 456, sign, send, receive, done);
    const std::string payload(MAX_PAYLOAD, 'x');
    assert(a.enqueue(B, Display, payload.c_str(), 42, 0));
    assert(!a.enqueue(B, Display, (payload + "x").c_str(), 0, 0));
    const uint8_t all[6] = {255,255,255,255,255,255};
    assert(!a.enqueue(all, Audio, "{}", 0, 0));
    a.tick(0);
    assert(wire.size() == MAX_FRAGMENTS);
    const auto fragments = wire;
    wire.clear();
    // Reordering and duplicate fragments, with one missing until retransmission.
    for (int i = MAX_FRAGMENTS - 1; i >= 1; --i) {
        b.input(A, &fragments[i], sizeof(Frame), 10);
        b.input(A, &fragments[i], sizeof(Frame), 11);
    }
    assert(received.empty());
    a.tick(RETRY_MS);
    const auto retry = wire;
    wire.clear();
    assert(retry.size() == MAX_FRAGMENTS);
    for (auto& f : retry) b.input(A, &f, sizeof(f), RETRY_MS + 1);
    assert(received.size() == 1 && received.back() == payload);
    assert(!wire.empty() && wire.back().kind == Ack);
    // Drop every first ACK, retry again; delivery remains exactly once in cache.
    wire.clear();
    a.tick(RETRY_MS * 2);
    const auto secondRetry = wire;
    wire.clear();
    for (auto& f : secondRetry) b.input(A, &f, sizeof(f), RETRY_MS * 2 + 1);
    assert(received.size() == 1);
    Frame ack = wire.back();
    wire.clear();
    a.input(B, &ack, sizeof(ack), 702);
    assert(results.size() == 1 && results.back() == Accepted);
    a.tick(1100);
    assert(wire.empty());

    auto bad = fragments[0];
    bad.data[0] ^= 1;
    b.input(A, &bad, sizeof(bad), 1200);
    b.input(B, &fragments[0], sizeof(Frame), 1200);
    b.input(A, &fragments[0], sizeof(Frame) - 1, 1200);
    bad = fragments[0]; bad.length = MAX_PAYLOAD + 1;
    sign(&bad, offsetof(Frame, tag), bad.tag);
    b.input(A, &bad, sizeof(bad), 1200);
    assert(b.invalidFrames == 4 && received.size() == 1);

    assert(a.enqueue(B, Audio, "{}", 43, 1200));
    for (uint32_t t = 1200; t <= 2600; t += RETRY_MS) a.tick(t);
    assert(results.back() == Expired);
    wire.clear();
    // Bounded queue, and time beacons do not require acknowledgement.
    Engine full;
    full.begin(A, 1789617600, 9, sign, send, receive, done);
    for (int i = 0; i < 16; ++i) assert(full.enqueue(B, Led, "{}", i, 0));
    assert(!full.enqueue(B, Led, "{}", 17, 0) && full.queueFull == 1);
    assert(a.enqueue(all, Time, "{\"epoch\":1789617000}", 0, 3000));
    a.tick(3000);
    Frame beacon = wire.back();
    wire.clear();
    b.input(A, &beacon, sizeof(beacon), 3001);
    assert(wire.empty());

    Recovery recovery;
    assert(!recovery.update(0, false, true, false));
    assert(recovery.update(8000, false, true, false) && recovery.phase == Recovery::Parked);
    assert(!recovery.update(68000, false, true, true)); // finale suppresses probes
    assert(recovery.update(68000, false, true, false) && recovery.phase == Recovery::Probe);
    assert(!recovery.update(69000, true, true, false));
    assert(!recovery.update(71999, true, true, false));
    assert(recovery.update(72000, true, true, false) && recovery.phase == Recovery::Online);
    assert(recovery.update(72001, false, true, false) && recovery.phase == Recovery::Parked);
    Recovery offline;
    assert(offline.update(8000, false, false, false));
    assert(!offline.update(100000, false, false, false));
    assert(elapsed(50, UINT32_MAX - 50, 100)); // millis wrap
    std::cout << "relay core: fragmentation, retry, dedup, validation, bounds, recovery passed\n";
}
