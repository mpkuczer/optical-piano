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
#include "Press.h"
#include <cstdio>
#include <cmath>
#include <vector>
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
