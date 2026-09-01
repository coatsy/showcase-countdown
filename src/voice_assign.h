#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fanfare.h"

// Deterministic voice assignment from a device's eFuse MAC.
//
// Kept free of Arduino dependencies so scripts/fleet_voices.py can be
// cross-validated against this exact code rather than against a reimplementation.
// If these two ever disagree, the helper would report a fleet as covered when it
// is not - which is worse than having no helper at all.

namespace voice {

// MurmurHash3 fmix64. Avalanches the MAC so numerically adjacent units, which
// is exactly what a batch of devices from one order will be, do not land in the
// same bucket.
inline uint32_t mixBits(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return static_cast<uint32_t>(value);
}

inline uint32_t rollFor(uint64_t efuseMac) {
    return mixBits(efuseMac) % 100;
}

inline uint8_t fromRoll(uint32_t roll) {
    uint16_t cumulative = 0;
    for (size_t i = 0; i < fanfare::VOICE_COUNT; ++i) {
        cumulative += fanfare::VOICES[i].weight;
        if (roll < cumulative) {
            return static_cast<uint8_t>(i);
        }
    }
    return 0;
}

inline uint8_t fromEfuseMac(uint64_t efuseMac) {
    return fromRoll(rollFor(efuseMac));
}

}  // namespace voice
