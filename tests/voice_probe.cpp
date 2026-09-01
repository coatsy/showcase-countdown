// Emits the voice assignment the firmware would compute, for a list of MACs.
// Built and diffed against scripts/fleet_voices.py by test_voice_assign.py.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../src/voice_assign.h"

static bool parseMac(const char* text, uint64_t* out) {
    uint8_t octets[6];
    int found = 0;
    for (const char* p = text; *p && found < 6;) {
        if (!std::isxdigit(static_cast<unsigned char>(*p))) {
            ++p;
            continue;
        }
        unsigned value = 0;
        int digits = 0;
        while (*p && std::isxdigit(static_cast<unsigned char>(*p)) && digits < 2) {
            const char c = *p;
            value = value * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            ++p;
            ++digits;
        }
        octets[found++] = static_cast<uint8_t>(value);
    }
    if (found != 6) {
        return false;
    }
    // ESP.getEfuseMac() lands octet 0 in the least significant byte.
    uint64_t efuse = 0;
    for (int i = 0; i < 6; ++i) {
        efuse |= static_cast<uint64_t>(octets[i]) << (8 * i);
    }
    *out = efuse;
    return true;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        uint64_t efuse = 0;
        if (!parseMac(argv[i], &efuse)) {
            std::fprintf(stderr, "bad mac: %s\n", argv[i]);
            return 2;
        }
        const uint32_t roll = voice::rollFor(efuse);
        const uint8_t index = voice::fromRoll(roll);
        std::printf("%s %u %u %s\n", argv[i], roll, index, fanfare::VOICES[index].name);
    }
    return 0;
}
