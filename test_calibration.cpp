// ============================================================================
// test_calibration.cpp — LAYER 1 tests
// ============================================================================
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "Calibration.h"
#include <cstdio>
#include <cmath>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what, const std::string &d) {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         d.empty() ? "" : "  -- ", d.c_str());
  if (!ok) failures++;
}

// One press: ride from `baseline` down to `bottom` and back.
static void press(CalKey &k, uint16_t baseline, uint16_t bottom) {
  for (int i = 0; i <= 60; i++)
    calUpdate(k, CAL_DEFAULT, (uint16_t)(baseline - (baseline-bottom)*i/60), false);
  for (int i = 0; i < 200; i++) calUpdate(k, CAL_DEFAULT, bottom, false);
  for (int i = 0; i <= 60; i++)
    calUpdate(k, CAL_DEFAULT, (uint16_t)(bottom + (baseline-bottom)*i/60), false);
  calNoteEnded(k, CAL_DEFAULT);
}

int main() {
  char buf[200];
  printf("calibration tests\n=================\n");

  // --- the whole travel must be usable ------------------------------------
  // The hardware bug: a global 0.95 top against a boot span of 2250 saturated
  // at raw 1934, so everything below that did nothing.
  printf("\nfull travel is usable\n");
  {
    CalKey k; calInit(k, 4072, CAL_DEFAULT);
    press(k, 4072, 400);
    float mid = calUpdate(k, CAL_DEFAULT, 2236, false);
    float deep = calUpdate(k, CAL_DEFAULT, 600, false);
    float bot = calUpdate(k, CAL_DEFAULT, 400, false);
    snprintf(buf, sizeof buf, "travel=%.0f  raw2236->%.2f raw600->%.2f raw400->%.2f",
             calTravel(k), mid, deep, bot);
    check(mid > 0.4f && mid < 0.6f, "mid travel reads mid", buf);
    check(deep > 0.90f && deep < 1.0f, "deep travel is not saturated", buf);
    check(bot > 0.98f, "the bottom reads full", buf);
  }

  // --- ONE learned bottom, shared by every articulation -------------------
  // The cross-contamination bug: struck play rescaled continuous play. There
  // is now one number and everything updates it, so this cannot recur — but
  // assert it, because it is the single most expensive bug so far.
  printf("\none bottom, updated by every kind of press\n");
  {
    CalKey k; calInit(k, 4072, CAL_DEFAULT);
    for (int i = 0; i < 5; i++) press(k, 4072, 400);
    float before = calTravel(k);
    for (int i = 0; i < 20; i++) press(k, 4072, 400);   // more of the same
    float after = calTravel(k);
    snprintf(buf, sizeof buf, "travel %.0f -> %.0f over 20 more notes", before, after);
    check(fabsf(after - before) < 60.0f, "consistent play keeps it stable", buf);

    // A run of shallow presses should move it, but only partway, and the next
    // deep press must recover the range immediately rather than clipping.
    for (int i = 0; i < 8; i++) press(k, 4072, 2000);
    float shallow = calTravel(k);
    float p = calUpdate(k, CAL_DEFAULT, 400, false);
    snprintf(buf, sizeof buf, "after shallow play travel=%.0f, then raw400 -> %.2f",
             shallow, p);
    check(p > 0.98f, "a deep press expands live, never clips", buf);
  }

  // --- baseline tracking --------------------------------------------------
  printf("\nbaseline tracking\n");
  {
    CalKey k; calInit(k, 4072, CAL_DEFAULT);
    for (int i = 0; i < 20000; i++) calUpdate(k, CAL_DEFAULT, 3950, true);
    snprintf(buf, sizeof buf, "baseline followed drift to %.0f", k.baseline);
    check(fabsf(k.baseline - 3950.0f) < 20.0f, "follows a drifting rest level", buf);

    CalKey k2; calInit(k2, 4072, CAL_DEFAULT);
    for (int i = 0; i < 2000; i++) calUpdate(k2, CAL_DEFAULT, 1500, false);
    snprintf(buf, sizeof buf, "baseline after a long hold = %.0f", k2.baseline);
    check(fabsf(k2.baseline - 4072.0f) < 5.0f, "does NOT follow a held key", buf);
  }

  // --- a dead sensor ------------------------------------------------------
  // s12 read 72 counts no matter what. Deadness must fall out of the
  // arithmetic, not out of a magic threshold that also disabled healthy keys
  // resting at 4071.
  printf("\ndead sensor detection\n");
  {
    CalKey dead; calInit(dead, 72, CAL_DEFAULT);
    CalKey high; calInit(high, 4090, CAL_DEFAULT);
    CalKey norm; calInit(norm, 3803, CAL_DEFAULT);
    snprintf(buf, sizeof buf, "travels: stuck=%.0f  high-resting=%.0f  normal=%.0f",
             calTravel(dead), calTravel(high), calTravel(norm));
    check(calDead(dead, CAL_DEFAULT), "a stuck-low sensor is dead", buf);
    check(!calDead(high, CAL_DEFAULT), "a HEALTHY high-resting key is not", buf);
    check(!calDead(norm, CAL_DEFAULT), "a normal key is not", buf);
  }

  // --- borrowing ----------------------------------------------------------
  printf("\nborrowed travel\n");
  {
    CalKey taught; calInit(taught, 4072, CAL_DEFAULT);
    for (int i = 0; i < 3; i++) press(taught, 4072, 500);
    CalKey fresh; calInit(fresh, 3900, CAL_DEFAULT);
    calSeedTravel(fresh, CAL_DEFAULT, calTravel(taught));
    float p = calUpdate(fresh, CAL_DEFAULT, 3900 - (int)calTravel(taught), false);
    snprintf(buf, sizeof buf, "borrowed %.0f counts -> full press reads %.2f",
             calTravel(taught), p);
    check(p > 0.95f, "usable on its very first note", buf);
    press(fresh, 3900, 700);
    check(!fresh.borrowed, "first real press replaces the borrowed value", buf);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
#endif
