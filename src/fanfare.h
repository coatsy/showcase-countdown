#pragma once

#include <stddef.h>
#include <stdint.h>

// A 4-second open-fifth fanfare split across three monophonic voices.
//
// Each unit plays exactly ONE voice start-to-finish. The harmony is an emergent
// property of the room: with 5-10 units drawing weighted-random voices, the
// ensemble sounds a C pedal, a moving fifth, and a melody line.
//
// There is deliberately no third anywhere in the piece. The palette is C-D-F-G
// only, so the harmony stays quartal/suspended and resolves to a bare open
// fifth. This is the classic brass-fanfare sonority and it survives a harsh
// square-wave piezo far better than a close-voiced triad, where the third beats
// against the fifth.

namespace fanfare {

struct Note {
    float hz;     // 0 = rest
    uint16_t ms;  // includes the note's own release; voices are gapless
};

constexpr float REST = 0.0f;

// Equal temperament, A4 = 440 Hz.
constexpr float C5 = 523.25f;
constexpr float F5 = 698.46f;
constexpr float G5 = 783.99f;
constexpr float C6 = 1046.50f;
constexpr float D6 = 1174.66f;
constexpr float F6 = 1396.91f;
constexpr float G6 = 1567.98f;
constexpr float C7 = 2093.00f;

constexpr uint32_t DURATION_MS = 4000;

// ---------------------------------------------------------------------------
// Voice 0 - ROOT (weight 40%)
//
// A pure C pedal. No harmonic movement of its own; it anchors the piece while
// the fifth moves above it. Rhythmically it drives the opening then sustains.
//
// C5 sits near the low end of what these piezos reproduce well. It gets the
// heaviest weight partly for that reason - the weakest register is carried by
// the most units. If it still disappears on hardware, raise TRANSPOSE[0].
// ---------------------------------------------------------------------------
constexpr Note VOICE_ROOT[] = {
    {C5, 150},   {REST, 50},  {C5, 150},  {REST, 50},   // 0-400    triple-hit pickup
    {C5, 350},   {REST, 50},                            // 400-800  arrival
    {C5, 200},   {REST, 50},  {C5, 200},  {REST, 50},
    {C5, 200},                                          // 800-1500 pulse
    {C5, 500},   {REST, 100},                           // 1500-2100 under the melody peak
    {C5, 250},   {REST, 50},  {C5, 250},  {REST, 50},   // 2100-2700 turn
    {C5, 1300},                                         // 2700-4000 final pedal
};

// ---------------------------------------------------------------------------
// Voice 1 - FIFTH (weight 20%)
//
// Doubles the melody's pickup, then becomes the harmonic mover: G -> F -> G.
// Against the C pedal that reads as C5 -> Csus4 -> C5, which supplies tension
// without ever implying major or minor.
//
// This is the fragile voice. At 20% across 5 units there is a ~33% chance no
// unit draws it, and without it the ensemble thins to root-plus-melody. Check
// the assigned-voice printout before the event and pin a unit with FORCE_VOICE
// if the draw left it empty.
// ---------------------------------------------------------------------------
constexpr Note VOICE_FIFTH[] = {
    {G5, 150},   {REST, 50},  {G5, 150},  {REST, 50},   // 0-400    doubles melody pickup
    {G5, 400},                                          // 400-800  arrival
    {G5, 350},   {REST, 50},  {G5, 300},                // 800-1500 sustain
    {F5, 600},                                          // 1500-2100 sus4 tension
    {G5, 600},                                          // 2100-2700 resolution
    {G5, 1300},                                         // 2700-4000 final fifth
};

// ---------------------------------------------------------------------------
// Voice 2 - MELODY (weight 40%)
//
// The only line with real contour, and the only one that has to imply the
// harmony on its own when the fifth is absent. Shape is the classic fanfare
// arc: low repeated pickup, leap, stepwise rise, peak, then a high final.
//
// Starts in unison with the fifth so the opening hits as one attack, then fans
// out. Palette is C-D-F-G: no third, ever.
// ---------------------------------------------------------------------------
constexpr Note VOICE_MELODY[] = {
    {G5, 150},   {REST, 50},  {G5, 150},  {REST, 50},   // 0-400    pickup, unison with fifth
    {C6, 400},                                          // 400-800  leap to the octave
    {C6, 200},   {D6, 200},   {F6, 300},                // 800-1500 stepwise rise
    {G6, 600},                                          // 1500-2100 peak
    {F6, 250},   {REST, 50},  {G6, 250},  {REST, 50},   // 2100-2700 turn figure
    {C7, 1300},                                         // 2700-4000 final, two octaves up
};

// Per-voice frequency multiplier, for tuning against real hardware.
// Set index 0 to 2.0f if C5 proves inaudible on the piezo.
constexpr float TRANSPOSE[] = {1.0f, 1.0f, 1.0f};

struct Voice {
    const char* name;
    const Note* notes;
    size_t length;
    uint8_t weight;  // percent; must total 100
};

constexpr Voice VOICES[] = {
    {"root", VOICE_ROOT, sizeof(VOICE_ROOT) / sizeof(Note), 40},
    {"fifth", VOICE_FIFTH, sizeof(VOICE_FIFTH) / sizeof(Note), 20},
    {"melody", VOICE_MELODY, sizeof(VOICE_MELODY) / sizeof(Note), 40},
};

constexpr size_t VOICE_COUNT = sizeof(VOICES) / sizeof(Voice);

// C++11-compatible recursive sums so the invariants below are compiler-enforced.
constexpr uint32_t totalMs(const Note* n, size_t len) {
    return len == 0 ? 0u : n[0].ms + totalMs(n + 1, len - 1);
}

constexpr uint16_t totalWeight(const Voice* v, size_t len) {
    return len == 0 ? 0u : v[0].weight + totalWeight(v + 1, len - 1);
}

// Voices must be exactly equal length: they start together on the NTP-derived
// fire instant, and the celebration animation runs for exactly this long.
static_assert(totalMs(VOICE_ROOT, sizeof(VOICE_ROOT) / sizeof(Note)) == DURATION_MS,
              "root voice duration must equal DURATION_MS");
static_assert(totalMs(VOICE_FIFTH, sizeof(VOICE_FIFTH) / sizeof(Note)) == DURATION_MS,
              "fifth voice duration must equal DURATION_MS");
static_assert(totalMs(VOICE_MELODY, sizeof(VOICE_MELODY) / sizeof(Note)) == DURATION_MS,
              "melody voice duration must equal DURATION_MS");

static_assert(totalWeight(VOICES, VOICE_COUNT) == 100, "voice weights must total 100");

}  // namespace fanfare
