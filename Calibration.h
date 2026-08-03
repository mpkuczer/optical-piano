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
  float    baselineGate;    // upward-reading side only: how far BELOW the
                            //   baseline a stationary key may sit and still
                            //   count as at rest. There is no limit on the
                            //   other side — a reading ABOVE the baseline
                            //   cannot be a finger, so it is drift by
                            //   definition and always followed.
  float    recoverMs;       // a stationary key shallower than recoverBelow
                            //   for this long is re-referenced regardless.
                            //   The last line of defence against a drift big
                            //   enough to sit above the trigger threshold.
  float    recoverBelow;    // ...but never for a key held deep enough to be a
                            //   deliberate, audible note.
  float    bottomLearn;     // EMA rate for the learned bottom, per note
  uint16_t defaultTravel;   // assumed travel before a key has been played
  uint16_t minTravel;       // below this a key cannot report a press at all
  float    stillCounts;     // counts per sample below which the key counts as
                            //   stationary. Decided HERE, from the signal, and
                            //   deliberately not asked of the gesture layer.
  float    velTau;          // samples of smoothing on that estimate
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
  float    vel;             // smoothed counts per sample
  float    stillMs;         // how long the reading has been stationary
  float    prevRaw;
  bool     havePrev;
};

// ---- crosstalk removal ---------------------------------------------------
// Rows are shared. When a column is selected the row line starts settling
// from wherever the PREVIOUS column left it, and with a finite settle time it
// does not fully arrive. A key pressed hard in one column therefore drags the
// reading of the next column ON THE SAME ROW downward — measured at ~140
// counts on this instrument, which is above the continuous trigger threshold.
// That is why holding s7 made s8 speak, and why the gated organ patch, whose
// threshold is seven times higher, showed nothing.
//
// It is a first-order settle, so it inverts exactly:
//     measured = true + (previous - true) * k
//     true     = (measured - k * previous) / (1 - k)
// with k = exp(-SETTLE_US / tau). One multiply-add per sample, and unlike
// raising SETTLE_US it costs no scan rate — which is velocity resolution.
uint16_t calRemoveBleed(uint16_t measured, uint16_t previousSameRow, float k);

void  calInit(CalKey &k, uint16_t baseline, const CalConfig &c);

// Feed one reading. `noteActive` says a note is in progress, and is used ONLY
// to decide whether to learn this key's bottom.
//
// It is deliberately NOT used to gate baseline tracking. It was, and that
// created a deadlock: a note holds the key out of idle, tracking is frozen
// while a note sounds, so once drift pushed the resting level above the
// note-off threshold the note could never end and the baseline could never
// catch up. The key latched on and stayed there. Whether a key is at rest is
// a question about the SIGNAL — is it low, and has it stopped moving — and
// this layer can answer it without asking anyone.
float calUpdate(CalKey &k, const CalConfig &c, uint16_t raw, bool noteActive);

// Fold this note's deepest point into the learned bottom.
void  calNoteEnded(CalKey &k, const CalConfig &c);

void  calSeedTravel(CalKey &k, const CalConfig &c, float travel);

float calTravel(const CalKey &k);          // learned travel, in counts
bool  calDead(const CalKey &k, const CalConfig &c);
