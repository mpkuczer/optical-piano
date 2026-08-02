#!/usr/bin/env python3
"""
surge2teensy.py — convert a Surge XT patch (.fxp) into a Lego piano preset.

    ./surge2teensy.py "/usr/share/surge-xt/patches_factory/Keys/Circus 2.fxp"
    ./surge2teensy.py somedir/*.fxp

The important change from the first version: it now REPORTS what it cannot
carry across, as // comments above each preset. The old version silently
emitted a lowpass preset for a highpass patch, which is why Circus 2 came out
sounding like nothing at all. A warning you can read beats a preset that lies.
"""
import re, sys, os

OSC_CLASSIC, OSC_SINE = 0, 1

# Surge filter type ids (revision 9 ordering) -> name and our LP/BP/HP blend
FILTER_NAMES = {
    0: ('off',    (1.0, 0.0, 0.0)),
    1: ('LP12',   (1.0, 0.0, 0.0)),
    2: ('LP24',   (1.0, 0.0, 0.0)),
    3: ('ladder', (1.0, 0.0, 0.0)),
    4: ('HP12',   (0.0, 0.0, 1.0)),
    5: ('HP24',   (0.0, 0.0, 1.0)),
    6: ('BP12',   (0.0, 1.0, 0.0)),
    7: ('notch',  (0.5, 0.0, 0.5)),
    8: ('comb',   (1.0, 0.0, 0.0)),
    9: ('S&H',    (1.0, 0.0, 0.0)),
}


def val(x, name, default=0.0):
    m = re.search(name + r' type="(\d+)" value="([-0-9.]+)"', x)
    return float(m.group(2)) if m else default


def secs(v):
    """Surge stores envelope times as log2(seconds)."""
    return 2.0 ** v


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def wave_for(x, n, warn):
    t = val(x, f'a_osc{n}_type', 0)
    if t == OSC_SINE:
        return 'WAVEFORM_SINE'
    if t == OSC_CLASSIC:
        # Classic morphs continuously from saw (-1) to pulse (+1). We can only
        # pick a corner, so anything in between loses its blend.
        shape = val(x, f'a_osc{n}_param0', -1.0)
        if shape < -0.55:
            return 'WAVEFORM_SAWTOOTH'
        if shape > 0.5:
            return 'WAVEFORM_PULSE'
        warn.append(f'osc{n} is a saw/pulse blend (shape {shape:+.2f}); '
                    'approximated as square')
        return 'WAVEFORM_SQUARE'
    warn.append(f'osc{n} type {int(t)} (wavetable/FM/string/noise) has no '
                'equivalent; substituted sawtooth')
    return 'WAVEFORM_SAWTOOTH'


def convert(path):
    raw = open(path, 'rb').read()
    i = raw.find(b'<patch')
    if i < 0:
        return None
    x = raw[i:].decode('utf-8', 'replace')
    warn = []

    title = (re.search(r'<meta name="([^"]*)"', x) or [None, '?'])[1]
    name = re.sub(r'[^A-Za-z0-9]+', '', title).lower()[:9] or 'patch'

    waveA, waveB = wave_for(x, 1, warn), wave_for(x, 2, warn)

    # ---- pitch and detune ------------------------------------------------
    p1 = val(x, 'a_osc1_octave') * 12 + val(x, 'a_osc1_pitch')
    p2 = val(x, 'a_osc2_octave') * 12 + val(x, 'a_osc2_pitch')
    delta = p2 - p1
    semiB = int(round(delta))
    cents = clamp(abs(delta - semiB) * 100 + val(x, 'a_osc1_param5') * 100,
                  0.0, 30.0)

    lv1 = max(val(x, 'a_level_o1', 1.0), 1e-3)
    mixB = clamp(val(x, 'a_level_o2', 0.0) / lv1, 0.0, 1.0)

    # ---- envelopes -------------------------------------------------------
    # env1 is the AMPLITUDE envelope, env2 the filter one. The first version
    # of this script had them the other way round, which gave every patch the
    # wrong shape — sustaining patches came out plucky. If a converted preset
    # decays when the original holds, suspect this pair first.
    atk = clamp(secs(val(x, 'a_env1_attack', -8)) * 1000, 1, 1500)
    dec = secs(val(x, 'a_env1_decay', 0)) * 1000
    sus = val(x, 'a_env1_sustain', 1.0)
    rel = clamp(secs(val(x, 'a_env1_release', -2)) * 1000, 20, 2500)
    tau = '1e9f' if sus > 0.45 else f'{clamp(dec * 0.5, 80, 4000):.0f}'

    # ---- filter ----------------------------------------------------------
    ftype = int(val(x, 'a_filter1_type', 1))
    fname, (lp, bp, hp) = FILTER_NAMES.get(ftype, ('?', (1.0, 0.0, 0.0)))
    if ftype in (2, 5):
        warn.append(f'{fname} is 4-pole; ours is 2-pole, so it rolls off half '
                    'as steeply and will sound less closed')
    if ftype in (3, 8, 9):
        warn.append(f'filter type {fname} has no equivalent; using lowpass')

    cut_hz = 440.0 * (2.0 ** (val(x, 'a_filter1_cutoff', 20) / 12.0))
    fBase = clamp(cut_hz / 261.6, 1.0, 60.0)
    envmod = val(x, 'a_filter1_envmod', 0)
    fPeak = clamp(fBase * (2.0 ** (envmod / 12.0)), fBase, 60.0)
    fTau = clamp(secs(val(x, 'a_env2_decay', 0)) * 1000 * 0.5, 80, 2000)
    # Surge resonance is normalised 0..1, NOT a Q value. Mapping 1.0 onto our
    # filter's 4.2 puts it near self-oscillation and everything sounds nasal.
    res = clamp(0.7 + val(x, 'a_filter1_resonance', 0) * 1.6, 0.7, 2.6)

    if val(x, 'a_filter2_type', 0) > 0:
        warn.append('patch uses a SECOND filter in series; we have one')

    # ---- waveshaper ------------------------------------------------------
    drive = 0.0
    if int(val(x, 'a_ws_type', 0)) > 0:
        # a_ws_drive is in DECIBELS. 1.5 dB is a hint of saturation, not
        # distortion. Dividing by 5 turned gentle patches into brass.
        drive = clamp(val(x, 'a_ws_drive', 0) / 24.0, 0.0, 0.6)
        warn.append('waveshaper is per-voice in Surge but shared on the bus '
                    'here, so chords will saturate more than single notes')

    # ---- effects ---------------------------------------------------------
    send1 = val(x, 'a_send_fx_1', 0.0)
    types = {n: int(val(x, f'fx{n}_type', 0)) for n in range(1, 9)}
    delay_slot = next((n for n, t in types.items() if t == 1), None)
    has_verb = any(t in (2, 11) for t in types.values())

    verb = round(clamp(send1 * 0.5 if has_verb else 0.20, 0.0, 0.6), 2)
    dly = dly_ms = dly_fb = 0.0
    if delay_slot:
        dly = round(clamp(send1, 0.0, 0.7), 2)
        # Param 2 of Surge's delay is feedback. A high value is often what
        # makes a short, plucky patch SEEM to sustain — read it, don't guess.
        dly_fb = round(clamp(val(x, f'fx{delay_slot}_p2', 0.35), 0.0, 0.85), 2)
        synced = f'fx{delay_slot}_p0 type="2" value="[-0-9.]+" temposync="1"'
        dly_ms = 250
        warn.append('delay time is tempo-synced in Surge; using a fixed '
                    f'{dly_ms} ms — adjust by ear'
                    if re.search(synced, x) else
                    f'delay time approximated as {dly_ms} ms')
        if dly_fb > 0.55:
            warn.append(f'delay feedback is high ({dly_fb:.2f}) — much of the '
                        'apparent sustain comes from repeats, not the envelope')

    # ---- things with no home at all --------------------------------------
    if val(x, 'a_osc1_param4', 0) > 0.5:
        warn.append('oscillator SYNC is used; no equivalent, dropped')
    if int(val(x, 'a_fm_switch', 0)) > 0:
        warn.append('FM between oscillators is used; dropped')
    if val(x, 'a_level_ring12', 0) > 0 and int(val(x, 'a_mute_ring12', 1)) == 0:
        warn.append('ring modulation is used; dropped')
    if val(x, 'a_level_noise', 0) > 0 and int(val(x, 'a_mute_noise', 1)) == 0:
        warn.append('noise oscillator is used; we have none, dropped')
    if val(x, 'a_level_o3', 0) > 0 and int(val(x, 'a_mute_o3', 1)) == 0:
        warn.append('third oscillator is used; we have two, dropped')
    # ---- vibrato: an LFO wired to pitch ----------------------------------
    vib_c = vib_hz = vib_dly = 0.0
    mp = re.search(r'<a_pitch[^>]*>\s*<modrouting source="(\d+)" '
                   r'depth="([-0-9.]+)"', x)
    if mp:
        depth = abs(float(mp.group(2)))
        # Scene pitch spans +/-7 semitones, so depth is a fraction of that.
        vib_c = round(clamp(depth * 700.0, 0.0, 80.0), 1)
        # Find the fastest configured LFO and assume that's the vibrato one.
        rates = [val(x, f'a_lfo{n}_rate', -99) for n in range(12)]
        best = max(r for r in rates if r is not None)
        vib_hz = round(clamp(2.0 ** best, 0.5, 12.0), 2)
        vib_dly = 120.0
        warn.append(f'vibrato: LFO->pitch, ~{vib_c:.0f} cents at {vib_hz} Hz '
                    '(fade-in time is a guess)')

    nmod = len(re.findall(r'<modrouting ', x))
    if nmod:
        warn.append(f'{nmod} modulation routing(s) (LFO or envelope to some '
                    'destination); only pulse-width modulation exists here')

    line = (f'{{ "{name}",{" " * max(0, 9 - len(name))} {waveA:<18} {waveB:<18} '
            f'{semiB:>4}, {cents:5.1f}, {mixB:4.2f},\n'
            f'    {atk:5.0f}, {tau:>6}, {rel:5.0f}, 0.50,   '
            f'{fPeak:5.1f},{fBase:5.1f}, {fTau:4.0f}, {res:4.2f}, 0.45,\n'
            f'    {lp:4.2f}, {bp:4.2f}, {hp:4.2f},   0.00, 0.00, '
            f'{vib_c:5.1f}, {vib_hz:4.2f}, {vib_dly:5.1f},\n    '
            f'{drive:4.2f}, {verb:4.2f}, {dly:4.2f}, {int(dly_ms):3d}, {dly_fb:5.2f} }},')
    return line, warn, fname, title


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        try:
            out = convert(p)
            if out is None:
                print(f'// {os.path.basename(p)}: not a Surge patch\n')
                continue
            line, warn, fname, title = out
            print(f'// ---- {title}   [Surge filter: {fname}] ' + '-' * 18)
            for w in warn:
                print(f'//   ~ {w}')
            print(line)
            print()
        except Exception as e:
            print(f'// {os.path.basename(p)}: {e}\n')
