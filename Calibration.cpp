// ============================================================================
// Calibration.cpp — see Calibration.h
// ============================================================================
#include "Calibration.h"

const CalConfig CAL_DEFAULT = {
  /* baselineTrack */ 0.0005f,   // ~4 s at 510 Hz: far slower than a press,
                                 //   fast enough for thermal and light drift
  /* baselineGate  */ 0.04f,     // only while the key is essentially up
  /* bottomLearn   */ 0.25f,     // symmetric. Expand-only is a ratchet, and
                                 //   this project has been bitten twice.
  /* defaultTravel */ 3200,      // measured: baselines ~4072, bottoms 300-1000
  /* minTravel     */ 400
};

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void calInit(CalKey &k, uint16_t baseline, const CalConfig &c) {
  k.baseline = (float)baseline;
  // A bottom below zero is not reachable, so a key whose resting level is
  // already near zero ends up with almost no travel — which is exactly how a
  // shorted sensor should present. Deadness falls out of the arithmetic
  // rather than needing a magic threshold.
  float b = k.baseline - (float)c.defaultTravel;
  k.bottom = b < 0.0f ? 0.0f : b;
  k.learned = false;
  k.borrowed = false;
  k.raw = baseline;
  k.position = 0.0f;
  k.noteMinRaw = k.baseline;
}

float calTravel(const CalKey &k) {
  float t = k.baseline - k.bottom;
  return t < 1.0f ? 1.0f : t;
}

bool calDead(const CalKey &k, const CalConfig &c) {
  return calTravel(k) < (float)c.minTravel;
}

float calUpdate(CalKey &k, const CalConfig &c, uint16_t raw, bool idle) {
  k.raw = raw;

  float pos = ((float)k.baseline - (float)raw) / calTravel(k);

  // Track the resting level only while the key is genuinely up. Without this
  // the baseline would follow the finger; with a fixed boot-time baseline,
  // thermal drift alone eventually crosses a 2% trigger threshold.
  if (idle && pos < c.baselineGate)
    k.baseline += c.baselineTrack * ((float)raw - k.baseline);

  // Expand live. Pressing further than this key has ever gone must extend the
  // range immediately, never clip — otherwise the bottom of the travel is
  // silent until the next note, which a continuous instrument cannot afford.
  if (!idle && (float)raw < k.bottom) {
    k.bottom = (float)raw;
    k.learned = true;
    k.borrowed = false;
    pos = ((float)k.baseline - (float)raw) / calTravel(k);
  }
  if (!idle && (float)raw < k.noteMinRaw) k.noteMinRaw = (float)raw;

  k.position = clampf(pos, 0.0f, 1.0f);
  return k.position;
}

void calNoteEnded(CalKey &k, const CalConfig &c) {
  if (k.noteMinRaw < k.baseline) {
    if (!k.learned || k.borrowed) {
      k.bottom = k.noteMinRaw;
      k.learned = true;
      k.borrowed = false;
    } else {
      k.bottom += c.bottomLearn * (k.noteMinRaw - k.bottom);
    }
    if (k.bottom < 0.0f) k.bottom = 0.0f;
    if (k.bottom > k.baseline - 1.0f) k.bottom = k.baseline - 1.0f;
  }
  k.noteMinRaw = k.baseline;
}

void calSeedTravel(CalKey &k, const CalConfig &c, float travel) {
  if (k.learned && !k.borrowed) return;      // never override real data
  if (travel < (float)c.minTravel) return;
  float b = k.baseline - travel;
  k.bottom = b < 0.0f ? 0.0f : b;
  k.learned = true;
  k.borrowed = true;
}
