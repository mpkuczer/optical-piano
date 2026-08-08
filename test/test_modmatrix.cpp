// ============================================================================
// test_modmatrix.cpp — the matrix maths, on the desktop
// ============================================================================
// ModMatrix.h opens by saying every bug in this project so far was a unit
// error a test would have caught in a second, and then this layer went
// untested anyway — run_tests.sh has named this file, commented out, for
// longer than it has existed. It exists now because CURVE_DETENT arrived, and
// a curve whose job is to make a bend land IN TUNE is a claim about specific
// numbers: "0.60 of travel is the prescribed pitch, exactly" is either true or
// the patch does not work, and listening is a slow way to find out.
//
// Two patches are pinned here — meend, which bends up into its pitch from
// below, and sarangi, which bends a whole tone either side of it — because in
// both cases the musical claim IS an arithmetic claim about route sums.
#ifdef ARDUINO
#warning "test file in the sketch folder — move it to test/"
#else
#include "ModMatrix.h"
#include <cstdio>
#include <cmath>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string &what, const std::string &d = "") {
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
         d.empty() ? "" : "  -- ", d.c_str());
  if (!ok) failures++;
}

// Curves are not reachable on their own — applyCurve is static — so every test
// here goes through the public entry point, which is also the only path the
// synth uses. One route, position in, one destination out.
static float bend(const ModRoute &r, float position) {
  ModContext ctx = {};
  ctx.position = position;
  float out[NUM_DESTS];
  modEvaluate(ctx, LfoConfig{0,0,0,0}, LfoConfig{0,0,0,0}, &r, 1, 1234u, out);
  return out[r.dest];
}

// Both bends are several rows that only mean anything together — a constant
// some interval flat, and detents that give it back — so the thing worth
// testing is the SUM, which is what the voice actually hears. Copied from
// Presets.cpp rather than included, because Presets.cpp needs Arduino.h; if
// these drift apart the tests are describing a patch nobody is playing.
static const ModRoute R_BEND[] = {
  { SRC_CONST,    DST_PITCH, -200.0f, SRC_CONST, CURVE_LINEAR, 0.0f, 0.0f },
  { SRC_POSITION, DST_PITCH,  200.0f, SRC_CONST, CURVE_DETENT, 0.20f, 0.40f },
};

// sarangi: the same idea made bipolar by a second detent that starts late.
static const ModRoute R_SLIDE[] = {
  { SRC_CONST,    DST_AMP,   -1.00f,  SRC_CONST, CURVE_LINEAR, 0.0f,  0.0f  },
  { SRC_POSITION, DST_AMP,    1.00f,  SRC_CONST, CURVE_DETENT, 0.15f, 0.10f },
  { SRC_CONST,    DST_PITCH, -200.0f, SRC_CONST, CURVE_LINEAR, 0.0f,  0.0f  },
  { SRC_POSITION, DST_PITCH,  200.0f, SRC_CONST, CURVE_DETENT, 0.15f, 0.30f },
  { SRC_POSITION, DST_PITCH,  200.0f, SRC_CONST, CURVE_DETENT, 0.65f, 0.35f },
};

static void evalAt(const ModRoute *r, int n, float position, float out[NUM_DESTS]) {
  ModContext ctx = {};
  ctx.position = position;
  modEvaluate(ctx, LfoConfig{0,0,0,0}, LfoConfig{0,0,0,0}, r, n, 1234u, out);
}

static float meend(float position) {
  float out[NUM_DESTS];
  evalAt(R_BEND, 2, position, out);
  return out[DST_PITCH];
}

static float slideCents(float position) {
  float out[NUM_DESTS];
  evalAt(R_SLIDE, 5, position, out);
  return out[DST_PITCH];
}

// DST_AMP is an OFFSET added to 1.0 by the synth, not the gain itself.
static float slideGain(float position) {
  float out[NUM_DESTS];
  evalAt(R_SLIDE, 5, position, out);
  return 1.0f + out[DST_AMP];
}

int main() {
  char buf[160];

  // ---- the bend meend actually plays --------------------------------------
  printf("\nmeend: born flat, pressed up to the prescribed pitch\n");
  {
    // THE ARRIVAL HAS TO BE EXACT, not close. "In tune below 0.60" means the
    // unmodulated pitch — the same float the note would have had with no
    // routes at all — and that is only true because the two depths are the
    // same number with opposite signs. A test at "within a cent" would pass
    // with 199 and hide the thing worth guarding.
    bool exact = true;
    for (float p = 0.60f; p <= 1.0f; p += 0.005f) if (meend(p) != 0.0f) exact = false;
    check(exact, "every depth from 0.60 to the key's own bottom is EXACTLY zero "
          "cents", "the two 200s cancel with no rounding");

    // Which is the answer to the complaint: a held chord is a chord. Fingers
    // resting anywhere in the deep half of the key are all on the same pitch.
    snprintf(buf, sizeof buf, "0.60 -> %.2f, 0.75 -> %.2f, 1.00 -> %.2f cents",
             meend(0.60f), meend(0.75f), meend(1.0f));
    check(meend(0.60f) == 0.0f && meend(1.0f) == 0.0f,
          "so a chord held down is in tune wherever the fingers sit", buf);

    // THE APPROACH, and this is the number MEEND_GESTURE exists to move. A note
    // is born at onPos and climbs from there, so onPos IS the patch's usable
    // bend range. At GESTURE_DEFAULT's 0.400 a pluck starts only a semitone
    // flat and the rest of the whole tone can be reached only by easing BACK
    // off a note already in tune — a slide down. 0.300 makes the whole gesture
    // press-inward.
    snprintf(buf, sizeof buf, "onPos 0.300 -> %.1f cents (the old 0.400 gave "
             "%.1f)", meend(0.30f), meend(0.40f));
    check(meend(0.30f) < -170.0f, "a fresh pluck starts within 30 cents of the "
          "full whole tone, all of it recoverable by pressing IN", buf);

    // offPos has to sit BELOW the detent edge or the deepest part of the bend
    // belongs to a note that has already been released. 0.150 against an edge
    // of 0.20 is 5% of travel — about four times the measured jitter.
    check(meend(0.15f) == meend(0.20f),
          "the bend bottoms out above offPos, not at it",
          "so the flattest pitch can be HELD rather than only crossed");

    // ...and it must be effectively home well BEFORE the stop, or a press that
    // falls a little short is a sour note. This is smootherstep's flat top
    // doing the work a linear ramp could not.
    snprintf(buf, sizeof buf, "0.50 -> %.1f, 0.55 -> %.1f cents",
             meend(0.50f), meend(0.55f));
    check(meend(0.55f) > -5.0f, "0.55 is within five cents — the note is home "
          "before the finger is", buf);

    // The full whole tone is reachable, and only just: offPos is 0.18, so the
    // bottom of the bend sits in a narrow band above the release point. If the
    // edge ever drifts below offPos the deepest part of the bend becomes a note
    // that has already ended.
    snprintf(buf, sizeof buf, "0.20 -> %.1f cents (offPos is 0.150)", meend(0.20f));
    check(fabsf(meend(0.20f) + 200.0f) < 0.01f,
          "the full whole tone is reachable by a note that is still sounding",
          buf);

    // Pressing must never lower the pitch. A bend that reversed anywhere would
    // read as a mistake in the playing rather than as an effect.
    bool mono = true;
    float prev = -1000.0f;
    for (float p = 0.0f; p <= 1.0f; p += 0.005f) {
      float v = meend(p);
      if (v < prev - 0.01f) mono = false;
      prev = v;
    }
    check(mono, "deeper is never flatter, over the whole travel");

    // And the direction, stated as bluntly as the patch comment does: the note
    // is under its pitch everywhere and reaches it, rather than starting there
    // and leaving.
    bool neverSharp = true;
    for (float p = 0.0f; p <= 1.0f; p += 0.005f) if (meend(p) > 0.0f) neverSharp = false;
    check(neverSharp, "the note is never sharp of its own pitch",
          "you bend UP to it, never away from it");
  }

  // ---- sarangi: the same bend made bipolar --------------------------------
  printf("\nsarangi: a whole tone either side, at one loudness\n");
  {
    // THE CENTRE IS EXACT, and it is the middle of the key rather than an end,
    // which is the only way a bend can go both directions. Both detents are
    // flat across this band — the first finished, the second not started — so
    // -200 + 200 + 0 cancels with nothing left over.
    //
    // The exactness comes from the CLAMP on t, so it is strict everywhere
    // strictly inside the plateau. The two boundary samples themselves are not
    // bit-exact: 0.45f - 0.15f divided by 0.30f is 0.99999989 rather than 1, so
    // a reading of exactly 0.45 is a ten-billionth of a cent flat. That is
    // tested separately rather than hidden, because "exact" is a claim worth
    // being able to trust literally.
    bool exact = true;
    for (float p = 0.455f; p <= 0.645f; p += 0.005f) if (slideCents(p) != 0.0f) exact = false;
    check(exact, "the interior of 0.45..0.65 is EXACTLY the prescribed pitch",
          "a fifth of the key is the note itself");

    snprintf(buf, sizeof buf, "0.45 -> %.3g cents, 0.65 -> %.3g cents",
             (double)slideCents(0.45f), (double)slideCents(0.65f));
    check(fabsf(slideCents(0.45f)) < 1e-4f && fabsf(slideCents(0.65f)) < 1e-4f,
          "and its edges are inside a rounding error of it", buf);

    // Down to a whole tone flat at the trigger, up to a whole tone sharp at the
    // key's own bottom. Both ends are mechanically or gesturally pinned.
    snprintf(buf, sizeof buf, "0.15 -> %.1f, 0.30 -> %.1f, 0.82 -> %.1f, "
             "1.00 -> %.1f cents", slideCents(0.15f), slideCents(0.30f),
             slideCents(0.825f), slideCents(1.0f));
    check(fabsf(slideCents(0.15f) + 200.0f) < 0.01f &&
          fabsf(slideCents(1.00f) - 200.0f) < 0.01f,
          "a whole tone below at the trigger, a whole tone above at the floor",
          buf);

    // A third of the travel plays in tune without being aimed at — smootherstep
    // approaching the centre plateau from both sides.
    int inTune = 0, total = 0;
    for (float p = 0.15f; p <= 1.0f; p += 0.005f) {
      total++;
      if (fabsf(slideCents(p)) <= 10.0f) inTune++;
    }
    snprintf(buf, sizeof buf, "%d%% of the sounding travel is within 10 cents",
             (100 * inTune) / total);
    check(inTune * 100 >= total * 30, "the in-tune band is wide enough to find "
          "without aiming", buf);

    // Monotonic across the whole key: deeper is always sharper, in both halves.
    bool mono = true;
    float prev = -1000.0f;
    for (float p = 0.0f; p <= 1.0f; p += 0.005f) {
      float v = slideCents(p);
      if (v < prev - 0.01f) mono = false;
      prev = v;
    }
    check(mono, "one direction the whole way down the key");

    // THE STABLE DYNAMIC, which is the claim that makes this different from
    // every other continuous patch: the level is not expressive, so a slide is
    // a slide and not a crescendo.
    bool level = true;
    for (float p = 0.255f; p <= 1.0f; p += 0.005f) if (slideGain(p) != 1.0f) level = false;
    snprintf(buf, sizeof buf, "0.15 -> %.2f, 0.20 -> %.2f, 0.26..1.00 -> %.2f",
             slideGain(0.15f), slideGain(0.20f), slideGain(0.60f));
    check(level, "every pitch the bend reaches is at EXACTLY the same level", buf);

    // Which covers the whole in-tune band and both extremes of the bend — the
    // level is settled well before the note is worth listening to.
    check(slideGain(0.26f) == 1.0f && slideGain(1.0f) == 1.0f,
          "the ramp is over before the music starts",
          "full level from 0.25, and the bend does not begin to matter until "
          "0.30");

    // ...but the onset is still the finger's. A continuous voice gets a 4 ms
    // envelope attack on the assumption that the patch draws its own; a patch
    // that held a constant here would arrive as a step.
    check(slideGain(0.15f) == 0.0f,
          "and the note still speaks from silence at the trigger",
          "the amp detent is the attack, compressed into 0.15..0.25");
  }

  // ---- the shape in isolation ---------------------------------------------
  printf("\nCURVE_DETENT is flat, then crosses, then is flat again\n");
  {
    const ModRoute R = { SRC_POSITION, DST_PITCH, 200.0f,
                         SRC_CONST, CURVE_DETENT, 0.20f, 0.40f };

    bool flatLo = true;
    for (float p = 0.0f; p <= 0.20f; p += 0.005f) if (fabsf(bend(R, p)) > 0.001f) flatLo = false;
    check(flatLo, "below the edge it is exactly zero");

    bool flatHi = true;
    for (float p = 0.60f; p <= 1.0f; p += 0.005f) if (bend(R, p) != 200.0f) flatHi = false;
    check(flatHi, "above edge+span it is exactly the depth",
          "both flats are exact, which is what lets two routes cancel");

    // Smootherstep, not a ramp: the crossing is symmetric about its midpoint
    // and leaves both ends with zero slope.
    snprintf(buf, sizeof buf, "0.40 (midpoint) -> %.1f of 200", bend(R, 0.40f));
    check(fabsf(bend(R, 0.40f) - 100.0f) < 0.01f, "half way across is half way "
          "up", buf);
  }

  // ---- the failure it replaces --------------------------------------------
  // Not a test of the detent so much as of why there is one: the power curves
  // cannot produce a flat anywhere at any exponent, so this is the pitch error
  // the old patch was asking a player to hold a finger still inside.
  printf("\nno power curve can do this, which is why the curve exists\n");
  {
    const ModRoute cube = { SRC_POSITION, DST_PITCH, 200.0f,
                            SRC_CONST, CURVE_LINEAR, 3.0f, 0.0f };
    const ModRoute steep = { SRC_POSITION, DST_PITCH, 200.0f,
                             SRC_CONST, CURVE_LINEAR, 12.0f, 0.0f };
    snprintf(buf, sizeof buf, "x^3 at 0.70 -> %.1f cents; x^12 at 0.95 -> %.1f",
             bend(cube, 0.70f), bend(steep, 0.95f));
    check(bend(cube, 0.70f) > 60.0f && bend(steep, 0.95f) > 60.0f,
          "every exponent leaves some landing depth badly out of tune", buf);
  }

  // ---- span defaults and guards -------------------------------------------
  // A span of zero is what a route table written before the field existed
  // leaves behind, and it has to mean the old one-breakpoint shape rather than
  // a zero-width step — a step in pitch is a click.
  printf("\nspan 0 means the crossing runs to full travel\n");
  {
    const ModRoute R = { SRC_POSITION, DST_PITCH, 200.0f,
                         SRC_CONST, CURVE_DETENT, 0.80f, 0.0f };
    snprintf(buf, sizeof buf, "0.90 -> %.1f cents (half way), 1.00 -> %.1f",
             bend(R, 0.90f), bend(R, 1.0f));
    check(fabsf(bend(R, 0.90f) - 100.0f) < 0.5f && bend(R, 1.0f) > 199.9f,
          "the crossing spans edge..1.0 and is half done at the midpoint", buf);
  }

  // A detent edge is a position, and the ways to mistype one all used to reach
  // the audio path: 1.0 divides by zero, 0.0 is the default an older route
  // table leaves behind, and a negative span is a sign error.
  printf("\na mistyped edge or span must not produce a NaN\n");
  {
    struct { float edge, span; } bad[] = {
      { 0.0f, 0.14f }, { 1.0f, 0.14f }, { -0.5f, 0.14f }, { 2.0f, 0.14f },
      { 0.8f, -0.1f }, { 0.8f, 5.0f },  { 0.99f, 0.9f },
    };
    for (auto &b : bad) {
      const ModRoute R = { SRC_POSITION, DST_PITCH, 200.0f,
                           SRC_CONST, CURVE_DETENT, b.edge, b.span };
      bool ok = true;
      for (float p = 0.0f; p <= 1.0f; p += 0.02f) {
        float v = bend(R, p);
        if (!std::isfinite(v) || v < -0.01f || v > 200.01f) ok = false;
      }
      snprintf(buf, sizeof buf, "edge %.2f span %.2f", b.edge, b.span);
      check(ok, "falls back to something finite and in range", buf);
    }
  }

  // ---- the other curves are untouched -------------------------------------
  // CURVE_DETENT is tested before the shape override in applyCurve, which is a
  // change to a branch every route in Presets.cpp goes through.
  printf("\nthe existing curves still mean what they meant\n");
  {
    struct { uint8_t curve; float shape; float at5; const char *name; } cases[] = {
      { CURVE_LINEAR, 0.0f, 0.500f, "linear" },
      { CURVE_SQUARE, 0.0f, 0.250f, "square" },
      { CURVE_AUDIO,  0.0f, 0.287f, "audio"  },
      { CURVE_SQRT,   0.0f, 0.707f, "sqrt"   },
      { CURVE_LATE,   0.0f, 0.125f, "late"   },
      { CURVE_LINEAR, 3.0f, 0.125f, "shape 3 overrides linear" },
      { CURVE_SQUARE, 3.0f, 0.125f, "shape 3 overrides the enum curve" },
    };
    for (auto &c : cases) {
      const ModRoute R = { SRC_POSITION, DST_AMP, 1.0f, SRC_CONST, c.curve,
                           c.shape, 0.0f };
      float v = bend(R, 0.5f);
      snprintf(buf, sizeof buf, "%s at 0.5 -> %.3f, expected %.3f", c.name, v, c.at5);
      check(fabsf(v - c.at5) < 0.002f, c.name, buf);
    }
  }

  // A bipolar source keeps its sign through a curve — the property that stops
  // a curved LFO turning into a rectifier. The detent is a curve like any
  // other here and must not be the exception.
  printf("\nbipolar sources stay symmetric through the detent\n");
  {
    const ModRoute R = { SRC_RANDOM, DST_PITCH, 100.0f,
                         SRC_CONST, CURVE_DETENT, 0.5f, 0.4f };
    ModContext up = {}, dn = {};
    // 0.7 is the midpoint of a 0.5..0.9 crossing, so this lands on the steep
    // part rather than on a plateau where the two sides would match trivially.
    up.randomv =  0.7f;
    dn.randomv = -0.7f;
    float o1[NUM_DESTS], o2[NUM_DESTS];
    modEvaluate(up, LfoConfig{0,0,0,0}, LfoConfig{0,0,0,0}, &R, 1, 1u, o1);
    modEvaluate(dn, LfoConfig{0,0,0,0}, LfoConfig{0,0,0,0}, &R, 1, 1u, o2);
    snprintf(buf, sizeof buf, "+0.7 -> %.2f, -0.7 -> %.2f", o1[DST_PITCH], o2[DST_PITCH]);
    check(fabsf(o1[DST_PITCH] + o2[DST_PITCH]) < 1e-4f && o1[DST_PITCH] > 0.0f,
          "equal and opposite, not rectified", buf);
  }

  printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
#endif
