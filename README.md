# showcase-countdown

A countdown timer for the M5StickC PLUS SE. Each unit shows an event name and
the time remaining, keeps itself accurate over NTP, and when the moment arrives
plays **one voice of a three-part fanfare** — so a room full of them performs
the piece together.

There is no radio link between units at the moment they fire. The shared,
NTP-disciplined clock is the only synchronisation mechanism.

## Hardware

Target is the **StickC PLUS SE** (SKU K016-P-SE). The **StickC PLUS 1.1** is a
near-exact proxy and can run everything including the fanfare — same
ESP32-PICO-D4, same `m5stick-c` board ID, same 135x240 ST7789v2 panel, same
BM8563 RTC, same AXP192, same buzzer on G2.

| Device | Builds | Countdown | NTP/RTC | Fanfare |
|---|---|---|---|---|
| StickC PLUS SE | `stick` | yes | yes | yes |
| StickC PLUS 1.1 | `stick` | yes | yes | yes |
| StickC (non-Plus) | `stick` | yes, 80x160 | yes | **no buzzer** |
| StickS3 | `sticks3` | unverified | unverified | unverified |

The layout derives everything from `M5.Display.width()/height()`, so one source
tree serves all of them.

## Setup

```sh
cp .env.template .env      # then fill it in
pio run -e stick           # build
pio run -e stick -t upload # flash
pio device monitor
```

`.env` is git-ignored. `.env.template` documents the structure and is committed.

Secrets never reach the compiler command line: `scripts/load_env.py` runs as a
pre-build step and generates `env_config.h` into the build directory. That also
means `EVENT_DATETIME` is resolved to a Unix epoch at build time, so the
firmware never parses a date or carries a timezone database.

The build fails loudly if `.env` is missing, if a required key is absent, or if
`EVENT_DATETIME` has no UTC offset — an ambiguous countdown target is worse
than a failed build.

## Buttons

| Action | Effect |
|---|---|
| **A**, held during boot | Audition: play all three voices in sequence |
| **A** | Force an NTP re-sync |
| **B** | Toggle the diagnostics screen |

The device also re-syncs automatically every 6 hours.

## The fanfare

Four seconds, in C, built from **C-D-F-G only** — there is no third anywhere in
any voice, so the harmony is a deliberate open fifth. That is the classic
brass-fanfare sonority and it survives a square-wave piezo far better than a
close-voiced triad would.

| Voice | Weight | Role |
|---|---|---|
| root | 40% | C pedal, no movement of its own |
| fifth | 20% | The harmonic mover: G to F to G, giving Csus4 tension |
| melody | 40% | The only line with real contour |

Each unit picks its voice from a hash of its own MAC address, so a given device
always plays the same part and rehearsal matches the real event. The assigned
voice is printed on the serial console at boot and shown on the diagnostics
screen.

### Check your fleet before the event

The **fifth is the fragile voice**. Across 5 units there is roughly a 33% chance
that no unit draws it, and without it the ensemble thins to root-plus-melody.

| Fleet size | Chance of no fifth |
|---|---|
| 5 | 33% |
| 7 | 21% |
| 10 | 11% |

Because assignment is deterministic, you can check before flashing:

```sh
python scripts/fleet_voices.py E8:9F:6D:09:CC:04 4C:75:25:00:00:02 ...
```

It reports the voice each unit will draw, flags any voice nobody covers, and
names a specific device to override — choosing a donor that can spare a unit
rather than one that would leave a new gap. It exits non-zero when a voice is
missing, so it can gate a deploy script.

The MAC and roll are also printed on the serial console at boot, so predictions
can be reconciled against real devices.

The helper parses the weights out of `src/fanfare.h` rather than duplicating
them, and `tests/test_voice_assign.py` compiles `src/voice_assign.h` and diffs
the two implementations over 2000 MACs:

```sh
python tests/test_voice_assign.py
```

That matters because a helper whose hash had drifted from the firmware would
report a fleet as covered when it is not — worse than having no helper.

### Tuning on hardware

`TRANSPOSE` in `src/fanfare.h` is a per-voice frequency multiplier. These piezos
are weak below ~500 Hz, so if the root voice at C5 proves inaudible, set index 0
to `2.0f` to lift it an octave without touching the score.

All three voices must be exactly the same length. That is enforced by
`static_assert`, not by convention — retiming one voice without the others is a
build error, because the celebration animation runs for exactly the length of
the melody.

## Layout

```text
platformio.ini              Build environments
scripts/load_env.py         .env -> generated env_config.h (pre-build)
scripts/fleet_voices.py     Predict voice assignment across a fleet
src/fanfare.h               Voice tables and compile-time invariants
src/voice_assign.h          Deterministic MAC -> voice hash
src/main.cpp                Boot sequence, countdown, melody engine
tests/test_voice_assign.py  Cross-validates helper against firmware
.env.template               Committed structure documentation
```

## A note on secrets

WiFi credentials are compiled into the firmware. This is **not** a security
boundary — they sit in plaintext in the image and can be read back off the
flash. Fine for a countdown ornament; use a guest or IoT SSID rather than your
primary network credential, and do not reuse this pattern for anything
sensitive.
