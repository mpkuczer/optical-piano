// ============================================================================
// Options.h — LAYER 2b: the keybed as a control surface, not an instrument
// ============================================================================
// Arduino-free. Testable with plain g++.
//
// Every way of changing anything about this instrument is a serial command or
// an encoder that is not wired (HAVE_ENCODER, Config.h). On a power bank with
// no host attached the piano boots into preset 0 and stays there for as long
// as it is switched on. This layer is the way out of that: a gesture no piece
// of music contains, made on the keys themselves.
//
// THE GESTURE IS A HOLD, and it has to be. The keybed's whole vocabulary is
// "press a key", so a mode switch must be something a player cannot do by
// accident while playing — and a chord is not that. Three adjacent notes
// sounded together is a cluster somebody will play. Three adjacent notes held
// down without moving for five seconds is not; the shortest gesture that
// separates control from music here is a long one, and that is the cost of
// having no buttons.
//
// WHAT OPTIONS MODE IS NOT. It is not a menu you are trapped in. Twenty-three
// of the twenty-five keys keep playing exactly as they did — only the two at
// the ends are taken away, and they are taken away for the whole time you are
// in the mode. Choosing a sound and hearing it are the same activity, so the
// mode that chooses sounds is the mode that still makes them.
//
// ---------------------------------------------------------------------------
// WHY THIS LAYER READS POSITION AND NOT GESTURE EVENTS
// ---------------------------------------------------------------------------
// The obvious implementation is to watch note-ons. It is wrong, because a
// note-on does not mean the same thing twice: presetGesture() hands Gesture a
// different GestureConfig for every patch, so `theremin` triggers at 0.150 of
// travel and a GATED organ at 0.400 — and under ART_TRIGGERED a press does not
// hold anything at all, it LATCHES, so the second key of the chord would be
// turning the first one's note off while this layer believed both were down.
//
// A control surface whose behaviour depends on the loaded patch is not a
// control surface. So this reads the LAYER 1 output — calUpdate's position,
// before Gesture sees it — through its own Schmitt trigger with its own
// thresholds. Held means held, in every preset, in every articulation. It is
// the same argument Gesture.h makes for why a patch is not allowed to reach
// down and rescale a key; this is that argument one layer further down.
//
// The thresholds are deliberately far above the ones Gesture uses. Detection
// wants the lowest floor that clears crosstalk, because a note that starts
// late is a note that feels wrong. This wants the opposite: nothing here is
// time-critical, and a phantom that reaches 0.150 and holds must not be able
// to spend five seconds pretending to be a finger.
//
// ---------------------------------------------------------------------------
// THE TWO RULES THAT ARE NOT OBVIOUS
// ---------------------------------------------------------------------------
// THE RE-ARM LATCH. At the instant the mode changes, the chord is still held —
// you cannot let go of three keys in the same millisecond the timer expires.
// Without a latch that same press immediately starts timing the next
// transition, and holding the chord would flap the mode every five seconds.
// So a transition disarms the chord until every one of its keys has come back
// up. One press, one transition.
//
// THE ARM DELAY, and it exists because of one specific collision. The default
// chord is the bottom three keys and the default "previous preset" key is the
// leftmost — which is the bottom key. They overlap, and they overlap in the
// direction that matters: pressing the exit chord looks exactly like pressing
// the preset-down key, plus two notes.
//
// The fix is to make a nav key wait. It acts only after navArmMs of being held
// with the chord NOT complete. Three fingers landing together are inside the
// same sweep or two — a few milliseconds — so by the time the leftmost key has
// been down 150 ms the chord is long since recognised and the press is spent
// on the mode instead. A single finger on the same key sees a lag of 150 ms
// before the preset changes, which on a control that is not part of a
// performance is not a lag anybody will find.
//
// It would have been simpler to move the chord off the leftmost key. That
// trades a rule nobody sees for a gesture nobody can remember: "the bottom
// three" is a thing you can tell someone, and "the second, third and fourth
// from the bottom" is not.
// ============================================================================
#pragma once
#include <stdint.h>

const int OPT_NUM_KEYS  = 25;
const int OPT_CHORD_MAX = 5;

struct OptionsConfig {
  // Key INDICES, row * 5 + col — the same numbering as the voices, which on
  // this key map (Config.h) ascends in pitch, so index 0 is the lowest key and
  // 24 the highest.
  uint8_t chord[OPT_CHORD_MAX];
  uint8_t nChord;
  uint8_t navPrev, navNext;

  float   holdMs;      // how long the chord must be held to switch mode
  float   navArmMs;    // how long a nav key must be held before it acts

  // Schmitt trigger on position. Wide, on purpose — see the header.
  float   keyOn, keyOff;
  // Consecutive samples over the threshold before a key counts as held, for
  // the same reason GestureConfig has onSamples: rows are shared, and a
  // neighbour moving fast puts a spike on the line that the bleed correction
  // cannot model. No finger produces a 10 ms press.
  uint8_t onSamples, offSamples;
};

extern const OptionsConfig OPTIONS_DEFAULT;

struct OptionsState {
  bool     active;

  uint32_t held;                    // one bit per key, debounced
  uint8_t  over[OPT_NUM_KEYS];      // Schmitt persistence counters
  uint8_t  under[OPT_NUM_KEYS];
  float    downMs[OPT_NUM_KEYS];    // when each key last became held

  bool     chordHeld;
  float    chordSinceMs;            // when the full chord became held

  // Set by a transition, cleared only when every chord key is up. Blocks both
  // the next transition and any nav action, so the press that changed the mode
  // cannot also change a preset on its way out.
  bool     rearmPending;

  bool     navFired[OPT_NUM_KEYS];  // one action per press, not per sample
};

// What one sweep produced. `enter` and `exit` are mutually exclusive, and a
// transition never carries a presetDelta with it.
struct OptionsEvent {
  bool   enter;
  bool   exit;
  int8_t presetDelta;   // -1, 0 or +1
};

void optionsInit(OptionsState &s);

// Feed every key's position for this sweep. Call ONCE per sweep, after the
// whole matrix has been read — the chord test is inherently about all the keys
// at the same instant, and a per-key call could not answer it.
OptionsEvent optionsUpdate(OptionsState &s, const OptionsConfig &c,
                           const float pos[OPT_NUM_KEYS], float nowMs);

bool optionsActive(const OptionsState &s);

// Force the mode, for the serial console. Returns the event the caller should
// act on, so a keyboard transition and a typed one produce identical sound and
// identical cleanup rather than two code paths that drift apart. Arms the
// re-arm latch exactly as a real transition would.
OptionsEvent optionsForce(OptionsState &s, const OptionsConfig &c, bool on);

// Is this key currently reserved by the mode? Scanner asks per key, so that a
// reserved key makes no note, no MIDI and no trace line while options is up.
bool optionsReserved(const OptionsState &s, const OptionsConfig &c, int key);
