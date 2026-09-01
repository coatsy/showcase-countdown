#!/usr/bin/env python3
"""Render the fanfare to MIDI and WAV so it can be heard without hardware.

Note data is parsed out of src/fanfare.h rather than duplicated here, so what
you hear is what the units will play. TRANSPOSE is applied, so tuning changes
made for real hardware show up in the preview too.

Two formats, because they answer different questions:

    fanfare.mid   All three voices as brass. What the piece is meant to be.
    fanfare.wav   Square waves, no filtering. What a room of piezos sounds like.

Per-voice files are written as well, so a single unit can be previewed alone.

Usage:
    python scripts/render_fanfare.py
    python scripts/render_fanfare.py --out tmp --wave sine
"""

import argparse
import math
import re
import struct
import sys
import wave
from array import array
from pathlib import Path

FANFARE_H = Path(__file__).resolve().parent.parent / "src" / "fanfare.h"

_COMMENT = re.compile(r"//[^\n]*")
_PITCH = re.compile(r"constexpr\s+float\s+(\w+)\s*=\s*([0-9.]+)f\s*;")
_ARRAY = re.compile(r"constexpr\s+Note\s+(VOICE_\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", re.DOTALL)
_ENTRY = re.compile(r"\{\s*(\w+)\s*,\s*(\d+)\s*\}")
_VOICE_ROW = re.compile(r'\{\s*"(\w+)"\s*,\s*(VOICE_\w+)\s*,')
_TRANSPOSE = re.compile(r"constexpr\s+float\s+TRANSPOSE\s*\[\s*\]\s*=\s*\{([^}]*)\}")

# General MIDI "Brass Section", zero-based.
MIDI_PROGRAM = 61
MIDI_VELOCITY = 100
TICKS_PER_QUARTER = 1000
MICROSECONDS_PER_QUARTER = 1000000  # makes one tick exactly one millisecond

FADE_MS = 2.0  # kills the click at note edges without softening the attack


def parse_header(path=FANFARE_H):
    """Return ([(name, [(hz, ms), ...]), ...], [transpose, ...]) from fanfare.h."""
    if not path.is_file():
        sys.exit("render_fanfare: cannot find %s" % path)
    source = path.read_text(encoding="utf-8")

    pitches = {name: float(value) for name, value in _PITCH.findall(source)}
    arrays = {name: body for name, body in _ARRAY.findall(source)}
    order = _VOICE_ROW.findall(source)

    if not order:
        sys.exit(
            "render_fanfare: found no VOICES entries in %s.\n"
            "The header format changed - update the regexes rather than guessing." % path
        )

    voices = []
    for voice_name, array_name in order:
        if array_name not in arrays:
            sys.exit("render_fanfare: VOICES references %s but no such array exists" % array_name)
        notes = []
        for pitch_name, ms in _ENTRY.findall(_COMMENT.sub("", arrays[array_name])):
            if pitch_name not in pitches:
                sys.exit("render_fanfare: unknown pitch constant %r in %s" % (pitch_name, array_name))
            notes.append((pitches[pitch_name], int(ms)))
        if not notes:
            sys.exit("render_fanfare: %s parsed as empty" % array_name)
        voices.append((voice_name, notes))

    match = _TRANSPOSE.search(source)
    transpose = [float(v.strip().rstrip("f")) for v in match.group(1).split(",") if v.strip()]
    if len(transpose) < len(voices):
        sys.exit("render_fanfare: TRANSPOSE has fewer entries than VOICES")

    return voices, transpose


def midi_note(hz):
    return int(round(69.0 + 12.0 * math.log(hz / 440.0, 2.0)))


def vlq(value):
    """MIDI variable-length quantity."""
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def midi_track(events):
    body = b"".join(events) + vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(body)) + body


def write_midi(path, voices, transpose):
    tempo = struct.pack(">I", MICROSECONDS_PER_QUARTER)[1:]
    conductor = midi_track([vlq(0) + b"\xff\x51\x03" + tempo])

    tracks = [conductor]
    for channel, (name, notes) in enumerate(voices):
        events = [
            vlq(0) + b"\xff\x03" + vlq(len(name)) + name.encode("ascii"),
            vlq(0) + bytes([0xC0 | channel, MIDI_PROGRAM]),
        ]
        pending_rest = 0
        for hz, ms in notes:
            if hz <= 0.0:
                pending_rest += ms
                continue
            key = midi_note(hz * transpose[channel])
            events.append(vlq(pending_rest) + bytes([0x90 | channel, key, MIDI_VELOCITY]))
            events.append(vlq(ms) + bytes([0x80 | channel, key, 0]))
            pending_rest = 0
        if pending_rest:
            # Trailing silence still has to be honoured or the track ends early.
            events.append(vlq(pending_rest) + b"\xff\x01\x00")
        tracks.append(midi_track(events))

    header = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), TICKS_PER_QUARTER)
    path.write_bytes(header + b"".join(tracks))


def render_voice(notes, multiplier, rate, waveform):
    """Synthesise one voice to a float sample list."""
    samples = []
    for hz, ms in notes:
        count = int(round(rate * ms / 1000.0))
        if hz <= 0.0:
            samples.extend([0.0] * count)
            continue

        freq = hz * multiplier
        fade = min(int(rate * FADE_MS / 1000.0), count // 2)
        for i in range(count):
            phase = (freq * i / rate) % 1.0
            value = (1.0 if phase < 0.5 else -1.0) if waveform == "square" else math.sin(2.0 * math.pi * phase)
            if fade:
                if i < fade:
                    value *= i / fade
                elif i >= count - fade:
                    value *= (count - i) / fade
            samples.append(value)
    return samples


def write_wav(path, tracks, rate, gain):
    length = max(len(t) for t in tracks)
    pcm = array("h", bytes(2 * length))
    for i in range(length):
        total = sum(t[i] for t in tracks if i < len(t))
        pcm[i] = max(-32768, min(32767, int(total * gain * 32767)))

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(pcm.tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default="tmp", help="output directory (default: tmp)")
    parser.add_argument("--rate", type=int, default=44100, help="WAV sample rate (default: 44100)")
    parser.add_argument(
        "--wave",
        choices=("square", "sine"),
        default="square",
        help="WAV timbre; square matches the piezo (default: square)",
    )
    args = parser.parse_args()

    voices, transpose = parse_header()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    midi_path = out_dir / "fanfare.mid"
    write_midi(midi_path, voices, transpose)

    rendered = [render_voice(notes, transpose[i], args.rate, args.wave) for i, (_, notes) in enumerate(voices)]
    mix_path = out_dir / ("fanfare-%s.wav" % args.wave)
    write_wav(mix_path, rendered, args.rate, gain=1.0 / len(rendered))

    part_paths = []
    for (name, _), track in zip(voices, rendered):
        part_path = out_dir / ("fanfare-%s-%s.wav" % (args.wave, name))
        write_wav(part_path, [track], args.rate, gain=0.9)
        part_paths.append(part_path)

    seconds = max(len(t) for t in rendered) / float(args.rate)
    print("%s  %d voices, %.2f s" % (midi_path, len(voices), seconds))
    for (name, notes), part_path in zip(voices, part_paths):
        sounded = sum(1 for hz, _ in notes if hz > 0.0)
        print("  %-7s %2d notes  ->  %s" % (name, sounded, part_path))
    print("%s  all voices mixed" % mix_path)


if __name__ == "__main__":
    main()
