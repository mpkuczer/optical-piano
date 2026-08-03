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
// Measured with the 's' settle probe: the sense line needs ~320 us to reach
// its final value. At the old 200 us it was still 23 counts high and falling
// at 0.33 counts/us. That offset cancels for note detection (the baseline is
// measured the same way) but not for pressure, where 23 counts is 16% of the
// whole usable window — and it is not even constant, because the audio ISR
// fires every 2.9 ms and stretches this delay when it lands inside it.
// Sampling on the flat part makes that jitter disappear.
// Cost: the sweep goes from ~1.25 ms to ~1.95 ms, so the scan rate drops from
// roughly 800 Hz to 510 Hz. Velocity flight times get coarser; see below.
// Raised from 350 after the crosstalk measurement: the row line's time
// constant is ~121 us, not the ~64 us a small-excursion settle probe
// suggested, so 350 us left ~5.6% of a full excursion bleeding into the next
// column on the same row. 500 us cuts that to 1.6% and the deconvolution
// below removes what remains. Scan rate drops from ~510 Hz to ~370 Hz.
const uint32_t SETTLE_US = 500;

// Fraction of the previous column's reading that survives into this one, on
// the same row: exp(-SETTLE_US / tau). Measure it with the 'k' probe, which
// prints the implied value. Set to 0 to disable the correction entirely.
// 0.016 was derived from a tau estimated off a small-excursion settle probe,
// and it was wrong: with a key held at 0.97 the residual on its row-neighbour
// implied k = 0.047, so only a third of the bleed was being removed. Measure
// it on YOUR hardware with the 'j' probe rather than trusting this number.
const float CROSS_BLEED = 0.047f;   // wait after selecting a column before reading
const int      SWEEP_COLS = 5;

// ---- diagnostics ---------------------------------------------------------
// 500 counts is ~0.4 V: below anything a semiconductor in this signal path
// can produce, so a reading under it means copper, not silicon.
const uint16_t STUCK_LOW_COUNTS = 500;

// Detection thresholds, velocity curves and pressure tuning USED to live
// here. They now belong to the layers that own them — GESTURE_DEFAULT in
// Gesture.cpp and CAL_DEFAULT in Calibration.cpp — because those are covered
// by tests, and a constant tuned outside the reach of its tests is a constant
// that will drift away from them.


// ---- boot ----------------------------------------------------------------
const uint32_t SERIAL_WAIT_MS = 1200;  // no host on a power bank, so this
                                       // always runs to its full timeout
const uint32_t SETTLE_BOOT_MS = 250;   // let the rail settle before calibrating
