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

Import("env")  # noqa: F821 - injected by SCons

ENV_PATH = os.path.join(env.subst("$PROJECT_DIR"), ".env")  # noqa: F821
GENERATED_DIR = os.path.join(env.subst("$BUILD_DIR"), "generated")  # noqa: F821
HEADER_PATH = os.path.join(GENERATED_DIR, "env_config.h")

REQUIRED = ("WIFI_SSID", "WIFI_PASSWORD", "EVENT_NAME", "EVENT_DATETIME")

DEFAULTS = {
    "NTP_SERVER_1": '"0.pool.ntp.org"',
    "NTP_SERVER_2": '"1.pool.ntp.org"',
    "NTP_SERVER_3": '"2.pool.ntp.org"',
    "BRIGHTNESS": "80",
    "LED_TYPE": '"NEOPIXEL"',
    "LED_COUNT": "1",
    "LED_PIN": "32",
    "LOCAL_UTC_OFFSET": '"+10:00"',
    "LED_ON_TIME": '"08:00"',
    "LED_OFF_TIME": '"18:00"',
    "TITLE_DWELL_MS": "5000",
    "TITLE_GAP_MS": "300",
}

_LINE = re.compile(r"^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
_NUMERIC = re.compile(r"^[+-]?(?:\d+|\d*\.\d+)$")

# Colour markup such as [red] / [#FF0000] / [/]. Stripped for the plain-text
# EVENT_NAME, kept intact in EVENT_TITLES where it is rendered.
_MARKUP = re.compile(r"\[(?:/|#[0-9A-Fa-f]{6}|[A-Za-z]+)\]")
_UTC_OFFSET = re.compile(r"^(?:UTC)?([+-])(\d{1,2})(?::([0-5]\d))?$", re.IGNORECASE)
_CLOCK_TIME = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")

_LED_TYPES = {
    "NONE": (0, "NEO_GRB"),
    "NEOPIXEL": (1, "NEO_GRB"),
    "NEOPIXEL_RGB": (1, "NEO_RGB"),
    "NEOPIXEL_RBG": (1, "NEO_RBG"),
    "NEOPIXEL_GRB": (1, "NEO_GRB"),
    "NEOPIXEL_GBR": (1, "NEO_GBR"),
    "NEOPIXEL_BRG": (1, "NEO_BRG"),
    "NEOPIXEL_BGR": (1, "NEO_BGR"),
    "WS2812": (1, "NEO_GRB"),
    "WS2812B": (1, "NEO_GRB"),
}

_DERIVED_SETTINGS = {
    "LED_TYPE",
    "LED_COUNT",
    "LED_PIN",
    "LOCAL_UTC_OFFSET",
    "LED_ON_TIME",
    "LED_OFF_TIME",
}


def strip_markup(text):
    return _MARKUP.sub("", text).strip()


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
    led_type = setting_value(entries, "LED_TYPE").upper()
    if led_type not in _LED_TYPES:
        fail("LED_TYPE=%r must be one of: %s" % (led_type, ", ".join(sorted(_LED_TYPES))))
    led_enabled, pixel_order = _LED_TYPES[led_type]
    led_count = integer_setting(entries, "LED_COUNT", (1, 2))
    led_pin = integer_setting(entries, "LED_PIN", (32, 33))
    led_on_minute = minute_of_day(entries, "LED_ON_TIME")
    led_off_minute = minute_of_day(entries, "LED_OFF_TIME")
    if led_on_minute == led_off_minute:
        fail("LED_ON_TIME and LED_OFF_TIME must differ")

    lines = [
        "// AUTO-GENERATED from .env by scripts/load_env.py. Do not edit, do not commit.",
        "#pragma once",
        "",
        "// %s -> %d" % (parsed.isoformat(), epoch),
        "#define EVENT_EPOCH_UTC %dLL" % epoch,
        "",
    ]

    # EVENT_NAME is pipe-separated so one key can carry a rotating set of titles.
    titles = [part.strip() for part in entries["EVENT_NAME"][0].split("|")]
    titles = [title for title in titles if title]
    if not titles:
        sys.exit("load_env: EVENT_NAME is empty")
    lines.append("#define EVENT_NAME %s" % c_string(strip_markup(titles[0])))
    lines.append("#define EVENT_TITLE_COUNT %d" % len(titles))
    lines.append("#define EVENT_TITLES {%s}" % ", ".join(c_string(t) for t in titles))
    lines.append("")
    lines.append("#define LED_ENABLED %d" % led_enabled)
    lines.append("#define LED_TYPE_NAME %s" % c_string(led_type))
    lines.append("#define LED_COUNT %d" % led_count)
    lines.append("#define LED_PIN %d" % led_pin)
    lines.append("#define LED_PIXEL_TYPE (%s + NEO_KHZ800)" % pixel_order)
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
