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

// ---- velocity -> loudness ------------------------------------------------
// This pair reproduces what synthNoteOn used to apply to every non-continuous
// patch behind the matrix's back: gain = 0.06 + 0.84 * velocity^1.4. The 0.06
// floor is what stops the softest touch being silent; the 1.4 exponent is the
// velocity curve, and it is now a number a patch can argue with rather than a
// constant in the engine.
//
// DST_AMP is an offset added to 1.0, so the CONST route has to cancel that
// unity first — hence -0.94 paired with a 0.84 span.
//
// CONTINUOUS patches deliberately do NOT carry this: their loudness is key
// position, and baking a strike into it is exactly what left a hard hit with
// a loud floor that leaning out could never get below.
//
// A patch wanting a different feel overrides by not using the macro: raise the
// exponent for a wider dynamic range, drop the floor to 0 for true silence at
// the softest touch, or omit it entirely so velocity does not affect level.
#define VEL_TO_AMP \
  { SRC_CONST,    DST_AMP, -0.94f }, \
  { SRC_VELOCITY, DST_AMP,  0.84f, SRC_CONST, CURVE_LINEAR, 1.4f }

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
//   2. Pressure is now scaled by the travel REMAINING below where the note
//      settled, so it stretches all the way to the key's own bottom — but
//      that also means it and position are reading the same stretch of key
//      from two directions, and only one of them needs to be routed here.
//
// The exponent below does the strike-cancelling that `restPos` used to do,
// without the state: x^4 makes everything below ~0.7 nearly inert, so the
// ordinary spread of landing depths produces almost no cutoff movement and
// the expression lives in the last third, where the key is close enough to
// occlude properly and the sensor is finally sensitive.
//
// Revert by putting SRC_PRESSURE back in both places and dropping the shape.
static const ModRoute R_FLUTE[] = {
  VEL_TO_AMP,
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
  // rest-point subtraction: pressure divides by the room left below the note,
  // which is an expression AMPLIFIER — and position, measured from rest, has
  // no equivalent. The exponent buys that leverage back. Total
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

// The SAME flute, expressed through PRESSURE — kept verbatim as it was before
// the position experiment, so the two can be A/B'd on adjacent preset slots
// rather than argued about. Preset 0 is position, preset 1 is this.
//
// What you are comparing is not "which source", it is TWO GESTURES:
//
//   preset 0  position^n. Absolute. Expression lives in the last third of
//             travel, so the gesture is a deliberate PUSH deeper into the
//             key. Where the note landed leaks in: strike hard and you begin
//             partway up the curve already.
//
//   preset 1  pressure. Relative to where THIS note settled, divided by the
//             travel REMAINING below it — so the gesture is a LEAN from
//             wherever you are, down toward the key's own bottom, and the
//             landing depth cancels. Land deep and the same small move spans
//             the range, because there genuinely is less key left. Nothing
//             is learned or accumulated; the divisor is pure geometry.
//
// Both leave DST_AMP alone, so neither changes the underlying dynamic — the
// expression is vibrato and brightness only.
static const ModRoute R_FLUTE_PRESS[] = {
  VEL_TO_AMP,
  { SRC_LFO1,     DST_PITCH,   26.0f, SRC_PRESSURE },
  { SRC_LFO1,     DST_PITCH,    6.0f },   // a little always present
  { SRC_PRESSURE, DST_CUTOFF,   7.0f },   // lean in and it opens up
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
  // AMPLITUDE WANTS A MILD CURVE, WHATEVER THE BOTTOM OF THE RANGE FEELS LIKE.
  // A steep exponent multiplies sensor jitter wherever it is steep: the slope
  // is depth * n * pos^(n-1), so against ~0.011 of travel of jitter, x^4 puts
  // 3.7% of amplitude wobble on a note held near the bottom versus 2.0% for
  // reed's x^1.8. That is amplitude modulation by noise, and it lands as hiss
  // pitched around whatever you are playing — see the anti-zipper note in
  // Synth.cpp, whose "around 1.5%" figure is measured against a 1.8 exponent.
  // 2.2 keeps a flatter bottom than reed while staying in the same noise class.
  { SRC_POSITION, DST_AMP,     0.85f, SRC_CONST, CURVE_LINEAR, 2.2f },
  // BRIGHTNESS WANTS LINEAR. Cutoff depth is in SEMITONES, which are already
  // log frequency, so a linear route is perceptually even across the whole
  // travel — which is exactly why reed (30) and bowed (40) feel controllable
  // everywhere and an x^3 version of this did not. Shaping it made brightness
  // inert to 0.6 and then lurch.
  { SRC_POSITION, DST_CUTOFF, 34.0f },
  { SRC_POSITION, DST_RES,     0.90f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_OSCB,    0.35f, SRC_CONST, CURVE_SQUARE },
  // Vibrato still grows LINEARLY with depth: `via` is multiplied in raw and
  // is deliberately not shaped (see modEvaluate), so there is no way to make
  // this bloom at the top the way the routes above do. Left as it was.
  { SRC_LFO1,     DST_PITCH,   7.0f,  SRC_POSITION },
};

// Airy: vibrato plus a slow wobble in the second oscillator's level, so the
// breathiness itself drifts.
static const ModRoute R_AIRY[] = {
  VEL_TO_AMP,
  { SRC_LFO1, DST_PITCH, 26.0f },
  { SRC_LFO2, DST_OSCB,   0.10f },
};

// Circus 2: the LFO->pitch routing Surge had, plus velocity brightness.
static const ModRoute R_CIRCUS[] = {
  VEL_TO_AMP,
  { SRC_LFO1,     DST_PITCH,  30.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

// Analog drift: every note detunes slightly differently and sits at a
// slightly different brightness. This is what "warm" mostly means.
static const ModRoute R_DRIFT[] = {
  VEL_TO_AMP,
  { SRC_RANDOM,   DST_DETUNE,  6.0f },
  { SRC_RANDOM,   DST_CUTOFF,  2.0f },
  { SRC_VELOCITY, DST_CUTOFF, 10.0f },
  { SRC_KEYTRACK, DST_CUTOFF,  4.0f },
};

// Oberheim-ish: wide velocity brightness, slow vibrato well after onset.
// Converted from pressure to position, on the same argument as the flute:
// SRC_VELOCITY below already carries how hard the key was hit, so routing a
// strike-relative signal as well was measuring the same thing twice. The
// exponents put the expression in the last third of travel, where this sensor
// still has resolution. Safe to do here because this patch is STRUCK and does
// route velocity — see R_HARPSI for the case where neither is true.
static const ModRoute R_OBIE[] = {
  VEL_TO_AMP,
  { SRC_VELOCITY, DST_CUTOFF, 16.0f },
  // Source/via swapped so POSITION can take the exponent and the LFO rides as
  // the multiplier — only the source is shaped. Depth 12 -> 14 because x^3
  // spends the bottom of the range doing nothing.
  { SRC_POSITION, DST_PITCH,  14.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
  { SRC_POSITION, DST_CUTOFF, 14.0f, SRC_CONST, CURVE_LINEAR, 4.0f },
};

// Percussive: hard hits much brighter, brightness tracking the envelope.
static const ModRoute R_PLUCK[] = {
  VEL_TO_AMP,
  { SRC_VELOCITY, DST_CUTOFF, 20.0f },
  { SRC_AMPENV,   DST_CUTOFF, 10.0f },
};

// Pulse-width motion, the Oberheim shimmer.
// Converted from pressure to position — same reasoning as R_OBIE. STRUCK, and
// velocity is routed below, so the strike is already accounted for.
static const ModRoute R_SHIMMER[] = {
  VEL_TO_AMP,
  { SRC_LFO2,     DST_PWM,     0.30f },
  // Swapped so the exponent lands on position; LFO2 rides as the multiplier.
  { SRC_POSITION, DST_PWM,     0.22f, SRC_LFO2, CURVE_LINEAR, 3.0f },
  { SRC_POSITION, DST_OSCB,    0.30f, SRC_CONST, CURVE_LINEAR, 4.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f  },
};

// Tremolo: a wobble in VOLUME rather than pitch. We never had this before.
static const ModRoute R_TREMOLO[] = {
  VEL_TO_AMP,
  // THE ONE PATCH WHERE MOVING VELOCITY INTO THE MATRIX IS NOT A NO-OP.
  // Velocity used to multiply the whole voice, so the tremolo swung a
  // PROPORTION of whatever loudness the strike had set. As a plain additive
  // route it would swing an absolute +/-0.45 instead: at velocity 0.3 the base
  // gain is only 0.216, so the trough would go negative and clamp — a tremolo
  // that chops to silence on soft notes and overshoots on the peak.
  //
  // `via SRC_VELOCITY` restores the proportionality. Checked against the old
  // behaviour: at v=1.0 [0.45,1.35] vs [0.50,1.31], at v=0.1 [0.048,0.138] vs
  // [0.051,0.135]. Close everywhere, and no clamping.
  { SRC_LFO2,     DST_AMP,    0.45f, SRC_VELOCITY },
  { SRC_VELOCITY, DST_CUTOFF, 8.0f  },
};

// Sample-and-hold to cutoff: the stepped, burbling, sci-fi sound.
static const ModRoute R_STEPPED[] = {
  VEL_TO_AMP,
  { SRC_LFO2,     DST_CUTOFF, 14.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

static const ModRoute R_NASAL[] = {
  VEL_TO_AMP,
  { SRC_VELOCITY, DST_CUTOFF, 8.0f },
};

// Deliberately unsubtle, for verifying the pressure path end to end. A
// sawtooth (harmonics to filter), a cutoff low enough that moving it is
// obvious, and a large amplitude swing. If leaning on a key does not
// visibly change THIS, the fault is not in preset design.
// blocky had no route table at all — it relied entirely on the velocity curve
// that used to be hidden in synthNoteOn, so with that gone it needed somewhere
// to put it. One route pair, no modulation: the plainest patch in the library
// and now the clearest statement of what VEL_TO_AMP is.
static const ModRoute R_BLOCKY[] = {
  VEL_TO_AMP,
};

// Deliberately NOT carrying VEL_TO_AMP, even though it is ART_STRUCK. This
// patch exists to sweep pressure and watch what happens, and its own DST_AMP
// pair already owns the gain (0.40 + 0.60*pressure). Adding the velocity pair
// would sum two independent claims on the same destination. It also makes the
// diagnostic strictly better: the sweep no longer depends on how hard the key
// that started it was struck.
static const ModRoute R_PRESSTEST[] = {
  { SRC_PRESSURE, DST_CUTOFF, 36.0f },   // three octaves of brightness
  { SRC_CONST,    DST_AMP,    -0.60f },
  { SRC_PRESSURE, DST_AMP,     0.60f },  // quiet -> full
};

// Converted to position, like obie and shimmer — but for a different reason,
// and it is worth recording why the obvious objection does not hold.
//
// PLUCKED hardcodes velocity to 0.5 (Gesture.cpp, note-on branch), so
// SRC_VELOCITY is a constant here. That means the argument used for the other
// two — "velocity already carries the strike, so pressure is redundant" — has
// nothing to stand on. It does NOT follow that position is unsafe: there is
// simply no redundancy either way, so the choice rests on how each one plays.
//
// The real worry was that absolute position would sneak strike force back in
// as vibrato depth, since a harder pluck tends to land the key deeper and a
// plectrum is supposed to sound identical however hard it is hit. Two things
// defuse that. Landing depth is only CORRELATED with pluck speed, not fixed by
// it — the finger can pluck hard and hold shallow. And the exponent below
// makes everything under ~0.7 nearly inert, so the ordinary spread of landing
// depths moves almost nothing and the vibrato only arrives when the finger
// deliberately pushes into the last third. That is the same trick that let the
// flute drop pressure, and it works here for the same reason.
//
// Which matches what this patch was always after: the pluck is fixed, and the
// motion AFTERWARDS is the expressive part.
static const ModRoute R_HARPSI[] = {
  VEL_TO_AMP,
  // Source/via swapped so position takes the exponent, LFO rides as the
  // multiplier. Depths 9 -> 11 and 10 -> 12 because x^3 and x^4 spend the
  // bottom of the range doing nothing.
  { SRC_POSITION, DST_PITCH,  11.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },  // finger vibrato
  { SRC_POSITION, DST_CUTOFF, 12.0f, SRC_CONST, CURVE_LINEAR, 4.0f },
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
  VEL_TO_AMP,
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
  VEL_TO_AMP,
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
  // -1.000 -> -1.033: the extra 0.033 is exactly p^1.8 evaluated at contOn
  // (0.150), so the gain curve now crosses ZERO at the moment the note starts
  // instead of switching on at -30 dB. Tuned when contOn moved 0.060 -> 0.150
  // to clear crosstalk: at the old floor p^1.8 was 0.0063, or -44 dB, which is
  // inaudible and is why a continuous patch could have "no trigger point" at
  // all; at 0.150 it was 0.0329 and the note announced itself.
  //
  // Done with the CONST rather than a steeper exponent on purpose. A constant
  // subtraction has no slope, so it costs nothing in noise; an exponent
  // multiplies sensor jitter wherever it is steep (see R_SWELL). If contOn
  // moves again, this is the number to move with it — recompute it as
  // contOn^1.8, not by ear.
  //
  // It also silences the whole hysteresis band: at contOff (0.100) the curve
  // is already negative and clamps to zero, so releases cannot click either.
  { SRC_CONST,    DST_AMP,   -1.033f },
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
  // -0.950 -> -1.021, same reasoning as R_REED. This patch was the worse of
  // the two: the old -0.95 left a 0.05 gain FLOOR that never went away, so a
  // note was audible at -25 dB the instant it started even before contOn moved.
  // Folding the floor into the offset and adding 0.95*contOn^2 on top puts the
  // zero crossing at 0.150 exactly. Recompute as 0.05 + 0.95*contOn^2 if the
  // threshold changes.
  { SRC_CONST,    DST_AMP,   -1.021f },
  { SRC_POSITION, DST_AMP,    0.95f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 40.0f },
  { SRC_POSITION, DST_RES,     0.80f, SRC_CONST, CURVE_SQUARE },
  { SRC_LFO1,     DST_PITCH,  10.0f, SRC_POSITION },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
};

// ---- the table -----------------------------------------------------------
// WAVEFORM_BANDLIMIT_*, not the plain names. AudioSynthWaveform's ordinary
// SAWTOOTH, SQUARE and PULSE are generated naively — a raw ramp or a raw step
// straight out of the phase accumulator — so every harmonic above Nyquist
// folds back down into the audible band at an inharmonic frequency. Twenty-five
// detuned voices' worth of folded partials is a wash of grit that reads as
// noise rather than as pitch, and it gets worse the higher you play.
//
// The BANDLIMIT variants (PolyBLEP) round off the discontinuity over a couple
// of samples and remove almost all of it. They cost more CPU per oscillator —
// check with 'a' — and are otherwise drop-in: same amplitude scaling, and
// BANDLIMIT_PULSE still honours pulseWidth(). SINE and TRIANGLE have no
// band-limited variant and need none; a sine has no harmonics to fold and a
// triangle's fall off at 12 dB/octave.
const Preset PRESET[] = {

// ============================ FLUTE FAMILY =================================
{ "flute",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE),
    0.00, 0.40, 0.00,   0,   0.00 ,   ART_STRUCK },

// Identical in every field except the route table. Sits next to the one above
// on purpose: 'n' and 'p' flip between them without touching anything else,
// which is the only honest way to judge a gesture.
{ "flutepr", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE_PRESS),
    0.00, 0.40, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "theremin",WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.00,
   140,  1e9f,  320, 0.90,   10.0, 10.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.8f, LFO_SINE, 0.0f, 400.0f }, LFO_OFF, NROUTES(R_THEREMIN),
    0.00, 0.45, 0.00,   0,   0.00,   ART_CONTINUOUS },

// The pad. Same continuous gesture, but with harmonics for the filter to work
// on, so leaning in changes TIMBRE and not just level — which the ear reads as
// effort in a way that loudness alone does not.
{ "swell",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  9.0, 0.45,
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
{ "circus",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SQUARE,     0,  8.8, 1.00,
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

{ "obie",    WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  9.0, 1.00,
    16,  1e9f,  450, 0.42,   15.0,  4.0,  650, 1.50,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 500.0f }, LFO_OFF, NROUTES(R_OBIE),
    0.15, 0.36, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "fatpluck",WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0, 13.0, 1.00,
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
{ "stepped", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  8.0, 1.00,
     4,  1e9f,  200, 0.45,    6.0,  6.0,  400, 1.80,
    1.00, 0.00, 0.00,
    LFO_OFF, { 8.0f, LFO_SAMPLE_HOLD, 0.0f, 0.0f }, NROUTES(R_STEPPED),
    0.05, 0.30, 0.35, 200,   0.40 ,   ART_STRUCK },

{ "shimmer", WAVEFORM_BANDLIMIT_PULSE,    WAVEFORM_BANDLIMIT_PULSE,      0,  5.0, 1.00,
    25,  1e9f,  600, 0.42,    9.0,  7.0,  800, 1.10,
    1.00, 0.00, 0.00,
    LFO_OFF, { 0.35f, LFO_SINE, 0.0f, 600.0f }, NROUTES(R_SHIMMER),
    0.00, 0.45, 0.00,   0,   0.00 ,   ART_STRUCK },

{ "nasal",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  9.0, 1.00,
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
{ "harpsi",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_PULSE,      0,  7.0, 0.55,
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

{ "reed",    WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SQUARE,    12,  5.0, 0.55,
    40,  1e9f,  250, 0.65,    1.6,  1.6,  400, 1.60,
    1.00, 0.00, 0.00,
    { 5.6f, LFO_SINE, 300.0f, 500.0f }, LFO_OFF, NROUTES(R_REED),
    0.05, 0.35, 0.00,   0,   0.00,   ART_CONTINUOUS },

{ "bowed",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0, 11.0, 1.00,
    30,  1e9f,  350, 0.50,    1.5,  1.5,  400, 1.20,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 700.0f }, LFO_OFF, NROUTES(R_BOWED),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS },

// ============================ DIAGNOSTIC ===================================
{ "presstest", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, 0,  6.0, 1.00,
     6,  1e9f,  200, 0.55,    2.0,  2.0,  400, 1.20,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_PRESSTEST),
    0.00, 0.20, 0.00,   0,   0.00 ,   ART_STRUCK },

// ============================ REFERENCE ====================================
// Single oscillator, no detune, static filter, no modulation, no effects.
{ "blocky",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  0.0, 0.00,
     3,   700,  250, 1.00,   60.0, 60.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_BLOCKY),
    0.00, 0.00, 0.00,   0,   0.00 ,   ART_STRUCK },
};

const int PRESET_COUNT = sizeof(PRESET) / sizeof(PRESET[0]);
