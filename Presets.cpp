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
//   drive, verb, delaySend, delayMs, delayFb,
//   articulation, gesture, fmRatio, atkMs, noise, noiseHp
//
// The last five zero-fill, which is why most rows stop at the articulation:
// gesture NULL means GESTURE_DEFAULT, fmRatio 0 means no phase modulation,
// atkMs 0 means the default SRC_ATTACK window, and noise 0 means no pink noise
// (so noiseHp is then irrelevant). A row that wants a later field has to state
// the earlier ones — nullptr, 0.0f — because this is read positionally.
//
// ---- pink noise ------------------------------------------------------------
// The newest field and the one that changed the most patches. Every acoustic
// instrument this library imitates is part noise — breath through a flute,
// rosin on a bow, air in a pipe before it speaks, felt on a bar — and until
// this existed every one of them was a pure tone pretending. Two oscillators
// and a filter cannot make broadband energy; only a noise source can.
//
// It is used in two distinct ways here and the difference is worth naming:
//
//   A FLOOR (Preset::noise) — air that is simply always there. flute, airy,
//   church, strings. It does not move, and you notice it only by removing it.
//
//   A ROUTE (DST_NOISE) — air that ARRIVES. This is where the expression is:
//   { SRC_ATTACK,   DST_NOISE, d, SRC_VELOCITY }  a chiff, a plectrum, a
//                                                 mallet. Loud, then gone.
//   { SRC_POSITION, DST_NOISE, d }                breath that answers effort:
//                                                 push the key and it rasps.
//   { SRC_KEYVEL,   DST_NOISE, d }                a consonant. Only while the
//                                                 finger is actually moving.
//
// Both are multiplied by the voice's own amplitude in Synth.cpp, so noise never
// outlives its note and never needs its own envelope.
//
// SOME PATCHES DELIBERATELY HAVE NONE. theremin is a pure heterodyne sine and
// noise would be a lie about the instrument; glock's silver comes from having
// nothing but two triangles ringing; dxpiano, fmbell and fmbow build their
// entire spectrum from sidebands and noise would only blur it. Absence is a
// choice here as much as presence is.
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
  // THE CHIFF. A flute player's note does not begin with the tone, it begins
  // with the consonant of the air arriving — and on this instrument that had
  // nowhere to come from, because a sine and a triangle have no transient of
  // their own however sharply you envelope them. The row's noise floor (0.11)
  // is the steady breath; this is the puff on top of it, at the default 90 ms
  // window, so it is over well before the note settles.
  //
  // Via velocity because that is exactly what changes when a player tongues a
  // note harder. It is the one part of a flute that is genuinely percussive.
  { SRC_ATTACK,   DST_NOISE,    0.18f, SRC_VELOCITY },
  // ...and breath that answers effort. Pushed deep the tone does not merely
  // get brighter, it gets AIRIER — which is the difference between a flute
  // being played loudly and a flute being overblown. x^3 keeps it out of the
  // way until the last third, the same place the cutoff route lives.
  { SRC_POSITION, DST_NOISE,    0.09f, SRC_CONST, CURVE_LINEAR, 3.0f },
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
  // AIR THAT IS NOT AUDIBLE AT REST, and it costs nothing to arrange because
  // the filter already does it. The row's noise corner is 2.5x pitch and its
  // fBase is 1.8x, so at the bottom of the travel the voice's own lowpass sits
  // BELOW the noise highpass and the two cancel: there is genuinely nothing
  // there. The 34-semitone cutoff route then opens the filter to ~12x, and the
  // air walks out from under it as the pad swells.
  //
  // That is a better shape than any curve on this route could give, because it
  // is not a taper — it is a threshold, at a depth the player can find, and
  // the same finger movement is already doing it.
  { SRC_POSITION, DST_NOISE,   0.10f, SRC_CONST, CURVE_SQUARE },
};

// Airy: vibrato plus a slow wobble in the second oscillator's level, so the
// breathiness itself drifts.
//
// "Breathiness" was a figure of speech until the row got a noise floor. What
// LFO2 was wobbling was the LEVEL OF A TRIANGLE an octave up, which is a
// perfectly good shimmer and is not breath by any stretch — the patch was
// named for a sound it could not make. Now the same LFO2 wobbles the actual
// air as well, at 0.28 Hz, so the breath swells and falls across a held note
// the way a singer's does. One LFO, two destinations, one gesture.
static const ModRoute R_AIRY[] = {
  VEL_TO_AMP,
  { SRC_LFO1, DST_PITCH, 26.0f },
  { SRC_LFO2, DST_OSCB,   0.10f },
  // Bipolar, so it takes air away as often as it adds it — and the row's floor
  // is 0.24, deep enough that it never clamps at zero and the drift stays
  // smooth instead of flattening out at the bottom of every cycle.
  { SRC_LFO2, DST_NOISE,  0.07f },
  // The breath arriving before the tone does. Smaller than flute's chiff
  // (0.18) because there is so much more air underneath it here that a big
  // transient would not read as an onset, only as a lurch in the floor.
  { SRC_ATTACK, DST_NOISE, 0.14f, SRC_VELOCITY },
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
  // THE QUILL. A harpsichord's plectrum plucks the string and then scrapes
  // past it, and that scrape is the single most recognisable thing about the
  // instrument — it is why a harpsichord is identifiable from one note and a
  // filtered sawtooth is not.
  //
  // NOT via velocity, which is the whole point and the same argument
  // ART_PLUCKED is built on: a plectrum releases the same way however hard the
  // key is pressed. The jack either trips or it does not. Every other struck
  // patch here scales its transient by velocity; this one must not, and that
  // difference is audible immediately when you play the two side by side.
  //
  // The row sets atkMs 30, far shorter than the 90 ms default. A quill is a
  // click, not a chiff — at 90 ms it becomes a breath and the instrument turns
  // into a very odd flute.
  { SRC_ATTACK,   DST_NOISE,   0.22f },
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
// fifth, the classic 2 2/3' drawbar)https://www.facebook.com/, brought in by depth with a square curve
// so it arrives late and deliberately. Hold a chord, lean on one note, and
// that note alone grows a fifth. Gated: no velocity, hard onset, organ
// behaviour — the expression is entirely in where the keys sit.
static const ModRoute R_DRAWBAR[] = {
  VEL_TO_AMP,
  { SRC_CONST,    DST_OSCB,  -0.70f },
  { SRC_POSITION, DST_OSCB,   0.70f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 8.0f },
};

// TRIGGER — the reference patch for ART_TRIGGERED, and the demonstration that
// "how hard" and "how loud" can be separated.
//
// NO VEL_TO_AMP, and that omission is the patch. With nothing routed to
// DST_AMP as a constant, the gain sits at unity for as long as the note is
// latched on — the same level for every note, every velocity. What velocity
// gets instead is the ONSET: SRC_ATTACK is 1.0 at note-on and gone by ~250 ms,
// so `via SRC_VELOCITY` gives a chiff that is loud when you hit the key and
// almost absent when you lean on it, over a body that never moves.
//
// Two routes carry it, because loudness alone reads as a level jump rather
// than an attack. The cutoff route is what makes it a chiff: the note speaks
// bright and then settles into its own colour, which is the shape every real
// pipe, reed and tine has. Depth 0.55 and 10 semitones are hard hits; a soft
// press gets velAtSlow (0.16), so about a tenth of both — audible as an onset,
// nowhere near an accent.
//
// AFTERTOUCH, and the honest state of it under a latch. The pressure route
// here works only while the key is DOWN, which under ART_TRIGGERED means the
// tail of the press that started the note: trigger it, keep holding, lean, and
// the colour moves. Let go and it is over — pressure reads a key, and a latched
// note's key is at rest. Pressing again would toggle the note off rather than
// express it, so there is no second bite. See Gesture.h; a latching patch that
// wants expression on a SOUNDING note has to use SRC_POSITION, which reads at
// any depth including ones too light to reach the trigger.
//
// Depth 10 is also sized for a limit that is not in this patch. updatePressure
// discards its rest point as soon as the key goes more than heldBand (0.10)
// deeper than where it settled — a rule for a hesitant press that has no time
// bound, so it also fires on a deliberate lean, and pressure collapses to zero
// at the moment the player pushes hardest. The usable range is about 0..0.22,
// so 10 semitones of depth is roughly 2 of actual movement: a colour change
// rather than a filter sweep, and small enough that the reset at the top reads
// as a settle rather than a drop. Linear, not curved, for the same reason — a
// square curve over a 0.22 range leaves 5% of the destination, which is nothing
// at all. test_pipeline.cpp pins the behaviour under "KNOWN LIMIT".
static const ModRoute R_TRIGGER[] = {
  { SRC_ATTACK,   DST_AMP,     0.55f, SRC_VELOCITY },
  { SRC_ATTACK,   DST_CUTOFF, 10.0f,  SRC_VELOCITY },
  { SRC_PRESSURE, DST_CUTOFF, 10.0f },
  { SRC_KEYTRACK, DST_CUTOFF, -3.0f },
};

// CHURCH — a pipe organ on a LATCHING keyboard, which is a thing that has
// never existed and is the point.
//
// A pipe has exactly two states, because the key opens a pallet and wind either
// reaches the pipe or does not; how hard you hit the key cannot make it louder,
// and no amount of leaning afterwards can either. Every organ ever built then
// adds a constraint that has nothing to do with the pipe: the pallet is held
// open by a finger, so the music is bounded by ten of them. ART_TRIGGERED keeps
// the first half and throws away the second. Press a key and the pallet stays
// open. Press it again and it shuts.
//
// What that gives you is REGISTRATION AS PERFORMANCE. Twenty-five keys, all
// available at once, and a chord you build a note at a time and then edit while
// your hands are free — add the third, walk away, come back and take out the
// fifth. A sustain pedal cannot do this: it holds everything indiscriminately
// and drops everything at once. An organ's stops cannot either; they act on the
// whole rank, not on the note.
//
// It is NOT a Hammond, and GATED (drawbar, above) is what a Hammond is: a
// tonewheel is spinning whether you play or not, so a key is a pure switch onto
// a signal that already exists. A pipe has to SPEAK. Air arrives at an edge and
// takes a few tens of milliseconds to settle into a standing wave, and on a
// mechanical action how fast the pallet opens genuinely changes that transient
// — the "tracker touch" organists argue about. Not the volume. The speech.
// That is exactly a velocity that may touch SRC_ATTACK and nothing else.
//
// THE STOPS. Two oscillators, so two ranks: an 8' Principal and the 4' Octave
// above it, which is the smallest chorus anyone would call an organ. The
// alternative was the 2 2/3' quint at +19 — drawbar's choice, and the reason
// its sound is hollow rather than round, since a quint reinforces the 3rd
// harmonic while an octave reinforces the evens. Rounder is what a diapason is.
//
// cutoff 4.5x, static. A diapason is not a sawtooth: it has a strong
// fundamental and harmonics that fall away quickly, so most of the saw has to
// go. fPeak == fBase because a pipe's timbre does not evolve — every bit of
// movement in this patch is in the speech and in the room, and that is the
// whole aesthetic. Nothing here decays, nothing sweeps, nothing wobbles on
// purpose except the wind.
static const ModRoute R_CHURCH[] = {
  // NO VEL_TO_AMP. The stop is drawn or it is not.
  //
  // Speech, in three parts, because an attack that is only louder reads as a
  // level jump rather than as a pipe starting.
  { SRC_ATTACK,   DST_AMP,     0.35f, SRC_VELOCITY },
  { SRC_ATTACK,   DST_CUTOFF, 14.0f,  SRC_VELOCITY },
  // ...in FOUR parts now, and this is the one the other three were standing in
  // for. A flue pipe does not begin as a tone at all: air hits the languid, the
  // jet is unstable for a few tens of milliseconds, and only then does it lock
  // to the resonator. Everything an organist means by "speech" is that
  // interval, and it is broadband — a hiss, not a pitch. Louder and cutoff
  // routes were both approximations of a transient they could not produce.
  //
  // 0.24 is the largest noise depth in the library and it should be: for the
  // first 120 ms this is nearly a quarter of the voice. It is also completely
  // gone by the time the chord settles, which is exactly what makes it read as
  // speech rather than as a breathy stop.
  //
  // IT IS 0.24 BECAUSE OF THE HEADROOM, not by ear — 0.30 sounded better and
  // does not fit. This patch stacks three things at the instant of the attack:
  // +0.35 of gain, +0.25 of oscB, and this. Worked through synthNoteOn's
  // arithmetic at full velocity (ampScale 0.55, mixB 0.42, noise floor 0.03):
  //
  //   amp    = 0.55 / (1 + 0.42 + 0.03)     = 0.379
  //   amp   *= (1 + 0.35)                   = 0.512
  //   bLevel = 0.42 + 0.25                  = 0.67
  //   peak   = 0.512 * (1 + 0.67 + nLevel)
  //
  // At 0.30 that is 1.024 — over full scale, so the engine divides the whole
  // voice down and the top of the strike flattens. At 0.24 it is 0.993 and
  // nothing clamps. Recompute it if any of the three routes above moves; the
  // binding constraint is the sum, not this route.
  { SRC_ATTACK,   DST_NOISE,   0.24f, SRC_VELOCITY },
  // ...and this one is NOT via velocity, because it is not about the player.
  // Small pipes speak faster than large ones — less air to set moving — so the
  // 4' rank is already sounding while the 8' is still finding its footing. The
  // octave is briefly too loud and then settles back, every time, however the
  // key was pressed. atkMs 120 is that settling: pipe speech, not a key click.
  { SRC_ATTACK,   DST_OSCB,    0.25f },
  // No two pipes are the same object. A rank is tuned, but tuned by hand and
  // then left to a building that changes temperature, so the octave sits a few
  // cents off the fundamental by a different amount for every note. DST_DETUNE
  // moves oscB only, so this never touches the pitch a chord is built on — it
  // just stops the two ranks phase-locking into one sterile tone.
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
  // WIND. A bellows and a chest full of pipes cannot hold pressure perfectly
  // steady, and a held chord drifts very slightly as a result. 0.32 Hz is far
  // below vibrato — nobody hears it as a rate — and the 2.5 s fade means a
  // short note is dead steady and only the long chords breathe. Both
  // oscillators together, because the whole chest shares one wind supply.
  { SRC_LFO1,     DST_PITCH,   2.5f },
  // Trebles proportionally purer than the bass, which is both what pipe scaling
  // does and what stops the top of the keyboard shrieking — the cutoff is a
  // multiple of the note, so without this every octave up is also brighter.
  // Across this keyboard's two octaves that is 6.0x of the fundamental at the
  // bottom and 3.4x at the top. THIS IS THE KNOB IF THE TREBLE SOUNDS DULL:
  // at 3.4x the top notes pass three harmonics and start reading as a flute
  // stop rather than a principal. Matching drawbar's -4.0 rather than going
  // further, because the correction and the dullness are the same number.
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
  // NO SRC_PRESSURE AND NO SRC_POSITION, and under a latch that is load-bearing
  // rather than a stylistic choice. Both of them describe a KEY, and a latched
  // note's key is lying at rest under nobody's finger, so both report zero for
  // almost the whole life of the note. A patch that routed either one to
  // amplitude or cutoff would sound correct while you held the key and then
  // change the instant you let go — the note would not sustain, it would
  // collapse to whatever the routes say at position zero.
  //
  // So a latching patch has to be built entirely from things that do not need
  // the finger to still be there: constants, the note's own envelopes, LFOs,
  // per-note randomness, keytrack, and the velocity latched at the trigger.
  // Every route above is one of those. It is also exactly right for an organ,
  // which does not respond to a finger leaning on a key that is already down —
  // the constraint and the instrument agree, which is why this is the patch
  // that fits the articulation instead of fighting it.
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
  // BREATH, and on this patch it is the missing half of the story the filter
  // was already telling. reed's identity is a fixed resonant peak with the
  // cutoff sweeping under it — sweet at 1.6x, nasal and pressed at 9x — and a
  // real reed instrument does exactly that AND gets audibly windier as it is
  // pushed. The peak alone reads as a filter opening; the peak plus the air
  // reads as somebody blowing harder.
  //
  // CURVE_AUDIO to match the amplitude route above it, so loudness and
  // breathiness arrive together on the same taper. They are the same physical
  // cause and should not need separate tuning.
  { SRC_POSITION, DST_NOISE,   0.14f, SRC_CONST, CURVE_AUDIO },
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
  // ROSIN. The thing a bow does that an oscillator cannot, and the reason this
  // family never quite convinced: a bowed string is a stick-slip oscillator,
  // and the slipping is broadband noise. It is not a flaw in the tone, it is
  // the tone's mechanism made audible, and it gets louder the harder you press
  // because harder pressing means a more violent release.
  //
  // No floor on the row at all — 0.00 — because a bow at rest is not touching
  // the string. Everything here is the route, and CURVE_SQUARE keeps the
  // bottom of the travel genuinely clean so there is somewhere to come from.
  //
  // The corner is 3.0x pitch against an fBase of 1.5x, so at rest the voice's
  // own lowpass is an octave BELOW the noise and nothing gets out. Press, the
  // 30-semitone cutoff route lifts the lowpass to 8.5x, and the rosin appears
  // — not as a level rising from silence but as a band opening up. That is
  // audibly different, and it is the closer of the two to what a bow does.
  { SRC_POSITION, DST_NOISE,  0.13f, SRC_CONST, CURVE_SQUARE },
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
  // THE BITE AT THE FRONT OF THE BOW STROKE. A section attacking together does
  // not arrive cleanly — twelve bows catch at slightly different instants, and
  // what the ear gets is a short broadband scrape before the pitch is even
  // established. This is that, at the default 90 ms window, via velocity so a
  // gentle entry does not have it.
  //
  // It is doing the job the 240 ms attack cannot. A slow envelope is how you
  // get "section" rather than "synth", and the cost is that the note has no
  // front edge at all — nothing tells you when it started. A noise transient
  // gives the onset back without shortening the envelope, which is the whole
  // reason to have a separate onset window (see SRC_ATTACK in ModMatrix.h).
  { SRC_ATTACK,   DST_NOISE,   0.10f, SRC_VELOCITY },
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
  // bowed's rosin at two thirds the depth, and the reduction is the same
  // argument as everything else that separates these two patches: this is a
  // SECTION. Twelve players' bow noise averages into a wash rather than
  // summing, so a section is proportionally less scratchy than any one of its
  // members — and on this instrument the averaging happens for real, because
  // every held key is a voice with its own noise generator.
  //
  // Which means the depth that sounds right on one note is too much on five.
  // 0.09 is judged on a chord; a single line will sound slightly under-rosined
  // and that is the correct trade.
  { SRC_POSITION, DST_NOISE,   0.09f, SRC_CONST, CURVE_SQUARE },
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
// Velocity is the character of the whole instrument. Lean on it and the bell
// barks; play softly and you get very nearly pure sine. On a real Rhodes that
// is the hammer striking nearer the tine, and it is why the thing answers to
// touch the way it does — the dynamic is TIMBRE, not just level.
//
// THE TOUCH IS NOW CURVED, and that is the difference between "responds to
// velocity" and "sounds like a Rhodes". The real instrument is nearly silent
// in its attack until you commit: a medium note is a soft, round, almost flutey
// thing, and everything people describe as the Rhodes sound — the bark, the
// bell, the grit — lives in the top third of the dynamic range. Linear routes
// spread all of that evenly across the range instead, so a medium note arrived
// with half a bark, half a bell and a bright filter, which is a synthesizer
// imitating a Rhodes rather than one.
//
// So every velocity route below carries an exponent, and they are deliberately
// not the same exponent — each one has its own idea of "hard":
//
//   tine     x^2   present from mid velocity, dominant at the top
//   cutoff   x^2   the note opens as it barks, not before
//   overdrive x^3  nothing at all until you really hit it
//
// Note the tine route reads source-then-via SWAPPED relative to how it used to:
// modEvaluate shapes only the SOURCE, so getting a curve onto velocity means
// velocity has to be the source and filtEnv the multiplier. The product is
// identical, the curve is the point.
static const ModRoute R_EPIANO[] = {
  VEL_TO_AMP,
  { SRC_VELOCITY, DST_OSCB,    0.90f, SRC_FILTENV, CURVE_SQUARE },   // the tine
  { SRC_KEYTRACK, DST_OSCB,    0.20f },  // bell up top, rounder in the bass
  // THE OVERDRIVE, and the reason this patch now sets fmRatio.
  //
  // A Rhodes is a pickup and an amplifier as much as it is a tine, and what it
  // does when you hit it hard is not "get brighter" — it DISTORTS. Nothing
  // subtractive here could do that: the bus waveshaper is the wrong tool (it is
  // shared by all 25 voices, so it responds to how much is sounding rather than
  // to how hard this note was struck — see R_BRASS), and a filter cannot add a
  // harmonic that the oscillator never generated.
  //
  // Phase modulation can. At ratio 1.0 the sidebands land on 2f, 3f, 4f... —
  // a harmonic series growing on the fundamental, which is what happens when
  // you drive a sine into anything nonlinear. So a small index IS a mild
  // overdrive, and the route makes it velocity-cubed and transient. Measured
  // at the instant of the strike:
  //
  //   v 0.2   index 0.003 (2 deg)     nothing. A bare sine carrier, which is
  //                                   the subtractive Rhodes this always was
  //   v 0.5   index 0.043 (26 deg)    one very weak sideband pair, audible as
  //                                   warmth rather than as a partial
  //   v 0.8   index 0.174 (104 deg)   a distinct edge on the strike
  //   v 1.0   index 0.340 (204 deg)   an audible growl, partials to ~5.6f
  //
  // `via SRC_ATTACK` (atkMs 120 on the row) is what keeps it a strike rather
  // than a timbre: the dirt belongs to the loud beginning of the note and is
  // gone while the note is still ringing, exactly as an amplifier stops
  // clipping once the note has decayed out of its headroom.
  //
  // SRC_ATTACK and not SRC_FILTENV on purpose, even though filtEnv is already
  // decaying right there. fTau is the TINE's fade and nothing else should be
  // welded to it — see the note on SRC_ATTACK in ModMatrix.h. Two transients,
  // two time constants, tunable apart: 190 ms of bell, 120 ms of grit.
  { SRC_VELOCITY, DST_FM,      0.34f, SRC_ATTACK,  CURVE_LATE },
  { SRC_VELOCITY, DST_CUTOFF, 12.0f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  // Tremolo, scaled by velocity for the same reason R_TREMOLO is: a bare
  // additive AMP route clamps to silence on soft notes.
  { SRC_LFO1,     DST_AMP,     0.12f, SRC_VELOCITY },
  // THE HAMMER, and a THIRD user of the same 120 ms onset window — alongside
  // the FM overdrive above it. That is fine and it is what the window is for:
  // both events belong to the loud beginning of the note and neither wants
  // fTau's 190 ms of tine.
  //
  // CURVE_SQUARE is doing real work here rather than being a taper. Squaring a
  // decaying exponential HALVES its time constant, so this noise is effectively
  // a 60 ms event inside a 120 ms window — short enough to read as neoprene
  // meeting a tine and not as a breath. The alternative was a fourth time
  // constant on the preset row, and the patch already has three.
  //
  // Small (0.06) because a Rhodes is not a noisy instrument. This is the felt,
  // not the strike; the strike is the tine and the overdrive.
  //
  // AND IT COSTS A QUARTER OF A DECIBEL AT THE TOP, which is worth writing
  // down because the row above documents that range to one decimal place.
  // epiano is the one patch that deliberately LIVES in synthUpdate's headroom
  // clamp at full velocity — peak voice sum 1.542, see the ampScale note —
  // and anything added to the voice there is taken back out of the tone.
  // Measured: at v 1.0 the tone's share goes 1.000 -> 0.972, i.e. -0.25 dB;
  // at v 0.5 nothing clamps and nothing changes. So the documented 9.5 dB
  // opening becomes about 9.25 dB.
  //
  // Accepted rather than compensated. Dropping ampScale to buy the level back
  // would move the whole curve to fix its last quarter dB, and a hammer that
  // steals a hair of tone at exactly the moment it strikes is not a defect —
  // it is what a felt-tipped hammer does. 0.09 was the first try and costs
  // -0.36 dB, which is where it starts being a trade rather than a rounding.
  { SRC_ATTACK,   DST_NOISE,   0.06f, SRC_VELOCITY, CURVE_SQUARE },
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
  // equivalent route is now the same shape at 12 semitones rather than 22 —
  // when this comment was written epiano's was linear and the difference in
  // SHAPE was the argument. It isn't any more: a Rhodes turned out to want the
  // same curve, just less of it. The reed/tine distinction lives in the pulse
  // width route below, which has no counterpart over there at all.
  { SRC_VELOCITY, DST_CUTOFF, 22.0f, SRC_CONST, CURVE_SQUARE },
  { SRC_VELOCITY, DST_PWM,   -0.18f, SRC_CONST, CURVE_SQUARE },
  { SRC_POSITION, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
  // Tremolo is not an effect bolted onto a Wurlitzer, it is wired into the
  // instrument — hence deeper than epiano's 0.12. Via velocity, because a bare
  // additive AMP route clamps to silence on soft notes.
  { SRC_LFO1,     DST_AMP,     0.22f, SRC_VELOCITY },
};

// ============================ FM ===========================================
// The third electric piano, and the first one that is not a subtractive
// impression of an instrument — it is the same MECHANISM the DX7 used.
//
// epiano fakes a tine by gating a second sine two octaves up: the partial is
// there or it is not, and in between it fades. A real tine does something a
// gate cannot do, which is CHANGE ITS SPECTRUM CONTINUOUSLY — bright and
// clangy at the strike, then partial by partial down to something very close
// to a sine, all while the note is still sounding. One decaying FM index does
// exactly that, and nothing subtractive does it at all. Hence the pair sitting
// next to each other in the table: same instrument, two mechanisms, and the
// difference is audible.
//
// Every route here goes to DST_FM, which is unusual and is the point — in an
// FM patch, brightness IS the index, so the destination that would normally be
// the filter cutoff is the modulation depth instead. The filter is static.
//
// fPeak == fBase, so `fTau` no longer describes a filter sweep at all: nothing
// moves the cutoff, and the only thing left reading filtEnv is the FM route.
// It is the INDEX decay time — 300 ms, the tine's own ring — and this is a
// happy accident of filtEnv being a plain exp(-t/fTau) that does not care what
// consumes it.
static const ModRoute R_DXPIANO[] = {
  VEL_TO_AMP,
  // THE WHOLE PATCH. Index falls from its peak to nothing over fTau, which is
  // the timbre sweeping from clang to sine while the note holds. Velocity is
  // what makes it an instrument rather than an effect: hit it hard for the
  // bark, play softly and the index barely rises, so a quiet note is very
  // nearly a pure sine. Same touch-is-timbre argument as R_EPIANO, done at the
  // source instead of downstream of it.
  //
  // SQUARED, and swapped source-for-via so that the curve can land on velocity
  // (modEvaluate shapes only the source). Linear, a mid-velocity note got half
  // the full index: 195 degrees of deviation with the floor added in, which is
  // not "the index barely rises", it is a fully-formed FM bark at half
  // strength. The whole middle of the dynamic range sounded like the loud end
  // played quietly.
  //
  // Squared, v 0.5 contributes a quarter instead — 112 degrees all in, against
  // 360 at full velocity. The top of the range is untouched, so nothing is lost
  // from the sound this patch was built for; what it gains is a soft register
  // underneath it that is a rounded electric piano rather than a small bark.
  { SRC_VELOCITY, DST_FM,   0.55f, SRC_FILTENV, CURVE_SQUARE },
  // A floor, so the sustain is never a bare sine. 0.05 of 600 degrees is about
  // 0.5 radians — one weak sideband pair either side of the carrier, which
  // reads as "wire" rather than as a synthesizer's test tone.
  { SRC_CONST,    DST_FM,   0.05f },
  // NEGATIVE, and this is the correction phase modulation forces you to make.
  // PM keeps the index constant across the keyboard by construction (see
  // Preset.h), so every note gets the same spectrum transposed — the machine
  // sound the brass patch's keytrack route also exists to break. A real Rhodes
  // is the opposite of uniform: the bass strings growl and the top two octaves
  // are nearly pure bell. keytrack is -1..1 from the bottom of the keyboard to
  // the top, so a negative depth takes index away up there and adds it down
  // low, which is the direction the instrument actually goes.
  { SRC_KEYTRACK, DST_FM,  -0.12f },
  // The gesture, and the reason this is worth building on THIS keyboard rather
  // than buying a DX7. Leaning into a held key adds partials to a note that is
  // already sounding — not louder, not filtered, genuinely more harmonics —
  // which no electric piano can do and no filter sweep sounds like. x^3 keeps
  // it out of the way until you commit.
  { SRC_POSITION, DST_FM,   0.30f, SRC_CONST, CURVE_LINEAR, 3.0f },
  { SRC_LFO1,     DST_AMP,  0.12f, SRC_VELOCITY },
  // Tape wow. LFO2 rather than LFO1 because the tremolo above is at 5.6 Hz and
  // this wants to be an order of magnitude slower — a warble sharing the
  // tremolo's rate stops reading as a sagging motor and starts reading as one
  // deeper vibrato. 4 cents is under a twentieth of a semitone: too small to
  // hear as a pitch and just enough to stop the sustain sitting perfectly
  // still, which is the tell that a held FM note is a machine.
  //
  // No `via`, so it is there on every note including the softest. That is the
  // point — a warped capstan does not care how hard you played.
  { SRC_LFO2,     DST_PITCH, 4.0f },
};

// INHARMONIC, and the cleanest demonstration of what the ratio does. 2.76 is
// the free-free bar's second mode, the same number glock is chasing — but
// glock could only APPROXIMATE it, because its partial is an actual oscillator
// playing an actual note and semitones are all it has. 17 semitones (2.670x)
// was as close as that gets, and even then the choice was constrained by what
// interval the ratio spelled: 18 was arithmetically closer and unusable
// because an octave plus a tritone is a sound you cannot unhear.
//
// Here the ratio is a float and the modulator is NEVER HEARD — it only exists
// as sidebands at carrier +- n*2.76, none of which is the modulator's own
// frequency. So the interval argument evaporates entirely, and the number can
// just be right. That is the difference between imitating inharmonicity and
// generating it.
//
// A bell also wants a long, quiet tail under a short clang, which is the same
// two-decay-rates problem epiano solves with a gated oscillator — solved here
// by the index decaying twenty times faster than the amplitude.
static const ModRoute R_FMBELL[] = {
  VEL_TO_AMP,
  { SRC_FILTENV,  DST_FM,     0.45f, SRC_VELOCITY },   // the strike
  { SRC_CONST,    DST_FM,     0.08f },                 // the ring
  // Fixed for the life of the note, so no two strikes have quite the same
  // spectrum. On a harmonic patch SRC_RANDOM goes to detune and you hear it as
  // drift; there is no second oscillator to detune here, and a bell's
  // note-to-note variation is in its overtones anyway.
  { SRC_RANDOM,   DST_FM,     0.03f },
  { SRC_POSITION, DST_FM,     0.25f, SRC_CONST, CURVE_LINEAR, 3.0f },
  // The one route that is not FM. Sidebands land wherever the ratio puts them,
  // including above Nyquist on the top octave, where they fold back down as
  // grit; pinning some of the brightness to an absolute frequency keeps the
  // high notes from reaching for partials this sample rate cannot hold.
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
};

// FM UNDER A CONTINUOUS FINGER, which is the combination this instrument can
// make and a keyboard cannot. dxpiano and fmbell are both ART_STRUCK: their
// index is a decaying envelope, so the sweep from clang to sine happens on the
// note's schedule and the player only chooses how hard it starts. Take the
// strike away and the index has no envelope left to follow — it follows the
// finger, and the timbre stops being something a note DOES and becomes
// something you are DOING, for as long as you hold it.
//
// Nothing subtractive gets close. swell, reed, bowed and strbow all move the
// same gesture onto a filter cutoff, and a filter can only uncover harmonics a
// sawtooth already contains: the spectrum's SHAPE is fixed and you slide a
// window over it. Here the finger changes how many partials EXIST, which is
// audibly a different kind of change — closer to a player pushing an
// instrument harder than to a knob being turned.
//
// The ratio is 2.005 and the 0.005 is doing real work. At exactly 2.0 the
// sidebands land at fc(1 +- 2n), i.e. odd harmonics only — a clarinet, hollow
// and dead still. The n=1 LOWER sideband lands at |1-2| = the fundamental
// itself, on top of the carrier, and the two either reinforce or cancel
// depending on index: that is why a 1:2 patch can lose its fundamental
// entirely just as you lean on it.
//
// Offset the ratio slightly and that collision becomes a BEAT instead of a
// cancellation. 2.005 puts the reflected sideband 0.005 of the fundamental
// away from the carrier — a fixed 8.6 cents, so like any detune in cents it
// beats at a rate proportional to pitch: 0.65 Hz at the bottom key (MIDI 48,
// 131 Hz) and 2.6 Hz at the top (MIDI 72, 523 Hz). Higher orders are detuned by
// n times as much, so the beating gets denser exactly as the index opens up.
// One float buys a chorus that grows with effort, out of a single oscillator.
//
// Retune it as a detune, not as a ratio: 2.0 is dead still and hollow, 2.01 is
// 17 cents and reads as a wide ensemble, past about 2.03 the partials stop
// fusing and it turns into fmbell.
static const ModRoute R_FMBOW[] = {
  // reed's exact pair, and the same derivation: -1.033 is unity plus contOn^1.8
  // (0.150^1.8 = 0.033), so the gain curve crosses ZERO at the moment the note
  // starts rather than switching on at -30 dB, and the whole hysteresis band
  // sits below zero where it clamps silent. Recompute it as contOn^1.8 if that
  // threshold moves.
  { SRC_CONST,    DST_AMP,   -1.033f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },

  // ---- the index, which is this patch's entire timbre ---------------------
  // SHAPED FOR THE KEY, NOT FOR THE MATHS, and this is the part that had to be
  // rewritten after playing it. These keys have a stiff spot at about 0.60:
  // you lean, the spring holds, you push a little harder and it gives way and
  // drops you at ~0.90. So 0.60..0.90 is not a stretch of travel the player
  // controls — it is a step that happens TO them — and any route that puts its
  // steepest slope there converts a mechanical defect into a musical event.
  //
  // The first version did exactly that. A squared position route has its
  // fastest movement at the top, so the pop-through landed 0.63 of an octave of
  // bandwidth in one lurch, on top of a keyvel spike (below) that fired for the
  // same reason. reed and bowed feel smoother across the same key because their
  // brightness route is LINEAR in semitones — no accelerating region for the
  // step to land in.
  //
  // Linear is not enough here though, because an FM index is not a cutoff.
  // Bandwidth goes as 2(index+1)*fm, so equal steps of index are equal steps of
  // BANDWIDTH IN HZ, and the ear hears brightness as a ratio: the same 0.1 of
  // index that doubles the spectrum down low barely widens it up top. Linear
  // index is therefore already front-loaded. What the key needs is more of
  // that, so the two routes below are a deliberately CONCAVE pair —
  //
  //     index(p) = 0.03 + 0.46p - 0.14p^2
  //
  // — a straight line with a squared route subtracted from it. It stays
  // monotone everywhere (measured slope 0.46 falling to 0.18, no reversal) and
  // its slope at the top is under half its slope in the middle. Spectral width
  // in octaves, measured across the two regions, against the patches that
  // already feel right on this key:
  //
  //                    0.15 -> 0.60        0.60 -> 0.90
  //                    you control this    the key does this to you
  //     fmbow             +0.74               +0.25
  //     reed              +1.12               +0.75
  //     bowed             +1.12               +0.75
  //
  // Three times as much timbre lives below the stiff spot as above it, which is
  // the opposite of the arrangement that made it lurch, and the step itself now
  // moves a third of what reed's does. That is deliberate overcorrection rather
  // than parity: adding sidebands is a coarser event than sliding a filter over
  // harmonics that are already sounding, so the same octave of width buys a
  // bigger surprise here than it does there. The step is still in the key —
  // nothing in a patch can remove it — but it now costs about what an ordinary
  // quarter of a lean costs, and FM_SMOOTH_MS spreads what is left over ~120 ms
  // so it arrives as a swell rather than as an edge.
  //
  // The subtracted route is the knob for this. More of it flattens the top
  // further (do not exceed half the linear depth, or the curve turns over and
  // pressing harder starts making it duller); less of it goes back toward the
  // straight line.
  { SRC_CONST,    DST_FM,     0.03f },   // a floor: never a bare test tone
  { SRC_POSITION, DST_FM,     0.46f },   // linear...
  { SRC_POSITION, DST_FM,    -0.14f, SRC_CONST, CURVE_SQUARE },  // ...bent flat
  // THE BOW. A continuous patch has no velocity by construction — there is no
  // strike to measure — so this is the only thing that can bite. SRC_KEYVEL is
  // how fast the key is moving RIGHT NOW, so it adds partials while the finger
  // is travelling and gives them back when it stops: press quickly and the note
  // scrapes in bright then settles, lift and it purifies on the way out, hold
  // still and it is exactly what position says it is. That decay is not an
  // envelope, it is the finger, and it happens as many times per note as you
  // want it to.
  //
  // SQUARED FOR NOISE, not for feel, and this route does not work without it.
  // keyvel is differentiated from the RAW position (Gesture.cpp uses `pos`, not
  // the smoothed copy), so ~0.011 of sensor jitter across a 2.7 ms scan is
  // ~4 position units/s of noise — full scale — and the 30 ms smoother only
  // gets that down to about 0.2 RMS. Linear, that would put 0.05 of index hiss
  // on a perfectly still key. Squaring costs a real press nothing, because a
  // press is at or near full scale anyway, and takes the noise to 0.04 of
  // itself: under 0.01 of index, which is inaudible.
  //
  // AND IT HAS TO GET OUT OF THE WAY OF THE STIFF SPOT, which is why it is two
  // routes rather than one. Squaring makes this fire only on FAST motion, and
  // the fastest thing that happens on this keyboard is not a bow stroke, it is
  // the key giving way at 0.60: 0.30 of travel in ~40 ms is 7.5 position
  // units/s, nearly twice full scale, so it SATURATES this source. No curve can
  // help — a steeper one discriminates against slow motion, and the give-way is
  // not slow. Measured, a single flat route collected its whole depth at the
  // exact instant position jumped: it was not accompanying the lurch, it was
  // most of it. Even at half the original depth it turned the position step's
  // +0.25 octave into +0.57 — worse than the shape fix above had just gained.
  //
  // What DOES separate the two is where on the key they happen. A bow attack is
  // a fast move through the SHALLOW end — you are entering the note — while the
  // give-way only ever happens deep, at 0.60 and below. So the depth is faded
  // out with position: a plain route minus a via-position route is
  //
  //     scrape(p) = 0.16 - 0.14p
  //
  // which is 0.13 at the entry, 0.076 by the stiff spot and 0.034 at 0.90.
  // Measured at full key speed: a deliberate stroke into a new note at 0.20 is
  // worth 79 degrees of extra deviation, +0.59 octave of width, and reads as a
  // scrape; the give-way sees the depth falling from 46 degrees to 20 as it
  // passes through, taking the whole pop-through to +0.35 octave instead of
  // +0.57. It also happens to be the truer bow model — speed matters most as
  // the string is caught, less once it is already speaking.
  { SRC_KEYVEL,   DST_FM,     0.16f, SRC_CONST,    CURVE_SQUARE },
  { SRC_KEYVEL,   DST_FM,    -0.14f, SRC_POSITION, CURVE_SQUARE },
  // Purer up the keyboard. Phase modulation holds the index constant across the
  // keyboard by construction, so without this every note is the same spectrum
  // transposed — the machine sound. Real instruments thin out on top.
  { SRC_KEYTRACK, DST_FM,    -0.10f },
  // Fixed for the life of the note: five keys held together are five slightly
  // different spectra rather than one spectrum five times.
  { SRC_RANDOM,   DST_FM,     0.02f },
  // The tone BREATHES. A slow index wobble is not vibrato and not tremolo —
  // pitch and loudness both stay put and only the brightness moves, which is
  // the one modulation a sustained wind or bowed note really has and the one
  // an oscillator pair cannot produce. Via position, so a note held at the
  // quiet end stays still.
  { SRC_LFO2,     DST_FM,     0.04f, SRC_POSITION },

  // Vibrato steered into the top of the travel, bowed's swapped-source trick:
  // only the SOURCE takes an exponent, so position has to be the source and the
  // LFO the via. Written the obvious way round vibrato is a constant fraction
  // of depth, which is not a playable gesture. At x^3: 1.5 cents at half
  // travel, 5 at three quarters, 12 at the bottom.
  { SRC_POSITION, DST_PITCH, 12.0f, SRC_LFO1, CURVE_LINEAR, 3.0f },

  // NO ROUTE TOUCHES THE FILTER, deliberately, and that is the demonstration.
  // fmbell needs a keytrack route on the cutoff because its 2.76 ratio throws
  // sidebands past Nyquist on the top octave. This one cannot: the keyboard
  // stops at MIDI 72, and the worst case there — full press, full-speed scrape,
  // breathing at its peak, index 0.233 after keytrack has taken its cut —
  // reaches 4.1 kHz. That is under a fifth of Nyquist. There is nothing to fold
  // and nothing to hide, so the filter sits wide open and static and every bit
  // of brightness you hear is the index.
};

// ============================ VOWELS =======================================
// A BANDPASS PEAK IS A FORMANT, and moving one under the finger is the closest
// this instrument gets to speech. reed already has the arrangement — a fixed
// resonant peak with the cutoff sweeping underneath it — but reed's peak is
// 1.60, which colours the tone; at 4.0 the peak stops being colour and becomes
// the thing you hear, and where you put it is a vowel.
//
// The point is that it is PER KEY. A vocoder or a formant filter on a synth is
// one filter after the mix, so a chord gets one vowel. Here every voice has its
// own state-variable filter and its own finger, so a held triad is three
// vowels, moving independently, and there is no other control surface that
// offers that.
//
// Where the peak lands, at fBase 3.0 and 26 semitones of travel, for a note at
// middle C (262 Hz):
//
//     position   0.00    0.50    1.00
//     ratio      3.0x    6.4x   13.5x
//     Hz          786    1665    3529
//
// Which is very nearly the F2 range of the vowels: /u/ around 800, /o/ 1000,
// /a/ 1200, /e/ 2000, /i/ 2300. It walks up that series as you press, so the
// gesture is closing the mouth, and it does it in the right direction.
static const ModRoute R_VOX[] = {
  // reed's pair exactly, and the same derivation — -1.033 is unity plus
  // contOn^1.8 (0.150^1.8 = 0.033), so the gain crosses ZERO at the moment the
  // note starts instead of switching on. Recompute it as contOn^1.8 if the
  // threshold moves.
  { SRC_CONST,    DST_AMP,   -1.033f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },
  // LINEAR, and for once the argument is not a preference. Cutoff depth is in
  // SEMITONES, which are already log frequency, so a linear route is
  // perceptually even across the whole travel — the same reason reed (30) and
  // bowed (40) feel controllable everywhere and R_SWELL's shaped version did
  // not. Formant frequencies are heard as ratios too, so this is the one
  // destination where the units and the ear already agree.
  { SRC_POSITION, DST_CUTOFF, 26.0f },
  // THE CONSONANT. Position says where the vowel is; this says how you got
  // there. Key speed adds a brief 6 semitones on the way in and gives them back
  // the moment the finger stops, so a stabbed key arrives with an edge on it and
  // a slid one does not — the difference between "ta" and "aa", out of one
  // gesture with nothing latched.
  //
  // SQUARED FOR NOISE, exactly as R_FMBOW argues at length: keyvel is
  // differentiated from the RAW position, so ~0.011 of sensor jitter across a
  // 2.7 ms scan is full-scale noise and the smoother only gets it to ~0.2 RMS.
  // Linear that would be 1.2 semitones of hiss on a still key. Squared it is
  // 0.24, which is inaudible, and a real press is near full scale anyway so it
  // costs the gesture nothing.
  { SRC_KEYVEL,   DST_CUTOFF,  6.0f, SRC_CONST, CURVE_SQUARE },
  // ...AND THE CONSONANT IS NOW ACTUALLY A CONSONANT. The route above makes a
  // stabbed key arrive brighter, which is a decent stand-in for "ta" and is
  // still, in the end, only a filter moving. Real consonants are mostly
  // FRICATIVES — the difference between "aa" and "sa" is broadband turbulence
  // at the front of the syllable, and no amount of cutoff produces that from
  // two sawtooths.
  //
  // Same source, same square, same reasoning about keyvel's noise floor, and
  // it belongs on the same gesture: brightness and turbulence are two halves
  // of one articulation and separating them would only be two things to tune.
  //
  // What makes this work rather than sound like a hiss gate is the FILTER. The
  // row is bpMix 0.70 at a static 3.0x with res 4.0, so the noise gets the
  // same sharp resonant band as the tone — it comes out formant-shaped, at the
  // vowel's own frequency, which is exactly what a fricative is. The one place
  // in the library where a bandpass on the noise is not incidental.
  { SRC_KEYVEL,   DST_NOISE,   0.20f, SRC_CONST, CURVE_SQUARE },
};

// ============================ PULSE WIDTH ==================================
// THE SAME GESTURE WITH A DIFFERENT PHYSICAL STORY, and that is the whole
// reason this patch is in the library. swell, reed, bowed, strbow and vox all
// answer "what does pressing do?" with a filter, and a filter has one story:
// something is being uncovered. This one never moves the filter at all.
//
// A square wave is odd harmonics only — 1, 3, 5, 7 — which is a stopped pipe
// and a clarinet, hollow and even and slightly empty in the middle. Narrow the
// pulse and the even harmonics arrive and the spectrum acquires a comb: at
// width w the nth harmonic is nulled whenever n*w is an integer, so a 0.10
// pulse is missing every tenth partial and is otherwise nearly flat out to the
// tenth. That is thin, buzzy and nasal — an oboe, a crumhorn, a bandoneon reed.
//
// So the finger is not opening anything. It is changing the shape of the
// vibrating column, and the ear knows the difference: a filter sweep sounds
// like a knob and this sounds like a different instrument arriving.
//
// The route is a single line, and the numbers it produces (Synth.cpp offsets
// oscB by 0.8x so the two never sit on the same comb):
//
//     position   0.00    0.50    0.75    1.00
//     oscA width 0.50    0.40    0.28    0.10
//     oscB width 0.50    0.42    0.32    0.18
//
// Squared rather than linear because pulse width is not perceptually even:
// 0.50 -> 0.40 barely changes the tone (the even harmonics are still tiny),
// while 0.20 -> 0.10 is the entire nasal half of the effect. The exponent puts
// the inert stretch at the top of the key where the travel is cheap.
static const ModRoute R_CLAROB[] = {
  { SRC_CONST,    DST_AMP,   -1.033f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },
  { SRC_POSITION, DST_PWM,   -0.40f, SRC_CONST, CURVE_LINEAR, 2.0f },
  // NO DST_CUTOFF ROUTE ANYWHERE. Deliberate, and the patch does not work
  // without the omission: the filter is static and wide open at 12x so that
  // every harmonic the pulse width creates or removes actually reaches the
  // output. Put a sweep here and you are back to hearing the filter.
};

// ============================ THE BEND =====================================
// MEEND — the slide between two notes that is the note, rather than a
// decoration on it. A sitar player pulls the string sideways across the fret to
// raise it a whole tone or more; a pedal steel does it with a mechanism; a koto
// does it by pressing behind the bridge. All three are the same idea, and all
// three need the pitch of ONE voice to move while the others hold.
//
// That is the thing a keyboard cannot do at all. Pitch bend on a MIDI keyboard
// is a wheel, and a wheel is monophonic by construction — it bends the whole
// instrument. Here the source is the key's own depth, so the chord you plucked
// stays where it is and the finger you push bends its note out of it.
//
// ART_PLUCKED is what makes it playable: the strike is DISCARDED (velocity is
// hardcoded to 0.5), so the pluck is identical however hard you hit, and
// everything expressive happens afterwards while the finger is still down.
// R_HARPSI is the same articulation with a 11-cent vibrato on the same route;
// this is that route with the depth turned up by a factor of eighteen until it
// stops being a wobble and becomes an interval.
//
// THE EXPONENT IS DOING SOMETHING DIFFERENT HERE, and it is worth being blunt
// about the risk. A pluck LANDS the key at some depth, and whatever that depth
// is, this route is already reading it — so resting depth is now detune:
//
//     position   0.30    0.50    0.70    0.85    1.00
//     cents       5.4    25.0    68.6   122.9   200.0
//
// x^3 keeps the ordinary spread of landing depths under about 25 cents, which
// is a wide-ish honky-tonk chorus and survives a chord. Land at 0.70 and the
// note is two thirds of a semitone sharp, which does not. THIS IS THE KNOB: if
// chords arrive out of tune, raise the exponent rather than cutting the depth —
// x^5 puts 0.70 back at 34 cents and still reaches the full whole tone at the
// bottom of the key, so the interval survives and the noise does not.
static const ModRoute R_MEEND[] = {
  VEL_TO_AMP,
  // 200 cents is a WHOLE TONE, chosen because it is the interval the gesture is
  // named for and because it is the largest one a finger can hold steady — the
  // route is linear in position above the exponent, so anything you can aim at
  // on the key you can aim at in pitch. A semitone (100) is easier to play in
  // tune and much less dramatic; a minor third (300) is a sitar's full pull and
  // needs a key you can lean into without bottoming.
  { SRC_POSITION, DST_PITCH, 200.0f, SRC_CONST, CURVE_LINEAR, 3.0f },
  // Both oscillators move together — DST_PITCH is the common route and
  // DST_DETUNE is the one that separates them — so oscB stays exactly two
  // octaves above oscA through the whole bend and keeps reinforcing the 4th
  // harmonic instead of sliding off it. A bend that detuned the pair would
  // sound like the string coming apart.
  //
  // Same keytrack correction as harpsi, and for the same reason: every cutoff
  // is already a multiple of the note, so without this the top octave gets pitch
  // tracking AND more brightness on top and ends up the thinnest thing here.
  { SRC_KEYTRACK, DST_CUTOFF, -5.0f },
  { SRC_RANDOM,   DST_DETUNE,  3.0f },
  // harpsi's bell, kept: filtEnv runs 1 -> 0 over fTau, so the high partial
  // rings briefly at the pluck and the sine underneath carries the rest. It is
  // via velocity, which under PLUCKED is a constant 0.5 — so this is a fixed
  // half-strength ring, exactly as intended for a plectrum.
  { SRC_FILTENV,  DST_OSCB,    0.30f, SRC_VELOCITY },
  // harpsi's quill, also kept, and at the same depth. It is the same physical
  // instrument as far as the pluck is concerned — a plectrum leaving a string —
  // and the difference between the two patches is what happens AFTER, which is
  // the whole claim these two rows are here to make.
  { SRC_ATTACK,   DST_NOISE,   0.22f },
  // NO VIBRATO ROUTE. harpsi puts an LFO on this same destination and the two
  // cannot coexist: a finger aiming at a whole tone does not want the target
  // moving, and 11 cents of wobble on top of a 200-cent bend is a note that
  // never quite arrives.
};

// ============================ SUB-OCTAVE FM ================================
// EVERY OTHER EXPRESSION PATCH GETS BRIGHTER WHEN YOU PRESS. That is not a
// stylistic rut, it is arithmetic: a filter can only uncover harmonics the
// oscillator already contains, and they are all ABOVE the fundamental, so
// "more" and "higher" are the same direction. swell, reed, bowed, strbow, vox
// and clarob all sound like effort because they all sound like brightness.
//
// fmRatio 0.5 breaks that. The modulator is an octave BELOW the carrier, so
// sidebands land at carrier +- n*(fc/2), i.e. at 0.5f, 1.5f, 2.5f, 3.5f — the
// odd harmonics of a note an octave down. Opening the index therefore adds
// content BENEATH the note as fast as above it, and the ear reads that as the
// note getting bigger and rougher rather than brighter. Nothing subtractive can
// produce it, because there is nothing down there to uncover.
//
// THE ONE THING TO KNOW ABOUT THIS RATIO: the n = -2 sideband lands at
// fc(1 - 2*0.5) = 0 Hz. That is DC, with amplitude J2(beta), and J2 peaks at
// 0.486 around beta = 3.05 — squarely inside this patch's range, because
// phaseModulation(600) makes index 1.0 worth 10.5 radians and the route below
// tops out at index 0.33, i.e. beta 3.46, where J2 is still about 0.47. So the
// patch sits at very nearly the worst case for its whole upper half. DC is not
// a sound; it is headroom being eaten, and with five keys held it is a third of
// full scale gone before the note starts. Hence ampScale 0.50 on the row, which
// is low for a patch with no second oscillator.
//
// If it clips or thumps, the fix is fmbow's: OFFSET THE RATIO. 0.505 moves the
// DC term to 0.005 of the fundamental — 0.65 Hz at the bottom key, 2.6 Hz at
// the top — so it becomes a slow amplitude growl instead of an offset, which on
// this patch of all patches is arguably an improvement rather than a repair.
// Retune it as a detune and not as a ratio, exactly as R_FMBOW says.
//
// fmRatio 3.0 is the counterpart if you want a pair: sidebands at 2f, 4f, 6f
// and nothing added below, which is reedy and nasal and gets thinner as you
// press. Same route table, one field on the row.
static const ModRoute R_GROWL[] = {
  { SRC_CONST,    DST_AMP,   -1.033f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },
  // fmbow's concave pair, and the reasoning transfers unchanged: bandwidth goes
  // as 2(index+1)*fm, so equal steps of index are equal steps of bandwidth in
  // HZ while the ear hears brightness as a ratio — linear index is already
  // front-loaded, and these keys need more of that because of the stiff spot at
  // 0.60 that drops you at ~0.90 whether you meant it or not.
  //
  //     index(p) = 0.02 + 0.44p - 0.13p^2
  //
  // Monotone throughout (slope 0.44 falling to 0.18) and its slope at the top
  // is under half its slope in the middle, so the give-way costs about a
  // quarter of a lean instead of a lurch. Do not let the squared route exceed
  // half the linear one or the curve turns over and pressing harder makes it
  // duller.
  { SRC_CONST,    DST_FM,     0.02f },
  { SRC_POSITION, DST_FM,     0.44f },
  { SRC_POSITION, DST_FM,    -0.13f, SRC_CONST, CURVE_SQUARE },
  // Purer up the keyboard. Phase modulation holds the index constant across the
  // keyboard by construction, so without this every note is one spectrum
  // transposed, which is the machine sound. It matters less here than on fmbow
  // — the sidebands go DOWN, so Nyquist is not the constraint — and is still
  // worth having because a growl on the top octave should be a smaller animal.
  { SRC_KEYTRACK, DST_FM,    -0.10f },
  // Five keys held together are five slightly different spectra rather than one
  // spectrum five times. There is no second oscillator to detune on an FM
  // patch, so the per-note variation has to live in the overtones.
  { SRC_RANDOM,   DST_FM,     0.02f },
};

// ============================ STEP DEPTH ===================================
// A SAMPLE-AND-HOLD ON THE CUTOFF is the oldest trick in the modular book and
// the commented-out R_STEPPED above is the ordinary version of it: an LFO at a
// fixed depth, so every note burbles by the same amount and the patch is a
// setting rather than a gesture.
//
// The interesting field is `via`. Routing the depth through SRC_POSITION makes
// the amount of sequencer a PER-KEY, PER-INSTANT quantity: a key held lightly
// is a steady bandpassed choir tone, the same key pushed is the same tone being
// chopped at 6 Hz, and a chord can be half and half. That is not a thing a
// modular does, because the S&H is one module feeding one filter — it needs one
// LFO and one filter per key, which is exactly what 25 voices already are.
//
// The other half of it is that the finger is doing TWO things at once with
// different shapes: loudness rises on an audio taper while the burble depth
// rises linearly on top of it, so leaning in makes a note louder AND less
// stable, which is what a strained voice does.
static const ModRoute R_STEPCHOIR[] = {
  { SRC_CONST,    DST_AMP,   -1.033f },
  { SRC_POSITION, DST_AMP,    1.00f, SRC_CONST, CURVE_AUDIO },
  // LFO2 is bipolar, so 12 semitones is +-1 octave: the peak steps around
  // fBase 4.0x, between 2.0x and 8.0x of the note's own pitch, landing on a new
  // value every 167 ms. On a resonant bandpass those steps are heard as PITCHES
  // — the peak is loud enough to sing — so this reads as a second, random,
  // stepped voice inside the note rather than as a filter being wobbled.
  //
  // `via` is multiplied in RAW and is deliberately not shaped (see
  // modEvaluate), so depth grows linearly with position and there is no way to
  // bloom it at the top. Linear is right here anyway: the first hint of burble
  // should arrive as soon as the note is properly speaking.
  { SRC_LFO2,     DST_CUTOFF, 12.0f, SRC_POSITION },
  // NO PLAIN POSITION -> CUTOFF ROUTE, which is what keeps the two ideas
  // separate. Add one and pressing would raise the average brightness as well
  // as the step depth, and the patch would stop being "how much sequencer" and
  // go back to being another filter sweep with an effect on it.
};

// ============================ DRONE ========================================
// FIVE KEYS, HANDS FREE. ART_TRIGGERED latches: the press turns a note on, the
// key comes back up under nobody's finger, and the next press turns it off.
// church uses it to be an organ; this uses it to be a tanpura, which is the
// other thing a latch is for — a chord that is not an event but a CONDITION,
// held indefinitely while both hands do something else to it.
//
// THE DELAY IS NOT AN ECHO. At 18.18 ms with 0.80 feedback it is a comb filter:
// resonant peaks every 1/0.01818 = 55 Hz, which is A1, so the whole instrument
// is ringing on a fixed harmonic series of A. Play in A and the drone locks into
// the room; play in E flat and it fights, audibly, which is the point — a
// tanpura is tuned to the raga and so is this. Retune it as a PITCH: delayMs =
// 1000 / f, so 16.35 ms is C, 18.18 is A, 20.41 is G.
//
// It is a BUS effect, shared by all 25 voices, which for once is exactly right.
// A tanpura's sympathy comes from one resonating body that every string feeds,
// not from per-string reverb, and a comb after the mix is that body.
//
// EXPRESSION UNDER A LATCH IS SRC_POSITION AND NOT SRC_PRESSURE, and Gesture.h
// spells out why: pressure is only updated while the key is past onPos, and on
// a sounding latched note crossing onPos is precisely what switches it OFF.
// Position is updated in every phase, so it reads a finger resting on a key too
// lightly to trigger anything. The usable window is therefore 0 .. onPos
// (0.150 .. 0.400 in practice), and everything below the trigger is aftertouch
// that cannot possibly turn a note off.
static const ModRoute R_TANPURA[] = {
  // NO VEL_TO_AMP, no DST_AMP route at all. A drone is drawn or it is not, and
  // tau 1e9 on the row means a latched note sits at exactly one level forever.
  //
  // WIDE, and this is the number the patch is built on. 18 cents is far past
  // where two oscillators inside ONE voice would sound like chorus — R_STRINGS
  // v1 failed at exactly that value — but this is SRC_RANDOM, fixed for the life
  // of the note and different for every key. Five latched notes are five
  // stationary offsets spread over +-18 cents, so nothing beats within a voice
  // and the chord as a whole shimmers. That distinction is the whole lesson of
  // the strings rewrite, applied deliberately instead of accidentally.
  { SRC_RANDOM,   DST_DETUNE, 18.0f },
  // THE AFTERTOUCH. Rest on a sounding key and the octave rank swells in; take
  // the finger off and it settles back. mixB is 0.30 on the row, so this walks
  // the pair from 0.30 to about 0.65 without ever touching level or pitch —
  // partials coming in and out of a chord that is otherwise completely static.
  //
  // CURVE_SQRT, and the curve is forced by the window rather than chosen. Only
  // 0..0.40 of travel is reachable before the note toggles, so a linear route
  // would deliver 40% of its depth and an exponent would deliver almost none.
  // Square root puts the resolution at the light end, which is where the finger
  // actually is: 0.17 at a tenth of travel, 0.28 at a quarter, 0.35 at the
  // trigger point. Depth 0.55 is sized so the top of that window is the top of
  // the effect — press further and the note stops, and it should not also have
  // been getting louder on the way.
  //
  // DST_FM is the other end of this route and needs only a non-zero fmRatio on
  // the row: an index under a resting finger brings inharmonic partials in and
  // out of a static chord, which is closer to what a jawari bridge does than an
  // octave rank is. Left off here because the comb is already doing the
  // spectral work and two of them argue.
  { SRC_POSITION, DST_OSCB,    0.55f, SRC_CONST, CURVE_SQRT },
  // Trebles purer than the bass, matching church and drawbar. The cutoff is a
  // multiple of the note, so without this every octave up is brighter too, and
  // a five-note drone stacked into a comb with 0.80 feedback is the last place
  // that wants extra top.
  { SRC_KEYTRACK, DST_CUTOFF, -4.0f },
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

{ "reed",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SQUARE,    12,  5.0, 0.55,
    40,  1e9f,  250, 0.65,    1.6,  1.6,  400, 1.60,
    1.00, 0.00, 0.00,
// noise 0.04 at 2.0x pitch. The floor is small on purpose: at rest this patch
// is meant to be its sweetest, and fBase 1.6x is below the noise corner, so
// almost none of that floor gets through until the cutoff route opens things
// up. The air is in the ROUTE here, not in the constant.
    { 5.6f, LFO_SINE, 300.0f, 500.0f }, LFO_OFF, NROUTES(R_REED),
    0.05, 0.35, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.0f, 0.0f,
    0.04f, 2.0f },

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
// noise 0.00 at 3.0x pitch — a corner with no floor under it. Every bit of the
// air on this patch comes from the position route; see R_BOWED. 3.0x is high
// because rosin is not breath: it sits ABOVE the tone as a hiss rather than
// around it, and dropping this toward 1.5 turns a violin back into a flute.
    { 5.0f, LFO_SINE, 400.0f, 700.0f }, LFO_OFF, NROUTES(R_BOWED),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.0f, 0.0f,
    0.00f, 3.0f },

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
// noise 0.04 at 2.0x pitch. fBase is 2.1x, so unlike bowed and strbow this one
// IS audible at rest — barely, and that is right for a struck section: the
// bows are already on the strings for the whole of a held chord, so the air
// should not need to be summoned. Small enough that five voices' worth summed
// is still a texture and not a hiss.
    { 4.6f, LFO_SINE, 450.0f, 800.0f }, LFO_OFF, NROUTES(R_STRINGS),
    0.00, 0.55, 0.22, 300,   0.30,   ART_STRUCK, nullptr, 0.0f, 0.0f,
    0.04f, 2.0f },

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
// noise 0.00 at 3.0x pitch, exactly bowed's arrangement — fBase is 1.8x here
// so the same "silent until the filter opens" threshold applies, a little
// earlier in the travel because this patch starts slightly brighter.
    { 4.6f, LFO_SINE, 400.0f, 800.0f }, LFO_OFF, NROUTES(R_STRBOW),
    0.00, 0.55, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.0f, 0.0f,
    0.00f, 3.0f },

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
// { "brass",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, -12, 11.0, 0.65,
//     45,  1e9f,  260, 0.62,    6.5,  6.5,  110, 1.05,
//     1.00, 0.00, 0.00,
//     { 5.0f, LFO_SINE, 350.0f, 500.0f }, LFO_OFF, NROUTES(R_BRASS),
//     0.20, 0.30, 0.00,   0,   0.00,   ART_STRUCK },

// semiB 24 puts the tine two octaves up; the 6 cents of detune against it is
// what stops the bell sounding like a plain harmonic and starts it shimmering.
// mixB is 0.00 — the tine is entirely the route table's doing.
//
// a 2 -> 11 ms, AND THIS IS THE "SILKY" ONE. A 2 ms attack on a sine is not an
// attack, it is an edge: at 262 Hz the envelope is fully open in half a cycle,
// so the note begins with a step and the ear hears the step, not the tine. Real
// hammers are tipped with neoprene and take a few milliseconds to give up their
// energy. 11 ms is about three cycles at middle C — still unmistakably struck,
// with nothing clicking at the front of it.
//
// It costs nothing on hard hits because the bark was never in the envelope
// edge. It is in the tine and the overdrive, and both of those are velocity
// routes that 11 ms does not touch.
//
// ampScale 0.90 -> 0.72, which is not a loudness trim but a DYNAMIC RANGE one.
// synthUpdate normalises a voice whose oscillator pair would exceed full scale
// (`peak = a * (1 + bLevel); if (peak > 1) a /= peak`), and at 0.90 with the
// tine at full level every hard note landed in that clamp: the body was scaled
// back to 1/(1+bLevel) no matter how hard it was struck, so the top of the
// range flattened out at about 7.6 dB above a mid note. The clamp is doing its
// job — it is the mixer's headroom — but the patch was living in it.
//
// At 0.72 the clamp is reached only at full velocity, so the top is unchanged
// and everything below it comes down: measured, v0.5-to-v1.0 opens from 7.6 dB
// to 9.5 dB. That is where "light strike unless you hit hard" actually comes
// from — the hard notes did not get louder, the soft ones got softer.


// THE ROOM IS PART OF THE INSTRUMENT. verb 0.75 is the wettest patch in the
// library by a distance, and it is not an effect here — an organ is voiced FOR
// a building, and dry pipe tone is a sound organists never hear. The delay is
// the nave rather than an echo: at 300 ms and only 0.15 send it is inaudible
// under a sustained chord and becomes obvious at the moment the chord stops,
// which is what a big stone room does.
//
// a 18 / r 130. Neither is a synth envelope. 18 ms is the pallet opening: fast
// enough to feel keyed, slow enough that the note is not a click. 130 ms is the
// pallet closing, and the reverb carries everything after it — cut this shorter
// and released chords snap off in a way no room does.
//
// r is a MIDPOINT, not a maximum, and under a latch that matters more than
// usual. The release is scaled by how fast the second press went down (0.35x to
// 1.6x), so the pallet shuts in 47 ms if you jab the key and 208 ms if you lean
// on it. That is the only expressive gesture left at the end of a latched note
// — there is no key release to time — and it is worth learning to use.
// tau 1e9: for as long as the note is latched on, the level does not move.
{ "church",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, 12,  3.0, 0.42,
    18,  1e9f,  130, 0.55,    4.5,  4.5,  400, 0.75,
    1.00, 0.00, 0.00,
// noise 0.03 at 2.5x pitch, and the constant is WIND rather than speech — the
// speech is the SRC_ATTACK route. An organ is a room-sized machine with a
// blower running the whole time it is switched on, and a chest under pressure
// leaks: the noise floor is part of what tells you this is a real instrument
// in a real building rather than a recording of one stop.
//
// 0.03 is deliberately at the bottom of the useful range because this patch
// latches. Five held notes never decay, so whatever is in the floor is in the
// floor indefinitely and it sums across every voice — the one place in the
// library where a noise constant genuinely accumulates. 2.5x keeps it out of
// the bass, where five voices of it would be mud rather than air.
    { 0.32f, LFO_SINE, 0.0f, 2500.0f }, LFO_OFF, NROUTES(R_CHURCH),
    0.00, 0.75, 0.15, 300,   0.30,   ART_TRIGGERED, nullptr, 0.0f, 120.0f,
    0.03f, 2.5f },

// ============================ FLUTE FAMILY =================================
{ "flute",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
    1.00, 0.00, 0.00,
// noise 0.11 at 2.0x pitch, and this is the patch the whole feature was built
// for. A flute is roughly half air by ear — the breath is not an artefact of
// the recording, it is how you know a flute from an organ stop — and this row
// was two of the purest waveforms available with nothing broadband anywhere.
//
// 2.0x .. fBase 6.0x is the band, and it is a real flute's: below the second
// harmonic there is nothing but rumble, above the sixth the ear stops reading
// it as breath and starts reading it as hiss. Both ends track the note, so the
// low register is not muddy and the top is not thin.
    { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE),
    0.00, 0.40, 0.00,   0,   0.00 ,   ART_STRUCK, nullptr, 0.0f, 0.0f,
    0.11f, 2.0f },

// Identical in every field except the route table. Sits next to the one above
// on purpose: 'n' and 'p' flip between them without touching anything else,
// which is the only honest way to judge a gesture.
// { "flutepr", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
//     90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
//     1.00, 0.00, 0.00,
//     { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE_PRESS),
//     0.00, 0.40, 0.00,   0,   0.00 ,   ART_STRUCK },

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
// noise 0.05 at 2.5x pitch. A small floor and a corner deliberately ABOVE
// fBase (1.8x) — see the DST_NOISE route, which is the whole point: the pad
// has no air at all until the filter opens past the noise corner, and then it
// does. Set noiseHp below 1.8 if you want the air present from the start.
    { 4.6f, LFO_SINE, 200.0f, 600.0f }, LFO_OFF, NROUTES(R_SWELL),
    0.00, 0.42, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.0f, 0.0f,
    0.05f, 2.5f },

{ "airy",    WAVEFORM_SINE,     WAVEFORM_TRIANGLE,  12,  6.0, 0.18,
   110,  1e9f,  300, 0.85,    8.0,  8.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.2f, LFO_SINE, 300.0f, 400.0f },
// noise 0.24 at 1.5x pitch — the deepest floor in the library by a factor of
// two, and the patch that earns it. Everything else here uses noise as a
// texture ON a tone; this is the one where the air is a voice of its own, so
// the corner comes down to 1.5x (flute is at 2.0x) to let it start just above
// the fundamental rather than well clear of it. With fBase at 8.0x that is a
// band over two and a half octaves wide.
    { 0.28f, LFO_TRIANGLE, 0.0f, 800.0f }, NROUTES(R_AIRY),
    0.00, 0.45, 0.20, 260,   0.30 ,   ART_STRUCK, nullptr, 0.0f, 0.0f,
    0.24f, 1.5f },

// ============================ CIRCUS 2 =====================================
// { "circus",  WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SQUARE,     0,  8.8, 1.00,
//      4,  1e9f,   31, 0.50,    2.0,  2.0,  500, 1.60,
//     0.00, 0.00, 1.00,
//     { 5.84f, LFO_SINE, 120.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
//     0.06, 0.20, 0.58, 250,   0.71 ,   ART_STRUCK },

// { "circusb", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  6.0, 0.35,
//      8,  1e9f,   60, 0.85,    9.0,  9.0,  400, 0.70,
//     1.00, 0.00, 0.00,
//     { 5.84f, LFO_SINE, 90.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
//     0.00, 0.30, 0.45, 250,   0.68 ,   ART_STRUCK },

// ============================ ANALOG-ISH ===================================
// { "drift",   WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  7.0, 1.00,
//      4,   900,  320, 0.50,    9.0,  5.0,  500, 0.80,
//     1.00, 0.00, 0.00,
//     LFO_OFF, LFO_OFF, NROUTES(R_DRIFT),
//     0.00, 0.25, 0.00,   0,   0.00 ,   ART_STRUCK },

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

// { "fatpluck",WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0, 13.0, 1.00,
//      2,   380,  140, 0.42,   24.0,  3.0,  180, 1.60,
//     1.00, 0.00, 0.00,
//     LFO_OFF, LFO_OFF, NROUTES(R_PLUCK),
//     0.10, 0.20, 0.00,   0,   0.00 ,   ART_STRUCK },

// ============================ NEW MOTION ===================================
// Tremolo — volume wobble. Only possible now the matrix exists.
// { "tremolo", WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  6.0, 1.00,
//     20,  1e9f,  300, 0.55,    8.0,  8.0,  400, 0.80,
//     1.00, 0.00, 0.00,
//     LFO_OFF, { 5.5f, LFO_SINE, 150.0f, 400.0f }, NROUTES(R_TREMOLO),
//     0.00, 0.30, 0.00,   0,   0.00 ,   ART_STRUCK },

// Sample-and-hold stepping the filter. Burbling and mechanical.
// { "stepped", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  8.0, 1.00,
//      4,  1e9f,  200, 0.45,    6.0,  6.0,  400, 1.80,
//     1.00, 0.00, 0.00,
//     LFO_OFF, { 8.0f, LFO_SAMPLE_HOLD, 0.0f, 0.0f }, NROUTES(R_STEPPED),
//     0.05, 0.30, 0.35, 200,   0.40 ,   ART_STRUCK },

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
// atkMs 30, noise 0.00 at 5.0x pitch. No floor — a plucked string is silent
// between plucks and the quill only exists at the moment the jack trips.
//
// 5.0x is the highest corner in the library and it is what makes this a CLICK
// rather than a puff. Below about 4x the transient starts to have a body to it
// and reads as breath; above it, there is nothing left but the top edge of the
// scrape, which is what the ear identifies. fBase 7.0x leaves it a band of
// only about half an octave to live in, and that narrowness is the sound.
    { 5.2f, LFO_SINE, 150.0f, 400.0f }, LFO_OFF, NROUTES(R_HARPSI),
    0.06, 0.38, 0.00,   0,   0.00,   ART_PLUCKED, nullptr, 0.0f, 30.0f,
    0.00f, 5.0f },

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
// { "chordtune",WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,  0,  3.0, 0.80,
//     30,  1e9f,  400, 0.55,    7.0,  7.0,  400, 0.75,
//     1.00, 0.00, 0.00,
//     LFO_OFF, LFO_OFF, NROUTES(R_CHORDTUNE),
//     0.00, 0.35, 0.00,   0,   0.00,   ART_PLUCKED },

{ "drawbar", WAVEFORM_SINE,     WAVEFORM_SINE,      19,  0.0, 0.70,
     8,  1e9f,  120, 0.80,   12.0, 12.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRAWBAR),
    0.00, 0.30, 0.00,   0,   0.00,   ART_GATED },

// tau 1e9 and no DST_AMP constant: held notes never decay and never differ in
// level. r 80 is an organ's release — a stop closing, not a string ringing on.
// fPeak == fBase so the ONLY thing that sweeps the filter is the attack route;
// the patch has no envelope of its own competing with it.
// atkMs 70: shorter than the 90 ms default, because this one is a key click
// rather than a pipe speaking. Long enough to hear, short enough that a fast
// repeated note gets a fresh one each time instead of a blur.
// Gesture left at the default: onPos 0.40 is where the rest of the instrument
// triggers, and a switch has no reason to disagree with the hardware about
// where "down" is.
// { "trigger", WAVEFORM_SINE,     WAVEFORM_SINE,      12,  4.0, 0.50,
//      6,  1e9f,   80, 0.55,    8.0,  8.0,  400, 0.80,
//     1.00, 0.00, 0.00,
//     LFO_OFF, LFO_OFF, NROUTES(R_TRIGGER),
//     0.00, 0.30, 0.00,   0,   0.00,   ART_TRIGGERED, nullptr, 0.0f, 70.0f },

{ "epiano",  WAVEFORM_SINE,     WAVEFORM_SINE,      24,  6.0, 0.00,
    11,  1500,  420, 0.72,    9.0,  9.0,  190, 0.70,
    1.00, 0.00, 0.00,
    { 5.6f, LFO_SINE, 0.0f, 250.0f }, LFO_OFF, NROUTES(R_EPIANO),
// NO DELAY, and the reason is worth keeping. The bus here is a plain delay
// line, not a chorus: at 260 ms and 0.18 send it puts a clean copy of the note
// a quarter of a second behind itself. On a pad that is ambience; on a patch
// with a fast attack and a bell transient it is a SECOND ATTACK, which is
// exactly how it was heard. Anything percussive wants either no delay or one
// short enough to thicken rather than repeat (under ~40 ms), and reverb is the
// better tool for space at this attack speed.
//
// fmRatio 1.0 and atkMs 120 are the overdrive, and both are inert until you hit
// the thing hard — see the DST_FM route in R_EPIANO. Ratio 1.0 means the
// sidebands land on harmonics, so this adds grit to the sine rather than the
// clangy inharmonic partials fmbell is made of; drive stays at 0.00 because the
// distortion is now per-note and lives inside the voice.
//
// COSTS TWO MORE OSCILLATORS PER VOICE. A non-zero fmRatio switches oscA off
// and runs the oscF/oscM pair instead, and unlike a patch with no FM at all
// those never idle out. Worth an 'a' check for CPU and block usage — dxpiano
// has always paid this, epiano now does too.
// noise 0.00 at 4.0x pitch. No floor: a Rhodes between notes is silent, and
// between the hammer and the tine there is nothing broadband left. 4.0x
// against an fBase of 9.0x gives the hammer a band roughly an octave wide,
// well above the fundamental — a contact tick rather than a thud. Lower this
// toward 2.0 and it starts to sound like the key mechanism instead of like
// the hammer, which is a real Rhodes noise but not one anybody wants.
    0.00, 0.34, 0.00,   0,   0.00,   ART_STRUCK, nullptr, 1.0f, 120.0f,
    0.00f, 4.0f },

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
// { "wurli",   WAVEFORM_BANDLIMIT_PULSE,    WAVEFORM_SINE,                0,  4.0, 0.45,
//      2,  1100,  300, 0.80,   11.0,  5.0,  220, 1.10,
//     1.00, 0.00, 0.00,
//     { 5.4f, LFO_SINE, 0.0f, 200.0f }, LFO_OFF, NROUTES(R_WURLI),
//     0.10, 0.30, 0.00,   0,   0.00,   ART_STRUCK },

// ============================ FM ===========================================
// Deliberately alongside epiano and wurli, because the comparison is the whole
// point — 'n' and 'p' between the three of them is the demonstration.
//
//   mixB 0.00, cents 0.0, semiB 0   oscB is not merely quiet, it is UNUSED.
//                          Everything you hear is one carrier. epiano needs a
//                          second oscillator for its bell; this makes the same
//                          bell out of the first one's own sidebands.
//   waveA SINE             stated for honesty and ignored: synthNoteOn begins
//                          the carrier as a sine unconditionally, because a
//                          phase-modulated saw is mud rather than timbre — the
//                          sidebands multiply against harmonics that are
//                          already there.
//   fPeak == fBase 16.0    open and STATIC. Nothing sweeps the filter; a
//                          patch whose brightness comes from the index does
//                          not want a second brightness control fighting it.
//                          The lowpass is here to catch folded sidebands.
//   res 0.70               flat. A resonant peak is a formant, and FM's
//                          spectrum is already fully specified.
//   fTau 300               NOT a filter time here — the index decay. See
//                          R_DXPIANO.
// a 2 -> 9, the same argument as epiano's 11 and slightly shorter because this
// patch has no separate bell to announce the note — the index transient is the
// attack, so the envelope only has to stop being a step.
{ "dxpiano", WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.00,
     9,  1600,  420, 0.90,   16.0, 16.0,  300, 0.70,
    1.00, 0.00, 0.00,
    { 5.6f, LFO_SINE, 0.0f, 250.0f },
    // LFO2 is the wow: 0.9 Hz, delayed past the strike and faded in over 700 ms
    // so the attack lands dead in tune and only the sustain drifts. Starting it
    // at note-on would bend the transient, which is the one part of an electric
    // piano the ear checks against its memory of the real thing.
    { 0.9f, LFO_SINE, 300.0f, 700.0f }, NROUTES(R_DXPIANO),
    0.00, 0.32, 0.00,   0,   0.00,   ART_STRUCK, nullptr, 1.0f },

// fmRatio 2.76 against dxpiano's 1.0, and that single field is the entire
// difference between the two patches' character. Everything else here is
// serving the LENGTH rather than the colour: tau 2400 against 1600, r 700
// against 420, verb 0.46 against 0.32. A bell is a bar that nobody damped.
//
// No LFO of any kind. Tremolo is a pickup and an amplifier, and this is
// neither.
{ "fmbell",  WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.00,
     1,  2400,  700, 0.85,   12.0, 12.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_FMBELL),
    0.00, 0.46, 0.00,   0,   0.00,   ART_STRUCK, nullptr, 2.76f },

// The expressive one. Third in the FM group so 'n' and 'p' walk dxpiano ->
// fmbell -> fmbow: same mechanism, struck harmonic, struck inharmonic, then
// the same thing with the envelope taken away and handed to your finger. It is
// also worth A/B-ing against strbow two rows up, which is the identical
// GESTURE driving a filter instead of an index.
//
//   waveB TRIANGLE, semiB -12, mixB 0.30
//                          The body, and the only non-FM sound here. An octave
//                          down it has partials at f/2, 3f/2, 5f/2 and nothing
//                          at f, so it adds weight without landing on the
//                          carrier or on any sideband — strings' argument, and
//                          it matters more here because the 2.005 beat is a
//                          deliberate effect that a second oscillator sitting
//                          on the fundamental would muddy. It also keeps the
//                          note anchored when the index nulls the carrier.
//   cents 0.0              inert for that pair — there is no shared partial to
//                          beat — so it is left at zero rather than pretending.
//                          The chorus in this patch is the RATIO, not detune.
//   fPeak == fBase 20.0    wide open and static: 2.6 kHz at the bottom key and
//                          10.5 kHz at the top, against worst-case partials of
//                          1.6 kHz and 4.1 kHz. It is clear of the sound
//                          everywhere, which is the point — it is a guard
//                          against a route being retuned later, not a voice.
//                          See the last note in R_FMBOW.
//   res 0.70               flat. A resonant peak is a formant, and an FM
//                          spectrum is already fully specified.
//   fTau 400               unused. Nothing routes SRC_FILTENV and the cutoff
//                          does not move; it is here because the field is
//                          positional.
//   a 30                   also unused — synthNoteOn forces a 4 ms attack on
//                          every continuous patch, because any ramp of its own
//                          is a second envelope competing with the one your
//                          finger is drawing.
//   r 420                  the one thing that IS a shape: shorter than strbow's
//                          600 so a released line stops when you stop, long
//                          enough that lifting is a decay rather than a cut.
//   verb 0.50, no delay    an echo argues with a line you are still bowing.
//   LFO2 0.27 Hz           the breathing, on a 1.5 s fade so it arrives after
//                          you have committed to the note rather than with it.
{ "fmbow",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,  -12,  0.0, 0.30,
    30,  1e9f,  420, 0.65,   20.0, 20.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.1f, LFO_SINE, 350.0f, 700.0f },
    { 0.27f, LFO_SINE, 0.0f, 1500.0f }, NROUTES(R_FMBOW),
    0.00, 0.50, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 2.005f },

// Sits directly after fmbow so 'n' walks between the two continuous FM patches
// without anything in between. Same gesture, same mechanism, opposite
// direction: 2.005 adds partials above the note, 0.5 adds them below.
//
//   waveA/B SINE, mixB 0.00, cents 0, semiB 0
//                          PURE FM, with no second oscillator anywhere. fmbow
//                          keeps a triangle an octave down for body; this one
//                          must not, because "the note gets bigger" is the
//                          claim being demonstrated and an octave-down partner
//                          would supply exactly that by ordinary means. The
//                          weight has to come from the index or it proves
//                          nothing. waveA is stated and ignored — synthNoteOn
//                          always begins the carrier as a sine.
//   fPeak == fBase 10.0    open and STATIC, like every FM patch here. The
//                          lowpass is only there to catch folded sidebands; a
//                          patch whose spectrum is fully specified by the index
//                          does not want a second brightness control.
//   res 0.70               flat. A resonant peak is a formant.
//   fTau 400               unused; nothing routes SRC_FILTENV.
//   ampScale 0.50          LOW ON PURPOSE. See the DC note in R_GROWL: the
//                          n = -2 sideband of a 0.5 ratio lands at 0 Hz and
//                          eats headroom that never becomes sound.
//   r 420                  fmbow's release, and for the same reason — long
//                          enough that lifting is a decay, short enough that a
//                          released line stops when you stop.
//   verb 0.40, no delay    an echo argues with a line you are still playing.
{ "growl",   WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.00,
    30,  1e9f,  420, 0.50,   10.0, 10.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_GROWL),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.5f },

// ============================ FORMANTS =====================================
// res 4.00 is the highest in the library by a distance and it is not a colour
// here, it is the instrument — see R_VOX. Everything else on this row serves
// keeping that peak audible and the rest of the tone out of its way:
//
//   two full saws, cents 7  The peak needs harmonics to find. A sine has
//                          nothing to resonate and a triangle falls off at
//                          12 dB/octave, so by 6x the fundamental there is
//                          nothing left for the formant to sit on. 7 cents is
//                          enough to stop the pair phase-locking and low
//                          enough that the 15th harmonic still beats under
//                          20 Hz, which is the strings-v1 lesson.
//   bpMix 0.70 / lpMix 0.30
//                          NOT pure bandpass. At res 4.0 a bandpass alone is a
//                          formant with no voice behind it — thin, and the
//                          fundamental disappears entirely as the peak walks
//                          up. The 30% lowpass is the chest: it keeps the note
//                          itself present while the peak does the vowel.
//   fPeak == fBase 3.0     static, as every continuous patch must be. Any
//                          built-in sweep is a second envelope competing with
//                          the one your finger is drawing.
//   ampScale 0.45          a Q of 4 is roughly 12 dB of gain at the peak, and
//                          it lands on a different harmonic for every note.
//   no LFO at all          a vowel is not a vibrato. Both LFOs are off, which
//                          is unusual for a continuous patch and deliberate:
//                          a wobble on top of a formant reads as a bad choir
//                          patch rather than as speech. The obvious extension
//                          is bowed's swapped-source vibrato,
//                          { SRC_POSITION, DST_PITCH, 14, SRC_LFO1, ..., 3 },
//                          which only arrives in the last third of travel.
{ "vox",     WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,   0,  7.0, 1.00,
    30,  1e9f,  320, 0.45,    3.0,  3.0,  400, 4.00,
    0.30, 0.70, 0.00,
// noise 0.02 at 1.5x pitch. The floor is BREATH — a sung note is never
// perfectly periodic and a voice with no air in it is the definition of a
// vocoder — and it is tiny because the resonant bandpass in front of it has a
// Q of 4.0 and will make far more of it than the number suggests.
//
// 1.5x, low, because a voice's turbulence is at the vowel rather than above it.
// This is the opposite end of the range from harpsi's 5.0x and the contrast is
// most of what distinguishes a fricative from a click.
    LFO_OFF, LFO_OFF, NROUTES(R_VOX),
    0.00, 0.42, 0.00,   0,   0.00,   ART_CONTINUOUS, nullptr, 0.0f, 0.0f,
    0.02f, 1.5f },

// The A/B for the row above, and it belongs next to it: identical articulation,
// identical amplitude routes, and the timbre control moved off the filter
// entirely. 'n' and 'p' between vox and clarob is the demonstration that the
// gesture and the story it tells are separate things.
//
//   two BANDLIMIT_PULSE    the plain PULSE waveform would alias badly here.
//                          A narrow pulse is almost all harmonic content, and
//                          at width 0.10 on the top octave a naive generator
//                          folds most of it back down as grit. PolyBLEP is not
//                          optional for this patch.
//   mixB 0.70, cents 5     oscB is a second pulse at a DIFFERENT width the
//                          whole time (Synth.cpp scales its offset by 0.8), so
//                          the pair's comb nulls never coincide and the
//                          spectrum stays filled in as the widths narrow.
//   fPeak == fBase 12.0    wide open and static. The entire timbre is the
//                          pulse width; a filter here would hide it.
//   res 0.70               flat, for the same reason.
//   ampScale 0.55          two pulses at 0.10 width are peaky — the waveform
//                          spends most of its cycle at one rail — so the sum
//                          needs more headroom than two saws.
{ "clarob",  WAVEFORM_BANDLIMIT_PULSE,    WAVEFORM_BANDLIMIT_PULSE,      0,  5.0, 0.70,
    30,  1e9f,  300, 0.55,   12.0, 12.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_CLAROB),
    0.00, 0.40, 0.00,   0,   0.00,   ART_CONTINUOUS },

// LFO2 IS THE PATCH. 6 Hz sample-and-hold, no delay and no fade — a new random
// value every 167 ms from the instant the note starts, because the burble is
// not an ornament that should arrive politely after the onset the way a
// vibrato does. lfoValue derives S&H by hashing the cycle number and every
// voice gets its own phase offset, so five keys held together step at the same
// rate and never in lockstep.
//
//   res 2.20               below vox's 4.0 on purpose. The peak has to sing
//                          enough that the steps read as pitches, and at 4.0
//                          each step would be a whistle rather than a note.
//   bpMix 0.85             more bandpass than vox because there is less to
//                          keep steady — the interest is the movement.
//   fPeak == fBase 4.0     the CENTRE the steps land around; LFO2 is bipolar,
//                          so +-12 semitones is 2.0x .. 8.0x.
//   cents 8, mixB 1.00     two full saws in near unison. A choir is many
//                          voices, and the S&H supplies the rest.
//   r 380                  long enough that a released chord's last few steps
//                          are still audible on the way out.
{ "stepchoir",WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH,  0,  8.0, 1.00,
    30,  1e9f,  380, 0.50,    4.0,  4.0,  400, 2.20,
    0.15, 0.85, 0.00,
    LFO_OFF, { 6.0f, LFO_SAMPLE_HOLD, 0.0f, 0.0f }, NROUTES(R_STEPCHOIR),
    0.00, 0.45, 0.00,   0,   0.00,   ART_CONTINUOUS },

// HARPSI'S OSCILLATORS, EXACTLY — saw plus a sine two octaves up at 0.40, the
// same 16 -> 7 filter sweep over 150 ms, the same 1600 ms decay. Every field on
// this row is harpsi's, and the only difference between the two patches is one
// route table. That is the claim worth being able to check: 'n' between them
// should sound like the same instrument being played differently, because it
// is.
//
// fBase 7.0 matters more here than it does there. A bent note is a note you are
// still listening to a second after the pluck, and a patch that goes dull the
// moment the transient passes has nothing left to bend.
{ "meend",   WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_SINE,               24,  4.0, 0.40,
     2,  1600,  300, 0.60,   16.0,  7.0,  150, 1.30,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_MEEND),
    0.06, 0.38, 0.00,   0,   0.00,   ART_PLUCKED, nullptr, 0.0f, 30.0f,
    0.00f, 5.0f },

// THE ONLY PATCH IN THE LIBRARY WHOSE EFFECTS ARE THE INSTRUMENT. delaySend
// 0.35 into an 18.18 ms line at 0.80 feedback is not an echo — see R_TANPURA —
// it is a comb tuned to A1, and the dry signal is mostly there to excite it.
//
//   ART_TRIGGERED          hands free. Five presses and the chord stays;
//                          a sixth press on any of those keys takes that note
//                          away again. 'o' is all-notes-off, and on this patch
//                          it is not a panic key, it is a stop button.
//   tau 1e9, no DST_AMP    a latched note sits at one level indefinitely. It
//                          has to: there is no key holding it and no timeout.
//   semiB 12, mixB 0.30    an octave rank at partial level, the same
//                          arrangement church uses, and the destination of the
//                          aftertouch route.
//   drive 0.08             slight, and doing a specific job. The waveshaper is
//                          a BUS effect, so it sits between the voices and the
//                          delay send and adds intermodulation between the five
//                          held notes before the comb ever sees them. That is
//                          how a tanpura's bridge behaves and it is the one
//                          place a bus-wide nonlinearity is the right model.
//                          Do not raise it much: five sustained notes through a
//                          shaper is already the densest input in the library.
//   ampScale 0.45          five latched voices plus 0.80 of feedback is the
//                          most gain anything here accumulates, and none of it
//                          ever decays.
//   fPeak == fBase 6.0     static. Nothing sweeps, by design — the whole patch
//                          is a chord that does not change unless you touch it.
//   r 200                  a MIDPOINT, not a maximum: church's trick, where the
//                          second press's speed scales the release 0.35x..1.6x.
//                          Jab the key and the string is damped in 70 ms, lean
//                          on it and it fades over 320. It is the only ending
//                          gesture a latch has.
{ "tanpura", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, 12,  6.0, 0.30,
    25,  1e9f,  200, 0.45,    6.0,  6.0,  400, 1.20,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_TANPURA),
    0.08, 0.35, 0.35, 18.18f, 0.80,   ART_TRIGGERED },

// ============================ DIAGNOSTIC ===================================
// { "presstest", WAVEFORM_BANDLIMIT_SAWTOOTH, WAVEFORM_BANDLIMIT_SAWTOOTH, 0,  6.0, 1.00,
//      6,  1e9f,  200, 0.55,    2.0,  2.0,  400, 1.20,
//     1.00, 0.00, 0.00,
//     LFO_OFF, LFO_OFF, NROUTES(R_PRESSTEST),
//     0.00, 0.20, 0.00,   0,   0.00 ,   ART_STRUCK },

// ============================ REFERENCE ====================================
// Single oscillator, no detune, static filter, no modulation, no effects.
{ "blocky",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  0.0, 0.00,
     3,   700,  250, 1.00,   60.0, 60.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_BLOCKY),
    0.00, 0.00, 0.00,   0,   0.00 ,   ART_STRUCK },
};

const int PRESET_COUNT = sizeof(PRESET) / sizeof(PRESET[0]);
