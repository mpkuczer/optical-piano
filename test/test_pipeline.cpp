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
};

// EXACTLY the body of scannerScanOnce's inner loop.
static void feed(Key &k, uint8_t art, uint16_t raw, float nowMs, Result &r) {
  float pos = calUpdate(k.cal, CAL_DEFAULT, raw, gestureIdle(k.g));
  GestureEvent e = gestureUpdate(k.g, GESTURE_DEFAULT, art, pos, nowMs);
  if (e.noteOn)  { r.onCount++;  r.velocity = e.velocity; }
  if (e.noteOff) { r.offCount++; }
  if (e.noteEnded) calNoteEnded(k.cal, CAL_DEFAULT);
  if (k.g.phase == GP_SOUNDING) {
    if (k.g.position < r.posLo) r.posLo = k.g.position;
    if (k.g.position > r.posHi) r.posHi = k.g.position;
  }
}

// A realistic press on this instrument: rest 4072, bottom 500.
static Result press(Key &k, uint8_t art, float *t,
                    uint16_t rest = 4072, uint16_t bottom = 500,
                    float strikeMs = 40.0f, int holdSamples = 400,
                    uint16_t noise = 6) {
  Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f };
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

  // ---- a key at rest must NOT produce notes from noise --------------------
  // CONTINUOUS triggers at 2% of travel, which on this instrument is about 64
  // counts. Measured jitter reached 40 counts peak-to-peak on some cells.
  printf("\nresting keys must stay silent under realistic noise\n");
  for (int nz : {4, 12, 25}) {
    Key k = makeKey(4072); float t = 0.0f;
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f };
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
    Result slow = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f };
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
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f };
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
    Result r = { 0, 0, 0.0f, 2.0f, -1.0f, 0.0f };
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
    Result ru = { 0,0,0,2.0f,-1.0f,0 }, rc = { 0,0,0,2.0f,-1.0f,0 };
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
