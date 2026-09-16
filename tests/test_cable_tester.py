"""Exercise the actual tester sketch with fake time, button and GPIO hardware."""

import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STUB = r"""
#pragma once
#include <cstdint>
#include <cassert>
constexpr int INPUT = 0, OUTPUT = 1, TFT_BLACK = 0;
constexpr int NEO_GRB = 0, NEO_KHZ800 = 0;
uint32_t clockMs = 0;
int pinModeValue = INPUT;
uint32_t millis() { return clockMs; }
void delay(unsigned ms) { clockMs += ms; }
void pinMode(int, int mode) { pinModeValue = mode; }
struct Display {
    void fillScreen(int) {}
    void setCursor(int, int) {}
    void println(const char*) {}
    void setRotation(int) {}
    void setBrightness(int) {}
    void setTextSize(int) {}
    void begin(int) {}
} Serial;
struct Power {
    bool enabled = false, fault = false;
    void setExtOutput(bool on) { enabled = on && !fault; }
    bool getExtOutput() { return enabled; }
};
struct Button {
    bool pressed = false;
    bool wasPressed() { bool p = pressed; pressed = false; return p; }
};
struct Device {
    struct Config { bool internal_spk = true; };
    ::Display Display;
    ::Power Power;
    Button BtnA;
    Config config() { return {}; }
    void begin(Config) {}
    void update() {}
} M5;
struct Adafruit_NeoPixel {
    uint32_t colour = 0;
    Adafruit_NeoPixel(int, int, int) {}
    void begin() { pinModeValue = OUTPUT; }
    void clear() { colour = 0; }
    void fill(uint32_t c) { colour = c; }
    void show() {}
};
"""
CHECK = r"""
#include "src/cable_test/main.cpp"
void press() { M5.BtnA.pressed = true; loop(); }
int main() {
    setup();
    assert(!running && !M5.Power.enabled && pinModeValue == INPUT);
    press();
    assert(running && M5.Power.enabled && pixels.colour == 0x400000);
    for (unsigned step = 0; step < 16; ++step) {
        clockMs = started + step * 500;
        loop();
        assert(pixels.colour == colours[step % 4]);
    }
    clockMs = started + 8000;
    loop();
    assert(!running && !M5.Power.enabled && pixels.colour == 0);
    assert(pinModeValue == INPUT);
    press(); press();
    assert(!running && !M5.Power.enabled);
    M5.Power.fault = true;
    press();
    assert(!running && pinModeValue == INPUT);
    M5.Power.fault = false;
    clockMs = UINT32_MAX - 100;
    press();
    clockMs = started + 8000;
    loop();
    assert(!running && !M5.Power.enabled);
}
"""


def main():
    compiler = shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        raise SystemExit("A native C++ compiler (g++ or clang++) is required")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "Arduino.h").write_text(STUB)
        for name in ("M5Unified.h", "Adafruit_NeoPixel.h"):
            (tmp / name).write_text('#include "Arduino.h"\n')
        source = tmp / "test.cpp"
        source.write_text(CHECK)
        exe = tmp / "cable_test.exe"
        subprocess.run(
            [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-DCABLE_LED_COUNT=7", "-DCABLE_LED_PIN=32",
             "-I", str(tmp), "-I", str(ROOT), str(source), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
    print("Cable tester: colours, timeout, stop, power failure and clock wrap passed")


if __name__ == "__main__":
    main()
