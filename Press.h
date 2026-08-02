// ============================================================================
// Press.h — turning key depth into 0..1 pressure
// ============================================================================
// Split out of Scanner.cpp for one reason: it can be tested. Pressure has now
// been wrong three times, and every failure was a unit/range error that a
// desktop assertion would have caught in a second — the same lesson
// ModMatrix.h records. Like ModMatrix, this file has NO Arduino dependencies
// and compiles with plain g++.
//
// THE PROBLEM
//   Depth is a fraction of the key's learned span. A key at rest reads ~0,
//   fully pressed reads ~1.0 (sometimes above, since span is the average
//   press and not the mechanical stop). Pressure is not that number. Pressure
//   is where you sit inside the SMALL region the key still travels after it
//   has already bottomed out — measured at ~0.03 to 0.06 of span on this
//   instrument, i.e. under 200 ADC counts out of 4096.
//
//   So the window has to be learned per key, and learning it means answering
//   one question correctly: which samples belong to the hold, and which
//   belong to the strike or the release? Getting that wrong doesn't produce
//   a subtly poor result. It produces a window covering the whole travel, and
//   pressure that never moves more than a few percent while you lean.
//
// THE RULE
//   A sample counts toward the held window only if it is within HELD_BAND of
//   the deepest point reached so far. When the maximum grows, any minimum
//   that has fallen outside the band is discarded. That single rule excludes
//   the strike ramp (the max keeps growing, so stale minima keep being
//   dropped) and the release ramp (depth falls outside the band, so it is
//   never recorded) without needing to know which phase we are in.
// ============================================================================
#pragma once

struct PressConfig {
  float settleMs;   // ignore everything this soon after the strike
  float heldBand;   // how far below the deepest point still counts as "held"
  float minRange;   // learned windows narrower than this report zero, rather
                    //   than amplifying ADC noise into imaginary expression
  float stillRate;  // depth units per SECOND below which the key counts as
                    //   stationary. The hold is defined by the key having
                    //   stopped, not by how deep it is: at a 1.2 ms scan rate
                    //   the approach is a smooth ramp, and depth alone cannot
                    //   tell "descending through 1.20" from "resting at 1.20".
  float velTauMs;   // smoothing on the velocity estimate
  float tauMs;      // output smoothing time constant. NOT a per-sample
                    //   coefficient: the scan rate varies with build options,
                    //   and a per-sample coefficient silently changes the feel
                    //   when it does.
  float openRate;   // learning rate when the window WIDENS
  float closeRate;  // ...and when it narrows. Deliberately much slower, so a
                    //   few notes played without leaning don't erase it.
};

extern const PressConfig PRESS_DEFAULT;

struct PressState {
  float ref, top;    // learned held window
  bool  learned;
  float noteMin, noteMax;
  float vel;         // depth units per second, smoothed
  float lastDepth;
  bool  haveLast;
  float value;       // smoothed 0..1 output
};

void  pressInit(PressState &s);
void  pressNoteOn(PressState &s);

// Feed one depth sample. ageMs is time since note on. Returns 0..1 pressure.
float pressUpdate(PressState &s, const PressConfig &c,
                  float depth, float ageMs, float dtMs);

// Fold this note's observed window into the learned one.
void  pressNoteOff(PressState &s, const PressConfig &c);

float pressRange(const PressState &s);
