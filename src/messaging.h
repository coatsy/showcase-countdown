#pragma once

#include <stdint.h>

// Room messaging over MQTT. See docs/messaging.md.
//
// The MQTT client runs in its own task. Nothing here touches the display or
// speaker: inbound commands are queued and the main loop drains them with
// poll(), so all rendering stays on the Arduino loop task.

namespace messaging {

enum class Kind : uint8_t { Self, Dm, Shout, Organiser };

struct Display {
    char text[160];
    char from[32];
    Kind kind;
    uint32_t ttlMs;
    uint8_t priority;
};

struct Config {
    char team[32];
    bool hasTeam;        // false when the payload carried no "team" key
    uint8_t brightness;  // 0 = leave alone
    bool locked;
    bool hasLocked;
};

struct Audio {
    char notes[400];   // composed note string, or the jingle's notes
    char jingle[24];   // name, for logging; empty for composed audio
    uint8_t volume;    // 0 = leave alone
};

enum class LedMode : uint8_t { Solid, Blink, Breathe, Off, Snake, Ping, Rainbow, RollingRainbow };

struct Led {
    uint32_t color;    // 0xRRGGBB
    LedMode mode;
    uint16_t periodMs;
    uint32_t ttlMs;
};

struct Handlers {
    void (*onDisplay)(const Display&);
    void (*onConfig)(const Config&);
    void (*onAudio)(const Audio&);
    void (*onLed)(const Led&);
};

struct Status {
    const char* voice;
    int battery;
    bool ntp;
    bool fired;
    uint16_t code;       // claim code shown on screen
    const char* team;
};

// Called once after WiFi is up. deviceId is the 6 hex-char id from the MAC.
void begin(const char* deviceId);

bool enabled();
bool connected();
const char* deviceId();

// Drain one inbound command. Handlers run on the caller's task.
void poll(const Handlers& handlers);

// Retained state for the dashboard and the MCP server. Cheap; call every few
// seconds and on any change.
void publishState(const Status& status);

// Button events for the teams' agents.
void publishButton(char button, const char* action);

}  // namespace messaging
