#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "env_config.h"

// Runtime configuration held in NVS. Every value falls back to the compile-time
// setting from .env, so a device that has never been configured behaves exactly
// as its build intended. Provisioned over serial, which keeps the published
// firmware free of both secrets and event-specific detail.
namespace settings {

struct SpeakerOption {
    const char* name;
    bool internal;
    bool hatSpk;
    bool hatSpk2;
};

const SpeakerOption SPEAKER_OPTIONS[] = {
    {"NONE", false, false, false},
    {"INTERNAL", true, false, false},
    {"HAT_SPK", false, true, false},
    {"HAT_SPK2", false, false, true},
};

inline Preferences& store() {
    static Preferences prefs;
    return prefs;
}

// Opening a namespace read-only before it exists logs an nvs_open error, which
// reads as a fault in the boot banner. Creating it once up front avoids that.
inline void begin() {
    if (store().begin("countdown", false)) {
        store().end();
    }
}

inline String read(const char* key, const char* fallback) {
    String value;
    if (store().begin("countdown", true)) {
        // getString() on an absent key logs at error level, so ask first.
        if (store().isKey(key)) {
            value = store().getString(key, "");
        }
        store().end();
    }
    if (value.length() == 0) {
        value = fallback;
    }
    return value;
}

inline void write(const char* key, const char* value) {
    if (!store().begin("countdown", false)) {
        return;
    }
    store().putString(key, value);
    store().end();
}

inline void remove(const char* key) {
    if (!store().begin("countdown", false)) {
        return;
    }
    // Erasing an absent key logs at error level, so ask first.
    if (store().isKey(key)) {
        store().remove(key);
    }
    store().end();
}

// --- Wi-Fi -----------------------------------------------------------------

inline String ssid() {
    return read("ssid", WIFI_SSID);
}

inline String password() {
    return read("password", WIFI_PASSWORD);
}

inline bool wifiConfigured() {
    return ssid().length() > 0;
}

inline void saveWifi(const char* ssid, const char* password) {
    if (!store().begin("countdown", false)) {
        return;
    }
    store().putString("ssid", ssid);
    store().putString("password", password);
    store().end();
}

inline void clearWifi() {
    remove("ssid");
    remove("password");
}

struct TeamOption {
    const char* name;
    uint32_t colour;
};

const TeamOption TEAM_OPTIONS[] = {
    {"Headwaters", 0x1D4ED8}, {"Atlas", 0x15803D}, {"Outpost", 0xEA580C},
    {"Gateway", 0x38BDF8}, {"Trailblazer", 0xDC2626}, {"Sentinel", 0xEAB308},
    {"Horizon", 0xDB2777}, {"Basecamp", 0x64748B}, {"Wayfinder", 0x7C3AED},
    {"Relay", 0x0D9488}, {"Waypoint", 0x84CC16},
};

inline const TeamOption* findTeam(const char* name) {
    for (const TeamOption& team : TEAM_OPTIONS) {
        if (strcmp(name, team.name) == 0) {
            return &team;
        }
    }
    return nullptr;
}

inline String teamName() {
    return read("team", "");
}

inline bool setTeamName(const String& name) {
    const TeamOption* team = findTeam(name.c_str());
    if (team == nullptr) {
        return false;
    }
    write("team", team->name);
    return true;
}

// --- Event titles ----------------------------------------------------------

inline String eventTitles() {
    return read("titles", EVENT_TITLES_RAW);
}

inline void setEventTitles(const String& value) {
    write("titles", value.c_str());
}

// --- Speaker ---------------------------------------------------------------

inline String speakerName() {
    return read("speaker", SPEAKER_NAME);
}

inline const SpeakerOption* findSpeaker(const String& name) {
    String wanted = name;
    wanted.trim();
    wanted.toUpperCase();
    for (const SpeakerOption& option : SPEAKER_OPTIONS) {
        if (wanted == option.name) {
            return &option;
        }
    }
    return nullptr;
}

inline const SpeakerOption& speaker() {
    const SpeakerOption* option = findSpeaker(speakerName());
    return option ? *option : SPEAKER_OPTIONS[1];
}

inline bool setSpeaker(const String& name) {
    const SpeakerOption* option = findSpeaker(name);
    if (option == nullptr) {
        return false;
    }
    write("speaker", option->name);
    return true;
}

// --- Local time zone -------------------------------------------------------

struct Timezone {
    const char* label;
    long offsetSeconds;
};

// Fixed standard-time offsets; the firmware does no DST calculation, so cities
// are named for their standard offset rather than their current local time.
const Timezone TIMEZONES[] = {
    {"Midway", -11 * 3600},        {"Honolulu", -10 * 3600},
    {"Anchorage", -9 * 3600},      {"Los Angeles", -8 * 3600},
    {"Denver", -7 * 3600},         {"Chicago", -6 * 3600},
    {"New York", -5 * 3600},       {"Halifax", -4 * 3600},
    {"Sao Paulo", -3 * 3600},      {"South Georgia", -2 * 3600},
    {"Azores", -1 * 3600},         {"London", 0},
    {"Paris", 1 * 3600},           {"Athens", 2 * 3600},
    {"Moscow", 3 * 3600},          {"Tehran", 3 * 3600 + 1800},
    {"Dubai", 4 * 3600},           {"Karachi", 5 * 3600},
    {"Mumbai", 5 * 3600 + 1800},   {"Dhaka", 6 * 3600},
    {"Bangkok", 7 * 3600},         {"Singapore", 8 * 3600},
    {"Tokyo", 9 * 3600},           {"Adelaide", 9 * 3600 + 1800},
    {"Sydney", 10 * 3600},         {"Brisbane", 10 * 3600},
    {"Lord Howe", 10 * 3600 + 1800}, {"Noumea", 11 * 3600},
    {"Auckland", 12 * 3600},       {"Apia", 13 * 3600},
};

inline size_t timezoneCount() {
    return sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);
}

inline String formatUtcOffset(long seconds) {
    const char sign = seconds < 0 ? '-' : '+';
    const long absolute = seconds < 0 ? -seconds : seconds;
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "%c%02ld:%02ld", sign, absolute / 3600, (absolute % 3600) / 60);
    return String(buffer);
}

inline long utcOffsetSeconds() {
    long seconds = LOCAL_UTC_OFFSET_SECONDS;
    if (store().begin("countdown", true)) {
        if (store().isKey("tz")) {
            seconds = store().getLong("tz", seconds);
        }
        store().end();
    }
    return seconds;
}

// Stores the offset rather than the list position, so a stored value stays
// correct if the table is ever reordered or extended.
inline bool setUtcOffsetSeconds(long seconds) {
    if (seconds < -12 * 3600 || seconds > 14 * 3600) {
        return false;
    }
    if (!store().begin("countdown", false)) {
        return false;
    }
    store().putLong("tz", seconds);
    store().end();
    return true;
}

inline bool setTimezone(size_t oneBasedIndex) {
    if (oneBasedIndex < 1 || oneBasedIndex > timezoneCount()) {
        return false;
    }
    return setUtcOffsetSeconds(TIMEZONES[oneBasedIndex - 1].offsetSeconds);
}

inline const char* timezoneLabel(long seconds) {
    for (const Timezone& zone : TIMEZONES) {
        if (zone.offsetSeconds == seconds) {
            return zone.label;
        }
    }
    return "custom";
}

// Leaves Wi-Fi credentials alone; those have their own command.
inline void resetConfigurable() {
    remove("team");
    remove("titles");
    remove("speaker");
    remove("tz");
}

}  // namespace settings
