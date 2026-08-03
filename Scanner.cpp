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
#include "Preset.h"
#include "Scanner.h"
#include "Synth.h"

#if SEND_USB_MIDI
#include <usb_midi.h>
#endif

CalKey       calKey[5][5];
GestureState gest[5][5];

static uint32_t nowUs = 0;

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
}

void scannerResetCalibration() {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      calInit(calKey[r][c], (uint16_t)calKey[r][c].baseline, CAL_DEFAULT);
      gestureInit(gest[r][c]);
    }
  Serial.println("# learned travel and widths forgotten");
}

// A key that has never been played borrows from those that have, so it works
// on its first note instead of needing two or three to warm up.
static void seedFromNeighbours() {
  float travel = 0.0f, width = 0.0f;
  int nt = 0, nw = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      const CalKey &k = calKey[r][c];
      if (k.learned && !k.borrowed && !calDead(k, CAL_DEFAULT)) {
        travel += calTravel(k); nt++;
      }
      const GestureState &g = gest[r][c];
      if (g.widthLearned && !g.widthBorrowed) { width += g.width; nw++; }
    }
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      if (nt) calSeedTravel(calKey[r][c], CAL_DEFAULT, travel / nt);
      if (nw) gestureSeedWidth(gest[r][c], GESTURE_DEFAULT, width / nw);
    }
}

// Release everything. Changing preset can change the articulation, and a
// sounding note carries its old rules to the grave (see GestureState::noteArt)
// — but a chord left hanging across a patch change is confusing regardless, so
// the change starts from silence.
void scannerAllNotesOff() {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (gest[r][c].phase != GP_IDLE) {
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

  for (int c = 0; c < 5; c++) {
    uint16_t v[5];
    scanReadColumn(c, v);
    nowUs = micros();
    float nowMs = nowUs * 0.001f;

    for (int r = 0; r < 5; r++) {
      CalKey &k = calKey[r][c];
      if (calDead(k, CAL_DEFAULT)) continue;
      GestureState &g = gest[r][c];

      // LAYER 1: counts -> position. The flag says only "a note is in
      // progress, so learn this key's bottom". Baseline tracking is decided
      // inside Calibration from the signal itself — see Calibration.h for why
      // gating it on the note state deadlocked.
      float pos = calUpdate(k, CAL_DEFAULT, v[r], !gestureIdle(g));

      // LAYER 2: position -> events + expression.
      GestureEvent e = gestureUpdate(g, GESTURE_DEFAULT, art, pos, nowMs);

      // LAYER 3: expression is pushed EVERY sweep, before any event, so a
      // note never begins at a level its modulation is about to contradict.
      // That gap was an audible pop.
      synthExpression(r, c, g.position, g.pressure);

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
        Serial.print("  travel="); Serial.print(calTravel(k), 0);
        Serial.print("  width=");  Serial.println(gestureWidth(g), 4);
#endif
      }
      if (e.noteEnded) {
        calNoteEnded(k, CAL_DEFAULT);
        seedFromNeighbours();
      }
    }
  }
}
