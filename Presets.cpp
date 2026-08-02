// ============================================================================
// Presets.cpp — the patch library
// ============================================================================
// Each preset owns a route table: which modulation source drives which
// destination, and how much. Depth is in the destination's own units —
// cents for pitch and detune, semitones for cutoff, linear for amplitude.
//
// Field order matches struct Preset:
//   name, waveA, waveB, semiB, cents, mixB,
//   a, tau, r, ampScale,
//   fPeak, fBase, fTau, res,
//   lpMix, bpMix, hpMix,
//   lfo1{rateHz, shape, delayMs, fadeMs}, lfo2{...}, routes, nRoutes,
//   drive, verb, delaySend, delayMs, delayFb
#include <Audio.h>
#include "Preset.h"

#define NROUTES(x) (x), (uint8_t)(sizeof(x) / sizeof(ModRoute))
static const LfoConfig LFO_OFF = { 0.0f, LFO_SINE, 0.0f, 0.0f };

// ---- route tables --------------------------------------------------------

// Flute: vibrato that fades in, and brightness that follows loudness — as a
// note dies away it also darkens, which is what real instruments do.
static const ModRoute R_FLUTE[] = {
  // Vibrato that BLOOMS under pressure: the LFO is routed through pressure,
  // so a held key is steady until you lean on it. This is what a flautist
  // does with breath and it is the reason `via` exists.
  { SRC_LFO1,     DST_PITCH,   26.0f, SRC_PRESSURE },
  { SRC_LFO1,     DST_PITCH,    6.0f },   // a little always present
  { SRC_PRESSURE, DST_CUTOFF,   7.0f },   // lean in and it opens up
  { SRC_VELOCITY, DST_CUTOFF,   5.0f },
};

// Theremin: the player's other hand controls VOLUME, continuously. Pressure
// is that hand. SRC_CONST pulls the floor down so pressure has room to push
// back up: 0.45 gain at rest, 1.0 fully depressed.
static const ModRoute R_THEREMIN[] = {
  { SRC_LFO1,     DST_PITCH,  45.0f },
  { SRC_CONST,    DST_AMP,    -0.55f },
  { SRC_PRESSURE, DST_AMP,     0.55f },
  { SRC_PRESSURE, DST_CUTOFF,  4.0f  },
};

// Airy: vibrato plus a slow wobble in the second oscillator's level, so the
// breathiness itself drifts.
static const ModRoute R_AIRY[] = {
  { SRC_LFO1, DST_PITCH, 26.0f },
  { SRC_LFO2, DST_OSCB,   0.10f },
};

// Circus 2: the LFO->pitch routing Surge had, plus velocity brightness.
static const ModRoute R_CIRCUS[] = {
  { SRC_LFO1,     DST_PITCH,  30.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

// Analog drift: every note detunes slightly differently and sits at a
// slightly different brightness. This is what "warm" mostly means.
static const ModRoute R_DRIFT[] = {
  { SRC_RANDOM,   DST_DETUNE,  6.0f },
  { SRC_RANDOM,   DST_CUTOFF,  2.0f },
  { SRC_VELOCITY, DST_CUTOFF, 10.0f },
  { SRC_KEYTRACK, DST_CUTOFF,  4.0f },
};

// Oberheim-ish: wide velocity brightness, slow vibrato well after onset.
static const ModRoute R_OBIE[] = {
  { SRC_VELOCITY, DST_CUTOFF, 16.0f },
  { SRC_LFO1,     DST_PITCH,  12.0f, SRC_PRESSURE },
  { SRC_RANDOM,   DST_DETUNE,  4.0f },
  { SRC_PRESSURE, DST_CUTOFF, 12.0f },
};

// Percussive: hard hits much brighter, brightness tracking the envelope.
static const ModRoute R_PLUCK[] = {
  { SRC_VELOCITY, DST_CUTOFF, 20.0f },
  { SRC_AMPENV,   DST_CUTOFF, 10.0f },
};

// Pulse-width motion, the Oberheim shimmer.
static const ModRoute R_SHIMMER[] = {
  { SRC_LFO2,     DST_PWM,     0.30f },
  { SRC_LFO2,     DST_PWM,     0.18f, SRC_PRESSURE },
  { SRC_PRESSURE, DST_OSCB,    0.25f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f  },
};

// Tremolo: a wobble in VOLUME rather than pitch. We never had this before.
static const ModRoute R_TREMOLO[] = {
  { SRC_LFO2,     DST_AMP,    0.45f },
  { SRC_VELOCITY, DST_CUTOFF, 8.0f  },
};

// Sample-and-hold to cutoff: the stepped, burbling, sci-fi sound.
static const ModRoute R_STEPPED[] = {
  { SRC_LFO2,     DST_CUTOFF, 14.0f },
  { SRC_VELOCITY, DST_CUTOFF,  6.0f },
};

static const ModRoute R_NASAL[] = {
  { SRC_VELOCITY, DST_CUTOFF, 8.0f },
};

// ---- the table -----------------------------------------------------------
const Preset PRESET[] = {

// ============================ FLUTE FAMILY =================================
{ "flute",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.4f, LFO_SINE, 260.0f, 300.0f }, LFO_OFF, NROUTES(R_FLUTE),
    0.00, 0.40, 0.00,   0,   0.00 },

{ "theremin",WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.25,
   140,  1e9f,  320, 0.90,   10.0, 10.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.8f, LFO_SINE, 0.0f, 400.0f }, LFO_OFF, NROUTES(R_THEREMIN),
    0.00, 0.45, 0.00,   0,   0.00 },

{ "airy",    WAVEFORM_SINE,     WAVEFORM_TRIANGLE,  12,  6.0, 0.18,
   110,  1e9f,  300, 0.85,    8.0,  8.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.2f, LFO_SINE, 300.0f, 400.0f },
    { 0.28f, LFO_TRIANGLE, 0.0f, 800.0f }, NROUTES(R_AIRY),
    0.00, 0.45, 0.20, 260,   0.30 },

// ============================ CIRCUS 2 =====================================
{ "circus",  WAVEFORM_SAWTOOTH, WAVEFORM_SQUARE,     0,  8.8, 1.00,
     4,  1e9f,   31, 0.50,    2.0,  2.0,  500, 1.60,
    0.00, 0.00, 1.00,
    { 5.84f, LFO_SINE, 120.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
    0.06, 0.20, 0.58, 250,   0.71 },

{ "circusb", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  6.0, 0.35,
     8,  1e9f,   60, 0.85,    9.0,  9.0,  400, 0.70,
    1.00, 0.00, 0.00,
    { 5.84f, LFO_SINE, 90.0f, 250.0f }, LFO_OFF, NROUTES(R_CIRCUS),
    0.00, 0.30, 0.45, 250,   0.68 },

// ============================ ANALOG-ISH ===================================
{ "drift",   WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  7.0, 1.00,
     4,   900,  320, 0.50,    9.0,  5.0,  500, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRIFT),
    0.00, 0.25, 0.00,   0,   0.00 },

{ "wide",    WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0, 16.0, 1.00,
     4,  1100,  400, 0.50,    8.0,  5.0,  600, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_DRIFT),
    0.00, 0.32, 0.00,   0,   0.00 },

{ "obie",    WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
    16,  1e9f,  450, 0.42,   15.0,  4.0,  650, 1.50,
    1.00, 0.00, 0.00,
    { 5.0f, LFO_SINE, 400.0f, 500.0f }, LFO_OFF, NROUTES(R_OBIE),
    0.15, 0.36, 0.00,   0,   0.00 },

{ "fatpluck",WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0, 13.0, 1.00,
     2,   380,  140, 0.42,   24.0,  3.0,  180, 1.60,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_PLUCK),
    0.10, 0.20, 0.00,   0,   0.00 },

// ============================ NEW MOTION ===================================
// Tremolo — volume wobble. Only possible now the matrix exists.
{ "tremolo", WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  6.0, 1.00,
    20,  1e9f,  300, 0.55,    8.0,  8.0,  400, 0.80,
    1.00, 0.00, 0.00,
    LFO_OFF, { 5.5f, LFO_SINE, 150.0f, 400.0f }, NROUTES(R_TREMOLO),
    0.00, 0.30, 0.00,   0,   0.00 },

// Sample-and-hold stepping the filter. Burbling and mechanical.
{ "stepped", WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  8.0, 1.00,
     4,  1e9f,  200, 0.45,    6.0,  6.0,  400, 1.80,
    1.00, 0.00, 0.00,
    LFO_OFF, { 8.0f, LFO_SAMPLE_HOLD, 0.0f, 0.0f }, NROUTES(R_STEPPED),
    0.05, 0.30, 0.35, 200,   0.40 },

{ "shimmer", WAVEFORM_PULSE,    WAVEFORM_PULSE,      0,  5.0, 1.00,
    25,  1e9f,  600, 0.42,    9.0,  7.0,  800, 1.10,
    1.00, 0.00, 0.00,
    LFO_OFF, { 0.35f, LFO_SINE, 0.0f, 600.0f }, NROUTES(R_SHIMMER),
    0.00, 0.45, 0.00,   0,   0.00 },

{ "nasal",   WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
     6,   800,  280, 0.60,    6.0,  3.0,  350, 2.60,
    0.10, 0.90, 0.00,
    LFO_OFF, LFO_OFF, NROUTES(R_NASAL),
    0.20, 0.28, 0.20, 220,   0.25 },

// ============================ REFERENCE ====================================
// Single oscillator, no detune, static filter, no modulation, no effects.
{ "blocky",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  0.0, 0.00,
     3,   700,  250, 1.00,   60.0, 60.0,  400, 0.70,
    1.00, 0.00, 0.00,
    LFO_OFF, LFO_OFF, nullptr, 0,
    0.00, 0.00, 0.00,   0,   0.00 },
};

const int PRESET_COUNT = sizeof(PRESET) / sizeof(PRESET[0]);
