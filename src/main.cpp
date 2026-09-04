#include <Adafruit_NeoPixel.h>
#include <M5Unified.h>
#include <WiFi.h>

#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#elif __has_include(<sntp.h>)
#include <sntp.h>
#endif

#include <string.h>

#include "env_config.h"
#include "fanfare.h"
#include "voice_assign.h"

#ifndef WIFI_SSID
#error "WIFI_SSID missing - copy .env.template to .env"
#endif

namespace {

constexpr uint32_t WIFI_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_TIMEOUT_MS = 15000;
constexpr uint32_t RESYNC_INTERVAL_MS = 6UL * 60 * 60 * 1000;  // requirement 3
constexpr int64_t PRE_EVENT_SYNC_LEAD_S = 5 * 60;

// Any real sync lands far past this; an unset clock starts at 0. Comparing the
// wall clock is more robust than sntp_get_sync_status(), whose return values
// differ across core versions and sync modes.
constexpr time_t SANE_EPOCH = 1750000000;  // 2025-06-15

const char* const TITLES[] = EVENT_TITLES;
constexpr size_t TITLE_COUNT = EVENT_TITLE_COUNT;

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

bool scheduledLightsOn(time_t now) {
    time_t localNow = now + LOCAL_UTC_OFFSET_SECONDS;
    struct tm local;
    gmtime_r(&localNow, &local);
    const int minuteOfDay = local.tm_hour * 60 + local.tm_min;

    if (LED_ON_MINUTE_OF_DAY < LED_OFF_MINUTE_OF_DAY) {
        return minuteOfDay >= LED_ON_MINUTE_OF_DAY && minuteOfDay < LED_OFF_MINUTE_OF_DAY;
    }
    return minuteOfDay >= LED_ON_MINUTE_OF_DAY || minuteOfDay < LED_OFF_MINUTE_OF_DAY;
}

void setLights(bool on, const char* source) {
    if (!LED_ENABLED) {
        return;
    }

    lightsOn = on;
    const uint32_t colour = on ? lights.Color(255, 255, 255) : 0;
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

    const bool scheduledOn = scheduledLightsOn(now);
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

    lights.begin();
    lights.clear();
    lights.show();
    Serial.printf("  leds    : %u x %s on GPIO %d\n", static_cast<unsigned>(LED_COUNT),
                  LED_TYPE_NAME, LED_PIN);
    updateLightSchedule(time(nullptr));
}

void toggleLights() {
    if (!LED_ENABLED) {
        Serial.println("  leds    : toggle ignored (disabled)");
        return;
    }
    setLights(!lightsOn, "button");
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
    if (TITLE_COUNT > 1 && elapsed >= titleCycleMs) {
        titleScrolling = false;
        return;
    }

    const int span = drawMarqueeTitle(g, TITLES[titleIndex], 2, titleH + 2, elapsed);
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

    constexpr int GAP = 2;
    const int bodyTop = ruleY + 1;
    const int blockH = numberH + GAP + labelH;
    const int blockTop = bodyTop + ((layout.h - bodyTop) - blockH) / 2;
    const int labelTop = blockTop + numberH + GAP;

    const Readout r = formatRemaining(remaining);
    drawUnit(g, r.first, r.firstLabel, layout.w / 4, blockTop, labelTop);
    drawUnit(g, r.second, r.secondLabel, (layout.w * 3) / 4, blockTop, labelTop);
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
    celebrationScrolling = drawMarqueeTitle(g, TITLES[0], titleY, titleH + 2, elapsedMs) > 0;

    g->setFont(layout.labelFont);
    g->setTextDatum(middle_center);
    g->setTextColor(accent);
    g->drawString("IT'S HERE", layout.w / 2, layout.h / 2 + 20);
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
    const uint32_t start = millis();

    for (size_t i = 0; i < voice.length; ++i) {
        const fanfare::Note& note = voice.notes[i];

        // Audio first, always. Rendering is slow and must not delay the attack.
        if (note.hz > 0.0f && M5.Speaker.isEnabled()) {
            M5.Speaker.tone(note.hz * transpose, note.ms);
        }

        const uint32_t noteEnd = millis() + note.ms;
        while (static_cast<int32_t>(noteEnd - millis()) > 0) {
            renderCelebration(millis() - start, true);
            M5.delay(1);
        }
    }

    M5.Speaker.stop();
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
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

    // Free ~40-50 KB of heap for the sprite, and stop burning battery.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

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

}  // namespace

void setup() {
    // Explicit rather than relying on M5.begin() to configure UART0: the boot
    // banner is the only way to read a unit's assigned voice.
    Serial.begin(115200);
    M5.delay(200);

    auto cfg = M5.config();
    M5.begin(cfg);

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
    Serial.printf("  speaker : %s\n", M5.Speaker.isEnabled() ? "present" : "ABSENT");
    Serial.printf("  event   : %s\n", EVENT_NAME);
    Serial.printf("  titles  : %u\n", static_cast<unsigned>(TITLE_COUNT));
    Serial.printf("  target  : %lld\n", static_cast<long long>(EVENT_EPOCH_UTC));
    // MAC and roll are printed so `pio run -t fleet` output can be reconciled
    // against scripts/fleet_voices.py.
    Serial.printf("  mac     : %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    Serial.printf("  roll    : %u\n", voice::rollFor(efuse));
    Serial.printf("  voice   : %u (%s)\n", voiceIndex, fanfare::VOICES[voiceIndex].name);
    Serial.println("  keys    : 'a' audition, 's' resync, 't' tone sweep, 'm' drive test");

    if (M5.Speaker.isEnabled()) {
        M5.Speaker.setVolume(255);
        M5.Speaker.setAllChannelVolume(255);
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

    titleStartMs = millis();
    renderMessage(EVENT_NAME, "syncing...");
    syncFromNtp();
}

void loop() {
    M5.update();

    // Serial trigger as well as the button, so a mounted unit can be auditioned
    // without being taken down.
    while (Serial.available()) {
        const int command = Serial.read();
        if (command == 'a' || command == 'A') {
            auditionAllVoices();
        } else if (command == 's' || command == 'S') {
            syncFromNtp();
        } else if (command == 't' || command == 'T') {
            sweepTones();
        } else if (command == 'm' || command == 'M') {
            driveTest();
        }
    }

    if (M5.BtnA.wasDoubleClicked()) {
        toggleLights();
    } else if (M5.BtnA.wasSingleClicked()) {
        renderMessage(EVENT_NAME, "syncing...");
        syncFromNtp();
    }
    if (M5.BtnB.wasPressed()) {
        showDiagnostics = !showDiagnostics;
    }

    const time_t now = time(nullptr);
    int64_t remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(now);
    updateLightSchedule(now);

    if (!fired && !preEventSyncAttempted && now > SANE_EPOCH &&
        remaining <= PRE_EVENT_SYNC_LEAD_S && remaining > FIRE_ARM_WINDOW_S) {
        preEventSyncAttempted = true;
        renderMessage(EVENT_NAME, "final sync...");
        syncFromNtp();
        remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    } else if (!fired && timeVerified && millis() - lastSyncMs >= RESYNC_INTERVAL_MS) {
        syncFromNtp();
        remaining = static_cast<int64_t>(EVENT_EPOCH_UTC) - static_cast<int64_t>(time(nullptr));
    }

    if (!fired && remaining <= FIRE_ARM_WINDOW_S) {
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

    renderCountdown(remaining);

    if (TITLE_COUNT > 1 && millis() - titleStartMs >= titleCycleMs + TITLE_GAP_MS) {
        titleIndex = (titleIndex + 1) % TITLE_COUNT;
        titleStartMs = millis();
    }

    // Only pay for a fast redraw while a title is actually moving.
    M5.delay(titleScrolling ? FRAME_MS_SCROLLING : FRAME_MS_IDLE);
}
