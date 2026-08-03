// ============================================================================
// test_gesture.cpp — LAYER 2 tests
// ============================================================================
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "Gesture.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string &what, const std::string &d) {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         d.empty() ? "" : "  -- ", d.c_str());
  if (!ok) failures++;
}

static const float DT = 2.0f;          // ~500 Hz scan

struct Played {
  bool  gotOn, gotOff;
  float velocity, release;
  float pressLo, pressHi;
  float posLo, posHi;
};

// Play one note: descend to `rest` over `strikeMs`, hold ~700 ms leaning by
// `lean`, then release. Records what the gesture layer reported.
static Played play(GestureState &g, const GestureConfig &c, uint8_t art,
                   float rest, float lean, float strikeMs, float noise,
                   float *nowMs) {
  Played p = { false, false, 0, 0, 2.0f, -1.0f, 2.0f, -1.0f };
  int strikeSteps = (int)(strikeMs / DT); if (strikeSteps < 1) strikeSteps = 1;

  auto step = [&](float pos, bool collect) {
    GestureEvent e = gestureUpdate(g, c, art, pos, *nowMs);
    if (e.noteOn)  { p.gotOn = true;  p.velocity = e.velocity; }
    if (e.noteOff) { p.gotOff = true; p.release  = e.releaseVel; }
    if (collect && g.phase == GP_SOUNDING) {
      if (g.pressure < p.pressLo) p.pressLo = g.pressure;
      if (g.pressure > p.pressHi) p.pressHi = g.pressure;
      if (g.position < p.posLo)   p.posLo   = g.position;
      if (g.position > p.posHi)   p.posHi   = g.position;
    }
    *nowMs += DT;
  };

  for (int i = 0; i <= strikeSteps; i++) step(rest * i / strikeSteps, false);
  for (int i = 0; i < 350; i++) {
    float n = noise * ((rand() % 2001 - 1000) / 1000.0f);
    step(rest + lean * i / 350.0f + n, true);
  }
  for (int i = 0; i <= 40; i++) step((rest + lean) * (1.0f - i / 40.0f), false);
  for (int i = 0; i < 30; i++) step(0.0f, false);
  return p;
}

int main() {
  char buf[220];
  const GestureConfig &C = GESTURE_DEFAULT;
  printf("gesture tests\n=============\n");
  srand(11);

  // ---- CONTINUOUS --------------------------------------------------------
  printf("\ncontinuous: no critical point, no velocity\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    // Creep the key down one hair at a time. A continuous instrument must
    // begin sounding essentially at once, and be silent when it does.
    GestureEvent e = { false, false, 0, 0, false };
    float startedAt = -1.0f;
    for (float pos = 0.0f; pos < 0.30f; pos += 0.002f) {
      e = gestureUpdate(g, C, ART_CONTINUOUS, pos, t); t += DT;
      if (e.noteOn && startedAt < 0.0f) startedAt = pos;
    }
    snprintf(buf, sizeof buf, "note began at position %.3f (contOn %.3f)",
             startedAt, C.contOn);
    // The bound tracks contOn rather than a literal: the threshold is set by
    // crosstalk margin, and the taper is what keeps it inaudible. Assert the
    // note begins AT the threshold and nowhere near a struck instrument's
    // critical point, which is 0.400.
    check(startedAt > 0.0f && startedAt < C.contOn + 0.01f,
          "starts at the threshold, not at a critical point", buf);
    check(C.contOn < 0.15f, "and the threshold stays near the noise floor", buf);

    gestureInit(g); t = 0.0f;
    Played hard = play(g, C, ART_CONTINUOUS, 0.85f, 0.05f, 4.0f,  0.001f, &t);
    Played soft = play(g, C, ART_CONTINUOUS, 0.85f, 0.05f, 300.0f,0.001f, &t);
    snprintf(buf, sizeof buf, "fast strike vel=%.2f, slow strike vel=%.2f",
             hard.velocity, soft.velocity);
    check(fabsf(hard.velocity - soft.velocity) < 0.001f,
          "strike speed is discarded entirely", buf);
    snprintf(buf, sizeof buf, "position ranged %.2f..%.2f", hard.posLo, hard.posHi);
    check(hard.posHi - hard.posLo > 0.02f, "position tracks the lean", buf);
  }

  // ---- transient rejection -----------------------------------------------
  // A neighbour moving fast puts a one- or two-sample spike on a shared row
  // line. The deconvolution cannot remove it: it models settling toward a
  // STATIC equilibrium, and during rapid movement the previous column's value
  // is already stale. A continuous note must not fire on a spike that lasts
  // less time than any finger can.
  printf("\ncontinuous notes ignore single-sample transients\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    int fired = 0;
    for (int n = 0; n < 40; n++) {
      for (int i = 0; i < 20; i++) {          // at rest
        if (gestureUpdate(g, C, ART_CONTINUOUS, 0.005f, t).noteOn) fired++;
        t += DT;
      }
      for (int i = 0; i < 2; i++) {           // a two-sample spike well over contOn
        if (gestureUpdate(g, C, ART_CONTINUOUS, 0.20f, t).noteOn) fired++;
        t += DT;
      }
    }
    snprintf(buf, sizeof buf, "40 spikes of 2 samples at 0.20 -> %d notes", fired);
    check(fired == 0, "spikes do not trigger", buf);

    // ...but a real press still must, and promptly.
    gestureInit(g);
    float began = -1.0f;
    for (int i = 0; i < 200; i++) {
      if (gestureUpdate(g, C, ART_CONTINUOUS, 0.20f, t).noteOn) began = (float)i;
      t += DT;
    }
    snprintf(buf, sizeof buf, "a sustained press fired at sample %.0f (%.0f ms)",
             began, began * DT);
    check(began >= 0.0f, "a real press still triggers", buf);
    check(began * DT < 15.0f, "and with no perceptible delay", buf);
  }

  // ---- STRUCK ------------------------------------------------------------
  printf("\nstruck: velocity from flight time\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    Played hard = play(g, C, ART_STRUCK, 0.85f, 0.04f, 6.0f,   0.001f, &t);
    Played soft = play(g, C, ART_STRUCK, 0.85f, 0.04f, 150.0f, 0.001f, &t);
    snprintf(buf, sizeof buf, "hard=%.2f soft=%.2f", hard.velocity, soft.velocity);
    check(hard.gotOn && soft.gotOn, "both notes fired", buf);
    check(hard.velocity > soft.velocity + 0.25f, "hard reads louder than soft", buf);
    check(hard.velocity <= 1.0f && soft.velocity > 0.0f, "velocity stays in range", buf);
  }

  // ---- PLUCKED -----------------------------------------------------------
  // A plectrum plucks identically however hard the key is hit, but the finger
  // can still move afterwards. Velocity discarded, pressure alive.
  printf("\nplucked: velocity discarded, pressure alive\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    play(g, C, ART_PLUCKED, 0.80f, 0.05f, 8.0f, 0.001f, &t);   // teach the width
    Played hard = play(g, C, ART_PLUCKED, 0.80f, 0.05f, 6.0f,   0.001f, &t);
    Played soft = play(g, C, ART_PLUCKED, 0.80f, 0.05f, 150.0f, 0.001f, &t);
    snprintf(buf, sizeof buf, "vel hard=%.2f soft=%.2f, pressure %.2f..%.2f",
             hard.velocity, soft.velocity, hard.pressLo, hard.pressHi);
    check(fabsf(hard.velocity - soft.velocity) < 0.001f, "velocity is discarded", buf);
    check(hard.pressHi - hard.pressLo > 0.4f, "pressure still swings", buf);
  }

  // ---- GATED -------------------------------------------------------------
  printf("\ngated: nothing after the start\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    play(g, C, ART_GATED, 0.80f, 0.05f, 8.0f, 0.001f, &t);
    Played p = play(g, C, ART_GATED, 0.80f, 0.05f, 8.0f, 0.001f, &t);
    snprintf(buf, sizeof buf, "vel=%.2f, pressure %.2f..%.2f",
             p.velocity, p.pressLo, p.pressHi);
    check(fabsf(p.velocity - 0.5f) < 0.001f, "velocity is discarded", buf);
    check(p.pressHi - p.pressLo < 0.001f, "pressure never moves", buf);
  }

  // ---- pressure is relative, position is absolute ------------------------
  // These are the two signals a patch chooses between, and the distinction is
  // the whole reason both exist. Strike depth must cancel in one and not the
  // other.
  printf("\npressure is relative, position is absolute\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    for (int i = 0; i < 4; i++) play(g, C, ART_PLUCKED, 0.75f, 0.05f, 8.0f, 0.001f, &t);
    Played shallow = play(g, C, ART_PLUCKED, 0.60f, 0.05f, 8.0f, 0.001f, &t);
    Played deep    = play(g, C, ART_PLUCKED, 0.90f, 0.05f, 8.0f, 0.001f, &t);
    snprintf(buf, sizeof buf,
             "shallow: press %.2f..%.2f pos %.2f..%.2f | deep: press %.2f..%.2f pos %.2f..%.2f",
             shallow.pressLo, shallow.pressHi, shallow.posLo, shallow.posHi,
             deep.pressLo, deep.pressHi, deep.posLo, deep.posHi);
    check(fabsf(shallow.pressLo - deep.pressLo) < 0.20f,
          "pressure starts near zero however deep the strike", buf);
    // Compare the SETTLED position, not the first sample of the hold: that
    // one is still catching up on the strike ramp through the smoother.
    check(deep.posHi > shallow.posHi + 0.20f,
          "position DOES reflect how deep the strike landed", buf);
  }

  // ---- noise -------------------------------------------------------------
  printf("\nADC noise at the true scan rate\n");
  for (float n : {0.0005f, 0.002f, 0.004f}) {
    GestureState g; gestureInit(g); float t = 0.0f;
    play(g, C, ART_PLUCKED, 0.80f, 0.05f, 8.0f, n, &t);
    Played p = play(g, C, ART_PLUCKED, 0.80f, 0.05f, 8.0f, n, &t);
    snprintf(buf, sizeof buf, "noise %.4f: width=%.4f pressure %.2f..%.2f",
             n, gestureWidth(g), p.pressLo, p.pressHi);
    check(p.pressHi - p.pressLo > 0.4f, "full swing survives the noise", buf);
  }

  // ---- a run of un-leaned notes must not switch pressure off -------------
  printf("\nnormal playing does not erase the learned width\n");
  {
    GestureState g; gestureInit(g); float t = 0.0f;
    for (int i = 0; i < 4; i++) play(g, C, ART_PLUCKED, 0.80f, 0.05f, 8.0f, 0.001f, &t);
    float before = gestureWidth(g);
    for (int i = 0; i < 20; i++) play(g, C, ART_PLUCKED, 0.80f, 0.0f, 8.0f, 0.001f, &t);
    Played p = play(g, C, ART_PLUCKED, 0.80f, 0.05f, 8.0f, 0.001f, &t);
    snprintf(buf, sizeof buf, "width %.4f -> %.4f, still swings %.2f..%.2f",
             before, gestureWidth(g), p.pressLo, p.pressHi);
    check(gestureWidth(g) > C.minWidth, "20 flat notes leave it intact", buf);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
#endif
