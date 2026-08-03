// ============================================================================
// Diag.cpp — every diagnostic, in one place
// ============================================================================
// ONE RULE: a diagnostic is a READ-ONLY TAP on the production pipeline. It
// never recomputes anything the pipeline already computes.
//
// This rule is not tidiness. The old pressure monitor read the ADC directly
// and never ran the note state machine, so it displayed unlearned zeros while
// the instrument was working perfectly — and those zeros were read as
// evidence for four rounds of debugging. A diagnostic that reimplements the
// thing it observes will eventually disagree with it, and a disagreeing
// diagnostic is worse than none.
//
// The hardware probes below are the one exception, and they are honest about
// it: they bypass the pipeline deliberately in order to test the wiring
// UNDERNEATH it, which is a different question.
// ============================================================================
#include <Arduino.h>
#include "Config.h"
#include "Calibration.h"
#include "Diag.h"
#include "Gesture.h"
#include "Scanner.h"
#include "Synth.h"

// ---- live trace ----------------------------------------------------------
// Runs alongside normal scanning, so the instrument keeps playing while you
// watch. Every number below is read straight out of the pipeline's own state.
static bool     traceOn   = false;
static uint32_t traceNext = 0;

void diagTraceToggle() {
  traceOn = !traceOn;
  if (!traceOn) { Serial.println("# trace off"); return; }
  Serial.println("# trace ON — play and lean, sound keeps running");
  Serial.println("#  key\traw\tbase\tpos\tpress\ttravel\twidth\tphase");
  Serial.println("#  (pos is ABSOLUTE 0..1; press is RELATIVE to where this");
  Serial.println("#   note landed. A patch routes one, the other, or both.)");
}

void diagServiceTrace() {
  if (!traceOn) return;
  uint32_t now = millis();
  if (now < traceNext) return;

  int br = -1, bc = -1;
  float best = 0.0f;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (gest[r][c].phase == GP_SOUNDING && calKey[r][c].position >= best) {
        best = calKey[r][c].position; br = r; bc = c;
      }
  if (br < 0) return;
  traceNext = now + 40;

  const CalKey &k = calKey[br][bc];
  const GestureState &g = gest[br][bc];
  Serial.print("#  s"); Serial.print(br * 5 + bc + 1);
  Serial.print("\t");   Serial.print(k.raw);
  Serial.print("\t");   Serial.print(k.baseline, 0);
  Serial.print("\t");   Serial.print(g.position, 3);
  Serial.print("\t");   Serial.print(g.pressure, 3);
  Serial.print("\t");   Serial.print(calTravel(k), 0);
  Serial.print("\t");   Serial.print(gestureWidth(g), 4);
  Serial.print("\t");   Serial.print(k.borrowed ? "borrowed" : (k.learned ? "own" : "new"));
  Serial.print("  ");
  int bars = (int)(g.position * 30.0f);
  for (int i = 0; i < bars; i++) Serial.print('#');
  Serial.println();
}

// ---- per-key report ------------------------------------------------------
void diagKeyReport() {
  Serial.println("# per-key calibration");
  Serial.println("#  key\tbase\ttravel\twidth\tphase\tstate");
  Serial.println("#  phase: 0 idle, 1 descending, 2 SOUNDING. Anything showing");
  Serial.println("#  2 with no finger on it is a stuck note.");
  Serial.println("#  travel = counts from rest to this key's own bottom");
  Serial.println("#  width  = the lean it has shown past a settled note");
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      const CalKey &k = calKey[r][c];
      const GestureState &g = gest[r][c];
      Serial.print("#  s");  Serial.print(r * 5 + c + 1);
      Serial.print("\t");    Serial.print(k.baseline, 0);
      if (calDead(k, CAL_DEFAULT)) {
        Serial.print("\t");  Serial.print(calTravel(k), 0);
        Serial.println("\t--\tDEAD, cannot register a press");
        continue;
      }
      Serial.print("\t");    Serial.print(calTravel(k), 0);
      Serial.print("\t");    Serial.print(gestureWidth(g), 4);
      Serial.print("\t");    Serial.print(g.phase);
      Serial.print("\t");    Serial.print(k.borrowed ? "borrowed" : (k.learned ? "own" : "default"));
      if (gestureWidth(g) > 0.0f && gestureWidth(g) < 0.020f)
        Serial.println("  (narrow lean — will feel coarse)");
      else
        Serial.println("");
    }
}

// ---- hardware probes -----------------------------------------------------
// These DO bypass the pipeline, on purpose: they test the wiring underneath
// it, which the pipeline cannot see.
void diagIsolationSnapshot() {
  Serial.println("# isolation snapshot, one line per column. '!' = stuck low.");
  for (int c = 0; c < 5; c++) {
    uint16_t v[5]; scanReadColumn(c, v);
    for (int r = 0; r < 5; r++) {
      Serial.print("s"); Serial.print(r * 5 + c + 1);
      Serial.print("="); Serial.print(v[r]);
      Serial.print(v[r] < STUCK_LOW_COUNTS ? "! " : "  ");
    }
    Serial.println();
  }
}

void diagFaultProbe() {
  Serial.println("# ---- fault probe ----");

  scanDeselectAll();
  delayMicroseconds(SETTLE_US * 4);
  uint16_t idle[5];
  bool rowFault[5];
  Serial.println("# 1) all columns OFF (expect ~4095 on every row):");
  for (int r = 0; r < 5; r++) {
    idle[r] = analogRead(ADC_PIN[r]);
    rowFault[r] = idle[r] < STUCK_LOW_COUNTS;
    Serial.print("#    row "); Serial.print(r + 1);
    Serial.print(" = ");       Serial.print(idle[r]);
    Serial.print(" (");        Serial.print(idle[r] * 3.3f / 4095.0f, 3);
    Serial.println(rowFault[r] ? " V)   <<< PULLED LOW WITH NO COLUMN SELECTED"
                               : " V)   ok");
  }

  uint16_t m[5][5];
  for (int c = 0; c < 5; c++) {
    uint16_t v[5]; scanReadColumn(c, v);
    for (int r = 0; r < 5; r++) m[r][c] = v[r];
  }
  Serial.println("# 2) per column (rows down, columns across):");
  for (int r = 0; r < 5; r++) {
    Serial.print("#    ");
    for (int c = 0; c < 5; c++) {
      Serial.print("s"); Serial.print(r * 5 + c + 1);
      Serial.print("="); Serial.print(m[r][c]);
      Serial.print(m[r][c] < STUCK_LOW_COUNTS ? "! " : "  ");
    }
    Serial.println();
  }

  Serial.println("# 3) jitter, 16 reads per cell (peak-to-peak counts):");
  for (int r = 0; r < 5; r++) {
    Serial.print("#    ");
    for (int c = 0; c < 5; c++) {
      uint16_t lo = 65535, hi = 0;
      for (int i = 0; i < 16; i++) {
        uint16_t v[5]; scanReadColumn(c, v);
        if (v[r] < lo) lo = v[r];
        if (v[r] > hi) hi = v[r];
      }
      Serial.print("s"); Serial.print(r * 5 + c + 1);
      Serial.print("="); Serial.print(hi - lo);
      Serial.print(hi - lo > 40 ? "! " : "  ");
    }
    Serial.println();
  }
  Serial.println("#    Under ~10 is clean. Over ~40 modulates amplitude at the");
  Serial.println("#    scan rate and is audible as buzz on continuous patches.");

  Serial.println("# 4) verdict:");
  bool any = false;
  for (int r = 0; r < 5; r++) {
    if (rowFault[r]) {
      Serial.print("#    ROW "); Serial.print(r + 1);
      Serial.println(" sense line shorted to ground, or its pull-up is open.");
      Serial.println("#      All five keys affected. Wiring, not a sensor.");
      any = true; continue;
    }
    for (int c = 0; c < 5; c++) {
      if (m[r][c] >= STUCK_LOW_COUNTS) continue;
      int lowIn = 0;
      for (int cc = 0; cc < 5; cc++) if (m[r][cc] < STUCK_LOW_COUNTS) lowIn++;
      Serial.print("#    s"); Serial.print(r * 5 + c + 1);
      if (lowIn == 1) {
        Serial.println(" low in ITS OWN column only.");
        Serial.println("#      Above ~0.7 V: a saturated sensor, or ambient light.");
        Serial.println("#      Below ~0.5 V: a collector-emitter SHORT. That welds");
        Serial.println("#      this row's sense line to this column's return line,");
        Serial.println("#      which degrades the whole row. Run the bridge probe.");
      } else {
        Serial.println(" low in SEVERAL columns: a path to ground that");
        Serial.println("#      bypasses the column transistors entirely.");
      }
      any = true;
    }
  }
  if (!any) Serial.println("#    no faults found — everything reads sane.");
}

void diagBridgeProbe() {
  Serial.println("# ---- bridge probe ----");
  Serial.println("# ALL columns are off, so no sensor has a path to ground and");
  Serial.println("# NOTHING you press should change any number below. Anything");
  Serial.println("# that moves is bridged past the column transistors.");
  Serial.println("# The shorted sensor sits at the row/column intersection.");
  Serial.println("# Send any character to stop.");
  Serial.println("# row1\trow2\trow3\trow4\trow5");

  scanDeselectAll();
  delayMicroseconds(SETTLE_US * 4);
  uint16_t base[5];
  for (int r = 0; r < 5; r++) base[r] = analogRead(ADC_PIN[r]);

  uint32_t next = 0;
  while (!Serial.available()) {
    if (millis() < next) continue;
    next = millis() + 150;
    scanDeselectAll();
    Serial.print("# ");
    for (int r = 0; r < 5; r++) {
      uint16_t v = analogRead(ADC_PIN[r]);
      int d = (int)v - (int)base[r];
      Serial.print(v);
      if (d < -40 || d > 40) { Serial.print("("); Serial.print(d); Serial.print(")!"); }
      Serial.print("\t");
    }
    Serial.println();
  }
  while (Serial.available()) Serial.read();
  Serial.println("# bridge probe done.");
}

void diagSettleProbe(int row, int col) {
  Serial.print("# settle probe: row "); Serial.print(row + 1);
  Serial.print(", column ");            Serial.println(col + 1);
  Serial.print("# SETTLE_US is ");      Serial.print(SETTLE_US);
  Serial.println(". If the curve has not flattened by then, every reading is");
  Serial.println("# taken on a moving line and jitters with interrupt timing.");
  Serial.println("# us,adc");
  scanSelectColumn(col);
  uint32_t t0 = micros();
  for (int i = 0; i < 150; i++) {
    uint32_t want = t0 + (uint32_t)i * 20;
    while ((int32_t)(micros() - want) < 0) {}
    Serial.print(i * 20); Serial.print(",");
    Serial.println(analogRead(ADC_PIN[row]));
  }
  scanDeselectAll();
}
