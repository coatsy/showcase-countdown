#!/usr/bin/env python3
"""Prove scripts/fleet_voices.py agrees with the firmware, MAC for MAC.

The helper exists to tell you whether your fleet covers all three voices. If its
hash diverged from src/voice_assign.h it would report a fleet as covered when it
is not, which is worse than having no helper. So rather than trusting that two
implementations of fmix64 match, this compiles the real firmware header and
diffs the two over a spread of MACs.

    python tests/test_voice_assign.py
"""

import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import fleet_voices  # noqa: E402

SAMPLE_COUNT = 2000


def build_probe(workdir):
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        return None
    binary = workdir / ("voice_probe.exe" if sys.platform == "win32" else "voice_probe")
    subprocess.run(
        [compiler, "-std=c++11", "-O1", "-Wall", str(ROOT / "tests" / "voice_probe.cpp"), "-o", str(binary)],
        check=True,
    )
    return binary


def sample_macs(count):
    rng = random.Random(20260901)
    macs = [
        # Consecutive MACs: the case a weak hash would clump into one voice.
        "4C:75:25:00:00:00",
        "4C:75:25:00:00:01",
        "4C:75:25:00:00:02",
        "4C:75:25:00:00:03",
        "00:00:00:00:00:00",
        "FF:FF:FF:FF:FF:FF",
    ]
    macs += [
        ":".join("%02X" % rng.randrange(256) for _ in range(6))
        for _ in range(count - len(macs))
    ]
    return macs


def main():
    voices = fleet_voices.load_voices()
    print("voices from src/fanfare.h: %s" % ", ".join("%s=%d%%" % v for v in voices))

    macs = sample_macs(SAMPLE_COUNT)

    with tempfile.TemporaryDirectory() as tmp:
        probe = build_probe(Path(tmp))
        if probe is None:
            print("no C++ compiler found - cannot cross-validate against firmware")
            return 1

        # Chunked to stay under the Windows command-line length limit.
        expected = []
        for start in range(0, len(macs), 200):
            chunk = macs[start : start + 200]
            result = subprocess.run([str(probe)] + chunk, check=True, capture_output=True, text=True)
            expected.extend(result.stdout.strip().splitlines())

    mismatches = []
    for line in expected:
        mac, roll_text, index_text, name = line.split()
        roll, index, py_name = fleet_voices.voice_for(fleet_voices.efuse_from_mac(mac), voices)
        if (roll, index, py_name) != (int(roll_text), int(index_text), name):
            mismatches.append((mac, (int(roll_text), int(index_text), name), (roll, index, py_name)))

    if mismatches:
        print("\nMISMATCH in %d of %d cases:" % (len(mismatches), len(macs)))
        for mac, firmware, python in mismatches[:10]:
            print("  %s firmware=%s python=%s" % (mac, firmware, python))
        return 1

    print("%d/%d MACs agree between firmware and helper." % (len(expected), len(macs)))

    # A weak hash would show up here as a distribution far from the weights.
    counts = {name: 0 for name, _ in voices}
    for line in expected:
        counts[line.split()[3]] += 1
    print("\ndistribution over %d random MACs:" % len(expected))
    for name, weight in voices:
        share = 100.0 * counts[name] / len(expected)
        print("  %-8s %5.1f%%  (weight %d%%)" % (name, share, weight))
        if abs(share - weight) > 4.0:
            print("    WARNING: more than 4 points off the configured weight")
    return 0


if __name__ == "__main__":
    sys.exit(main())
