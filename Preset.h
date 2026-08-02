// ============================================================================
// Preset.h — what a patch is
// ============================================================================
#pragma once
#include <Arduino.h>

// A Preset is just numbers. Switching preset doesn't change the machinery,
// only the values fed to the 25 voices.
struct Preset {
  const char *name;

  // ---- oscillators -------------------------------------------------------
  short waveA, waveB;   // WAVEFORM_SINE / TRIANGLE / SAWTOOTH / SQUARE / PULSE
  int   semiB;          // osc B pitch offset in semitones (-12 = octave down:
                        //   adds weight without adding brightness)
  float cents;          // detune between the two oscillators. THE RICHNESS
                        //   KNOB. 0 sterile, 5-10 warm, 20+ out of tune.
  float mixB;           // osc B level relative to A, 0..1 (0 = single osc)

  // ---- amplitude ---------------------------------------------------------
  float a;              // attack, ms
  float tau;            // exponential decay time constant, ms.
                        //   1e9 = never decay (organ/pad behaviour)
  float r;              // release, ms — how fast a note dies when you lift
  float ampScale;       // loudness trim

  // ---- filter (per voice) ------------------------------------------------
  // Cutoffs are MULTIPLES OF THE NOTE'S OWN PITCH, so they track the keyboard
  // instead of muffling the bass and thinning the treble.
  float fPeak;          // cutoff ratio at the instant of attack
  float fBase;          // cutoff ratio it settles to
                        //   fPeak == fBase means no sweep, a static filter
  float fTau;           // how fast it sweeps between them, ms
  float res;            // 0.7 neutral, >2 vocal, >4 whistles
  float fVel;           // how much velocity opens the filter — this is what
                        //   makes hard hits BRIGHTER, not merely louder

  // Filter response blend. The state-variable filter computes all three
  // simultaneously, so this costs nothing but changes everything:
  //   lowpass  — removes highs. Warm, dark, the default synth sound.
  //   bandpass — keeps only around the cutoff. Nasal, hollow, vocal.
  //   highpass — removes lows. Thin, glassy, brittle.
  float lpMix, bpMix, hpMix;

  // ---- motion ------------------------------------------------------------
  float pwmD, pwmHz;    // pulse-width modulation depth and rate (PULSE only)

  // Vibrato: a slow wobble in PITCH. Distinct from tremolo (wobble in volume)
  // and from delay (repeats). 5-7 Hz is the natural range — much slower reads
  // as drunk, much faster as a special effect.
  float vibCents;       // depth, cents either side. 15 subtle, 30 singing,
                        //   60+ operatic. 0 = off.
  float vibHz;          // rate
  float vibDelayMs;     // wait this long after the note starts, then fade the
                        //   vibrato in over ~300 ms. Flute and violin players
                        //   do exactly this; starting a note with full vibrato
                        //   sounds mechanical.

  // ---- bus effects (shared by all voices) --------------------------------
  float drive;          // waveshaper. 0 = clean. Adds harmonics and grit.
  float verb;           // reverb send, 0..1
  float delaySend;      // delay send, 0..1
  float delayMs;        // delay time, ms (see DELAY_MAX_MS)
  float delayFb;        // delay feedback, 0..0.9 — how many repeats
};

extern const Preset PRESET[];
extern const int    PRESET_COUNT;
