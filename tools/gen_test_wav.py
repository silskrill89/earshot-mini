#!/usr/bin/env python3
"""Generate the Earshot_reminid placeholder gunshot sample.

Produces assets/test/gunshot_test_mono.wav:
  44100 Hz, MONO, 16-bit PCM, ~180 ms.

Sound design (kept deliberately simple - this is a *test* stem until real
EarShot wav stems land):
  * band-limited noise burst (one-pole LPF @ ~5.5 kHz + one-pole HPF @ ~120 Hz)
    with a 1.5 ms attack and exponential decay  -> the "crack",
  * a ~80 Hz sine thump with its own (slower) exponential decay
    -> the low-frequency body so it reads as a gunshot, not a click.

Stdlib only (wave / struct / math / random). Run:

    python tools/gen_test_wav.py

Kept in-tree: this is our asset generator until the real stems arrive.
"""

import math
import os
import random
import struct
import wave

SR = 44100            # Hz
DURATION = 0.18       # seconds (~180 ms)
N = int(SR * DURATION)

# --- noise channel -----------------------------------------------------------
NOISE_LP_FC = 5500.0  # one-pole low-pass cutoff  (band-limit the top end)
NOISE_HP_FC = 120.0   # one-pole high-pass cutoff (kill DC / infrasonic rumble)
NOISE_TAU = 0.035     # exponential decay time constant of the crack
ATTACK = 0.0015       # 1.5 ms attack ramp - avoids a click onset

# --- thump channel -----------------------------------------------------------
THUMP_FREQ = 80.0     # Hz (task: low ~70-90 Hz sine thump)
THUMP_TAU = 0.045     # slower decay than the noise -> body lingers
THUMP_AMP = 0.55      # relative to the noise burst


def one_pole_lp(x, fc, sr):
    """One-pole low-pass; returns a list (two cascaded calls = steeper)."""
    a = math.exp(-2.0 * math.pi * fc / sr)
    y = 0.0
    out = []
    for s in x:
        y = (1.0 - a) * s + a * y
        out.append(y)
    return out


def one_pole_hp(x, fc, sr):
    """One-pole high-pass = x - one_pole_lp(x)."""
    lp = one_pole_lp(x, fc, sr)
    return [s - l for s, l in zip(x, lp)]


def build_samples():
    rng = random.Random(0xEA75)  # deterministic output - reproducible test asset

    # 1) white noise -> band-limited
    white = [rng.uniform(-1.0, 1.0) for _ in range(N)]
    noise = one_pole_lp(one_pole_lp(white, NOISE_LP_FC, SR), NOISE_LP_FC, SR)
    noise = one_pole_hp(noise, NOISE_HP_FC, SR)

    # normalize the noise burst so later stages are level-independent
    peak = max(abs(v) for v in noise) or 1.0
    noise = [v / peak for v in noise]

    # 2) low sine thump
    thump = [
        THUMP_AMP * math.sin(2.0 * math.pi * THUMP_FREQ * (i / SR))
        for i in range(N)
    ]

    # 3) envelopes: fast attack + exponential decay
    out = []
    for i in range(N):
        t = i / SR
        attack = min(1.0, t / ATTACK) if ATTACK > 0 else 1.0
        env_noise = attack * math.exp(-t / NOISE_TAU)
        env_thump = attack * math.exp(-t / THUMP_TAU)
        s = 0.62 * noise[i] * env_noise + thump[i] * env_thump
        out.append(s)

    # 4) fade the last 3 ms to zero (no end-of-buffer click), then normalize
    fade = int(0.003 * SR)
    for i in range(fade):
        out[N - 1 - i] *= i / fade

    peak = max(abs(v) for v in out) or 1.0
    gain = 0.92 / peak  # headroom, no clipping
    return [int(round(max(-1.0, min(1.0, v * gain)) * 32767.0)) for v in out]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    out_dir = os.path.join(repo, "assets", "test")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "gunshot_test_mono.wav")

    samples = build_samples()
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)        # MONO - required for 3D spatialization
        w.setsampwidth(2)        # 16-bit
        w.setframerate(SR)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))

    # Verify what we just wrote (task: prove mono via the wave module)
    with wave.open(out_path, "rb") as w:
        nch, sw, sr, nframes = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    assert nch == 1, "expected MONO, got %d channels" % nch
    assert sw == 2, "expected 16-bit, got %d bytes/sample" % sw
    assert sr == SR, "expected %d Hz, got %d" % (SR, sr)
    print("wrote %s" % out_path)
    print("  channels=%d  samplewidth=%d  rate=%d  frames=%d  dur=%.1fms  bytes=%d"
          % (nch, sw, sr, nframes, 1000.0 * nframes / sr, os.path.getsize(out_path)))


if __name__ == "__main__":
    main()
