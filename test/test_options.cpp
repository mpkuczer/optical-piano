// ============================================================================
// test_options.cpp — LAYER 2b tests
// ============================================================================
// Options mode is a timed state machine with two non-obvious rules — the
// re-arm latch and the nav arm delay — and every bug worth the name in this
// project has lived in a state machine that could not be tested. This is that
// file for this one.
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "Options.h"
#include <cstdio>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what, const std::string &d = "") {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         d.empty() ? "" : "  -- ", d.c_str());
  if (!ok) failures++;
}

// The sweep rate the instrument actually runs at: SETTLE_US 500 gives ~370 Hz,
// so ~2.7 ms per sweep. Using the real number means the sample-persistence
// counts in OPTIONS_DEFAULT are exercised at the rate they were chosen for.
static const float DT = 2.7f;

// A rig that plays keys at a clock, so a test reads as a sequence of gestures
// rather than as a loop over an array.
struct Rig {
  OptionsState s;
  float        pos[OPT_NUM_KEYS];
  float        now;
  // Everything that happened since the last time the test looked.
  int          entered, exited, deltaSum, deltaCount;

  Rig() : now(0.0f), entered(0), exited(0), deltaSum(0), deltaCount(0) {
    optionsInit(s);
    for (int i = 0; i < OPT_NUM_KEYS; i++) pos[i] = 0.0f;
  }

  void down(int k) { pos[k] = 0.90f; }   // well past keyOn
  void up(int k)   { pos[k] = 0.00f; }

  // Advance the clock, one sweep at a time, collecting events.
  void run(float ms) {
    for (float t = 0.0f; t < ms; t += DT) {
      now += DT;
      OptionsEvent e = optionsUpdate(s, OPTIONS_DEFAULT, pos, now);
      if (e.enter) entered++;
      if (e.exit)  exited++;
      if (e.presetDelta) { deltaSum += e.presetDelta; deltaCount++; }
    }
  }

  void clear() { entered = exited = deltaSum = deltaCount = 0; }

  // Enter the mode the way a player does, and leave the keys up afterwards —
  // which is the state most tests want to start from.
  void enterMode() {
    down(0); down(1); down(2);
    run(5400.0f);
    up(0); up(1); up(2);
    run(200.0f);
    clear();
  }
};

int main() {
  char buf[240];
  const OptionsConfig &C = OPTIONS_DEFAULT;
  printf("options tests\n=============\n");

  // --- the hold -----------------------------------------------------------
  printf("\nthe chord hold\n");
  {
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(4800.0f);
    check(r.entered == 0 && !optionsActive(r.s), "4.8 s is not yet enough");
    r.run(500.0f);
    check(r.entered == 1 && optionsActive(r.s), "5 s enters options mode");
  }
  {
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(4800.0f);
    r.up(1);
    r.run(1000.0f);
    check(r.entered == 0 && !optionsActive(r.s),
          "letting go at 4.8 s does not enter");
  }
  {
    // The timer restarts, it does not resume. Otherwise the gesture would be
    // "five seconds of chord in total, whenever", which nothing in a piece of
    // music is safe from.
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(4000.0f);
    r.up(1); r.run(200.0f); r.down(1);
    r.run(4000.0f);
    check(r.entered == 0, "releasing one key restarts the timer, not resumes it");
    r.run(1200.0f);
    check(r.entered == 1, "...and a full five seconds after that does enter");
  }
  {
    // Two of three is a real interval somebody plays. It must be inert
    // however long it is held.
    Rig r;
    r.down(0); r.down(1);
    r.run(12000.0f);
    check(r.entered == 0, "two of the three keys never enters, however long");
  }

  // --- the re-arm latch ---------------------------------------------------
  printf("\nthe re-arm latch\n");
  {
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(5200.0f);
    check(r.entered == 1, "entered");
    // You cannot release three keys in the millisecond the timer expires, so
    // the chord is necessarily still down here.
    r.run(11000.0f);
    snprintf(buf, sizeof buf, "enters=%d exits=%d", r.entered, r.exited);
    check(r.exited == 0 && optionsActive(r.s),
          "holding on past the transition does not flap the mode", buf);
    r.up(0); r.up(1); r.up(2);
    r.run(200.0f);
    r.down(0); r.down(1); r.down(2);
    r.run(5200.0f);
    check(r.exited == 1 && !optionsActive(r.s),
          "a fresh five-second hold leaves the mode");
  }
  {
    // Lifting one finger and putting it back is not a release. If it re-armed
    // the latch, an unsteady hand would get a second transition out of one
    // press.
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(5200.0f);
    r.clear();
    r.up(1); r.run(300.0f); r.down(1);
    r.run(11000.0f);
    check(r.exited == 0, "re-forming the chord without a full release stays latched");
  }

  // --- the chord/nav collision --------------------------------------------
  // Key 0 is both the bottom of the chord and the previous-preset key. This is
  // the case the arm delay exists for, and the one most likely to regress.
  printf("\nthe chord and the leftmost key\n");
  {
    bool bad = false;
    int  worst = -1;
    // Every stagger a hand can produce, from perfectly simultaneous out to
    // well past the arm delay.
    for (int lagMs = 0; lagMs <= 140; lagMs += 5) {
      Rig r;
      r.enterMode();
      r.down(0);                       // the nav key lands first
      r.run((float)lagMs);
      r.down(1); r.down(2);
      r.run(6000.0f);
      if (r.deltaCount != 0) { bad = true; if (worst < 0) worst = lagMs; }
      if (r.exited != 1) { bad = true; if (worst < 0) worst = lagMs; }
    }
    snprintf(buf, sizeof buf, "first failing stagger = %d ms", worst);
    check(!bad, "pressing the exit chord never changes preset, at any stagger",
          bad ? buf : "");
  }
  {
    // The other direction: the two upper chord keys first. Trivially safe,
    // but it is the ordering a right-handed player is most likely to produce.
    Rig r;
    r.enterMode();
    r.down(1); r.down(2);
    r.run(60.0f);
    r.down(0);
    r.run(6000.0f);
    check(r.deltaCount == 0 && r.exited == 1,
          "...and in the other order too");
  }
  {
    // The arm delay must not be so long that a real press is missed. A
    // deliberate tap is well over 150 ms, but check the boundary is where it
    // is claimed to be rather than somewhere convenient.
    Rig r;
    r.enterMode();
    r.down(0);
    r.run(C.navArmMs - 60.0f);
    check(r.deltaCount == 0, "a nav key does nothing before the arm delay");
    r.run(120.0f);
    snprintf(buf, sizeof buf, "delta=%d count=%d", r.deltaSum, r.deltaCount);
    check(r.deltaCount == 1 && r.deltaSum == -1,
          "and steps down once just after it", buf);
  }

  // --- navigation ---------------------------------------------------------
  printf("\nnavigation\n");
  {
    Rig r;
    r.enterMode();
    r.down(24);
    r.run(3000.0f);
    snprintf(buf, sizeof buf, "delta=%d count=%d", r.deltaSum, r.deltaCount);
    check(r.deltaCount == 1 && r.deltaSum == +1,
          "the rightmost key steps up exactly once, however long it is held", buf);
    r.up(24); r.run(200.0f);
    r.down(24); r.run(400.0f);
    check(r.deltaCount == 2 && r.deltaSum == 2, "and again on the next press");
  }
  {
    Rig r;   // NOT in options mode
    r.down(0);  r.run(1000.0f);
    r.up(0);    r.run(100.0f);
    r.down(24); r.run(1000.0f);
    check(r.deltaCount == 0, "the end keys change nothing while playing");
  }
  {
    // The other twenty-three keys are not controls in either mode.
    Rig r;
    r.enterMode();
    for (int k = 1; k < 24; k++) {
      r.down(k); r.run(500.0f); r.up(k); r.run(100.0f);
    }
    check(r.deltaCount == 0 && r.exited == 0,
          "the middle keys are inert as controls in options mode");
  }

  // --- the Schmitt trigger ------------------------------------------------
  printf("\nholding still\n");
  {
    // A finger resting at the trigger point wobbles. Between keyOn and keyOff
    // nothing may change, or a held chord would chatter and never accumulate
    // its five seconds.
    Rig r;
    r.enterMode();
    r.pos[24] = 0.90f;
    r.run(400.0f);
    check(r.deltaCount == 1, "pressed");
    for (int i = 0; i < 300; i++) {   // dither inside the hysteresis band
      r.pos[24] = (i & 1) ? 0.34f : 0.26f;
      r.run(DT);
    }
    snprintf(buf, sizeof buf, "count=%d", r.deltaCount);
    check(r.deltaCount == 1, "dithering below keyOn does not re-fire", buf);
  }
  {
    // Crosstalk. GESTURE_DEFAULT had to be raised to 0.150 because the
    // residual on this hardware reaches it; options must be nowhere near.
    Rig r;
    for (int k = 0; k < OPT_NUM_KEYS; k++) r.pos[k] = 0.16f;
    r.run(20000.0f);
    check(r.entered == 0,
          "every key sitting at the crosstalk ceiling never enters the mode");
  }

  // --- the console path ---------------------------------------------------
  printf("\nforcing the mode\n");
  {
    Rig r;
    OptionsEvent e = optionsForce(r.s, C, true);
    check(e.enter && !e.exit && optionsActive(r.s), "'O' enters");
    e = optionsForce(r.s, C, true);
    check(!e.enter && !e.exit, "forcing a mode it is already in is a no-op");
    e = optionsForce(r.s, C, false);
    check(e.exit && !optionsActive(r.s), "'O' again leaves");
  }
  {
    // A forced entry must arm the latch too. Otherwise typing 'O' with a hand
    // resting on the bottom three keys would drop straight back out.
    Rig r;
    r.down(0); r.down(1); r.down(2);
    r.run(1000.0f);
    optionsForce(r.s, C, true);
    r.clear();
    r.run(11000.0f);
    check(r.exited == 0, "'O' with the chord already held does not bounce out");
  }

  // --- what the scanner asks ----------------------------------------------
  printf("\nreserved keys\n");
  {
    Rig r;
    check(!optionsReserved(r.s, C, 0) && !optionsReserved(r.s, C, 24),
          "no key is reserved while playing");
    r.enterMode();
    check(optionsReserved(r.s, C, 0) && optionsReserved(r.s, C, 24),
          "both end keys are reserved in options mode");
    bool anyMiddle = false;
    for (int k = 1; k < 24; k++) if (optionsReserved(r.s, C, k)) anyMiddle = true;
    check(!anyMiddle, "and nothing else is — 23 keys keep playing");
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
#endif
