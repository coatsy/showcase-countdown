#if __has_include(<Network.h>)
#include <Network.h>
#endif

#include <Adafruit_NeoPixel.h>
#include <ImprovWiFiLibrary.h>
#include <M5Unified.h>
#include <WiFi.h>

#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#elif __has_include(<sntp.h>)
#include <sntp.h>
#else
#error "SNTP support is required"
#endif

#include <string.h>

#include <vector>

#include "env_config.h"
#include "espnow_link.h"
#include "fanfare.h"
#include "jingles.h"
#include "light_schedule.h"
#include "messaging.h"
#include "sequencer.h"
#include "settings.h"
#include "voice_assign.h"

namespace {

constexpr uint32_t WIFI_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_TIMEOUT_MS = 15000;
constexpr uint32_t RESYNC_INTERVAL_MS = 6UL * 60 * 60 * 1000;  // requirement 3
constexpr int64_t PRE_EVENT_SYNC_LEAD_S = 5 * 60;

// Any real sync lands far past this; an unset clock starts at 0. Comparing the
// wall clock is more robust than SNTP status values across core versions.
constexpr time_t SANE_EPOCH = 1750000000;  // 2025-06-15

// Replaceable over serial, so the set is parsed at boot rather than baked in.
std::vector<String> titles;
String plainTitle;
constexpr uint32_t MARQUEE_MS_PER_PX = 15;  // ~65 px/sec
constexpr int MARQUEE_GAP = 24;             // blank run between wrapped copies
constexpr uint32_t MARQUEE_MIN_LOOPS = 2;
constexpr uint32_t FRAME_MS_SCROLLING = 40;
constexpr uint32_t FRAME_MS_IDLE = 200;

size_t titleIndex = 0;
uint32_t titleStartMs = 0;
bool titleScrolling = false;
bool celebrationScrolling = false;
uint32_t titleCycleMs = TITLE_DWELL_MS;

// ---------------------------------------------------------------------------
// Title colour markup
//
// Titles may carry inline tags: [red]Westpac[/] + [blue]Microsoft[/] Hackathon
// [/] resets to the default. [#RRGGBB] gives any colour without a code change,
// which is what keeps the palette configurable from .env.
// ---------------------------------------------------------------------------

constexpr uint16_t TITLE_DEFAULT_COLOUR = 0xFFFF;  // white

struct NamedColour {
    const char* name;
    uint16_t value;
};

// RGB565 literals rather than TFT_* macros, so the palette does not depend on
// which colour names a given M5GFX version happens to define.
const NamedColour NAMED_COLOURS[] = {
    {"red", 0xF800},    {"green", 0x07E0},  {"blue", 0x001F},     {"yellow", 0xFFE0},
    {"cyan", 0x07FF},   {"magenta", 0xF81F}, {"white", 0xFFFF},   {"orange", 0xFD20},
    {"grey", 0x8410},   {"gray", 0x8410},   {"skyblue", 0x867D}, {"pink", 0xFE19},
};

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Unrecognised tags deliberately fail to parse so they render as literal text.
// Silently swallowing a typo would be far harder to diagnose on a 240 px screen.
bool parseColourTag(const char* p, const char** afterTag, uint16_t* colour) {
    if (*p != '[') {
        return false;
    }
    const char* close = strchr(p, ']');
    if (close == nullptr) {
        return false;
    }
    const char* body = p + 1;
    const size_t length = static_cast<size_t>(close - body);

    if (length == 1 && body[0] == '/') {
        *colour = TITLE_DEFAULT_COLOUR;
        *afterTag = close + 1;
        return true;
    }

    if (length == 7 && body[0] == '#') {
        int value[6];
        for (int i = 0; i < 6; ++i) {
            value[i] = hexDigit(body[1 + i]);
            if (value[i] < 0) {
                return false;
            }
        }
        *colour = rgb565(static_cast<uint8_t>(value[0] << 4 | value[1]),
                         static_cast<uint8_t>(value[2] << 4 | value[3]),
                         static_cast<uint8_t>(value[4] << 4 | value[5]));
        *afterTag = close + 1;
        return true;
    }

    for (const NamedColour& candidate : NAMED_COLOURS) {
        if (strlen(candidate.name) == length && strncmp(candidate.name, body, length) == 0) {
            *colour = candidate.value;
            *afterTag = close + 1;
            return true;
        }
    }
    return false;
}

// Only tags the renderer would consume are removed, so a typo survives here for
// the same reason it survives on screen.
String stripMarkup(const String& text) {
    String out;
    for (const char* p = text.c_str(); *p != '\0';) {
        const char* afterTag = nullptr;
        uint16_t ignored = TITLE_DEFAULT_COLOUR;
        if (parseColourTag(p, &afterTag, &ignored)) {
            p = afterTag;
            continue;
        }
        out += *p++;
    }
    out.trim();
    return out;
}

void loadTitles() {
    const String raw = settings::eventTitles();
    titles.clear();

    int start = 0;
    while (start <= static_cast<int>(raw.length())) {
        int bar = raw.indexOf('|', start);
        if (bar < 0) {
            bar = static_cast<int>(raw.length());
        }
        String part = raw.substring(start, bar);
        part.trim();
        if (part.length() > 0) {
            titles.push_back(part);
        }
        start = bar + 1;
    }
    if (titles.empty()) {
        titles.push_back(String("Countdown"));
    }

    plainTitle = stripMarkup(titles[0]);
    titleIndex = 0;
    titleStartMs = millis();
}

// Measures when draw is false, renders when true. One implementation so the
// marquee's width can never disagree with what is actually drawn.
int renderTitleMarkup(LovyanGFX* g, const char* text, int x, int y, bool draw) {
    uint16_t colour = TITLE_DEFAULT_COLOUR;
    char run[64];
    size_t runLength = 0;
    int cursor = x;

    for (const char* p = text;;) {
        const bool atEnd = (*p == '\0');
        const char* afterTag = nullptr;
        uint16_t tagColour = TITLE_DEFAULT_COLOUR;
        const bool isTag = !atEnd && parseColourTag(p, &afterTag, &tagColour);

        if (atEnd || isTag) {
            if (runLength > 0) {
                run[runLength] = '\0';
                if (draw) {
                    g->setTextColor(colour);
                    g->drawString(run, cursor, y);
                }
                cursor += g->textWidth(run);
                runLength = 0;
            }
            if (atEnd) {
                break;
            }
            colour = tagColour;
            p = afterTag;
            continue;
        }

        if (runLength + 1 < sizeof(run)) {
            run[runLength++] = *p;
        }
        ++p;
    }
    return cursor - x;
}

// Below this, leave the 1 Hz render loop and start spinning on the clock.
// The loop period, not NTP jitter, is the dominant error term for ensemble
// alignment - see research 7.4.
constexpr int64_t FIRE_ARM_WINDOW_S = 3;

struct Layout {
    int w;
    int h;
    const lgfx::IFont* numberFont;
    const lgfx::IFont* labelFont;
    const lgfx::IFont* titleFont;
};

Layout layout;
bool spriteReady = false;

// Function-local static: constructed on first use, after M5.begin(). A global
// M5Canvas would dereference M5.Display during static init, before it exists.
M5Canvas& sprite() {
    static M5Canvas instance(&M5.Display);
    return instance;
}

uint8_t voiceIndex = 0;
bool timeVerified = false;
uint32_t lastSyncMs = 0;
bool preEventSyncAttempted = false;
volatile bool ntpSyncReceived = false;
bool fired = false;
bool showDiagnostics = false;

Adafruit_NeoPixel lights(LED_COUNT, LED_PIN, LED_PIXEL_TYPE);
bool lightsOn = false;
bool lightScheduleInitialized = false;
bool lastScheduledLightsOn = false;

// ---------------------------------------------------------------------------
// Grove lights
// ---------------------------------------------------------------------------

void setLights(bool on, const char* source) {
    if (!LED_ENABLED) {
        return;
    }

    lightsOn = on;
    const uint32_t colour = on ? lights.Color(LED_BRIGHTNESS, LED_BRIGHTNESS, LED_BRIGHTNESS) : 0;
    for (uint16_t index = 0; index < LED_COUNT; ++index) {
        lights.setPixelColor(index, colour);
    }
    lights.show();
    Serial.printf("  leds    : %s (%s)\n", on ? "on" : "off", source);
}

void updateLightSchedule(time_t now) {
    if (!LED_ENABLED || now <= SANE_EPOCH) {
        return;
    }

    const bool scheduledOn = light_schedule::isOnAt(
        static_cast<int64_t>(now), settings::utcOffsetSeconds(), LED_ON_MINUTE_OF_DAY,
        LED_OFF_MINUTE_OF_DAY);
    if (!lightScheduleInitialized || scheduledOn != lastScheduledLightsOn) {
        lightScheduleInitialized = true;
        lastScheduledLightsOn = scheduledOn;
        setLights(scheduledOn, "schedule");
    }
}

void initializeLights() {
    if (!LED_ENABLED) {
        Serial.println("  leds    : disabled");
        return;
    }

    M5.Power.setExtOutput(true);
    Serial.printf("  grove 5v: %s\n", M5.Power.getExtOutput() ? "on" : "FAILED");
    lights.begin();
    lights.clear();
    lights.show();
    Serial.printf("  leds    : %u x %s on GPIO %d\n", static_cast<unsigned>(LED_COUNT),
                  LED_TYPE_NAME, LED_PIN);
    updateLightSchedule(time(nullptr));
}

void toggleLights(const char* source) {
    if (!LED_ENABLED) {
        Serial.println("  leds    : toggle ignored (disabled)");
        return;
    }
    setLights(!lightsOn, source);
}

// ---------------------------------------------------------------------------
// Room messaging (docs/messaging.md)
//
// With MQTT_HOST set, the unit keeps WiFi up after the sync and holds an MQTT
// session: it shows overlaid messages, plays audio, drives the LED, and
// publishes its state and button presses. All rendering stays on the loop
// task; the MQTT client only queues commands.
// ---------------------------------------------------------------------------

constexpr uint32_t STATE_PUBLISH_MS = 10000;
// Team commands are refused from here until the fanfare has finished, even
// with no network: the lock is computed from the clock as well as pushed.
constexpr int64_t LOCK_LEAD_S = 60;

char deviceIdStr[8] = {0};
uint16_t claimCode = 0;
char teamName[32] = {0};
bool roomLocked = false;  // retained organiser config
uint32_t lastStatePublishMs = 0;

struct Overlay {
    char text[160];
    char from[32];
    messaging::Kind kind;
    uint8_t priority;
    uint32_t startMs;
    uint32_t untilMs;
    bool active;
    bool scrolling;
};
Overlay overlay = {};

// FNV-1a over id + salt, folded to four digits. Stable across reboots and
// not derivable from the id alone.
uint16_t claimCodeFor(const char* id, const char* salt) {
    uint32_t h = 2166136261u;
    for (const char* p = id; *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    for (const char* p = salt; *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return static_cast<uint16_t>(1000 + (h % 9000));
}

const char* kindName(messaging::Kind kind) {
    switch (kind) {
        case messaging::Kind::Dm: return "dm";
        case messaging::Kind::Shout: return "shout";
        case messaging::Kind::Organiser: return "organiser";
        default: return "self";
    }
}

// True once the fanfare is close enough that nothing may risk delaying it.
// Publishing blocks on the MQTT client lock, which can stall for the network
// timeout on a half-open socket, so it stops well before the fire instant.
bool nearFanfare() {
    const int64_t remaining =
        static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    const int64_t fanfareS = fanfare::DURATION_MS / 1000;
    return remaining <= LOCK_LEAD_S && remaining > -fanfareS;
}

bool roomLockedNow() {
    if (roomLocked) {
        return true;
    }
    const int64_t remaining =
        static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    const int64_t fanfareS = fanfare::DURATION_MS / 1000;
    return remaining <= LOCK_LEAD_S && remaining > -fanfareS;
}

void onDisplayCommand(const messaging::Display& d) {
    const bool organiser = d.kind == messaging::Kind::Organiser;
    if (roomLockedNow() && !organiser) {
        Serial.println("  overlay : dropped, room locked");
        return;
    }
    const bool current = overlay.active && static_cast<int32_t>(millis() - overlay.untilMs) < 0;
    if (current && !organiser && d.priority < overlay.priority) {
        Serial.println("  overlay : dropped, lower priority");
        return;
    }
    strncpy(overlay.text, d.text, sizeof(overlay.text) - 1);
    overlay.text[sizeof(overlay.text) - 1] = '\0';
    strncpy(overlay.from, d.from, sizeof(overlay.from) - 1);
    overlay.from[sizeof(overlay.from) - 1] = '\0';
    overlay.kind = d.kind;
    overlay.priority = organiser ? 255 : d.priority;
    overlay.startMs = millis();
    overlay.untilMs = overlay.startMs + d.ttlMs;
    overlay.active = true;
    Serial.printf("  overlay : [%s] %s: %s (%lu ms)\n", kindName(d.kind), d.from, d.text,
                  static_cast<unsigned long>(d.ttlMs));
}

// ---------------------------------------------------------------------------
// Team audio: a non-blocking note sequencer driven from loop(). tone() is
// asynchronous, so each note is started at its scheduled instant and the loop
// keeps rendering and polling MQTT in between.
// ---------------------------------------------------------------------------

struct Sequence {
    sequencer::Note notes[sequencer::MAX_NOTES];
    size_t count;
    size_t index;
    uint32_t nextMs;
};
Sequence sequence = {};

void stopSequence() {
    if (sequence.count > 0) {
        sequence.count = 0;
        sequence.index = 0;
        M5.Speaker.stop();
    }
}

void startSequence(const char* notes) {
    sequence.count = sequencer::parse(notes, sequence.notes, sequencer::MAX_NOTES);
    sequence.index = 0;
    sequence.nextMs = millis();
}

void serviceSequence() {
    while (sequence.index < sequence.count &&
           static_cast<int32_t>(millis() - sequence.nextMs) >= 0) {
        const sequencer::Note& note = sequence.notes[sequence.index++];
        if (note.hz > 0.0f && M5.Speaker.isEnabled()) {
            M5.Speaker.tone(note.hz, note.ms);
        }
        sequence.nextMs += note.ms;
    }
    if (sequence.index >= sequence.count) {
        sequence.count = 0;
    }
}

void onAudioCommand(const messaging::Audio& a) {
    if (roomLockedNow()) {
        Serial.println("  audio   : dropped, room locked");
        return;
    }
    if (a.volume > 0 && M5.Speaker.isEnabled()) {
        M5.Speaker.setVolume(a.volume);
    }
    stopSequence();
    startSequence(a.notes);
    Serial.printf("  audio   : %s%s %u notes, %lu ms\n", a.jingle[0] ? "jingle " : "composed",
                  a.jingle, static_cast<unsigned>(sequence.count),
                  static_cast<unsigned long>(sequencer::totalMs(sequence.notes, sequence.count)));
}

// ---------------------------------------------------------------------------
// LED override: a team colour and pattern on the Grove pixel, then back to the
// daily schedule when it expires.
// ---------------------------------------------------------------------------

struct LedOverride {
    bool active;
    uint32_t color;
    messaging::LedMode mode;
    uint16_t periodMs;
    uint32_t startMs;
    uint32_t untilMs;
    uint32_t lastShown;
    uint32_t lastFrame;
};
LedOverride ledOverride = {};

uint32_t scaleColor(uint32_t color, uint8_t level) {
    const uint32_t r = ((color >> 16) & 0xFF) * level / 255;
    const uint32_t g = ((color >> 8) & 0xFF) * level / 255;
    const uint32_t b = (color & 0xFF) * level / 255;
    return (r << 16) | (g << 8) | b;
}

void showColor(uint32_t color) {
    for (uint16_t index = 0; index < LED_COUNT; ++index) {
        lights.setPixelColor(index, color);
    }
    lights.show();
}

// Animated modes paint each pixel separately, so they are throttled per frame
// instead of by the single-colour comparison the uniform modes rely on.
constexpr uint32_t LED_FRAME_MS = 20;

// Red through violet spans roughly 280 degrees of the 16-bit hue wheel.
constexpr uint32_t LED_HUE_VIOLET = 51000;

bool isAnimatedMode(messaging::LedMode mode) {
    switch (mode) {
        case messaging::LedMode::Snake:
        case messaging::LedMode::Ping:
        case messaging::LedMode::Rainbow:
        case messaging::LedMode::RollingRainbow:
            return true;
        default:
            return false;
    }
}

uint16_t rainbowHue(uint16_t index) {
    const uint16_t count = LED_COUNT;
    // Guarding the divisor keeps a single-pixel strip from dividing by zero.
    const uint16_t span = count > 1 ? static_cast<uint16_t>(count - 1) : 1;
    return static_cast<uint16_t>(static_cast<uint32_t>(index) * LED_HUE_VIOLET / span);
}

void paintAnimatedFrame(uint32_t elapsed) {
    const uint16_t count = LED_COUNT;
    const uint32_t period = ledOverride.periodMs;

    switch (ledOverride.mode) {
        case messaging::LedMode::Snake: {
            // One lap per period, each pixel behind the head halving in brightness.
            const uint32_t stepMs = period / count > 0 ? period / count : 1;
            const uint16_t head = static_cast<uint16_t>((elapsed / stepMs) % count);
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t behind = static_cast<uint16_t>((head + count - i) % count);
                const uint8_t level = behind < 8 ? static_cast<uint8_t>(255u >> behind) : 0;
                lights.setPixelColor(i, scaleColor(ledOverride.color, level));
            }
            break;
        }
        case messaging::LedMode::Ping: {
            // One out-and-back trip per period, without repeating either end.
            const uint16_t span = count > 1 ? static_cast<uint16_t>((count - 1) * 2) : 1;
            const uint32_t stepMs = period / span > 0 ? period / span : 1;
            const uint32_t step = (elapsed / stepMs) % span;
            const uint16_t pos = step < count ? static_cast<uint16_t>(step)
                                             : static_cast<uint16_t>(span - step);
            for (uint16_t i = 0; i < count; ++i) {
                lights.setPixelColor(i, i == pos ? ledOverride.color : 0);
            }
            break;
        }
        case messaging::LedMode::Rainbow: {
            for (uint16_t i = 0; i < count; ++i) {
                lights.setPixelColor(
                    i, lights.gamma32(lights.ColorHSV(rainbowHue(i), 255, LED_BRIGHTNESS)));
            }
            break;
        }
        case messaging::LedMode::RollingRainbow: {
            // Hue wraps on the 16-bit wheel, so the rotation has no visible seam.
            const uint32_t phase =
                static_cast<uint32_t>((static_cast<uint64_t>(elapsed % period) * 65536) / period);
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t hue = static_cast<uint16_t>((rainbowHue(i) + phase) & 0xFFFF);
                lights.setPixelColor(i, lights.gamma32(lights.ColorHSV(hue, 255, LED_BRIGHTNESS)));
            }
            break;
        }
        default:
            break;
    }
    lights.show();
}

void onLedCommand(const messaging::Led& l) {
    if (!LED_ENABLED) {
        Serial.println("  led     : ignored (disabled)");
        return;
    }
    if (roomLockedNow()) {
        Serial.println("  led     : dropped, room locked");
        return;
    }
    ledOverride.active = true;
    ledOverride.color = l.color;
    ledOverride.mode = l.mode;
    ledOverride.periodMs = l.periodMs;
    ledOverride.startMs = millis();
    ledOverride.untilMs = ledOverride.startMs + l.ttlMs;
    ledOverride.lastShown = 0xFFFFFFFF;
    ledOverride.lastFrame = 0xFFFFFFFF;
    Serial.printf("  led     : #%06lX mode=%u period=%u ttl=%lu ms\n",
                  static_cast<unsigned long>(l.color), static_cast<unsigned>(l.mode),
                  static_cast<unsigned>(l.periodMs), static_cast<unsigned long>(l.ttlMs));
}

// Drives the animations through the same path an MQTT command takes, so a
// stick with no broker can still be checked on the bench.
void nextLedAnimation() {
    static const messaging::LedMode MODES[] = {
        messaging::LedMode::Snake, messaging::LedMode::Ping, messaging::LedMode::Rainbow,
        messaging::LedMode::RollingRainbow};
    static const char* NAMES[] = {"snake", "ping", "rainbow", "rolling_rainbow"};
    constexpr uint8_t COUNT = sizeof(MODES) / sizeof(MODES[0]);
    static uint8_t index = 0;

    messaging::Led l = {};
    l.color = 0x00A4EF;
    l.mode = MODES[index];
    l.periodMs = 2000;
    l.ttlMs = 15000;
    Serial.printf("  led     : %s demo\n", NAMES[index]);
    index = static_cast<uint8_t>((index + 1) % COUNT);
    onLedCommand(l);
}

void serviceLedOverride() {
    if (!ledOverride.active) {
        return;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - ledOverride.untilMs) >= 0) {
        ledOverride.active = false;
        lightScheduleInitialized = false;  // re-apply the schedule on the next loop
        return;
    }
    const uint32_t elapsed = now - ledOverride.startMs;
    if (isAnimatedMode(ledOverride.mode)) {
        const uint32_t frame = elapsed / LED_FRAME_MS;
        if (frame != ledOverride.lastFrame) {
            ledOverride.lastFrame = frame;
            paintAnimatedFrame(elapsed);
        }
        return;
    }
    const uint32_t period = ledOverride.periodMs;
    uint32_t color = ledOverride.color;
    switch (ledOverride.mode) {
        case messaging::LedMode::Blink:
            color = ((elapsed / (period / 2)) % 2 == 0) ? color : 0;
            break;
        case messaging::LedMode::Breathe: {
            const uint32_t phase = elapsed % period;
            const uint32_t half = period / 2;
            const uint32_t level = phase < half ? (phase * 255) / half : ((period - phase) * 255) / half;
            color = scaleColor(color, static_cast<uint8_t>(level));
            break;
        }
        case messaging::LedMode::Off:
            color = 0;
            break;
        default:
            break;
    }
    if (color != ledOverride.lastShown) {
        ledOverride.lastShown = color;
        showColor(color);
    }
}

void onConfigCommand(const messaging::Config& c) {
    if (c.hasTeam) {
        strncpy(teamName, c.team, sizeof(teamName) - 1);
        teamName[sizeof(teamName) - 1] = '\0';
    }
    if (c.brightness > 0) {
        M5.Display.setBrightness(c.brightness);
    }
    if (c.hasLocked) {
        roomLocked = c.locked;
    }
    lastStatePublishMs = 0;  // reflect the change in state promptly
    Serial.printf("  config  : team='%s' brightness=%u locked=%s\n", teamName,
                  static_cast<unsigned>(c.brightness), roomLocked ? "yes" : "no");
}

uint32_t parseSolidColorValue(const String& value) {
    String text = value;
    text.trim();
    if (text.length() == 0) {
        return 0xFFFFFF;
    }
    if (text.startsWith("#")) {
        text = text.substring(1);
    }
    if (text.length() == 6) {
        uint32_t rgb = 0;
        for (size_t i = 0; i < 6; ++i) {
            const int nibble = hexDigit(text.charAt(i));
            if (nibble < 0) {
                return 0xFFFFFF;
            }
            rgb = (rgb << 4) | static_cast<uint32_t>(nibble);
        }
        return rgb;
    }

    static const struct {
        const char* name;
        uint32_t value;
    } names[] = {
        {"black", 0x000000}, {"off", 0x000000}, {"red", 0xFF0000}, {"green", 0x00FF00},
        {"blue", 0x0000FF}, {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF},
        {"white", 0xFFFFFF}, {"orange", 0xFFA500}, {"grey", 0x808080}, {"gray", 0x808080},
        {"skyblue", 0x87CEEB}, {"pink", 0xFFC0CB},
    };

    const String lower = text;  // keep it simple; serial input is short
    for (const auto& candidate : names) {
        if (strcasecmp(lower.c_str(), candidate.name) == 0) {
            return candidate.value;
        }
    }
    return 0xFFFFFF;
}

void printSerialHelp() {
    Serial.println("serial commands:");
    Serial.println("  ?  show this help menu");
    Serial.println("  i  show the current settings");
    Serial.println("  e  set the event title(s)");
    Serial.println("  p  set the speaker type");
    Serial.println("  z  set the local time zone");
    Serial.println("  a  audition all fanfare voices");
    Serial.println("  s  resync the clock from NTP");
    Serial.println("  q  play a short speaker test tone");
    Serial.println("  t  run the speaker tone sweep");
    Serial.println("  m  run the speaker drive test");
    Serial.println("  l  toggle the Grove lights");
    Serial.println("  c  set all LEDs to a hex or named colour (e.g. #00A4EF, red, blue)");
    Serial.println("  n  cycle the LED animations (snake, ping, rainbow, rolling)");
    Serial.println("  j  play the 'tada' jingle through the sequencer");
    Serial.println("  w  forget the stored Wi-Fi credentials");
    Serial.println("  x  reset settings to the built-in defaults");
}

void printSettings() {
    const long offset = settings::utcOffsetSeconds();
    Serial.println("current settings:");
    Serial.printf("  titles  : %s\n", settings::eventTitles().c_str());
    Serial.printf("  speaker : %s\n", settings::speakerName().c_str());
    Serial.printf("  timezone: UTC%s (%s)\n", settings::formatUtcOffset(offset).c_str(),
                  settings::timezoneLabel(offset));
    Serial.printf("  wifi    : %s\n",
                  settings::wifiConfigured() ? settings::ssid().c_str() : "NOT PROVISIONED");

    const time_t now = time(nullptr);
    Serial.printf("  clock   : %lld (%s)\n", static_cast<long long>(now),
                  timeVerified ? "synced" : "UNVERIFIED");
    Serial.printf("  remain  : %lld s\n",
                  static_cast<long long>(EVENT_EPOCH_UTC - static_cast<int64_t>(now)));
    Serial.printf("  state   : fired=%d sprite=%d diag=%d title=%u/%u\n", fired ? 1 : 0,
                  spriteReady ? 1 : 0, showDiagnostics ? 1 : 0, static_cast<unsigned>(titleIndex),
                  static_cast<unsigned>(titles.size()));
    Serial.printf("  heap    : %u free, %u largest\n", static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    Serial.printf("  bright  : %u\n", static_cast<unsigned>(M5.Display.getBrightness()));
}

// Commands issued by software rather than a person, so values arrive with the
// command instead of through a prompt. Every reply starts ok: or err: so the
// caller can tell the outcome without parsing prose.
String commandBuffer;
bool collectingCommand = false;

bool parseLong(const String& text, long* value) {
    if (text.length() == 0) {
        return false;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text[i];
        if (!isdigit(c) && !(i == 0 && (c == '-' || c == '+'))) {
            return false;
        }
    }
    *value = text.toInt();
    return true;
}

void handleMachineCommand(const String& line) {
    const int space = line.indexOf(' ');
    const String verb = space < 0 ? line : line.substring(0, space);
    String arg = space < 0 ? String("") : line.substring(space + 1);
    arg.trim();

    if (verb == "get") {
        const long offset = settings::utcOffsetSeconds();
        Serial.printf("ok: get title=%s\n", settings::eventTitles().c_str());
        Serial.printf("ok: get speaker=%s\n", settings::speakerName().c_str());
        Serial.printf("ok: get tz=%ld\n", offset);
        return;
    }

    if (verb == "led" || verb == "color" || verb == "colour") {
        const String text = arg;
        const uint32_t rgb = parseSolidColorValue(text);
        if (rgb == 0xFFFFFF && text.length() > 0 && !text.equalsIgnoreCase("white") &&
            !text.equalsIgnoreCase("red") && !text.equalsIgnoreCase("green") &&
            !text.equalsIgnoreCase("blue") && !text.equalsIgnoreCase("yellow") &&
            !text.equalsIgnoreCase("cyan") && !text.equalsIgnoreCase("magenta") &&
            !text.equalsIgnoreCase("orange") && !text.equalsIgnoreCase("pink") &&
            !text.equalsIgnoreCase("grey") && !text.equalsIgnoreCase("gray") &&
            !text.equalsIgnoreCase("skyblue") && !text.equalsIgnoreCase("black") &&
            !text.equalsIgnoreCase("off") && !text.startsWith("#")) {
            Serial.println("err: led needs a hex colour like #00A4EF or a named colour like red, blue, pink, white, or off");
            return;
        }
        messaging::Led l = {};
        l.color = rgb;
        l.mode = messaging::LedMode::Solid;
        l.periodMs = 600;
        l.ttlMs = 15000;
        Serial.printf("  led     : %s solid\n", text.length() > 0 ? text.c_str() : "white");
        onLedCommand(l);
        return;
    }

    if (verb == "tz") {
        long seconds = 0;
        if (!parseLong(arg, &seconds) || !settings::setUtcOffsetSeconds(seconds)) {
            Serial.println("err: tz needs an offset in seconds between -43200 and 50400");
            return;
        }
        Serial.printf("ok: tz=%ld (UTC%s)\n", seconds,
                      settings::formatUtcOffset(seconds).c_str());
        return;
    }

    if (verb == "title") {
        if (arg.length() == 0) {
            Serial.println("err: title needs a value");
            return;
        }
        settings::setEventTitles(arg);
        loadTitles();
        Serial.printf("ok: title=%s\n", settings::eventTitles().c_str());
        return;
    }

    if (verb == "speaker") {
        if (!settings::setSpeaker(arg)) {
            Serial.println("err: speaker must be NONE, INTERNAL, HAT_SPK, or HAT_SPK2");
            return;
        }
        Serial.printf("ok: speaker=%s (restart to apply)\n", settings::speakerName().c_str());
        return;
    }

    Serial.printf("err: unknown command %s\n", verb.c_str());
}

// Settings that need a value read a whole line, so input is buffered until
// Enter rather than dispatched per character like the single-key commands.
enum class Prompt { None, EventTitles, Speaker, Timezone, Color };

Prompt activePrompt = Prompt::None;
String promptBuffer;

void beginPrompt(Prompt prompt) {
    activePrompt = prompt;
    promptBuffer = "";

    switch (prompt) {
        case Prompt::EventTitles:
            Serial.printf("current: %s\n", settings::eventTitles().c_str());
            Serial.println("separate multiple titles with | and use [red]...[/] for colour");
            Serial.print("new title(s)> ");
            break;
        case Prompt::Speaker:
            Serial.printf("current: %s\n", settings::speakerName().c_str());
            for (const settings::SpeakerOption& option : settings::SPEAKER_OPTIONS) {
                Serial.printf("  %s\n", option.name);
            }
            Serial.print("speaker> ");
            break;
        case Prompt::Timezone: {
            const long current = settings::utcOffsetSeconds();
            bool marked = false;
            for (size_t i = 0; i < settings::timezoneCount(); ++i) {
                const settings::Timezone& zone = settings::TIMEZONES[i];
                // Several cities share an offset, so only flag the first match.
                const bool isCurrent = !marked && zone.offsetSeconds == current;
                marked = marked || isCurrent;
                Serial.printf("  %2u  UTC%s  %-14s%s\n", static_cast<unsigned>(i + 1),
                              settings::formatUtcOffset(zone.offsetSeconds).c_str(), zone.label,
                              isCurrent ? "  <- current" : "");
            }
            Serial.print("time zone number> ");
            break;
        }
        case Prompt::Color:
            Serial.println("set all LEDs to a hex or named colour (e.g. #00A4EF, red, blue, white, off)");
            Serial.print("colour> ");
            break;
        case Prompt::None:
            break;
    }
}

void applyPrompt() {
    const Prompt prompt = activePrompt;
    String value = promptBuffer;
    value.trim();
    activePrompt = Prompt::None;
    promptBuffer = "";
    Serial.println();

    if (value.length() == 0) {
        Serial.println("  unchanged");
        return;
    }

    switch (prompt) {
        case Prompt::EventTitles:
            settings::setEventTitles(value);
            loadTitles();
            Serial.printf("  titles  : %s\n", settings::eventTitles().c_str());
            break;
        case Prompt::Speaker:
            if (settings::setSpeaker(value)) {
                Serial.printf("  speaker : %s (restarting to apply)\n",
                              settings::speakerName().c_str());
                Serial.flush();
                ESP.restart();
            } else {
                Serial.printf("  %s is not a known speaker type\n", value.c_str());
            }
            break;
        case Prompt::Timezone: {
            const long choice = value.toInt();
            if (settings::setTimezone(static_cast<size_t>(choice))) {
                const long offset = settings::utcOffsetSeconds();
                Serial.printf("  timezone: UTC%s (%s)\n",
                              settings::formatUtcOffset(offset).c_str(),
                              settings::timezoneLabel(offset));
            } else {
                Serial.printf("  pick a number between 1 and %u\n",
                              static_cast<unsigned>(settings::timezoneCount()));
            }
            break;
        }
        case Prompt::Color: {
            const uint32_t rgb = parseSolidColorValue(value);
            if (rgb == 0xFFFFFF && value.length() > 0 && !value.equalsIgnoreCase("white") &&
                !value.equalsIgnoreCase("red") && !value.equalsIgnoreCase("green") &&
                !value.equalsIgnoreCase("blue") && !value.equalsIgnoreCase("yellow") &&
                !value.equalsIgnoreCase("cyan") && !value.equalsIgnoreCase("magenta") &&
                !value.equalsIgnoreCase("orange") && !value.equalsIgnoreCase("pink") &&
                !value.equalsIgnoreCase("grey") && !value.equalsIgnoreCase("gray") &&
                !value.equalsIgnoreCase("skyblue") && !value.equalsIgnoreCase("black") &&
                !value.equalsIgnoreCase("off") && !value.startsWith("#")) {
                Serial.println("  colour : invalid; use #RRGGBB or a named colour like red, blue, pink, white, or off");
                break;
            }
            messaging::Led l = {};
            l.color = rgb;
            l.mode = messaging::LedMode::Solid;
            l.periodMs = 600;
            l.ttlMs = 15000;
            Serial.printf("  led     : %s solid\n", value.length() > 0 ? value.c_str() : "white");
            onLedCommand(l);
            break;
        }
        case Prompt::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// Improv Wi-Fi provisioning
//
// Lets the browser installer hand over credentials over the same USB serial
// link, so no network details are compiled into a published image.
// ---------------------------------------------------------------------------

ImprovWiFi improv(&Serial);

// handleSerial() consumes a byte per call regardless of content, so it would
// otherwise swallow the single-character commands. Packets arrive as one burst,
// so the header byte opens a short window during which bytes belong to Improv.
constexpr uint32_t IMPROV_BURST_MS = 250;
uint32_t improvBurstUntil = 0;

void onImprovConnected(const char* ssid, const char* password) {
    settings::saveWifi(ssid, password);
    Serial.printf("  improv  : provisioned for %s\n", ssid);
}

// ---------------------------------------------------------------------------
// Voice assignment
// ---------------------------------------------------------------------------

uint8_t selectVoice() {
#ifdef FORCE_VOICE
    if (FORCE_VOICE >= 0 && FORCE_VOICE < static_cast<int>(fanfare::VOICE_COUNT)) {
        return static_cast<uint8_t>(FORCE_VOICE);
    }
#endif
    return voice::fromEfuseMac(ESP.getEfuseMac());
}

// ---------------------------------------------------------------------------
// Countdown formatting - largest two units (requirement 1)
// ---------------------------------------------------------------------------

struct Readout {
    long first;
    const char* firstLabel;
    long second;
    const char* secondLabel;
};

const char* unitLabel(long value, const char* singular, const char* plural) {
    return value == 1 ? singular : plural;
}

Readout formatRemaining(int64_t seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    const long days = static_cast<long>(seconds / 86400);
    const long hours = static_cast<long>((seconds % 86400) / 3600);
    const long minutes = static_cast<long>((seconds % 3600) / 60);
    const long secs = static_cast<long>(seconds % 60);

    if (days > 0) {
        return {days, unitLabel(days, "DAY", "DAYS"), hours, unitLabel(hours, "HR", "HRS")};
    }
    if (hours > 0) {
        return {hours, unitLabel(hours, "HR", "HRS"), minutes, unitLabel(minutes, "MIN", "MINS")};
    }
    return {minutes, unitLabel(minutes, "MIN", "MINS"), secs, unitLabel(secs, "SEC", "SECS")};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void computeLayout() {
    layout.w = M5.Display.width();
    layout.h = M5.Display.height();

    // Derived from the panel, never hardcoded: this same binary has to look
    // sane on an 80x160 StickC and a 135x240 Plus SE.
    // Each fonts:: entry is a different concrete type, so the ternaries need an
    // explicit common base.
    const bool roomy = layout.h >= 120;
    layout.numberFont = roomy ? static_cast<const lgfx::IFont*>(&fonts::Font7)
                              : static_cast<const lgfx::IFont*>(&fonts::Font4);
    layout.labelFont = roomy ? static_cast<const lgfx::IFont*>(&fonts::Font4)
                             : static_cast<const lgfx::IFont*>(&fonts::Font2);
    layout.titleFont = roomy ? static_cast<const lgfx::IFont*>(&fonts::FreeSansBold12pt7b)
                             : static_cast<const lgfx::IFont*>(&fonts::Font2);
}

void ensureSprite() {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;
    sprite().setColorDepth(16);
    spriteReady = sprite().createSprite(layout.w, layout.h) != nullptr;
}

// Font7 is a seven-segment face: '1' lights only the right-hand segments yet
// still occupies a full-width cell, so any string containing one sits visually
// right of its advance box. Return the nudge needed to align the label with the
// ink the eye actually sees rather than with the font metrics.
int inkOffset(LovyanGFX* g, const char* digits) {
    if (layout.numberFont != static_cast<const lgfx::IFont*>(&fonts::Font7)) {
        return 0;
    }
    int length = 0;
    int ones = 0;
    for (const char* p = digits; *p; ++p) {
        ++length;
        if (*p == '1') {
            ++ones;
        }
    }
    if (length == 0 || ones == 0) {
        return 0;
    }
    // A '1' glyph's ink centre sits roughly 30% of a cell right of the cell centre.
    return (g->textWidth("8") * 3 * ones) / (10 * length);
}

void drawUnit(LovyanGFX* g, long value, const char* label, int centreX, int top, int labelTop) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%ld", value);

    g->setFont(layout.numberFont);
    g->setTextDatum(top_center);
    g->setTextColor(TFT_WHITE);
    g->drawString(digits, centreX, top);

    const int labelX = centreX + inkOffset(g, digits);

    // Cyan, not dark grey: this has to read across a room, and low-contrast
    // grey on black disappears entirely at any distance.
    g->setFont(layout.labelFont);
    g->setTextDatum(top_center);
    g->setTextColor(TFT_CYAN);
    g->drawString(label, labelX, labelTop);
}

// Draws a title centred, or scrolling when it is wider than the screen, with
// colour markup respected either way. Returns the scroll span in pixels, or 0
// when the title fits without scrolling.
int drawMarqueeTitle(LovyanGFX* g, const char* title, int y, int bandH, uint32_t elapsed) {
    g->setFont(layout.titleFont);
    g->setTextDatum(top_left);

    const int avail = layout.w - 4;
    const int textW = renderTitleMarkup(g, title, 0, 0, false);

    if (textW <= avail) {
        renderTitleMarkup(g, title, (layout.w - textW) / 2, y, true);
        return 0;
    }

    // Drawn twice a span apart so the wrap is seamless rather than blanking out
    // between repeats.
    const int span = textW + MARQUEE_GAP;
    const int shift = static_cast<int>((elapsed / MARQUEE_MS_PER_PX) % span);
    g->setClipRect(2, y, avail, bandH);
    renderTitleMarkup(g, title, 2 - shift, y, true);
    renderTitleMarkup(g, title, 2 - shift + span, y, true);
    g->clearClipRect();
    return span;
}

void drawTitle(LovyanGFX* g, int titleH) {
    const uint32_t elapsed = millis() - titleStartMs;

    // Blank pause between titles. Only the title area clears; the rule and
    // countdown below stay put.
    if (titles.size() > 1 && elapsed >= titleCycleMs) {
        titleScrolling = false;
        return;
    }

    const int span = drawMarqueeTitle(g, titles[titleIndex].c_str(), 2, titleH + 2, elapsed);
    titleScrolling = span > 0;

    if (span > 0) {
        // Long titles get at least two full passes, so someone who glances up
        // midway through still sees the whole message.
        const uint32_t scrollMs =
            static_cast<uint32_t>(span) * MARQUEE_MS_PER_PX * MARQUEE_MIN_LOOPS;
        titleCycleMs = scrollMs > TITLE_DWELL_MS ? scrollMs : TITLE_DWELL_MS;
    } else {
        titleCycleMs = TITLE_DWELL_MS;
    }
}

// Claim code, broker link state, and team name along the bottom edge. Only
// present when messaging is configured, so the plain countdown keeps its
// original proportions.
int stripHeight(LovyanGFX* g) {
    if (!messaging::enabled()) {
        return 0;
    }
    g->setFont(&fonts::Font2);
    return g->fontHeight();
}

void drawStrip(LovyanGFX* g) {
    if (!messaging::enabled()) {
        return;
    }
    g->setFont(&fonts::Font2);
    const int sh = g->fontHeight();
    const int y = layout.h - sh;

    char code[8];
    snprintf(code, sizeof(code), "%04u", static_cast<unsigned>(claimCode));
    g->setTextDatum(top_left);
    g->setTextColor(TFT_LIGHTGREY);
    g->drawString(code, 4, y);

    g->fillCircle(layout.w / 2, y + sh / 2, 3, messaging::connected() ? TFT_GREEN : TFT_RED);

    if (teamName[0] != '\0') {
        g->setTextDatum(top_right);
        g->drawString(teamName, layout.w - 4, y);
    }
}

void drawCountdown(LovyanGFX* g, int64_t remaining) {
    g->fillRect(0, 0, layout.w, layout.h, TFT_BLACK);

    g->setFont(layout.titleFont);
    const int titleH = g->fontHeight();
    drawTitle(g, titleH);

    const int ruleY = titleH + 6;
    g->drawFastHLine(6, ruleY, layout.w - 12, TFT_DARKGREY);

    // Measured rather than assumed: the previous fixed fractions put the title
    // baseline inside the digits.
    g->setFont(layout.numberFont);
    const int numberH = g->fontHeight();
    g->setFont(layout.labelFont);
    const int labelH = g->fontHeight();
    const int stripH = stripHeight(g);

    constexpr int GAP = 2;
    const int bodyTop = ruleY + 1;
    const int blockH = numberH + GAP + labelH;
    const int blockTop = bodyTop + ((layout.h - stripH - bodyTop) - blockH) / 2;
    const int labelTop = blockTop + numberH + GAP;

    const Readout r = formatRemaining(remaining);
    drawUnit(g, r.first, r.firstLabel, layout.w / 4, blockTop, labelTop);
    drawUnit(g, r.second, r.secondLabel, (layout.w * 3) / 4, blockTop, labelTop);
    drawStrip(g);
}

void renderCountdown(int64_t remaining) {
    ensureSprite();
    if (spriteReady) {
        drawCountdown(&sprite(), remaining);
        sprite().pushSprite(0, 0);
    } else {
        // Flickers, but a blank screen would look like a crash.
        drawCountdown(&M5.Display, remaining);
    }
}

// A message from a team, another team, or the organiser, above the countdown.
void drawOverlay(LovyanGFX* g, uint32_t elapsed) {
    g->fillRect(0, 0, layout.w, layout.h, TFT_BLACK);

    uint16_t accent = TFT_CYAN;
    char header[48];
    switch (overlay.kind) {
        case messaging::Kind::Dm:
            accent = TFT_YELLOW;
            snprintf(header, sizeof(header), "%s to you", overlay.from);
            break;
        case messaging::Kind::Shout:
            accent = TFT_ORANGE;
            snprintf(header, sizeof(header), "%s shouts", overlay.from);
            break;
        case messaging::Kind::Organiser:
            accent = TFT_MAGENTA;
            snprintf(header, sizeof(header), "ORGANISER");
            break;
        default:
            snprintf(header, sizeof(header), "%s", teamName[0] ? teamName : "message");
            break;
    }

    g->setFont(&fonts::Font2);
    const int headerH = g->fontHeight();
    g->setTextDatum(top_left);
    g->setTextColor(accent);
    g->drawString(header, 4, 2);
    const int ruleY = headerH + 4;
    g->drawFastHLine(4, ruleY, layout.w - 8, accent);

    const int stripH = stripHeight(g);
    g->setFont(layout.titleFont);
    const int textH = g->fontHeight();
    const int areaTop = ruleY + 2;
    const int areaH = layout.h - stripH - areaTop;
    const int textY = areaTop + (areaH - textH) / 2;
    overlay.scrolling = drawMarqueeTitle(g, overlay.text, textY, textH + 2, elapsed) > 0;

    drawStrip(g);
}

void renderOverlay(uint32_t elapsed) {
    ensureSprite();
    if (spriteReady) {
        drawOverlay(&sprite(), elapsed);
        sprite().pushSprite(0, 0);
    } else {
        drawOverlay(&M5.Display, elapsed);
    }
}

void renderMessage(const char* line1, const char* line2) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(layout.titleFont);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(line1, layout.w / 2, layout.h / 2 - 12);
    if (line2) {
        M5.Display.setFont(layout.labelFont);
        M5.Display.drawString(line2, layout.w / 2, layout.h / 2 + 14);
    }
}

void renderDiagnostics() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setFont(&fonts::Font2);
    constexpr int DIAGNOSTIC_ROWS = 8;
    if (2 + M5.Display.fontHeight() * DIAGNOSTIC_ROWS > layout.h) {
        M5.Display.setFont(&fonts::Font0);
    }
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(2, 2);

    M5.Display.printf("voice : %s\n", fanfare::VOICES[voiceIndex].name);
    M5.Display.printf("spkr  : %s\n", M5.Speaker.isEnabled() ? "yes" : "NONE");
    M5.Display.printf("rtc   : %s\n", M5.Rtc.isEnabled() ? "yes" : "no");
    M5.Display.printf("ntp   : %s\n", timeVerified ? "synced" : "UNVERIFIED");
    M5.Display.printf("batt  : %d%%\n", M5.Power.getBatteryLevel());
    M5.Display.printf("leds  : %s\n", LED_ENABLED ? (lightsOn ? "on" : "off") : "disabled");
    M5.Display.printf("panel : %dx%d\n", layout.w, layout.h);

    const time_t now = time(nullptr);
    struct tm* utc = gmtime(&now);
    if (utc) {
        M5.Display.printf("utc   : %02d:%02d:%02d\n", utc->tm_hour, utc->tm_min, utc->tm_sec);
    }
}

// ---------------------------------------------------------------------------
// Celebration
// ---------------------------------------------------------------------------

void drawCelebration(LovyanGFX* g, uint32_t elapsedMs, bool flashing) {
    static const uint16_t palette[6] = {TFT_RED,  TFT_YELLOW, TFT_GREEN,
                                        TFT_CYAN, TFT_BLUE,   TFT_MAGENTA};
    const uint16_t accent = flashing ? palette[(elapsedMs / 90) % 6] : TFT_CYAN;

    // The flash is a border, not a background fill: a cycling fill would put the
    // title's own markup colours against a clashing colour and, at worst, render
    // red text on a red screen.
    g->fillRect(0, 0, layout.w, layout.h, TFT_BLACK);
    g->drawRect(0, 0, layout.w, layout.h, accent);
    g->drawRect(1, 1, layout.w - 2, layout.h - 2, accent);

    g->setFont(layout.titleFont);
    const int titleH = g->fontHeight();
    const int titleY = layout.h / 2 - titleH - 2;
    celebrationScrolling = drawMarqueeTitle(g, titles[0].c_str(), titleY, titleH + 2, elapsedMs) > 0;

    g->setFont(layout.labelFont);
    g->setTextDatum(middle_center);
    g->setTextColor(accent);
    g->drawString("IT'S TIME", layout.w / 2, layout.h / 2 + 20);
}

void renderCelebration(uint32_t elapsedMs, bool flashing) {
    ensureSprite();
    if (spriteReady) {
        drawCelebration(&sprite(), elapsedMs, flashing);
        sprite().pushSprite(0, 0);
    } else {
        drawCelebration(&M5.Display, elapsedMs, flashing);
    }
}

void performCelebration() {
    const fanfare::Voice& voice = fanfare::VOICES[voiceIndex];
    const float transpose = fanfare::TRANSPOSE[voiceIndex];

    // Captured now, printed at the end: Serial blocks for milliseconds at
    // 115200 and would delay the very attack this is trying to measure.
    struct timeval attack;
    gettimeofday(&attack, nullptr);
    const uint32_t start = millis();
    uint32_t scheduled = 0;

    for (size_t i = 0; i < voice.length; ++i) {
        const fanfare::Note& note = voice.notes[i];

        // Audio first, always. Rendering is slow and must not delay the attack.
        if (note.hz > 0.0f && M5.Speaker.isEnabled()) {
            M5.Speaker.tone(note.hz * transpose, note.ms);
        }

        // Anchored to the absolute schedule, not to "now": an overrunning note
        // is absorbed by the next one rather than accumulating. Relative timing
        // drifts voices apart in proportion to their differing note counts.
        scheduled += note.ms;
        const uint32_t noteEnd = start + scheduled;
        while (static_cast<int32_t>(noteEnd - millis()) > 0) {
            renderCelebration(millis() - start, true);
            M5.delay(1);
        }
    }

    M5.Speaker.stop();
    const uint32_t measured = millis() - start;

    const int64_t attackUs =
        static_cast<int64_t>(attack.tv_sec) * 1000000LL + static_cast<int64_t>(attack.tv_usec);
    const int64_t skewUs = attackUs - static_cast<int64_t>(EVENT_EPOCH_UTC) * 1000000LL;

    Serial.println("fire");
    Serial.printf("  voice   : %u (%s)\n", static_cast<unsigned>(voiceIndex), voice.name);
    Serial.printf("  target  : %lu.000000\n", static_cast<unsigned long>(EVENT_EPOCH_UTC));
    Serial.printf("  attack  : %lu.%06lu\n", static_cast<unsigned long>(attack.tv_sec),
                  static_cast<unsigned long>(attack.tv_usec));
    // Skew is the number to compare between units. Printed in microseconds when
    // it is small enough to matter, seconds when the target had already passed.
    if (skewUs > -1000000LL && skewUs < 1000000LL) {
        Serial.printf("  skew    : %+ld us\n", static_cast<long>(skewUs));
    } else {
        Serial.printf("  skew    : %+ld s (late start, not a sync measurement)\n",
                      static_cast<long>(skewUs / 1000000LL));
    }
    Serial.printf("  duration: %lu ms (expect %u)\n", static_cast<unsigned long>(measured),
                  fanfare::DURATION_MS);
}

void auditionAllVoices() {
    Serial.printf("audition: %u voices, %u ms each\n", static_cast<unsigned>(fanfare::VOICE_COUNT),
                  fanfare::DURATION_MS);

    for (size_t i = 0; i < fanfare::VOICE_COUNT; ++i) {
        const fanfare::Voice& voice = fanfare::VOICES[i];
        char header[32];
        snprintf(header, sizeof(header), "%u/%u %s", static_cast<unsigned>(i + 1),
                 static_cast<unsigned>(fanfare::VOICE_COUNT), voice.name);
        renderMessage("AUDITION", header);

        float lowest = 0.0f;
        float highest = 0.0f;
        const uint32_t start = millis();

        for (size_t n = 0; n < voice.length; ++n) {
            const fanfare::Note& note = voice.notes[n];
            if (note.hz > 0.0f) {
                const float hz = note.hz * fanfare::TRANSPOSE[i];
                if (lowest == 0.0f || hz < lowest) {
                    lowest = hz;
                }
                if (hz > highest) {
                    highest = hz;
                }
                if (M5.Speaker.isEnabled()) {
                    M5.Speaker.tone(hz, note.ms);
                }
            }
            M5.delay(note.ms);
        }
        M5.Speaker.stop();

        // Measured vs expected is the part that can be checked without ears.
        Serial.printf("  %-7s %2u notes  %4lu ms (expect %u)  %.0f-%.0f Hz\n", voice.name,
                      static_cast<unsigned>(voice.length),
                      static_cast<unsigned long>(millis() - start), fanfare::DURATION_MS, lowest,
                      highest);
        M5.delay(600);
    }
    Serial.println("audition: done");
}

void sweepTones() {
    // Piezo buzzers have a sharp resonant peak whose position varies by part and
    // is coloured by the enclosure. Measure this unit rather than trusting a
    // datasheet: whichever step is loudest is where the fanfare should sit.
    static const float steps[] = {440.0f,  523.0f,  700.0f,  880.0f,  1047.0f, 1400.0f,
                                  1760.0f, 2093.0f, 2500.0f, 3000.0f, 3520.0f, 4000.0f,
                                  4500.0f, 5000.0f, 6000.0f, 7000.0f};
    constexpr size_t count = sizeof(steps) / sizeof(steps[0]);

    Serial.printf("sweep: %u steps, 400 ms each. Note which are loudest.\n",
                  static_cast<unsigned>(count));

    for (size_t i = 0; i < count; ++i) {
        char label[16];
        snprintf(label, sizeof(label), "%.0f Hz", steps[i]);
        renderMessage("SWEEP", label);
        Serial.printf("  %2u/%u  %5.0f Hz\n", static_cast<unsigned>(i + 1),
                      static_cast<unsigned>(count), steps[i]);

        if (M5.Speaker.isEnabled()) {
            M5.Speaker.tone(steps[i], 400);
        }
        M5.delay(600);
    }
    M5.Speaker.stop();
    Serial.println("sweep: done");
}

void quickTone() {
    constexpr float TEST_HZ = 523.0f;
    constexpr uint32_t TEST_MS = 250;

    Serial.printf("tone: %.0f Hz for %lu ms\n", TEST_HZ, static_cast<unsigned long>(TEST_MS));
    if (M5.Speaker.isEnabled()) {
        M5.Speaker.tone(TEST_HZ, TEST_MS);
        M5.delay(TEST_MS + 100);
        M5.Speaker.stop();
    }
    Serial.println("tone: done");
}

// One cycle of a full-scale square wave, 8-bit unsigned. The default tone()
// waveform is a sine; a square pushes a 1-bit delta-sigma buzzer much harder.
const uint8_t SQUARE_WAVE[16] = {255, 255, 255, 255, 255, 255, 255, 255,
                                 0,   0,   0,   0,   0,   0,   0,   0};

void setMagnification(uint8_t magnification) {
    auto cfg = M5.Speaker.config();
    cfg.magnification = magnification;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);
}

void driveTest() {
    // Frequency was ruled out by the sweep being uniformly quiet, so isolate the
    // two remaining levers: delta-sigma magnification, and waveform shape.
    struct Mode {
        const char* label;
        uint8_t magnification;
        bool square;
    };
    static const Mode modes[] = {
        {"A sine  mag48", 48, false},
        {"B sine  mag255", 255, false},
        {"C square mag48", 48, true},
        {"D square mag255", 255, true},
    };
    constexpr size_t count = sizeof(modes) / sizeof(modes[0]);
    constexpr float TEST_HZ = 4000.0f;

    const uint8_t original = M5.Speaker.config().magnification;
    Serial.printf("drive: %u modes at %.0f Hz, 800 ms each. Which is loudest?\n",
                  static_cast<unsigned>(count), TEST_HZ);

    for (size_t i = 0; i < count; ++i) {
        const Mode& mode = modes[i];
        renderMessage("DRIVE", mode.label);
        Serial.printf("  %u/%u  %s\n", static_cast<unsigned>(i + 1), static_cast<unsigned>(count),
                      mode.label);

        setMagnification(mode.magnification);
        if (M5.Speaker.isEnabled()) {
            if (mode.square) {
                M5.Speaker.tone(TEST_HZ, 800, 0, true, SQUARE_WAVE, sizeof(SQUARE_WAVE));
            } else {
                M5.Speaker.tone(TEST_HZ, 800);
            }
        }
        M5.delay(1200);
    }

    M5.Speaker.stop();
    setMagnification(original);
    Serial.println("drive: done");
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void onNtpSync(struct timeval*) {
    ntpSyncReceived = true;
}

bool syncFromNtp() {
    const String ssid = settings::ssid();
    if (ssid.length() == 0) {
        Serial.println("  wifi    : NOT PROVISIONED (use the web installer to set Wi-Fi)");
        return false;
    }

    WiFi.mode(WIFI_STA);
    if (messaging::enabled()) {
        // Modem sleep adds hundreds of ms to inbound delivery. Units with
        // messaging are on USB power, so latency wins.
        WiFi.setSleep(false);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(ssid.c_str(), settings::password().c_str());
    }

    const uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        M5.delay(100);
    }

    bool ok = false;
    const bool connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("  wifi    : %s\n", connected ? "connected" : "TIMEOUT");

    if (connected) {
        Serial.printf("  ip      : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  dns     : %s\n", WiFi.dnsIP().toString().c_str());

        IPAddress resolved;
        const bool dnsOk = WiFi.hostByName(NTP_SERVER_1, resolved);
        Serial.printf("  %s : %s\n", NTP_SERVER_1,
                      dnsOk ? resolved.toString().c_str() : "RESOLVE FAILED");

        // Start SNTP only once the link is up. Started any earlier, the first
        // request fails DNS and lwip then backs off for CONFIG_LWIP_SNTP_UPDATE_DELAY
        // - one hour by default - so no wait here would ever see a sync.
        ntpSyncReceived = false;
        sntp_set_time_sync_notification_cb(onNtpSync);
        configTzTime("UTC0", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

        const uint32_t ntpDeadline = millis() + NTP_TIMEOUT_MS;
        while (millis() < ntpDeadline) {
            if (ntpSyncReceived && time(nullptr) > SANE_EPOCH) {
                ok = true;
                break;
            }
            M5.delay(100);
        }

        if (ok && M5.Rtc.isEnabled()) {
            // Align to a second boundary before writing, so every unit's RTC
            // agrees to well under the ensemble's tolerance.
            time_t t = time(nullptr) + 1;
            while (t > time(nullptr)) {
            }
            M5.Rtc.setDateTime(gmtime(&t));
        }
    }

    Serial.printf("  ntp     : %s\n", ok ? "synced" : "FAILED");
    if (ok) {
        const time_t now = time(nullptr);
        struct tm* utc = gmtime(&now);
        if (utc) {
            Serial.printf("  utc     : %04d-%02d-%02d %02d:%02d:%02d\n", utc->tm_year + 1900,
                          utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
        }
        Serial.printf("  remain  : %lld s\n",
                      static_cast<long long>(EVENT_EPOCH_UTC) - static_cast<long long>(now));
    }

    if (messaging::enabled()) {
        // The radio stays up for MQTT. Auto-reconnect covers a dropped link.
        WiFi.setAutoReconnect(true);
        if (connected) {
            messaging::begin(deviceIdStr);  // idempotent
        }
        // Lock and fire also arrive over ESP-NOW, with or without the AP.
        espnow_link::begin(ESPNOW_CHANNEL, connected);
        Serial.printf("  heap    : %u free\n", static_cast<unsigned>(ESP.getFreeHeap()));
    } else {
        // Free ~40-50 KB of heap for the sprite, and stop burning battery.
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }

    if (ok) {
        timeVerified = true;
        lastSyncMs = millis();
    }
    return ok;
}

void waitForFireInstant() {
    const int64_t targetUs = static_cast<int64_t>(EVENT_EPOCH_UTC) * 1000000LL;
    for (;;) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        const int64_t nowUs = static_cast<int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec;
        const int64_t remainingUs = targetUs - nowUs;
        if (remainingUs <= 0) {
            return;
        }
        if (remainingUs > 5000) {
            M5.delay(1);  // yields, keeps the watchdog fed
        }
    }
}

void configureStickS3SpkHat2(bool enabled) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!enabled || M5.getBoard() != m5::board_t::board_M5StickS3) {
        return;
    }

    M5.Power.setExtOutput(true);

    auto speakerConfig = M5.Speaker.config();
    speakerConfig.pin_bck = GPIO_NUM_0;
    speakerConfig.pin_ws = GPIO_NUM_8;
    speakerConfig.pin_data_out = GPIO_NUM_1;
    speakerConfig.pin_mck = GPIO_NUM_NC;
    speakerConfig.i2s_port = I2S_NUM_0;
    speakerConfig.use_dac = false;
    speakerConfig.buzzer = false;
    speakerConfig.stereo = false;
    speakerConfig.magnification = 16;
    M5.Speaker.config(speakerConfig);
    M5.Speaker.begin();
#else
    (void)enabled;
#endif
}

}  // namespace

void setup() {
    // Explicit rather than relying on M5.begin() to configure UART0: the boot
    // banner is the only way to read a unit's assigned voice.
    Serial.begin(115200);
    M5.delay(200);

    // NVS is ready before setup() runs, so the stored speaker can be applied to
    // the very first M5.begin() rather than needing a second restart.
    settings::begin();
    const settings::SpeakerOption& speaker = settings::speaker();

    auto cfg = M5.config();
    // Speaker hats are not detectable at runtime, so the fitted one is configured.
    cfg.internal_spk = speaker.internal;
    cfg.external_speaker.hat_spk = speaker.hatSpk;
    cfg.external_speaker.hat_spk2 = speaker.hatSpk2;
    M5.begin(cfg);
    configureStickS3SpkHat2(speaker.hatSpk2);

    loadTitles();

    M5.Display.setRotation(1);
    M5.Display.setBrightness(BRIGHTNESS);
    computeLayout();

    voiceIndex = selectVoice();

    const uint64_t efuse = ESP.getEfuseMac();
    uint8_t mac[6];
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(efuse >> (8 * i));
    }

    Serial.printf("\nshowcase-countdown\n");
    Serial.printf("  board   : %d\n", static_cast<int>(M5.getBoard()));
    Serial.printf("  panel   : %dx%d\n", layout.w, layout.h);
    Serial.printf("  speaker : %s (%s)\n", M5.Speaker.isEnabled() ? "present" : "ABSENT",
                  speaker.name);
    if (M5.Speaker.isEnabled()) {
        const auto speakerConfig = M5.Speaker.config();
        Serial.printf("  audio   : BCK=%d LRCLK=%d DATA=%d\n", speakerConfig.pin_bck,
                      speakerConfig.pin_ws, speakerConfig.pin_data_out);
    }
    Serial.printf("  event   : %s\n", plainTitle.c_str());
    Serial.printf("  titles  : %u\n", static_cast<unsigned>(titles.size()));
    Serial.printf("  target  : %lld\n", static_cast<long long>(EVENT_EPOCH_UTC));
    // MAC and roll are printed so `pio run -t fleet` output can be reconciled
    // against scripts/fleet_voices.py.
    Serial.printf("  mac     : %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    Serial.printf("  roll    : %u\n", voice::rollFor(efuse));
    Serial.printf("  voice   : %u (%s)\n", voiceIndex, fanfare::VOICES[voiceIndex].name);
    Serial.printf("  wifi    : %s\n",
                  settings::wifiConfigured() ? "provisioned" : "NOT PROVISIONED");

    snprintf(deviceIdStr, sizeof(deviceIdStr), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    claimCode = claimCodeFor(deviceIdStr, CLAIM_SALT);
    Serial.printf("  id      : %s\n", deviceIdStr);
    Serial.printf("  claim   : %04u\n", static_cast<unsigned>(claimCode));
    Serial.printf("  mqtt    : %s\n", messaging::enabled() ? MQTT_HOST : "disabled");
    printSerialHelp();

    improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, FIRMWARE_NAME, FIRMWARE_VERSION,
                         FIRMWARE_NAME);
    improv.onImprovConnected(onImprovConnected);

    if (M5.Speaker.isEnabled()) {
        const uint8_t volume =
            M5.getBoard() == m5::board_t::board_M5StickS3 && speaker.hatSpk2 ? 128 : 255;
        M5.Speaker.setVolume(volume);
        M5.Speaker.setAllChannelVolume(volume);
    }

    // Seed from the RTC first so the countdown is live before any network
    // attempt. A blank screen during a 10 s WiFi timeout reads as a crash.
    if (M5.Rtc.isEnabled()) {
        M5.Rtc.setSystemTimeFromRtc();
    }
    initializeLights();

    M5.update();
    if (M5.BtnA.isPressed()) {
        auditionAllVoices();
    }

    // Claim the 64 KB sprite before WiFi, MQTT and ESP-NOW fragment the heap.
    // Falling back to direct drawing costs render time inside the fanfare's
    // note loop, which is where jitter turns into an audible ragged attack.
    ensureSprite();

    titleStartMs = millis();
    renderMessage(plainTitle.c_str(), "syncing...");
    syncFromNtp();
}

void loop() {
    M5.update();

    // Serial trigger as well as the button, so a mounted unit can be auditioned
    // without being taken down.
    while (Serial.available()) {
        // A title may legitimately begin with 'I', so the Improv sniff is
        // suspended while a typed value is being collected.
        if (activePrompt != Prompt::None) {
            const int c = Serial.read();
            if (c == '\r' || c == '\n') {
                applyPrompt();
            } else if (c == 0x1B) {
                activePrompt = Prompt::None;
                promptBuffer = "";
                Serial.println("\n  cancelled");
            } else if (c == 0x08 || c == 0x7F) {
                if (promptBuffer.length() > 0) {
                    promptBuffer.remove(promptBuffer.length() - 1);
                    Serial.print("\b \b");
                }
            } else if (c >= 0x20 && promptBuffer.length() < 160) {
                promptBuffer += static_cast<char>(c);
                Serial.write(static_cast<char>(c));
            }
            continue;
        }

        if (collectingCommand) {
            const int c = Serial.read();
            if (c == '\r' || c == '\n') {
                collectingCommand = false;
                commandBuffer.trim();
                handleMachineCommand(commandBuffer);
                commandBuffer = "";
            } else if (commandBuffer.length() < 200) {
                commandBuffer += static_cast<char>(c);
            }
            continue;
        }

        if (millis() < improvBurstUntil || Serial.peek() == 'I') {
            improv.handleSerial();
            improvBurstUntil = millis() + IMPROV_BURST_MS;
            continue;
        }
        const int command = Serial.read();
        if (command == '!') {
            collectingCommand = true;
            commandBuffer = "";
        } else if (command == 'a' || command == 'A') {
            auditionAllVoices();
        } else if (command == 's' || command == 'S') {
            syncFromNtp();
        } else if (command == 'q' || command == 'Q') {
            quickTone();
        } else if (command == 't' || command == 'T') {
            sweepTones();
        } else if (command == 'm' || command == 'M') {
            driveTest();
        } else if (command == 'l' || command == 'L') {
            toggleLights("serial");
        } else if (command == 'c' || command == 'C') {
            beginPrompt(Prompt::Color);
        } else if (command == 'n' || command == 'N') {
            nextLedAnimation();
        } else if (command == 'j' || command == 'J') {
            messaging::Audio a = {};
            strncpy(a.notes, jingles::find("tada"), sizeof(a.notes) - 1);
            strncpy(a.jingle, "tada", sizeof(a.jingle) - 1);
            onAudioCommand(a);
        } else if (command == 'i' || command == 'I') {
            printSettings();
        } else if (command == 'e' || command == 'E') {
            beginPrompt(Prompt::EventTitles);
        } else if (command == 'p' || command == 'P') {
            beginPrompt(Prompt::Speaker);
        } else if (command == 'z' || command == 'Z') {
            beginPrompt(Prompt::Timezone);
        } else if (command == 'w' || command == 'W') {
            settings::clearWifi();
            Serial.println("  wifi    : stored credentials cleared");
        } else if (command == 'x' || command == 'X') {
            settings::resetConfigurable();
            loadTitles();
            Serial.println("  settings reset to built-in defaults (restarting)");
            Serial.flush();
            ESP.restart();
        } else if (command == '?') {
            printSerialHelp();
        }
    }

    if (M5.BtnA.wasDoubleClicked()) {
        messaging::publishButton('A', "double");
        toggleLights("button");
    } else if (M5.BtnA.wasSingleClicked()) {
        messaging::publishButton('A', "click");
        renderMessage(plainTitle.c_str(), "syncing...");
        syncFromNtp();
    }
    if (M5.BtnB.wasPressed()) {
        messaging::publishButton('B', "press");
        showDiagnostics = !showDiagnostics;
    }

    static const messaging::Handlers handlers = {onDisplayCommand, onConfigCommand,
                                                 onAudioCommand, onLedCommand};
    messaging::poll(handlers);
    serviceSequence();
    serviceLedOverride();
    if (messaging::connected() && !nearFanfare() &&
        millis() - lastStatePublishMs >= STATE_PUBLISH_MS) {
        lastStatePublishMs = millis();
        messaging::Status status = {fanfare::VOICES[voiceIndex].name,
                                    M5.Power.getBatteryLevel(), timeVerified, fired,
                                    claimCode, teamName};
        messaging::publishState(status);
    }

    bool lockFrame = false;
    if (espnow_link::takeLock(&lockFrame)) {
        roomLocked = lockFrame;
        Serial.printf("  espnow  : %s\n", roomLocked ? "lock" : "unlock");
    }
    int64_t fireFrame = 0;
    if (espnow_link::takeFire(&fireFrame)) {
        // Last-resort path for a unit that never got a trusted clock: the
        // bridge sends this three seconds before the target.
        //
        // A frame is only ever acted on when this unit has NO verified time of
        // its own. ESP-NOW is unauthenticated broadcast and the event epoch is
        // public, so a unit that knows the time must never let a frame move its
        // clock - otherwise anyone in radio range could fire the whole room at
        // any hour, and `fired` would latch for the rest of the day.
        if (fired || fireFrame != static_cast<int64_t>(EVENT_EPOCH_UTC)) {
            Serial.printf("  espnow  : ignored fire for %lld\n", static_cast<long long>(fireFrame));
        } else if (timeVerified) {
            Serial.println("  espnow  : fire frame ignored, this unit has a verified clock");
        } else {
            struct timeval tv = {static_cast<time_t>(fireFrame - FIRE_ARM_WINDOW_S), 0};
            settimeofday(&tv, nullptr);
            Serial.println("  espnow  : fire frame accepted, no verified clock of our own");
        }
    }

    const time_t now = time(nullptr);
    int64_t remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(now);
    updateLightSchedule(now);

    if (!fired && !preEventSyncAttempted && now > SANE_EPOCH &&
        remaining <= PRE_EVENT_SYNC_LEAD_S && remaining > FIRE_ARM_WINDOW_S) {
        preEventSyncAttempted = true;
        renderMessage(plainTitle.c_str(), "final sync...");
        syncFromNtp();
        remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    } else if (!fired && timeVerified && millis() - lastSyncMs >= RESYNC_INTERVAL_MS) {
        syncFromNtp();
        remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    }

    if (!fired && remaining <= FIRE_ARM_WINDOW_S) {
        stopSequence();  // nothing plays over the fanfare
        // A team's play() may have left the volume anywhere; the fanfare is
        // the one thing that must be heard.
        if (M5.Speaker.isEnabled()) {
            M5.Speaker.setVolume(255);
            M5.Speaker.setAllChannelVolume(255);
        }
        waitForFireInstant();
        performCelebration();
        fired = true;
        return;
    }

    if (fired) {
        // Keep rendering so a long title carries on scrolling after the melody.
        renderCelebration(millis(), false);
        M5.delay(celebrationScrolling ? FRAME_MS_SCROLLING : FRAME_MS_IDLE);
        return;
    }

    if (showDiagnostics) {
        renderDiagnostics();
        M5.delay(FRAME_MS_IDLE);
        return;
    }

    if (overlay.active && static_cast<int32_t>(millis() - overlay.untilMs) >= 0) {
        overlay.active = false;
    }
    if (overlay.active) {
        renderOverlay(millis() - overlay.startMs);
        M5.delay(overlay.scrolling ? FRAME_MS_SCROLLING : FRAME_MS_IDLE);
        return;
    }

    renderCountdown(remaining);

    if (titles.size() > 1 && millis() - titleStartMs >= titleCycleMs + TITLE_GAP_MS) {
        titleIndex = (titleIndex + 1) % titles.size();
        titleStartMs = millis();
    }

    // Only pay for a fast redraw while a title is actually moving.
    M5.delay(titleScrolling ? FRAME_MS_SCROLLING : FRAME_MS_IDLE);
}
