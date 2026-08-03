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
  /* minTravel     */ 400,
  /* stillCounts   */ 30.0f,     // a press moves ~140 counts per sample;
                                 //   measured noise reaches ~25
  /* velTau        */ 15.0f
};

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

uint16_t calRemoveBleed(uint16_t measured, uint16_t previousSameRow, float k) {
  if (k <= 0.0f || k >= 0.5f) return measured;      // nonsense k: do nothing
  float t = ((float)measured - k * (float)previousSameRow) / (1.0f - k);
  if (t < 0.0f) t = 0.0f;
  if (t > 4095.0f) t = 4095.0f;
  return (uint16_t)(t + 0.5f);
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
  k.vel = 0.0f; k.prevRaw = k.baseline; k.havePrev = false;
}

float calTravel(const CalKey &k) {
  float t = k.baseline - k.bottom;
  return t < 1.0f ? 1.0f : t;
}

bool calDead(const CalKey &k, const CalConfig &c) {
  return calTravel(k) < (float)c.minTravel;
}

float calUpdate(CalKey &k, const CalConfig &c, uint16_t raw, bool noteActive) {
  k.raw = raw;

  // How fast the reading is moving, smoothed. This is how the layer decides
  // for itself whether the key is at rest.
  if (k.havePrev) {
    float a = 1.0f / (c.velTau + 1.0f);
    k.vel += a * (((float)raw - k.prevRaw) - k.vel);
  }
  k.prevRaw = (float)raw;
  k.havePrev = true;

  float pos = ((float)k.baseline - (float)raw) / calTravel(k);

  // Track the resting level whenever the key is LOW and STATIONARY, whatever
  // the gesture layer thinks is happening. A key sitting still near its rest
  // for seconds IS at rest, even if a note is nominally sounding — and that
  // case is exactly the one that used to latch forever.
  bool still = k.vel < c.stillCounts && k.vel > -c.stillCounts;
  if (still && pos < c.baselineGate && pos > -c.baselineGate)
    k.baseline += c.baselineTrack * ((float)raw - k.baseline);

  // Expand live. Pressing further than this key has ever gone must extend the
  // range immediately, never clip — otherwise the bottom of the travel is
  // silent until the next note, which a continuous instrument cannot afford.
  if (noteActive && (float)raw < k.bottom) {
    k.bottom = (float)raw;
    k.learned = true;
    k.borrowed = false;
    pos = ((float)k.baseline - (float)raw) / calTravel(k);
  }
  if (noteActive && (float)raw < k.noteMinRaw) k.noteMinRaw = (float)raw;

  k.position = clampf(pos, 0.0f, 1.0f);
  return k.position;
}

void calNoteEnded(CalKey &k, const CalConfig &c) {
  // A note that never went anywhere teaches NOTHING. Without this guard, a
  // note triggered by drift or noise — one that only ever reached 40 counts
  // deep — taught the key that its bottom was 40 counts down. Travel then
  // collapsed, every subsequent press saturated instantly, and the key
  // behaved erratically ever after. Same principle as the gesture layer's
  // refusal to let an un-leaned note shrink the pressure width.
  float observed = k.baseline - k.noteMinRaw;
  if (observed < (float)c.minTravel) { k.noteMinRaw = k.baseline; return; }

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
