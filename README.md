---
title: showcase-countdown
description: Build and configure an NTP-synchronized M5StickC event countdown
---

A countdown timer for the M5StickC PLUS SE. Each unit shows an event name and
the time remaining, and disciplines its RTC from NTP so that a shelf full of
them agrees with each other rather than drifting apart over the weeks before the
event.

WiFi credentials, the event name and the target date all come from a `.env` file
that is never committed. Nothing device-specific is hard-coded in the source.

## Hardware

| Item | SKU | Price | Required |
| --- | --- | --- | --- |
| [M5StickC PLUS SE Mini IoT Dev Kit](https://shop.m5stack.com/products/m5stickc-plus-se-mini-iot-dev-kit-esp32-pico) | K016-P-SE | US$19.00 | yes |
| [Hat SPK2 speaker, MAX98357](https://shop.m5stack.com/products/m5stickcplus-speaker-2-hat-max98357) | U055-B | US$4.95 | optional |

Prices are from the M5Stack store at the time of writing. The store ships from
Shenzhen; if you would rather buy locally, M5Stack publishes a
[distributor list](https://m5stack.com/distributor). Product documentation lives
at [K016-P-SE](https://docs.m5stack.com/en/products/sku/K016-P-SE) and
[Hat SPK2](https://docs.m5stack.com/en/hat/hat-spk2).

### About the speaker HAT

The Stick has an onboard piezo buzzer, but it is barely audible across a room.
The SPK2 HAT plugs straight onto the 8-pin Hat-Bus and drives a 1 W speaker
through a MAX98357 I2S amplifier — worth the US$5 if you want the unit to be
heard rather than just seen.

Note that there are two speaker HATs in the M5Stack catalogue and they are
**not** interchangeable: the older
[Speaker Hat](https://shop.m5stack.com/products/m5stickc-speaker-hat) is an
analogue PAM8303 fed from the DAC pin, while SPK2 is I2S. Get SPK2.

### Device compatibility

Target is the **StickC PLUS SE** (SKU K016-P-SE). The **StickC PLUS 1.1** is a
near-exact proxy — same ESP32-PICO-D4, same `m5stick-c` board ID, same 135x240
ST7789v2 panel, same BM8563 RTC, same AXP192 — so it makes a fine development
unit if you already own one.

| Device | Environment | Countdown | NTP/RTC |
| --- | --- | --- | --- |
| StickC PLUS SE | `stick` | yes | yes |
| StickC PLUS 1.1 | `stick` | yes | yes |
| StickC (non-Plus) | `stick` | yes, 80x160 | yes |
| StickS3 | `sticks3` | unverified | unverified |

The layout derives everything from `M5.Display.width()/height()`, so one source
tree serves all of them.

## Setup and building

### Prerequisites

* [PlatformIO](https://platformio.org/install) — either the VS Code extension
  (recommended, and this repo suggests it in `.vscode/extensions.json`) or
  PlatformIO Core on its own.
* Python 3, which PlatformIO installs into its own virtual environment.
* On Windows, the [FTDI VCP driver](https://ftdichip.com/drivers/vcp-drivers/).
  The PLUS SE uses an FTDI USB bridge rather than the CH9102 found on newer
  Sticks, and Windows will not enumerate a COM port without it. Some machines
  need the driver installed twice before it takes.

### Configure

```sh
git clone https://github.com/coatsy/showcase-countdown
cd showcase-countdown
cp .env.template .env
```

`.env` is git-ignored; `.env.template` documents the structure and is committed.
The template covers every key, and the four with no sensible default are:

| Key | Notes |
| --- | --- |
| `WIFI_SSID` | 2.4 GHz only — the ESP32-PICO-D4 has no 5 GHz radio |
| `WIFI_PASSWORD` | See the note on secrets below |
| `EVENT_NAME` | One or more titles, separated by `\|` |
| `EVENT_DATETIME` | ISO-8601 with an **explicit** UTC offset or trailing `Z` |

`EVENT_NAME` carries a few conveniences. Multiple titles separated by `|` are
cycled; any title too wide for the screen scrolls marquee-style and is given at
least two full passes before the next one takes over. Colour markup works too —
`[red]`, `[skyblue]`, `[#00A4EF]`, and `[/]` to return to white:

```dotenv
EVENT_NAME="[red]Westpac[/] + [#00A4EF]Microsoft[/] Hackathon|Doors open 6pm"
```

The panel is RGB565, so colours are quantised to five bits per channel and brand
hex values often read duller than the named equivalents.

### Build and flash

```sh
pio run -e stick                    # build
pio run -e stick -t upload          # flash the first port found
pio device monitor                  # 115200 baud
```

To target a specific unit — which you will want when flashing several — pass the
port explicitly:

```sh
pio device list
pio run -e stick -t upload --upload-port COM3
```

A clean build lands at roughly 960 KB of the 3 MB `huge_app` partition.

### How the secrets get in

Secrets never reach the compiler command line: `scripts/load_env.py` runs as a
pre-build step and generates `env_config.h` into the build directory. That also
means `EVENT_DATETIME` is resolved to a Unix epoch at build time, so the
firmware never parses a date or carries a time zone database.

The build fails loudly if `.env` is missing, if a required key is absent, or if
`EVENT_DATETIME` has no UTC offset — an ambiguous countdown target is worse
than a failed build.

Because the target is baked in at compile time, **changing the event means a
rebuild and a reflash**, not just an edit.

## Buttons

| Action | Effect |
| --- | --- |
| **A** | Force an NTP re-sync |
| **B** | Toggle the diagnostics screen |

The device also re-syncs automatically every 6 hours and makes one additional
sync attempt when the countdown reaches 5 minutes.

## Layout

```text
platformio.ini              Build environments
scripts/load_env.py         .env -> generated env_config.h (pre-build)
scripts/read_serial.py      One-shot serial capture, for scripted checks
src/main.cpp                Boot sequence, display, countdown
.env.template               Committed structure documentation
```

Plus a few extras you can find for yourself.

## A note on secrets

WiFi credentials are compiled into the firmware. This is **not** a security
boundary — they sit in plaintext in the image and can be read back off the
flash. Fine for a countdown ornament; use a guest or IoT SSID rather than your
primary network credential, and do not reuse this pattern for anything
sensitive.
