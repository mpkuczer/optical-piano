// ============================================================================
// Press.cpp — see Press.h for why this is a separate, Arduino-free file
// ============================================================================
#include "Press.h"

// heldBand 0.10 of span is about 270 counts here: comfortably wider than any
// lean observed (0.03–0.06), and far narrower than the ~1.2 of full travel,
// so strike and release both fall outside it.
const PressConfig PRESS_DEFAULT = {
  /* settleMs  */ 60.0f,
  /* heldBand  */ 0.10f,
  /* minRange  */ 0.015f,
  /* stillRate */ 0.25f,
  /* velTauMs  */ 30.0f,
  /* tauMs     */ 20.0f,
  /* openRate  */ 0.35f,
  /* closeRate */ 0.04f
};

static const float NO_MIN = 9.0f;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void pressInit(PressState &s) {
  s.ref = 0.0f; s.top = 0.0f; s.learned = false;
  s.noteMin = NO_MIN; s.noteMax = 0.0f; s.value = 0.0f;
  s.vel = 0.0f; s.lastDepth = 0.0f; s.haveLast = false;
}

void pressNoteOn(PressState &s) {
  s.noteMin = NO_MIN; s.noteMax = 0.0f; s.value = 0.0f;
  s.vel = 0.0f; s.lastDepth = 0.0f; s.haveLast = false;
}

float pressRange(const PressState &s) {
  return s.learned ? (s.top - s.ref) : 0.0f;
}

float pressUpdate(PressState &s, const PressConfig &c,
                  float depth, float ageMs, float dtMs) {
  // --- map, using whatever window we learned on previous notes -------------
  float range  = pressRange(s);
  float target = (range >= c.minRange)
               ? clampf((depth - s.ref) / range, 0.0f, 1.0f)
               : 0.0f;
  // Time-based one-pole, so the response is identical whether we are
  // called every 1.2 ms or every 60 ms.
  float alpha = (c.tauMs > 0.0f) ? dtMs / (c.tauMs + dtMs) : 1.0f;
  s.value += clampf(alpha, 0.0f, 1.0f) * (target - s.value);

  // --- learn this note's held window ---------------------------------------
  // --- velocity, so we can tell moving from resting ------------------------
  if (s.haveLast && dtMs > 0.0f) {
    float inst = (depth - s.lastDepth) * 1000.0f / dtMs;
    float av   = dtMs / (c.velTauMs + dtMs);
    s.vel += av * (inst - s.vel);
  }
  s.lastDepth = depth;
  s.haveLast  = true;

  if (ageMs >= c.settleMs) {
    if (depth > s.noteMax) {
      s.noteMax = depth;
      // The deepest point moved far enough that any earlier minimum belonged
      // to the approach, not the hold. Discard it outright.
      if (s.noteMin < s.noteMax - c.heldBand) s.noteMin = NO_MIN;
    }
    // Record a minimum only while the key is stationary AND near the bottom.
    // Requiring stillness is what excludes the descent; requiring the band
    // is what excludes the release.
    bool still = s.vel < c.stillRate && s.vel > -c.stillRate;
    if (still && depth > s.noteMax - c.heldBand && depth < s.noteMin)
      s.noteMin = depth;
  }
  return s.value;
}

void pressNoteOff(PressState &s, const PressConfig &c) {
  if (s.noteMax > 0.0f && s.noteMin < NO_MIN) {
    if (!s.learned) {
      // No seed guess survives contact with real hardware — two attempts
      // proved that — so the first note sets the window outright.
      s.ref = s.noteMin; s.top = s.noteMax; s.learned = true;
    } else {
      s.ref += (s.noteMin < s.ref ? c.openRate : c.closeRate)
               * (s.noteMin - s.ref);
      s.top += (s.noteMax > s.top ? c.openRate : c.closeRate)
               * (s.noteMax - s.top);
    }
  }
  s.value = 0.0f;
}
