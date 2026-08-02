// ============================================================================
// LEGO OPTICAL PIANO
// Teensy 4.1 + Audio Shield Rev D
// ============================================================================
// 25 Lego keys, each with a photodarlington sensor. Pressing a key blocks
// light; how FAST it gets blocked is the strike velocity. Each key drives one
// permanently-assigned synth voice.
//
// FILES
//   Config.h      pins, key map, detection thresholds — the only file that
//                 describes your particular hardware
//   Preset.h      what a patch is, with every field documented
//   Presets.cpp   the patch library — add a row and it appears
//   Scanner.*     reads keys, measures velocity, calls the synth
//   Synth.*       oscillators, filters, envelopes, effects
//
// SERIAL COMMANDS
//   0-9   select preset          n / p   next / previous preset
//   l     list presets           b       recalibrate (hands off the keys)
//   r     reset learned spans    a       audio CPU and memory
//   i     isolation snapshot     s       settle probe
//   q     idle (stop scanning — useful when chasing noise)
//
// USB Type must be "Serial + MIDI".
// ============================================================================

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <Encoder.h>

#include "Config.h"
#include "Preset.h"
#include "Scanner.h"
#include "Synth.h"

#if SEND_USB_MIDI && !defined(USB_MIDI_SERIAL) && !defined(USB_MIDI)
#error "Set Tools -> USB Type -> Serial + MIDI (or set SEND_USB_MIDI to 0)"
#endif

static bool idle = false;

// ---- preset audition -----------------------------------------------------
// Play a short note on preset change so you can hear what you landed on
// without touching a key. Middle of the keyboard.
static const int AUDITION_ROW = 2, AUDITION_COL = 2;
static uint32_t auditionOffAt = 0;

static void selectPresetAndAudition(int idx) {
  presetSelect(idx);
  if (synthVoiceIdle(AUDITION_ROW, AUDITION_COL)) {
    synthNoteOn(AUDITION_ROW, AUDITION_COL, 92);
    auditionOffAt = millis() + 400;
  }
}

static void serviceAudition() {
  if (auditionOffAt && (int32_t)(millis() - auditionOffAt) >= 0) {
    synthNoteOff(AUDITION_ROW, AUDITION_COL);
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
  bool sw = digitalRead(ENC_SW_PIN);            // LOW = pressed
  if (sw != swLast && millis() - swLastChange > 30) {
    swLastChange = millis();
    swLast = sw;
    if (!sw) {
      Serial.println("# button: recalibrating, hands off the keys");
      scannerCalibrate();
    }
  }
}
#endif

// ---- serial --------------------------------------------------------------
static void handleCommand(char ch) {
  if (ch >= '0' && ch <= '9') { selectPresetAndAudition(ch - '0'); return; }
  switch (ch) {
    case 'n': selectPresetAndAudition(presetCurrent() + 1); break;
    case 'p': selectPresetAndAudition(presetCurrent() - 1); break;
    case 'l': presetList();                                 break;
    case 'b': Serial.println("# recalibrating, hands off the keys");
              scannerCalibrate();                           break;
    case 'r': scannerResetSpans();                          break;
    case 'a': synthReportLoad();                            break;
    case 'i': scannerIsolationSnapshot();                   break;
    case 's': scannerSettleProbe();                         break;
    case 'q': idle = !idle;
              Serial.println(idle ? "# idle (scanning stopped)"
                                  : "# scanning");          break;
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

  delay(SETTLE_BOOT_MS);
  scannerCalibrate();
#if HAVE_ENCODER
  encLastDetent = encoder.read() / ENC_COUNTS_PER_DETENT;
#endif

  Serial.println("# READY");
  Serial.println("# 0-9 preset   n/p next/prev   l list   b recal   a load");
  presetList();
}

void loop() {
  while (Serial.available()) handleCommand(Serial.read());

  if (!idle) {
    scannerScanOnce();
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
