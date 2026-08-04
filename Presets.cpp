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
// route velocity. A PLUCKED patch would not be: PLUCKED hardcodes velocity to
// 0.5, so SRC_VELOCITY is a constant and there is no strike information in the
// matrix at all — the double-counting argument has nothing to stand on there.
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
  // multiplier.
  { SRC_POSITION, DST_PITCH,  11.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },  // finger vibrato
  { SRC_POSITION, DST_CUTOFF, 12.0f, SRC_CONST, CURVE_LINEAR, 4.0f },
  // +4 -> -5, and this was half of "electronic and tinny".
  //
  // Every cutoff here is already a multiple of the note's own pitch, so the
  // filter tracks the keyboard exactly. A POSITIVE keytrack depth made the top
  // octave brighter still — pitch tracking plus more on top — which is why the
  // high notes were the thinnest. Negative pins some of the brightness to an
  // absolute frequency instead, the way a real soundboard resonance does not
  // move when you change note. Same trick brass needed.
  { SRC_KEYTRACK, DST_CUTOFF, -5.0f },
  // 5 -> 3. oscB is now a pure sine two octaves up, sitting almost exactly on
  // oscA's 4th harmonic, and "perfectly resonant" means letting it lock there
  // rather than beat against it.
  { SRC_RANDOM,   DST_DETUNE,  3.0f },
  // THE BELL, STRUCK. Epiano's tine trick, but short and small: filtEnv runs
  // 1 -> 0 over fTau (150 ms), so this is an extra ring of the high partial at
  // the moment of the pluck, on top of the sine that keeps sounding underneath
  // it. Via velocity, so a hard pluck rings and a soft one just speaks.
  { SRC_FILTENV,  DST_OSCB,    0.30f, SRC_VELOCITY },
};

// ============================ STRUCK BAR ===================================
// A GLOCKENSPIEL, and the one thing that makes it one is INHARMONICITY.
//
// The previous attempt at this became indistinguishable from epiano, for a
// reason worth writing down: it used semiB 24, and 24 semitones is EXACTLY the
// 4th harmonic. A partial sitting on a harmonic FUSES with the fundamental and
// the ear hears one tone with a bright colour — which is precisely what makes
// epiano's tine golden rather than metallic. Metal sounds like metal because
// its partials refuse to fuse.
//
// A free-free bar's modes are 1 : 2.756 : 5.404 : 8.933 — nowhere near
// integers. semiB 18 is 2.83x, close to that second mode and, critically,
// sitting BETWEEN the 2nd harmonic (2.0) and the 3rd (3.0) where nothing can
// lock to it. That single number is the difference between a bell and a bar.
//
// TWO CORRECTIONS, both mine, and worth recording because they are the two
// ways an inharmonic partial goes wrong.
//
// semiB 18 -> 17. Eighteen semitones is an octave plus SIX, and six semitones
// is the tritone — the most dissonant interval there is. 2.828x happened to be
// arithmetically closest to the bar's 2.756 mode, and I picked it on that
// number alone without checking what interval it spelled. 17 is 2.670x, only
// 3% further from the ideal, and it spells an octave plus a perfect fourth.
//
// mixB 0.85 -> 0.00, with the partial now arriving through filtEnv instead.
// This is the bigger error. A partial only reads as TIMBRE while the
// fundamental clearly dominates; at 0.85 the ear stops hearing colour and
// starts hearing a second note, and then the interval matters enormously.
// Gated through filtEnv the clang exists for ~200 ms and is gone, which is
// also what a real bar does — its high modes decay far faster than its first.
//
// That means this shares epiano's mechanism after all. I avoided it originally
// to keep the patches distinct, which was the wrong reason: the mechanism is
// correct for both, and the RATIO is the real distinction. epiano's +24 is
// exactly the 4th harmonic and fuses into one golden tone; +17 cannot fuse and
// rings as metal. One number, entirely different instrument.
static const ModRoute R_GLOCK[] = {
  VEL_TO_AMP,
  // The strike clang: inharmonic, loud for an instant, then gone — leaving a
  // pure triangle ringing on. Metallic attack, clean tone.
  { SRC_FILTENV,  DST_OSCB,   0.55f, SRC_VELOCITY },
  // No vibrato, no tremolo, no LFO of any kind. A struck bar cannot bend and
  // cannot swell; every one of those absences is load-bearing.
  { SRC_VELOCITY, DST_CUTOFF, 14.0f },
  { SRC_POSITION, DST_CUTOFF,  8.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
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
  // SWEETENED, same treatment as strings and strbow, but this patch keeps its
  // own identity: two full saws in NEAR-UNISON rather than an octave-down
  // triangle. That is the difference between one player and a section — a
  // violin gets its richness from a body, not from a second register — so
  // bowed stays dense and single-register while strbow carries the weight.
  //
  // 40 -> 30 semitones. The old top reached 15x the fundamental, which is where
  // the detuned pair's rough upper harmonics were finally getting through: the
  // dark fBase of 1.5x was hiding them at rest and the wide sweep exposed them
  // under the finger. 30 lands at 8.5x, still a 5.7:1 range — wider than
  // strbow's 4:1, which is right for the more expressive of the two.
  { SRC_POSITION, DST_CUTOFF, 30.0f },
  // THE RESONANCE ROUTE IS GONE, and res is now a constant 1.50 on the preset
  // row. This is the thing that was making it dead.
  //
  // Compare reed, which is the liveliest patch here: its res is 1.60 and it
  // NEVER MOVES, while the cutoff sweeps 1.6x -> 9x. A fixed peak travelling
  // past the harmonics one by one is a formant sweep, and that sweep is the
  // "nasal middle" that makes reed sound like an instrument. Bowed had the
  // peak climbing alongside the cutoff, so the two moved together and the
  // resonance never swept past anything — it just got sharper. Sweetening it
  // to 1.55 only made a weak effect weaker.
  //
  // oscB ARRIVES IN THE UPPER HALF, also reed's structure. The bottom of the
  // travel is now a single saw: no partner, so no beating at all, which is as
  // pure as this patch can get. The detuned second oscillator joins as you
  // press, and that arrival is an EVENT — something happening at a particular
  // depth rather than everything sliding smoothly together.
  { SRC_CONST,    DST_OSCB,  -0.75f },
  { SRC_POSITION, DST_OSCB,   0.75f, SRC_CONST, CURVE_SQUARE },
  // VIBRATO STEERED INTO THE UPPER HALF, as you asked. Source and via swapped
  // so POSITION can take an exponent — only the source is shaped, and `via`
  // multiplies in raw. Written the obvious way round (LFO as source, position
  // as via) vibrato grows linearly and is a near-constant fraction of depth,
  // which is not a playable gesture. At x^3: 2 cents at half travel, 6.8 at
  // three quarters, 16 at the bottom.
  { SRC_POSITION, DST_PITCH,  16.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
};

// ============================ ENSEMBLE =====================================
// A string ensemble is not one sound, it is several nearly-identical ones
// beating against each other. Two oscillators give a beat at ONE rate, and a
// fixed beat reads as "out of tune" rather than "ensemble" — so LFO2 walks the
// detune slowly, which is what the old string machines got from their
// bucket-brigade chorus. SRC_RANDOM on top offsets every note, so a chord
// struck together does not wander in lockstep.
// THIRD ATTEMPT, and the two failures are the useful part of the story.
//
//   v1: 18 cents detune, a 0.23 Hz LFO on DST_DETUNE, plus an always-on
//       vibrato. Up to 39 cents of instability, most of it wandering.
//       SEASICK. And metallic, because two saws c cents apart have their nth
//       harmonic beating at n TIMES the fundamental rate — at 18 cents
//       everything above the 5th harmonic beats past 20 Hz, which the ear
//       files as roughness rather than chorus.
//
//   v2: cured the roughness by holding oscB out until the top of the travel,
//       reed-style. That left the bottom two-thirds as ONE bright saw with
//       the filter wide open. TINNY — the opposite defect.
//
// Both failures come from trying to get body out of near-unison detune. Two
// oscillators a few cents apart can only ever produce one beat frequency and
// no extra weight; that is a supersaw's job and it needs seven of them.
//
// So stop asking detune for body and ask an OCTAVE for it. semiB -12 puts
// oscB a full octave down, which does not produce the dense harmonic
// roughness near-unison does, so it can stay at full level throughout — and
// an octave below the note is exactly the weight that was missing. The
// ensemble width still comes from the chord: 25 voices, each with its own
// fixed random detune, so five keys held together are ten oscillators sitting
// at ten different offsets and STAYING there.
static const ModRoute R_STRINGS[] = {
  VEL_TO_AMP,
  { SRC_RANDOM,   DST_DETUNE, 4.0f },    // per-note, static, widens a chord
  // 22 -> 17 and 7 -> 5. The top of the range was reaching 11x the fundamental,
  // which on a sawtooth is a lot of upper harmonic to have present at once.
  // Sweetness in this library has a precise meaning — reed's "sweet" bottom is
  // the filter at 1.6x, i.e. almost nothing but the fundamental — so this is
  // simply less of what makes a saw buzz.
  { SRC_POSITION, DST_CUTOFF, 17.0f },   // linear, like reed and bowed
  { SRC_VELOCITY, DST_CUTOFF,  5.0f },
  // Vibrato you ADD rather than vibrato that is always on.
  { SRC_LFO1,     DST_PITCH,   6.0f, SRC_POSITION },
};

// STRINGS TIMBRE, BOWED GESTURE. The same octave-down saw pair and warm filter
// as R_STRINGS, played the way R_BOWED is played: ART_CONTINUOUS, no strike at
// all, loudness nothing but where the finger is.
//
// This is the clearest demonstration in the library that articulation and
// timbre are independent. Nothing about the oscillators changes; what changes
// is where DST_AMP gets its source, and one enum on the preset row. strings is
// a chord you hit and hold; this is a single line you draw.
//
// The CONST is -1.015, which is exactly 0.150^2.2 below unity — so the gain
// curve crosses ZERO at contOn and the note swells from true silence rather
// than switching on. Recompute it as contOn^2.2 if that threshold moves.
static const ModRoute R_STRBOW[] = {
  { SRC_CONST,    DST_AMP,   -1.015f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_LINEAR, 2.2f },
  { SRC_POSITION, DST_CUTOFF, 24.0f },
  // Resonance climbing under the finger is bowed's trick and most of why it
  // feels alive: pressing does not just open the filter, it sharpens it.
  //
  // 0.60 -> 0.35 as part of the sweetening. This is the one place where the two
  // goals genuinely pull against each other — a rising Q is what makes the
  // gesture feel like it is doing something, and a peak is also exactly what
  // brass taught us reads as nasal. 0.35 climbs 0.80 -> 1.15, which is enough
  // to hear the filter sharpen and not enough to put a bump on a harmonic.
  { SRC_POSITION, DST_RES,     0.35f, SRC_CONST, CURVE_SQUARE },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
  { SRC_LFO1,     DST_PITCH,   7.0f, SRC_POSITION },
};

// Brass is defined by its ATTACK: the tone starts dark and the brightness
// arrives a moment later. That rise cannot come from the engine's own sweep —
// voiceCutPeak is clamped to be >= voiceCutBase, so the built-in sweep only
// ever FALLS. It has to come from the matrix instead, and SRC_FILTENV with a
// NEGATIVE depth is exactly it: filtEnv runs 1 -> 0 over fTau, so the cutoff
// starts 20 semitones below where fBase puts it and climbs.
//
// fTau (110 ms) IS the blat. Shorten it for a tighter stab, lengthen it toward
// 250 ms for something that swells like a section coming in.
static const ModRoute R_BRASS[] = {
  VEL_TO_AMP,
  { SRC_FILTENV,  DST_CUTOFF, -20.0f },  // dark -> bright over fTau
  { SRC_VELOCITY, DST_CUTOFF,  14.0f },  // blow harder, get brighter
  { SRC_POSITION, DST_CUTOFF,  12.0f, SRC_CONST, CURVE_LINEAR, 2.5f },
  // A RESONANT PEAK RIDING THE ATTACK. res sits at 1.60 and this lifts it to
  // ~2.5 for the duration of the blat, then it settles. A brass bell has a
  // strong formant that is loudest exactly while the note is speaking, and a
  // moving peak over a sawtooth is how you fake one. Costs one route.
  // 1.30 -> 0.30, and the base res drops 1.60 -> 1.05 on the preset row.
  // RESONANCE IS NOT POWER. Chasing "powerful" by raising Q put a peak of 2.90
  // on the attack, and a strong narrow peak sweeping through the harmonics is
  // precisely a wah — hence "duck". It also reads as tinny, because the peak
  // lifts whatever upper harmonic sits at the cutoff and leaves everything
  // below it relatively quieter.
  //
  // Real brass does have a formant, but a bell's radiation is BROAD. A
  // state-variable filter at Q near 3 is far narrower than anything acoustic.
  // Power here comes from harmonic density — many partials at once, which is
  // a high cutoff and two full oscillators — not from emphasising one.
  { SRC_FILTENV,  DST_RES,      0.30f },
  // A FIXED FORMANT — the cure for "electronic".
  //
  // Every cutoff in this synth is a multiple of the note's own pitch, so the
  // filter tracks the keyboard exactly. That is deliberate (see Preset.h) and
  // it is also the most synthetic thing here: a real trumpet's bell resonates
  // at ONE frequency whatever you play, which is why its low notes sound rich
  // and its high notes sound piercing. Perfect tracking gives every note the
  // same spectrum transposed, and the ear reads that as a machine.
  //
  // keytrack runs (note-60)/12, so +-1 across this keyboard, and one octave of
  // pitch is +12 semitones of cutoff. Depth -12 would cancel the tracking
  // completely and pin the formant at an absolute frequency; -4 cancels a
  // third of it, enough to stop every note being the same spectrum transposed
  // without making the low notes noticeably duller than the high ones.
  { SRC_KEYTRACK, DST_CUTOFF,  -4.0f },
  // ...and a slow SECOND opening long after the blat is done. NOTEAGE ramps
  // over 2 s, so this is the player leaning into a held note rather than the
  // attack transient. Two brightness envelopes on different timescales is
  // most of what separates a sustained brass note from a saw through a filter.
  { SRC_NOTEAGE,  DST_CUTOFF,   7.0f, SRC_CONST, CURVE_SQRT },
  { SRC_LFO1,     DST_PITCH,    9.0f },
  { SRC_RANDOM,   DST_DETUNE,   6.0f },
};

// A Rhodes is two sounds decaying at DIFFERENT RATES: a sine-ish body that
// rings for over a second, and a bell-like tine two octaves up that is gone in
// a couple of hundred milliseconds. There is one amplitude envelope per voice,
// so the second rate has to come from somewhere else — here, filtEnv driving
// oscB's LEVEL instead of the filter. mixB is 0.00, so the tine exists ONLY as
// this route: it appears at note-on and fades over fTau (190 ms) while the body
// decays on its own tau (1500 ms). Two decay rates, one envelope object.
//
// `via SRC_VELOCITY` is the character of the whole instrument. Lean on it and
// the bell barks; play softly and you get very nearly pure sine. On a real
// Rhodes that is the hammer striking nearer the tine, and it is why the thing
// answers to touch the way it does — the dynamic is TIMBRE, not just level.
static const ModRoute R_EPIANO[] = {
  VEL_TO_AMP,
  { SRC_FILTENV,  DST_OSCB,    0.90f, SRC_VELOCITY },   // the tine
  { SRC_KEYTRACK, DST_OSCB,    0.20f },  // bell up top, rounder in the bass
  { SRC_VELOCITY, DST_CUTOFF, 12.0f },
  { SRC_POSITION, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  // Tremolo, scaled by velocity for the same reason R_TREMOLO is: a bare
  // additive AMP route clamps to silence on soft notes.
  { SRC_LFO1,     DST_AMP,     0.12f, SRC_VELOCITY },
};

// The OTHER electric piano, and a genuinely different instrument rather than a
// second flavour of the one above. A Rhodes strikes a TINE — a stiff rod whose
// tone is nearly a pure high partial, which is why epiano is sine + sine and
// why its character lives in a bell that fades. A Wurlitzer strikes a flat
// steel REED read by an electrostatic pickup, and a reed is hollow: strong odd
// harmonics, closer to a pulse than to a sine.
//
// The identity is the BARK. Hit a Wurli hard and the reed's own nonlinearity
// makes it snarl — far more brightness-per-velocity than a Rhodes, and the
// tone gets nasal as well as loud. Two routes do that here: a steep velocity
// curve on the cutoff, and velocity NARROWING the pulse, since a narrower
// pulse is more odd-harmonic and more hollow. Play it softly and it is a
// rounded, almost flutey thing; lean on it and it snaps.
static const ModRoute R_WURLI[] = {
  VEL_TO_AMP,
  // x^2 on the cutoff: soft notes stay well back, hard notes jump. epiano's
  // equivalent route is a flat 12 semitones with no curve at all — that
  // difference in shape IS the difference between a tine and a reed.
  { SRC_VELOCITY, DST_CUTOFF, 22.0f, SRC_CONST, CURVE_SQUARE },
  { SRC_VELOCITY, DST_PWM,   -0.18f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
  // Tremolo is not an effect bolted onto a Wurlitzer, it is wired into the
  // instrument — hence deeper than epiano's 0.12. Via velocity, because a bare
  // additive AMP route clamps to silence on soft notes.
  { SRC_LFO1,     DST_AMP,     0.22f, SRC_VELOCITY },
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
// Reinstated as it was: a golden plucked string with a small bell on it.
// oscB is a pure sine two octaves up, exactly oscA's 4th harmonic, so it
// reinforces a partial the string already has rather than adding a voice —
// which is what makes it ring rather than clang. fBase 7.0 keeps it glowing
// after the pluck instead of going dull the moment the transient passes.
{ "harpsi",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_SINE,               24,  4.0, 0.40,
     2,  1600,  300, 0.60,   16.0,  7.0,  150, 1.30,
    1.00, 0.00, 0.00,
    { 5.2f, LFO_SINE, 150.0f, 400.0f }, LFO_OFF, NROUTES(R_HARPSI),
    0.06, 0.38, 0.00,   0,   0.00,   ART_PLUCKED },

// SILVERY, BRIGHT AND FOCUSED — see R_GLOCK for why semiB 18 is the whole
// patch. Everything else here serves "focused":
//
//   cents 0.0   Nothing beats against anything. Beating is movement, and
//               movement is the opposite of focus. This is the only patch in
//               the library with the detune switched fully off.
//   res 0.75    Essentially flat. A resonant peak is a COLOUR, and silver is
//               the absence of colour — the brightness has to come from the
//               cutoff being high, never from emphasis.
//   verb 0.28   Low. Reverb is diffusion, and a diffuse glockenspiel is a
//               wash rather than a struck bar.
//
// fPeak 26 -> fBase 14 over 90 ms is the mallet: a bright flash that settles
// almost immediately, doing the job epiano's oscB gate does but through the
// filter, so the two patches share no mechanism.
{ "glock",   WAVEFORM_TRIANGLE,           WAVEFORM_TRIANGLE,           17,  0.0, 0.00,
     1,  1100,  200, 0.72,   26.0, 14.0,  200, 0.75,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_GLOCK),
    0.00, 0.28, 0.00,   0,   0.00,   ART_STRUCK },

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

// cents 11 -> 5, res 1.20 -> 1.00.
//
// The detune was the roughness. Two saws c cents apart have their nth harmonic
// beating at n times the fundamental rate, and at 11 cents the 8th harmonic is
// already at 22 Hz — past where beating stops sounding like chorus and starts
// sounding like grain. At 5 cents everything through the 15th harmonic stays
// under 20 Hz. This patch got away with it for a long time only because its
// fBase of 1.5x cut those harmonics off at rest; the wide filter sweep is what
// eventually let them out.
// res 1.00 -> 1.50 and FIXED — see the route table. This is reed's number and
// reed's arrangement: a stationary peak with the cutoff sweeping underneath it.
{ "bowed",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  5.0, 1.00,
    30,  1e9f,  350, 0.50,    1.5,  1.5,  400, 1.50,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 700.0f }, LFO_OFF, NROUTES(R_BOWED),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS },

// ============================ ENSEMBLE =====================================
// Slow in, slow out, and never decaying while held. The long release (900 ms)
// is most of what makes it sound like a section rather than a synth: chords
// overlap each other instead of stopping dead.
// semiB -12 is the whole fix for "tinny": oscB an OCTAVE DOWN at 0.85, present
// the entire time, so every note carries its own bass. Cutoff base comes back
// down to 2.4x as well — v2 sat around 15x the fundamental at ordinary playing
// depth, which passes everything and leaves a single saw sounding like wire.
// 6 cents is now only there to keep oscA and oscB's second harmonic from
// locking dead flat, not to do any of the work.
// oscB SAWTOOTH -> TRIANGLE, and res 0.90 -> 0.75.
//
// The octave-down partner is there for WEIGHT, and a sawtooth pays for that
// weight with a second full set of harmonics falling off at only 6 dB/octave.
// A triangle falls off at 12 and carries odd partials only, so it delivers the
// same body with a fraction of the buzz — which is the single biggest
// sweetness lever available without abandoning the saw that makes it a string.
//
// A side effect worth knowing: a triangle an octave down has partials at
// f/2, 3f/2, 5f/2 and NOTHING at f, so it cannot beat with oscA's fundamental
// at all. The 6 cents of detune is now inert for this pair. That is fine —
// the ensemble was never coming from within a voice, it comes from the
// per-note RANDOM offset across a chord — but it does mean a single held note
// is now perfectly steady.
//
// res 0.75 is essentially neutral (0.7 is flat). Any emphasis at all puts a
// bump on a harmonic, and a bump is the opposite of sweet.
{ "strings", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_TRIANGLE,           -12,  6.0, 0.85,
   240,  1e9f,  900, 0.55,    2.6,  2.1, 1400, 0.75,
    1.00, 0.00, 0.00,
    { 4.6f, LFO_SINE, 450.0f, 800.0f }, LFO_OFF, NROUTES(R_STRINGS),
    0.00, 0.55, 0.22, 300,   0.30,   ART_STRUCK },

// Same oscillators and filter as strings above; only the gesture differs.
// fPeak == fBase because a continuous patch must not carry an attack shape of
// its own — any sweep here is a second envelope competing with the one your
// finger is drawing. No delay either: a 300 ms echo argues with a line you are
// still bowing. Release 600 ms, shorter than struck strings, because the note
// already ends when you lift rather than ringing on.
// Carries the same sweetening as strings: oscB is a TRIANGLE an octave down,
// so the weight arrives without a second sawtooth's worth of buzz, and res
// drops to 0.80 which is very nearly flat.
//
// fBase goes 2.6 -> 1.8 rather than following strings down to 2.1, and the
// reason is the gesture. On a CONTINUOUS patch position drives loudness AND
// brightness, so the bottom of the travel is where the note is quietest and
// purest — it can afford to be darker than a struck patch's resting tone,
// and starting at 1.8x buys back the expressive RANGE that pulling the top
// down costs. 1.8x -> 7.2x is the reed shape: sweet where you begin, open
// where you commit.
{ "strbow",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_TRIANGLE,           -12,  6.0, 0.85,
    30,  1e9f,  600, 0.55,    1.8,  1.8,  400, 0.80,
    1.00, 0.00, 0.00,
    { 4.6f, LFO_SINE, 400.0f, 800.0f }, LFO_OFF, NROUTES(R_STRBOW),
    0.00, 0.55, 0.00,   0,   0.00,   ART_CONTINUOUS },

// fPeak == fBase on purpose: the sweep that matters is the RISING one in the
// route table, and leaving the built-in falling sweep switched on would fight
// it. A little drive adds the harmonics a filter can then bite on.
// semiB -12 at 0.65 is standard synth-brass practice and the fix for "small":
// two NEAR-UNISON saws give width and no weight at all — the same failure that
// made strings v1 thin — so the second oscillator goes an octave down at
// partial level instead. oscA stays the voice; oscB is the chest behind it.
//
// drive stays at 0.20 and should not go much higher. The waveshaper is a BUS
// effect (mixOut -> shaper), applied to all 25 voices summed, so it cannot do
// what real brass does — get brighter as ONE note gets louder, through
// nonlinear steepening inside the bore. Push it and chords crunch instead.
// res 1.60 -> 1.05 and fBase 5.0 -> 6.5. Together with the resonance route
// above this is backing out the "raise Q for power" idea entirely and getting
// brightness the honest way: a higher cutoff, so more partials are simply
// present, rather than a peak making a few of them loud.
{ "brass",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, -12, 11.0, 0.65,
    45,  1e9f,  260, 0.62,    6.5,  6.5,  110, 1.05,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 350.0f, 500.0f }, LFO_OFF, NROUTES(R_BRASS),
    0.20, 0.30, 0.00,   0,   0.00,   ART_STRUCK },

// semiB 24 puts the tine two octaves up; the 6 cents of detune against it is
// what stops the bell sounding like a plain harmonic and starts it shimmering.
// mixB is 0.00 — the tine is entirely the route table's doing.
{ "epiano",  WAVEFORM_SINE,     WAVEFORM_SINE,      24,  6.0, 0.00,
     2,  1500,  420, 0.90,    9.0,  9.0,  190, 0.70,
    1.00, 0.00, 0.00,
    { 5.6f, LFO_SINE, 0.0f, 250.0f }, LFO_OFF, NROUTES(R_EPIANO),
// NO DELAY, and the reason is worth keeping. The bus here is a plain delay
// line, not a chorus: at 260 ms and 0.18 send it puts a clean copy of the note
// a quarter of a second behind itself. On a pad that is ambience; on a patch
// with a 2 ms attack and a bell transient it is a SECOND ATTACK, which is
// exactly how it was heard. Anything percussive wants either no delay or one
// short enough to thicken rather than repeat (under ~40 ms), and reverb is the
// better tool for space at this attack speed.
    0.00, 0.34, 0.00,   0,   0.00,   ART_STRUCK },

// Sits next to epiano on purpose — the pair is a Rhodes and a Wurlitzer, and
// they are only comparable side by side. Everything here differs from the
// patch above in the direction "reed, not tine":
//
//   oscA PULSE, not SINE   a reed is hollow; a tine is nearly a pure partial
//   mixB 0.45 CONSTANT     no gated bell. The Wurli's colour is in oscA, and
//                          the sine underneath is just body, always present
//   semiB 0                unison, not two octaves up — nothing bell-like
//   fBase 5.0, res 1.10    darker and slightly peaky, where epiano is open
//                          and flat. The bark needs somewhere to bark FROM
//   tau 1100 vs 1500       shorter; a Wurli is punchier and less sustaining
//   drive 0.10             the one patch where bus distortion is idiomatic
{ "wurli",   WAVEFORM_BANDLIMIT_PULSE,    WAVEFORM_SINE,                0,  4.0, 0.45,
     2,  1100,  300, 0.80,   11.0,  5.0,  220, 1.10,
    1.00, 0.00, 0.00,
    { 5.4f, LFO_SINE, 0.0f, 200.0f }, LFO_OFF, NROUTES(R_WURLI),
    0.10, 0.30, 0.00,   0,   0.00,   ART_STRUCK },

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
