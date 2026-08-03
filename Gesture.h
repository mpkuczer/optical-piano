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
//   GATED       organ. Schmitt trigger, no velocity, no continuing
//               expression. Audible start, nothing else.
//   PLUCKED     harpsichord. Trigger on crossing but DISCARD velocity — a
//               plectrum plucks identically however hard you hit it — then
//               keep reading position, so the finger can still add vibrato.
//
// THREE EXPRESSION SIGNALS, ALWAYS COMPUTED
//   position  absolute, 0 at rest, 1 at this key's own bottom
//   pressure  relative to where THIS note settled after its strike
//   strike    latched flight-time velocity
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
  ART_PLUCKED
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
  float heldBand;                 // cap on the learned lean width
  float minWidth;
  float widthLearn;
  float smoothTauMs;              // on pressure and position outputs
  // guards
  float retreatHyst;
  float maxDescentMs;
};

extern const GestureConfig GESTURE_DEFAULT;

enum GesturePhase : uint8_t { GP_IDLE = 0, GP_DESCENDING, GP_SOUNDING };

struct GestureState {
  uint8_t phase;
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

  // --- outputs ---
  float   position;           // smoothed absolute position
  float   pressure;           // smoothed relative pressure
  float   strike;             // latched 0..1, valid from note-on

  // --- pressure learning ---
  float   restPos;
  bool    haveRest;
  float   noteMaxPos;
  float   width;
  bool    widthLearned, widthBorrowed;
};

struct GestureEvent {
  bool  noteOn;
  bool  noteOff;
  float velocity;      // 0..1 at note-on (0.5 where the articulation ignores it)
  float releaseVel;    // 0..1 at note-off
  bool  noteEnded;     // note-off happened: calibration should fold in its bottom
};

void gestureInit(GestureState &g);

// Feed one position sample. Returns any events that occurred.
GestureEvent gestureUpdate(GestureState &g, const GestureConfig &c,
                           uint8_t articulation, float position, float nowMs);

bool  gestureIdle(const GestureState &g);
void  gestureSeedWidth(GestureState &g, const GestureConfig &c, float width);
float gestureWidth(const GestureState &g);
