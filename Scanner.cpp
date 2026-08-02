// ============================================================================
// Scanner.cpp — key matrix, calibration, strike detection, velocity
// ============================================================================
// HARDWARE MODEL (column-switched)
//   All 25 LEDs sit permanently on 3.3V through their row's 220R.
//   Each row has a 10k pull-up and a sense line to one ADC pin.
//   Each COLUMN has an NPN transistor: base <- 1k <- column GPIO,
//   collector = column return line, emitter = GND.
//
//   Column GPIO HIGH => transistor ON => that column is grounded => its five
//   sensors get a return path and each pulls its OWN row's sense line down.
//   Read all five row ADCs together. Column LOW => open circuit => dead.
//
//   A sweep is 5 column selects x 5 ADC reads = 25 samples but only 5 settle
//   waits, roughly 1 ms. The five sensors lit together are five keys apart,
//   so the arrangement is crosstalk-safe by construction.
//
// VELOCITY
//   A key press is a descent in ADC value. We timestamp when it crosses a
//   shallow threshold and again when it crosses the trigger threshold; the
//   flight time between them is the strike speed. Both crossings are
//   INTERPOLATED between samples, which is what keeps velocity usable even
//   though the sweep rate is only ~1 kHz.
// ============================================================================
#include <Arduino.h>
#include "Config.h"
#include "Scanner.h"
#include "Synth.h"

#if SEND_USB_MIDI
#include <usb_midi.h>
#endif

struct Key {
  uint16_t baseline, on_lv, off_lv, vel_lv;
  uint16_t span;          // learned travel: resting level minus pressed level
  uint16_t note_low;
  uint8_t  state;         // 0 ARMED, 1 DESCENDING, 2 SOUNDING
  bool     rel_armed, above_off;
  uint32_t note_on, rose;
  float    vel_start_us;
  uint16_t prev_v;
  uint32_t prev_us;
  bool     have_prev;
  float    press;         // 0..1 mapped, smoothed
  float    rise_spans_s;  // smoothed upward speed, spans per second
  float    press_ref;     // learned depth of a normally-held key
  float    press_max;     // learned deepest depth when leaning
  float    note_maxdepth; // deepest depth during THIS note
  bool     ref_taken;
};

static Key key[5][5];
static uint32_t now_us = 0;

// ---- matrix primitives ---------------------------------------------------
static void selectColumn(int c) {
  for (int j = 0; j < 5; j++)
    digitalWrite(COL_PIN[j], j == c ? HIGH : LOW);
}
static void deselectAll() {
  for (int j = 0; j < 5; j++) digitalWrite(COL_PIN[j], LOW);
}
static void readColumn(int c, uint16_t out[5]) {
  selectColumn(c);
  delayMicroseconds(SETTLE_US);
  for (int r = 0; r < 5; r++) out[r] = analogRead(ADC_PIN[r]);
  deselectAll();
}

// ---- thresholds ----------------------------------------------------------
static void deriveThresholds(Key &k) {
  k.on_lv  = k.baseline - (uint16_t)(ON_DEPTH        * k.span);
  k.off_lv = k.baseline - (uint16_t)(OFF_DEPTH       * k.span);
  k.vel_lv = k.baseline - (uint16_t)(VEL_START_DEPTH * k.span);
}

static void reportKey(int r, int c) {
  Key &k = key[r][c];
  Serial.print("# cell(R"); Serial.print(r + 1);
  Serial.print(",C"); Serial.print(c + 1);
  Serial.print(" s"); Serial.print(r * 5 + c + 1);
  Serial.print(") base="); Serial.print(k.baseline);
  Serial.print(" span="); Serial.print(k.span);
  Serial.print(" on<"); Serial.print(k.on_lv);
  Serial.print(" off>"); Serial.println(k.off_lv);
}

// ---- setup / calibration -------------------------------------------------
void scannerBegin() {
  analogReadResolution(12);
  for (int j = 0; j < 5; j++) {
    pinMode(COL_PIN[j], OUTPUT);
    digitalWrite(COL_PIN[j], LOW);
  }
}

void scannerCalibrate() {
  uint32_t sum[5][5];
  for (int r = 0; r < 5; r++) for (int c = 0; c < 5; c++) sum[r][c] = 0;

  const int N = 200;
  for (int i = 0; i < N; i++)
    for (int c = 0; c < 5; c++) {
      uint16_t v[5]; readColumn(c, v);
      for (int r = 0; r < 5; r++) sum[r][c] += v[r];
    }

  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      Key &k = key[r][c];
      k.baseline = sum[r][c] / N;
      if (k.span == 0) k.span = DEFAULT_SPAN;
      deriveThresholds(k);
      k.state = 0; k.rel_armed = false; k.above_off = false; k.have_prev = false;
      k.press = 0.0f; k.rise_spans_s = 0.0f;
      k.press_ref = PRESS_SEED_REF; k.press_max = PRESS_SEED_MAX;
      k.note_maxdepth = 0.0f; k.ref_taken = false;
      reportKey(r, c);
    }
}

void scannerResetSpans() {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      key[r][c].span = DEFAULT_SPAN;
      deriveThresholds(key[r][c]);
    }
  Serial.println("# spans reset");
}

// ---- diagnostics ---------------------------------------------------------
void scannerIsolationSnapshot() {
  Serial.println("# isolation snapshot (one line per column):");
  for (int c = 0; c < 5; c++) {
    uint16_t v[5]; readColumn(c, v);
    for (int r = 0; r < 5; r++) {
      Serial.print("s"); Serial.print(r * 5 + c + 1);
      Serial.print("="); Serial.print(v[r]); Serial.print("  ");
    }
    Serial.println();
  }
}

void scannerSettleProbe() {
  Serial.println("# settle probe, column 1 row 1: us,adc");
  selectColumn(0);
  for (int i = 0; i < 150; i++) {
    uint32_t t0 = micros();
    Serial.print(i * 20); Serial.print(",");
    Serial.println(analogRead(ADC_PIN[0]));
    while (micros() - t0 < 20) {}
  }
  deselectAll();
}

// ---- velocity ------------------------------------------------------------
static int velocityFromFlight(float flight_us) {
  float ms = flight_us / 1000.0f;
  if (ms < 0.2f) ms = 0.2f;
  float ls = logf(VEL_SLOW_MS) - logf(VEL_FAST_MS);
  float fr = (logf(ms) - logf(VEL_FAST_MS)) / ls;
  int v = (int)(VEL_AT_FAST + fr * (VEL_AT_SLOW - VEL_AT_FAST) + 0.5f);
  if (v > 127) v = 127;
  if (v < 1) v = 1;
  return v;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Raw ADC level -> depth as a fraction of this key's learned span.
static float depthOf(const Key &k, uint16_t v) {
  if (v >= k.baseline || k.span == 0) return 0.0f;
  return (float)(k.baseline - v) / (float)k.span;
}

// Depth -> 0..1 pressure, using the range this key has actually shown it has.
// The previous version measured against fixed fractions of `span`, which
// cannot work: span is itself learned from the deepest press, so a full press
// always lands at depth 1.0 and pressure pins at maximum the instant the key
// bottoms. Measuring between "held normally" and "leaned on" is the only
// framing that has any range in it.
static float pressureOf(const Key &k, float depth) {
  float range = k.press_max - k.press_ref;
  if (range < PRESS_MIN_RANGE) return 0.0f;
  return clampf((depth - k.press_ref) / range, 0.0f, 1.0f);
}

// Where between the previous sample and this one did the signal cross `th`?
// Without this the timing resolution would be the sweep period (~1 ms), which
// is far too coarse to tell a soft press from a hard one.
static float crossingTime(uint16_t pv, uint32_t pu,
                          uint16_t v,  uint32_t cu, uint16_t th) {
  if (pv == v) return (float)cu;
  float fr = (float)(pv - th) / (float)(pv - v);
  if (fr < 0) fr = 0;
  if (fr > 1) fr = 1;
  return (float)pu + fr * (float)(cu - pu);
}

// ---- per-key state machine ----------------------------------------------
static void feed(int r, int c, uint16_t v) {
  Key &k = key[r][c];
  switch (k.state) {

    case 0: // ARMED — waiting for a descent to begin
      if (v < k.vel_lv) {
        k.vel_start_us = (k.have_prev && k.prev_v > k.vel_lv)
          ? crossingTime(k.prev_v, k.prev_us, v, now_us, k.vel_lv)
          : (float)now_us;
        k.note_low = v;
        k.state = 1;
      }
      break;

    case 1: // DESCENDING — in flight, not yet deep enough to sound
      if (v < k.note_low) k.note_low = v;
      if (now_us - (uint32_t)k.vel_start_us > DESCEND_TIMEOUT_US) {
        k.state = 0;      // armed but never fired: phantom, discard
        break;
      }
      if (v < k.on_lv) {
        float on_us = (k.have_prev && k.prev_v > k.on_lv)
          ? crossingTime(k.prev_v, k.prev_us, v, now_us, k.on_lv)
          : (float)now_us;
        float fl = on_us - k.vel_start_us;
        if (fl < 0) fl = 0;
        int vel = velocityFromFlight(fl);

        synthNoteOn(r, c, vel);
#if SEND_USB_MIDI
        usbMIDI.sendNoteOn(MIDI_NOTE[r][c], vel, 1);
#endif
#if VERBOSE
        Serial.print("s"); Serial.print(r * 5 + c + 1);
        Serial.print(" ON v="); Serial.print(vel);
        Serial.print(" fl="); Serial.print(fl / 1000.0f, 2);
        Serial.println("ms");
#endif
        k.note_on = now_us;
        k.rel_armed = false; k.above_off = false;
        k.note_maxdepth = 0.0f; k.ref_taken = false;
        k.state = 2;
      } else if (v > k.vel_lv + RETREAT_HYST) {
        k.state = 0;      // half-press, withdrawn
      }
      break;

    case 2: // SOUNDING — report pressure, wait for a genuine release
      if (v < k.note_low) k.note_low = v;

      // The key is still under a finger and we still have a position
      // reading, so keep feeding it to the voice. This is the whole point of
      // an optical keybed: a MIDI keyboard would have thrown this away.
      {
        float depth = depthOf(k, v);
        if (depth > k.note_maxdepth) k.note_maxdepth = depth;
        // Take the reference once the strike has settled — before that the
        // key is still moving and the reading means nothing.
        if (!k.ref_taken && (now_us - k.note_on) > PRESS_REF_US) {
          k.press_ref += PRESS_LEARN * (depth - k.press_ref);
          k.ref_taken = true;
        }
        k.press += PRESS_SMOOTH * (pressureOf(k, depth) - k.press);
        synthPressure(r, c, k.press);
      }

      // Smoothed upward speed, for release velocity. Only ascents count;
      // pressing further down must not read as a fast release.
      if (k.have_prev && now_us > k.prev_us && v > k.prev_v && k.span) {
        float spans_s = ((float)(v - k.prev_v) / (float)(now_us - k.prev_us))
                        * 1000000.0f / (float)k.span;
        k.rise_spans_s += PRESS_SMOOTH * (spans_s - k.rise_spans_s);
      }

      if (!k.above_off && v > k.off_lv) { k.above_off = true; k.rose = now_us; }
      else if (k.above_off && v <= k.off_lv) {
        k.above_off = false;
        k.rel_armed = true;               // overshoot completed
      }
      // A soft strike may never overshoot at all; arm on a timer instead.
      if (!k.rel_armed && !k.above_off &&
          (now_us - k.note_on) > OVERSHOOT_WINDOW_US)
        k.rel_armed = true;

      if (k.above_off) {
        bool off = k.rel_armed || (now_us - k.rose) > EXCURSION_MAX_US;
        if (off) {
          float relv = clampf(k.rise_spans_s / REL_FAST_SPANS_PER_S,
                              0.0f, 1.0f);
          synthNoteOff(r, c, relv);
#if SEND_USB_MIDI
          usbMIDI.sendNoteOff(MIDI_NOTE[r][c], (int)(relv * 127.0f), 1);
#endif
#if VERBOSE
          Serial.print("s"); Serial.print(r * 5 + c + 1);
          Serial.println(" OFF");
#endif
          // Learn this key's travel. Note the EMA: the old version only ever
          // GREW span, so one bottomed-out press permanently deepened every
          // threshold on that key — making it harder to trigger and quieter
          // the harder you played it.
          if (k.note_low < k.baseline) {
            uint16_t obs = k.baseline - k.note_low;
            k.span = (uint16_t)(0.9f * k.span + 0.1f * obs);
            deriveThresholds(k);
          }
          // Learn how deep this key goes when leaned on. If you never lean,
          // press_max converges toward press_ref, the range collapses and
          // pressure correctly reports zero rather than inventing motion.
          if (k.note_maxdepth > 0.0f)
            k.press_max += PRESS_LEARN * (k.note_maxdepth - k.press_max);
          k.press = 0.0f;
          k.rise_spans_s = 0.0f;
          k.state = 0;
        }
      }
      break;
  }
  k.prev_v = v;
  k.prev_us = now_us;
  k.have_prev = true;
}

// Live view of one key's depth while you hold it. This is the only way to
// find out whether your key mechanism HAS any travel left after the note
// fires — no amount of firmware can invent range that the hardware doesn't
// produce. Hold a key down and lean on it; watch whether depth moves.
void scannerPressureMonitor() {
  Serial.println("# pressure monitor. Hold ONE key down and lean on it.");
  Serial.println("# Watch 'depth'. If it does not move when you lean, the key");
  Serial.println("# has bottomed out and pressure is a hardware problem.");
  Serial.println("# Send any character to stop.");
  Serial.println("#  key   raw   base  span   depth   min   max   range  press");

  float dmin[5][5], dmax[5][5];
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) { dmin[r][c] = 9.0f; dmax[r][c] = 0.0f; }

  uint32_t nextPrint = 0;
  while (!Serial.available()) {
    int br = -1, bc = -1;
    float bd = 0.10f;                 // ignore keys that are basically at rest
    for (int c = 0; c < 5; c++) {
      uint16_t v[5]; readColumn(c, v);
      for (int r = 0; r < 5; r++) {
        float d = depthOf(key[r][c], v[r]);
        if (d > 0.10f) {
          if (d < dmin[r][c]) dmin[r][c] = d;
          if (d > dmax[r][c]) dmax[r][c] = d;
        }
        if (d > bd) { bd = d; br = r; bc = c; key[r][c].note_low = v[r]; }
      }
    }
    if (br < 0 || millis() < nextPrint) continue;
    nextPrint = millis() + 60;

    Key &k = key[br][bc];
    float lo = dmin[br][bc], hi = dmax[br][bc];
    float range = (lo < 9.0f) ? hi - lo : 0.0f;
    Serial.print("#  s");   Serial.print(br * 5 + bc + 1);
    Serial.print("\t");     Serial.print(k.note_low);
    Serial.print("\t");     Serial.print(k.baseline);
    Serial.print("\t");     Serial.print(k.span);
    Serial.print("\t");     Serial.print(bd, 3);
    Serial.print("\t");     Serial.print(lo < 9.0f ? lo : 0.0f, 3);
    Serial.print("\t");     Serial.print(hi, 3);
    Serial.print("\t");     Serial.print(range, 3);
    Serial.print("\t");     Serial.print(pressureOf(k, bd), 2);
    Serial.print("  ");
    // Crude bar so the motion is visible without reading numbers.
    int bars = (int)(bd * 40.0f);
    for (int i = 0; i < bars && i < 50; i++) Serial.print('#');
    Serial.println();
  }
  while (Serial.available()) Serial.read();

  Serial.println("# --- summary: observed depth range per key while held ---");
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (dmax[r][c] > 0.0f) {
        float lo = dmin[r][c] < 9.0f ? dmin[r][c] : dmax[r][c];
        float range = dmax[r][c] - lo;
        Serial.print("#   s"); Serial.print(r * 5 + c + 1);
        Serial.print("  range="); Serial.print(range, 3);
        Serial.println(range < PRESS_MIN_RANGE
                       ? "  <-- NO usable pressure travel" : "  ok");
      }
}

// ---- one sweep -----------------------------------------------------------
void scannerScanOnce() {
  for (int c = 0; c < SWEEP_COLS; c++) {
    if (c < 5) {
      uint16_t v[5]; readColumn(c, v);
      now_us = micros();
      for (int r = 0; r < 5; r++) feed(r, c, v[r]);
    } else {
      delayMicroseconds(SETTLE_US);
    }
  }
}
