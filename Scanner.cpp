// ============================================================================
// Scanner.cpp — LAYER 0 (the matrix) plus the wiring between layers
// ============================================================================
// This file used to be 600 lines containing calibration, strike detection,
// velocity, pressure learning and nine diagnostics. Almost all of it was
// untestable, and almost every bug in this instrument lived here.
//
// What is left is the only part that genuinely needs Arduino: switch a
// column, wait for the line to settle, read five ADCs. Everything else is a
// three-line pipeline:
//
//     raw counts --> Calibration --> position --> Gesture --> events
//                                        |            |
//                                        +------------+--> Synth
//
// Diagnostics live in Diag.cpp and are read-only taps on this pipeline. They
// never recompute anything: a diagnostic that reimplements the thing it is
// meant to observe will eventually disagree with it, and then it lies.
// ============================================================================
#include <Arduino.h>
#include "Config.h"
#include "Calibration.h"
#include "Gesture.h"
#include "Options.h"
#include "Preset.h"
#include "Scanner.h"
#include "Synth.h"

#if SEND_USB_MIDI
#include <usb_midi.h>
#endif

CalKey       calKey[5][5];
GestureState gest[5][5];
// The reading as the ADC actually returned it, before bleed removal. Needed
// to MEASURE the coupling coefficient — calKey[].raw holds the corrected
// value, and measuring a correction with its own output is circular.
uint16_t     rawMeasured[5][5];

static uint32_t nowUs = 0;

// Options mode. The state lives here rather than in the sketch because the
// positions it needs are produced inside the sweep and nowhere else.
static OptionsState opts;
// LAYER 1 output for the whole matrix, one sweep old at worst. Options reads
// this rather than gest[][].position deliberately — see Options.h.
static float optPos[OPT_NUM_KEYS];

bool scannerOptionsActive() { return optionsActive(opts); }

// Default ON, so an instrument that is never told about this behaves exactly
// as it always has. Flip to false here if you would rather boot frozen.
static bool learnTravel = true;

bool scannerLearn() { return learnTravel; }

void scannerSetLearn(bool on) {
  // Re-enabling must not fold in a stale deepest-point. While learning is off
  // `noteMinRaw` stops being updated (calUpdate only tracks it when a note is
  // active AND learning is on), so whatever it held when we froze would
  // otherwise be taught to the key on the very next note-off.
  if (on && !learnTravel)
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 5; c++) calKey[r][c].noteMinRaw = calKey[r][c].baseline;
  learnTravel = on;
}

// ---- matrix primitives ---------------------------------------------------
void scanSelectColumn(int c) {
#ifdef HOST_TEST
  extern void digitalWriteCol(int);
  digitalWriteCol(c);
#endif
  for (int j = 0; j < 5; j++) digitalWrite(COL_PIN[j], j == c ? HIGH : LOW);
}
void scanDeselectAll() {
  for (int j = 0; j < 5; j++) digitalWrite(COL_PIN[j], LOW);
}
void scanReadColumn(int c, uint16_t out[5]) {
  scanSelectColumn(c);
  delayMicroseconds(SETTLE_US);
  for (int r = 0; r < 5; r++) out[r] = analogRead(ADC_PIN[r]);
  scanDeselectAll();
}

// ---- setup / calibration -------------------------------------------------
void scannerBegin() {
  analogReadResolution(12);
  for (int j = 0; j < 5; j++) {
    pinMode(COL_PIN[j], OUTPUT);
    digitalWrite(COL_PIN[j], LOW);
  }
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) gestureInit(gest[r][c]);
  optionsInit(opts);
  for (int i = 0; i < OPT_NUM_KEYS; i++) optPos[i] = 0.0f;
}

// A dead key is skipped by the sweep, so it reads 0.0 forever — and if it is
// one of the four keys the mode depends on, the mode is either unreachable or
// unusable, with no symptom except a gesture that does nothing. Say so at
// boot, where somebody is already reading the dead-sensor list.
static void reportOptionsKeys() {
  const OptionsConfig &oc = OPTIONS_DEFAULT;
  uint8_t needed[OPT_CHORD_MAX + 2];
  int n = 0;
  for (int i = 0; i < oc.nChord && i < OPT_CHORD_MAX; i++) needed[n++] = oc.chord[i];
  needed[n++] = oc.navPrev;
  needed[n++] = oc.navNext;

  for (int i = 0; i < n; i++) {
    const int k = needed[i];
    if (!calDead(calKey[k / 5][k % 5], CAL_DEFAULT)) continue;
    Serial.print("# WARNING: key s"); Serial.print(k + 1);
    Serial.println(i < oc.nChord
      ? " is part of the options chord and is DEAD — options mode cannot be"
        " entered from the keyboard. Use the 'O' command."
      : " is an options nav key and is DEAD — you will not be able to change"
        " preset from the keyboard.");
  }
}

void scannerCalibrate() {
  uint32_t sum[5][5] = {{0}};
  const int N = 200;
  for (int i = 0; i < N; i++)
    for (int c = 0; c < 5; c++) {
      uint16_t v[5]; scanReadColumn(c, v);
      for (int r = 0; r < 5; r++) sum[r][c] += v[r];
    }

  int dead = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      calInit(calKey[r][c], (uint16_t)(sum[r][c] / N), CAL_DEFAULT);
      gestureInit(gest[r][c]);
      if (calDead(calKey[r][c], CAL_DEFAULT)) dead++;
    }

  Serial.println("# calibrated (hands off the keys)");
  if (dead) {
    Serial.print("# WARNING: "); Serial.print(dead);
    Serial.println(dead == 1 ? " key is DEAD and disabled:"
                             : " keys are DEAD and disabled:");
    for (int r = 0; r < 5; r++)
      for (int c = 0; c < 5; c++)
        if (calDead(calKey[r][c], CAL_DEFAULT)) {
          Serial.print("#   s"); Serial.print(r * 5 + c + 1);
          Serial.print("  resting level "); Serial.print(calKey[r][c].raw);
          Serial.print(", usable travel "); Serial.print(calTravel(calKey[r][c]), 0);
          Serial.print(" (needs "); Serial.print(CAL_DEFAULT.minTravel);
          Serial.println("). It cannot register a press.");
        }
    Serial.println("# A whole ROW listed means the shared sense line, not the sensors.");
  } else {
    Serial.println("# all 25 sensors healthy");
  }
  reportOptionsKeys();
}

void scannerResetCalibration() {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      calInit(calKey[r][c], (uint16_t)calKey[r][c].baseline, CAL_DEFAULT);
      gestureInit(gest[r][c]);
    }
  Serial.println("# learned travel forgotten");
}

// A key that has never been played borrows from those that have, so it works
// on its first note instead of needing two or three to warm up.
// Travel only. Lean width used to be seeded across keys too, which was never
// defensible: travel is a property of a plastic key and a sensor, so borrowing
// it from a neighbour is a reasonable guess, but a lean is a property of the
// player and identical on every key by definition.
static void seedFromNeighbours() {
  float travel = 0.0f;
  int nt = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      const CalKey &k = calKey[r][c];
      if (k.learned && !k.borrowed && !calDead(k, CAL_DEFAULT)) {
        travel += calTravel(k); nt++;
      }
    }
  if (!nt) return;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      calSeedTravel(calKey[r][c], CAL_DEFAULT, travel / nt);
}

// Release everything. Changing preset can change the articulation, and a
// sounding note carries its old rules to the grave (see GestureState::noteArt)
// — but a chord left hanging across a patch change is confusing regardless, so
// the change starts from silence.
//
// Under ART_TRIGGERED this stopped being a courtesy and became the safety net.
// A latched note has no key holding it up and no timeout: nothing else in the
// system will ever end it except the player pressing that key again. So the
// test cannot be "is a key down" any more — a latched note's key is at rest,
// looks exactly like the twenty-four that are not sounding, and would be
// skipped. Ask about the NOTE.
void scannerAllNotesOff() {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (gestureSounding(gest[r][c]) || gest[r][c].phase != GP_IDLE) {
        synthNoteOff(r, c, 0.5f);
#if SEND_USB_MIDI
        usbMIDI.sendNoteOff(MIDI_NOTE[r][c], 0, 1);
#endif
        calNoteEnded(calKey[r][c], CAL_DEFAULT);
        gestureInit(gest[r][c]);
      }
}

// ---- one sweep -----------------------------------------------------------
void scannerScanOnce() {
  const uint8_t art = presetArticulation();
  // Fetched once per sweep rather than per key: it cannot change mid-sweep,
  // and a preset change kills every sounding note anyway (scannerAllNotesOff).
  const GestureConfig &gc = presetGesture();

  // The previous column's reading for each row, so the settling bleed can be
  // subtracted. Rows are shared, and a key pressed to the bottom in one
  // column drags the next column's reading on that row down with it — enough
  // to trigger a continuous note on a key nobody is touching.
  static uint16_t prevRow[5] = { 4095, 4095, 4095, 4095, 4095 };

  for (int c = 0; c < 5; c++) {
    uint16_t v[5];
    scanReadColumn(c, v);
    for (int r = 0; r < 5; r++) {
      rawMeasured[r][c] = v[r];
      uint16_t corrected = calRemoveBleed(v[r], prevRow[r], CROSS_BLEED);
      prevRow[r] = v[r];               // the RAW value is what the line held
      v[r] = corrected;
    }
    nowUs = micros();
    float nowMs = nowUs * 0.001f;

    for (int r = 0; r < 5; r++) {
      CalKey &k = calKey[r][c];
      const int idx = r * 5 + c;
      if (calDead(k, CAL_DEFAULT)) {
        // Skipped, but its slot must not go stale — a dead key that kept the
        // last position it ever read would hold the chord down forever.
        optPos[idx] = 0.0f;
        continue;
      }
      GestureState &g = gest[r][c];

      // LAYER 1: counts -> position. The flag says only "a note is in
      // progress, so learn this key's bottom". Baseline tracking is decided
      // inside Calibration from the signal itself — see Calibration.h for why
      // gating it on the note state deadlocked.
      // `noteActive` gates ONLY the learning of this key's bottom — both the
      // live expansion and the noteMinRaw tracking that feeds calNoteEnded.
      // Baseline tracking ignores it entirely and is decided from the signal,
      // so freezing travel here leaves drift compensation running.
      float pos = calUpdate(k, CAL_DEFAULT, v[r], learnTravel && !gestureIdle(g));
      optPos[idx] = pos;

      // LAYER 2b: the two keys options mode has taken away. Held permanently
      // idle rather than merely muted, so nothing is left stranded when the
      // mode ends — no phase mid-descent, and under ART_TRIGGERED no latched
      // note that the player never asked for and cannot see. It costs the key
      // its travel learning for the duration, which is right: nobody is
      // playing a key that is being used as a button.
      if (optionsReserved(opts, OPTIONS_DEFAULT, idx)) { gestureInit(g); continue; }

      // LAYER 2: position -> events + expression. The config comes from the
      // PATCH now, not from one global for the whole instrument — see
      // Preset::gesture for why a threshold belongs to the sound that needs it.
      GestureEvent e = gestureUpdate(g, gc, art, pos, nowMs);

      // LAYER 3: expression is pushed EVERY sweep, before any event, so a
      // note never begins at a level its modulation is about to contradict.
      // That gap was an audible pop.
      synthExpression(r, c, g.position, g.pressure, gestureKeyVel(g, gc));

      if (e.noteOn) {
        synthNoteOn(r, c, e.velocity);
#if SEND_USB_MIDI
        usbMIDI.sendNoteOn(MIDI_NOTE[r][c], (int)(e.velocity * 127.0f), 1);
#endif
#if VERBOSE
        Serial.print("s"); Serial.print(r * 5 + c + 1);
        Serial.print(" ON  v="); Serial.println(e.velocity, 2);
#endif
      }
      if (e.noteOff) {
        synthNoteOff(r, c, e.releaseVel);
#if SEND_USB_MIDI
        usbMIDI.sendNoteOff(MIDI_NOTE[r][c], 0, 1);
#endif
#if VERBOSE
        Serial.print("s"); Serial.print(r * 5 + c + 1);
        Serial.print(" OFF rel="); Serial.print(e.releaseVel, 2);
        Serial.print("  travel="); Serial.println(calTravel(k), 0);
#endif
      }
      if (e.noteEnded && learnTravel) {
        calNoteEnded(k, CAL_DEFAULT);
        seedFromNeighbours();   // also writes bottoms, so it freezes too
      }
    }
  }

  // LAYER 2b, once the whole matrix has been read. The chord test is about all
  // twenty-five keys at one instant, which is a question no per-key call can
  // answer.
  scannerApplyOptions(optionsUpdate(opts, OPTIONS_DEFAULT, optPos,
                                    micros() * 0.001f));
}

// One place where a mode change becomes sound, whether a chord or the console
// caused it. Two paths here would drift, and the one that drifted would be the
// console's — the one nobody plays with and everybody debugs with.
void scannerApplyOptions(OptionsEvent oe) {
  if (oe.enter || oe.exit) {
    // Whatever was sounding belonged to the other mode. Under ART_TRIGGERED
    // this is not a courtesy: a latched note has nothing holding it up and
    // would otherwise ring straight through the mode change and out the far
    // side.
    scannerAllNotesOff();
    synthGlissando(oe.enter);
    Serial.println(oe.enter
      ? "# OPTIONS — the end keys change preset, the rest still play."
        " Hold the bottom three again to leave."
      : "# playing");
    presetAnnounce();
  }
  if (oe.presetDelta) {
    // A preset change silences everything first — see Gesture.cpp on why a
    // latched note must not outlive the articulation that knows how to end it.
    scannerAllNotesOff();
    presetSelect(presetCurrent() + oe.presetDelta);
  }
}

void scannerSetOptions(bool on) {
  scannerApplyOptions(optionsForce(opts, OPTIONS_DEFAULT, on));
}
