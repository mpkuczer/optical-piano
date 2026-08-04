// ============================================================================
// Gesture.h — LAYER 2: position stream -> note events + expression signals
// ============================================================================
// Arduino-free. Testable with plain g++.
//
// This layer answers one question: what IS a note on this instrument? The
// answer differs per instrument and has nothing to do with the sound:
//
//   CONTINUOUS  theremin, ondes martenot. No trigger point at all. The note
//               exists whenever the key is off its rest, and the sound is a
//               pure function of where your finger is now.
//   STRUCK      piano. Detect the descent, time the flight, latch a velocity.
//               What happens afterwards does not matter.
//   GATED       organ. Schmitt trigger, no velocity, no PRESSURE. Absolute
//               position is still reported, because "where is the key" is
//               always a meaningful question — a drawbar per key needs it,
//               and an organ's stops are not a pressure gesture.
//   PLUCKED     harpsichord. Trigger on crossing but DISCARD velocity — a
//               plectrum plucks identically however hard you hit it — then
//               keep reading position, so the finger can still add vibrato.
//   TRIGGERED   the key as a LATCHING switch — a light switch, not a doorbell.
//               Press and let go: the note sounds, and keeps sounding with
//               your hand nowhere near it. Press the same key again: it stops.
//               Every other articulation here ends a note when the key comes
//               back up; this one ignores the key coming up entirely.
//
// WHY THIS IS THE ONE ARTICULATION THE KEYBOARD ITSELF SUGGESTS. Every other
// one is imitating an instrument that already exists, and all of them are
// bounded by ten fingers: a note lasts as long as a finger can be spared for
// it. A latch removes that. Twenty-five keys can all be sounding with both
// hands free, and turning one off is a gesture aimed at that one key rather
// than an act of letting go. Chords get BUILT and then edited a note at a
// time, which no keyboard instrument can do and no sustain pedal approximates
// — a pedal holds everything indiscriminately and releases everything at once.
//
// WHAT IT COSTS: a latched note has no timeout and no natural end. If one is
// latched by accident it sounds until that key is pressed again or something
// clears it, which is why scannerAllNotesOff and the 'o' serial command exist.
// The saving grace is the threshold: a latch can only be armed by crossing
// onPos, 40% of the key's travel, and measured crosstalk residual is ~6.5%.
// A phantom note cannot reach the trigger, so the failure mode that would
// really hurt here is the one this articulation is furthest from.
//
// THE PRESS IS STILL MEASURED. Latching says when a note starts and stops, not
// how loud it is. TRIGGERED reads flight time like STRUCK, differing in two
// places, both about velocity being a report rather than a verdict:
//
//   the floor      past velSlowMs, STRUCK keeps extrapolating down to zero,
//                  because a hammer that barely moved makes no sound. A
//                  switch has no such state — closed is closed — so TRIGGERED
//                  clamps at velAtSlow.
//   the watchdog   maxDescentMs abandons a stalled descent and re-times it
//                  from where the watchdog fired, which makes STRUCK's
//                  velocity non-monotonic in how slowly you pressed. See
//                  Gesture.cpp for the measured numbers. TRIGGERED does not
//                  apply it.
//
// The press that turns a note OFF is measured the same way, and its reading
// becomes releaseVel — jab the key and the note stops short, press it slowly
// and it fades. That is the only expressive gesture a latch has left at the
// end of a note, since there is no key release to time.
//
// What velocity is then allowed to TOUCH is a routing decision and not this
// layer's business. A patch wanting a velocity-sensitive ONSET over a body at
// one organ-like level routes SRC_ATTACK via SRC_VELOCITY (see ModMatrix.h)
// and does NOT carry VEL_TO_AMP, which scales the whole note. That split is
// the reason this articulation is worth having: a hammer's strike owns the
// note, a switch's owns only its beginning.
//
// EXPRESSION UNDER A LATCH, and it is not what it is under the others.
// position and pressure describe A KEY, and a latched note's key is usually at
// rest with nothing on it, so they report what is true: nothing.
//
// The two signals then behave differently, because of where they are computed:
//
//   position  updated every sample in every phase, so it reads whenever a
//             finger is on the key AT ALL, including a touch far too light to
//             reach onPos. That makes it the aftertouch signal for a latched
//             patch: everything below the trigger is expression, and nothing
//             below the trigger can switch a note, so you can rest on a
//             sounding key and shape it with no risk of turning it off.
//   pressure  only updated while the key is past onPos — and on a note that is
//             already sounding, crossing onPos is exactly what turns it OFF.
//             So pressure on a latched note means the tail of the press that
//             STARTED it: trigger the note, keep holding, lean. Let go and it
//             is over.
//
// Which is to say a latching patch should reach for SRC_POSITION where a
// momentary one would reach for SRC_PRESSURE. Pressure's whole advantage —
// zeroing itself at the depth the note landed — assumes a key that stays down,
// and this is the one articulation where it does not.
//
// THREE EXPRESSION SIGNALS, ALWAYS COMPUTED
//   position  absolute, 0 at rest, 1 at this key's own bottom
//   pressure  relative to where THIS note settled after its strike, scaled by
//             the travel REMAINING below that point. Landing depth cancels,
//             and the gain rises as the room runs out, which is what the
//             finger is already feeling.
//   strike    latched flight-time velocity
//
// CALIBRATION DESCRIBES THE HARDWARE, NEVER THE PLAYER.
//   Pressure used to divide by a `width` learned per key from how far the
//   player habitually leaned. That crossed the line: baseline and bottom
//   model a sensor and a plastic key, and are properties of the instrument;
//   a lean is a property of whoever is standing at it. It also failed on its
//   own terms — the learned width was allowed down to the jitter floor, and
//   it was seeded between KEYS, which cannot mean anything for a habit.
//   Remaining travel is the same normalisation taken from geometry instead,
//   with nothing accumulated across notes. `restPos` stays because it
//   measures the note in progress; it is an observation, not a model of you.
//
// All three are cheap and all three are always available. Which of them a
// sound uses is a routing decision in the patch, not a mode here. That is
// deliberate: a mode flag would make this layer depend on which preset is
// loaded, and that upward dependency is what let a piano patch rescale a
// theremin key.
// ============================================================================
#pragma once
#include <stdint.h>

enum Articulation : uint8_t {
  ART_CONTINUOUS = 0,
  ART_STRUCK,
  ART_GATED,
  ART_PLUCKED,
  // Appended, never inserted: Preset rows and the serial console both name
  // articulations by value.
  ART_TRIGGERED
};

struct GestureConfig {
  // continuous: essentially the noise floor
  float contOn, contOff;
  // triggered articulations, in position units
  float velStart, onPos, offPos;
  float velFastMs, velSlowMs;     // flight-time endpoints of the velocity map
  float velAtFast, velAtSlow;     // 0..1
  // pressure (relative to where the note landed)
  float settleMs;                 // ignore this long after the strike
  float stillRate;                // position units per second = "at rest"
  float velTauMs;
  float keyVelFull;               // key speed, in position units per second,
                                  //   that maps to a full-scale SRC_KEYVEL.
                                  //   4.0 matches the divisor the release
                                  //   velocity already uses, so "as fast as a
                                  //   finger goes" means the same thing to
                                  //   both.
  float heldBand;                 // how far past the rest point still counts
                                  //   as a lean rather than a fresh descent
  float minRoom;                  // floor on the remaining-travel divisor, so
                                  //   a strike that bottoms the key on landing
                                  //   cannot divide by ~zero
  float smoothTauMs;              // on pressure and position outputs
  // guards
  float retreatHyst;
  float maxDescentMs;
  uint8_t onSamples;    // consecutive samples over the threshold before a
  uint8_t offSamples;   //   CONTINUOUS note starts, and under it before one
                        //   ends. Rows are shared, so a neighbour moving fast
                        //   puts a one- or two-sample spike on the line that
                        //   the bleed correction cannot model — it assumes a
                        //   static equilibrium. No finger produces a 4 ms
                        //   press, so requiring persistence costs nothing and
                        //   rejects every transient of that shape.
};

extern const GestureConfig GESTURE_DEFAULT;

// The phase tracks THE KEY, not the sound. It used to track both, because for
// every articulation but one they are the same thing — GP_SOUNDING was named
// for the sound and tested for the key. A latch separates them: a note can
// sound with its key at GP_IDLE, and a key can be at GP_KEYDOWN in the middle
// of the press that switches a note OFF. Ask `sounding` about the sound.
enum GesturePhase : uint8_t { GP_IDLE = 0, GP_DESCENDING, GP_KEYDOWN };

struct GestureState {
  uint8_t phase;
  bool    sounding;           // a note is on. For every articulation except
                              //   TRIGGERED this is exactly phase==GP_KEYDOWN;
                              //   for TRIGGERED it outlives the key press that
                              //   started it and ends at the next one.
  uint8_t noteArt;            // articulation LATCHED at note-on. Switching
                              //   preset mid-note used to change the note-off
                              //   threshold underneath a sounding note —
                              //   CONTINUOUS releases at 1.2% of travel,
                              //   STRUCK at 18% — so a held key was either cut
                              //   off or stranded depending on the direction.
  float   noteOnMs;
  float   prevPos, prevMs;
  bool    havePrev;
  float   velStartMs;
  float   vel;                // smoothed d(position)/dt, per second
  uint8_t overCount, underCount;

  // --- outputs ---
  float   position;           // smoothed absolute position
  float   pressure;           // smoothed relative pressure
  float   strike;             // latched 0..1, valid from note-on

  // --- pressure learning ---
  float   restPos;
  bool    haveRest;
  float   noteMaxPos;
};

struct GestureEvent {
  bool  noteOn;
  bool  noteOff;
  float velocity;      // 0..1 at note-on (0.5 where the articulation ignores it)
  float releaseVel;    // 0..1 at note-off
  // The KEY finished a press and returned to rest: calibration should fold in
  // the bottom it just measured. This used to be a synonym for noteOff and is
  // no longer one — under a latch a key completes a press without ending
  // anything, and ends a note without the key having gone anywhere yet. What
  // calibration cares about is the key, so this follows the key.
  bool  noteEnded;
};

void gestureInit(GestureState &g);

// Feed one position sample. Returns any events that occurred.
GestureEvent gestureUpdate(GestureState &g, const GestureConfig &c,
                           uint8_t articulation, float position, float nowMs);

// Is the KEY at rest? This is what calibration asks, because learning a key's
// bottom is about the key. A latched note whose key is up reads idle, which is
// correct: nothing is touching it, so its baseline should be tracking drift.
bool  gestureIdle(const GestureState &g);

// Is a NOTE on? What anything about the sound should ask — silencing every
// voice, showing what is playing in the trace. Under a latch this is the only
// way to find a note whose key is back at rest.
bool  gestureSounding(const GestureState &g);

// How fast the key is moving RIGHT NOW, normalised bipolar: +1 is descending
// at keyVelFull, -1 is rising that fast, 0 is stationary. Already smoothed
// (velTauMs) because it is the same estimate the layer uses internally to
// decide whether a key has come to rest.
//
// This was computed and thrown away for the whole life of the project — it
// drove the stillness test and the release velocity and was never offered to
// a patch. A bow needs it: energy in, not depth. So does anything where
// stopping should change the sound rather than just holding it.
float gestureKeyVel(const GestureState &g, const GestureConfig &c);
