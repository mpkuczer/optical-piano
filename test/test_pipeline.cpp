// ============================================================================
// test_pipeline.cpp — Calibration + Gesture together, in RAW ADC COUNTS
// ============================================================================
// The unit tests cover each layer alone. This one replicates exactly what
// scannerScanOnce() does, sample for sample, because the bugs that survive
// unit tests are the ones that live BETWEEN layers — in this case, in what
// `idle` means to two modules that each define it slightly differently.
// ============================================================================
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "Calibration.h"
#include "Gesture.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string &w, const std::string &d) {
  printf("  [%s] %s%s%s\n", ok?"PASS":"FAIL", w.c_str(), d.empty()?"":"  -- ", d.c_str());
  if (!ok) failures++;
}

static const float DT = 2.0f;          // ~500 Hz per key

struct Key { CalKey cal; GestureState g; };

struct Result {
  int   onCount, offCount;
  float velocity;
  float posLo, posHi;
  float travel;
  float pressHi;      // highest pressure seen while sounding: aftertouch
  int   endedCount;   // noteEnded events: "a key finished a press"
};

// EXACTLY the body of scannerScanOnce's inner loop.
static void feed(Key &k, uint8_t art, uint16_t raw, float nowMs, Result &r) {
  float pos = calUpdate(k.cal, CAL_DEFAULT, raw, gestureIdle(k.g));
  GestureEvent e = gestureUpdate(k.g, GESTURE_DEFAULT, art, pos, nowMs);
  if (e.noteOn)  { r.onCount++;  r.velocity = e.velocity; }
  if (e.noteOff) { r.offCount++; }
  if (e.noteEnded) { r.endedCount++; calNoteEnded(k.cal, CAL_DEFAULT); }
  if (k.g.phase == GP_KEYDOWN) {
    if (k.g.position < r.posLo) r.posLo = k.g.position;
    if (k.g.position > r.posHi) r.posHi = k.g.position;
    if (k.g.pressure > r.pressHi) r.pressHi = k.g.pressure;
  }
}

// A realistic press on this instrument: rest 4072, bottom 500.
static Result press(Key &k, uint8_t art, float *t,
                    uint16_t rest = 4072, uint16_t bottom = 500,
                    float strikeMs = 40.0f, int holdSamples = 400,
                    uint16_t noise = 6) {
  Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
  int steps = (int)(strikeMs / DT); if (steps < 1) steps = 1;
  for (int i = 0; i <= steps; i++) {
    uint16_t v = (uint16_t)(rest - (rest - bottom) * (float)i / steps);
    feed(k, art, v, *t, r); *t += DT;
  }
  for (int i = 0; i < holdSamples; i++) {
    int n = noise ? (rand() % (2*noise+1)) - noise : 0;
    feed(k, art, (uint16_t)(bottom + n), *t, r); *t += DT;
  }
  for (int i = 0; i <= 30; i++) {
    uint16_t v = (uint16_t)(bottom + (rest - bottom) * (float)i / 30);
    feed(k, art, v, *t, r); *t += DT;
  }
  for (int i = 0; i < 60; i++) {           // settle at rest
    int n = noise ? (rand() % (2*noise+1)) - noise : 0;
    feed(k, art, (uint16_t)(rest + n), *t, r); *t += DT;
  }
  r.travel = calTravel(k.cal);
  return r;
}

static Key makeKey(uint16_t baseline) {
  Key k; calInit(k.cal, baseline, CAL_DEFAULT); gestureInit(k.g); return k;
}

int main() {
  char buf[240];
  printf("pipeline tests (calibration + gesture, raw counts)\n");
  printf("=================================================\n");
  srand(5);

  // ---- the basic contract: a press makes exactly one note -----------------
  // TRIGGERED is excluded on purpose and gets its own contract below. It is
  // the one articulation for which "one press, one note" is FALSE by design:
  // the first press starts a note and the second ends it, so a press is half a
  // note rather than a whole one.
  for (int a = 0; a < 4; a++) {
    const char *name[] = { "CONTINUOUS", "STRUCK", "GATED", "PLUCKED" };
    printf("\n%s: one press, one note\n", name[a]);
    Key k = makeKey(4072); float t = 0.0f;
    Result r1 = press(k, (uint8_t)a, &t);
    Result r2 = press(k, (uint8_t)a, &t);
    snprintf(buf, sizeof buf, "note-ons %d then %d, travel %.0f, position %.2f..%.2f",
             r1.onCount, r2.onCount, r2.travel, r2.posLo, r2.posHi);
    check(r1.onCount == 1 && r1.offCount == 1, "first press: exactly one note", buf);
    check(r2.onCount == 1 && r2.offCount == 1, "second press: exactly one note", buf);
    check(r2.posHi > 0.9f, "position reaches the bottom of the key", buf);
  }

  // ---- TRIGGERED: the latch ------------------------------------------------
  // The whole articulation, in one sequence: press, LET GO, press, let go.
  printf("\nTRIGGERED: press latches on, next press latches off\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result p1 = press(k, ART_TRIGGERED, &t);
    bool onAfter1 = gestureSounding(k.g), idleAfter1 = gestureIdle(k.g);
    Result p2 = press(k, ART_TRIGGERED, &t);
    bool onAfter2 = gestureSounding(k.g);
    Result p3 = press(k, ART_TRIGGERED, &t);
    bool onAfter3 = gestureSounding(k.g);

    snprintf(buf, sizeof buf,
             "press1 %d on/%d off -> sounding=%d, press2 %d on/%d off -> sounding=%d,"
             " press3 %d on/%d off -> sounding=%d",
             p1.onCount, p1.offCount, (int)onAfter1,
             p2.onCount, p2.offCount, (int)onAfter2,
             p3.onCount, p3.offCount, (int)onAfter3);
    check(p1.onCount == 1 && p1.offCount == 0,
          "the first press starts a note and does not end it", buf);
    // The key is BACK AT REST here and the note is still going. That sentence
    // is the entire difference from every other articulation.
    check(onAfter1 && idleAfter1,
          "with the key released, the note is still sounding", buf);
    check(p2.onCount == 0 && p2.offCount == 1,
          "the second press ends it and starts nothing", buf);
    check(!onAfter2, "and leaves nothing sounding", buf);
    check(p3.onCount == 1 && p3.offCount == 0 && onAfter3,
          "the third press starts a new one — it toggles, not one-shot", buf);
  }

  // THE FAILURE MODE THAT MATTERS. A latched note has no key holding it up and
  // no timeout, so anything that could spuriously toggle it is unbounded
  // damage: a phantom ON sounds forever, a phantom OFF kills a note nobody
  // touched. The defence is the trigger depth — onPos is 40% of travel, while
  // the noise and crosstalk that reach a resting key are a few percent — so
  // this checks the whole span between them, at noise levels well past what
  // the hardware produces, for as long as anyone would hold a chord.
  printf("\nTRIGGERED: a latched note is not toggled by noise on a resting key\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result latch = press(k, ART_TRIGGERED, &t);
    Result quiet = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
    for (int i = 0; i < 20000; i++) {           // 40 s, key at rest
      int n = (rand() % 51) - 25;               // +/-25 counts, ~0.8% of travel
      feed(k, ART_TRIGGERED, (uint16_t)(4072 + n), t, quiet); t += DT;
    }
    snprintf(buf, sizeof buf,
             "latched, then 40 s of +/-25 counts at rest: %d on / %d off, still"
             " sounding=%d", quiet.onCount, quiet.offCount,
             (int)gestureSounding(k.g));
    check(latch.onCount == 1 && quiet.onCount == 0 && quiet.offCount == 0,
          "nothing toggles either way", buf);
    check(gestureSounding(k.g), "and the note is still there at the end", buf);
  }

  // Calibration must not be collateral damage. It learns a key's bottom when a
  // PRESS finishes, which used to be the same event as a note ending; under a
  // latch the two have come apart in both directions, and calibration follows
  // the key. Both presses of a latch cycle must report it, or a latched key
  // stops contributing to its own travel estimate.
  printf("\nTRIGGERED: every completed press still feeds calibration\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    int endedOn = 0, endedOff = 0;
    auto pressCountingEnds = [&](int *ended) {
      Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
      const uint16_t bottom = 500;
      for (int i = 0; i <= 20; i++)
        { feed(k, ART_TRIGGERED, (uint16_t)(4072 - 3572.0f*i/20.0f), t, r); t += DT; }
      for (int i = 0; i < 200; i++) { feed(k, ART_TRIGGERED, bottom, t, r); t += DT; }
      for (int i = 0; i <= 30; i++)
        { feed(k, ART_TRIGGERED, (uint16_t)(bottom + 3572.0f*i/30.0f), t, r); t += DT; }
      // The key finished its press: gestureUpdate must have said so.
      for (int i = 0; i < 60; i++) { feed(k, ART_TRIGGERED, 4072, t, r); t += DT; }
      *ended = r.endedCount;
    };
    pressCountingEnds(&endedOn);
    pressCountingEnds(&endedOff);
    snprintf(buf, sizeof buf, "noteEnded fired %d time(s) on the latch-on press,"
             " %d on the latch-off press, travel %.0f", endedOn, endedOff,
             calTravel(k.cal));
    check(endedOn == 1, "the press that STARTS a note reports the key coming up", buf);
    check(endedOff == 1, "so does the press that ENDS one", buf);
  }

  // ---- TRIGGERED: the key as a switch -------------------------------------
  // A press over strikeMs, held, released. Everything below reads velocity out
  // of that, because velocity is the ONLY number the two triggered
  // articulations disagree about.
  auto pressVel = [&](uint8_t art, float strikeMs) {
    Key k = makeKey(4072); float t = 0.0f;
    return press(k, art, &t, 4072, 500, strikeMs).velocity;
  };

  printf("\nTRIGGERED: every press speaks\n");
  {
    // THE FLOOR. Past velSlowMs the map keeps extrapolating and clamps at zero,
    // which is right for a hammer and wrong for a switch: a note that triggered
    // and then produced silence is just a key that did not work.
    float s = pressVel(ART_STRUCK, 1200.0f), g = pressVel(ART_TRIGGERED, 1200.0f);
    snprintf(buf, sizeof buf, "1.2 s press: STRUCK v=%.3f, TRIGGERED v=%.3f"
             " (velAtSlow %.2f)", s, g, GESTURE_DEFAULT.velAtSlow);
    check(g >= GESTURE_DEFAULT.velAtSlow - 0.001f,
          "a very slow press still reads at the slow end of the map", buf);
    check(s < g, "where STRUCK is free to fade to nothing", buf);

    // ...and the floor must not have flattened the range onto itself. The
    // whole point is that the ATTACK still varies.
    float fast = pressVel(ART_TRIGGERED, 8.0f);
    snprintf(buf, sizeof buf, "8 ms press v=%.3f vs 1.2 s press v=%.3f", fast, g);
    check(fast > g + 0.5f, "a fast press still reads far harder", buf);
  }

  // MONOTONICITY, which is what dropping maxDescentMs actually buys. STRUCK
  // abandons a stalled descent and immediately re-arms the timer at the key's
  // current depth, so the flight it eventually reports is measured from the
  // moment the watchdog fired rather than from the start of the press. The
  // reading is then not a function of how the key was played: pressing SLOWER
  // can come out LOUDER, depending on where the 600 ms boundary happened to
  // land. Fine on a hammer, where the watchdog is guarding against a stale
  // timer that would otherwise make a fresh strike read as a crawl. Not fine
  // on a switch, whose whole contract is that it reports what you did.
  printf("\nTRIGGERED: a slower press never reads harder than a faster one\n");
  {
    // Two hesitations either side of maxDescentMs (600 ms), identical in every
    // other respect. The 700 ms press is unambiguously the slower gesture.
    auto hesitate = [&](uint8_t art, int pauseSamples) {
      Key k = makeKey(4072); float t = 0.0f;
      Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
      const uint16_t mid = (uint16_t)(4072 - 0.30f * 3572);
      for (int i = 0; i <= 30; i++)                       // 60 ms to 30% depth
        { feed(k, art, (uint16_t)(4072 - (4072-mid)*i/30.0f), t, r); t += DT; }
      for (int i = 0; i < pauseSamples; i++)              // hesitate
        { feed(k, art, mid, t, r); t += DT; }
      for (int i = 0; i <= 10; i++)                       // 20 ms to the bottom
        { feed(k, art, (uint16_t)(mid - (mid-500)*i/10.0f), t, r); t += DT; }
      for (int i = 0; i < 200; i++) { feed(k, art, 500, t, r); t += DT; }
      return r;
    };
    Result s5 = hesitate(ART_STRUCK, 250),    s7 = hesitate(ART_STRUCK, 350);
    Result g5 = hesitate(ART_TRIGGERED, 250), g7 = hesitate(ART_TRIGGERED, 350);
    snprintf(buf, sizeof buf,
             "500 ms vs 700 ms hesitation: STRUCK %.3f -> %.3f, TRIGGERED %.3f -> %.3f",
             s5.velocity, s7.velocity, g5.velocity, g7.velocity);
    check(g5.onCount == 1 && g7.onCount == 1, "both hesitations sound once", buf);
    check(g7.velocity <= g5.velocity + 0.001f,
          "TRIGGERED: the slower press does not read harder", buf);
    check(s7.velocity > s5.velocity,
          "STRUCK inverts here, which is the behaviour being avoided", buf);
  }

  // ---- aftertouch ---------------------------------------------------------
  // The signal the future feature would use. Pressure is measured from where
  // the note SETTLED, so it starts at zero however deep the trigger left the
  // key and reports only the lean that came afterwards. GATED is the control:
  // it is defined as reporting nothing at all once the note has begun.
  printf("\nTRIGGERED reports the lean after the attack, GATED does not\n");
  {
    // Trigger, settle, then lean deeper by `extra` of full travel.
    auto leanIn = [&](uint8_t art, float extra) {
      Key k = makeKey(4072); float t = 0.0f;
      Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
      const uint16_t land = (uint16_t)(4072 - 0.55f * 3572);
      const uint16_t deep = (uint16_t)(4072 - (0.55f + extra) * 3572);
      for (int i = 0; i <= 15; i++)
        { feed(k, art, (uint16_t)(4072 - (4072-land)*i/15.0f), t, r); t += DT; }
      for (int i = 0; i < 300; i++) { feed(k, art, land, t, r); t += DT; }
      float settled = k.g.pressure, landedAt = k.g.position;
      for (int i = 0; i <= 60; i++)      // 120 ms of lean
        { feed(k, art, (uint16_t)(land - (land-deep)*i/60.0f), t, r); t += DT; }
      for (int i = 0; i < 300; i++) { feed(k, art, deep, t, r); t += DT; }
      snprintf(buf, sizeof buf,
               "landed at %.2f (pressure %.3f), leaned +%.2f -> pressure %.3f",
               landedAt, settled, extra, k.g.pressure);
      return k.g.pressure;
    };
    // Each result into a local first: `check(leanIn(...), ..., buf)` reads buf
    // in the same argument list that fills it, and the order is unspecified —
    // which is how three checks came to print each other's numbers.
    float pTrig = leanIn(ART_TRIGGERED, 0.08f);
    check(pTrig > 0.15f, "TRIGGERED: a lean past the landing point registers", buf);
    float pGate = leanIn(ART_GATED, 0.08f);
    check(pGate == 0.0f, "GATED: the same lean registers nothing at all", buf);

    // THE LIMIT, pinned here so it cannot move silently. updatePressure drops
    // its rest point the moment the key goes more than heldBand (0.10) deeper
    // than where it settled — a rule meant for a HESITANT PRESS, where an early
    // pause was mistaken for the landing — and then re-latches at the new
    // depth. It has no time bound, so it also fires on a deliberate lean
    // several seconds into a held note, and pressure collapses to zero exactly
    // when the player pushes hardest.
    //
    // That is the one thing between this articulation and the aftertouch
    // feature it was built for, and it belongs to updatePressure, not here:
    // STRUCK and PLUCKED patches routing SRC_PRESSURE have the same ceiling
    // today. Bounding the discard to the first few hundred ms of a note would
    // fix all of them at once, and would change the feel of every patch that
    // reads pressure, so it is a decision rather than a repair.
    // Not `== 0`: the output is a one-pole smoother, so it approaches the
    // discarded target rather than snapping to it.
    float pDeep = leanIn(ART_TRIGGERED, 0.35f);
    check(pDeep < 0.01f,
          "KNOWN LIMIT: a lean past heldBand resets pressure to zero", buf);
  }

  // ---- a key at rest must NOT produce notes from noise --------------------
  // CONTINUOUS triggers at 2% of travel, which on this instrument is about 64
  // counts. Measured jitter reached 40 counts peak-to-peak on some cells.
  printf("\nresting keys must stay silent under realistic noise\n");
  for (int nz : {4, 12, 25}) {
    Key k = makeKey(4072); float t = 0.0f;
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
    for (int i = 0; i < 20000; i++) {
      int n = (rand() % (2*nz+1)) - nz;
      feed(k, ART_CONTINUOUS, (uint16_t)(4072 + n), t, r); t += DT;
    }
    snprintf(buf, sizeof buf, "+/-%d counts of noise over 40 s -> %d spurious notes",
             nz, r.onCount);
    check(r.onCount == 0, "no phantom notes at rest", buf);
  }

  // ---- baseline drift must not silence the instrument ---------------------
  // The baseline tracks while a key is idle. If "idle" is defined loosely
  // enough that it also tracks while the key is being pressed, the reference
  // follows the finger and the key can never reach its trigger.
  printf("\nbaseline tracking must not follow the finger\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result slow = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
    // A deliberately slow press: 6 seconds from rest to bottom.
    for (int i = 0; i <= 3000; i++) {
      uint16_t v = (uint16_t)(4072 - 3572.0f * i / 3000.0f);
      feed(k, ART_CONTINUOUS, v, t, slow); t += DT;
    }
    for (int i = 0; i < 200; i++) { feed(k, ART_CONTINUOUS, 500, t, slow); t += DT; }
    snprintf(buf, sizeof buf, "slow 6 s press -> %d note-ons, baseline %.0f, position %.2f",
             slow.onCount, k.cal.baseline, k.g.position);
    check(slow.onCount >= 1, "a very slow press still triggers", buf);
    check(k.cal.baseline > 4000.0f, "baseline did not chase the finger down", buf);
    check(k.g.position > 0.9f, "and position still reaches full", buf);
  }

  // ---- a held note must not be cut short ---------------------------------
  printf("\na long hold stays sounding\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result r = press(k, ART_CONTINUOUS, &t, 4072, 500, 40.0f, 15000);  // 30 s
    snprintf(buf, sizeof buf, "%d on / %d off over a 30 s hold", r.onCount, r.offCount);
    check(r.onCount == 1 && r.offCount == 1, "exactly one note across 30 s", buf);
  }

  // ---- THE LATCH-UP ------------------------------------------------------
  // Baseline tracking is gated on the key being idle. A note holds the key
  // out of idle. So if drift ever puts the resting level above the note-off
  // threshold, the note never ends, the baseline never catches up, and the
  // key is stuck sounding forever. CONTINUOUS triggers at 2% of travel — on
  // this instrument about 70 counts — which is inside the measured drift.
  printf("\nbaseline drift must not latch a note on forever\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
    // Settle, then drift the resting level down by 120 counts over 5 s and
    // hold it there. Nothing is being pressed at any point.
    for (int i = 0; i < 500; i++)  { feed(k, ART_CONTINUOUS, 4072, t, r); t += DT; }
    for (int i = 0; i < 2500; i++) {
      feed(k, ART_CONTINUOUS, (uint16_t)(4072 - 120.0f*i/2500.0f), t, r); t += DT;
    }
    for (int i = 0; i < 10000; i++) { feed(k, ART_CONTINUOUS, 3952, t, r); t += DT; }
    snprintf(buf, sizeof buf,
             "after 20 s at a drifted rest: phase=%d baseline=%.0f pos=%.3f (%d on/%d off)",
             k.g.phase, k.cal.baseline, k.g.position, r.onCount, r.offCount);
    check(k.g.phase == GP_IDLE, "the key returns to idle", buf);
    check(k.cal.baseline < 4000.0f, "the baseline follows the drift", buf);

    // ...and the key must still play afterwards.
    Result after = press(k, ART_CONTINUOUS, &t, 3952, 500);
    snprintf(buf, sizeof buf, "after recovery: %d note-ons, position %.2f",
             after.onCount, after.posHi);
    check(after.onCount == 1, "and still plays normally afterwards", buf);
  }

  // ---- switching preset mid-note -----------------------------------------
  // The articulation is read per sweep and applied to every key. Changing it
  // while a note sounds changes the note-off threshold underneath that note:
  // CONTINUOUS releases at 1.2% of travel, STRUCK at 18%. A held key sitting
  // between the two is either cut off or stranded, depending on direction.
  printf("\narticulation must be latched for the life of a note\n");
  {
    Key k = makeKey(4072); float t = 0.0f;
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0 };
    for (int i = 0; i <= 30; i++) {                    // press to ~10% depth
      feed(k, ART_CONTINUOUS, (uint16_t)(4072 - 360.0f*i/30.0f), t, r); t += DT;
    }
    for (int i = 0; i < 200; i++) { feed(k, ART_CONTINUOUS, 3712, t, r); t += DT; }
    int onBefore = r.onCount, offBefore = r.offCount;
    for (int i = 0; i < 200; i++) { feed(k, ART_STRUCK, 3712, t, r); t += DT; }
    snprintf(buf, sizeof buf,
             "held at 10%% depth, preset switched: %d->%d on, %d->%d off, phase=%d",
             onBefore, r.onCount, offBefore, r.offCount, k.g.phase);
    check(r.offCount == offBefore, "the sounding note is not cut off", buf);
  }

  // ---- CROSSTALK ---------------------------------------------------------
  // Reproduces the reported fault: hold s7, and s8 — same row, next column
  // read — chatters on and off. The bleed measured about 0.05 of travel,
  // which sits between contOff (0.040) and contOn (0.060).
  printf("\ncrosstalk from a held neighbour must not trigger a note\n");
  {
    const float K = 0.056f;                 // measured on the instrument
    // s7 pressed to the bottom of its travel. The sensors on this instrument
    // reach ~300 counts, and it is that full excursion — not a moderate
    // press — that puts the bleed over the threshold.
    const uint16_t BASE = 4000, HELD = 300;
    Key uncorrected = makeKey(BASE), corrected = makeKey(BASE);
    Result ru = { 0,0,0,2.0f,-1.0f,0,0,0 }, rc = { 0,0,0,2.0f,-1.0f,0,0,0 };
    float t = 0.0f;
    for (int i = 0; i < 6000; i++) {        // 12 s with s7 held down
      // What the ADC actually reports for s8: settling from s7's level.
      uint16_t measured = (uint16_t)(BASE + (HELD - BASE) * K);
      feed(uncorrected, ART_CONTINUOUS, measured, t, ru);
      feed(corrected, ART_CONTINUOUS, calRemoveBleed(measured, HELD, K), t, rc);
      t += DT;
    }
    snprintf(buf, sizeof buf,
             "bleed %d counts = %.3f of travel (contOn %.3f): uncorrected fired"
             " %d notes, corrected fired %d",
             (int)((BASE-HELD)*K), (BASE-HELD)*K/3200.0f, GESTURE_DEFAULT.contOn,
             ru.onCount, rc.onCount);
    check(ru.onCount > 0, "the bug reproduces without correction", buf);
    check(rc.onCount == 0, "and is gone with it", buf);
  }

  // Correction must leave an UNAFFECTED reading alone, or it would distort
  // every key that has no pressed neighbour — which is most of them.
  printf("\ncorrection is a no-op when there is nothing to correct\n");
  {
    const float K = 0.056f;
    int worst = 0;
    for (int v = 300; v <= 4000; v += 100) {
      int out = calRemoveBleed((uint16_t)v, (uint16_t)v, K);   // line already settled
      int err = out - v; if (err < 0) err = -err;
      if (err > worst) worst = err;
    }
    snprintf(buf, sizeof buf, "worst error across the range: %d counts", worst);
    check(worst <= 1, "a settled line passes through untouched", buf);
  }

  // ---- a dead sensor must not play, and must not poison its neighbours ----
  printf("\ndead sensor stays out of the way\n");
  {
    Key dead = makeKey(72);
    snprintf(buf, sizeof buf, "travel %.0f", calTravel(dead.cal));
    check(calDead(dead.cal, CAL_DEFAULT), "a stuck-low sensor reads dead", buf);
  }

  printf("\n%s (%d failure%s)\n", failures?"FAILED":"ALL PASSED", failures,
         failures==1?"":"s");
  return failures ? 1 : 0;
}
#endif
