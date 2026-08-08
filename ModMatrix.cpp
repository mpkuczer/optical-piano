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

// Curves are defined only on 0..1. A bipolar source (an LFO) keeps its sign
// and the shape is applied to its magnitude, so a curved LFO stays symmetric
// instead of turning into a rectifier.
static float applyCurve(float v, uint8_t curve, float shape, float span) {
  if (shape <= 0.0f && curve == CURVE_LINEAR) return v;
  float s = v < 0.0f ? -1.0f : 1.0f;
  float m = v < 0.0f ? -v : v;
  // Tested before the shape override, because for this curve `shape` is the
  // detent edge and not an exponent — see ModMatrix.h. Every other curve keeps
  // the old rule exactly.
  if (curve == CURVE_DETENT) {
    // An edge at 1.0 divides by zero and an edge at or below 0 makes the dead
    // zone empty, which is not a detent and is the sort of typo that reaches
    // the audio path as a NaN. Half travel is the harmless reading.
    float a = (shape > 0.0f && shape < 1.0f) ? shape : 0.5f;
    // A span of zero is the default, and it means the crossing runs out to
    // full travel rather than being instantaneous — a zero-width crossing is
    // a step, and a step in pitch is a click.
    float w = (span > 0.0f) ? span : (1.0f - a);
    float t = clampf((m - a) / w, 0.0f, 1.0f);
    return s * t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
  }
  if (shape > 0.0f) {
    // A route that asks for a specific exponent pays for a powf; the enum
    // curves keep their multiply-based paths, which is most routes.
    m = powf(m, shape);
  } else {
    switch (curve) {
      case CURVE_SQUARE: m = m * m;            break;
      case CURVE_AUDIO:  m = powf(m, 1.8f);    break;
      case CURVE_SQRT:   m = sqrtf(m);         break;
      case CURVE_LATE:   m = m * m * m;        break;
      default: break;
    }
  }
  return s * m;
}

float lfoValue(const LfoConfig &cfg, float ageMs, uint32_t seed,
               float phaseOffset) {
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

  // The offset is added in CYCLES, so it shifts the waveform without
  // changing its rate. Sample-and-hold picks up the offset too, which means
  // held notes step to new values at different moments rather than together.
  float phase = t * cfg.rateHz + phaseOffset;        // in cycles
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
    case SRC_LFO1:     return lfoValue(lfo1, c.ageMs, seed, c.lfoPhase);
    case SRC_LFO2:     return lfoValue(lfo2, c.ageMs, seed ^ 0x9E3779B9U,
                                       c.lfoPhase);
    case SRC_LFO1_UNI: return 0.5f * (lfoValue(lfo1, c.ageMs, seed,
                                               c.lfoPhase) + 1.0f);
    case SRC_RANDOM:   return c.randomv;
    case SRC_NOTEAGE:  return clampf(c.ageMs / NOTEAGE_FULL_MS, 0.0f, 1.0f);
    case SRC_PRESSURE: return c.pressure;
    case SRC_POSITION: return c.position;
    case SRC_KEYVEL:   return c.keyvel;
    case SRC_ATTACK:   return c.attack;
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
    uint8_t g = routes[i].via;
    if (s >= NUM_SOURCES || d >= NUM_DESTS) continue;
    if (g >= NUM_SOURCES) g = SRC_CONST;
    if (!have[s]) { cache[s] = sourceValue(s, ctx, lfo1, lfo2, seed); have[s] = true; }
    if (!have[g]) { cache[g] = sourceValue(g, ctx, lfo1, lfo2, seed); have[g] = true; }
    // The curve shapes the SOURCE, not the via-multiplier: `via` scales how
    // much of the route applies, and shaping that would compound confusingly.
    out[d] += applyCurve(cache[s], routes[i].curve, routes[i].shape,
                         routes[i].span)
            * cache[g] * routes[i].depth;
  }
}

// Sized by the initialiser, not by NUM_SOURCES, so the static_assert below can
// catch a source added to the enum without a name. Declaring [NUM_SOURCES]
// instead would silently zero-fill and print "(null)" at runtime.
static const char *SRC_NAMES[] = {
  "const", "velocity", "keytrack", "ampenv", "filtenv",
  "lfo1", "lfo2", "lfo1uni", "random", "noteage", "pressure", "position",
  "keyvel", "attack"
};
static const char *DST_NAMES[] = {
  "pitch", "detune", "cutoff", "amp", "pwm", "res", "oscB", "fm", "noise"
};
static_assert(sizeof(SRC_NAMES) / sizeof(*SRC_NAMES) == NUM_SOURCES,
              "a ModSource was added without a name in SRC_NAMES");
static_assert(sizeof(DST_NAMES) / sizeof(*DST_NAMES) == NUM_DESTS,
              "a ModDest was added without a name in DST_NAMES");

const char *modSourceName(uint8_t s) {
  return s < NUM_SOURCES ? SRC_NAMES[s] : "?";
}
const char *modDestName(uint8_t d) {
  return d < NUM_DESTS ? DST_NAMES[d] : "?";
}
