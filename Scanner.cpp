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
#include "Preset.h"
#include "Press.h"
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
  PressState pressure;    // learned lean width — see Press.h, tested by
                          //   test_press.cpp against real captured traces
  float    disp_depth;    // last depth computed, for the live trace
  uint16_t disp_raw;      // last raw ADC reading — the same number the
                          //   isolation snapshot prints, so the two views
                          //   can be compared directly
  bool     dead;          // sensor failed health check; ignored entirely
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
// Saturating subtraction. These are uint16_t, and baseline - fraction*span
// goes NEGATIVE for any key whose resting level is low — a failed sensor, or
// simply a dim one. Unsigned wrap then produces a threshold near 65000 that
// every reading satisfies. Clamping to zero fails the other way: the key
// never triggers, which is the correct behaviour for a sensor that cannot
// report a press.
static uint16_t levelBelow(uint16_t baseline, float frac, uint16_t span) {
  uint32_t drop = (uint32_t)(frac * (float)span);
  return (baseline > drop) ? (uint16_t)(baseline - drop) : 0;
}

static void deriveThresholds(Key &k) {
  k.on_lv  = levelBelow(k.baseline, ON_DEPTH,        k.span);
  k.off_lv = levelBelow(k.baseline, OFF_DEPTH,       k.span);
  k.vel_lv = levelBelow(k.baseline, VEL_START_DEPTH, k.span);
}

static void reportKey(int r, int c) {
  Key &k = key[r][c];
  Serial.print("# cell(R"); Serial.print(r + 1);
  Serial.print(",C"); Serial.print(c + 1);
  Serial.print(" s"); Serial.print(r * 5 + c + 1);
  Serial.print(") base="); Serial.print(k.baseline);
  if (k.dead) {
    Serial.print("  *** DEAD: resting level is below this key's own trigger"
                 " threshold (");
    Serial.print((uint16_t)(ON_DEPTH * k.span));
    Serial.println("). It cannot register a press. Key disabled.");
    return;
  }
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

// Called at the end of calibration: one obvious line rather than needing the
// reader to spot a flag among 25 rows.
static void reportDeadKeys() {
  int n = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) if (key[r][c].dead) n++;
  if (n == 0) { Serial.println("# all 25 sensors healthy"); return; }
  Serial.print("# WARNING: "); Serial.print(n);
  Serial.print(n == 1 ? " sensor is" : " sensors are");
  Serial.print(" dead and disabled:");
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (key[r][c].dead) {
        Serial.print(" s"); Serial.print(r * 5 + c + 1);
        Serial.print("("); Serial.print(key[r][c].baseline); Serial.print(")");
      }
  Serial.println();
  Serial.println("# If a whole ROW is listed, the shared sense line is at"
                 " fault, not the sensors.");
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
      // Dead only if the key cannot physically reach its own trigger point.
      k.dead = (k.baseline <= (uint16_t)(ON_DEPTH * k.span));
      if (k.span == 0) k.span = DEFAULT_SPAN;
      deriveThresholds(k);
      k.state = 0; k.rel_armed = false; k.above_off = false; k.have_prev = false;
      k.press = 0.0f; k.rise_spans_s = 0.0f;
      pressInit(k.pressure);
      reportKey(r, c);
    }
  reportDeadKeys();
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
  Serial.println("# isolation snapshot (one line per column). '!' = dead.");
  Serial.println("# A whole ROW dead => the shared sense line is at fault;");
  Serial.println("# a single cell dead => that sensor is at fault.");
  for (int c = 0; c < 5; c++) {
    uint16_t v[5]; readColumn(c, v);
    for (int r = 0; r < 5; r++) {
      Serial.print("s"); Serial.print(r * 5 + c + 1);
      Serial.print("="); Serial.print(v[r]);
      Serial.print(key[r][c].dead ? "! " : "  ");
    }
    Serial.println();
  }
}

// ---- fault probe ---------------------------------------------------------
// Separates the fault classes by using the column transistor as the test
// switch. A sensor can only pull its row down when ITS column is grounded, so
// anything that pulls low with every column OFF is downstream of the matrix
// entirely — a short, or a missing pull-up. Anything that pulls low in one
// column only is the sensor for that cell. Anything low across several
// columns has a path to ground that bypasses the column transistor.
void scannerFaultProbe() {
  Serial.println("# ---- fault probe ----");

  // 1) Everything off. The 10k pull-ups should hold every row near full scale.
  deselectAll();
  delayMicroseconds(SETTLE_US * 4);
  uint16_t idle[5];
  for (int r = 0; r < 5; r++) idle[r] = analogRead(ADC_PIN[r]);

  Serial.println("# 1) all columns OFF (expect ~4095 on every row):");
  bool rowFault[5];
  for (int r = 0; r < 5; r++) {
    rowFault[r] = idle[r] < STUCK_LOW_COUNTS;
    Serial.print("#    row "); Serial.print(r + 1);
    Serial.print(" = ");       Serial.print(idle[r]);
    Serial.print(" (");        Serial.print(idle[r] * 3.3f / 4095.0f, 3);
    Serial.print(" V)");
    if (rowFault[r])
      Serial.println("   <<< PULLED LOW WITH NO COLUMN SELECTED");
    else
      Serial.println("   ok");
  }

  // 2) One column at a time.
  uint16_t m[5][5];
  for (int c = 0; c < 5; c++) {
    uint16_t v[5]; readColumn(c, v);
    for (int r = 0; r < 5; r++) m[r][c] = v[r];
  }
  Serial.println("# 2) per column (rows down, columns across):");
  for (int r = 0; r < 5; r++) {
    Serial.print("#    ");
    for (int c = 0; c < 5; c++) {
      Serial.print("s");  Serial.print(r * 5 + c + 1);
      Serial.print("=");  Serial.print(m[r][c]);
      Serial.print(m[r][c] < STUCK_LOW_COUNTS ? "! " : "  ");
    }
    Serial.println();
  }

  // 3) Verdict.
  Serial.println("# 3) verdict:");
  bool any = false;
  for (int r = 0; r < 5; r++) {
    if (rowFault[r]) {
      Serial.print("#    ROW "); Serial.print(r + 1);
      Serial.println(" sense line is shorted to ground, or its 10k pull-up is");
      Serial.println("#      open. All five keys in that row are affected. This");
      Serial.println("#      is wiring, not a sensor.");
      any = true;
      continue;
    }
    for (int c = 0; c < 5; c++) {
      int lowIn = 0;
      for (int cc = 0; cc < 5; cc++)
        if (m[r][cc] < STUCK_LOW_COUNTS) lowIn++;
      if (m[r][c] >= STUCK_LOW_COUNTS) continue;
      Serial.print("#    s"); Serial.print(r * 5 + c + 1);
      if (lowIn == 1) {
        Serial.println(" is low in ITS OWN column only.");
        Serial.println("#      Check the VOLTAGE. A photodarlington in hard");
        Serial.println("#      saturation sits around 0.8-1.2 V, and the column");
        Serial.println("#      NPN adds ~0.1 V on top. So:");
        Serial.println("#        > 0.7 V  -> sensor saturated (or flooded with");
        Serial.println("#                    light; shade the rail and re-run)");
        Serial.println("#        < 0.5 V  -> NOT a semiconductor. Copper, or a");
        Serial.println("#                    missing pull-up. Reach for a meter.");
      } else {
        Serial.println(" is low in SEVERAL columns:");
        Serial.println("#      it has a path to ground that bypasses the column");
        Serial.println("#      transistor. Look for a solder bridge between this");
        Serial.println("#      sensor's emitter and ground.");
      }
      any = true;
    }
  }
  // A column entirely low points at its transistor rather than its sensors.
  for (int c = 0; c < 5; c++) {
    int n = 0;
    for (int r = 0; r < 5; r++) if (m[r][c] < STUCK_LOW_COUNTS) n++;
    if (n == 5) {
      Serial.print("#    COLUMN "); Serial.print(c + 1);
      Serial.println(" is low on every row: its NPN is stuck on, or the");
      Serial.println("#      column return line is shorted to ground.");
      any = true;
    }
  }
  if (!any) Serial.println("#    no faults found — everything reads sane.");
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

// Average window across keys that have taught themselves. Keys vary — travel,
// sensor alignment and how much give each hinge has all differ — so this is
// only a starting point, replaced by a key's own first note.
static void seedUnlearnedKeys() {
  float sw = 0.0f;
  int n = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      const PressState &p = key[r][c].pressure;
      if (p.learned && !p.borrowed) { sw += p.width; n++; }
    }
  if (n == 0) return;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      pressSeedFrom(key[r][c].pressure, sw / n);
}

// ---- continuous gesture --------------------------------------------------
// A completely separate reading of the same sensor. No strike detection, no
// velocity, no overshoot logic, no release arming — every one of those exists
// to answer "has the hammer been thrown?", which is not a question a theremin
// asks. Position in, sound out.
static void feedContinuous(int r, int c, uint16_t v, Key &k) {
  float depth = depthOf(k, v);
  k.disp_depth = depth;

  if (k.state != 2) {
    // Track the resting level while the key is up. Photodarlingtons drift
    // with temperature and room light, and a 5% threshold is far too tight
    // to survive a fixed boot-time baseline.
    if (depth < BASELINE_GATE && v < 4095)
      k.baseline += BASELINE_TRACK * ((float)v - (float)k.baseline);

    if (depth > CONT_ON_DEPTH) {
      pressNoteOn(k.pressure);
      pressSetMode(k.pressure, PRESS_ABSOLUTE);
      // Establish position first so the voice opens at its true level —
      // which at this depth is zero.
      k.press = pressUpdate(k.pressure, PRESS_DEFAULT, depth, 0.0f, 1000.0f);
      synthPressure(r, c, k.press);
      synthNoteOn(r, c, 64);          // velocity is ignored in this mode
#if SEND_USB_MIDI
      usbMIDI.sendNoteOn(MIDI_NOTE[r][c], 64, 1);
#endif
      k.note_on = now_us;
      k.state = 2;
    }
    return;
  }

  float dt_ms = k.have_prev ? (now_us - k.prev_us) * 0.001f : 1.2f;
  k.press = pressUpdate(k.pressure, PRESS_DEFAULT, depth,
                        (now_us - k.note_on) * 0.001f, dt_ms);
  synthPressure(r, c, k.press);

  if (depth < CONT_OFF_DEPTH) {
    synthNoteOff(r, c, 0.5f);
#if SEND_USB_MIDI
    usbMIDI.sendNoteOff(MIDI_NOTE[r][c], 0, 1);
#endif
    k.state = 0;
  }
}

// ---- per-key state machine ----------------------------------------------
static void feed(int r, int c, uint16_t v) {
  Key &k = key[r][c];
  k.disp_raw = v;
  if (k.dead) return;      // a failed sensor must not be allowed to play

  if (presetGesture() == GESTURE_CONTINUOUS) {
    feedContinuous(r, c, v, k);
    k.prev_v = v; k.prev_us = now_us; k.have_prev = true;
    return;
  }

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

        if (presetGesture() == GESTURE_CONTINUOUS) {
          // Establish position BEFORE the note starts, or the voice opens at
          // zero and swells for no musical reason.
          pressSetMode(k.pressure, PRESS_ABSOLUTE);
          k.disp_depth = depthOf(k, v);
          k.press = pressUpdate(k.pressure, PRESS_DEFAULT,
                                k.disp_depth, 0.0f, 1000.0f);
          synthPressure(r, c, k.press);
        }
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
        pressNoteOn(k.pressure);
        // The preset decides how this key should be read. Chosen per note, so
        // switching patch mid-performance takes effect on the next note.
        pressSetMode(k.pressure,
                     presetGesture() == GESTURE_CONTINUOUS ? PRESS_ABSOLUTE
                                                           : PRESS_RELATIVE);
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
        float dt_ms  = k.have_prev ? (now_us - k.prev_us) * 0.001f : 1.2f;
        float age_ms = (now_us - k.note_on) * 0.001f;
        k.disp_depth = depthOf(k, v);
        k.press = pressUpdate(k.pressure, PRESS_DEFAULT,
                              k.disp_depth, age_ms, dt_ms);
        synthPressure(r, c, k.press);
      }

      // Smoothed upward speed, for release velocity. Only ascents count;
      // pressing further down must not read as a fast release.
      if (k.have_prev && now_us > k.prev_us && v > k.prev_v && k.span) {
        float spans_s = ((float)(v - k.prev_v) / (float)(now_us - k.prev_us))
                        * 1000000.0f / (float)k.span;
        k.rise_spans_s += REL_SMOOTH * (spans_s - k.rise_spans_s);
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
          Serial.print(" OFF relv="); Serial.print(relv, 2);
          // The learned window, printed on the REAL path. The 'm' monitor
          // cannot show this: it reads the ADC directly and never runs the
          // note state machine, so it always displayed an unlearned zero.
          Serial.print("  note[rest="); Serial.print(k.pressure.noteRest, 3);
          Serial.print(" max=");        Serial.print(k.pressure.noteMax, 3);
          Serial.print(" lean=");
          Serial.print(k.pressure.haveRest
                       ? k.pressure.noteMax - k.pressure.noteRest : 0.0f, 3);
          Serial.print("]  width=");    Serial.print(pressRange(k.pressure), 4);
          Serial.println(k.pressure.learned ? "" : "  NOT LEARNED");
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
          if (k.pressure.mode != PRESS_ABSOLUTE) {
            pressNoteOff(k.pressure, PRESS_DEFAULT);
            seedUnlearnedKeys();  // every key learned makes the rest usable
          }
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
// Which keys have a usable pressure window, and how wide. Keys differ
// mechanically, so some will always respond better than others; this shows
// which ones and by how much, instead of leaving it to feel.
void scannerPressureReport() {
  Serial.println("# per-key pressure windows");
  Serial.println("#  key   width   counts  state");
  Serial.println("#  (width is the lean this key has shown, as a fraction of");
  Serial.println("#   its span; counts is that in raw ADC units)");
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      const Key &k = key[r][c];
      if (k.dead) {
        Serial.print("#  s");  Serial.print(r * 5 + c + 1);
        Serial.print("\t--\t--\tDEAD, base=");
        Serial.println(k.baseline);
        continue;
      }
      float rng = pressRange(k.pressure);
      Serial.print("#  s");    Serial.print(r * 5 + c + 1);
      Serial.print("\t");     Serial.print(rng, 4);
      Serial.print("\t");     Serial.print((int)(rng * k.span));
      if (!k.pressure.learned)      Serial.println("\tnever played");
      else if (k.pressure.borrowed) Serial.println("\tborrowed");
      else if (rng < 0.025f)        Serial.println("\town - NARROW, will feel coarse");
      else                          Serial.println("\town");
    }
}

void scannerPressureMonitor() {
  Serial.println("# pressure monitor: RAW DEPTH ONLY. This does not run the");
  Serial.println("# note state machine, so ref/top stay zero until you have");
  Serial.println("# played real notes. Use VERBOSE note-off lines for those.");
  Serial.println("# Hold ONE key down and lean on it.");
  Serial.println("# Watch 'depth'. If it does not move when you lean, the key");
  Serial.println("# has bottomed out and pressure is a hardware problem.");
  Serial.println("# Send any character to stop.");
  Serial.println("#  key\tbase\tspan\tdepth\thMin\thMax\trange\twidth\tlearned");

  float dmin[5][5], dmax[5][5];
  uint32_t downSince[5][5];
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      dmin[r][c] = 9.0f; dmax[r][c] = 0.0f; downSince[r][c] = 0;
    }

  uint32_t nextPrint = 0;
  while (!Serial.available()) {
    int br = -1, bc = -1;
    float bd = 0.10f;                 // ignore keys that are basically at rest
    for (int c = 0; c < 5; c++) {
      uint16_t v[5]; readColumn(c, v);
      uint32_t nowms = millis();
      for (int r = 0; r < 5; r++) {
        float d = depthOf(key[r][c], v[r]);
        if (d < 0.60f) { downSince[r][c] = 0; }
        else {
          if (downSince[r][c] == 0) downSince[r][c] = nowms;
          // Only count depth once the key has been down and still for a
          // moment. Including the strike is what made the earlier range
          // reading 1.100 — that was the whole travel, not the held part.
          else if (nowms - downSince[r][c] > (uint32_t)PRESS_DEFAULT.settleMs) {
            if (d < dmin[r][c]) dmin[r][c] = d;
            if (d > dmax[r][c]) dmax[r][c] = d;
          }
        }
        if (d > bd) { bd = d; br = r; bc = c; }
      }
    }
    if (br < 0 || millis() < nextPrint) continue;
    nextPrint = millis() + 60;

    Key &k = key[br][bc];
    float lo = dmin[br][bc], hi = dmax[br][bc];
    float range = (lo < 9.0f) ? hi - lo : 0.0f;
    Serial.print("#  s");   Serial.print(br * 5 + bc + 1);
    Serial.print("\t");     Serial.print(k.baseline);
    Serial.print("\t");     Serial.print(k.span);
    Serial.print("\t");     Serial.print(bd, 3);
    Serial.print("\t");     Serial.print(lo < 9.0f ? lo : 0.0f, 3);
    Serial.print("\t");     Serial.print(hi, 3);
    Serial.print("\t");     Serial.print(range, 3);
    Serial.print("\t");     Serial.print(k.pressure.width, 4);
    Serial.print("\t");     Serial.print(k.pressure.learned ? "y" : "n");
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
        Serial.println(range < PRESS_DEFAULT.minRange
                       ? "  <-- NO usable pressure travel" : "  ok");
      }
}

// ---- live trace ----------------------------------------------------------
// Unlike the 'm' monitor, this does NOT take over the loop. It piggybacks on
// normal scanning, so notes sound, the state machine runs and the learner
// learns while you watch. That combination is what was missing: every earlier
// diagnostic forced a choice between hearing the instrument and seeing the
// numbers, which is exactly the wrong trade when the question is whether the
// numbers match the sound.
static bool     traceOn   = false;
static uint32_t traceNext = 0;

void scannerTraceToggle() {
  traceOn = !traceOn;
  if (traceOn) {
    Serial.println("# trace ON — play and lean, sound keeps running");
    Serial.println("#  key\traw\tbase\tdepth\trest\tlean\twidth\tpress\tmode");
    Serial.println("#  (raw is the same reading the 'i' isolation snapshot"
                   " shows; depth = (base-raw)/span)");
  } else {
    Serial.println("# trace off");
  }
}

static void serviceTrace() {
  if (!traceOn) return;
  uint32_t now = millis();
  if (now < traceNext) return;

  // Follow the most recently struck key that is still sounding.
  int br = -1, bc = -1;
  uint32_t newest = 0;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++)
      if (key[r][c].state == 2 && key[r][c].note_on >= newest) {
        newest = key[r][c].note_on; br = r; bc = c;
      }
  if (br < 0) return;                 // nothing held: stay quiet
  traceNext = now + 40;               // 25 Hz is plenty to see a lean

  const Key &k = key[br][bc];
  const PressState &p = k.pressure;
  float lean = p.haveRest ? (p.noteMax - p.noteRest) : 0.0f;

  Serial.print("#  s");  Serial.print(br * 5 + bc + 1);
  Serial.print("\t");   Serial.print(k.disp_raw);
  // Printed alongside because the baseline now TRACKS: raw alone cannot be
  // interpreted without knowing what the key currently rests at.
  Serial.print("\t");   Serial.print(k.baseline);
  Serial.print("\t");   Serial.print(k.disp_depth, 3);
  Serial.print("\t");   Serial.print(p.haveRest ? p.noteRest : 0.0f, 3);
  Serial.print("\t");   Serial.print(lean, 3);
  Serial.print("\t");   Serial.print(pressRange(p), 4);
  Serial.print("\t");   Serial.print(k.press, 2);
  Serial.print(p.mode == PRESS_ABSOLUTE ? " abs" : (p.learned ? " rel" : " ?"));
  int bars = (int)(k.press * 30.0f);
  for (int i = 0; i < bars; i++) Serial.print('#');
  Serial.println();
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
  serviceTrace();
}
