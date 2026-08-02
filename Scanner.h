// ============================================================================
// Scanner.h — the key-sensing side
// ============================================================================
// Reads the 5x5 photodarlington matrix, decides when a key has been struck,
// and measures how fast it was struck. Knows nothing about sound: it calls
// synthNoteOn/synthNoteOff and that's the whole interface.
#pragma once
#include <Arduino.h>

void scannerBegin();      // set pin modes
void scannerCalibrate();  // learn resting levels — hands OFF the keys
void scannerResetSpans(); // forget learned key travel, back to defaults
void scannerScanOnce();   // one full sweep of all five columns

void scannerIsolationSnapshot();  // print all 25 cells, one row per line
void scannerSettleProbe();        // print the settling curve of one cell
void scannerPressureMonitor();    // live depth of a held key — is there any
                                  //   travel left after the note fires?
