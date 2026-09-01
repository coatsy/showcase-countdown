#!/usr/bin/env python3
"""Predict which fanfare voice each unit in a fleet will play.

Voice assignment is a deterministic hash of the device's eFuse MAC, so the whole
ensemble can be checked before the event rather than discovered during it.

The fifth is the fragile voice at 20%: across five units there is roughly a 33%
chance no device draws it, and without it the ensemble thins to root-plus-melody.

Weights are parsed out of src/fanfare.h rather than duplicated here, so this
script cannot drift from the firmware. The hash is cross-validated against
src/voice_assign.h by tests/test_voice_assign.py.

Usage:
    python scripts/fleet_voices.py AA:BB:CC:DD:EE:FF 11:22:33:44:55:66
    python scripts/fleet_voices.py --file macs.txt
"""

import argparse
import re
import sys
from pathlib import Path

MASK64 = (1 << 64) - 1

FANFARE_H = Path(__file__).resolve().parent.parent / "src" / "fanfare.h"

_VOICE_ENTRY = re.compile(
    r'\{\s*"(?P<name>\w+)"\s*,\s*\w+\s*,\s*sizeof\([^)]*\)\s*/\s*sizeof\(Note\)\s*,\s*(?P<weight>\d+)\s*\}'
)


def load_voices(header=FANFARE_H):
    """Read (name, weight) pairs from the firmware's VOICES table."""
    if not header.is_file():
        sys.exit("fleet_voices: cannot find %s" % header)
    voices = [
        (m.group("name"), int(m.group("weight")))
        for m in _VOICE_ENTRY.finditer(header.read_text(encoding="utf-8"))
    ]
    if not voices:
        sys.exit(
            "fleet_voices: found no VOICES entries in %s.\n"
            "The header format changed - update _VOICE_ENTRY rather than guessing weights."
            % header
        )
    total = sum(weight for _, weight in voices)
    if total != 100:
        sys.exit("fleet_voices: weights in %s total %d, expected 100" % (header, total))
    return voices


def mix_bits(value):
    """MurmurHash3 fmix64, matching voice::mixBits in src/voice_assign.h."""
    value &= MASK64
    value ^= value >> 33
    value = (value * 0xFF51AFD7ED558CCD) & MASK64
    value ^= value >> 33
    value = (value * 0xC4CEB9FE1A85EC53) & MASK64
    value ^= value >> 33
    return value & 0xFFFFFFFF


def efuse_from_mac(text):
    """ESP.getEfuseMac() reads the 6 MAC octets into the low bytes of a
    little-endian uint64, so octet 0 ends up least significant."""
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", text)
    if len(cleaned) != 12:
        raise ValueError("expected 6 hex octets, got %r" % text)
    return int.from_bytes(bytes.fromhex(cleaned), "little")


def voice_for(efuse, voices):
    roll = mix_bits(efuse) % 100
    cumulative = 0
    for index, (name, weight) in enumerate(voices):
        cumulative += weight
        if roll < cumulative:
            return roll, index, name
    return roll, 0, voices[0][0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("macs", nargs="*", help="MAC addresses, any separator")
    parser.add_argument("--file", type=Path, help="file with one MAC per line")
    args = parser.parse_args()

    entries = list(args.macs)
    if args.file:
        entries += [
            line.strip()
            for line in args.file.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        ]
    if not entries:
        parser.error("no MAC addresses given")

    voices = load_voices()
    counts = {name: 0 for name, _ in voices}
    rows = []

    for entry in entries:
        try:
            efuse = efuse_from_mac(entry)
        except ValueError as exc:
            sys.exit("fleet_voices: %s" % exc)
        roll, index, name = voice_for(efuse, voices)
        counts[name] += 1
        rows.append((entry, roll, index, name))

    print("%-20s %6s  %s" % ("MAC", "roll", "voice"))
    for entry, roll, index, name in rows:
        print("%-20s %6d  %d %s" % (entry, roll, index, name))

    print("\n%d unit(s):" % len(rows))
    for name, weight in voices:
        print("  %-8s %2d  (weight %d%%)" % (name, counts[name], weight))

    missing = [name for name, _ in voices if counts[name] == 0]
    if not missing:
        print("\nAll voices covered.")
        return 0

    print("\nMISSING: %s" % ", ".join(missing))
    suggest_reassignments(rows, counts, voices, missing)
    return 1


def suggest_reassignments(rows, counts, voices, missing):
    """Propose FORCE_VOICE overrides, without robbing a voice to fill another."""
    index_of = {name: i for i, (name, _) in enumerate(voices)}
    remaining = dict(counts)
    reassigned = set()

    for name in missing:
        donors = [
            (n, c) for n, c in remaining.items() if n not in missing and c >= 2
        ]
        if not donors:
            print(
                "  %s: no unit can be spared without leaving another voice empty.\n"
                "     A %d-unit fleet cannot cover %d voices reliably - add more devices."
                % (name, len(rows), len(voices))
            )
            continue

        donor = max(donors, key=lambda item: item[1])[0]
        candidate = next(
            (row for row in rows if row[3] == donor and row[0] not in reassigned), None
        )
        if candidate is None:
            continue

        reassigned.add(candidate[0])
        remaining[donor] -= 1
        remaining[name] += 1
        print(
            "  Set FORCE_VOICE=%d in the .env for %s (currently %s, which keeps %d unit%s)"
            % (
                index_of[name],
                candidate[0],
                donor,
                remaining[donor],
                "" if remaining[donor] == 1 else "s",
            )
        )


if __name__ == "__main__":
    sys.exit(main())
