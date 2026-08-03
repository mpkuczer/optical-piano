// ============================================================================
// Press.h — turning key depth into 0..1 pressure
// ============================================================================
// Arduino-free on purpose, like ModMatrix: test_press.cpp exercises it with
// plain g++. Pressure has been wrong several times and every failure was a
// range error visible on a desktop in milliseconds.
//
// THE KEY IDEA
//   How deep a key lands when struck, and how hard you then lean on it, are
//   the SAME physical quantity. So a per-key window of absolute depths cannot
//   separate them: strike a little deeper than usual and the "window" learns
//   that as lean, widening until leaning moves nothing.
//
//   Instead, each NOTE establishes its own zero — the depth at which the key
//   comes to rest after the strike — and only the WIDTH of the lean is
//   learned per key. Pressure is (depth - this note's rest) / learned width.
//   Strike depth then cancels out by construction, and the learned quantity
//   is bounded by heldBand, so it cannot run away.
//
//   "At rest" means the key has stopped moving, not that it is deep. At a
//   1.2 ms scan rate the approach is a smooth ramp and depth alone cannot
//   distinguish "descending through 1.20" from "resting at 1.20".
// ============================================================================
#pragma once

// How depth becomes pressure.
enum PressMode : unsigned char {
  PRESS_RELATIVE = 0,  // struck instruments: each note's own resting depth is
                       //   its zero, so pressure means "past where I landed".
                       //   Has memory by design.
  PRESS_ABSOLUTE       // continuous instruments: pressure is a pure function
                       //   of where the key is RIGHT NOW. No per-note zero, no
                       //   learned width, no memory of the strike at all.
};

struct PressConfig {
  float settleMs;   // ignore everything this soon after the strike
  float heldBand;   // hard cap on the learned width. A lean is small; if we
                    //   ever measure more than this, it was not a lean.
  float minRange;   // widths under this report zero rather than amplifying
                    //   ADC noise into imaginary expression
  float tauMs;      // output smoothing time constant. NOT a per-sample
                    //   coefficient — the scan rate changes with build
                    //   options and that would silently change the feel.
  float learnRate;  // symmetric. Asymmetric rates were tried and ratchet:
                    //   whichever end learns faster wins and the width grows
                    //   without bound.
  float stillRate;  // depth units per SECOND below which the key is "resting"
  float velTauMs;   // smoothing on the velocity estimate
  // --- absolute mode only ---
  float absLo;      // absolute floor, as a minimum
  float absLoFrac;  // ...or this fraction of the learned top, whichever is
                    //   larger. Expressing the floor as a FRACTION is what
                    //   makes the mapping independent of `span`: top and
                    //   floor then scale together and span cancels out.
  float absHi;      // fallback top, used only until a key has been played
  float absTauMs;   // shorter: a continuous instrument must feel immediate
  float absGamma;   // curve on the mapped value. 1.0 is linear, which FEELS
                    //   flat at the top: perceived loudness goes as roughly
                    //   amplitude^0.6, so a linear map spends only 6 dB on
                    //   the whole upper half of the travel. ~1.8 restores an
                    //   audio taper — the same reason volume pedals are not
                    //   linear — and gives leaning in somewhere to go.
};

extern const PressConfig PRESS_DEFAULT;

struct PressState {
  float width;       // learned lean width for this key
  bool  learned;
  bool  borrowed;    // width came from another key, so an untouched key can
                     //   respond on its first note. Replaced outright, never
                     //   averaged into, once this key has its own data.
  float absTop;      // ABSOLUTE mode: learned depth at this key's own bottom.
                     //   Global constants cannot work here — keys bottom out
                     //   in different places, and `span` (which depth is
                     //   divided by) is maintained only by the STRUCK path,
                     //   so a few notes on a piano patch silently rescaled
                     //   every theremin key.
  bool  absLearned;
  float absNoteMax;
  float noteRest;    // THIS note's zero point
  float noteMax;     // deepest point this note reached
  bool  haveRest;
  float vel, lastDepth;
  bool  haveLast;
  float value;       // smoothed 0..1 output
  unsigned char mode;
};

void  pressInit(PressState &s);
void  pressNoteOn(PressState &s);
float pressUpdate(PressState &s, const PressConfig &c,
                  float depth, float ageMs, float dtMs);
void  pressNoteOff(PressState &s, const PressConfig &c);
float pressRange(const PressState &s);
void  pressSeedFrom(PressState &s, float width);
void  pressSetMode(PressState &s, unsigned char mode);
void  pressSeedAbsTop(PressState &s, float top);
