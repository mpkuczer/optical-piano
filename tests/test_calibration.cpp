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

// One press: ride from `baseline` down to `bottom` and back. The flag means
// "a note is in progress", which is what licenses learning this key's bottom.
static const bool NOTE = true, NO_NOTE = false;

static void press(CalKey &k, uint16_t baseline, uint16_t bottom) {
  for (int i = 0; i <= 60; i++)
    calUpdate(k, CAL_DEFAULT, (uint16_t)(baseline - (baseline-bottom)*i/60), NOTE);
  for (int i = 0; i < 200; i++) calUpdate(k, CAL_DEFAULT, bottom, NOTE);
  for (int i = 0; i <= 60; i++)
    calUpdate(k, CAL_DEFAULT, (uint16_t)(bottom + (baseline-bottom)*i/60), NOTE);
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
    float mid = calUpdate(k, CAL_DEFAULT, 2236, NOTE);
    float deep = calUpdate(k, CAL_DEFAULT, 600, NOTE);
    float bot = calUpdate(k, CAL_DEFAULT, 400, NOTE);
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
    float p = calUpdate(k, CAL_DEFAULT, 400, NOTE);
    snprintf(buf, sizeof buf, "after shallow play travel=%.0f, then raw400 -> %.2f",
             shallow, p);
    check(p > 0.98f, "a deep press expands live, never clips", buf);
  }

  // --- baseline tracking --------------------------------------------------
  printf("\nbaseline tracking\n");
  {
    CalKey k; calInit(k, 4072, CAL_DEFAULT);
    for (int i = 0; i < 20000; i++) calUpdate(k, CAL_DEFAULT, 3950, NO_NOTE);
    snprintf(buf, sizeof buf, "baseline followed drift to %.0f", k.baseline);
    check(fabsf(k.baseline - 3950.0f) < 20.0f, "follows a drifting rest level", buf);

    CalKey k2; calInit(k2, 4072, CAL_DEFAULT);
    for (int i = 0; i < 2000; i++) calUpdate(k2, CAL_DEFAULT, 1500, NOTE);
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
  // A note that barely moved must not redefine the key's bottom. A drift- or
  // noise-triggered note that only reached 40 counts deep used to teach
  // exactly that, collapsing travel and making the key erratic ever after.
  printf("\na shallow note teaches nothing\n");
  {
    CalKey k; calInit(k, 4072, CAL_DEFAULT);
    for (int i = 0; i < 3; i++) press(k, 4072, 500);
    float goodBottom = k.bottom;
    press(k, 4072, 4030);                     // 42 counts: not a real press
    // The BOTTOM is the learned quantity and must be untouched. Travel may
    // shift by a few counts because the baseline correctly tracks a key that
    // sits 42 counts down and stationary — that is the drift recovery
    // working, not the bottom being corrupted.
    snprintf(buf, sizeof buf, "bottom %.0f -> %.0f, travel %.0f after a 42-count 'note'",
             goodBottom, k.bottom, calTravel(k));
    check(fabsf(k.bottom - goodBottom) < 1.0f, "the learned bottom is untouched", buf);
  }

  printf("\nborrowed travel\n");
  {
    CalKey taught; calInit(taught, 4072, CAL_DEFAULT);
    for (int i = 0; i < 3; i++) press(taught, 4072, 500);
    CalKey fresh; calInit(fresh, 3900, CAL_DEFAULT);
    calSeedTravel(fresh, CAL_DEFAULT, calTravel(taught));
    float p = calUpdate(fresh, CAL_DEFAULT, 3900 - (int)calTravel(taught), NOTE);
    snprintf(buf, sizeof buf, "borrowed %.0f counts -> full press reads %.2f",
             calTravel(taught), p);
    check(p > 0.95f, "usable on its very first note", buf);
    press(fresh, 3900, 700);
    snprintf(buf, sizeof buf, "after its own press: travel=%.0f, borrowed=%d",
             calTravel(fresh), (int)fresh.borrowed);
    check(!fresh.borrowed, "first real press replaces the borrowed value", buf);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
#endif
