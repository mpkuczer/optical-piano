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
#include "Gesture.h"
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

// ---- anti-zipper ----------------------------------------------------------
// AudioSynthWaveform::amplitude() and AudioFilterStateVariable::frequency()
// take effect IMMEDIATELY and hold for the whole next 128-sample block. They
// do not ramp. So whatever value synthUpdate writes becomes a staircase in the
// audio, one step every 2-3 ms.
//
// That would be harmless if the values were smooth, but they are not: on the
// continuous patches they are the photodarlington reading, and Gesture.cpp
// measures roughly 0.011 of travel of jitter on it. Through an exponent that
// is around 1.5% of amplitude wobbling at the scan rate — which is amplitude
// modulation by noise, and modulating a tone with noise puts noise sidebands
// either side of it. Sensor hiss becomes audio hiss, pitched around whatever
// you are playing.
//
// A one-pole on the APPLIED value fixes it. Gesture.cpp already smooths at
// 8 ms; this is a second stage, and two poles reject the jitter roughly twice
// as hard in dB. 12 ms is a 13 Hz corner: about 25 dB down at the scan rate
// and far faster than any gesture a finger can make, so it costs nothing you
// can feel. Raise it if hiss persists, lower it if expression feels laggy.
static const float AMP_SMOOTH_MS = 12.0f;
// The cutoff wants a shorter one. Filter sweeps are a deliberate part of the
// attack on the plucked patches (fTau down to 180 ms) and over-smoothing them
// blunts the transient, but the SVF is just as sensitive to a stepping
// coefficient as the oscillator is to a stepping amplitude.
static const float CUT_SMOOTH_MS = 6.0f;

// Gain staging. Deliberately conservative: resonant filters add gain around
// the cutoff and the effects add more, so the raw voice sum needs headroom
// well below unity. If it's too quiet, raise sgtl.volume() instead — that is
// analog gain applied AFTER the digital summing, so it cannot clip.
// Release-time scaling from release velocity. A sound property, so it lives
// with the synth rather than in the hardware config.
static const float REL_SCALE_FAST = 0.35f;   // snatched away: cut it short
static const float REL_SCALE_SLOW = 1.60f;   // lifted gently: let it hang

static const float MIX1_GAIN = 0.32f;
static const float MIX2_GAIN = 0.60f;
static const float VERB_ROOM = 0.72f;
// Raised 0.45 -> 0.62. `damping` is a one-pole INSIDE each comb's feedback
// loop, so it darkens the recirculating quantisation noise as well as the
// signal. At 0.45 the pole coefficient is only 0.18 — a corner around 12 kHz,
// which is barely a lowpass at all, and the reverb's own hiss stayed white.
static const float VERB_DAMP = 0.62f;

// ---- reverb noise: why the send is hot and the return is quiet -------------
// AudioEffectFreeverbStereo is fixed point with INT16 comb and allpass
// buffers, and it scales its input down by 1/15 for internal headroom
// (effect_freeverb.cpp: `input = sat16(block->data[i] * 8738, 17)`) then
// multiplies the output back up by 30. Every sample in every one of the eight
// combs is truncated to int16 and fed back at ~0.9, so the round-off error
// recirculates, accumulates across combs, and leaves at 30x.
//
// That error is broadband, diffuse and decorrelated between L and R — which is
// to say it is white noise, and it appears exactly when you play. It is the
// dominant noise source in this instrument, not the oscillators.
//
// Its level is fixed relative to the reverb's OWN full scale, so the only
// lever is to send the reverb a hotter signal and turn the return back down by
// the same factor. The dry bus sits at about 0.16 of full scale for one voice
// (MIX1_GAIN * MIX2_GAIN * ampScale), so it is throwing away four bits before
// the reverb ever sees it.
//
// 2.0 is deliberately modest: AudioMixer4 SATURATES, so a send this hot will
// flat-top the reverb input somewhere above three simultaneous full-amplitude
// notes. Distortion in a reverb tail on a dense chord is far less noticeable
// than hiss on every note, but if you hear the tail crunch on big chords, lower
// this. Raising it buys 6 dB of reverb SNR per doubling.
static const float VERB_SEND = 2.0f;
// The recirculated round-off is white; the reverb's musical content is not.
// A 4th-order lowpass on the RETURN removes most of the hiss the ear actually
// weights while leaving the tail intact — most of white noise's perceived
// loudness lives above this corner, and a real room has nothing up there
// anyway. Lower it for a darker room, raise it if the tail sounds muffled.
static const float VERB_LP_HZ = 4500.0f;

// ---- audio objects -------------------------------------------------------
static AudioSynthWaveform        oscA[NUM_VOICES];
static AudioSynthWaveform        oscB[NUM_VOICES];
// FM pair. oscF is a carrier that takes a phase-modulation input; oscM drives
// it. They are SEPARATE objects rather than a conversion of oscA because
// AudioSynthWaveformModulated has no pulseWidth(), and DST_PWM is load-bearing
// for wurli and shimmer.
//
// Fifty extra objects for 25 voices sounds expensive and is not: both classes
// return early when magnitude is 0 (synth_waveform.cpp) without allocating a
// block, so a preset with fmRatio 0 pays essentially nothing for them.
static AudioSynthWaveformModulated oscF[NUM_VOICES];
static AudioSynthWaveform          oscM[NUM_VOICES];
static AudioMixer4               vmix[NUM_VOICES];   // sums the two oscillators
static AudioFilterStateVariable  vfilt[NUM_VOICES];
static AudioMixer4               fmix[NUM_VOICES];   // blends LP/BP/HP
static AudioEffectEnvelope       env[NUM_VOICES];
static AudioMixer4               mix1[7];
static AudioMixer4               mix2[2];
static AudioMixer4               mixOut;
static AudioEffectWaveshaper     shaper;
static AudioMixer4               verbSend;           // gain-up into the reverb
static AudioEffectFreeverbStereo verb;
static AudioFilterBiquad         verbLpL, verbLpR;   // de-hiss the return
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
static float    voicePress[NUM_VOICES];   // relative, 0..1
static float    voicePos[NUM_VOICES];     // absolute, 0..1
static float    voiceKeyVel[NUM_VOICES];  // -1..1, positive descending
static float    voiceLfoPh[NUM_VOICES];   // 0..1 cycles of LFO head start
static float    voiceRelease[NUM_VOICES]; // ms, after release-velocity scaling
static bool     voiceReleased[NUM_VOICES];
static float    voiceLastGain[NUM_VOICES];  // for diagnostics only
static uint32_t voiceOffMs[NUM_VOICES];
// Smoothed copies of the three parameters synthUpdate writes every sweep. See
// AMP_SMOOTH_MS. Seeded at note-on so a voice opens at its real value rather
// than sliding up to it from wherever the previous note left off.
static float    voiceAmpSmA[NUM_VOICES];
static float    voiceAmpSmB[NUM_VOICES];
static float    voiceCutSm[NUM_VOICES];
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
static const int SHAPE_N = 257;        // must be 2^n + 1
static float    shapeTable[SHAPE_N];

static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// ---- graph ---------------------------------------------------------------
static void buildGraph() {
  for (int i = 0; i < NUM_VOICES; i++) {
    pc[pcCount++] = new AudioConnection(oscA[i],  0, vmix[i],  0);
    pc[pcCount++] = new AudioConnection(oscB[i],  0, vmix[i],  1);
    // Modulator into the carrier's input 0 — that is the modulation input
    // (synth_waveform.cpp reads moddata from 0, shapedata from 1). The carrier
    // then joins the voice on vmix input 2, which was already allocated and
    // zeroed, so the graph does not change shape.
    pc[pcCount++] = new AudioConnection(oscM[i],  0, oscF[i],  0);
    pc[pcCount++] = new AudioConnection(oscF[i],  0, vmix[i],  2);
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
  pc[pcCount++] = new AudioConnection(shaper, 0, verbSend, 0);
  pc[pcCount++] = new AudioConnection(shaper, 0, delayIn, 0);
  pc[pcCount++] = new AudioConnection(shaper, 0, outL,    0);
  pc[pcCount++] = new AudioConnection(shaper, 0, outR,    0);

  // Hot in, lowpassed and quiet out — see VERB_SEND for why.
  pc[pcCount++] = new AudioConnection(verbSend, 0, verb,    0);
  pc[pcCount++] = new AudioConnection(verb,     0, verbLpL, 0);
  pc[pcCount++] = new AudioConnection(verb,     1, verbLpR, 0);
  pc[pcCount++] = new AudioConnection(verbLpL,  0, outL,    1);
  pc[pcCount++] = new AudioConnection(verbLpR,  0, outR,    1);

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
//
// 257 points, not 65. AudioEffectWaveshaper LERPS between table entries, so
// the curve the audio actually meets is piecewise linear, and every kink
// between segments is a discontinuity in the first derivative — which radiates
// high harmonics that then alias back down as inharmonic hash. At 65 points
// each segment spanned 1/32 of full scale, coarse enough to hear on the drive
// presets. 257 cuts the kink angle by 4x for 512 bytes of table.
// The length must be a power of two plus one or shape() silently does nothing.
static void buildShapeTable(float drive) {
  const int N = SHAPE_N;
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
  // The send already multiplied by VERB_SEND, so the return divides it back
  // out. p.verb still means what it always meant; nothing in Presets.cpp moves.
  float verbRet = p.verb / VERB_SEND;
  outL.gain(1, verbRet);     outR.gain(1, verbRet);
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
    vmix[i].gain(2, 1.0f); vmix[i].gain(3, 0.0f);

    // Full-scale phase deviation for a modulator at amplitude 1.0. 600 degrees
    // is 10.5 radians, which puts the whole musically useful index range
    // (roughly 0 to 10) across a DST_FM depth of 0..1 — so a route depth reads
    // directly as "how bright", with 0.3 mellow and 0.8 brassy.
    oscF[i].phaseModulation(600.0f);
    oscF[i].begin(WAVEFORM_SINE);
    oscF[i].amplitude(0.0f);
    oscM[i].begin(WAVEFORM_SINE);
    oscM[i].amplitude(0.0f);

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

  verbSend.gain(0, VERB_SEND);
  verbSend.gain(1, 0.0f); verbSend.gain(2, 0.0f); verbSend.gain(3, 0.0f);
  verb.roomsize(VERB_ROOM);
  verb.damping(VERB_DAMP);
  // Two Butterworth stages = 24 dB/octave. The Q pair is what makes a cascade
  // of two biquads maximally flat rather than peaky at the corner.
  verbLpL.setLowpass(0, VERB_LP_HZ, 0.5412f);
  verbLpL.setLowpass(1, VERB_LP_HZ, 1.3066f);
  verbLpR.setLowpass(0, VERB_LP_HZ, 0.5412f);
  verbLpR.setLowpass(1, VERB_LP_HZ, 1.3066f);
  for (int t = 1; t < 8; t++) delayLine.disable(t);

  applyPresetBus();

  sgtl.enable();
  // Raised from 0.80 to make up for the voice-mixer normalisation. This is
  // analog gain applied AFTER the digital summing, so unlike raising the
  // mixer gains it cannot clip. If the instrument is too quiet, raise this
  // and nothing else.
  sgtl.volume(0.92f);
  sgtl.lineOutLevel(29);
}

// ---- notes ---------------------------------------------------------------
void synthNoteOn(int row, int col, float velocity) {
  int i = row * 5 + col;
  float v = clampf(velocity, 0.0f, 1.0f);
  const Preset &p = PRESET[currentPreset];
  // In continuous mode the strike carries no information: loudness is key
  // POSITION, delivered later through pressure. Baking velocity into the
  // amplitude here is exactly what made a hard strike leave a loud floor
  // that leaning out could never get below.
  // The articulation already decided whether velocity means anything — see
  // Gesture.cpp, which hands us 0.5 when it does not. Nothing to re-decide.
  bool cont = (p.articulation == ART_CONTINUOUS);
  // Velocity -> loudness used to live HERE, as (0.06 + 0.84*v^1.4)*ampScale,
  // and it was the last modulation that never went through the matrix. The
  // comment below about the filter claimed "all modulation goes through one
  // path" while this line quietly disproved it: a patch could not change its
  // own velocity curve, and a SRC_VELOCITY -> DST_AMP route would have stacked
  // on top of an invisible one.
  //
  // It is now the VEL_TO_AMP route pair in Presets.cpp, carried by every
  // non-continuous patch. The migration is EXACT for any patch with no other
  // DST_AMP route, because
  //     old: ampScale*k * ampEnv * (1 + others)
  //     new: ampScale   * ampEnv * (k + others)
  // agree when others == 0. Where they do not agree — tremolo — the route
  // table says so explicitly.
  float amp = p.ampScale;
  // Both oscillators feed vmix at gain 1.0, so the voice's peak is
  // amp * (1 + mixB) — and AudioMixer4 SATURATES at full scale. theremin sat
  // at 1.125, flute 1.105, tremolo 1.595. Clipping a sine flat-tops it, and a
  // flat-topped sine is odd harmonics, which is why it read as a clarinet's
  // low register. Normalising here means ampScale now means what it says: the
  // peak of the whole voice, not of one oscillator.
  amp /= (1.0f + p.mixB);

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
  // Expression is pushed every sweep, including before this note began, so
  // the voice opens at the key's real level rather than jumping up from zero.
  voicePress[i] = 0.0f;
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

  // Evaluate modulation ONCE, right now, so the oscillator never starts at a
  // level the modulation is about to contradict. Without this the voice runs
  // at full amplitude until the next synthUpdate — up to UPDATE_MS plus a
  // whole scan sweep — and AudioSynthWaveform::amplitude() does not ramp, so
  // that is a step discontinuity straight into the speaker. Audible as a pop
  // on the first note out of silence, and masked (not absent) when a reverb
  // tail happens to be ringing.
  // ...and that has to mean EVERY destination the first synthUpdate is about to
  // apply, not just DST_AMP. It used to mean only DST_AMP, which left the other
  // six stepping on the first update — up to UPDATE_MS plus a whole scan sweep
  // later, and then through a smoother, so it arrived not as a click but as a
  // distinct SECOND ATTACK.
  //
  // Invisible until a patch made a destination carry the whole sound. epiano
  // has mixB 0.00 and builds its tine entirely from a DST_OSCB route, so oscB
  // opened at exactly zero and the bell swelled in ~5 ms behind the body. Every
  // other preset has a non-zero mixB and modest routes, so its step was small
  // enough to pass for part of the attack.
  //
  // The STORED bases — voiceFA/FB, voiceCutPeak/Base, voiceMixB, voiceResBase —
  // are deliberately left alone above. They are what synthUpdate modulates
  // FROM; folding this instant's modulation into them would bake it in forever.
  float aFreq0 = fA, bFreq0 = fB;
  float bLevel0 = p.mixB, cut0 = peak, res0 = p.res, pw0 = 0.5f;
  const bool useFM = (p.fmRatio > 0.0f);
  float fmIdx0 = 0.0f;
  {
    ModContext ctx;
    ctx.velocity = voiceVel[i];
    ctx.keytrack = voiceKeytrk[i];
    ctx.ampEnv   = 1.0f;
    ctx.filtEnv  = 1.0f;
    ctx.ageMs    = 0.0f;
    ctx.randomv  = voiceRand[i];
    ctx.pressure = voicePress[i];
    ctx.position = voicePos[i];
    ctx.keyvel   = voiceKeyVel[i];
    ctx.lfoPhase = voiceLfoPh[i];
    float mod0[NUM_DESTS];
    modEvaluate(ctx, p.lfo1, p.lfo2, p.routes, p.nRoutes, voiceSeed[i], mod0);
    amp *= clampf(1.0f + mod0[DST_AMP], 0.0f, 2.0f);
    bLevel0 = clampf(p.mixB + mod0[DST_OSCB], 0.0f, 1.5f);
    if (mod0[DST_PITCH] != 0.0f || mod0[DST_DETUNE] != 0.0f) {
      aFreq0 = fA * powf(2.0f, mod0[DST_PITCH] / 1200.0f);
      bFreq0 = fB * powf(2.0f, (mod0[DST_PITCH] + mod0[DST_DETUNE]) / 1200.0f);
    }
    if (mod0[DST_CUTOFF] != 0.0f)
      cut0 = clampf(peak * powf(2.0f, mod0[DST_CUTOFF] / 12.0f),
                    FILT_MIN_HZ, FILT_MAX_HZ);
    res0 = clampf(p.res + mod0[DST_RES], 0.7f, 4.5f);
    pw0  = clampf(0.5f + mod0[DST_PWM], 0.05f, 0.95f);
    fmIdx0 = clampf(mod0[DST_FM], 0.0f, 1.0f);
    // The same headroom guard synthUpdate applies, for the same reason: the
    // earlier `amp /= (1 + p.mixB)` normalised against the UNMODULATED mixB,
    // and a route can push the pair well past that.
    float pk = amp * (1.0f + bLevel0);
    if (pk > 1.0f) amp /= pk;
  }

  // Waveform and envelope are applied HERE rather than when the preset
  // changes, so switching timbre mid-chord never clicks.
  AudioNoInterrupts();
  // ORDER MATTERS, and it did not used to. The band-limited waveforms carry
  // internal state, and begin() seeds it from whatever phase_increment and
  // pulse_width are set at that instant — see AudioSynthWaveform::begin() in
  // synth_waveform.h. Called before frequency(), it seeds the BLEP generator
  // with the previous note's pitch, and the oscillator spends the first cycles
  // correcting itself. Frequency and width first, then begin.
  oscA[i].frequency(aFreq0);
  oscB[i].frequency(bFreq0);
  oscA[i].pulseWidth(pw0);
  oscB[i].pulseWidth(pw0);
  oscA[i].begin(p.waveA);
  oscB[i].begin(p.waveB);
  // The FM carrier OCCUPIES oscA'S SLOT rather than adding to it: same
  // frequency, same level, same envelope, and exactly one of the two sounds.
  // That keeps the gain staging honest — `amp /= (1 + mixB)` above still
  // describes the whole voice — and leaves oscB free to layer underneath.
  oscA[i].amplitude(useFM ? 0.0f : amp);
  oscB[i].amplitude(amp * bLevel0);
  if (useFM) {
    oscM[i].frequency(aFreq0 * p.fmRatio);
    oscM[i].begin(WAVEFORM_SINE);
    oscM[i].amplitude(fmIdx0);
    oscF[i].frequency(aFreq0);
    oscF[i].begin(WAVEFORM_SINE);
  }
  oscF[i].amplitude(useFM ? amp : 0.0f);
  // Seed the smoothers at the modulated values too. Seeding them at the
  // unmodulated ones is what turned the step into a ramp — which is worse,
  // not better: a click is a click, but a ramp arriving late is a second note.
  voiceAmpSmA[i] = amp;
  voiceAmpSmB[i] = amp * bLevel0;
  voiceCutSm[i]  = cut0;
  vfilt[i].resonance(res0);
  vfilt[i].frequency(cut0);
  // A continuous instrument must not have an attack shape of its own: any
  // ramp here is a second, competing amplitude envelope layered under the one
  // your finger is drawing.
  // 4 ms rather than 1: long enough that the envelope's own opening edge is
  // below the threshold of a click, short enough to feel instantaneous.
  env[i].attack(cont ? 4.0f : p.a);
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

void synthExpression(int row, int col, float position, float pressure,
                     float keyvel) {
  int i = row * 5 + col;
  voicePos[i] = clampf(position, 0.0f, 1.0f);
  voiceKeyVel[i] = clampf(keyvel, -1.0f, 1.0f);
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
      // The modulator too, not just the carrier. A silent carrier is inaudible
      // either way, but a modulator left running is an oscillator that never
      // takes the magnitude==0 early-out — it would allocate a block every
      // update for all 25 voices, forever.
      oscF[i].amplitude(0.0f);
      oscM[i].amplitude(0.0f);
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
    ctx.position = voicePos[i];
    ctx.keyvel   = voiceKeyVel[i];
    ctx.lfoPhase = voiceLfoPh[i];
    modEvaluate(ctx, p.lfo1, p.lfo2, p.routes, p.nRoutes, voiceSeed[i], mod);

    // --- apply: amplitude -------------------------------------------------
    float gain = 1.0f + mod[DST_AMP];
    if (gain < 0.0f) gain = 0.0f;
    voiceLastGain[i] = gain;
    float a = voiceAmp0[i] * ampEnv * gain;
    float bLevel = clampf(voiceMixB[i] + mod[DST_OSCB], 0.0f, 1.5f);
    // Modulation can push the pair back over full scale even after the
    // note-on normalisation — tremolo adds up to +0.45 of gain, presstest
    // +0.60. Scale the pair down rather than let the mixer saturate.
    {
      float peak = a * (1.0f + bLevel);
      if (peak > 1.0f) a /= peak;
    }
    // Smooth what is actually written, not the modulation that produced it —
    // the normalisation above can move `a` on its own, and a step is a step
    // whatever caused it. Both oscillators get their own pole so a moving
    // bLevel does not zipper either.
    const float kAmp = (float)UPDATE_MS / (AMP_SMOOTH_MS + (float)UPDATE_MS);
    voiceAmpSmA[i] += kAmp * (a          - voiceAmpSmA[i]);
    voiceAmpSmB[i] += kAmp * (a * bLevel - voiceAmpSmB[i]);
    const bool fmOn = (p.fmRatio > 0.0f);
    oscA[i].amplitude(fmOn ? 0.0f : voiceAmpSmA[i]);
    oscB[i].amplitude(voiceAmpSmB[i]);
    if (fmOn) {
      oscF[i].amplitude(voiceAmpSmA[i]);
      // The INDEX is not smoothed by kAmp. It is a timbre control, not a level,
      // and the whole point of an FM electric piano is that the index falls
      // fast at the start of the note — smoothing it with the amplitude pole
      // would blunt exactly the transient the patch exists for. Gesture.cpp
      // and the filtEnv already provide whatever smoothing it needs.
      oscM[i].amplitude(clampf(mod[DST_FM], 0.0f, 1.0f));
    } else {
      // Silenced unconditionally, not just on the way out. Switching preset
      // does NOT stop the notes already sounding, so a voice struck under an
      // FM patch can arrive here on the ordinary path the very next update:
      // oscA comes back at full level while the carrier is still holding
      // whatever the last FM update left it at — two oscillators where the
      // gain staging budgeted for one, until that note finally releases.
      oscF[i].amplitude(0.0f);
      oscM[i].amplitude(0.0f);
    }

    // --- apply: pitch (cents on both) and detune (cents on B only) --------
    if (mod[DST_PITCH] != 0.0f || mod[DST_DETUNE] != 0.0f) {
      float mA = powf(2.0f, mod[DST_PITCH] / 1200.0f);
      float mB = powf(2.0f, (mod[DST_PITCH] + mod[DST_DETUNE]) / 1200.0f);
      oscA[i].frequency(voiceFA[i] * mA);
      oscB[i].frequency(voiceFB[i] * mB);
      // Carrier AND modulator, or the ratio between them drifts and vibrato
      // would change the timbre rather than the pitch. They have to move
      // together, by the same factor, always.
      if (fmOn) {
        oscF[i].frequency(voiceFA[i] * mA);
        oscM[i].frequency(voiceFA[i] * mA * p.fmRatio);
      }
    }

    // --- apply: cutoff. The sweep is the base curve; routes offset it in
    //     SEMITONES, which is why velocity and keytrack compose sensibly.
    float cut = voiceCutBase[i] + (voiceCutPeak[i] - voiceCutBase[i]) * filtEnv;
    if (mod[DST_CUTOFF] != 0.0f)
      cut *= powf(2.0f, mod[DST_CUTOFF] / 12.0f);
    cut = clampf(cut, FILT_MIN_HZ, FILT_MAX_HZ);
    // Smoothed in LOG space, because that is the domain the ear and the route
    // depths both work in: a fixed-Hz pole would crawl at the bottom of the
    // range and snap at the top, so the same sweep would smear differently
    // depending on which octave it happened in.
    const float kCut = (float)UPDATE_MS / (CUT_SMOOTH_MS + (float)UPDATE_MS);
    voiceCutSm[i] *= powf(cut / voiceCutSm[i], kCut);
    vfilt[i].frequency(voiceCutSm[i]);

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
  switch (q.articulation) {
    case ART_CONTINUOUS: Serial.println("CONTINUOUS (no trigger point, no velocity)"); break;
    case ART_STRUCK:     Serial.println("STRUCK (velocity from flight time)");         break;
    case ART_GATED:      Serial.println("GATED (no velocity, no held expression)");    break;
    case ART_PLUCKED:    Serial.println("PLUCKED (velocity discarded, held expression)"); break;
    default:             Serial.println("?");                                          break;
  }
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
    // shape overrides curve, so report whichever is actually in force.
    if (m.shape > 0.0f) {
      Serial.print("  shape x^"); Serial.print(m.shape, 2);
    } else if (m.curve != CURVE_LINEAR) {
      Serial.print("  curve "); Serial.print(m.curve);
    }
    Serial.println();
    if (m.source == SRC_PRESSURE || m.via == SRC_PRESSURE ||
        m.source == SRC_POSITION || m.via == SRC_POSITION) hasPress = true;
  }
  Serial.println(hasPress ? "#   -> key motion IS routed in this preset"
                          : "#   -> NO position/pressure route: the held key does nothing");
  // DST_FM is the one destination with a precondition on the preset row, and
  // therefore the one that can be routed, dumped, and still do absolutely
  // nothing. This function exists to catch exactly that class of mistake.
  bool hasFM = false;
  for (int k = 0; k < q.nRoutes; k++)
    if (q.routes[k].dest == DST_FM) hasFM = true;
  if (q.fmRatio > 0.0f) {
    Serial.print("#   -> FM carrier ON, ratio "); Serial.print(q.fmRatio, 2);
    if (!hasFM)
      Serial.print("  (but NO route to fm: index is 0, so it is a plain sine)");
    Serial.println();
  } else if (hasFM) {
    Serial.println("#   -> fm routed but fmRatio is 0: FM is OFF and those "
                   "routes move nothing");
  }
  // Worst-case level check, since silent clipping is the least obvious way
  // for a patch to sound wrong.
  // Sum ALL amplitude routes, not just the positive ones. A patch that pulls
  // the floor to zero with SRC_CONST and pushes back up with SRC_POSITION has
  // a maximum of 1.0, not 2.0, and counting only the positive half reported a
  // clip that cannot happen.
  float ampMod = 0.0f;
  for (int k = 0; k < q.nRoutes; k++)
    if (q.routes[k].dest == DST_AMP) ampMod += q.routes[k].depth;
  if (ampMod < 0.0f) ampMod = 0.0f;
  float peak = q.ampScale * (1.0f + q.mixB) * (1.0f + ampMod);
  Serial.print("#   headroom: peak voice sum = "); Serial.print(peak, 3);
  Serial.println(peak > 1.0f
                 ? "  (was clipping before normalisation; now scaled)"
                 : "  (fits)");
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
    if (p.routes[k].source == SRC_PRESSURE || p.routes[k].via == SRC_PRESSURE ||
        p.routes[k].source == SRC_POSITION || p.routes[k].via == SRC_POSITION)
      routed = true;
  if (!routed)
    Serial.println("# WARNING: this preset has no pressure route. Try 'flute',"
                   " 'theremin', 'obie' or 'shimmer'.");

  synthNoteOn(2, 2, 0.78f);
  uint32_t t0 = millis(), nextPrint = 0;
  while (millis() - t0 < 6000) {
    float x = (millis() - t0) / 6000.0f;
    float press = x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;   // 0 -> 1 -> 0
    synthExpression(2, 2, press, press);
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

uint8_t presetArticulation() { return PRESET[currentPreset].articulation; }

const GestureConfig &presetGesture() {
  const GestureConfig *g = PRESET[currentPreset].gesture;
  return g ? *g : GESTURE_DEFAULT;
}

void synthReportLoad() {
  Serial.print("# audio CPU now="); Serial.print(AudioProcessorUsage(), 1);
  Serial.print("% max="); Serial.print(AudioProcessorUsageMax(), 1);
  Serial.print("%  blocks max="); Serial.print(AudioMemoryUsageMax());
  Serial.print(" / 400   patchcords="); Serial.println(pcCount);
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
}
