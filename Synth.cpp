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
static float    voiceFA[NUM_VOICES];    // base pitch, before vibrato
static float    voiceFB[NUM_VOICES];
static bool     voiceDecaying[NUM_VOICES];

static int      currentPreset = 0;
static uint32_t lastUpdateMs  = 0;
static float    lfoPhase      = 0.0f;
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
  float amp = (0.06f + 0.84f * powf(v, 1.4f)) * p.ampScale;

  // Detune split symmetrically so perceived pitch stays put.
  float half = p.cents * 0.5f;
  float fA   = voiceFreq[i] * powf(2.0f, -half / 1200.0f);
  float fB   = voiceFreq[i] * powf(2.0f, (p.semiB * 100.0f + half) / 1200.0f);

  // Velocity opens the filter, so hard hits are brighter and not just louder.
  float open = 1.0f + p.fVel * (v - 0.5f) * 2.0f;
  float base = clampf(voiceFreq[i] * p.fBase,        FILT_MIN_HZ, FILT_MAX_HZ);
  float peak = clampf(voiceFreq[i] * p.fPeak * open, base,        FILT_MAX_HZ);

  voiceAmp0[i]     = amp;
  voiceMixB[i]     = p.mixB;
  voiceFA[i]       = fA;
  voiceFB[i]       = fB;
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
  if (p.pwmD > 0.0f) { oscA[i].pulseWidth(0.5f); oscB[i].pulseWidth(0.5f); }
  vfilt[i].resonance(p.res);
  vfilt[i].frequency(peak);
  env[i].attack(p.a);
  env[i].hold(0.0f);
  env[i].decay(0.0f);
  env[i].sustain(1.0f);
  env[i].release(p.r);
  env[i].noteOn();
  AudioInterrupts();
}

void synthNoteOff(int row, int col) {
  env[row * 5 + col].noteOff();
}

bool synthVoiceIdle(int row, int col) {
  return !voiceDecaying[row * 5 + col];
}

void synthUpdate() {
  if (millis() - lastUpdateMs < UPDATE_MS) return;
  lastUpdateMs = millis();

  const Preset &p = PRESET[currentPreset];
  uint32_t t = millis();

  if (p.pwmD > 0.0f) {
    lfoPhase += 6.2831853f * p.pwmHz * (UPDATE_MS / 1000.0f);
    if (lfoPhase > 6.2831853f) lfoPhase -= 6.2831853f;
  }

  for (int i = 0; i < NUM_VOICES; i++) {
    if (!voiceDecaying[i]) continue;
    float dt = (float)(t - voiceOnMs[i]);

    float a = voiceAmp0[i] * expf(-dt / voiceTau[i]);
    if (a < AMP_FLOOR) {
      oscA[i].amplitude(0.0f);
      oscB[i].amplitude(0.0f);
      env[i].noteOff();
      voiceDecaying[i] = false;
      continue;
    }
    oscA[i].amplitude(a);
    oscB[i].amplitude(a * voiceMixB[i]);

    float k = expf(-dt / voiceFTau[i]);
    vfilt[i].frequency(voiceCutBase[i] +
                       (voiceCutPeak[i] - voiceCutBase[i]) * k);

    if (p.pwmD > 0.0f) {
      // Phase-offset per voice so a chord shimmers instead of pulsing as one.
      float ph = lfoPhase + i * 0.37f;
      oscA[i].pulseWidth(clampf(0.5f + p.pwmD * sinf(ph),        0.05f, 0.95f));
      oscB[i].pulseWidth(clampf(0.5f + p.pwmD * sinf(ph + 1.9f), 0.05f, 0.95f));
    }

    // Vibrato. Phase runs from each note's own start, so notes in a chord
    // wobble independently rather than in lockstep. Unlike oscillator sync,
    // this is safe to drive from here: 5 Hz sampled every 2 ms is ~100 steps
    // per cycle, far finer than the ear can resolve.
    if (p.vibCents > 0.0f) {
      float ramp = (p.vibDelayMs > 0.0f)
                   ? clampf((dt - p.vibDelayMs) / 300.0f, 0.0f, 1.0f) : 1.0f;
      float cents = p.vibCents * ramp
                    * sinf(6.2831853f * p.vibHz * dt * 0.001f);
      float m = powf(2.0f, cents / 1200.0f);
      oscA[i].frequency(voiceFA[i] * m);
      oscB[i].frequency(voiceFB[i] * m);
    }
  }
}

// ---- presets -------------------------------------------------------------
int presetCurrent() { return currentPreset; }

void presetAnnounce() {
  Serial.print("# preset "); Serial.print(currentPreset);
  Serial.print(": "); Serial.println(PRESET[currentPreset].name);
}

void presetList() {
  Serial.println("# presets:");
  for (int i = 0; i < PRESET_COUNT; i++) {
    const Preset &q = PRESET[i];
    const char *resp = q.hpMix > 0.5f ? "HP" : (q.bpMix > 0.5f ? "BP" : "LP");
    Serial.print("#   "); Serial.print(i);
    Serial.print(i == currentPreset ? " * " : "   ");
    Serial.print(q.name);
    Serial.print("\t"); Serial.print(resp);
    Serial.print("\tdetune "); Serial.print(q.cents, 0);
    Serial.print("c\tdrive "); Serial.print(q.drive, 2);
    Serial.print("\tverb "); Serial.print(q.verb, 2);
    Serial.print("\tdelay "); Serial.println(q.delaySend, 2);
  }
}

void presetSelect(int index) {
  currentPreset = ((index % PRESET_COUNT) + PRESET_COUNT) % PRESET_COUNT;
  applyPresetBus();
  presetAnnounce();
}

void synthReportLoad() {
  Serial.print("# audio CPU now="); Serial.print(AudioProcessorUsage(), 1);
  Serial.print("% max="); Serial.print(AudioProcessorUsageMax(), 1);
  Serial.print("%  blocks max="); Serial.print(AudioMemoryUsageMax());
  Serial.print(" / 400   patchcords="); Serial.println(pcCount);
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
}
