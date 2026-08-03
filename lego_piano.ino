// ============================================================================
// LEGO OPTICAL PIANO — an optical-keybed electrophone
// ============================================================================
// 25 Lego keys, each over a photodarlington. The sensors report POSITION
// continuously, not just on/off, which is the unusual thing about this
// instrument and the thing the architecture is built around.
//
// LAYERS (each one testable without the others, and without a board)
//   Config.h        pins, key map, scan timing — this hardware, nothing else
//   Calibration.*   raw ADC counts  -> normalised position 0..1   [pure]
//   Gesture.*       position stream -> note events + expression   [pure]
//   ModMatrix.*     sources -> destinations, with curves          [pure]
//   Preset.h /
//   Presets.cpp     the patch library
//   Synth.*         oscillators, filters, envelopes, effects
//   Scanner.*       the ADC matrix, and the wiring between layers
//   Diag.*          every serial diagnostic
//
// The pure layers compile with plain g++ and are covered by test/. Every bug
// worth the name in this project lived in a state machine that could not be
// tested; that is why the split exists.
//
// SERIAL COMMANDS
//   0-9  select preset        n / p  next / previous
//   l    list presets         R      dump the active preset's routes
//   b    recalibrate (hands off the keys)
//   r    forget learned travel and widths
//   L    travel learning on/off. On to teach the keys, off to play: while it
//        is on, a crosstalk phantom note can teach a key a false bottom.
//   a    audio CPU and memory
//   t    live trace (keeps playing)
//   w    per-key calibration report
//   k    crosstalk probe (hold one key, see what the others report)
//   j    bleed probe — measures CROSS_BLEED and prints the value to set
//   i    isolation snapshot   x  fault probe   z  bridge probe   s  settle probe
//   P    pressure sweep with the scanner bypassed
//   q    stop scanning (useful when chasing noise)
//
// USB Type must be "Serial + MIDI".
// ============================================================================
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <Encoder.h>

#include "Config.h"
#include "Diag.h"
#include "Preset.h"
#include "Scanner.h"
#include "Synth.h"

#if SEND_USB_MIDI && !defined(USB_MIDI_SERIAL) && !defined(USB_MIDI)
#error "Set Tools -> USB Type -> Serial + MIDI (or set SEND_USB_MIDI to 0)"
#endif

static bool idle = false;

// ---- preset audition -----------------------------------------------------
static const int AUDITION_ROW = 2, AUDITION_COL = 2;
static uint32_t auditionOffAt = 0;

static void selectPresetAndAudition(int idx) {
  scannerAllNotesOff();          // never carry a note across a patch change
  presetSelect(idx);
  synthNoteOn(AUDITION_ROW, AUDITION_COL, 0.72f);
  // Continuous patches map position to loudness, so an audition note needs a
  // position pushed to it or it is silent by design.
  synthExpression(AUDITION_ROW, AUDITION_COL, 0.75f, 0.5f);
  auditionOffAt = millis() + 400;
}

static void serviceAudition() {
  if (auditionOffAt && (int32_t)(millis() - auditionOffAt) >= 0) {
    synthNoteOff(AUDITION_ROW, AUDITION_COL, 0.5f);
    auditionOffAt = 0;
  }
}

// ---- encoder -------------------------------------------------------------
#if HAVE_ENCODER
static Encoder  encoder(ENC_CLK_PIN, ENC_DT_PIN);
static long     encLastDetent = 0;
static bool     swLast        = true;
static uint32_t swLastChange  = 0;

static void serviceEncoder() {
  long detent = encoder.read() / ENC_COUNTS_PER_DETENT;
  if (detent != encLastDetent) {
    selectPresetAndAudition(presetCurrent() + (int)(detent - encLastDetent));
    encLastDetent = detent;
  }
  bool sw = digitalRead(ENC_SW_PIN);
  if (sw != swLast && millis() - swLastChange > 30) {
    swLastChange = millis(); swLast = sw;
    if (!sw) scannerCalibrate();
  }
}
#endif

// ---- serial --------------------------------------------------------------
static void handleCommand(char ch) {
  if (ch >= '0' && ch <= '9') { selectPresetAndAudition(ch - '0'); return; }
  switch (ch) {
    case 'n': selectPresetAndAudition(presetCurrent() + 1); break;
    case 'p': selectPresetAndAudition(presetCurrent() - 1); break;
    case 'l': presetList();              break;
    case 'R': presetDumpRoutes();        break;
    case 'b': scannerCalibrate();        break;
    case 'r': scannerResetCalibration(); break;
    case 'L': scannerSetLearn(!scannerLearn());
              Serial.println(scannerLearn()
                ? "# travel learning ON — play every key to its bottom once"
                : "# travel learning FROZEN — phantom notes can no longer"
                  " teach a false bottom");
              break;
    case 'a': synthReportLoad();         break;
    case 't': diagTraceToggle();         break;
    case 'w': diagKeyReport();           break;
    case 'k': diagCrosstalkProbe();      break;
    case 'j': diagBleedProbe();          break;
    case 'i': diagIsolationSnapshot();   break;
    case 'x': diagFaultProbe();          break;
    case 'z': diagBridgeProbe();         break;
    case 's': diagSettleProbe(0, 0);     break;
    case 'P': synthPressureTest();       break;
    case 'q': idle = !idle;
              Serial.println(idle ? "# idle" : "# scanning"); break;
    default: break;
  }
}

// ---- main ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < SERIAL_WAIT_MS) {}

  synthBegin();          // audio first: the codec must be up before anything
  scannerBegin();
#if HAVE_ENCODER
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
#endif

  Serial.println("# warming up the sensors before calibrating, hands off...");
  delay(SETTLE_BOOT_MS);
  scannerCalibrate();
#if HAVE_ENCODER
  encLastDetent = encoder.read() / ENC_COUNTS_PER_DETENT;
#endif

  Serial.println("# READY");
  Serial.println("# 0-9 preset  n/p next/prev  l list  R routes  b recal  t trace  w keys");
  Serial.println("# L toggles travel learning — play each key to its bottom, then turn it off");
  presetList();
}

void loop() {
  while (Serial.available()) handleCommand(Serial.read());

  if (!idle) {
    scannerScanOnce();
    diagServiceTrace();
    serviceAudition();
#if HAVE_ENCODER
    serviceEncoder();
#endif
  }
  synthUpdate();

#if SEND_USB_MIDI
  while (usbMIDI.read()) {}
#endif
}
