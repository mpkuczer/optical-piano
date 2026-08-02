// ============================================================================
// Press.cpp — see Press.h for the design and why it is shaped this way
// ============================================================================
#include "Press.h"

const PressConfig PRESS_DEFAULT = {
  /* settleMs  */  60.0f,
  /* heldBand  */   0.12f,   // measured leans are 0.03–0.07 of span
  /* minRange  */   0.015f,
  /* tauMs     */  20.0f,
  /* learnRate */   0.25f,
  /* stillRate */   0.25f,
  /* velTauMs  */  30.0f,
  /* absLo     */   0.06f,   // above resting noise
  /* absHi     */   0.95f,   // short of the stop, where the sensor saturates
  /* absTauMs  */   8.0f
};

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void pressInit(PressState &s) {
  s.width = 0.0f; s.learned = false; s.borrowed = false;
  s.mode = PRESS_RELATIVE;
  pressNoteOn(s);
}

void pressSetMode(PressState &s, unsigned char mode) { s.mode = mode; }

void pressNoteOn(PressState &s) {
  s.noteRest = 0.0f; s.noteMax = 0.0f; s.haveRest = false;
  s.vel = 0.0f; s.lastDepth = 0.0f; s.haveLast = false; s.value = 0.0f;
}

float pressRange(const PressState &s) { return s.learned ? s.width : 0.0f; }

float pressUpdate(PressState &s, const PressConfig &c,
                  float depth, float ageMs, float dtMs) {
  // --- absolute: a pure function of the current reading --------------------
  // Nothing is learned, nothing is remembered, and the strike is irrelevant.
  // This is the theremin case: the sound is the position of your hand, and
  // asking "past where I landed" would be asking the wrong question.
  if (s.mode == PRESS_ABSOLUTE) {
    float target = clampf((depth - c.absLo) / (c.absHi - c.absLo), 0.0f, 1.0f);
    float a = (c.absTauMs > 0.0f) ? dtMs / (c.absTauMs + dtMs) : 1.0f;
    s.value += clampf(a, 0.0f, 1.0f) * (target - s.value);
    return s.value;
  }

  // --- velocity, so we can tell "moving" from "resting" --------------------
  if (s.haveLast && dtMs > 0.0f) {
    float inst = (depth - s.lastDepth) * 1000.0f / dtMs;
    float av   = dtMs / (c.velTauMs + dtMs);
    s.vel += av * (inst - s.vel);
  }
  s.lastDepth = depth;
  s.haveLast  = true;

  if (ageMs >= c.settleMs) {
    bool still = s.vel < c.stillRate && s.vel > -c.stillRate;
    // A hesitant press pauses part-way down, looks momentarily still, and
    // sets a rest point that is not the bottom. If the key later goes more
    // than a lean deeper than that, it was still descending — discard and
    // wait for it to stop again. The s16 capture does exactly this.
    if (s.haveRest && depth > s.noteRest + c.heldBand) s.haveRest = false;
    if (!s.haveRest) {
      // The first moment the key stops moving is this note's zero.
      if (still) { s.noteRest = depth; s.noteMax = depth; s.haveRest = true; }
    } else {
      if (depth > s.noteMax) s.noteMax = depth;
      // The zero may drop a little if the key settles further — but only
      // while stationary and still near the bottom. Without those guards the
      // RELEASE drags it down (the key rising IS depth falling), and the
      // learned width becomes the whole travel, pinned at heldBand.
      if (still && depth < s.noteRest && depth > s.noteMax - c.heldBand)
        s.noteRest = depth;
    }
  }

  float w = pressRange(s);
  float target = (w >= c.minRange && s.haveRest)
               ? clampf((depth - s.noteRest) / w, 0.0f, 1.0f)
               : 0.0f;
  float alpha = (c.tauMs > 0.0f) ? dtMs / (c.tauMs + dtMs) : 1.0f;
  s.value += clampf(alpha, 0.0f, 1.0f) * (target - s.value);
  return s.value;
}

void pressNoteOff(PressState &s, const PressConfig &c) {
  if (s.haveRest) {
    float obs = s.noteMax - s.noteRest;
    if (obs > c.heldBand) obs = c.heldBand;   // not a lean; do not believe it
    if (obs >= c.minRange) {
      if (!s.learned || s.borrowed) {
        s.width = obs; s.learned = true; s.borrowed = false;
      } else {
        s.width += c.learnRate * (obs - s.width);   // symmetric: no ratchet
      }
    }
    // A note with no lean teaches nothing. It must not SHRINK the width
    // either, or playing normally for a while would switch pressure off.
  }
  s.value = 0.0f;
}

void pressSeedFrom(PressState &s, float width) {
  if (s.learned && !s.borrowed) return;      // never override real data
  if (width < PRESS_DEFAULT.minRange) return;
  s.width = width; s.learned = true; s.borrowed = true;
}
