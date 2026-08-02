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
void synthNoteOff(int row, int col);
void synthUpdate();                                 // call often: modulation
bool synthVoiceIdle(int row, int col);

void  presetSelect(int index);
void  presetList();
void  presetAnnounce();
int   presetCurrent();

void synthReportLoad();                             // CPU and block usage
