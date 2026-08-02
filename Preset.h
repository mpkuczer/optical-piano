// ============================================================================
// Preset.h — what a patch is
// ============================================================================
// A Preset is just numbers. Switching preset doesn't change the machinery,
// only the values fed to the 25 voices.
//
// The dedicated modulation fields are gone. There used to be vibCents/vibHz/
// vibDelayMs for vibrato, pwmD/pwmHz for pulse width, and fVel for velocity
// brightness — three features, three code paths, three fixed depths. They are
// now three rows in a route table, and combinations nobody planned for come
// free: keytrack to cutoff, amplitude envelope to cutoff, random to detune.
#pragma once
#include <Arduino.h>
#include "ModMatrix.h"

// How the instrument should be PLAYED, as opposed to how it sounds. This is
// the difference between a keyboard and a theremin, and it is not something
// the oscillator settings can express.
enum GestureMode : uint8_t {
  GESTURE_STRUCK = 0,   // piano, harpsichord: velocity sets the loudness, and
                        //   pressure (if routed) means "past where I landed"
  GESTURE_CONTINUOUS    // theremin, ondes martenot: the key is a position
                        //   sensor. Velocity is ignored entirely and the sound
                        //   is a pure function of where your finger is now.
};

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

  // Filter response blend. The state-variable filter computes all three
  // simultaneously, so this costs nothing but changes everything:
  //   lowpass  — removes highs. Warm, dark, the default synth sound.
  //   bandpass — keeps only around the cutoff. Nasal, hollow, vocal.
  //   highpass — removes lows. Thin, glassy, brittle.
  float lpMix, bpMix, hpMix;

  // ---- modulation --------------------------------------------------------
  LfoConfig       lfo1, lfo2;   // rate, shape, delay before starting, fade-in
  const ModRoute *routes;       // source -> destination -> depth
  uint8_t         nRoutes;

  // ---- bus effects (shared by all voices) --------------------------------
  float drive;          // waveshaper. 0 = clean. Adds harmonics and grit.
  float verb;           // reverb send, 0..1
  float delaySend;      // delay send, 0..1
  float delayMs;        // delay time, ms (see DELAY_MAX_MS)
  float delayFb;        // delay feedback, 0..0.85 — how many repeats

  // Last so that existing preset rows, which do not mention it, default to
  // GESTURE_STRUCK and keep behaving exactly as before.
  uint8_t gesture;
};

extern const Preset PRESET[];
extern const int    PRESET_COUNT;
