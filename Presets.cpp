// ============================================================================
// Presets.cpp — the patch library
// ============================================================================
// Field order matches struct Preset in Preset.h:
//   name, waveA, waveB, semiB, cents, mixB,
//   a, tau, r, ampScale,
//   fPeak, fBase, fTau, res, fVel,
//   lpMix, bpMix, hpMix,
//   pwmD, pwmHz, vibCents, vibHz, vibDelayMs,
//   drive, verb, delaySend, delayMs, delayFb
#include <Audio.h>
#include "Preset.h"

const Preset PRESET[] = {

// ============================ FLUTE FAMILY =================================
// Few harmonics, soft attack, sustains while held, gentle vibrato that fades
// in. The opposite of everything that sounds metallic: sine and triangle
// rather than saw, lowpass rather than highpass, resonance left neutral.

{ "flute",   WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  4.0, 0.30,
    90,  1e9f,  240, 0.85,    6.0,  6.0,  400, 0.70, 0.25,
    1.00, 0.00, 0.00,   0.00, 0.00,   22.0, 5.4, 260.0,
    0.00, 0.40, 0.00,   0,   0.00 },

// A theremin is essentially one pure sine with vibrato and nothing else.
{ "theremin",WAVEFORM_SINE,     WAVEFORM_SINE,       0,  0.0, 0.25,
   140,  1e9f,  320, 0.90,   10.0, 10.0,  400, 0.70, 0.15,
    1.00, 0.00, 0.00,   0.00, 0.00,   45.0, 5.8,   0.0,
    0.00, 0.45, 0.00,   0,   0.00 },

// Breathier: a touch of the second oscillator an octave up adds air without
// adding edge.
{ "airy",    WAVEFORM_SINE,     WAVEFORM_TRIANGLE,  12,  6.0, 0.18,
   110,  1e9f,  300, 0.85,    8.0,  8.0,  400, 0.70, 0.20,
    1.00, 0.00, 0.00,   0.00, 0.00,   26.0, 5.2, 300.0,
    0.00, 0.45, 0.20, 260,   0.30 },

// ============================ CIRCUS 2 =====================================
// Converted from Surge XT. The amplitude envelope SUSTAINS (tau 1e9) — the
// earlier decaying versions came from reading Surge's filter envelope as if
// it were the amplitude one. Vibrato at 5.84 Hz is the LFO->pitch routing
// that the converter previously dropped without saying so.

{ "circus",  WAVEFORM_SAWTOOTH, WAVEFORM_SQUARE,     0,  8.8, 1.00,
     4,  1e9f,   31, 0.50,    2.0,  2.0,  500, 1.60, 0.30,
    0.00, 0.00, 1.00,   0.00, 0.00,   30.0, 5.84, 120.0,
    0.06, 0.20, 0.58, 250,   0.71 },

// Same, but sine-based and lowpass — much cleaner and more transparent. If
// "circus" is still too metallic, the culprit is the sawtooth plus highpass,
// and this is what removing both sounds like.
{ "circusb", WAVEFORM_SINE,     WAVEFORM_TRIANGLE,   0,  6.0, 0.35,
     8,  1e9f,   60, 0.85,    9.0,  9.0,  400, 0.70, 0.20,
    1.00, 0.00, 0.00,   0.00, 0.00,   30.0, 5.84,  90.0,
    0.00, 0.30, 0.45, 250,   0.68 },

// ============================ LOWPASS ======================================
{ "creamy",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  7.0, 1.00,
     4,   900,  320, 0.50,    9.0,  5.0,  500, 0.80, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.00, 0.25, 0.00,   0,   0.00 },

{ "wide",    WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0, 16.0, 1.00,
     4,  1100,  400, 0.50,    8.0,  5.0,  600, 0.80, 0.40,
    1.00, 0.00, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.00, 0.32, 0.00,   0,   0.00 },

{ "obie",    WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
    16,  1e9f,  450, 0.42,   15.0,  4.0,  650, 1.50, 0.70,
    1.00, 0.00, 0.00,   0.00, 0.00,   12.0, 5.0, 400.0,
    0.15, 0.36, 0.00,   0,   0.00 },

{ "fatpluck",WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0, 13.0, 1.00,
     2,   380,  140, 0.42,   24.0,  3.0,  180, 1.60, 0.90,
    1.00, 0.00, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.10, 0.20, 0.00,   0,   0.00 },

// ============================ BAND / HIGH PASS =============================
{ "nasal",   WAVEFORM_SAWTOOTH, WAVEFORM_SAWTOOTH,   0,  9.0, 1.00,
     6,   800,  280, 0.60,    6.0,  3.0,  350, 2.60, 0.60,
    0.10, 0.90, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.20, 0.28, 0.20, 220,   0.25 },

// ============================ MOTION =======================================
{ "shimmer", WAVEFORM_PULSE,    WAVEFORM_PULSE,      0,  5.0, 1.00,
    25,  1e9f,  600, 0.42,    9.0,  7.0,  800, 1.10, 0.35,
    1.00, 0.00, 0.00,   0.38, 0.35,    0.0, 0.0,   0.0,
    0.00, 0.45, 0.00,   0,   0.00 },

{ "bounce",  WAVEFORM_SQUARE,   WAVEFORM_SQUARE,     0,  6.0, 0.85,
     2,   200,  110, 0.50,   14.0,  4.0,  200, 1.40, 0.70,
    0.85, 0.15, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.10, 0.18, 0.60, 150,   0.62 },

// ============================ REFERENCE ====================================
// Single oscillator, no detune, static wide-open filter, no effects at all.
{ "blocky",  WAVEFORM_TRIANGLE, WAVEFORM_TRIANGLE,   0,  0.0, 0.00,
     3,   700,  250, 1.00,   60.0, 60.0,  400, 0.70, 0.00,
    1.00, 0.00, 0.00,   0.00, 0.00,    0.0, 0.0,   0.0,
    0.00, 0.00, 0.00,   0,   0.00 },
    
// ---- 80s Futures Pasts   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 31 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "80sfuture", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.22, 0.44, 250,  0.00 },

// ---- Afterglow   [Surge filter: ladder] ------------------
//   ~ osc1 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type ladder has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ FM between oscillators is used; dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ 15 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "afterglow", WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,     0,   2.0, 0.89,
       15,   1e9f,  1050, 0.50,     3.8,  3.8,  250, 0.86, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.17, 0.28, 0.56, 250,  0.00 },

// ---- Arpsychord   [Surge filter: comb] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ FM between oscillators is used; dropped
//   ~ 13 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "arpsychor", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,  30.0, 0.00,
       10,   1e9f,  2000, 0.50,    13.4,  3.2,  507, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.47, 0.00,   0,  0.00 },

// ---- Bell Keys 1 (1-3-5)   [Surge filter: comb] ------------------
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ 38 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bellkeys1", WAVEFORM_SINE,      WAVEFORM_SINE,         2,  30.0, 0.00,
       10,   1e9f,  2000, 0.50,    55.4, 13.4,  750, 1.35, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.40, 0.00,   0,  0.00 },

// ---- Bell Keys 2 (1-5-6)   [Surge filter: comb] ------------------
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ 40 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bellkeys2", WAVEFORM_SINE,      WAVEFORM_SINE,         2,  30.0, 0.00,
       10,   1e9f,  2000, 0.50,    32.9,  8.0,  750, 1.08, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.35, 0.00,   0,  0.00 },

// ---- Bell Keys 3 (1-3-5)   [Surge filter: comb] ------------------
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ 41 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bellkeys3", WAVEFORM_SINE,      WAVEFORM_SINE,         2,  30.0, 0.00,
       10,    750,  2000, 0.50,    18.9,  4.7, 2000, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.40, 0.70, 250,  0.00 },

// ---- Bell Song   [Surge filter: comb] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ ring modulation is used; dropped
//   ~ 14 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bellsong",  WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        24,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    46.5,  3.8, 1228, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.38, 0.70, 250,  0.00 },

// ---- Bore Excitations   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 26 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "boreexcit", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,   0.0, 0.00,
       10,   1e9f,  1000, 0.50,     2.0,  2.0,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.30, 0.59, 250,  0.00 },

// ---- Brass Nostalgia   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 51 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "brassnost", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,   0.0, 0.00,
       24,   1e9f,  2000, 0.50,    30.6,  7.6, 1031, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.32, 0.63, 250,  0.00 },

// ---- Corporealities   [Surge filter: comb] ------------------
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ 23 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "corporeal", WAVEFORM_SINE,      WAVEFORM_SAWTOOTH,     0,   2.0, 0.00,
        9,   1e9f,  1765, 0.50,     5.4,  5.4,  125, 1.06, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.37, 0.70, 250,  0.00 },

// ---- EPad   [Surge filter: LP24] ------------------
//   ~ osc1 type 5 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ LP24 is 4-pole; ours is 2-pole, so it rolls off half as steeply and will sound less closed
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ FM between oscillators is used; dropped
//   ~ ring modulation is used; dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 2.0 Hz (fade-in time is a guess)
//   ~ 12 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "epad",      WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,     0,   0.0, 1.00,
       25,   1e9f,  2000, 0.50,     3.8,  2.3,  422, 0.84, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 2.00, 120.0,
    0.07, 0.36, 0.70, 250,  0.09 },

// ---- EPleasantries   [Surge filter: off] ------------------
//   ~ osc1 is a saw/pulse blend (shape +0.00); approximated as square
//   ~ osc2 is a saw/pulse blend (shape +0.00); approximated as square
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 27 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "epleasant", WAVEFORM_SQUARE,    WAVEFORM_SQUARE,       0,   2.0, 1.00,
      364,   1e9f,  2000, 0.50,     2.0,  2.0,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.35, 0.70, 250,  0.00 },

// ---- Fiftyish   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 51 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "fiftyish",  WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6, 1031, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.63, 250,  0.00 },

// ---- FM3 Deiified   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 49 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "fm3deiifi", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        36,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6, 1031, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.63, 250,  0.00 },

// ---- FM EPiano 1, 3, 5   [Surge filter: comb] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ vibrato: LFO->pitch, ~16 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 34 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "fmepiano1", WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,    12,   0.0, 0.00,
        4,   1e9f,   750, 0.50,    15.3, 15.3, 1000, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  15.8, 4.00, 120.0,
    0.00, 0.25, 0.50, 250,  0.00 },

// ---- FM Swirlscapes   [Surge filter: LP12] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 30 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "fmswirlsc", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,     9.9,  9.9,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.36, 0.70, 250,  0.00 },

// ---- Harmonic Dimensions   [Surge filter: comb] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 17 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "harmonicd", WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,   -12,   0.0, 1.00,
       10,   1021,  1500, 0.50,     6.6,  6.6,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.32, 0.63, 250,  0.00 },

// ---- Harmonicscape   [Surge filter: comb] ------------------
//   ~ osc1 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 9 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "harmonics", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,   2.0, 0.00,
       10,   1e9f,  2000, 0.50,     4.2,  4.2,  610, 0.89, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.08, 0.29, 0.57, 250,  0.25 },

// ---- Iso EP   [Surge filter: ?] ------------------
//   ~ osc1 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ FM between oscillators is used; dropped
//   ~ 13 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "isoep",     WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,    12,   2.0, 0.82,
       10,   2000,  1000, 0.50,    11.1, 11.1,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.29, 0.58, 250,  0.00 },

// ---- Iso-Keys   [Surge filter: comb] ------------------
//   ~ osc1 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 16 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "isokeys",   WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,     0,   8.2, 0.00,
        5,   2667,  2000, 0.50,     7.5,  7.5,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.34, 0.00,   0,  0.00 },

// ---- Lighter Still   [Surge filter: comb] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ FM between oscillators is used; dropped
//   ~ 13 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "lighterst", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,  30.0, 0.00,
        5,   1e9f,  1181, 0.50,     9.4,  5.1, 1649, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.50, 0.70, 250,  0.00 },

// ---- Midnightly   [Surge filter: off] ------------------
//   ~ osc1 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ osc2 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 7 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "midnightl", WAVEFORM_SAWTOOTH,  WAVEFORM_SAWTOOTH,     0,   2.0, 0.00,
       10,   1e9f,  1134, 0.50,     2.0,  2.0,  378, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.33, 0.67, 250,  0.00 },

// ---- Midnight Retrospections   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 25 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "midnightr", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.22, 0.44, 250,  0.00 },

// ---- Min-Triad Me   [Surge filter: LP24] ------------------
//   ~ osc1 is a saw/pulse blend (shape +0.13); approximated as square
//   ~ LP24 is 4-pole; ours is 2-pole, so it rolls off half as steeply and will sound less closed
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ ring modulation is used; dropped
//   ~ noise oscillator is used; we have none, dropped
//   ~ 6 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "mintriadm", WAVEFORM_SQUARE,    WAVEFORM_SINE,         0,   2.0, 1.00,
        7,   1e9f,  2000, 0.50,     4.7,  3.8,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.46, 0.25, 0.50, 250,  0.00 },

// ---- Open Windows   [Surge filter: comb] ------------------
//   ~ osc1 type 7 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ ring modulation is used; dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ 13 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "openwindo", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,   2.0, 0.72,
       21,   1e9f,  1958, 0.50,     9.7,  9.7,  418, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.33, 0.66, 250,  0.00 },

// ---- Our Stories   [Surge filter: comb] ------------------
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 1.0 Hz (fade-in time is a guess)
//   ~ 22 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "ourstorie", WAVEFORM_SINE,      WAVEFORM_SINE,        12,   2.0, 0.86,
       10,   1e9f,  2000, 0.50,    10.0, 10.0,  500, 1.21, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 1.00, 120.0,
    0.00, 0.42, 0.70, 250,  0.33 },

// ---- Palm Mute Pi   [Surge filter: off] ------------------
//   ~ osc1 is a saw/pulse blend (shape +0.00); approximated as square
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 7 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "palmmutep", WAVEFORM_SQUARE,    WAVEFORM_SINE,        12,  12.4, 0.00,
        4,   1e9f,   100, 0.50,     2.0,  2.0,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.20, 0.00,   0,  0.00 },

// ---- Pluckstring   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 22 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "pluckstri", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 0.00,
       10,   2000,  2000, 0.50,    30.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.63, 250,  0.00 },

// ---- Rediscoveries (1-3-6)   [Surge filter: off] ------------------
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 28 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "rediscove", WAVEFORM_SINE,      WAVEFORM_SINE,         2,  30.0, 0.00,
        5,   1e9f,  2000, 0.50,     2.0,  2.0, 1250, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.31, 0.63, 250,  0.00 },

// ---- Rediscoveries (Variant)   [Surge filter: ?] ------------------
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ ring modulation is used; dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 29 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "rediscove", WAVEFORM_SINE,      WAVEFORM_SINE,        24,   2.0, 1.00,
       10,   1e9f,  2000, 0.50,     8.8,  8.8,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.40, 0.70, 250,  0.00 },

// ---- Saw Things   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ FM between oscillators is used; dropped
//   ~ 22 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "sawthings", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.24, 0.48, 250,  0.00 },

// ---- Slow Waves   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 19 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "slowwaves", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,    30.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.25, 0.51, 250,  0.00 },

// ---- Sunny   [Surge filter: comb] ------------------
//   ~ osc1 is a saw/pulse blend (shape +0.00); approximated as square
//   ~ osc2 is a saw/pulse blend (shape +0.00); approximated as square
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 23 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "sunny",     WAVEFORM_SQUARE,    WAVEFORM_SQUARE,       0,   2.0, 1.00,
       10,   1e9f,  1533, 0.50,     9.6,  9.6,  750, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.28, 0.56, 250,  0.00 },

// ---- Through Open Windows   [Surge filter: off] ------------------
//   ~ osc1 type 6 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 22 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "throughop", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,        12,   0.0, 0.00,
       10,   1e9f,  2000, 0.50,     7.6,  7.6,  750, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.22, 0.44, 250,  0.00 },

// ---- X-Fade Keys   [Surge filter: comb] ------------------
//   ~ osc2 type 2 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ ring modulation is used; dropped
//   ~ third oscillator is used; we have two, dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 31 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "xfadekeys", WAVEFORM_SINE,      WAVEFORM_SAWTOOTH,    12,   2.0, 1.00,
       10,    750,  2000, 0.50,    11.1,  2.8,  500, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.37, 0.70, 250,  0.00 },

// ---- Y87 (1-3-9)   [Surge filter: comb] ------------------
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ waveshaper is per-voice in Surge but shared on the bus here, so chords will saturate more than single notes
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ oscillator SYNC is used; no equivalent, dropped
//   ~ FM between oscillators is used; dropped
//   ~ ring modulation is used; dropped
//   ~ vibrato: LFO->pitch, ~80 cents at 4.0 Hz (fade-in time is a guess)
//   ~ 31 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "y87139",    WAVEFORM_SINE,      WAVEFORM_SINE,         2,  30.0, 0.00,
       10,   1e9f,  2000, 0.50,     4.5,  4.5, 1250, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,  80.0, 4.00, 120.0,
    0.00, 0.31, 0.63, 250,  0.00 },
    
    // ---- Bass Marimba 1   [Surge filter: comb] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ 11 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bassmarim", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -36,   0.0, 0.00,
       10,   1e9f,  2500, 0.50,    22.9, 22.9,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.00,   0,  0.00 },

// ---- Bass Marimba 2   [Surge filter: comb] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ 11 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "bassmarim", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -36,   0.0, 0.00,
       10,   1e9f,  2500, 0.50,    22.9, 22.9,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.00,   0,  0.00 },

// ---- Chimes 1   [Surge filter: comb] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 8 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "chimes1",   WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 0.00,
        5,   1e9f,  2500, 0.50,     9.6,  9.6,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.35, 0.70, 250,  0.00 },

// ---- Cymbell 1   [Surge filter: ?] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 10 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "cymbell1",  WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -24,   0.0, 0.00,
        5,   1e9f,  2500, 0.50,     1.0,  1.0, 1000, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.00,   0,  0.00 },

// ---- Cymbell 2   [Surge filter: ?] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 10 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "cymbell2",  WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -24,   0.0, 0.00,
        5,   1e9f,  2500, 0.50,     1.0,  1.0, 1000, 1.10, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.00,   0,  0.00 },

// ---- Golden Mallets   [Surge filter: off] ------------------
//   ~ osc1 type 5 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
//   ~ 20 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "goldenmal", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,         0,  30.0, 1.00,
       10,   1000,  2000, 0.50,     8.9,  8.9,  500, 0.95, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.26, 0.53, 250,  0.25 },

// ---- Lithovibes   [Surge filter: ?] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 13 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "lithovibe", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -24,   0.0, 0.00,
        4,   1e9f,  2500, 0.50,     5.7,  5.7,  624, 0.90, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.40, 0.00,   0,  0.00 },

// ---- Square Metallophone    [Surge filter: comb] ------------------
//   ~ osc1 type 9 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ filter type comb has no equivalent; using lowpass
//   ~ patch uses a SECOND filter in series; we have one
//   ~ 6 modulation routing(s) (LFO or envelope to some destination); only pulse-width modulation exists here
{ "squaremet", WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 0.00,
        4,   1e9f,  2500, 0.50,    16.4, 16.4,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.31, 0.00,   0,  0.00 },

// ---- Toy Piano   [Surge filter: off] ------------------
//   ~ osc1 type 10 (wavetable/FM/string/noise) has no equivalent; substituted sawtooth
//   ~ delay time is tempo-synced in Surge; using a fixed 250 ms — adjust by ear
{ "toypiano",  WAVEFORM_SAWTOOTH,  WAVEFORM_SINE,       -12,   0.0, 1.00,
        4,   1e9f,  2000, 0.50,     2.0,  2.0,  125, 0.70, 0.45,
    1.00, 0.00, 0.00,   0.00, 0.00,   0.0, 0.00,   0.0,
    0.00, 0.20, 0.70, 250,  0.00 },


    
    
    
    
    
    
    
    
    
    
};

const int PRESET_COUNT = sizeof(PRESET) / sizeof(PRESET[0]);
