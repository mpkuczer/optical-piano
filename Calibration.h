// ============================================================================
// Calibration.h — LAYER 1: raw ADC counts -> normalised key position
// ============================================================================
// Arduino-free. Testable with plain g++.
//
// ONE NUMBER PER KEY, MAINTAINED BY EVERYTHING.
//   The single worst bug in this instrument's history was two consumers
//   sharing a calibration variable that only one of them updated: the struck
//   path maintained `span`, the continuous path read it, and playing a piano
//   patch for ten seconds silently rescaled every theremin key. So there is
//   now exactly one learned quantity — where this key bottoms out — and every
//   press updates it, whatever articulation is loaded.
//
// WHAT THIS LAYER DOES NOT DO
//   No notes. No velocity. No taper, gamma or curve. Position is LINEAR in
//   physical travel, because a curve is a musical decision and belongs to the
//   patch, not to the sensor. Layer 1 answers one question: how far down is
//   this key, as a fraction of how far it goes?
// ============================================================================
#pragma once
#include <stdint.h>

struct CalConfig {
  float    baselineTrack;   // EMA rate for the resting level, per sample
  float    baselineGate;    // only track while position is below this
  float    bottomLearn;     // EMA rate for the learned bottom, per note
  uint16_t defaultTravel;   // assumed travel before a key has been played
  uint16_t minTravel;       // below this a key cannot report a press at all
};

extern const CalConfig CAL_DEFAULT;

struct CalKey {
  float    baseline;        // tracked resting level, in counts. Float because
                            //   the EMA is far slower than one count.
  float    bottom;          // learned level at full press, in counts
  bool     learned;         // this key has taught itself
  bool     borrowed;        // travel came from a neighbour, pending its own
  uint16_t raw;             // last reading, for diagnostics only
  float    position;        // last computed 0..1
  float    noteMinRaw;      // deepest raw seen since the last note ended
};

void  calInit(CalKey &k, uint16_t baseline, const CalConfig &c);

// Feed one reading. `idle` means the gesture layer considers this key to be at
// rest, which is the only time the baseline may track. Returns 0..1.
float calUpdate(CalKey &k, const CalConfig &c, uint16_t raw, bool idle);

// Fold this note's deepest point into the learned bottom.
void  calNoteEnded(CalKey &k, const CalConfig &c);

void  calSeedTravel(CalKey &k, const CalConfig &c, float travel);

float calTravel(const CalKey &k);          // learned travel, in counts
bool  calDead(const CalKey &k, const CalConfig &c);
