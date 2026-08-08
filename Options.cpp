// ============================================================================
// Options.cpp — see Options.h
// ============================================================================
#include "Options.h"

const OptionsConfig OPTIONS_DEFAULT = {
  // The bottom three keys. Adjacent, at one end, and a cluster rather than a
  // chord anyone would voice — the two things that make a gesture both easy to
  // describe and hard to play by accident.
  /* chord    */ { 0, 1, 2, 0, 0 },
  /* nChord   */ 3,
  // The ends of the keyboard. They are the only two keys you can find without
  // counting, which is the whole requirement for a control you reach for while
  // looking at your hands rather than at a screen that does not exist.
  /* navPrev  */ 0,
  /* navNext  */ 24,

  /* holdMs   */ 5000.0f,
  /* navArmMs */ 150.0f,

  // Far above GESTURE_DEFAULT's contOn of 0.150 — see Options.h for why this
  // layer wants the opposite trade from note detection. 0.350 is past the
  // point where a key is unambiguously being pressed rather than rested on,
  // and 0.200 leaves 1.75:1 of hysteresis, so a finger holding still through
  // the jitter band cannot chatter the mode timer.
  /* keyOn    */ 0.350f,
  /* keyOff   */ 0.200f,
  // ~11 ms and ~16 ms at the ~370 Hz sweep rate SETTLE_US gives (Config.h).
  // Longer on release than on press because a release that reads early
  // restarts a five-second timer, and a press that reads late costs 11 ms of
  // a five-second hold.
  /* onSamples  */ 4,
  /* offSamples */ 6
};

void optionsInit(OptionsState &s) {
  s.active = false;
  s.held = 0;
  for (int i = 0; i < OPT_NUM_KEYS; i++) {
    s.over[i] = 0; s.under[i] = 0; s.downMs[i] = 0.0f; s.navFired[i] = false;
  }
  s.chordHeld = false;
  s.chordSinceMs = 0.0f;
  s.rearmPending = false;
}

bool optionsActive(const OptionsState &s) { return s.active; }

bool optionsReserved(const OptionsState &s, const OptionsConfig &c, int key) {
  if (!s.active) return false;
  return key == c.navPrev || key == c.navNext;
}

// A transition always looks the same from the outside, whether a chord or the
// serial console caused it: the mode flips, the latch arms, and every nav key
// currently down is marked spent so the press that did this cannot also step a
// preset when it is released.
static OptionsEvent transition(OptionsState &s, const OptionsConfig &c, bool on) {
  OptionsEvent e = { false, false, 0 };
  s.active = on;
  e.enter  =  on;
  e.exit   = !on;
  s.rearmPending = true;
  s.chordSinceMs = 0.0f;
  s.navFired[c.navPrev] = true;
  s.navFired[c.navNext] = true;
  return e;
}

OptionsEvent optionsForce(OptionsState &s, const OptionsConfig &c, bool on) {
  if (s.active == on) return OptionsEvent{ false, false, 0 };
  return transition(s, c, on);
}

OptionsEvent optionsUpdate(OptionsState &s, const OptionsConfig &c,
                           const float pos[OPT_NUM_KEYS], float nowMs) {
  OptionsEvent e = { false, false, 0 };

  // ---- Schmitt trigger, per key -------------------------------------------
  for (int i = 0; i < OPT_NUM_KEYS; i++) {
    const bool wasHeld = (s.held >> i) & 1u;
    if (!wasHeld) {
      if (pos[i] > c.keyOn) { if (s.over[i] < 255) s.over[i]++; }
      else                    s.over[i] = 0;
      if (s.over[i] >= c.onSamples) {
        s.held |= (uint32_t)1u << i;
        s.over[i] = 0; s.under[i] = 0;
        s.downMs[i]   = nowMs;
        s.navFired[i] = false;
      }
    } else {
      if (pos[i] < c.keyOff) { if (s.under[i] < 255) s.under[i]++; }
      else                     s.under[i] = 0;
      if (s.under[i] >= c.offSamples) {
        s.held &= ~((uint32_t)1u << i);
        s.over[i] = 0; s.under[i] = 0;
      }
    }
  }

  // ---- the chord ----------------------------------------------------------
  bool all = (c.nChord > 0);
  bool any = false;
  for (int i = 0; i < c.nChord && i < OPT_CHORD_MAX; i++) {
    const bool down = (s.held >> c.chord[i]) & 1u;
    if (down) any = true; else all = false;
  }

  // The latch clears on a full release, not on the chord merely becoming
  // incomplete: lifting one finger and putting it back must not re-arm a press
  // that has already been spent on a transition.
  if (!any) s.rearmPending = false;

  if (all && !s.chordHeld) s.chordSinceMs = nowMs;
  s.chordHeld = all;

  if (all && !s.rearmPending && (nowMs - s.chordSinceMs) >= c.holdMs)
    return transition(s, c, !s.active);

  // ---- navigation ---------------------------------------------------------
  // Only inside the mode, only when the chord is not forming, and only once
  // per press. The chord test is what makes the leftmost key usable as both a
  // chord member and a nav key — see Options.h.
  if (s.active && !all && !s.rearmPending) {
    const uint8_t nav[2] = { c.navPrev, c.navNext };
    const int8_t  dir[2] = { -1, +1 };
    for (int k = 0; k < 2; k++) {
      const int i = nav[k];
      if (!((s.held >> i) & 1u) || s.navFired[i]) continue;
      if ((nowMs - s.downMs[i]) < c.navArmMs)     continue;
      s.navFired[i] = true;
      e.presetDelta = dir[k];
      // One step per sweep. Both ends held at once is not a gesture with a
      // meaning; letting them cancel to zero would be a silent no-op that
      // looks like a broken key.
      break;
    }
  }

  return e;
}
