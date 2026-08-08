// ============================================================================
// ModMatrix.h — route any modulation source to any destination
// ============================================================================
// This is the thing that makes a synth feel open-ended rather than fixed. Up
// to now every modulation was hardcoded as its own preset field: vibCents was
// "LFO to pitch", pwmD was "LFO to pulse width", fVel was "velocity to
// cutoff". Each needed its own field, its own code path, its own fixed depth.
//
// A matrix replaces all of them with one mechanism: a list of (source,
// destination, depth) triples. Velocity to cutoff and LFO to pitch stop being
// separate features and become two rows in a table.
//
// DELIBERATELY NO ARDUINO DEPENDENCIES. This file and ModMatrix.cpp compile
// with plain g++, which means the maths can be unit tested on a desktop
// instead of being debugged by flashing a board and listening. Every bug in
// this project so far — decibels read as a ratio, a normalised control read
// as a Q value, two envelopes swapped — was a unit error that a test would
// have caught in a second.
// ============================================================================
#pragma once
#include <stdint.h>

// ---- sources -------------------------------------------------------------
enum ModSource : uint8_t {
  SRC_CONST = 0,   // always 1.0 — use for a fixed offset
  SRC_VELOCITY,    // 0..1, how hard the key was struck
  SRC_KEYTRACK,    // -1..1, position on the keyboard (0 at middle C)
  SRC_AMPENV,      // 0..1, the note's own amplitude envelope
  SRC_FILTENV,     // 0..1, the note's own filter sweep
  SRC_LFO1,        // -1..1 bipolar
  SRC_LFO2,        // -1..1 bipolar
  SRC_LFO1_UNI,    // 0..1 unipolar version of LFO1
  SRC_RANDOM,      // -1..1, fixed for the life of one note. This is how you
                   //   fake analog drift: every note is slightly different.
  SRC_NOTEAGE,     // 0..1 ramp over NOTEAGE_FULL_MS since the note started
  SRC_PRESSURE,    // 0..1, key depth RELATIVE to where this note landed
                   //   after its strike. "How much am I leaning past where I
                   //   started." Strike depth cancels out.
  SRC_POSITION,    // 0..1, ABSOLUTE key depth: 0 at rest, 1 at this key's own
                   //   learned bottom. Nothing per-note, nothing remembered.
                   //   A theremin is this and only this.
                   //
                   //   These two are the same sensor read two ways, and both
                   //   are always computed. Choosing between them used to be
                   //   a MODE in the sensing layer, which meant the sensing
                   //   layer had to know which preset was loaded — and that
                   //   upward dependency is how a piano patch ended up
                   //   rescaling every theremin key. It is now a routing
                   //   decision, made here, by the patch.
  SRC_KEYVEL,      // -1..1, how fast the key is moving RIGHT NOW. Positive is
                   //   descending. Position says where the finger IS; this
                   //   says what it is DOING, and the two are independent —
                   //   a key held deep and a key sweeping through the same
                   //   depth read identically on SRC_POSITION and opposite
                   //   here. A bow is this: stop moving and the note dies.
  SRC_ATTACK,      // 1.0 at note-on, decaying to 0 over the preset's atkMs.
                   //   An ONSET window, and the only source that is one.
                   //
                   //   It exists because "velocity shapes the attack but not
                   //   the note" was inexpressible. SRC_VELOCITY is constant
                   //   for the whole note, so routing it to DST_AMP moves the
                   //   level a held note settles at — which is exactly what an
                   //   organ must not do. Routed as
                   //       { SRC_ATTACK, DST_AMP, depth, via SRC_VELOCITY }
                   //   the bump is velocity-sized and then GONE, leaving the
                   //   body at whatever the constant routes say. That is a
                   //   chiff, a key click, a hammer thump: the part of a
                   //   strike that belongs to the beginning of the note.
                   //
                   //   SRC_FILTENV was the only decaying source before this
                   //   and is the wrong tool — its time constant is fTau, the
                   //   filter's own sweep, so borrowing it for amplitude
                   //   welds the two together and neither can be tuned again.
                   //   Nothing stops a patch from using both; they are now
                   //   separate numbers.
  NUM_SOURCES
};

// ---- destinations --------------------------------------------------------
// Units matter. Depth is expressed in the destination's own units, so a
// route's depth reads as "how much of this, at full source value".
enum ModDest : uint8_t {
  DST_PITCH = 0,   // cents, both oscillators (vibrato lives here)
  DST_DETUNE,      // cents, oscillator B only (widens or narrows the beat)
  DST_CUTOFF,      // SEMITONES of filter cutoff offset
  DST_AMP,         // linear gain offset, added to 1.0 (tremolo lives here)
  DST_PWM,         // pulse width offset, added to 0.5
  DST_RES,         // resonance offset
  DST_OSCB,        // oscillator B level offset, added to mixB
  DST_FM,          // 0..1 PHASE MODULATION INDEX, when the preset sets fmRatio.
                   //   This is the one destination that does not shape a sound
                   //   the oscillators already make — it CREATES partials, at
                   //   frequencies carrier +- n*modulator, and when the ratio
                   //   is non-integer those partials are inharmonic. Nothing
                   //   subtractive can do that: a filter can only take away
                   //   from a harmonic series, never move a partial off it.
                   //
                   //   Which is why a Rhodes was out of reach. A tine is a
                   //   stiff rod with inharmonic modes and a spectrum that
                   //   evolves through the whole note; the DX7 owned that
                   //   sound because a decaying index sweeps timbre
                   //   continuously, and that is exactly
                   //   { SRC_FILTENV, DST_FM, depth, via SRC_VELOCITY }.
  DST_NOISE,       // PINK NOISE level offset, added to the preset's `noise`.
                   //   Linear, in the same units as the oscillators: 0.10 is a
                   //   tenth of the voice's peak.
                   //
                   //   The other destination that adds rather than shapes, and
                   //   the counterpart to DST_FM. FM creates partials at
                   //   specific frequencies; this creates energy at ALL of
                   //   them, which is the one thing two oscillators and a
                   //   filter genuinely cannot fake. Breath, bow rosin, pipe
                   //   speech, plectrum, mallet felt — every acoustic
                   //   instrument in the library is part noise, and until now
                   //   every patch here was a pure tone pretending otherwise.
                   //
                   //   PINK, not white. Real breath and bow noise fall off
                   //   with frequency; white noise put through a lowpass still
                   //   sounds like a hiss generator behind the note, because
                   //   the ear reads a flat spectrum as electronic. -3 dB per
                   //   octave is what makes it read as air.
                   //
                   //   Level is multiplied by the voice's amplitude before it
                   //   is applied, so noise decays with the note and stops when
                   //   it does — it is part of the voice, not a bed under it.
  NUM_DESTS
};

// Shape applied to a source before it is scaled by depth. A taper is a
// MUSICAL decision, not a calibration one: a theremin wants an audio taper on
// amplitude (perceived loudness goes as roughly amplitude^0.6, so a linear
// map spends only 6 dB on the whole upper half of the travel) while the same
// finger driving a filter usually wants linear. Putting the curve on the
// route lets one gesture do both.
enum ModCurve : uint8_t {
  CURVE_LINEAR = 0,
  CURVE_SQUARE,      // x^2   — slow start, dramatic top. Audio-taper-ish.
  CURVE_AUDIO,       // x^1.8 — the volume-pedal curve
  CURVE_SQRT,        // x^0.5 — fast start, gentle top
  CURVE_LATE,        // x^3   — almost nothing until you really commit
  NUM_CURVES
};

struct ModRoute {
  uint8_t source;
  uint8_t dest;
  float   depth;
  // Optional second source that SCALES this route. Summing alone can't
  // express "vibrato that grows as you lean on the key" — that needs a
  // multiply. Left at SRC_CONST (which is always 1.0) a route behaves
  // exactly as before, so existing three-field tables are unaffected.
  uint8_t via;
  uint8_t curve;     // ModCurve. Zero (linear) by default, so three- and
                     //   four-field route tables are unaffected.
  // Continuous exponent, OVERRIDING `curve` when non-zero. The five ModCurve
  // values are fixed powers chosen for common musical shapes; this is the
  // same family with the exponent as a free number, for a patch that wants a
  // specific amount of "nothing happens until you commit". The enum tops out
  // at x^3, which gives the last tenth of travel about 4x the expressive
  // density of the middle third — enough for a volume taper, not enough when
  // the sensor itself is compressed through the middle of its range.
  //
  // Zero means "use curve", so every existing route table is unaffected —
  // the same append-a-defaulted-field trick as `via` and `curve` before it.
  float   shape;
};

// ---- LFO -----------------------------------------------------------------
enum LfoShape : uint8_t {
  LFO_SINE = 0,
  LFO_TRIANGLE,
  LFO_SAW_DOWN,
  LFO_RAMP_UP,
  LFO_SQUARE,
  LFO_SAMPLE_HOLD,   // steps to a new random value once per cycle
  NUM_LFO_SHAPES
};

struct LfoConfig {
  float   rateHz;
  uint8_t shape;
  float   delayMs;   // hold at zero this long after the note starts
  float   fadeMs;    // then fade in over this long. Players do this; starting
                     //   a note at full vibrato sounds mechanical.
};

// Everything the matrix needs to know about one voice at one instant.
struct ModContext {
  float velocity;    // 0..1
  float keytrack;    // -1..1
  float ampEnv;      // 0..1
  float filtEnv;     // 0..1
  float ageMs;       // since note on
  float randomv;     // -1..1, drawn once per note
  float pressure;    // 0..1 relative to where this note landed
  float position;    // 0..1 absolute key depth
  float keyvel;      // -1..1 key speed, positive descending
  float attack;      // 1..0 onset window, decayed from the note's age
  float lfoPhase;    // 0..1 CYCLES of head start for this voice's LFOs.
                     //   Without it, every note in a chord struck together
                     //   wobbles in lockstep, which sounds like one machine
                     //   rather than several players.
};

const float NOTEAGE_FULL_MS = 2000.0f;

// One LFO, evaluated from the note's age. Stateless on purpose: sample-and-
// hold is derived by hashing the cycle number rather than being remembered,
// so the same inputs always give the same output and tests are reproducible.
// phaseOffset is in cycles (0..1) and shifts this voice's LFO relative to
// other voices. Defaulted so existing tests and callers are unaffected.
float lfoValue(const LfoConfig &cfg, float ageMs, uint32_t seed,
               float phaseOffset = 0.0f);

// Sum every route into out[NUM_DESTS]. Destinations with no routes come back
// as zero, which is the neutral value for all of them.
void modEvaluate(const ModContext &ctx,
                 const LfoConfig &lfo1, const LfoConfig &lfo2,
                 const ModRoute *routes, int numRoutes,
                 uint32_t seed,
                 float out[NUM_DESTS]);

const char *modSourceName(uint8_t s);
const char *modDestName(uint8_t d);
