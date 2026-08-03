// ============================================================================
// Presets.cpp — the patch library
// ============================================================================
// Each preset owns a route table: which modulation source drives which
// destination, and how much. Depth is in the destination's own units —
// cents for pitch and detune, semitones for cutoff, linear for amplitude.
//
// Field order matches struct Preset:
//   name, waveA, waveB, semiB, cents, mixB,
//   a, tau, r, ampScale,
//   fPeak, fBase, fTau, res,
//   lpMix, bpMix, hpMix,
//   lfo1{rateHz, shape, delayMs, fadeMs}, lfo2{...}, routes, nRoutes,
//   drive, verb, delaySend, delayMs, delayFb
#include <Audio.h>
#include "Preset.h"

#define NROUTES(x) (x), (uint8_t)(sizeof(x) / sizeof(ModRoute))
static const LfoConfig LFO_OFF = { 0.0f, LFO_SINE, 0.0f, 0.0f };

// ---- route tables --------------------------------------------------------

// Flute: vibrato that fades in, and brightness that follows loudness — as a
// note dies away it also darkens, which is what real instruments do.
//
// EXPERIMENT: driven by absolute POSITION, not pressure. This patch used to
// route both expression destinations through SRC_PRESSURE, on the argument
// that a struck note should express relative to where it landed so the strike
// cancels out. Two objections to that:
//
//   1. Velocity ALREADY carries strike intensity, explicitly, on its own
//      route below. Position carrying it again is double-counting, and
//      pressure exists mainly to undo a redundancy we could just not create.
//   2. Pressure's window is at most `heldBand` (0.10) of travel, placed
//      wherever the note happened to land — usually mid-range, which is the
//      most optically compressed part of this sensor's response. It also
//      divides by a learned `width` that is allowed to be 0.012, barely above
//      the ~0.011 jitter floor, so on a narrow-lean key pressure is mostly
//      noise. `w` prints that width; check it before trusting the signal.
//
// The exponent below does the strike-cancelling that `restPos` used to do,
// without the state: x^4 makes everything below ~0.7 nearly inert, so the
// ordinary spread of landing depths produces almost no cutoff movement and
// the expression lives in the last third, where the key is close enough to
// occlude properly and the sensor is finally sensitive.
//
// Revert by putting SRC_PRESSURE back in both places and dropping the shape.
static const ModRoute R_FLUTE[] = {
  // Vibrato that BLOOMS with depth — note the source and via are SWAPPED
  // relative to the obvious spelling, and that is the whole trick.
  //
  // modEvaluate computes curve(source) * via * depth, and only the SOURCE is
  // shaped. Written the obvious way round — LFO as source, position as via —
  // position multiplies in raw and linear, and vibrato is then a near-constant
  // fraction of a held note's depth: moving 0.6 -> 0.7 changes it by 17%,
  // which is not a playable gesture. That is what made this feel dead.
  //
  // Multiplication commutes, so putting POSITION in the source slot lets it
  // take an exponent while the LFO rides along as the multiplier, giving
  // lfo * position^3 * 30. The LFO keeps its own sign (applyCurve shapes
  // magnitude only), so the vibrato stays symmetric.
  //
  // This is what `pressure` was really providing here, and it was not the
  // rest-point subtraction: pressure divides by a learned `width` capped at
  // 0.10, so a 10% lean drove it 0 -> 1. That division is an expression
  // AMPLIFIER, and position has no equivalent. The exponent buys the leverage
  // back, at the cost of asking for a deeper push than the old lean. Total
  // peak deviation, both pitch routes summed, LFO at full fade:
  //     position  0.50   0.70   0.90   1.00
  //     cents      9.8   16.3   27.9   36.0
  // Measured, not estimated — the arithmetic is a host test away, since
  // ModMatrix compiles with plain g++.
  { SRC_POSITION, DST_PITCH,   30.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },
  { SRC_LFO1,     DST_PITCH,    6.0f },   // a little always present
  // Depth raised 7 -> 9 because x^4 spends most of the range doing nothing;
  // set it back to 7.0 if you want a like-for-like A/B against pressure.
  { SRC_POSITION, DST_CUTOFF,   9.0f, SRC_CONST, CURVE_LINEAR, 4.0f },
  { SRC_VELOCITY, DST_CUTOFF,   5.0f },
};

// Theremin: the player's other hand controls VOLUME, continuously, all the
// way down to silence. SRC_CONST pulls the gain floor to ZERO and pressure
// pushes it back up, so amplitude is nothing but key position. Combined with
// ART_CONTINUOUS (velocity ignored, absolute position mapping) a hard strike
// and a slow one produce exactly the same sound — the difference lives
// entirely in where you hold your finger afterwards.
// A theremin is one sine and nothing else. Amplitude is ABSOLUTE key
// position — not pressure, which would measure "past where I landed" and so
// carry the strike with it. CURVE_AUDIO is the volume-pedal taper: a linear
// map spends only 6 dB on the entire upper half of the travel, which is why
// leaning in felt like it did nothing.
static const ModRoute R_THEREMIN[] = {
  { SRC_LFO1,     DST_PITCH,  45.0f },
  { SRC_CONST,    DST_AMP,    -1.00f },
  { SRC_POSITION, DST_AMP,     1.00f, SRC_CONST, CURVE_AUDIO },
};

// The bright-swelling pad the theremin deliberately is not: the same gesture,
// but opening a filter over a sawtooth instead of only changing level. Note
// the curves differ per destination — putting the curve on the ROUTE is what
// lets one finger drive several shapes at once.
//
// FLATTENED BOTTOM, LIVE TOP. This patch used to taper loudness (x^1.8) and
// leave brightness linear, on the theory that the ear wants those different.
// That assumed position was linear in FINGER TRAVEL, and it is not: the map
// in Calibration is linear in ADC COUNTS, and a reflective photodarlington's
// response over distance is anything but. The middle of the range is where
// that compression bites, so a linear route spends most of its depth on
// travel the player cannot resolve, and arrives at the top with nothing left.
//
// So every position route now uses an explicit exponent instead: the bottom
// two thirds deliberately do almost nothing, and the last tenth — where the
// key is close enough to occlude properly and the sensor is finally
// sensitive — carries the swell. Rough shape of what the player gets:
//
//     position   0.30    0.60    0.90    1.00
//     amp  ^4    0.16    0.26    0.71    1.00   (gain, on a 0.15 floor)
//     cut  ^3     0.9    7.3    24.8    34.0    (semitones of cutoff)
//
// The middle third moves brightness ~6 semitones; the top tenth moves it ~9.
// Retune the exponents per key feel — that is what the field is for. Below
// about 2.5 the bottom starts speaking again; above about 5 the top becomes
// a switch rather than a swell.
static const ModRoute R_SWELL[] = {
  { SRC_CONST,    DST_AMP,    -0.85f },
  { SRC_POSITION, DST_AMP,     0.85f, SRC_CONST, CURVE_LINEAR, 4.0f },
  { SRC_POSITION, DST_CUTOFF, 34.0f,  SRC_CONST, CURVE_LINEAR, 3.0f },
  { SRC_POSITION, DST_RES,     0.90f, SRC_CONST, CURVE_LINEAR, 4.0f },
  { SRC_POSITION, DST_OSCB,    0.35f, SRC_CONST, CURVE_LINEAR, 3.0f },
  // Vibrato still grows LINEARLY with depth: `via` is multiplied in raw and
  // is deliberately not shaped (see modEvaluate), so there is no way to make
  // this bloom at the top the way the routes above do. Left as it was.
  { SRC_LFO1,     DST_PITCH,   7.0f,  SRC_POSITION },
};

// Airy: vibrato plus a slow wobble in the second oscillator's level, so the
// breathiness itself drifts.
static const ModRoute R_AIRY[] = {
  { SRC_LFO1, DST_PITCH, 26.0f },
  { SRC_LFO2, DST_OSCB,   0.10f },
};

// Circus 2: the LFO->pitch routing Surge had, plus velocity brightness.
static const ModRoute R_CIRCUS[] = {
  { SRC_LFO1,     DST_PITCH,  30.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

// Analog drift: every note detunes slightly differently and sits at a
// slightly different brightness. This is what "warm" mostly means.
static const ModRoute R_DRIFT[] = {
  { SRC_RANDOM,   DST_DETUNE,  6.0f },
  { SRC_RANDOM,   DST_CUTOFF,  2.0f },
  { SRC_VELOCITY, DST_CUTOFF, 10.0f },
  { SRC_KEYTRACK, DST_CUTOFF,  4.0f },
};

// Oberheim-ish: wide velocity brightness, slow vibrato well after onset.
static const ModRoute R_OBIE[] = {
  { SRC_VELOCITY, DST_CUTOFF, 16.0f },
  { SRC_LFO1,     DST_PITCH,  12.0f, SRC_PRESSURE },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
  { SRC_PRESSURE, DST_CUTOFF, 12.0f },
};

// Percussive: hard hits much brighter, brightness tracking the envelope.
static const ModRoute R_PLUCK[] = {
  { SRC_VELOCITY, DST_CUTOFF, 20.0f },
  { SRC_AMPENV,   DST_CUTOFF, 10.0f },
};

// Pulse-width motion, the Oberheim shimmer.
static const ModRoute R_SHIMMER[] = {
  { SRC_LFO2,     DST_PWM,     0.30f },
  { SRC_LFO2,     DST_PWM,     0.18f, SRC_PRESSURE },
  { SRC_PRESSURE, DST_OSCB,    0.25f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f  },
};

// Tremolo: a wobble in VOLUME rather than pitch. We never had this before.
static const ModRoute R_TREMOLO[] = {
  { SRC_LFO2,     DST_AMP,    0.45f },
  { SRC_VELOCITY, DST_CUTOFF, 8.0f  },
};

// Sample-and-hold to cutoff: the stepped, burbling, sci-fi sound.
static const ModRoute R_STEPPED[] = {
  { SRC_LFO2,     DST_CUTOFF, 14.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

static const ModRoute R_NASAL[] = {
  { SRC_VELOCITY, DST_CUTOFF, 8.0f },
};

// Deliberately unsubtle, for verifying the pressure path end to end. A
// sawtooth (harmonics to filter), a cutoff low enough that moving it is
// obvious, and a large amplitude swing. If leaning on a key does not
// visibly change THIS, the fault is not in preset design.
static const ModRoute R_PRESSTEST[] = {
  { SRC_PRESSURE, DST_CUTOFF, 36.0f },   // three octaves of brightness
  { SRC_CONST,    DST_AMP,    -0.60f },
  { SRC_PRESSURE, DST_AMP,     0.60f },  // quiet -> full
};

static const ModRoute R_HARPSI[] = {
  { SRC_LFO1,     DST_PITCH,   9.0f, SRC_PRESSURE },   // finger vibrato
  { SRC_PRESSURE, DST_CUTOFF, 10.0f },
  { SRC_KEYTRACK, DST_CUTOFF,  4.0f },
  { SRC_RANDOM,   DST_DETUNE,  5.0f },
};

// ============================ SHOWCASE =====================================
// Four patches built around the one thing this instrument can do that no
// conventional keyboard can: twenty-five INDEPENDENT continuous streams,
// under one hand, at the same time. The test for each was "could a keyboard
// with perfect polyphonic aftertouch do this?" — if yes, it isn't a showcase.

// CHORDTUNE — retune each note of a held chord with your fingers.
// Equal temperament is a compromise: a just major third is 14 cents flat of
// ET, a just minor third 16 cents sharp, the harmonic seventh 31 cents flat.
// Position spans about +/-26 cents around neutral, so you can hold a triad
// and push each note until the beating stops. You are tuning by ear, in real
// time, per voice. Plucked, so the strike gets the note going and then leaves
// the key entirely to your ear.
static const ModRoute R_CHORDTUNE[] = {
  { SRC_CONST,    DST_PITCH, -60.0f },
  { SRC_POSITION, DST_PITCH,  85.0f },
  { SRC_KEYTRACK, DST_CUTOFF,  3.0f },
};

// DRAWBAR — a Hammond has nine drawbars for the WHOLE instrument. This gives
// every key its own. The second oscillator is a nineteenth up (octave plus
// fifth, the classic 2 2/3' drawbar), brought in by depth with a square curve
// so it arrives late and deliberately. Hold a chord, lean on one note, and
// that note alone grows a fifth. Gated: no velocity, hard onset, organ
// behaviour — the expression is entirely in where the keys sit.
static const ModRoute R_DRAWBAR[] = {
  { SRC_CONST,    DST_OSCB,  -0.70f },
  { SRC_POSITION, DST_OSCB,   0.70f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 8.0f },
};

// REED — a wind instrument's note does not exist below a certain effort, and
// changes character when you push past it. Amplitude follows position with an
// audio taper; the octave partial arrives on a CUBIC curve, so almost nothing
// happens until you really commit and then the note opens up and strains
// upward. Vibrato appears only under depth, as a player's does.
static const ModRoute R_REED[] = {
  { SRC_CONST,    DST_AMP,   -1.00f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },
  { SRC_CONST,    DST_OSCB,  -0.55f },
  { SRC_POSITION, DST_OSCB,   0.55f, SRC_CONST, CURVE_LATE },
  { SRC_POSITION, DST_CUTOFF, 30.0f },
  { SRC_LFO1,     DST_PITCH,  14.0f, SRC_POSITION },
};

// BOWED — the family this instrument is really suited to. A bow has no
// attack: loudness and brightness both follow the arm, together, and that
// correlation is most of what makes a bow sound like a bow. Resonance rises
// with depth so hard bowing gets that pinched, pressed quality. Per-note
// random detune keeps a chord from sounding like one machine.
static const ModRoute R_BOWED[] = {
  { SRC_CONST,    DST_AMP,   -0.95f },
  { SRC_POSITION, DST_AMP,    0.95f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 40.0f },
  { SRC_POSITION, DST_RES,     0.80f, SRC_CONST, CURVE_SQUARE },
  { SRC_LFO1,     DST_PITCH,  10.0f, SRC_POSITION },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
};

// ---- the table -----------------------------------------------------------
const Preset PRESET[] = {

// ============================ FLUTE FAMILY =================================
{ "flute",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE),
    0.00, 0.40, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "theremin",WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.00,
   140,  1e9f,  320, 0.90,   10.0, 10.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.8f, LFO_SINE, 0.0f, 400.0f }, LFO_OFF, NROUTES(R_THEREMIN),
    0.00, 0.45, 0.00,   0,   0.00,   ART_CONTINUOUS },

// The pad. Same continuous gesture, but with harmonics for the filter to work
// on, so leaning in changes TIMBRE and not just level — which the ear reads as
// effort in a way that loudness alone does not.
{ "swell",   WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 0.45,
    60,  1e9f,  600, 0.70,    1.8,  1.8,  400, 0.90,
    1.00, 0.00, 0.00,
    { 4.6f, LFO_SINE, 200.0f, 600.0f }, LFO_OFF, NROUTES(R_SWELL),
    0.00, 0.42, 0.00,   0,   0.00,   ART_CONTINUOUS },

{ "airy",    WAVEFORM_SINE,     WAVEFORM_TRIANGLE,  12,  6.0, 0.18,
   110,  1e9f,  300, 0.85,    8.0,  8.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.2f, LFO_SINE, 300.0f, 400.0f },
    { 0.28f, LFO_TRIANGLE, 0.0f, 800.0f }, NROUTES(R_AIRY),
    0.00, 0.45, 0.20, 260,   0.30 ,   ART_STRUCK },

// ============================ CIRCUS 2 =====================================
{ "circus",  WAVEFORM_SAWTOOTH, WAVEFORM_SQUARE,     0,  8.8, 1.00,
     4,  1e9f,   31, 0.50,    2.0,  2.0,  500, 1.60,
    0.00, 0.00, 1.00,
    { 5.84f, LFO_SINE, 120.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
    0.06, 0.20, 0.58, 250,   0.71 ,   ART_STRUCK },

{ "circusb", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  6.0, 0.35,
     8,  1e9f,   60, 0.85,    9.0,  9.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.84f, LFO_SINE, 90.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
    0.00, 0.30, 0.45, 250,   0.68 ,   ART_STRUCK },

// ============================ ANALOG-ISH ===================================
{ "drift",   WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  7.0, 1.00,
     4,   900,  320, 0.50,    9.0,  5.0,  500, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRIFT),
    0.00, 0.25, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "wide",    WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0, 16.0, 1.00,
     4,  1100,  400, 0.50,    8.0,  5.0,  600, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRIFT),
    0.00, 0.32, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "obie",    WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
    16,  1e9f,  450, 0.42,   15.0,  4.0,  650, 1.50,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 500.0f }, LFO_OFF, NROUTES(R_OBIE),
    0.15, 0.36, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "fatpluck",WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0, 13.0, 1.00,
     2,   380,  140, 0.42,   24.0,  3.0,  180, 1.60,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_PLUCK),
    0.10, 0.20, 0.00,   0,   0.00 ,   ART_STRUCK },

// ============================ NEW MOTION ===================================
// Tremolo — volume wobble. Only possible now the matrix exists.
{ "tremolo", WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  6.0, 1.00,
    20,  1e9f,  300, 0.55,    8.0,  8.0,  400, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, { 5.5f, LFO_SINE, 150.0f, 400.0f }, NROUTES(R_TREMOLO),
    0.00, 0.30, 0.00,   0,   0.00 ,   ART_STRUCK },

// Sample-and-hold stepping the filter. Burbling and mechanical.
{ "stepped", WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  8.0, 1.00,
     4,  1e9f,  200, 0.45,    6.0,  6.0,  400, 1.80,
    1.00, 0.00, 0.00,
    LFO_OFF, { 8.0f, LFO_SAMPLE_HOLD, 0.0f, 0.0f }, NROUTES(R_STEPPED),
    0.05, 0.30, 0.35, 200,   0.40 ,   ART_STRUCK },

{ "shimmer", WAVEFORM_PULSE,    WAVEFORM_PULSE,      0,  5.0, 1.00,
    25,  1e9f,  600, 0.42,    9.0,  7.0,  800, 1.10,
    1.00, 0.00, 0.00,
    LFO_OFF, { 0.35f, LFO_SINE, 0.0f, 600.0f }, NROUTES(R_SHIMMER),
    0.00, 0.45, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "nasal",   WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
     6,   800,  280, 0.60,    6.0,  3.0,  350, 2.60,
    0.10, 0.90, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_NASAL),
    0.20, 0.28, 0.20, 220,   0.25 ,   ART_STRUCK },

// ============================ PLUCKED ======================================
// The hybrid the whole articulation split exists for. A plectrum plucks the
// string identically however hard the key is hit, so velocity is DISCARDED —
// but the finger stays on the key afterwards, and that continuing motion adds
// the vibrato. Struck trigger, expressive hold: neither a piano nor a
// theremin, and impossible to express before gesture and sound were separated.
{ "harpsi",  WAVEFORM_SAWTOOTH, WAVEFORM_PULSE,      0,  7.0, 0.55,
     2,  1400,  260, 0.55,   14.0,  4.0,  240, 1.30,
    1.00, 0.00, 0.00,
    { 5.2f, LFO_SINE, 150.0f, 400.0f }, LFO_OFF, NROUTES(R_HARPSI),
    0.08, 0.30, 0.00,   0,   0.00,   ART_PLUCKED },

// ============================ SHOWCASE =====================================
{ "chordtune",WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,  0,  3.0, 0.80,
    30,  1e9f,  400, 0.55,    7.0,  7.0,  400, 0.75,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_CHORDTUNE),
    0.00, 0.35, 0.00,   0,   0.00,   ART_PLUCKED },

{ "drawbar", WAVEFORM_SINE,     WAVEFORM_SINE,      19,  0.0, 0.70,
     8,  1e9f,  120, 0.80,   12.0, 12.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRAWBAR),
    0.00, 0.30, 0.00,   0,   0.00,   ART_GATED },

{ "reed",    WAVEFORM_SAWTOOTH, WAVEFORM_SQUARE,    12,  5.0, 0.55,
    40,  1e9f,  250, 0.65,    1.6,  1.6,  400, 1.60,
    1.00, 0.00, 0.00,
    { 5.6f, LFO_SINE, 300.0f, 500.0f }, LFO_OFF, NROUTES(R_REED),
    0.05, 0.35, 0.00,   0,   0.00,   ART_CONTINUOUS },

{ "bowed",   WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0, 11.0, 1.00,
    30,  1e9f,  350, 0.50,    1.5,  1.5,  400, 1.20,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 700.0f }, LFO_OFF, NROUTES(R_BOWED),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS },

// ============================ DIAGNOSTIC ===================================
{ "presstest", WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH, 0,  6.0, 1.00,
     6,  1e9f,  200, 0.55,    2.0,  2.0,  400, 1.20,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_PRESSTEST),
    0.00, 0.20, 0.00,   0,   0.00 ,   ART_STRUCK },

// ============================ REFERENCE ====================================
// Single oscillator, no detune, static filter, no modulation, no effects.
{ "blocky",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  0.0, 0.00,
     3,   700,  250, 1.00,   60.0, 60.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, nullptr, 0,
    0.00, 0.00, 0.00,   0,   0.00 ,   ART_STRUCK },
};

const int PRESET_COUNT = sizeof(PRESET) / sizeof(PRESET[0]);
