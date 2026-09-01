<!-- markdownlint-disable-file -->
# Research: M5StickC PLUS SE countdown timer (PlatformIO)

**Date:** 2026-09-01
**Repo:** `showcase-countdown` (empty — only `.git/`)
**Difficulty:** Medium-hard (greenfield embedded project, cross-cutting toolchain + secrets + hardware decisions)

## 1. Scope

Set up this repo to host a PlatformIO project for the **M5StickC PLUS SE** (SKU `K016-P-SE`) that:

1. Displays an **event name** and a **live countdown** on the built-in LCD.
2. Initialises the **hardware RTC from an NTP server over WiFi** at startup.
3. Takes **WiFi credentials, event name, and target date/time** from a git-ignored `.env`, compiled into the firmware.
4. Ships a committed **`.env.template`** documenting the structure.
5. On reaching zero, runs a **celebration animation** and plays **one voice of a multi-part melody**, so that a room full of these units performs the piece together.

### Confirmed requirements (user, 2026-09-01)

| # | Decision |
|---|---|
| 1 | **Countdown format:** largest two units — e.g. "12 days 4 hrs" |
| 2 | **At zero:** freeze with a message **and** a celebration animation, **plus** the distributed melody (§7) |
| 3 | **NTP re-sync:** at boot, then **every 6 hours** |
| 4 | **Buttons:** force an NTP re-sync; show a status/diagnostics screen. No brightness control |
| 5 | **Events:** exactly one |
| 6 | **Screen content:** event name and countdown only — no battery or sync indicator on the main view (both move to the diagnostics screen) |
| 7 | **Fleet size:** 5–10 units |
| 8 | **Piece:** an original fanfare, composed for this project |
| 9 | **Voices:** 3 — root, fifth, melody. **No third** — the piece is deliberately an open fifth |
| 10 | **Weights:** root 40% / fifth 20% / melody 40% |
| 11 | **Assignment:** MAC-derived, with a `.env` override |
| 12 | **Celebration length:** exactly as long as the melody |

### Success criteria

- `pio run` builds clean from a fresh clone once `.env` is created from the template.
- No secrets in git history, and no secrets in incidentally-committed artefacts.
- Countdown remains correct across reboots without WiFi (RTC-backed).
- Build fails loudly, not silently, when `.env` is missing or incomplete.
- Multiple units reaching zero **sound as one chord**, not as a stagger (§7.4).

## 2. Hardware facts (confirmed)

| Property | Value |
|---|---|
| SoC | ESP32-PICO-D4, Xtensa LX6 dual-core @ 240 MHz |
| Flash | 4 MB |
| SRAM | 520 KB |
| PSRAM | **None** |
| WiFi | 2.4 GHz only |
| LCD | 1.14", **ST7789v2, 135 × 240** |
| RTC | **BM8563** (I2C `0x51`) |
| PMU | **AXP192** (I2C `0x34`); LDO1 = RTC VDD, BACKUP = RTC coin-cell rail |
| IMU | **None** — this is the only functional delta vs. the regular StickC Plus |
| Battery | 3.7 V, 120 mAh |
| Buttons | 2 (A/B) + power button |
| Other | Red LED, IR TX, SPM1423 mic, passive buzzer, Grove HY2.0-4P (G32/G33), Hat-Bus |
| Upload baud | 1200–115200, 250 K, 500 K, 750 K, **1500 K** |

**Hardware gotcha (from the vendor docs):** G36 and G25 share a port. When using one, set the other to floating input:

```cpp
pinMode(36, INPUT);
gpio_pulldown_dis(GPIO_NUM_25);
gpio_pullup_dis(GPIO_NUM_25);
```

Not relevant to the countdown itself, but it will bite if a Hat or Grove sensor is added later.

Sources: [product page](https://shop.m5stack.com/products/m5stickc-plus-se-mini-iot-dev-kit-esp32-pico), [docs K016-P-SE](https://docs.m5stack.com/en/products/sku/K016-P-SE) (= [/en/core/StickC-Plus_SE](https://docs.m5stack.com/en/core/StickC-Plus_SE), same page).

### 2.1 Development hardware — StickC Plus 1.1

The SE units are on order. Development happens on a **StickC Plus 1.1**, which is a near-exact proxy:

| | StickC Plus 1.1 (dev) | StickC Plus SE (target) | Same? |
|---|---|---|---|
| SoC | ESP32-PICO-D4 | ESP32-PICO-D4 | ✅ |
| PlatformIO board | `m5stick-c` | `m5stick-c` | ✅ |
| Display | ST7789v2 135×240 | ST7789v2 135×240 | ✅ |
| RTC | BM8563 | BM8563 | ✅ |
| PMU | AXP192 | AXP192 | ✅ |
| Buzzer | passive, **G2** | passive | ✅ |
| `M5.getBoard()` | `board_M5StickCPlus` | `board_M5StickCPlus` | ✅ |
| IMU | MPU6886 | none | — irrelevant here |

Every feature including the fanfare can be developed and validated before the SE arrives. **Nothing is blocked.**

Two other devices were considered and rejected as dev targets:

- **StickC (non-Plus, K016-C)** — same silicon and RTC, but an 80×160 ST7735S panel and, decisively, **no buzzer**. Its spec sheet and Arduino docs list no Speaker/Buzzer page, unlike the Plus.
- **StickS3** — has a speaker, but it is ESP32-S3 with no published PlatformIO board JSON, so it needs a borrowed generic S3 board definition and the pioarduino fork.

Both remain buildable via `platformio.ini` environments, but the StickC Plus is the one to develop against.

**Design consequence:** the UI must never hardcode panel dimensions. M5Stack's own StickS3 example demonstrates the idiom (`M5.Display.height() / 60`, `rand() % M5.Display.width()`), and it is what lets one source tree serve an 80×160 StickC, a 135×240 Plus/SE, and an S3.

## 3. Toolchain decisions

### 3.0 Vendor reference implementation ⭐

**[`m5stack/M5StickC-Plus-SE-Factory`](https://github.com/m5stack/M5StickC-Plus-SE-Factory)** is M5Stack's own factory firmware for this exact SKU — and it is a **PlatformIO + Arduino** project. This is the single highest-authority source for toolchain decisions and it settles several questions outright.

```ini
; https://github.com/m5stack/M5StickC-Plus-SE-Factory/blob/main/platformio.ini
[env:m5stick-c]
platform = espressif32@6.12.0
board = m5stick-c
framework = arduino
lib_ldf_mode = deep+
build_flags =
	-DCORE_DEBUG_LEVEL=0
board_build.partitions = partition.csv
board_upload.flash_size = 4MB
lib_deps=
	z3t0/IRremote@^4.5.0
```

```csv
# https://github.com/m5stack/M5StickC-Plus-SE-Factory/blob/main/partition.csv
# Name,   Type, SubType, Offset,  Size,     Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x300000,
```

What this establishes:

| Question | Vendor answer | Effect on this research |
|---|---|---|
| Board ID | `m5stick-c` | ✅ Confirms §3.1 |
| Framework | `arduino` | ✅ Confirms |
| Platform | `espressif32@6.12.0` (**official**, Arduino 2.0.17) | ⚠️ **Overturns** the pioarduino recommendation — see §3.3 |
| App partition | **3 MB single slot**, no OTA | ✅ Confirms §3.5 |
| LDF mode | `deep+` | ➕ **New** — adopt it |
| Flash size | Explicit `4MB` override | ➕ **New** — adopt it |

Note `lib_deps` lists only IRremote because M5Unified/M5GFX are vendored under `lib/`. For this project, resolve them from the registry instead.

### 3.1 PlatformIO board → `m5stick-c`

Verified: **neither** `platformio/platform-espressif32` nor `pioarduino/platform-espressif32` has an `m5stick-c-plus`, `m5stick-c-plus2`, or SE-specific board JSON. `boards/m5stick-c-plus.json` returns 404 in both. Only `m5stick-c.json` exists:

```json
{ "build": { "core": "esp32", "extra_flags": "-DARDUINO_M5Stick_C",
             "f_cpu": "240000000L", "f_flash": "40000000L",
             "flash_mode": "dio", "mcu": "esp32", "variant": "m5stick_c" },
  "upload": { "flash_size": "4MB", "maximum_ram_size": 327680,
              "maximum_size": 4194304, "speed": 1500000 } }
```

That is an exact match for the SE's silicon (ESP32-PICO-D4 / 4 MB / no PSRAM). The `-DARDUINO_M5Stick_C` macro is **not** a problem: M5Unified and M5GFX identify the board by **runtime I2C/panel probing**, not by that macro.

This aligns with M5Stack's own [Arduino quick start for the StickC-Plus SE](https://docs.m5stack.com/en/arduino/m5stickc-plus_se/program), which instructs selecting the **`M5StickCPlus`** board and installing **M5Unified + M5GFX**.

### 3.2 Runtime board identity → `board_M5StickCPlus`

The canonical board enum ([`M5GFX/src/lgfx/boards.hpp`](https://github.com/m5stack/M5GFX/blob/master/src/lgfx/boards.hpp)) has **no SE entry**:

```cpp
board_M5StickC = 3, board_M5StickCPlus = 4, board_M5StickCPlus2 = 5, ...
```

So `M5.getBoard()` on an SE returns `board_M5StickCPlus`. Practical consequence: everything works, `M5.Imu.isEnabled()` returns `false`. Do not branch on IMU presence.

### 3.3 ESP32 platform package → **`espressif32@6.12.0`** (revised)

This is the one genuinely load-bearing decision, and my initial reading of it was wrong.

| | `platformio/espressif32` | `pioarduino/platform-espressif32` |
|---|---|---|
| Latest | **7.1.0** (actively released) | **55.03.311** |
| ESP-IDF framework | v6.1.0 (current) | v5.5.5 |
| **Arduino framework** | **v2.0.17 (IDF 4.4.7)** | **v3.3.11** |
| Arduino core status | Frozen since 6.8.0 (Jul 2024); upstream EOL | Tracks upstream |
| **Validated on this board by M5Stack** | ✅ **Yes — `@6.12.0`** | ❌ No evidence |

The purely-technical argument favours pioarduino: the official platform is alive and shipping, but **only its ESP-IDF side moves**. Every release from 6.8.0 through 7.1.0 lists `Arduino - v2.0.17 (based on IDF v4.4.7)`, so `framework = arduino` pins you to an upstream-EOL core.

But the vendor ships **production firmware for this exact SKU** on `espressif32@6.12.0` (§3.0). For a device whose entire driver stack (M5Unified, M5GFX, AXP192, BM8563) comes from that same vendor, matching their validated combination is worth more than a newer core number. Arduino 2.0.17 on classic ESP32 is mature and extremely well-trodden; nothing this project needs requires Arduino 3.x.

**Recommendation: match the vendor.**

```ini
platform = espressif32@6.12.0
```

**Alternative, if a modern core is wanted later** (pin the exact release URL, not a branch):

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
```

The `__has_include` SNTP guard in §5.2 is what makes switching between these two a non-event — keep it regardless of which you choose. Record which one you land on.

### 3.4 Libraries

```ini
lib_deps =
    m5stack/M5Unified @ ^0.2.21
    m5stack/M5GFX @ ^0.2.28
```

M5Unified declares M5GFX as a dependency (`idf_component.yml`: "Bump version to 0.2.21 and require M5GFX 0.2.28"), so M5GFX resolves transitively. Listing it explicitly pins it and documents intent — worth doing.

### 3.5 Partition table → 3 MB single app slot (vendor-confirmed)

4 MB flash, default scheme gives **1.25 MB** per OTA app slot. M5Unified + M5GFX + WiFi + SNTP lands close to that ceiling, and hitting it manifests as a confusing link-stage failure.

M5Stack's factory firmware independently reaches the same conclusion — a custom `partition.csv` with a single **3 MB** `app0` and no second OTA slot (§3.0).

Two equivalent options:

- `board_build.partitions = huge_app.csv` — built-in, app0 = 0x300000, remainder given to SPIFFS.
- Copy M5Stack's `partition.csv` verbatim — identical app size, leaves the tail unallocated.

Use `huge_app.csv`. Same 3 MB app slot, one less file to maintain, and this project stores nothing in a filesystem.

Also set `board_upload.flash_size = 4MB` explicitly, as the vendor does.

### 3.6 `lib_ldf_mode = deep+`

Adopted from the vendor config. PlatformIO's default LDF mode (`chain`) only follows `#include` directives it can see without evaluating the preprocessor. M5Unified and M5GFX lean heavily on conditional includes (`__has_include`, target-specific `#if` blocks — visible in the SNTP guard in §5.2 and throughout M5GFX's panel selection). `deep+` evaluates those conditionals, which prevents intermittent "library not found" and missing-symbol link errors.

## 4. Secrets pipeline

### 4.1 Decision: generated header, not raw build flags

Two viable mechanisms for `.env` → C++. Recommendation is the **generated header**.

| | Build flags (`CPPDEFINES`) | **Generated header** |
|---|---|---|
| Escaping | Fragile — see 4.2 | Real C string literals; no escaping class of bugs |
| Rebuild scope | Changing any secret rebuilds the **entire Arduino core** | Only TUs that `#include` the header |
| Inspectable | Only via `pio run -v` | `cat` the generated file |
| Leak surface | Appears in `compile_commands.json`, `idedata.json` | Lives under `.pio/` (already git-ignored, uncommittable by construction) |

PlatformIO's own docs list "generate dynamic headers (`*.h`)" as a first-class `extra_scripts` use case, so this is idiomatic rather than a workaround.

### 4.2 Why the build-flag route is fragile (recorded so we don't regress into it)

`env.Append(CPPDEFINES=[("WIFI_SSID", "My Network")])` emits `-DWIFI_SSID=My Network`; one quoting layer is consumed between SCons and the compiler, and GCC reports `'My' was not declared in this scope`. The value must arrive pre-escaped as `\"My Network\"`. PlatformIO ships a helper for this:

```python
def StringifyMacro(env, value):        # PlatformIO Core >= 6.1.0 — note: SINGULAR
    return '\\"%s\\"' % value.replace('"', '\\\\\\"')
```

Additional traps that all disappear with the header approach:

- **`$` is not escaped by `StringifyMacro`.** CPPDEFINES values pass through SCons `env.subst()`, so a `$` in a WiFi password becomes a variable reference. Must be manually doubled to `$$`.
- **Numeric coercion.** Values routed through `BUILD_FLAGS` get shlex-tokenised and digit-only values coerced to `int`. An all-numeric password silently becomes `#define WIFI_PASSWORD 12345678`.
- **`CPPDEFINES` tuples bypass `ParseFlagsExtended` entirely** — different escaping rules from the `.ini` form, which is why forum answers contradict each other.

### 4.3 Compute the target epoch at build time

The strongest simplification available. Rather than shipping a date string to the device and doing `strptime` + timezone math in firmware, have the pre-script parse the ISO-8601 value and emit a **Unix epoch integer**:

```ini
EVENT_DATETIME="2026-11-15T09:00:00+11:00"
```
becomes
```cpp
#define EVENT_EPOCH_UTC 1794355200LL
```

Device math reduces to `difftime(EVENT_EPOCH_UTC, time(nullptr))`. No `strptime`, no TZ database on the device, no DST edge cases.

Require an **explicit UTC offset** in the `.env` value and parse with `datetime.fromisoformat()`. This avoids `zoneinfo`/`tzdata` (which is not installed by default on Windows) and avoids adding any Python dependency to PlatformIO's venv. Handle a trailing `Z` by rewriting it to `+00:00` before parsing (bare `Z` only works on Python 3.11+).

**Reject offset-naive values with a hard error** rather than assuming local time — an ambiguous countdown target is worse than a failed build.

### 4.4 Not chosen

- **`${sysenv.VAR}` in `platformio.ini`** — good for CI secret stores, but escaping is shell-dependent and onboarding a machine means remembering shell exports. Worth layering on later for CI; not the local-dev path.
- **Git-ignored `include/secrets.h` + committed template** — simplest, but the real file sits in a *committable* directory, one `git add -A` from disaster, and template/actual drift silently.

## 5. RTC + NTP design

### 5.1 Confirmed M5Unified RTC API

From the [RTC8563 class docs](https://docs.m5stack.com/en/arduino/m5unified/rtc8563_class):

```cpp
bool           M5.Rtc.isEnabled();
bool           M5.Rtc.getVoltLow();
rtc_datetime_t M5.Rtc.getDateTime() const;
void           M5.Rtc.setDateTime(const rtc_datetime_t&);
void           M5.Rtc.setDateTime(const tm* const);        // takes std C tm
void           M5.Rtc.setSystemTimeFromRtc(struct timezone* tz = nullptr);
int            M5.Rtc.setAlarmIRQ(int afterSeconds);       // 1–15300
```

```cpp
struct __attribute__((packed)) rtc_time_t { int8_t hours, minutes, seconds; };
struct __attribute__((packed)) rtc_date_t {
  int16_t year;    // 1900–2099
  int8_t  month;   // 1–12
  int8_t  date;    // 1–31
  int8_t  weekDay; // 0=Sun … 6=Sat
};
struct __attribute__((packed)) rtc_datetime_t { rtc_date_t date; rtc_time_t time; };
```

`setSystemTimeFromRtc()` is the key helper — it seeds the ESP32 system clock from the BM8563, so `time(nullptr)` is valid before (and without) any network.

### 5.2 NTP → RTC — revised after hardware testing

The M5Unified example ([`Rtc.ino`](https://github.com/m5stack/M5Unified/blob/master/examples/Basic/Rtc/Rtc.ino)) polls `sntp_get_sync_status()` behind an `__has_include` guard for `<esp_sntp.h>` / `<sntp.h>`. **That approach did not work on this hardware** and has been replaced.

**What failed.** On the StickC Plus with `espressif32@6.12.0` (Arduino 2.0.17 / IDF 4.4.7), WiFi associated and DHCP succeeded, but `sntp_get_sync_status()` never reached `SNTP_SYNC_STATUS_COMPLETED` inside a 15 s window — so the code reported `ntp : FAILED` even though nothing was actually wrong with the network.

**What works.** Poll the **wall clock** instead. An unset ESP32 clock starts at epoch 0; any successful sync lands far past it:

```cpp
constexpr time_t SANE_EPOCH = 1750000000;  // 2025-06-15
while (millis() < deadline) {
    if (time(nullptr) > SANE_EPOCH) { ok = true; break; }
    M5.delay(100);
}
```

This is ground truth rather than a status API whose semantics vary by core version and sync mode, and it removes the need for the SNTP headers altogether — so the `__has_include` guard is gone. `configTzTime()` comes from Arduino's `esp32-hal-time.c` and needs no special include.

**Two other changes made at the same time**, both worth keeping regardless:

1. **Call `configTzTime()` only after `WL_CONNECTED`.** Started before the link is up, the first SNTP request fails DNS and lwip then backs off for `CONFIG_LWIP_SNTP_UPDATE_DELAY` — **one hour** by default. No reasonable wait would ever see a sync.
2. **Pre-resolve the NTP host** with `WiFi.hostByName()` before starting SNTP. This doubles as a diagnostic (DNS failure is otherwise indistinguishable from a blocked UDP 123) and warms the resolver cache.

Since changes 1 and 2 and the wall-clock check went in together, the fix is not fully isolated — but the previous build already had change 1 and still failed, which points at the status API as the culprit.

**Confirmed working end to end:**

```text
  wifi    : connected
  ip      : 192.168.2.222
  dns     : 192.168.2.1
  0.pool.ntp.org : 110.232.114.22
  ntp     : synced
  utc     : 2026-09-01 02:56:31
  remain  : 1386209 s
```

The second-boundary alignment before writing the RTC is retained from the vendor example — that part is sound:

```cpp
time_t t = time(nullptr) + 1;
while (t > time(nullptr));   // spin to the second boundary
M5.Rtc.setDateTime(gmtime(&t));
```

**Store UTC in the RTC.** The M5Unified example says so explicitly, and it keeps the countdown arithmetic offset-free.

### 5.3 Boot sequence

```text
M5.begin(cfg)
  └─ cfg.internal_rtc / cfg.external_rtc left at defaults
setRotation(1) → 240×135 landscape; setBrightness()
if (M5.Rtc.isEnabled())
      M5.Rtc.setSystemTimeFromRtc()      ← countdown is live immediately, offline
render first frame (possibly flagged "time unverified")
WiFi.begin(...) with a bounded timeout (~10 s)
  ├─ success → wait for SNTP → align to second → M5.Rtc.setDateTime(gmtime(&t))
  └─ timeout → carry on with RTC time; surface a small staleness indicator
WiFi.disconnect(true); WiFi.mode(WIFI_OFF)   ← frees ~40–50 KB heap, saves battery
loop: difftime(EVENT_EPOCH_UTC, time(nullptr)) → render
```

Rendering the countdown *before* the network attempt matters: on a 120 mAh battery, a blank screen during a 10-second WiFi timeout reads as a crash.

### 5.4 Flagged ambiguity — `getVoltLow()`

M5Stack's docs state `true: No power failure / false: Power failure occurred`. That is the **inverse** of the BM8563 `VL` (voltage-low) register bit convention, where the flag being set means the oscillator stopped and time data is unreliable. Suspected documentation error.

**Do not gate logic on this until verified on hardware.** Test: set the RTC, fully power down, remove power for a while, power up, print `getVoltLow()`. Until then, treat NTP as authoritative whenever reachable.

## 6. Display plan (135 × 240)

The vendor's own SE speaker example ([docs](https://docs.m5stack.com/en/arduino/m5stickc-plus_se/speaker)) independently confirms the rotation, datum and font conventions proposed below:

```cpp
M5.Lcd.setRotation(1);
M5.Lcd.setTextDatum(middle_center);
M5.Lcd.setTextFont(&fonts::FreeMonoBold9pt7b);
M5.Lcd.drawString("Speaker", M5.Lcd.width() / 2, M5.Lcd.height() / 2);
```

- **Rotation.** Native panel orientation is portrait 135 W × 240 H. `setRotation(1)` (or `3`) gives **240 × 135 landscape** — far better for a wide countdown readout plus an event-name line. Vendor-confirmed.
- **Flicker-free redraw.** Draw into an off-screen sprite and blit once per second:
  ```cpp
  M5Canvas canvas(&M5.Display);
  canvas.createSprite(240, 135);
  // ... draw ...
  canvas.pushSprite(0, 0);
  ```
  Full-screen 16-bit sprite = 240 × 135 × 2 = **64,800 bytes (~63 KB)**. No PSRAM, so this comes from internal heap. Comfortable *after* WiFi is shut down (§5.3); if heap is tight, sprite only the numeric region and draw the static event name directly.
- **Fonts, given the "largest two units" format (requirement 1).** The format needs letters, so `fonts::Font7` cannot carry the whole readout — it has digits, colon, period and minus only. Best of both: render the **numbers** in `Font7` and the **unit words** in a normal face beside them.
  ```text
  ┌─────────────────────────┐
  │      Launch Day        │  <- event name, FreeSansBold12pt7b
  │   12 DAYS   4 HRS      │  <- digits Font7, labels small
  └─────────────────────────┘
  ```
  The `fonts::` namespace and FreeFont naming (`FreeMonoBold9pt7b`) are vendor-confirmed; the specific faces above still want checking against the installed M5GFX.
- **Unit pairs.** days+hrs → hrs+mins → mins+secs as the event approaches. Singular/plural handling needed ("1 DAY" not "1 DAYS").
- **Text layout.** `setTextDatum(middle_center)` + `drawString(str, x, y)` — vendor-confirmed, far easier to centre than cursor-based printing.
- **Brightness.** `M5.Display.setBrightness(0–255)` still worth setting once at boot for battery life, even though the user declined a brightness button.
- **Long event names.** 240 px is narrow. Plan for truncation or a marquee.

## 7. Celebration and the distributed melody

Requirement 2 is the most distinctive part of this project: **many units, each playing one voice at random, root weighted heaviest, all firing together.**

### 7.1 Hardware capability — confirmed

The SE has an onboard passive buzzer driven through `M5.Speaker`. Vendor example for this exact SKU:

```cpp
M5.Speaker.tone(7000, 100);   // frequency Hz, duration ms
M5.Speaker.tone(4000, 200);
```

Relevant `Speaker_Class` API ([docs](https://docs.m5stack.com/en/arduino/m5unified/speaker_class)):

```cpp
bool     tone(float frequency, uint32_t duration = UINT32_MAX,
              int channel = -1, bool stop_current_sound = true);
void     setVolume(uint8_t master_volume);   // 0-255
bool     isPlaying(void);
void     stop(void);
bool     isEnabled(void);
```

**Constraint that shapes the design:** the buzzer is a single-GPIO device (`speaker_config_t::buzzer = true`), so it is **strictly monophonic**. One unit cannot play a chord. That is precisely why the piece must be split across units — the requirement and the hardware agree.

### 7.2 Voice model

Define the piece as **3 parallel voice parts**, each an array of `{frequency_hz, duration_ms}`. Every unit plays exactly one part start-to-finish. Polyphony is an emergent property of the room, not of any device.

```cpp
struct Note { float hz; uint16_t ms; };
struct Voice { const char* name; const Note* notes; size_t len; uint8_t weight; };
```

### 7.3 Weighted part selection — confirmed

**Three voices. No third.** The third was dropped and its weight folded into the melody.

| Part | Role | Weight | Expected count at 7 units |
|---|---|---|---|
| 0 | **Root / bass** | **40%** | ~2.8 |
| 1 | Fifth | **20%** | ~1.4 |
| 2 | Melody | **40%** | ~2.8 |

#### Why removing the third is the better design

The earlier 4-voice weighting put the third at 10%, which meant it was absent roughly half the time — the ensemble's harmonic quality would have varied between performances, and between rehearsal and the real event. Removing it entirely makes the sonority **deterministic**: every performance is a root, a fifth and a melody, every time.

It is also the right sound. An open fifth with a bold top line is the classic brass-fanfare texture, and it suits a monophonic square-wave piezo far better than a close-voiced triad, where the third tends to beat unpleasantly against the fifth at these frequencies.

Secondary benefits: one fewer note table, simpler weighted draw, and better coverage per voice across a small fleet.

#### Coverage — the fifth is now the voice to watch

Probability a voice is drawn by **no unit at all**, `(1 - w)^n`:

| Fleet size | No root (40%) | **No fifth (20%)** | No melody (40%) |
|---|---|---|---|
| 5 | 7.8% | **32.8%** | 7.8% |
| 7 | 2.8% | **21.0%** | 2.8% |
| 10 | 0.6% | **10.7%** | 0.6% |

Root and melody are effectively guaranteed at this fleet size. The **fifth is the fragile one** — at 5 units it is missing about a third of the time, and without it the ensemble collapses to root-plus-melody, which is a much thinner, octave-ish sound.

This is exactly what the `FORCE_VOICE` override exists for: check the computed distribution before the event and pin one unit to the fifth if the draw left it empty.

#### Selection mechanism

Selection happens **once at boot** and is held stable, never re-rolled at fire time.

- **MAC-derived** (chosen) — hash the station MAC into a weighted bucket. Stable per physical device, so a given unit always plays the same part and rehearsal matches the real event.
- `.env` override (e.g. `FORCE_VOICE=1`) — forces a specific part for a hand-assigned build.

Because MAC assignment is deterministic, the distribution across a specific set of units can be **computed ahead of time**. Print each unit's assigned voice at boot so a missing fifth is spotted and patched before the event rather than discovered during it.

### 7.4 Synchronisation — why NTP was the right call

There is no radio link between units at fire time. **The shared NTP-disciplined clock *is* the synchronisation mechanism.** This elevates the RTC/NTP requirement from "nice accuracy" to *load-bearing for the headline feature*.

Budget: notes need to land within roughly **±50 ms** to read as a chord rather than a flam.

| Source | Typical error |
|---|---|
| NTP over WiFi (LAN) | ±10–50 ms |
| BM8563 drift over 6 h | small, and re-synced every 6 h (requirement 3) |
| **1 Hz render loop — worst case** | **up to 1000 ms** ❌ |

The loop period, not the clock, is the dominant error term. Mitigation: **arm a precise fire time**. In the final seconds before zero, leave the 1 Hz render cadence and spin on `gettimeofday()` at sub-millisecond granularity until the target instant, then call `tone()` immediately. Do all setup (voice chosen, note table resident, `M5.Speaker` already begun) *before* the spin so the trigger path is as short as possible.

Also: **start the celebration animation after the first note is queued**, not before. Rendering is slow and must not delay audio.

### 7.5 Choosing the piece

An original fanfare of roughly **3–5 s**, composed for this project. Constraints that shape it:

- **Open-fifth harmony throughout.** With no third, the piece has no major/minor quality to work with — write it as root, fifth and melody. Harmonic movement has to come from the melody line and from moving the root/fifth pair, not from chord colour.
- **The melody carries the piece.** At 40% it is the most-played voice and the only one with real contour. Root and fifth are closer to a drone or a rhythmic pedal.
- **Piezo register.** These buzzers are weak below ~500 Hz. A true bass line may be inaudible; put the "root" voice in a usable register (roughly 200–400 Hz at minimum, possibly higher) and let it function as a tenor. The vendor example uses 4 kHz and 7 kHz — loudest, but shrill. Expect to tune by ear on hardware.
- **Guard the fifth's interval.** Since the fifth may be absent (§7.3), the root and melody together must still imply the harmony.
- **Equal total duration.** All three voices must run exactly the same length so they end together and so the celebration (requirement 12) has one well-defined end.
- **Sparse, rhythmic writing** suits a monophonic square-wave source far better than sustained legato.

### 7.6 Celebration timing

Celebration runs **exactly as long as the melody** (requirement 12), so the total duration is a property of the note tables, not a separate constant. Derive it by summing any one voice's durations at compile time — all voices being equal length makes this unambiguous, and it means retiming the piece can't desynchronise the visuals.

## 8. Proposed repo layout

```text
showcase-countdown/
├─ .env.template            ← committed
├─ .env                     ← git-ignored, never committed
├─ .gitignore
├─ platformio.ini
├─ scripts/
│  └─ load_env.py           ← pre-script: .env → .pio/.../generated/env_config.h
├─ include/
├─ src/
│  └─ main.cpp
├─ lib/
└─ README.md
```

Generate the skeleton with `pio project init --ide vscode --board m5stick-c`, then layer the above on top. PlatformIO will not overwrite an existing `.gitignore`.

### 8.1 `platformio.ini`

Vendor config (§3.0) as the base, plus what this project adds.

```ini
[env:m5stick-c-plus-se]
; Matches m5stack/M5StickC-Plus-SE-Factory exactly.
platform = espressif32@6.12.0
board = m5stick-c
framework = arduino

lib_ldf_mode = deep+
board_build.partitions = huge_app.csv
board_upload.flash_size = 4MB

lib_deps =
    m5stack/M5Unified @ ^0.2.21
    m5stack/M5GFX @ ^0.2.28

extra_scripts = pre:scripts/load_env.py

monitor_speed = 115200
upload_speed = 1500000

build_flags = -DCORE_DEBUG_LEVEL=1
```

Deltas from the vendor file, and why:

| Line | Vendor | Here | Reason |
|---|---|---|---|
| `board_build.partitions` | `partition.csv` | `huge_app.csv` | Same 3 MB app slot, one less file (§3.5) |
| `lib_deps` | vendored under `lib/` | registry | Normal dependency resolution |
| `extra_scripts` | — | `pre:scripts/load_env.py` | Secrets pipeline (§4) |
| `CORE_DEBUG_LEVEL` | `0` | `1` | Surface WiFi/SNTP errors during bring-up; drop to `0` for release |
| `monitor_speed` | — | `115200` | Serial console |

Notes:

- The `pre:` prefix is **required**. A missing prefix silently defaults to `post:`, where `env` no longer reaches project sources as expected. `pre:` also makes the generated header's include path visible to libraries, not just `src/`.
- `upload_speed = 1500000` is the board JSON default and on M5's supported list. Drop to `115200` if flashing proves flaky on a given cable/port.

### 8.2 `.env.template` (committed)

```dotenv
# Copy to .env and fill in. .env is git-ignored — never commit it.
# Quote every value. Unquoted all-numeric values are emitted as C numbers, not strings.

WIFI_SSID="Your Network Name"
WIFI_PASSWORD="your-wifi-password"

EVENT_NAME="Launch Day"

# ISO-8601 with an EXPLICIT UTC offset. Offset-naive values are rejected at build time.
EVENT_DATETIME="2026-11-15T09:00:00+11:00"

# POSIX TZ string, only used to render local wall-clock time on screen.
# Sydney: AEST-10AEDT,M10.1.0,M4.1.0/3   UTC: UTC0   London: GMT0BST,M3.5.0/1,M10.5.0
LOCAL_TZ="AEST-10AEDT,M10.1.0,M4.1.0/3"

NTP_SERVER_1="0.pool.ntp.org"
NTP_SERVER_2="1.pool.ntp.org"
NTP_SERVER_3="2.pool.ntp.org"
```

### 8.3 `.gitignore`

PlatformIO's canonical vscode template plus secrets and leak-surface entries:

```gitignore
# PlatformIO
.pio
.vscode/.browse.c_cpp.db*
.vscode/c_cpp_properties.json
.vscode/launch.json
.vscode/ipch

# Written to the PROJECT ROOT and contains every -D flag in plaintext
compile_commands.json

# Secrets
.env
.env.*
!.env.template
```

`.env` alone would not match `.env.template` (gitignore matches whole path components) — the negation is needed only because of the `.env.*` glob. Keep it; it makes intent explicit.

### 8.4 `scripts/load_env.py` — shape

Responsibilities, in order:

1. Parse `$PROJECT_DIR/.env` (`KEY=VALUE`, `#` comments, blank lines, optional `export `, quoted values).
2. Validate required keys are present; `sys.exit("...")` with a clear message if not — **fail the build, do not warn**.
3. Parse `EVENT_DATETIME` via `datetime.fromisoformat` (rewriting a trailing `Z` to `+00:00`); reject offset-naive values; emit `EVENT_EPOCH_UTC` as a `long long` literal.
4. Render `env_config.h` into `$BUILD_DIR/generated/`, escaping each string as a real C literal (`\` → `\\`, `"` → `\"`, newlines → `\n`).
5. **Write only if changed** — unconditional rewriting triggers a full rebuild every single build.
6. `env.Append(CPPPATH=[GENERATED_DIR])`.

Then in `src/main.cpp`:

```cpp
#include "env_config.h"
#ifndef WIFI_SSID
#error "WIFI_SSID missing — copy .env.template to .env"
#endif
```

Do **not** early-return on `env.IsIntegrationDump()`; PlatformIO re-runs extra scripts to gather IDE metadata, and IntelliSense needs the generated path.

Extra scripts cannot be run standalone (`Import()` only exists inside SCons) — debug with `pio run -v`.

## 9. Security notes

- Compiled-in credentials are **not a security boundary**. They sit in plaintext in the firmware image and can be read back off flash. Acceptable for a hobby countdown device; do not reuse this pattern for anything sensitive. Real secrecy needs NVS + flash encryption + secure boot.
- Prefer a **guest/IoT SSID** over the primary network credential.
- Verbose builds and CI logs print full compiler command lines. With the generated-header approach the secrets are not on the command line, which is a real advantage.
- `.gitignore` has no effect on already-tracked files. If `.env` is ever committed: `git rm --cached .env`, rewrite history, and rotate every value.

## 10. Open questions / verify on hardware

**Resolved by the first successful build (`pio run -e stick`, espressif32@6.12.0):**

- [x] **Toolchain works.** Builds clean. `RAM 15.0% (49,048 B) · Flash 30.4% (954,897 B of 3 MB)`.
- [x] **Partition sizing.** 933 KB would also have fit the default scheme's 1.25 MB app slot (~71% full). `huge_app.csv` was **prudent, not necessary** — still the right call with no OTA requirement, but the risk was smaller than assumed.
- [x] **Heap headroom.** 49 KB static leaves ~278 KB; minus WiFi's ~40–50 KB, the 63 KB full-screen sprite is comfortable.

**Two compile errors worth remembering (both now fixed):**

- `fonts::Font0` / `Font2` / `FreeSansBold12pt7b` are **distinct types** (`GLCDfont`, `BMPfont`, `GFXfont`). A ternary selecting between them needs an explicit `static_cast<const lgfx::IFont*>`.
- Wrapping `#include <WiFi.h>` in `#if __has_include(...)` **breaks PlatformIO's LDF**, which scans for `#include` directives to decide what to link. The guard was copied from M5Unified's example, which also targets ESP-IDF; this project is Arduino-only and does not need it. The SNTP guard is still required — that one is about header naming across core versions.

**Resolved on real hardware** (StickC Plus 1.1, COM3, flashed 2026-09-01):

```text
showcase-countdown
  board   : 4          <- board_M5StickCPlus, exactly as §3.2 predicted
  panel   : 240x135    <- setRotation(1) landscape, same as the SE
  speaker : present    <- M5.Speaker.isEnabled() on espressif32@6.12.0
  event   : Launch Day <- secrets pipeline end to end
  target  : 1794693600 <- epoch resolved at build time
  mac     : E8:9F:6D:09:CC:04
  roll    : 7
  voice   : 0 (root)
```

- [x] **The top risk is closed.** `M5.Speaker.isEnabled()` returns true on the vendor-pinned `espressif32@6.12.0` / Arduino 2.0.17. The SE speaker docs' "board manager >= 3.3.8" note marks when SE support landed in M5Stack's *Arduino IDE board package*; it does not imply Arduino core 3.x. **No need to move to pioarduino.** Audibility of `tone()` still needs the button-A audition.
- [x] **`board_M5StickCPlus` confirmed** — reports as enum value 4, as predicted from `M5GFX/src/lgfx/boards.hpp`. The SE will report the same.
- [x] **Voice assignment validated three ways.** The helper predicted `roll 7 -> root` for MAC `E8:9F:6D:09:CC:04`; the device independently reported `roll : 7`, `voice : 0 (root)`. Python helper, compiled C++ header and real silicon all agree, plus 2000/2000 synthetic MACs.

**A bug the hardware caught immediately:**

- **`M5.begin()` does not call `Serial.begin()`.** The first flash produced bootloader output and then total silence — the app was running and driving the display, but every `Serial.printf` went nowhere. Since the boot banner is the only way to read a unit's assigned voice, this would have made fleet coverage unverifiable. Fixed with an explicit `Serial.begin(115200)` at the top of `setup()`. Do not assume a framework initialises the UART.

**Still to verify on hardware:**

- [x] **NTP path with real credentials** — working; see §5.2 for the fix and the caveat about `sntp_get_sync_status()`.
- [ ] **Audibility.** Hold button A at boot to audition all three voices. `isEnabled()` being true does not prove the piezo makes a usable sound.
- [ ] `getVoltLow()` polarity (§5.4).
- [ ] **Measured fire-time spread across two or more units** (§7.4) — the acceptance test for the ensemble idea.
- [ ] Buzzer frequency response — whether C5 is audible at all (§7.5).
- [ ] Battery runtime with 6-hourly wake-ups.
- [ ] Legibility of the Font7 + label layout.

**Toolchain note:** PlatformIO's virtualenv was found mid-upgrade (core package removed, dependencies present). Recovery was `python -m pip install -U platformio` **plus** `python -m pip install esptool==4.9.0` — the second is needed because `tool-esptoolpy` depends on `intelhex`, `reedsolo` and others that reinstalling platformio core does not restore.

## 11. User decisions — all resolved

All twelve scoping and melody decisions are captured in §1. Nothing is blocking implementation.

Items deliberately deferred to hardware bring-up rather than decided up front:

- Exact note frequencies and rhythm of the fanfare — needs the piezo's real response (§7.5).
- Master volume level.
- Visual design of the celebration animation.

## 11. Sources

**Vendor reference implementation (highest authority):**

- [`m5stack/M5StickC-Plus-SE-Factory`](https://github.com/m5stack/M5StickC-Plus-SE-Factory) — official factory firmware, PlatformIO + Arduino
- [`platformio.ini`](https://github.com/m5stack/M5StickC-Plus-SE-Factory/blob/main/platformio.ini)
- [`partition.csv`](https://github.com/m5stack/M5StickC-Plus-SE-Factory/blob/main/partition.csv)

**Device documentation:**

- [Product page — M5StickC PLUS SE](https://shop.m5stack.com/products/m5stickc-plus-se-mini-iot-dev-kit-esp32-pico)
- [Docs — StickC-Plus SE](https://docs.m5stack.com/en/core/StickC-Plus_SE) (alias of [`/en/products/sku/K016-P-SE`](https://docs.m5stack.com/en/products/sku/K016-P-SE))
- [Docs — StickC-Plus SE Arduino quick start](https://docs.m5stack.com/en/arduino/m5stickc-plus_se/program)

**Library APIs:**

- [Docs — M5Unified RTC8563 class](https://docs.m5stack.com/en/arduino/m5unified/rtc8563_class)
- [Docs — M5Unified Speaker class](https://docs.m5stack.com/en/arduino/m5unified/speaker_class)
- [Docs — StickC-Plus SE Speaker example](https://docs.m5stack.com/en/arduino/m5stickc-plus_se/speaker)
- [Docs — M5Unified PlatformIO](https://docs.m5stack.com/en/arduino/m5unified/intro_vscode)
- [M5Unified README](https://github.com/m5stack/M5Unified)
- [M5Unified RTC example](https://github.com/m5stack/M5Unified/blob/master/examples/Basic/Rtc/Rtc.ino)
- [M5GFX board enum](https://github.com/m5stack/M5GFX/blob/master/src/lgfx/boards.hpp)

**Platform packages:**

- [platform-espressif32 releases (official)](https://github.com/platformio/platform-espressif32/releases)
- [platform-espressif32 releases (pioarduino)](https://github.com/pioarduino/platform-espressif32/releases)
- `boards/m5stick-c.json` in both platform repos

**Secrets pipeline:**

- Subagent research: `.copilot-tracking/research/subagents/2026-09-01/platformio-env-secrets-build-flags.md`

## 13. Next steps

1. ~~Scaffold the project~~ — done; builds clean.
2. **Flash the StickC Plus and hold button A at boot** to audition all three voices. This closes the §10 speaker risk in one action.
3. Verify the countdown layout and the NTP→RTC path on hardware.
4. Read the assigned voice off the serial console; check fleet coverage, especially the fifth (§7.3).
5. Flash two units and measure fire-time spread (§7.4).
6. Tune the fanfare register by ear and adjust `TRANSPOSE` / note tables.
