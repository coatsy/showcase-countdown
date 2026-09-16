#include <Arduino.h>
#include <M5Unified.h>
#include <Adafruit_NeoPixel.h>

namespace {
Adafruit_NeoPixel pixels(CABLE_LED_COUNT, CABLE_LED_PIN, NEO_GRB + NEO_KHZ800);
constexpr uint32_t colours[] = {0x400000, 0x004000, 0x000040, 0x404040};
constexpr const char* names[] = {"RED", "GREEN", "BLUE", "WHITE"};
constexpr uint32_t stepMs = 500;
constexpr uint32_t durationMs = 8000;
bool running = false;
uint32_t started = 0;
unsigned lastStep = 0;

void screen(const char* message) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("GROVE CABLE TEST");
    M5.Display.println(message);
    Serial.println(message);
}

void stop() {
    pixels.clear();
    pixels.show();
    // Do not drive an unpowered pixel through its data pin.
    pinMode(CABLE_LED_PIN, INPUT);
    M5.Power.setExtOutput(false);
    running = false;
    screen("OFF - swap cable\nA: test for 8 sec\nCheck visually");
}

void start() {
    M5.Power.setExtOutput(true);
    if (!M5.Power.getExtOutput()) {
        M5.Power.setExtOutput(false);
        screen("ERROR: Grove power\nCheck hardware\nA: retry");
        return;
    }
    delay(10);
    pixels.begin();
    started = millis();
    lastStep = 0;
    running = true;
    pixels.fill(colours[0]);
    pixels.show();
    screen("RED\nA: stop");
}
}  // namespace

void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    cfg.internal_spk = false;
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setBrightness(100);
    M5.Display.setTextSize(2);
    pixels.begin();
    stop();
}

void loop() {
    M5.update();
    if (M5.BtnA.wasPressed()) {
        if (running) stop();
        else start();
    }
    if (running) {
        const uint32_t elapsed = millis() - started;
        if (elapsed >= durationMs) {
            stop();
        } else {
            const unsigned step = elapsed / stepMs;
            if (step != lastStep) {
                lastStep = step;
                pixels.fill(colours[step % 4]);
                pixels.show();
                screen(names[step % 4]);
                M5.Display.println("A: stop");
            }
        }
    }
    delay(5);
}
