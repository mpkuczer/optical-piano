// ============================================================================
// test_press.cpp — desktop tests for the pressure tracker
// ============================================================================
//   g++ -std=c++17 -Wall -O2 -o test_press test_press.cpp Press.cpp && ./test_press
//
// Fixtures are REAL depth traces captured from the instrument with the 'm'
// monitor, not invented numbers. Two of these tests fail against the previous
// implementations, which is the point: both bugs were reproducible on a
// desktop in milliseconds and neither needed the board.
// ============================================================================
// This file is a DESKTOP test and must never be compiled into the sketch.
// It defines main(). The Arduino build compiles every .cpp in the sketch
// folder, and because the core's main() lives in an archive it loses to a
// main() defined in a plain object file — the board then silently runs this
// instead of setup()/loop(), enumerates over USB, and answers nothing.
// Keep it in test/ (the Arduino build ignores subfolders other than src/),
// and the guard below makes the mistake harmless if it ever moves.
#ifdef ARDUINO
#warning "test_press.cpp is in the sketch folder — move it to test/"
#else

#include "Press.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <utility>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string &what, const std::string &detail) {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         detail.empty() ? "" : "  -- ", detail.c_str());
  if (!ok) failures++;
}

// ---- fixtures --------------------------------------------------------------
// Key s16, captured 2026-08-02. Slow press, held, leaned, released.
static const std::vector<float> S16 = {
  0.103,0.249,0.249,0.255,0.268,0.322,0.382,0.389,0.427,0.452,0.541,0.663,
  0.788,0.878,1.053,1.138,1.230,1.236,1.237,1.238,1.234,1.232,1.238,1.252,
  1.227,1.255,0.773,0.566,0.500,0.319,0.263,0.230,0.119
};
// Key s14, captured earlier. Same gesture, different key.
static const std::vector<float> S14 = {
  0.100,0.174,0.178,0.181,0.182,0.184,0.200,0.211,0.228,0.280,0.304,0.369,
  0.409,0.537,0.544,0.541,0.550,0.598,0.652,0.745,0.952,0.994,1.024,1.024,
  1.024,1.028,1.036,1.045,1.056,1.060,1.083,1.078,1.074,1.068,0.980,0.910,
  0.704,0.492,0.484,0.485,0.484,0.484,0.469,0.424,0.204,0.199,0.192,0.160
};
// A press held dead still: no lean at all. Must NOT invent expression.
static std::vector<float> flatHold() {
  std::vector<float> v = {0.10,0.35,0.70,1.05,1.19};
  for (int i = 0; i < 40; i++) v.push_back(1.200 + (i % 3) * 0.004);  // noise
  for (float d : {0.80,0.50,0.30,0.12}) v.push_back(d);
  return v;
}

// Linear interpolation between captured samples, to a finer time grid.
static std::vector<float> upsample(const std::vector<float> &in,
                                   float fromDt, float toDt) {
  std::vector<float> out;
  int steps = (int)(fromDt / toDt);
  for (size_t i = 0; i + 1 < in.size(); i++)
    for (int k = 0; k < steps; k++)
      out.push_back(in[i] + (in[i+1] - in[i]) * ((float)k / steps));
  out.push_back(in.back());
  return out;
}

struct Result { float lo, hi, ref, top; };

// Replay a trace. dtMs is the spacing between samples.
static Result replay(PressState &s, const std::vector<float> &trace,
                     float dtMs, bool collect) {
  pressNoteOn(s);
  float age = 0.0f, lo = 2.0f, hi = -1.0f;
  float plateau = 0.0f;
  for (float d : trace) if (d > plateau) plateau = d;
  for (float d : trace) {
    float p = pressUpdate(s, PRESS_DEFAULT, d, age, dtMs);
    // Sample the output only where the key is genuinely held near the bottom.
    if (collect && d > plateau - PRESS_DEFAULT.heldBand) {
      if (p < lo) lo = p;
      if (p > hi) hi = p;
    }
    age += dtMs;
  }
  pressNoteOff(s, PRESS_DEFAULT);
  return { lo, hi, s.ref, s.top };
}

static void suite(const char *name, const std::vector<float> &trace,
                  float dtMs, float expectPlateauLo, float expectPlateauHi) {
  printf("\n%s (%zu samples @ %.0f ms)\n", name, trace.size(), dtMs);
  PressState s; pressInit(s);

  Result r1 = replay(s, trace, dtMs, false);

  // 1. The learned window must sit inside the held plateau, not span the
  //    whole key travel. The previous build learned ref=0.172 top=1.373.
  char buf[160];
  snprintf(buf, sizeof buf, "learned ref=%.3f top=%.3f (range %.3f)",
           r1.ref, r1.top, r1.top - r1.ref);
  check(r1.ref > expectPlateauLo - 0.02f, "window floor is inside the hold", buf);
  check(r1.top - r1.ref < 0.20f, "window is not the whole travel", buf);

  // 2. The release ramp must not pull the floor down.
  check(r1.ref > 0.5f, "release ramp excluded from learning", buf);

  // 3. On the next note, pressure must actually swing across the plateau.
  Result r2 = replay(s, trace, dtMs, true);
  snprintf(buf, sizeof buf, "pressure %.2f .. %.2f (swing %.2f)",
           r2.lo, r2.hi, r2.hi - r2.lo);
  check(r2.hi - r2.lo > 0.35f, "usable pressure swing while held", buf);

  // 4. Pressure must be near zero at the shallow end of the hold.
  check(r2.lo < 0.35f, "rests near zero when not leaning", buf);

  (void)expectPlateauHi;
}

// A synthetic press at the TRUE scan rate WITH per-sample ADC noise. The
// interpolated fixture above cannot cover this: linear interpolation is
// perfectly smooth, so it silently erased the very noise the stillness
// detector has to survive. Injected lean is 0.050 of span.
static void noiseSuite(int counts) {
  char name[80];
  snprintf(name, sizeof name, "synthetic press, %d counts of ADC noise", counts);
  printf("\n%s\n", name);
  float n = counts / 2250.0f;
  PressState s; pressInit(s);
  srand(7);
  auto once = [&](bool collect) {
    pressNoteOn(s);
    float age = 0.0f, dt = 1.2f, lo = 2.0f, hi = -1.0f;
    for (int k = 0; k < 33; k++) {           // 40 ms strike
      pressUpdate(s, PRESS_DEFAULT, 0.05f + 1.10f * k / 33.0f, age, dt); age += dt;
    }
    for (int k = 0; k < 750; k++) {          // 900 ms held, leaning 0.05
      float d = 1.15f + 0.05f * k / 750.0f
              + n * ((rand() % 2001 - 1000) / 1000.0f);
      float p = pressUpdate(s, PRESS_DEFAULT, d, age, dt); age += dt;
      if (collect) { if (p < lo) lo = p; if (p > hi) hi = p; }
    }
    for (int k = 0; k < 80; k++) {           // 96 ms release
      pressUpdate(s, PRESS_DEFAULT, 1.20f - 1.10f * k / 80.0f, age, dt); age += dt;
    }
    pressNoteOff(s, PRESS_DEFAULT);
    return std::pair<float,float>(lo, hi);
  };
  once(false);
  auto r = once(true);
  char buf[160];
  snprintf(buf, sizeof buf, "range=%.4f  pressure %.2f..%.2f",
           pressRange(s), r.first, r.second);
  check(pressRange(s) > 0.030f && pressRange(s) < 0.070f,
        "learns the injected 0.050 lean", buf);
  check(r.second - r.first > 0.60f, "full swing survives the noise", buf);
}

int main() {
  printf("pressure tracker tests\n======================");
  suite("s16 — real capture", S16, 60.0f, 1.227f, 1.255f);
  suite("s14 — real capture", S14, 60.0f, 1.024f, 1.083f);

  // The same gesture at the real scan rate. The capture is 60 ms per sample
  // because that is the monitor's print interval; the scanner actually sees
  // it at ~1.2 ms. Interpolate rather than replaying faster — the gesture
  // took two seconds and must still take two seconds.
  suite("s16 — interpolated to true scan rate", upsample(S16, 60.0f, 1.2f),
        1.2f, 1.227f, 1.255f);

  for (int counts : {1, 2, 4, 8}) noiseSuite(counts);

  printf("\nborrowed window — a key that has never been played\n");
  {
    PressState taught; pressInit(taught);
    replay(taught, S16, 60.0f, false);        // teach one key
    PressState fresh; pressInit(fresh);
    pressSeedFrom(fresh, taught.ref, taught.top);
    Result r = replay(fresh, S16, 60.0f, true);
    char buf[160];
    snprintf(buf, sizeof buf, "borrowed [%.3f,%.3f] -> pressure %.2f..%.2f, "
             "own window after one note [%.3f,%.3f]",
             taught.ref, taught.top, r.lo, r.hi, fresh.ref, fresh.top);
    check(r.hi - r.lo > 0.35f, "responds on its very FIRST note", buf);
    check(!fresh.borrowed, "first real note replaces the borrowed window", buf);
    check(fresh.ref > 1.0f, "own data, not an average with the neighbour", buf);
  }

  printf("\nflat hold — no lean at all\n");
  {
    PressState s; pressInit(s);
    auto t = flatHold();
    replay(s, t, 1.2f, false);
    Result r = replay(s, t, 1.2f, true);
    char buf[160];
    snprintf(buf, sizeof buf, "range=%.4f  output %.2f..%.2f",
             pressRange(s), r.lo, r.hi);
    check(pressRange(s) < PRESS_DEFAULT.minRange * 2.0f,
          "narrow window when the key never leans", buf);
    check(r.hi < 0.9f, "does not amplify ADC noise into full-scale", buf);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

#endif  // ARDUINO
