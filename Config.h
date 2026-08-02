// ============================================================================
// Config.h — hardware wiring and detection tuning
// ============================================================================
// Everything here describes THIS instrument. If you rewire something or the
// keys start behaving oddly, this is the only file you should need to touch.
#pragma once
#include <Arduino.h>

// ---- build options -------------------------------------------------------
#define SEND_USB_MIDI 1     // also emit USB MIDI (needs USB Type: Serial + MIDI)
#define VERBOSE       1     // print every note on/off — costs time in the hot path
#define HAVE_ENCODER  0     // set 1 once the KY-040 is wired to 33/34/35

// ---- pins ----------------------------------------------------------------
// Columns drive transistor bases: HIGH = column selected (grounded).
const int COL_PIN[5] = { 28, 29, 30, 31, 32 };
// Rows sense one photodarlington each, one per column position.
const int ADC_PIN[5] = { A10, A11, A12, A14, A15 };  // = 24, 25, 26, 38, 39

// KY-040 rotary encoder. POWER FROM 3.3V — Teensy pins are not 5V tolerant.
const int ENC_CLK_PIN = 33;
const int ENC_DT_PIN  = 34;
const int ENC_SW_PIN  = 35;
const int ENC_COUNTS_PER_DETENT = 4;

// ---- key map -------------------------------------------------------------
// MIDI note for each (row, col). Change this if your physical key order
// doesn't match the wiring order.
const int MIDI_NOTE[5][5] = {
  { 48, 49, 50, 51, 52 },
  { 53, 54, 55, 56, 57 },
  { 58, 59, 60, 61, 62 },
  { 63, 64, 65, 66, 67 },
  { 68, 69, 70, 71, 72 },
};

const int NUM_VOICES = 25;

// ---- scan timing ---------------------------------------------------------
const uint32_t SETTLE_US = 200;   // wait after selecting a column before reading
const int      SWEEP_COLS = 5;

// ---- detection thresholds (fractions of each key's learned span) ---------
const float ON_DEPTH        = 0.40f;
const float OFF_DEPTH       = 0.20f;
const float VEL_START_DEPTH = 0.15f;
const int   RETREAT_HYST    = 15;

// ---- guards --------------------------------------------------------------
const uint32_t OVERSHOOT_WINDOW_US = 60000;
const uint32_t EXCURSION_MAX_US    = 250000;
const uint32_t DESCEND_TIMEOUT_US  = 600000;

// ---- pressure (continuous key depth while held) --------------------------
// The tuning now lives in Press.cpp as PRESS_DEFAULT, because it is covered
// by tests (test_press.cpp) that replay real captured traces. Changing it
// here would have put the numbers out of reach of those tests.

const float REL_SMOOTH = 0.12f;   // smoothing on the ascent-rate estimate

// ---- release velocity ----------------------------------------------------
// How fast the key came back up, measured as spans-per-second of ascent.
// Snatching your hand away shortens the release; lifting gently lengthens it.
const float REL_FAST_SPANS_PER_S = 7.0f;   // this ascent rate reads as 1.0
const float REL_SCALE_FAST       = 0.35f;  // release time multiplier, fast
const float REL_SCALE_SLOW       = 1.60f;  // release time multiplier, slow

// ---- velocity curve ------------------------------------------------------
const float VEL_FAST_MS = 3.0f;
const float VEL_SLOW_MS = 200.0f;
const float VEL_AT_FAST = 127.0f;
const float VEL_AT_SLOW = 20.0f;

const uint16_t DEFAULT_SPAN = 2250;

// ---- boot ----------------------------------------------------------------
const uint32_t SERIAL_WAIT_MS = 1200;  // no host on a power bank, so this
                                       // always runs to its full timeout
const uint32_t SETTLE_BOOT_MS = 250;   // let the rail settle before calibrating
