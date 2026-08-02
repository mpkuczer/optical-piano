// ============================================================================
// test_press.cpp — desktop tests for the pressure tracker
// ============================================================================
//   ./run_tests.sh      (or: g++ -std=c++17 -O2 test_press.cpp Press.cpp)
//
// Fixtures are REAL depth traces captured from the instrument, plus synthetic
// sequences for the failure modes real captures are too short to expose.
// ============================================================================
#ifdef ARDUINO
#warning "test_press.cpp is in the sketch folder — move it to test/"
#else

#include "Press.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what, const std::string &detail) {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         detail.empty() ? "" : "  -- ", detail.c_str());
  if (!ok) failures++;
}

// Key s16, captured from the instrument: slow press, held, leaned, released.
static const std::vector<float> S16 = {
  0.103,0.249,0.249,0.255,0.268,0.322,0.382,0.389,0.427,0.452,0.541,0.663,
  0.788,0.878,1.053,1.138,1.230,1.236,1.237,1.238,1.234,1.232,1.238,1.252,
  1.227,1.255,0.773,0.566,0.500,0.319,0.263,0.230,0.119
};

// One synthetic note: strike to `rest`, hold ~900 ms leaning by `lean`,
// release. `noise` is per-sample ADC noise in depth units.
struct Note { float lo, hi; };
static Note playNote(PressState &s, float rest, float lean, float noise) {
  pressNoteOn(s);
  float age = 0.0f, dt = 1.2f, lo = 2.0f, hi = -1.0f;
  for (int k = 0; k < 33; k++) {
    pressUpdate(s, PRESS_DEFAULT, 0.05f + (rest - 0.05f) * k / 33.0f, age, dt);
    age += dt;
  }
  for (int k = 0; k < 750; k++) {
    float d = rest + lean * k / 750.0f
            + noise * ((rand() % 2001 - 1000) / 1000.0f);
    float p = pressUpdate(s, PRESS_DEFAULT, d, age, dt);
    age += dt;
    if (p < lo) lo = p;
    if (p > hi) hi = p;
  }
  for (int k = 0; k < 80; k++) {
    pressUpdate(s, PRESS_DEFAULT, rest + lean - (rest + lean) * k / 80.0f, age, dt);
    age += dt;
  }
  pressNoteOff(s, PRESS_DEFAULT);
  return { lo, hi };
}

int main() {
  char buf[200];
  printf("pressure tracker tests\n======================\n");

  // --- THE REGRESSION THAT BIT ON HARDWARE --------------------------------
  // Windows grew to 0.28, 0.50, even 0.72 of span from leans of a few
  // hundredths, because strike depth varies note to note and asymmetric
  // learning ratcheted it in. The learned quantity must stay bounded.
  printf("\nmany notes, varying strike depth (the hardware regression)\n");
  {
    PressState s; pressInit(s); srand(3);
    for (int n = 0; n < 40; n++) {
      float rest = 1.00f + (rand() % 301 - 150) / 1000.0f;   // +/- 0.15
      playNote(s, rest, 0.05f, 0.002f);
    }
    snprintf(buf, sizeof buf, "width=%.4f after 40 notes (lean was 0.050)",
             pressRange(s));
    check(pressRange(s) < 0.09f, "width does not run away", buf);
    check(pressRange(s) > 0.030f, "width still tracks the real lean", buf);
  }

  // Strike depth must not leak into pressure: a deep strike is not a lean.
  printf("\nstrike depth must not read as pressure\n");
  {
    PressState s; pressInit(s); srand(5);
    for (int n = 0; n < 8; n++) playNote(s, 1.00f, 0.05f, 0.002f);
    Note shallow = playNote(s, 0.90f, 0.05f, 0.002f);
    Note deep    = playNote(s, 1.30f, 0.05f, 0.002f);
    snprintf(buf, sizeof buf, "shallow strike %.2f..%.2f, deep strike %.2f..%.2f",
             shallow.lo, shallow.hi, deep.lo, deep.hi);
    check(deep.lo < 0.30f, "deep strike still starts near zero", buf);
    check(shallow.hi > 0.60f && deep.hi > 0.60f, "both reach full lean", buf);
  }

  // --- ADC noise at the true scan rate ------------------------------------
  for (int counts : {1, 2, 4, 8}) {
    printf("\n%d counts of ADC noise\n", counts);
    PressState s; pressInit(s); srand(7);
    float n = counts / 2250.0f;
    playNote(s, 1.15f, 0.05f, n);
    Note r = playNote(s, 1.15f, 0.05f, n);
    snprintf(buf, sizeof buf, "width=%.4f  pressure %.2f..%.2f",
             pressRange(s), r.lo, r.hi);
    check(pressRange(s) > 0.030f && pressRange(s) < 0.090f,
          "learns the injected 0.050 lean", buf);
    check(r.hi - r.lo > 0.55f, "full swing survives the noise", buf);
  }

  // --- a key that is never leaned on --------------------------------------
  printf("\nflat hold — no lean at all\n");
  {
    PressState s; pressInit(s); srand(11);
    for (int i = 0; i < 5; i++) playNote(s, 1.20f, 0.000f, 0.002f);
    snprintf(buf, sizeof buf, "width=%.4f", pressRange(s));
    check(pressRange(s) < PRESS_DEFAULT.minRange, "no window without a lean", buf);
  }

  // ...and must not have its width destroyed by such notes afterwards.
  printf("\nnormal playing must not switch pressure off\n");
  {
    PressState s; pressInit(s); srand(13);
    for (int i = 0; i < 5; i++) playNote(s, 1.10f, 0.05f, 0.002f);
    float before = pressRange(s);
    for (int i = 0; i < 20; i++) playNote(s, 1.10f, 0.000f, 0.002f);
    Note r = playNote(s, 1.10f, 0.05f, 0.002f);
    snprintf(buf, sizeof buf, "width %.4f -> %.4f, still swings %.2f..%.2f",
             before, pressRange(s), r.lo, r.hi);
    check(pressRange(s) > 0.030f, "20 un-leaned notes do not erase it", buf);
  }

  // --- borrowing ----------------------------------------------------------
  printf("\nborrowed width — a key never played before\n");
  {
    PressState taught; pressInit(taught); srand(17);
    for (int i = 0; i < 4; i++) playNote(taught, 1.10f, 0.05f, 0.002f);
    PressState fresh; pressInit(fresh);
    pressSeedFrom(fresh, taught.width);
    Note r = playNote(fresh, 1.35f, 0.05f, 0.002f);   // different rest depth
    snprintf(buf, sizeof buf, "borrowed %.4f -> pressure %.2f..%.2f, own %.4f",
             taught.width, r.lo, r.hi, fresh.width);
    check(r.hi - r.lo > 0.50f, "responds on its very first note", buf);
    check(!fresh.borrowed, "first real note replaces the borrowed width", buf);
  }

  // --- the real capture ---------------------------------------------------
  printf("\ns16 — real capture replayed twice\n");
  {
    PressState s; pressInit(s);
    for (int pass = 0; pass < 2; pass++) {
      pressNoteOn(s);
      float age = 0.0f, lo = 2.0f, hi = -1.0f;
      for (float d : S16) {
        float p = pressUpdate(s, PRESS_DEFAULT, d, age, 60.0f);
        if (d > 1.15f) { if (p < lo) lo = p; if (p > hi) hi = p; }
        age += 60.0f;
      }
      pressNoteOff(s, PRESS_DEFAULT);
      if (pass) {
        snprintf(buf, sizeof buf, "width=%.4f  pressure %.2f..%.2f",
                 pressRange(s), lo, hi);
        check(pressRange(s) > 0.015f && pressRange(s) < 0.09f,
              "learns the captured lean", buf);
        check(hi - lo > 0.35f, "swings across the captured hold", buf);
      }
    }
  }

  // --- ABSOLUTE MODE: the property the whole gesture idea rests on --------
  printf("\nabsolute mode — memoryless, no strike dependence\n");
  {
    PressState s; pressInit(s); pressSetMode(s, PRESS_ABSOLUTE);
    // Same depth reached three ways: gently, violently, and after a long
    // hold. A continuous instrument must give the same answer every time.
    auto settle = [&](float from, float to, int steps) {
      pressNoteOn(s); pressSetMode(s, PRESS_ABSOLUTE);
      for (int k = 0; k < steps; k++)
        pressUpdate(s, PRESS_DEFAULT, from + (to - from) * k / steps, 0.0f, 1.2f);
      for (int k = 0; k < 400; k++)      // settle at `to`
        pressUpdate(s, PRESS_DEFAULT, to, 0.0f, 1.2f);
      return s.value;
    };
    float slow = settle(0.05f, 0.60f, 800);
    float fast = settle(0.05f, 0.60f, 8);
    float deep = settle(1.20f, 0.60f, 8);     // arriving from BELOW
    snprintf(buf, sizeof buf, "slow=%.3f fast=%.3f from-deep=%.3f", slow, fast, deep);
    check(fabsf(slow - fast) < 0.01f, "strike speed does not change the result", buf);
    check(fabsf(slow - deep) < 0.01f, "no hysteresis: path does not matter", buf);

    // And it must be a monotonic function of depth, reaching both ends.
    pressNoteOn(s); pressSetMode(s, PRESS_ABSOLUTE);
    float lo = settle(0.5f, 0.02f, 4), hi = settle(0.5f, 1.00f, 4);
    snprintf(buf, sizeof buf, "at rest=%.3f  at bottom=%.3f", lo, hi);
    check(lo < 0.02f, "silent with the key up", buf);
    check(hi > 0.95f, "full at the bottom", buf);

    // Nothing may be learned in this mode — no width, ever.
    check(pressRange(s) == 0.0f, "learns nothing, remembers nothing", "");
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

#endif  // ARDUINO
