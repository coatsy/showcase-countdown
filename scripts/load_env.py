"""Generate a C header of compile-time config from a git-ignored .env file.

Emits `env_config.h` into the build directory rather than injecting `-D` build
flags. That avoids the SCons quoting minefield entirely (a `$` in a password
gets substituted away; an all-digit password gets coerced to an int), keeps the
secrets off the compiler command line and out of compile_commands.json, and
limits rebuilds to translation units that actually include the header.

EVENT_DATETIME is resolved to a Unix epoch here, at build time, so the firmware
never has to parse a date or carry a timezone database.
"""

import os
import re
import sys
from datetime import datetime
from urllib.parse import urlsplit

Import("env")  # noqa: F821 - injected by SCons

ENV_PATH = os.environ.get("SHOWCASE_ENV_FILE") or os.path.join(env.subst("$PROJECT_DIR"), ".env")  # noqa: F821
GENERATED_DIR = os.path.join(env.subst("$BUILD_DIR"), "generated")  # noqa: F821
HEADER_PATH = os.path.join(GENERATED_DIR, "env_config.h")

REQUIRED = ("EVENT_NAME", "EVENT_DATETIME")

DEFAULTS = {
    # Empty means "provision over Improv at runtime". Publishable images must
    # leave these unset so no network credentials end up in the binary.
    "WIFI_SSID": '""',
    "WIFI_PASSWORD": '""',
    "FIRMWARE_NAME": '"showcase-countdown"',
    "FIRMWARE_VERSION": '"dev"',
    "NTP_SERVER_1": '"0.pool.ntp.org"',
    "NTP_SERVER_2": '"1.pool.ntp.org"',
    "NTP_SERVER_3": '"2.pool.ntp.org"',
    "BRIGHTNESS": "80",
    "SPEAKER": '"INTERNAL"',
    "LED_ENABLED": '"false"',
    "LED_TYPE": '"NEOPIXEL"',
    "LED_COUNT": "1",
    "LED_PIN": "32",
    "LED_BRIGHTNESS": "64",
    "LOCAL_UTC_OFFSET": '"+10:00"',
    "LED_ON_TIME": '"08:00"',
    "LED_OFF_TIME": '"18:00"',
    "TITLE_DWELL_MS": "5000",
    "TITLE_GAP_MS": "300",
    # Room messaging (see docs/messaging.md). Off unless a broker is named, so
    # a published image keeps the original behaviour: the radio powers off
    # after the NTP sync and no MQTT client is started.
    "MQTT_HOST": '""',
    "MQTT_PORT": "1883",
    "MQTT_URI": '""',
    "MQTT_USERNAME": '""',
    "MQTT_PASSWORD": '""',
    # Mixed into the on-screen claim code so it cannot be derived from the
    # device id printed in the boot banner.
    "CLAIM_SALT": '"showcase"',
    # 2.4 GHz channel the event AP is pinned to; ESP-NOW frames from the bridge
    # are sent on it and unassociated sticks park their radio there.
    "ESPNOW_CHANNEL": "6",
    "ESPNOW_RELAY_ENABLED": "0",
    "ESPNOW_RELAY_KEY": '""',
}

_LINE = re.compile(r"^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
_NUMERIC = re.compile(r"^[+-]?(?:\d+|\d*\.\d+)$")

_UTC_OFFSET = re.compile(r"^(?:UTC)?([+-])(\d{1,2})(?::([0-5]\d))?$", re.IGNORECASE)
_CLOCK_TIME = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")

_LED_TYPES = {
    "NONE": (0, "NEO_GRB", "NEO_KHZ800"),
    "NEOPIXEL": (1, "NEO_GRB", "NEO_KHZ800"),
    "NEOPIXEL_V1": (1, "NEO_RGB", "NEO_KHZ400"),
    "NEOPIXEL_RGB": (1, "NEO_RGB", "NEO_KHZ800"),
    "NEOPIXEL_RBG": (1, "NEO_RBG", "NEO_KHZ800"),
    "NEOPIXEL_GRB": (1, "NEO_GRB", "NEO_KHZ800"),
    "NEOPIXEL_GBR": (1, "NEO_GBR", "NEO_KHZ800"),
    "NEOPIXEL_BRG": (1, "NEO_BRG", "NEO_KHZ800"),
    "NEOPIXEL_BGR": (1, "NEO_BGR", "NEO_KHZ800"),
    "WS2812": (1, "NEO_GRB", "NEO_KHZ800"),
    "WS2812B": (1, "NEO_GRB", "NEO_KHZ800"),
}

# M5Unified cannot probe for a speaker hat, so the fitted hardware has to be
# named here: (internal_spk, hat_spk, hat_spk2).
_SPEAKERS = {
    "NONE": (0, 0, 0),
    "INTERNAL": (1, 0, 0),
    "HAT_SPK": (0, 1, 0),
    "HAT_SPK2": (0, 0, 1),
}

_BOOLEANS = {
    "1": 1,
    "true": 1,
    "yes": 1,
    "on": 1,
    "0": 0,
    "false": 0,
    "no": 0,
    "off": 0,
}

_DERIVED_SETTINGS = {
    "MQTT_URI",
    "MQTT_USERNAME",
    "MQTT_PASSWORD",
    "ESPNOW_RELAY_ENABLED",
    "ESPNOW_RELAY_KEY",
    "SPEAKER",
    "LED_ENABLED",
    "LED_TYPE",
    "LED_COUNT",
    "LED_PIN",
    "LED_BRIGHTNESS",
    "LOCAL_UTC_OFFSET",
    "LED_ON_TIME",
    "LED_OFF_TIME",
}


def fail(message):
    sys.stderr.write("\nload_env: %s\n\n" % message)
    env.Exit(1)  # noqa: F821


def setting_value(entries, key):
    if key in entries:
        return entries[key][0]
    raw = DEFAULTS[key]
    if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in "\"'":
        return raw[1:-1]
    return raw


def integer_setting(entries, key, allowed):
    value = setting_value(entries, key)
    try:
        parsed = int(value, 10)
    except ValueError:
        fail("%s=%r must be an integer" % (key, value))
    if parsed not in allowed:
        fail("%s=%r must be one of: %s" % (key, value, ", ".join(map(str, allowed))))
    return parsed


def integer_range_setting(entries, key, minimum, maximum):
    value = setting_value(entries, key)
    try:
        parsed = int(value, 10)
    except ValueError:
        fail("%s=%r must be an integer" % (key, value))
    if parsed < minimum or parsed > maximum:
        fail("%s=%r must be between %d and %d" % (key, value, minimum, maximum))
    return parsed


def boolean_setting(entries, key):
    value = setting_value(entries, key)
    parsed = _BOOLEANS.get(value.strip().lower())
    if parsed is None:
        fail("%s=%r must be true or false" % (key, value))
    return parsed


def utc_offset_seconds(entries):
    value = setting_value(entries, "LOCAL_UTC_OFFSET")
    match = _UTC_OFFSET.fullmatch(value)
    if not match:
        fail("LOCAL_UTC_OFFSET=%r must look like +10:00, -05:00, or UTC+10" % value)
    sign, hours_text, minutes_text = match.groups()
    hours = int(hours_text)
    minutes = int(minutes_text or "0")
    if hours > 14 or (hours == 14 and minutes):
        fail("LOCAL_UTC_OFFSET=%r is outside the supported UTC-14:00 to UTC+14:00 range" % value)
    seconds = (hours * 60 + minutes) * 60
    return -seconds if sign == "-" else seconds


def minute_of_day(entries, key):
    value = setting_value(entries, key)
    match = _CLOCK_TIME.fullmatch(value)
    if not match:
        fail("%s=%r must use 24-hour HH:MM format" % (key, value))
    return int(match.group(1)) * 60 + int(match.group(2))


def parse_env_file(path):
    """Return {key: (value, was_quoted)}. Quoting decides string vs numeric."""
    entries = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            match = _LINE.match(line)
            if not match:
                continue
            key, raw = match.group(1), match.group(2).strip()
            quoted = len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in "\"'"
            if quoted:
                quote, raw = raw[0], raw[1:-1]
                if quote == '"':
                    raw = raw.replace('\\"', '"').replace("\\\\", "\\")
            else:
                raw = raw.split(" #", 1)[0].rstrip()
            entries[key] = (raw, quoted)
    return entries


def to_epoch(value):
    """Parse ISO-8601 with an explicit offset into a Unix epoch."""
    text = value.strip()
    if text.endswith("Z") or text.endswith("z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        fail(
            "EVENT_DATETIME=%r is not valid ISO-8601.\n"
            "         Expected something like 2026-11-15T09:00:00+11:00" % value
        )
    if parsed.utcoffset() is None:
        fail(
            "EVENT_DATETIME=%r has no UTC offset.\n"
            "         An ambiguous countdown target is worse than a failed build - add\n"
            "         an explicit offset (+11:00) or a trailing Z." % value
        )
    return int(parsed.timestamp()), parsed


def c_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    escaped = escaped.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return '"%s"' % escaped


def literal(value, quoted):
    if not quoted and _NUMERIC.match(value):
        return value
    if not quoted and value.lower() in ("true", "false"):
        return "1" if value.lower() == "true" else "0"
    return c_string(value)


def render(entries, epoch, parsed):
    mqtt_uri = setting_value(entries, "MQTT_URI")
    if not mqtt_uri and setting_value(entries, "MQTT_HOST"):
        port = integer_range_setting(entries, "MQTT_PORT", 1, 65535)
        mqtt_uri = "mqtt://%s:%d" % (setting_value(entries, "MQTT_HOST"), port)
    username = setting_value(entries, "MQTT_USERNAME")
    password = setting_value(entries, "MQTT_PASSWORD")
    try:
        broker = urlsplit(mqtt_uri)
        port = broker.port
    except ValueError:
        fail("MQTT_URI must contain a valid broker hostname and port")
    if mqtt_uri and (
        broker.scheme not in ("mqtt", "mqtts", "wss") or not broker.hostname
        or broker.username is not None or broker.password is not None
        or broker.query or broker.fragment or port == 0
        or any(char.isspace() or ord(char) < 32 for char in mqtt_uri)
    ):
        fail("MQTT_URI must be mqtt://, mqtts:// or wss:// without userinfo, query or fragment")
    tls = broker.scheme in ("mqtts", "wss")
    if username or password:
        if not tls or not username or not password:
            fail("MQTT credentials require TLS and both MQTT_USERNAME and MQTT_PASSWORD")
    if broker.scheme == "wss" and (not username or len(password) < 32):
        fail("Public WSS requires MQTT_USERNAME and a private password of at least 32 characters")
    relay_enabled = boolean_setting(entries, "ESPNOW_RELAY_ENABLED")
    relay_key = setting_value(entries, "ESPNOW_RELAY_KEY")
    if relay_enabled and not re.fullmatch(r"[0-9a-fA-F]{64}", relay_key):
        fail("ESPNOW_RELAY_ENABLED requires a private 64-hex-character ESPNOW_RELAY_KEY")
    integer_range_setting(entries, "ESPNOW_CHANNEL", 1, 14)
    speaker = setting_value(entries, "SPEAKER").upper()
    if speaker not in _SPEAKERS:
        fail("SPEAKER=%r must be one of: %s" % (speaker, ", ".join(sorted(_SPEAKERS))))
    internal_spk, hat_spk, hat_spk2 = _SPEAKERS[speaker]
    led_type = setting_value(entries, "LED_TYPE").upper()
    if led_type not in _LED_TYPES:
        fail("LED_TYPE=%r must be one of: %s" % (led_type, ", ".join(sorted(_LED_TYPES))))
    type_enabled, pixel_order, pixel_speed = _LED_TYPES[led_type]
    led_enabled = 1 if type_enabled and boolean_setting(entries, "LED_ENABLED") else 0
    # Up to a small ring or jewel. Watch the Grove 5 V budget above ~8 pixels.
    led_count = integer_range_setting(entries, "LED_COUNT", 1, 16)
    led_pin = integer_setting(entries, "LED_PIN", (32, 33))
    led_brightness = integer_range_setting(entries, "LED_BRIGHTNESS", 1, 255)
    led_on_minute = minute_of_day(entries, "LED_ON_TIME")
    led_off_minute = minute_of_day(entries, "LED_OFF_TIME")
    if led_on_minute == led_off_minute:
        fail("LED_ON_TIME and LED_OFF_TIME must differ")

    lines = [
        "// AUTO-GENERATED from .env by scripts/load_env.py. Do not edit, do not commit.",
        "#pragma once",
        "#define ESPNOW_RELAY_ENABLED %d" % relay_enabled,
        "#define ESPNOW_RELAY_KEY %s" % c_string(relay_key if relay_enabled else ""),
        "#define MQTT_URI %s" % c_string(mqtt_uri),
        "#define MQTT_USERNAME %s" % c_string(username),
        "#define MQTT_PASSWORD %s" % c_string(password),
        "#define MQTT_TLS %d" % tls,
        "",
        "// %s -> %d" % (parsed.isoformat(), epoch),
        "#define EVENT_EPOCH_UTC %dLL" % epoch,
        "",
    ]

    # EVENT_NAME is pipe-separated so one key can carry a rotating set of titles.
    # The firmware splits and strips markup itself, because the titles can also
    # be replaced at runtime over serial.
    titles = [part.strip() for part in entries["EVENT_NAME"][0].split("|")]
    titles = [title for title in titles if title]
    if not titles:
        sys.exit("load_env: EVENT_NAME is empty")
    lines.append("#define EVENT_TITLES_RAW %s" % c_string("|".join(titles)))
    lines.append("")
    lines.append("#define SPEAKER_NAME %s" % c_string(speaker))
    lines.append("#define SPEAKER_INTERNAL %d" % internal_spk)
    lines.append("#define SPEAKER_HAT_SPK %d" % hat_spk)
    lines.append("#define SPEAKER_HAT_SPK2 %d" % hat_spk2)
    lines.append("")
    lines.append("#define LED_ENABLED %d" % led_enabled)
    lines.append("#define LED_TYPE_NAME %s" % c_string(led_type))
    lines.append("#define LED_COUNT %d" % led_count)
    lines.append("#define LED_PIN %d" % led_pin)
    lines.append("#define LED_BRIGHTNESS %d" % led_brightness)
    lines.append("#define LED_PIXEL_TYPE (%s + %s)" % (pixel_order, pixel_speed))
    lines.append("#define LOCAL_UTC_OFFSET_SECONDS %d" % utc_offset_seconds(entries))
    lines.append("#define LED_ON_MINUTE_OF_DAY %d" % led_on_minute)
    lines.append("#define LED_OFF_MINUTE_OF_DAY %d" % led_off_minute)
    lines.append("")

    for key in sorted(entries):
        if key in ("EVENT_DATETIME", "EVENT_NAME") or key in _DERIVED_SETTINGS:
            continue
        value, quoted = entries[key]
        lines.append("#define %s %s" % (key, literal(value, quoted)))
    for key, value in sorted(DEFAULTS.items()):
        if key not in entries and key not in _DERIVED_SETTINGS:
            lines.append("#define %s %s" % (key, value))
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path, content):
    """Rewriting unconditionally would force a full rebuild on every build."""
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as handle:
            if handle.read() == content:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)
    return True


if not os.path.isfile(ENV_PATH):
    fail("no .env found at %s\n         Copy .env.template to .env and fill it in." % ENV_PATH)

entries = parse_env_file(ENV_PATH)

missing = [key for key in REQUIRED if key not in entries or not entries[key][0]]
if missing:
    fail("missing required key(s) in .env: %s" % ", ".join(missing))

epoch, parsed = to_epoch(entries["EVENT_DATETIME"][0])
changed = write_if_changed(HEADER_PATH, render(entries, epoch, parsed))

env.Append(CPPPATH=[GENERATED_DIR])  # noqa: F821

print(
    "load_env: %s env_config.h  event=%s  epoch=%d"
    % ("wrote" if changed else "unchanged", entries["EVENT_NAME"][0], epoch)
)
