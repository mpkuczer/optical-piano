// ============================================================================
// Synth.cpp — audio graph, voices, modulation, bus effects
// ============================================================================
// SIGNAL PATH (per voice, x25)
//
//   oscA ┐                        ┌─ lowpass  ─┐
//        ├─> voice mix -> filter ─┼─ bandpass ─┼─> blend -> amp env ─┐
//   oscB ┘                        └─ highpass ─┘                     │
//                                                                    v
//                                                             mixer tree
//                                                                    │
//        ┌───────────────────────────────────────────────────────────┘
//        v
//   waveshaper ─┬──────────────────────────────> dry ─┐
//               ├─> reverb ─────────────────────────> ├─> out L/R
//               └─> delay (with feedback) ───────────>┘
//
// The state-variable filter computes all three responses simultaneously, so
// blending them costs one extra mixer per voice and nothing in DSP.
//
// Two modulations run in loop() rather than as audio objects, because the
// library has no object for either and control rate is plenty:
//   - amplitude: exponential decay (the envelope object's decay is linear)
//   - filter cutoff: exponential sweep from peak down to base
// ============================================================================
#include <Audio.h>
#include "Config.h"
#include "Preset.h"
#include "Synth.h"
#include "ModMatrix.h"

// ---- update rates --------------------------------------------------------
static const float    AMP_FLOOR   = 0.002f;   // below this, close the voice
static const uint32_t UPDATE_MS   = 2;
static const float    TAU_VEL_MIN = 0.75f;    // soft hits decay faster
static const float    TAU_VEL_MAX = 1.35f;    // hard hits ring longer
static const float    FILT_MIN_HZ = 120.0f;
static const float    FILT_MAX_HZ = 15000.0f;

// Gain staging. Deliberately conservative: resonant filters add gain around
// the cutoff and the effects add more, so the raw voice sum needs headroom
// well below unity. If it's too quiet, raise sgtl.volume() instead — that is
// analog gain applied AFTER the digital summing, so it cannot clip.
static const float MIX1_GAIN = 0.32f;
static const float MIX2_GAIN = 0.60f;
static const float VERB_ROOM = 0.72f;
static const float VERB_DAMP = 0.45f;

// ---- audio objects -------------------------------------------------------
static AudioSynthWaveform        oscA[NUM_VOICES];
static AudioSynthWaveform        oscB[NUM_VOICES];
static AudioMixer4               vmix[NUM_VOICES];   // sums the two oscillators
static AudioFilterStateVariable  vfilt[NUM_VOICES];
static AudioMixer4               fmix[NUM_VOICES];   // blends LP/BP/HP
static AudioEffectEnvelope       env[NUM_VOICES];
static AudioMixer4               mix1[7];
static AudioMixer4               mix2[2];
static AudioMixer4               mixOut;
static AudioEffectWaveshaper     shaper;
static AudioEffectFreeverbStereo verb;
static AudioMixer4               delayIn;            // dry + feedback
static AudioEffectDelay          delayLine;
static AudioMixer4               outL, outR;
static AudioOutputI2S            i2s1;
static AudioControlSGTL5000      sgtl;

static AudioConnection *pc[256];
static int pcCount = 0;

// ---- per-voice state -----------------------------------------------------
static float    voiceFreq[NUM_VOICES];
static uint32_t voiceOnMs[NUM_VOICES];
static float    voiceAmp0[NUM_VOICES];
static float    voiceTau[NUM_VOICES];
static float    voiceMixB[NUM_VOICES];
static float    voiceCutPeak[NUM_VOICES];
static float    voiceCutBase[NUM_VOICES];
static float    voiceFTau[NUM_VOICES];
static float    voiceFA[NUM_VOICES];    // base pitch, before modulation
static float    voiceFB[NUM_VOICES];
static float    voiceVel[NUM_VOICES];   // 0..1, for SRC_VELOCITY
static float    voiceKeytrk[NUM_VOICES];// -1..1, for SRC_KEYTRACK
static float    voiceRand[NUM_VOICES];  // -1..1, drawn once per note
static uint32_t voiceSeed[NUM_VOICES];  // sample-and-hold seed
static float    voiceResBase[NUM_VOICES];
static float    voicePress[NUM_VOICES];   // 0..1, updated while key is held
static float    voiceLfoPh[NUM_VOICES];   // 0..1 cycles of LFO head start
static float    voiceRelease[NUM_VOICES]; // ms, after release-velocity scaling
static bool     voiceReleased[NUM_VOICES];
static float    voiceLastGain[NUM_VOICES];  // for diagnostics only
static uint32_t voiceOffMs[NUM_VOICES];
static uint32_t rngState = 0x12345678;

// Cheap deterministic RNG. Per-note randomness is how analog drift is faked:
// every note lands on a slightly different detune and brightness.
static float nextRandomBipolar() {
  rngState = rngState * 1664525u + 1013904223u;
  return (float)((rngState >> 8) & 0xFFFF) / 32767.5f - 1.0f;
}
static bool     voiceDecaying[NUM_VOICES];

static int      currentPreset = 0;
static uint32_t lastUpdateMs  = 0;
static float    shapeTable[65];

static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// ---- graph ---------------------------------------------------------------
static void buildGraph() {
  for (int i = 0; i < NUM_VOICES; i++) {
    pc[pcCount++] = new AudioConnection(oscA[i],  0, vmix[i],  0);
    pc[pcCount++] = new AudioConnection(oscB[i],  0, vmix[i],  1);
    pc[pcCount++] = new AudioConnection(vmix[i],  0, vfilt[i], 0);
    // Outputs 0/1/2 of the state-variable filter are lowpass, bandpass and
    // highpass of the SAME signal, computed together. Blending is free.
    pc[pcCount++] = new AudioConnection(vfilt[i], 0, fmix[i],  0);
    pc[pcCount++] = new AudioConnection(vfilt[i], 1, fmix[i],  1);
    pc[pcCount++] = new AudioConnection(vfilt[i], 2, fmix[i],  2);
    pc[pcCount++] = new AudioConnection(fmix[i],  0, env[i],   0);
    pc[pcCount++] = new AudioConnection(env[i],   0, mix1[i / 4], i % 4);
  }
  for (int i = 0; i < 4; i++)
    pc[pcCount++] = new AudioConnection(mix1[i], 0, mix2[0], i);
  for (int i = 4; i < 7; i++)
    pc[pcCount++] = new AudioConnection(mix1[i], 0, mix2[1], i - 4);

  pc[pcCount++] = new AudioConnection(mix2[0], 0, mixOut, 0);
  pc[pcCount++] = new AudioConnection(mix2[1], 0, mixOut, 1);

  // Bus: waveshaper first (it's a distortion, so it wants the raw sum),
  // then parallel dry / reverb / delay sends.
  pc[pcCount++] = new AudioConnection(mixOut, 0, shaper, 0);
  pc[pcCount++] = new AudioConnection(shaper, 0, verb,    0);
  pc[pcCount++] = new AudioConnection(shaper, 0, delayIn, 0);
  pc[pcCount++] = new AudioConnection(shaper, 0, outL,    0);
  pc[pcCount++] = new AudioConnection(shaper, 0, outR,    0);

  pc[pcCount++] = new AudioConnection(verb, 0, outL, 1);
  pc[pcCount++] = new AudioConnection(verb, 1, outR, 1);

  // Feedback loop: the delay's output goes back into its own input. That is
  // what produces repeats rather than a single echo.
  pc[pcCount++] = new AudioConnection(delayIn,   0, delayLine, 0);
  pc[pcCount++] = new AudioConnection(delayLine, 0, delayIn,   1);
  pc[pcCount++] = new AudioConnection(delayLine, 0, outL,      2);
  pc[pcCount++] = new AudioConnection(delayLine, 0, outR,      2);

  pc[pcCount++] = new AudioConnection(outL, 0, i2s1, 0);
  pc[pcCount++] = new AudioConnection(outR, 0, i2s1, 1);
}

// A soft-saturation curve. drive 0 gives a straight line (no change at all);
// higher drive bends it, which is what generates extra harmonics.
static void buildShapeTable(float drive) {
  const int N = 65;
  float k = 1.0f + drive * 12.0f;
  for (int i = 0; i < N; i++) {
    float x = -1.0f + 2.0f * i / (N - 1);
    shapeTable[i] = (drive <= 0.0001f) ? x : tanhf(k * x) / tanhf(k);
  }
  shaper.shape(shapeTable, N);
}

// Everything that changes the moment you switch preset, as opposed to on the
// next note played.
static void applyPresetBus() {
  const Preset &p = PRESET[currentPreset];

  buildShapeTable(p.drive);

  float dry = 1.0f - 0.4f * p.verb - 0.3f * p.delaySend;
  if (dry < 0.25f) dry = 0.25f;
  outL.gain(0, dry);         outR.gain(0, dry);
  outL.gain(1, p.verb);      outR.gain(1, p.verb);
  outL.gain(2, p.delaySend); outR.gain(2, p.delaySend);
  outL.gain(3, 0.0f);        outR.gain(3, 0.0f);

  delayIn.gain(0, 1.0f);
  delayIn.gain(1, clampf(p.delayFb, 0.0f, 0.85f));   // >0.85 runs away
  delayIn.gain(2, 0.0f);
  delayIn.gain(3, 0.0f);
  if (p.delaySend > 0.0f && p.delayMs > 0.0f)
    delayLine.delay(0, clampf(p.delayMs, 10.0f, DELAY_MAX_MS));
  else
    delayLine.disable(0);

  for (int i = 0; i < NUM_VOICES; i++) {
    fmix[i].gain(0, p.lpMix);
    fmix[i].gain(1, p.bpMix);
    fmix[i].gain(2, p.hpMix);
    fmix[i].gain(3, 0.0f);
  }
}

void synthBegin() {
  AudioMemory(400);   // the delay line alone can want ~120 blocks
  buildGraph();

  for (int i = 0; i < NUM_VOICES; i++) {
    int r = i / 5, c = i % 5;
    voiceFreq[i] = 440.0f * powf(2.0f, (MIDI_NOTE[r][c] - 69) / 12.0f);

    oscA[i].begin(PRESET[0].waveA);
    oscB[i].begin(PRESET[0].waveB);
    oscA[i].amplitude(0.0f);
    oscB[i].amplitude(0.0f);

    vmix[i].gain(0, 1.0f); vmix[i].gain(1, 1.0f);
    vmix[i].gain(2, 0.0f); vmix[i].gain(3, 0.0f);

    vfilt[i].octaveControl(0.0f);
    vfilt[i].resonance(PRESET[0].res);
    vfilt[i].frequency(FILT_MAX_HZ);

    voiceDecaying[i] = false;
    voicePress[i]    = 0.0f;
    voiceReleased[i] = false;
    voiceRelease[i]  = PRESET[0].r;
  }
  for (int m = 0; m < 7; m++)
    for (int ch = 0; ch < 4; ch++) mix1[m].gain(ch, MIX1_GAIN);
  for (int m = 0; m < 2; m++)
    for (int ch = 0; ch < 4; ch++) mix2[m].gain(ch, MIX2_GAIN);
  mixOut.gain(0, 1.0f); mixOut.gain(1, 1.0f);
  mixOut.gain(2, 0.0f); mixOut.gain(3, 0.0f);

  verb.roomsize(VERB_ROOM);
  verb.damping(VERB_DAMP);
  for (int t = 1; t < 8; t++) delayLine.disable(t);

  applyPresetBus();

  sgtl.enable();
  sgtl.volume(0.80f);
  sgtl.lineOutLevel(29);
}

// ---- notes ---------------------------------------------------------------
void synthNoteOn(int row, int col, int velocity) {
  int i = row * 5 + col;
  float v = velocity / 127.0f;
  const Preset &p = PRESET[currentPreset];
  // In continuous mode the strike carries no information: loudness is key
  // POSITION, delivered later through pressure. Baking velocity into the
  // amplitude here is exactly what made a hard strike leave a loud floor
  // that leaning out could never get below.
  bool cont = (p.gesture == GESTURE_CONTINUOUS);
  if (cont) v = 0.5f;
  float amp = cont ? p.ampScale
                   : (0.06f + 0.84f * powf(v, 1.4f)) * p.ampScale;

  // Detune split symmetrically so perceived pitch stays put.
  float half = p.cents * 0.5f;
  float fA   = voiceFreq[i] * powf(2.0f, -half / 1200.0f);
  float fB   = voiceFreq[i] * powf(2.0f, (p.semiB * 100.0f + half) / 1200.0f);

  // Velocity no longer opens the filter here — that is now a route
  // (SRC_VELOCITY -> DST_CUTOFF) applied in synthUpdate along with everything
  // else, so all modulation goes through one path.
  float base = clampf(voiceFreq[i] * p.fBase, FILT_MIN_HZ, FILT_MAX_HZ);
  float peak = clampf(voiceFreq[i] * p.fPeak, base,        FILT_MAX_HZ);

  voiceAmp0[i]     = amp;
  voiceMixB[i]     = p.mixB;
  voiceFA[i]       = fA;
  voiceFB[i]       = fB;
  voiceVel[i]      = v;
  voiceKeytrk[i]   = clampf((MIDI_NOTE[row][col] - 60) / 12.0f, -1.0f, 1.0f);
  voiceRand[i]     = nextRandomBipolar();
  voiceSeed[i]     = rngState;
  voiceResBase[i]  = p.res;
  // Continuous voices keep whatever pressure the scanner has already
  // reported, so the note opens from the key's real position instead of
  // jumping up from zero.
  if (!cont) voicePress[i] = 0.0f;
  voiceReleased[i] = false;
  voiceRelease[i]  = p.r;
  // Golden-ratio stride: 25 voices spread almost perfectly evenly around the
  // cycle, and neighbouring keys never land close together. A chord played
  // dead flat still shimmers because each note's LFO sits somewhere else.
  voiceLfoPh[i]    = fmodf(i * 0.6180339887f, 1.0f);
  voiceTau[i]      = p.tau * (TAU_VEL_MIN + (TAU_VEL_MAX - TAU_VEL_MIN) * v);
  voiceCutPeak[i]  = peak;
  voiceCutBase[i]  = base;
  voiceFTau[i]     = p.fTau;
  voiceOnMs[i]     = millis();
  voiceDecaying[i] = true;

  // Waveform and envelope are applied HERE rather than when the preset
  // changes, so switching timbre mid-chord never clicks.
  AudioNoInterrupts();
  oscA[i].begin(p.waveA);
  oscB[i].begin(p.waveB);
  oscA[i].frequency(fA);
  oscB[i].frequency(fB);
  oscA[i].amplitude(amp);
  oscB[i].amplitude(amp * p.mixB);
  oscA[i].pulseWidth(0.5f);
  oscB[i].pulseWidth(0.5f);
  vfilt[i].resonance(p.res);
  vfilt[i].frequency(peak);
  // A continuous instrument must not have an attack shape of its own: any
  // ramp here is a second, competing amplitude envelope layered under the one
  // your finger is drawing.
  env[i].attack(cont ? 1.0f : p.a);
  env[i].hold(0.0f);
  env[i].decay(0.0f);
  env[i].sustain(1.0f);
  env[i].release(p.r);
  env[i].noteOn();
  AudioInterrupts();
}

void synthNoteOff(int row, int col, float releaseVel) {
  int i = row * 5 + col;
  // Fast lift -> short release, slow lift -> long. The preset's r is the
  // midpoint, not the maximum, so both directions are available.
  float s = REL_SCALE_SLOW +
            (REL_SCALE_FAST - REL_SCALE_SLOW) * clampf(releaseVel, 0.0f, 1.0f);
  voiceRelease[i]  = clampf(voiceRelease[i] * s, 5.0f, 4000.0f);
  voiceOffMs[i]    = millis();
  voiceReleased[i] = true;
  env[i].release(voiceRelease[i]);   // must be set BEFORE noteOff
  env[i].noteOff();
}

void synthPressure(int row, int col, float pressure) {
  int i = row * 5 + col;
  if (!voiceReleased[i]) voicePress[i] = clampf(pressure, 0.0f, 1.0f);
}

bool synthVoiceIdle(int row, int col) {
  return !voiceDecaying[row * 5 + col];
}

void synthUpdate() {
  if (millis() - lastUpdateMs < UPDATE_MS) return;
  lastUpdateMs = millis();

  const Preset &p = PRESET[currentPreset];
  uint32_t t = millis();
  float mod[NUM_DESTS];

  for (int i = 0; i < NUM_VOICES; i++) {
    if (!voiceDecaying[i]) continue;
    float dt = (float)(t - voiceOnMs[i]);

    // --- amplitude envelope (exponential; the library object's is linear) --
    float ampEnv = expf(-dt / voiceTau[i]);
    // Two ways a voice ends. The amplitude floor catches decaying patches;
    // the release timer catches sustaining ones (tau 1e9), whose amplitude
    // never falls at all. Without the second test those voices stayed
    // "decaying" forever — burning CPU, and making synthVoiceIdle lie, which
    // is why the preset audition went silent after the first sustained note.
    bool spent = voiceReleased[i] &&
                 (t - voiceOffMs[i]) > (uint32_t)(voiceRelease[i] + 50.0f);
    if (voiceAmp0[i] * ampEnv < AMP_FLOOR || spent) {
      oscA[i].amplitude(0.0f);
      oscB[i].amplitude(0.0f);
      env[i].noteOff();
      voiceDecaying[i] = false;
      continue;
    }

    // --- filter sweep, peak down to base ----------------------------------
    float filtEnv = expf(-dt / voiceFTau[i]);

    // --- evaluate every modulation route for this voice -------------------
    ModContext ctx;
    ctx.velocity = voiceVel[i];
    ctx.keytrack = voiceKeytrk[i];
    ctx.ampEnv   = ampEnv;
    ctx.filtEnv  = filtEnv;
    ctx.ageMs    = dt;
    ctx.randomv  = voiceRand[i];
    ctx.pressure = voicePress[i];
    ctx.lfoPhase = voiceLfoPh[i];
    modEvaluate(ctx, p.lfo1, p.lfo2, p.routes, p.nRoutes, voiceSeed[i], mod);

    // --- apply: amplitude -------------------------------------------------
    float gain = 1.0f + mod[DST_AMP];
    if (gain < 0.0f) gain = 0.0f;
    voiceLastGain[i] = gain;
    float a = voiceAmp0[i] * ampEnv * gain;
    float bLevel = clampf(voiceMixB[i] + mod[DST_OSCB], 0.0f, 1.5f);
    oscA[i].amplitude(a);
    oscB[i].amplitude(a * bLevel);

    // --- apply: pitch (cents on both) and detune (cents on B only) --------
    if (mod[DST_PITCH] != 0.0f || mod[DST_DETUNE] != 0.0f) {
      float mA = powf(2.0f, mod[DST_PITCH] / 1200.0f);
      float mB = powf(2.0f, (mod[DST_PITCH] + mod[DST_DETUNE]) / 1200.0f);
      oscA[i].frequency(voiceFA[i] * mA);
      oscB[i].frequency(voiceFB[i] * mB);
    }

    // --- apply: cutoff. The sweep is the base curve; routes offset it in
    //     SEMITONES, which is why velocity and keytrack compose sensibly.
    float cut = voiceCutBase[i] + (voiceCutPeak[i] - voiceCutBase[i]) * filtEnv;
    if (mod[DST_CUTOFF] != 0.0f)
      cut *= powf(2.0f, mod[DST_CUTOFF] / 12.0f);
    vfilt[i].frequency(clampf(cut, FILT_MIN_HZ, FILT_MAX_HZ));

    // --- apply: resonance and pulse width ---------------------------------
    if (mod[DST_RES] != 0.0f)
      vfilt[i].resonance(clampf(voiceResBase[i] + mod[DST_RES], 0.7f, 4.5f));

    if (mod[DST_PWM] != 0.0f) {
      float w = clampf(0.5f + mod[DST_PWM], 0.05f, 0.95f);
      oscA[i].pulseWidth(w);
      // Offset osc B slightly so the two don't move as one block.
      oscB[i].pulseWidth(clampf(0.5f + mod[DST_PWM] * 0.8f, 0.05f, 0.95f));
    }
  }
}

// ---- presets -------------------------------------------------------------
int presetCurrent() { return currentPreset; }

void presetAnnounce() {
  Serial.print("# preset "); Serial.print(currentPreset);
  Serial.print(": "); Serial.println(PRESET[currentPreset].name);
}

// Print the ACTIVE preset's modulation routes. Until now there has been no
// way to confirm from the board that the firmware you flashed actually
// contains pressure routing — if Presets.cpp was not updated along with
// everything else, pressure works perfectly and moves nothing.
void presetDumpRoutes() {
  const Preset &q = PRESET[currentPreset];
  Serial.print("# routes for preset "); Serial.print(currentPreset);
  Serial.print(" '"); Serial.print(q.name); Serial.print("' (");
  Serial.print(q.nRoutes); Serial.print(" routes, gesture ");
  Serial.println(q.gesture == GESTURE_CONTINUOUS
                 ? "CONTINUOUS — velocity ignored, absolute key position"
                 : "STRUCK — velocity sets loudness, pressure is relative");
  if (q.nRoutes == 0 || q.routes == nullptr) {
    Serial.println("#   (none)");
    return;
  }
  bool hasPress = false;
  for (int k = 0; k < q.nRoutes; k++) {
    const ModRoute &m = q.routes[k];
    Serial.print("#   "); Serial.print(modSourceName(m.source));
    Serial.print(" -> ");  Serial.print(modDestName(m.dest));
    Serial.print("  depth "); Serial.print(m.depth, 2);
    if (m.via != SRC_CONST) {
      Serial.print("  via "); Serial.print(modSourceName(m.via));
    }
    Serial.println();
    if (m.source == SRC_PRESSURE || m.via == SRC_PRESSURE) hasPress = true;
  }
  Serial.println(hasPress ? "#   -> pressure IS routed in this preset"
                          : "#   -> NO pressure route: nothing here can move");
}

void presetList() {
  Serial.println("# presets:");
  for (int i = 0; i < PRESET_COUNT; i++) {
    const Preset &q = PRESET[i];
    const char *resp = q.hpMix > 0.5f ? "HP" : (q.bpMix > 0.5f ? "BP" : "LP");
    Serial.print("#  "); Serial.print(i);
    Serial.print(i == currentPreset ? " * " : "   ");
    Serial.print(q.name);
    Serial.print("\t"); Serial.print(resp);
    Serial.print("\tdet "); Serial.print(q.cents, 0);
    Serial.print("c\tverb "); Serial.print(q.verb, 2);
    Serial.print("\tdly "); Serial.print(q.delaySend, 2);
    Serial.print("\t| ");
    for (int k = 0; k < q.nRoutes; k++) {
      Serial.print(modSourceName(q.routes[k].source));
      Serial.print(">");
      Serial.print(modDestName(q.routes[k].dest));
      Serial.print(" ");
    }
    Serial.println();
  }
}

void presetSelect(int index) {
  currentPreset = ((index % PRESET_COUNT) + PRESET_COUNT) % PRESET_COUNT;
  applyPresetBus();
  presetAnnounce();
}

// Drive pressure directly, with the scanner completely out of the picture.
// If the sound does not change under this, the fault is in the audio graph or
// the route table. If it does change, the fault is upstream in the scanner.
// Everything so far has been guesswork for want of exactly this split.
void synthPressureTest() {
  const Preset &p = PRESET[currentPreset];
  Serial.print("# pressure sweep on the middle key, preset '");
  Serial.print(p.name); Serial.println("' — scanner bypassed");
  bool routed = false;
  for (int k = 0; k < p.nRoutes; k++)
    if (p.routes[k].source == SRC_PRESSURE || p.routes[k].via == SRC_PRESSURE)
      routed = true;
  if (!routed)
    Serial.println("# WARNING: this preset has no pressure route. Try 'flute',"
                   " 'theremin', 'obie' or 'shimmer'.");

  synthNoteOn(2, 2, 100);
  uint32_t t0 = millis(), nextPrint = 0;
  while (millis() - t0 < 6000) {
    float x = (millis() - t0) / 6000.0f;
    float press = x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;   // 0 -> 1 -> 0
    synthPressure(2, 2, press);
    synthUpdate();
    if (millis() > nextPrint) {
      nextPrint = millis() + 250;
      Serial.print("#   pressure="); Serial.print(press, 2);
      Serial.print("  ampGain=");    Serial.print(voiceLastGain[12], 3);
      Serial.println();
    }
    delay(2);
  }
  synthNoteOff(2, 2, 0.5f);
  Serial.println("# sweep done. Did the sound move?");
}

uint8_t presetGesture() { return PRESET[currentPreset].gesture; }

void synthReportLoad() {
  Serial.print("# audio CPU now="); Serial.print(AudioProcessorUsage(), 1);
  Serial.print("% max="); Serial.print(AudioProcessorUsageMax(), 1);
  Serial.print("%  blocks max="); Serial.print(AudioMemoryUsageMax());
  Serial.print(" / 400   patchcords="); Serial.println(pcCount);
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
}
