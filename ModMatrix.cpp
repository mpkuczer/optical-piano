// ============================================================================
// ModMatrix.cpp — LFO shapes and route summing
// ============================================================================
#include "ModMatrix.h"
#include <math.h>

static const float TWO_PI_F = 6.28318530718f;

// Deterministic hash, used for sample-and-hold. Not cryptographic; it just
// needs to scatter consecutive integers well.
static float hashToUnit(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return (float)(x & 0xFFFFFF) / 8388607.5f - 1.0f;   // -1..1
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

float lfoValue(const LfoConfig &cfg, float ageMs, uint32_t seed) {
  if (cfg.rateHz <= 0.0f) return 0.0f;
  // Strict '<': at exactly delayMs (and at ageMs 0 with no delay) the LFO
  // must EVALUATE, not return zero. Using '<=' silently discarded the first
  // sample of every note — invisible for a sine, since sin(0) is 0 anyway,
  // but it clipped the start of triangle, saw and square shapes.
  if (ageMs < cfg.delayMs) return 0.0f;

  float t = (ageMs - cfg.delayMs) * 0.001f;          // seconds since it began
  float fade = (cfg.fadeMs > 0.0f)
             ? clampf((ageMs - cfg.delayMs) / cfg.fadeMs, 0.0f, 1.0f)
             : 1.0f;

  float phase = t * cfg.rateHz;                      // in cycles
  float frac  = phase - floorf(phase);               // 0..1 within the cycle
  float v = 0.0f;

  switch (cfg.shape) {
    case LFO_SINE:        v = sinf(TWO_PI_F * frac);            break;
    case LFO_TRIANGLE:    v = (frac < 0.5f) ? (4.0f * frac - 1.0f)
                                            : (3.0f - 4.0f * frac); break;
    case LFO_SAW_DOWN:    v = 1.0f - 2.0f * frac;               break;
    case LFO_RAMP_UP:     v = 2.0f * frac - 1.0f;               break;
    case LFO_SQUARE:      v = (frac < 0.5f) ? 1.0f : -1.0f;     break;
    case LFO_SAMPLE_HOLD: v = hashToUnit((uint32_t)floorf(phase) * 2654435761U
                                         + seed);               break;
    default:              v = 0.0f;                             break;
  }
  return v * fade;
}

static float sourceValue(uint8_t src, const ModContext &c,
                         const LfoConfig &lfo1, const LfoConfig &lfo2,
                         uint32_t seed) {
  switch (src) {
    case SRC_CONST:    return 1.0f;
    case SRC_VELOCITY: return c.velocity;
    case SRC_KEYTRACK: return c.keytrack;
    case SRC_AMPENV:   return c.ampEnv;
    case SRC_FILTENV:  return c.filtEnv;
    case SRC_LFO1:     return lfoValue(lfo1, c.ageMs, seed);
    case SRC_LFO2:     return lfoValue(lfo2, c.ageMs, seed ^ 0x9E3779B9U);
    case SRC_LFO1_UNI: return 0.5f * (lfoValue(lfo1, c.ageMs, seed) + 1.0f);
    case SRC_RANDOM:   return c.randomv;
    case SRC_NOTEAGE:  return clampf(c.ageMs / NOTEAGE_FULL_MS, 0.0f, 1.0f);
    default:           return 0.0f;
  }
}

void modEvaluate(const ModContext &ctx,
                 const LfoConfig &lfo1, const LfoConfig &lfo2,
                 const ModRoute *routes, int numRoutes,
                 uint32_t seed,
                 float out[NUM_DESTS]) {
  for (int d = 0; d < NUM_DESTS; d++) out[d] = 0.0f;
  if (!routes) return;

  // Cache each source so two routes from the same LFO don't evaluate it
  // twice — and, more importantly, so they can't disagree.
  float cache[NUM_SOURCES];
  bool  have[NUM_SOURCES] = { false };

  for (int i = 0; i < numRoutes; i++) {
    uint8_t s = routes[i].source;
    uint8_t d = routes[i].dest;
    if (s >= NUM_SOURCES || d >= NUM_DESTS) continue;
    if (!have[s]) { cache[s] = sourceValue(s, ctx, lfo1, lfo2, seed); have[s] = true; }
    out[d] += cache[s] * routes[i].depth;
  }
}

static const char *SRC_NAMES[NUM_SOURCES] = {
  "const", "velocity", "keytrack", "ampenv", "filtenv",
  "lfo1", "lfo2", "lfo1uni", "random", "noteage"
};
static const char *DST_NAMES[NUM_DESTS] = {
  "pitch", "detune", "cutoff", "amp", "pwm", "res", "oscB"
};

const char *modSourceName(uint8_t s) {
  return s < NUM_SOURCES ? SRC_NAMES[s] : "?";
}
const char *modDestName(uint8_t d) {
  return d < NUM_DESTS ? DST_NAMES[d] : "?";
}
