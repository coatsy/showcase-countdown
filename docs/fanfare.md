# The fanfare

<!-- Deliberately not linked from the README. It is meant to be a surprise. -->

When the countdown reaches zero, each unit plays **one voice of a three-part
fanfare** — so a room full of them performs the piece together.

There is no radio link between units at the moment they fire. The shared,
NTP-disciplined clock is the only synchronisation mechanism. That is why the
firmware bothers to write the RTC on a second boundary and re-syncs every six
hours: the whole effect collapses if the units disagree by more than a few tens
of milliseconds.

## The piece

Twelve seconds, in C, built from **C-D-F-G only** — there is no third anywhere
in any voice, so the harmony is a deliberate open fifth. That is the classic
brass-fanfare sonority and it survives a square-wave piezo far better than a
close-voiced triad would.

It is in ternary form, three equal four-second sections:

| Section | Span | Role |
| --- | --- | --- |
| A | 0.0–4.0 s | Statement: pickup, leap, rise, summit |
| B | 4.0–8.0 s | Development: the melody falls to the pedal and climbs back, while the root turns from a sustain into a driving ostinato |
| A' | 8.0–12.0 s | Restatement, ending on a longer held final |

Splitting it this way means the second summit is earned rather than merely
repeated — B spends its time below the melody's opening register so that the
return lands.

| Voice | Weight | Role |
| --- | --- | --- |
| root | 40% | C pedal, no movement of its own |
| fifth | 20% | The harmonic mover: G to F to G, giving Csus4 tension |
| melody | 40% | The only line with real contour |

Each unit picks its voice from a hash of its own MAC address, so a given device
always plays the same part and rehearsal matches the real event. The assigned
voice is printed on the serial console at boot and shown on the diagnostics
screen.

All three voices must be exactly the same length. That is enforced by
`static_assert`, not by convention — retiming one voice without the others is a
build error, because the celebration animation runs for exactly the length of
the melody.

## Check your fleet before the event

The **fifth is the fragile voice**. Across 5 units there is roughly a 33% chance
that no unit draws it, and without it the ensemble thins to root-plus-melody.

| Fleet size | Chance of no fifth |
| --- | --- |
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

Override a single unit by setting `FORCE_VOICE` in its `.env` before flashing:
`0` = root, `1` = fifth, `2` = melody.

## Keeping the helper honest

The helper parses the weights out of `src/fanfare.h` rather than duplicating
them, and `tests/test_voice_assign.py` compiles `src/voice_assign.h` and diffs
the two implementations over 2000 MACs:

```sh
python tests/test_voice_assign.py
```

That matters because a helper whose hash had drifted from the firmware would
report a fleet as covered when it is not — worse than having no helper.

## Audio hardware

The onboard buzzer is a piezo driven as 1-bit delta-sigma over I2S. It is
**too quiet to carry a room**, and a sweep from 440 Hz to 7 kHz was uniformly
quiet, so this is a drive-level limit rather than a resonance mismatch.

The intended fix is the Hat SPK2 (MAX98357 I2S amplifier, 1 W speaker) — see the
hardware table in the README. Untested at the time of writing.

`TRANSPOSE` in `src/fanfare.h` is a per-voice frequency multiplier for tuning
against real hardware. A piezo element is weak below ~500 Hz, so if the root
voice at C5 proves inaudible, set index 0 to `2.0f` to lift it an octave without
touching the score.

## Trying it without waiting

To hear the piece without any hardware at all, render it:

```sh
python scripts/render_fanfare.py --wave square
```

That parses the note tables out of `src/fanfare.h` and writes a MIDI file plus
WAV renders of the full ensemble and of each voice alone, into `tmp/`. Use
`--wave square` for an honest preview of the piezo and `--wave sine` when you
just want to hear the harmony.

With a serial monitor attached to a real unit:

| Key | Effect |
| --- | --- |
| `a` | Audition every voice in sequence |
| `t` | Sweep tones from 440 Hz to 7 kHz |
| `m` | Drive test: sine and square at two magnifications |

Holding button **A** during boot auditions the voices as well, for a unit that
is not plugged into a computer.
