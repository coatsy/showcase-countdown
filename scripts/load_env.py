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
    "TITLE_DWELL_MS": "5000",
    "TITLE_GAP_MS": "300",
}

_LINE = re.compile(r"^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
_NUMERIC = re.compile(r"^[+-]?(?:\d+|\d*\.\d+)$")

# Colour markup such as [red] / [#FF0000] / [/]. Stripped for the plain-text
# EVENT_NAME, kept intact in EVENT_TITLES where it is rendered.
_MARKUP = re.compile(r"\[(?:/|#[0-9A-Fa-f]{6}|[A-Za-z]+)\]")


def strip_markup(text):
    return _MARKUP.sub("", text).strip()


def fail(message):
    sys.stderr.write("\nload_env: %s\n\n" % message)
    env.Exit(1)  # noqa: F821


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

    for key in sorted(entries):
        if key in ("EVENT_DATETIME", "EVENT_NAME"):
            continue
        value, quoted = entries[key]
        lines.append("#define %s %s" % (key, literal(value, quoted)))
    for key, value in sorted(DEFAULTS.items()):
        if key not in entries:
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
