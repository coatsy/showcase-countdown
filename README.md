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

Whichever HAT you fit, set `SPEAKER` in `.env` to match it. The firmware cannot
probe for one, and the default routes audio to the onboard buzzer instead.

Note that there are two speaker HATs in the M5Stack catalogue and they are
**not** interchangeable: the older
[Speaker Hat](https://shop.m5stack.com/products/m5stickc-speaker-hat) is an
analogue PAM8303 fed from the DAC pin, while SPK2 is I2S. Get SPK2.

### About the Grove lights

The Grove connector exposes 5 V, ground, GPIO 32 and GPIO 33. Connect the
pixel's data-in wire to the configured GPIO (32 by default), power to 5 V, and
ground to ground. One or two NeoPixel/WS2812-compatible RGB pixels are
supported, at either the usual 800 kHz or the 400 kHz rate used by classic v1
pixels. The lights are disabled by default; set `LED_ENABLED="true"` in `.env`
to turn the output on.

See the [Grove-to-NeoPixel schematic](docs/grove-neopixel-schematic.md) for the
connector pin numbers, cable colours, level shifter, protection resistor,
decoupling, and second-pixel wiring.

NeoPixels powered at 5 V can be marginal with the ESP32's 3.3 V data signal.
For a short lead this commonly works, but use a 3.3 V-to-5 V logic-level
shifter if the pixels flicker or ignore updates. Two pixels at full white can
draw about 120 mA, so account for them in the power budget.

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

## Install from your browser

The quickest way to get the firmware onto a Stick is the hosted installer at
[coatsy.github.io/showcase-countdown](https://coatsy.github.io/showcase-countdown/).
Plug the device in over USB, click install, and answer the Wi-Fi prompt. No
toolchain, no clone, no `.env`.

This needs Chrome, Edge, or Opera on a desktop: the page drives the serial port
through the Web Serial API, which Safari and mobile browsers do not implement.

The published image contains no network credentials. It ships with Wi-Fi unset
and is provisioned over the
[Improv serial standard](https://www.improv-wifi.com/serial/), so the browser
sends the credentials down the USB cable after flashing and the device keeps
them in NVS. Send `w` over serial to forget them again.

Build the firmware yourself if you want to change the event date, the palette,
or the lights, since those are compile-time settings. The event title, team name,
speaker type, and time zone can be changed without rebuilding, either from the
installer page or over serial.

### Configuring from the page

The installer page also has a configuration panel for a Stick that is already
running. Connect the device, pick a title, team name, time zone, and speaker,
and apply. The team selector offers the eleven predefined teams. Its selection
is saved on that device across restarts, and the team name appears in its
assigned colour on the device and dashboard. The page reads the current values
when connected, so other settings are preserved when changing the team.

The time zone list matters because the firmware stores a plain offset from UTC
and never calculates daylight saving. The page resolves your chosen zone to the
offset it is on at that moment and sends that number, so Europe/London sends
UTC+01:00 during British Summer Time rather than UTC+00:00. The consequence is
that a device needs the setting re-applied after a daylight-saving transition.

## Configuring a device over serial

The event title, speaker type, and time zone can be changed on a running device
without rebuilding. Connect at 115200 baud, or use the console built into the
[hosted installer](https://coatsy.github.io/showcase-countdown/), and send a
single key:

| Key | Effect |
| --- | --- |
| `?` | List the available commands |
| `i` | Show the current settings |
| `e` | Set the event title(s), pipe-separated, colour markup allowed |
| `p` | Set the speaker type |
| `z` | Choose a time zone from a numbered list |
| `w` | Forget the stored Wi-Fi credentials |
| `x` | Reset settings to the values the firmware was built with |

Commands that need a value print a prompt and read a line, so type the value and
press Enter. Enter on its own leaves the setting alone, and Escape cancels.

Everything set this way lives in NVS and survives a power cycle. The values in
`.env` become the defaults: a device that has never been configured behaves
exactly as its build intended, and `x` returns it to that state. Changing the
speaker restarts the device, because M5Unified selects the audio hardware during
startup.

The `z` list offers standard-time offsets only, since the device has no daylight
saving rules of its own. Use the installer page instead if you want the offset a
zone is actually on today.

### Commands for scripts

Lines beginning with `!` carry their value with them, which is how the installer
page configures a device. Each one answers with `ok:` or `err:`.

| Command | Effect |
| --- | --- |
| `!get` | Report the current team, title, speaker, and offset |
| `!team <name>` | Save a team from the page's list, for example `!team Atlas` |
| `!title <text>` | Set the title(s) |
| `!tz <seconds>` | Set the UTC offset in seconds, from -43200 to 50400 |
| `!speaker <name>` | Set the speaker type |

The event date is not in this list. It is resolved to a fixed epoch at build
time, so moving the event still means a rebuild and reflash.

### Updating devices over Wi-Fi

OTA-capable firmware uses the two-slot `min_spiffs.csv` partition layout. The
uploader preserves the device's NVS data, including Wi-Fi credentials and team
configuration, because it writes only the application image.

Create the private OTA password file once before building or updating devices:

```powershell
.\scripts\setup_ota.ps1
```

Build the application image and test the uploader without contacting a device:

```powershell
pio run -e stick
$env:OTA_PASSWORD = (Get-Content .env.ota | Select-String '^OTA_PASSWORD=').ToString().Split('=', 2)[1].Trim('"')
python scripts\ota_push.py --target 192.168.8.128 --dry-run
```

The dashboard server can invoke the same uploader for an individual device or
all online OTA-capable devices. Set `OTA_RUNNER` in the server environment; the
`{target}` placeholder is replaced with each device IP:

```powershell
$env:OTA_RUNNER = 'python ../scripts/ota_push.py --target {target}'
go run .
```

The dashboard still requires `ORGANISER_SECRET`. The OTA password is read by
`ota_push.py` from `OTA_PASSWORD` or the local `.env.ota` file and is never
placed in the dashboard request or printed by the dry-run command.

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
The template covers every key, and the two with no sensible default are:

| Key | Notes |
| --- | --- |
| `EVENT_NAME` | One or more titles, separated by `\|` |
| `EVENT_DATETIME` | ISO-8601 with an **explicit** UTC offset or trailing `Z` |

Wi-Fi is optional at build time. Set `WIFI_SSID` and `WIFI_PASSWORD` to bake
credentials into a private build, or leave them out and provision the device
over Improv instead. A compiled-in value is only a fallback: anything stored on
the device takes precedence. Note that the ESP32-PICO-D4 has no 5 GHz radio, so
the network must be 2.4 GHz either way.

`EVENT_NAME` carries a few conveniences. Multiple titles separated by `|` are
cycled; any title too wide for the screen scrolls marquee-style and is given at
least two full passes before the next one takes over. Colour markup works too —
`[red]`, `[skyblue]`, `[#00A4EF]`, and `[/]` to return to white:

```dotenv
EVENT_NAME="[red]Westpac[/] + [#00A4EF]Microsoft[/] Hackathon|Doors open 6pm"
```

The panel is RGB565, so colours are quantised to five bits per channel and brand
hex values often read duller than the named equivalents.

The speaker setting is optional and defaults to the onboard buzzer:

| Key       | Default    | Notes                                        |
|-----------|------------|----------------------------------------------|
| `SPEAKER` | `INTERNAL` | `INTERNAL`, `HAT_SPK`, `HAT_SPK2`, or `NONE` |

M5Unified cannot detect a speaker HAT at runtime, so the fitted hardware has to
be named here. Leaving this at `INTERNAL` while a HAT is attached is the usual
cause of a unit that plays the fanfare far too quietly: the audio goes to the
onboard buzzer and the HAT never makes a sound. Set `SPEAKER="HAT_SPK2"` for the
SPK2 HAT, or `SPEAKER="HAT_SPK"` for the older analogue Speaker Hat. This is only
the default; it can also be changed on a running device with the `p` command.
The boot banner reports the configured choice either way.

The Grove light settings are optional and have defaults:

| Key                | Default    | Notes                                        |
|--------------------|------------|----------------------------------------------|
| `LED_ENABLED`      | `false`    | Master switch for the Grove lights           |
| `LED_TYPE`         | `NEOPIXEL` | GRB pixels at 800 kHz; `NONE` also disables  |
| `LED_COUNT`        | `1`        | One or two pixels                            |
| `LED_PIN`          | `32`       | GPIO 32 or GPIO 33                           |
| `LED_BRIGHTNESS`   | `64`       | Per-channel white level from 1 to 255        |
| `LOCAL_UTC_OFFSET` | `+10:00`   | Fixed offset from UTC; no DST calculation    |
| `LED_ON_TIME`      | `08:00`    | Local time to turn the pixels white          |
| `LED_OFF_TIME`     | `18:00`    | Local time to turn the pixels off            |

The lights stay off until you set `LED_ENABLED="true"`, so a unit with nothing
wired to the Grove port behaves sensibly out of the box. The remaining keys are
ignored while the switch is off.

Explicit colour-order variants such as `NEOPIXEL_RGB`, `NEOPIXEL_RBG`, and
`NEOPIXEL_BGR` are available when a pixel does not use the usual GRB order.
`WS2812` and `WS2812B` are aliases for the default GRB configuration. Classic v1
NeoPixels are a different case again: they expect RGB ordering clocked at
400 kHz rather than 800 kHz, so use `NEOPIXEL_V1` for those. A v1 pixel driven
at 800 kHz typically stays dark rather than showing wrong colours. Changing any
of these settings requires a rebuild and reflash.

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

A clean build lands at roughly 970 KB of the 3 MB `huge_app` partition.

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
| **A, single click** | Force an NTP re-sync |
| **A, double click** | Toggle the Grove lights on or off |
| **B** | Toggle the diagnostics screen |

The device also re-syncs automatically every 6 hours and makes one additional
sync attempt when the countdown reaches 5 minutes.

The automatic light schedule is applied at startup and at each on/off boundary.
A double-click override therefore lasts until the next configured boundary:
08:00 or 18:00 with the defaults.

Run the deterministic schedule checks with:

```sh
python tests/test_light_schedule.py
```

## Room messaging

With `MQTT_HOST` or `MQTT_URI` set in `.env`, each unit keeps WiFi up after the time sync and
joins an MQTT broker. Teams then drive their own stick from their coding agents
through an MCP server: messages, jingles and composed tunes, LED patterns,
shouts to the room, and messages to other tables. The four-digit claim code
shown under the countdown is the team's credential. Messaging is off when
both settings are empty (unless ESP-NOW relay is enabled), so a published,
Improv-provisioned image is unaffected. Public connections use authenticated,
certificate-verified MQTT over WebSockets; see [public transport](docs/messaging.md#public-mqtt-over-secure-websockets).

| Doc | What it covers |
| --- | --- |
| [docs/plan.md](docs/plan.md) | The decisions, architecture and schedule |
| [docs/messaging.md](docs/messaging.md) | MQTT topics, payloads and the tool list |
| [docs/teams.md](docs/teams.md) | The one-page handout for tables |
| [docs/flashing-quickstart.md](docs/flashing-quickstart.md) | Chris and Coatsey's USB/OTA flashing checklist |
| [server/](server/) | The Go MCP server, dashboard, fake fleet, and router deploy |

The server, broker, LAN NTP and dashboard run as one Go binary on an
**OpenWrt** router (tested only on a GL-MT3000, `aarch64`, OpenWrt 24.10). Run
it locally against any broker, with a dozen virtual sticks:

```sh
cd server && go build -o showcase-server . && \
  EVENT_DATETIME="2026-11-15T09:00:00+11:00" ORGANISER_SECRET=secret \
  ./showcase-server -embedded-broker :1883 -fake 12
```

Dashboard at `http://localhost:8090/`, MCP at `http://localhost:8090/mcp/<code>`.

## Layout

```text
platformio.ini              Build environments (stick, bridge, sticks3)
scripts/load_env.py         .env -> generated env_config.h (pre-build)
scripts/merge_firmware.py   Single flashable image for the web installer
scripts/read_serial.py      One-shot serial capture, for scripted checks
src/main.cpp                Boot sequence, display, countdown, messaging
src/settings.h              Runtime settings in NVS, provisioned over serial
src/messaging.cpp           MQTT session, commands in, state and events out
src/sequencer.h             Note-string parser for team audio
src/espnow_link.cpp         ESP-NOW receiver for the lock and fire signals
src/bridge/                 The ESP-NOW bridge sketch (env: bridge)
server/                     Go MCP server, dashboard, fake fleet, deploy
web/                        Browser installer page and manifest
.env.template               Committed structure documentation
.env.public                 Credential-free config for the published build
```

Plus a few extras you can find for yourself.

## A note on secrets

Wi-Fi credentials provisioned over Improv live in NVS on the device, so they are
not part of the firmware image and the published build carries none. They are
still not protected: NVS is unencrypted here and can be read back off the flash
by anyone holding the device.

If you instead put `WIFI_SSID` and `WIFI_PASSWORD` in `.env`, they are compiled
into the image in plaintext. That is fine for a private build, but such an image
must never be published. `.env` is git-ignored for this reason, the public build
uses `.env.public`, and CI refuses to publish if credentials appear in the build
configuration.

Either way, prefer a guest or IoT SSID over your primary network credential, and
do not reuse this pattern for anything sensitive.
