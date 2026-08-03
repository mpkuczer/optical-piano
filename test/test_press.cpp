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

    // Calibrate first. Memorylessness is a property of a CALIBRATED key:
    // pressing deeper than the key has ever gone must extend the range (the
    // alternative is clipping the bottom of the travel, which is worse), so
    // the property only holds once the bottom is known.
    const float BOTTOM = 1.20f;
    for (int n = 0; n < 3; n++) {
      pressNoteOn(s); pressSetMode(s, PRESS_ABSOLUTE);
      for (int i = 0; i <= 400; i++)
        pressUpdate(s, PRESS_DEFAULT, 0.05f + (BOTTOM-0.05f)*i/400.0f, 0.0f, 1.2f);
      pressNoteOff(s, PRESS_DEFAULT);
    }

    // Same depth reached three ways: gently, violently, and by coming back up
    // from the bottom. A continuous instrument must answer the same each time.
    auto hold = [&](float from, float to, int steps) {
      pressNoteOn(s); pressSetMode(s, PRESS_ABSOLUTE);
      for (int k = 0; k < steps; k++)
        pressUpdate(s, PRESS_DEFAULT, from + (to-from)*k/steps, 0.0f, 1.2f);
      float last = 0.0f;
      for (int k = 0; k < 400; k++)
        last = pressUpdate(s, PRESS_DEFAULT, to, 0.0f, 1.2f);
      return last;                       // deliberately no noteOff: measuring,
    };                                   // not teaching

    float slow = hold(0.05f, 0.60f, 800);
    float fast = hold(0.05f, 0.60f, 8);
    float deep = hold(BOTTOM, 0.60f, 8);      // arriving from BELOW
    snprintf(buf, sizeof buf, "top=%.2f slow=%.3f fast=%.3f from-deep=%.3f",
             s.absTop, slow, fast, deep);
    check(fabsf(slow - fast) < 0.01f, "strike speed does not change the result", buf);
    check(fabsf(slow - deep) < 0.01f, "no hysteresis: path does not matter", buf);

    float lo = hold(0.30f, 0.02f, 4), hi = hold(0.30f, BOTTOM, 4);
    snprintf(buf, sizeof buf, "at rest=%.3f  at the learned bottom=%.3f", lo, hi);
    check(lo < 0.02f, "silent with the key up", buf);
    check(hi > 0.95f, "full at the key's own bottom", buf);

    // THE BUG FROM HARDWARE: with a global top of 0.95 and a boot span of
    // 2250, a key whose real travel reaches depth 1.68 saturated at raw 1934
    // and the whole bottom half of the key did nothing.
    {
      PressState k; pressInit(k); pressSetMode(k, PRESS_ABSOLUTE);
      auto sweep = [&](float to) {
        float last = 0.0f;
        for (int i = 0; i <= 400; i++)
          last = pressUpdate(k, PRESS_DEFAULT, to * i / 400.0f, 0.0f, 1.2f);
        return last;
      };
      sweep(1.68f); pressNoteOff(k, PRESS_DEFAULT);   // one full press teaches it
      pressNoteOn(k); pressSetMode(k, PRESS_ABSOLUTE);
      float mid = 0.0f, deep = 0.0f;
      for (int i = 0; i < 300; i++) mid  = pressUpdate(k, PRESS_DEFAULT, 1.14f, 0.0f, 1.2f);
      for (int i = 0; i < 300; i++) deep = pressUpdate(k, PRESS_DEFAULT, 1.60f, 0.0f, 1.2f);
      snprintf(buf, sizeof buf, "top=%.2f  at depth 1.14 -> %.2f, at 1.60 -> %.2f",
               k.absTop, mid, deep);
      check(mid < 0.85f, "mid-travel is NOT saturated", buf);
      check(deep > mid + 0.20f, "the bottom of the travel still moves", buf);
    }

    // SPAN INDEPENDENCE: the struck path maintains `span`, the continuous
    // path does not. The same physical gesture measured against two different
    // spans must still produce the same pressure, or playing a piano patch
    // silently rescales every theremin key.
    {
      float outs[2];
      float spans[2] = { 2250.0f, 3600.0f };
      for (int j = 0; j < 2; j++) {
        PressState k; pressInit(k); pressSetMode(k, PRESS_ABSOLUTE);
        const float baseline = 4072.0f, bottom = 400.0f;
        auto depthOf = [&](float raw) { return (baseline - raw) / spans[j]; };
        for (int pass = 0; pass < 4; pass++) {          // learn the bottom
          pressNoteOn(k); pressSetMode(k, PRESS_ABSOLUTE);
          for (int i = 0; i <= 400; i++)
            pressUpdate(k, PRESS_DEFAULT,
                        depthOf(baseline - (baseline-bottom)*i/400.0f), 0.0f, 1.2f);
          pressNoteOff(k, PRESS_DEFAULT);
        }
        pressNoteOn(k); pressSetMode(k, PRESS_ABSOLUTE);
        for (int i = 0; i < 400; i++)                   // hold at raw 2200
          outs[j] = pressUpdate(k, PRESS_DEFAULT, depthOf(2200.0f), 0.0f, 1.2f);
      }
      snprintf(buf, sizeof buf, "span 2250 -> %.3f, span 3600 -> %.3f",
               outs[0], outs[1]);
      check(fabsf(outs[0] - outs[1]) < 0.02f,
            "same gesture, different span, same pressure", buf);
    }

    // The taper must keep the endpoints exact and stay monotonic — a curve
    // that fails either is a bug, not a preference.
    {
      PressState k; pressInit(k); pressSetMode(k, PRESS_ABSOLUTE);
      for (int i = 0; i <= 400; i++)
        pressUpdate(k, PRESS_DEFAULT, 1.20f*i/400.0f, 0.0f, 1.2f);
      pressNoteOff(k, PRESS_DEFAULT);
      float prev = -1.0f; bool mono = true;
      for (int i = 0; i <= 20; i++) {
        pressNoteOn(k); pressSetMode(k, PRESS_ABSOLUTE);
        float v = 0.0f;
        for (int j = 0; j < 300; j++)
          v = pressUpdate(k, PRESS_DEFAULT, 1.20f*i/20.0f, 0.0f, 1.2f);
        if (v < prev - 1e-4f) mono = false;
        prev = v;
      }
      snprintf(buf, sizeof buf, "gamma=%.1f, top of curve = %.3f",
               PRESS_DEFAULT.absGamma, prev);
      check(mono, "taper is monotonic", buf);
      check(prev > 0.98f, "taper still reaches full scale", buf);
    }

    // Nothing may be learned in this mode — no width, ever.
    check(pressRange(s) == 0.0f, "learns nothing, remembers nothing", "");
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

#endif  // ARDUINO
