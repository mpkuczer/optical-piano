// ============================================================================
// Scanner.h — the matrix, and the wiring between Calibration and Gesture
// ============================================================================
// Knows nothing about sound. Reads the 5x5 photodarlington matrix, runs each
// key through Calibration then Gesture, and forwards the results to Synth.
#pragma once
#include <Arduino.h>
#include "Calibration.h"
#include "Gesture.h"

void scannerBegin();
void scannerCalibrate();          // learn resting levels — hands OFF the keys
void scannerResetCalibration();   // forget learned travel
void scannerScanOnce();           // one full sweep of all five columns
void scannerAllNotesOff();        // silence everything (used on preset change)

// Travel learning, on or off. Learning used to run permanently, which meant
// every fault in note DETECTION became a permanent fault in CALIBRATION: a
// crosstalk phantom note on an untouched key taught that key a false bottom
// that outlived the sound. Baseline tracking is deliberately NOT affected —
// thermal drift is continuous and must always be followed. Only the learned
// bottom freezes. Play a pass with this on, then turn it off.
void scannerSetLearn(bool on);
bool scannerLearn();

// Matrix primitives, exposed so Diag.cpp can probe the hardware directly
// WITHOUT reimplementing anything above them.
void scanSelectColumn(int c);
void scanDeselectAll();
void scanReadColumn(int c, uint16_t out[5]);

// Live state, read-only for diagnostics. A diagnostic that recomputes what
// the pipeline computes will eventually disagree with it, and then it lies —
// which cost us four rounds of debugging on a monitor that never ran the
// note state machine at all.
extern CalKey       calKey[5][5];
extern GestureState gest[5][5];
extern uint16_t     rawMeasured[5][5];   // pre-correction ADC readings
