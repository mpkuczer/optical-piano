// ============================================================================
// Synth.h — the sound-making side
// ============================================================================
// A VOICE is the machinery that produces one note: two oscillators, a filter
// and an envelope. One voice makes one note at a time. You have 25 keys and
// 25 voices permanently paired, so voice i always belongs to key i — there is
// no allocation and no note stealing.
#pragma once
#include <Arduino.h>

// Longest delay the bus effect can produce. Delay lines consume audio memory
// blocks (one block = 2.9 ms), so raising this means raising AudioMemory too.
const float DELAY_MAX_MS = 350.0f;

void synthBegin();                                  // build graph, start codec
void synthNoteOn(int row, int col, int velocity);

// releaseVel 0..1: how fast the key came back up. Snatching your hand away
// cuts the note short, lifting gently lets it hang. Defaulted so callers that
// don't measure it (the preset audition) still compile.
void synthNoteOff(int row, int col, float releaseVel = 0.5f);

// Continuous key depth, 0..1, called every scan sweep while a key is held.
// Feeds SRC_PRESSURE in the modulation matrix. This is polyphonic aftertouch
// and it costs nothing extra — the scanner already has the number.
void synthPressure(int row, int col, float pressure);

void synthUpdate();                                 // call often: modulation
bool synthVoiceIdle(int row, int col);

void  presetSelect(int index);
void  presetList();
void  presetDumpRoutes();   // what the flashed firmware actually contains
void  presetAnnounce();
int   presetCurrent();

void synthPressureTest();   // sweep pressure 0->1->0 with no scanner
void synthReportLoad();                             // CPU and block usage
