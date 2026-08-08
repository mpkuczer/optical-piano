// ============================================================================
// Synth.h — the sound-making side
// ============================================================================
// A VOICE is the machinery that produces one note: two oscillators, a filter
// and an envelope. One voice makes one note at a time. You have 25 keys and
// 25 voices permanently paired, so voice i always belongs to key i — there is
// no allocation and no note stealing.
#pragma once
#include <Arduino.h>
#include "Gesture.h"   // for presetGesture()'s return type

// Longest delay the bus effect can produce. Delay lines consume audio memory
// blocks (one block = 2.9 ms), so raising this means raising AudioMemory too.
const float DELAY_MAX_MS = 350.0f;

void synthBegin();                                  // build graph, start codec
void synthNoteOn(int row, int col, float velocity);   // velocity 0..1

// releaseVel 0..1: how fast the key came back up. Snatching your hand away
// cuts the note short, lifting gently lets it hang. Defaulted so callers that
// don't measure it (the preset audition) still compile.
void synthNoteOff(int row, int col, float releaseVel = 0.5f);

// Both expression signals, pushed every sweep. `position` is absolute (0 at
// rest, 1 at this key's bottom); `pressure` is relative to where this note
// landed. Both feed the modulation matrix; the patch chooses which to use.
// `keyvel` is -1..1, positive descending. Defaulted so the preset audition and
// any other caller that has no key to measure still compiles and reads as
// stationary, which is the honest value for a note nobody is touching.
void synthExpression(int row, int col, float position, float pressure,
                     float keyvel = 0.0f);

void synthUpdate();                                 // call often: modulation
bool synthVoiceIdle(int row, int col);

void  presetSelect(int index);
void  presetList();
void  presetDumpRoutes();   // what the flashed firmware actually contains
void  presetAnnounce();
int   presetCurrent();
uint8_t presetArticulation();   // see Gesture.h

// Detection tuning for the active patch: its own GestureConfig if it declares
// one, GESTURE_DEFAULT otherwise. Scanner feeds this to gestureUpdate, so a
// threshold is now a property of the patch rather than of the instrument.
const GestureConfig &presetGesture();

// The mode cue: a run up or down the whole keyboard, in a fixed built-in voice
// rather than the selected preset. Non-blocking — it steps forward inside
// synthUpdate, so the scanner keeps running underneath it, which matters
// because the cue fires while the chord that triggered it is still held.
//
// It is deliberately NOT the selected patch. Under `shimmer` or `strings` a
// 450 ms run would arrive as a slow wash well after the mode had changed, and
// the one moment this instrument has to be unambiguous is the moment it stops
// being an instrument. See CUE_PRESET in Synth.cpp.
void synthGlissando(bool up);
bool synthCueActive();

void synthPressureTest();   // sweep pressure 0->1->0 with no scanner
void synthReportLoad();                             // CPU and block usage
