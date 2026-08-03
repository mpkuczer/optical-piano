// ============================================================================
// Gesture.cpp — see Gesture.h
// ============================================================================
#include "Gesture.h"
#include <math.h>

const GestureConfig GESTURE_DEFAULT = {
  // A continuous instrument must have no critical POINT, but it still needs
  // a floor. At 0.020 the margin over crosstalk was only 1.3x: settling bleed
  // from a fully pressed key on the same row is ~0.004 of travel, and
  // measured jitter reaches ~0.011, so a held chord could trigger its
  // neighbours. 0.060 gave 3.9x on those numbers.
  //
  // Those numbers no longer hold on this hardware: 0.060 still triggers from
  // crosstalk in practice, so the residual is well above the ~0.004 the bleed
  // correction was believed to leave. Raised to 0.150. Suspect CROSS_BLEED
  // (Config.h) is stale — the 'j' probe measures it and 'k' shows the worst
  // residual across untouched keys. Fixing it there is the real repair; this
  // is the floor that stops false notes meanwhile.
  //
  // THIS IS NO LONGER FREE. The old value cost nothing audible because the
  // amplitude route uses CURVE_AUDIO: 0.060^1.8 is 0.0063, or -44 dB. At
  // 0.150 the taper gives 0.033, or -30 dB — 14 dB louder at onset, and
  // heading back toward the -24 dB that clicked. If notes now announce
  // themselves, put CURVE_LATE (x^3) on the amplitude route of the continuous
  // patches: 0.150^3 is 0.0034, back to -49 dB.
  /* contOn      */ 0.150f,
  // Kept at the same 1.5:1 hysteresis as before. Raising this with contOn is
  // not optional: crosstalk that can push a line to 0.150 can certainly hold
  // it above 0.040, so leaving the release threshold behind would trade false
  // starts for notes that never end while a neighbour is held.
  /* contOff     */ 0.100f,
  /* velStart    */ 0.060f,
  /* onPos       */ 0.400f,
  /* offPos      */ 0.180f,
  /* velFastMs   */ 2.0f,
  /* velSlowMs   */ 200.0f,
  /* velAtFast   */ 1.0f,
  /* velAtSlow   */ 0.16f,
  /* settleMs    */ 60.0f,
  /* stillRate   */ 0.20f,
  /* velTauMs    */ 30.0f,
  /* keyVelFull  */ 4.0f,
  /* heldBand    */ 0.10f,
  // Room left below the rest point can legitimately be tiny — a hard strike
  // can land the key near its own bottom — and the divisor must not run away.
  // 0.10 also matches heldBand, so a note that lands fully bottomed behaves
  // like one with exactly a lean's worth of room rather than going infinitely
  // twitchy.
  /* minRoom     */ 0.100f,
  /* smoothTauMs */ 8.0f,
  /* retreatHyst */ 0.025f,
  /* maxDescentMs*/ 600.0f,
  /* onSamples   */ 3,       // ~8 ms at a 370 Hz scan
  /* offSamples  */ 3
};

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void gestureInit(GestureState &g) {
  g.phase = GP_IDLE;
  g.noteArt = ART_CONTINUOUS;
  g.noteOnMs = 0.0f;
  g.prevPos = 0.0f; g.prevMs = 0.0f; g.havePrev = false;
  g.velStartMs = 0.0f; g.vel = 0.0f;
  g.overCount = 0; g.underCount = 0;
  g.position = 0.0f; g.pressure = 0.0f; g.strike = 0.5f;
  g.restPos = 0.0f; g.haveRest = false; g.noteMaxPos = 0.0f;
}

bool gestureIdle(const GestureState &g) { return g.phase == GP_IDLE; }

float gestureKeyVel(const GestureState &g, const GestureConfig &c) {
  float full = c.keyVelFull > 0.0f ? c.keyVelFull : 4.0f;
  return clampf(g.vel / full, -1.0f, 1.0f);
}

// Flight time -> 0..1. Logarithmic, because the ear's sense of "harder" is
// closer to a ratio than a difference.
static float velocityFromFlight(const GestureConfig &c, float ms) {
  if (ms < 0.2f) ms = 0.2f;
  float span = logf(c.velSlowMs) - logf(c.velFastMs);
  float fr   = (logf(ms) - logf(c.velFastMs)) / span;
  return clampf(c.velAtFast + fr * (c.velAtSlow - c.velAtFast), 0.0f, 1.0f);
}

// Linear interpolation of the moment a rising signal crossed `th`. Without it
// the timing resolution is the scan period, far too coarse to tell a soft
// press from a hard one.
static float crossingTime(float pPos, float pMs, float pos, float ms, float th) {
  if (pos == pPos) return ms;
  float fr = clampf((th - pPos) / (pos - pPos), 0.0f, 1.0f);
  return pMs + fr * (ms - pMs);
}

// ---- pressure: position relative to where THIS note landed -----------------
// The rest point is defined by the key having STOPPED, not by it being deep:
// at a 500 Hz scan rate the approach is a smooth ramp, and position alone
// cannot tell "descending through 0.8" from "resting at 0.8".
static void updatePressure(GestureState &g, const GestureConfig &c,
                           float pos, float ageMs, float dtMs) {
  if (ageMs >= c.settleMs) {
    bool still = g.vel < c.stillRate && g.vel > -c.stillRate;

    // A hesitant press pauses part-way down and looks momentarily still. If
    // the key later goes more than a lean deeper, that pause was not the
    // rest point — discard it and wait for the key to stop again.
    if (g.haveRest && pos > g.restPos + c.heldBand) g.haveRest = false;

    if (!g.haveRest) {
      if (still) { g.restPos = pos; g.noteMaxPos = pos; g.haveRest = true; }
    } else {
      if (pos > g.noteMaxPos) g.noteMaxPos = pos;
      if (still && pos < g.restPos && pos > g.noteMaxPos - c.heldBand)
        g.restPos = pos;
    }
  }

  // Scale the lean by the travel LEFT below where this note settled. Land
  // shallow and there is a lot of key to push through; land deep and a small
  // move spans the same range — which is the honest report, because a deep
  // landing really has left you less to work with.
  float target = 0.0f;
  if (g.haveRest) {
    float room = 1.0f - g.restPos;
    if (room < c.minRoom) room = c.minRoom;
    target = clampf((pos - g.restPos) / room, 0.0f, 1.0f);
  }

  float a = dtMs / (c.smoothTauMs + dtMs);
  g.pressure += clampf(a, 0.0f, 1.0f) * (target - g.pressure);
}

static void beginNote(GestureState &g, GestureEvent &e, float vel, float nowMs,
                      uint8_t art) {
  g.noteArt = art;
  g.strike = vel;
  g.noteOnMs = nowMs;
  g.haveRest = false; g.noteMaxPos = 0.0f; g.pressure = 0.0f;
  g.overCount = 0; g.underCount = 0;
  g.phase = GP_SOUNDING;
  e.noteOn = true; e.velocity = vel;
}

// No longer takes the config: with width gone there is nothing left to learn
// at note end. Ending a note is now pure teardown.
static void endNote(GestureState &g, GestureEvent &e, float releaseVel) {
  g.phase = GP_IDLE;
  g.pressure = 0.0f;
  g.overCount = 0; g.underCount = 0;
  e.noteOff = true; e.releaseVel = releaseVel; e.noteEnded = true;
}

GestureEvent gestureUpdate(GestureState &g, const GestureConfig &c,
                           uint8_t art, float pos, float nowMs) {
  GestureEvent e = { false, false, 0.5f, 0.5f, false };

  float dtMs = (g.havePrev && nowMs > g.prevMs) ? (nowMs - g.prevMs) : 2.0f;

  // Velocity of the position signal, smoothed. Used both for release speed
  // and for deciding when the key has come to rest.
  if (g.havePrev && dtMs > 0.0f) {
    float inst = (pos - g.prevPos) * 1000.0f / dtMs;
    float av = dtMs / (c.velTauMs + dtMs);
    g.vel += av * (inst - g.vel);
  }

  // Smoothed absolute position is an output in its own right, available in
  // every articulation whether the patch uses it or not.
  {
    float a = dtMs / (c.smoothTauMs + dtMs);
    g.position += clampf(a, 0.0f, 1.0f) * (pos - g.position);
  }

  // Starting a note uses the CURRENT articulation; ending one uses the
  // articulation that note began with.
  const bool triggered = (art != ART_CONTINUOUS);
  const float onLv  = triggered ? c.onPos : c.contOn;
  const bool  heldTriggered = (g.noteArt != ART_CONTINUOUS);
  const float offLv = heldTriggered ? c.offPos : c.contOff;

  switch (g.phase) {
    case GP_IDLE:
      if (!triggered) {
        // No critical point, no velocity, no descent phase. The note simply
        // begins when the key leaves rest, silent at that instant because the
        // patch maps position to amplitude — but only once the reading has
        // PERSISTED, so a crosstalk spike cannot start it.
        if (pos > onLv) {
          if (g.overCount < 255) g.overCount++;
        } else {
          g.overCount = 0;
        }
        if (g.overCount >= c.onSamples) beginNote(g, e, 0.5f, nowMs, art);
      } else if (pos > c.velStart) {
        g.velStartMs = (g.havePrev && g.prevPos <= c.velStart)
                     ? crossingTime(g.prevPos, g.prevMs, pos, nowMs, c.velStart)
                     : nowMs;
        g.phase = GP_DESCENDING;
      }
      break;

    case GP_DESCENDING:
      if (nowMs - g.velStartMs > c.maxDescentMs) { g.phase = GP_IDLE; break; }
      if (pos > onLv) {
        float onMs = (g.havePrev && g.prevPos <= onLv)
                   ? crossingTime(g.prevPos, g.prevMs, pos, nowMs, onLv)
                   : nowMs;
        float flight = onMs - g.velStartMs;
        if (flight < 0.0f) flight = 0.0f;
        // PLUCKED and GATED discard the measurement on purpose: a plectrum
        // plucks identically however hard the key is hit, and an organ pipe
        // either speaks or does not.
        float v = (art == ART_STRUCK) ? velocityFromFlight(c, flight) : 0.5f;
        beginNote(g, e, v, nowMs, art);
      } else if (pos < c.velStart - c.retreatHyst) {
        g.phase = GP_IDLE;               // half press, withdrawn
      }
      break;

    case GP_SOUNDING:
      // GATED ignores everything after the start by definition.
      if (g.noteArt != ART_GATED)
        updatePressure(g, c, pos, nowMs - g.noteOnMs, dtMs);
      // The same persistence on the way out, so a downward transient cannot
      // chop a held note.
      if (pos < offLv) {
        if (g.underCount < 255) g.underCount++;
      } else {
        g.underCount = 0;
      }
      if (g.underCount >= (heldTriggered ? 1 : c.offSamples)) {
        // Release speed: how fast the key came back up, in position per
        // second. Negative velocity means rising.
        float rel = clampf(-g.vel / 4.0f, 0.0f, 1.0f);
        endNote(g, e, rel);
      }
      break;
  }

  g.prevPos = pos; g.prevMs = nowMs; g.havePrev = true;
  return e;
}
