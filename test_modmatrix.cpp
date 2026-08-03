// ============================================================================
// test_modmatrix.cpp — routing, via-multipliers and curves
// ============================================================================
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "ModMatrix.h"
#include <cstdio>
#include <cmath>
#include <string>
static int failures = 0;
static void check(bool ok, const std::string &w, const std::string &d) {
  printf("  [%s] %s%s%s\n", ok?"PASS":"FAIL", w.c_str(), d.empty()?"":"  -- ", d.c_str());
  if (!ok) failures++;
}
int main() {
  char buf[200];
  LfoConfig lfo = { 5.0f, LFO_SINE, 0.0f, 0.0f }, off = { 0.0f, LFO_SINE, 0.0f, 0.0f };
  float out[NUM_DESTS];
  printf("modmatrix tests\n===============\n");

  // position and pressure are distinct sources, both always available
  printf("\nposition and pressure are separate sources\n");
  {
    const ModRoute R[] = { { SRC_POSITION, DST_AMP, 1.0f, SRC_CONST, CURVE_LINEAR },
                           { SRC_PRESSURE, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR } };
    ModContext c{}; c.position = 0.80f; c.pressure = 0.20f;
    modEvaluate(c, off, off, R, 2, 1, out);
    snprintf(buf, sizeof buf, "amp=%.2f cutoff=%.2f", out[DST_AMP], out[DST_CUTOFF]);
    check(fabsf(out[DST_AMP] - 0.80f) < 1e-4f, "position drives its route", buf);
    check(fabsf(out[DST_CUTOFF] - 2.0f) < 1e-4f, "pressure drives its own", buf);
  }

  // curves: the taper is a per-route musical choice
  printf("\ncurves are per route\n");
  {
    const ModRoute R[] = { { SRC_POSITION, DST_AMP,    1.0f,  SRC_CONST, CURVE_AUDIO  },
                           { SRC_POSITION, DST_CUTOFF, 10.0f, SRC_CONST, CURVE_LINEAR } };
    ModContext c{}; c.position = 0.5f;
    modEvaluate(c, off, off, R, 2, 1, out);
    snprintf(buf, sizeof buf, "at position 0.5: amp=%.3f (tapered), cutoff=%.2f (linear)",
             out[DST_AMP], out[DST_CUTOFF]);
    check(out[DST_AMP] < 0.35f, "audio taper compresses the bottom half", buf);
    check(fabsf(out[DST_CUTOFF] - 5.0f) < 1e-4f, "the same source stays linear elsewhere", buf);
  }

  // a curved bipolar source must stay symmetric, not rectify
  printf("\ncurved LFOs stay bipolar\n");
  {
    const ModRoute R[] = { { SRC_LFO1, DST_PITCH, 100.0f, SRC_CONST, CURVE_SQUARE } };
    ModContext a{}; a.ageMs = 50.0f;    // quarter cycle at 5 Hz: sin = +1
    ModContext b{}; b.ageMs = 150.0f;   // three quarters:        sin = -1
    modEvaluate(a, lfo, off, R, 1, 1, out); float pos = out[DST_PITCH];
    modEvaluate(b, lfo, off, R, 1, 1, out); float neg = out[DST_PITCH];
    snprintf(buf, sizeof buf, "peak=%.1f trough=%.1f", pos, neg);
    check(pos > 99.0f && neg < -99.0f, "squaring does not rectify", buf);
  }

  // via still multiplies, and is not itself curved
  printf("\nvia multiplies\n");
  {
    const ModRoute R[] = { { SRC_LFO1, DST_PITCH, 30.0f, SRC_PRESSURE, CURVE_LINEAR } };
    ModContext c{}; c.ageMs = 50.0f; c.pressure = 0.0f;
    modEvaluate(c, lfo, off, R, 1, 1, out); float none = out[DST_PITCH];
    c.pressure = 1.0f;
    modEvaluate(c, lfo, off, R, 1, 1, out); float full = out[DST_PITCH];
    snprintf(buf, sizeof buf, "pressure 0 -> %.1f, pressure 1 -> %.1f", none, full);
    check(fabsf(none) < 1e-4f && full > 29.0f, "vibrato blooms under pressure", buf);
  }

  printf("\n%s (%d failure%s)\n", failures?"FAILED":"ALL PASSED", failures,
         failures==1?"":"s");
  return failures ? 1 : 0;
}
#endif
