"""Generate PLACEHOLDER crowd reaction one-shots.

Five files -> dist/SKSE/Plugins/BardHero/sfx/crowd/{cheer,groan,swell,
applause,awkward}.wav (48 kHz mono PCM16). The session loads them BY NAME in
CrowdReactions::Kind order, so the names here are the contract.

*** THESE ARE PLACEHOLDERS, NOT SHIPPABLE AUDIO. ***

A fret buzz (tools/gen_miss_sfx.py) is a physical event a few oscillators can
actually be; a room full of people is not. What comes out of this script is
shaped noise with a crowd-ish envelope: enough for a field tester to tell one
reaction from another and for the code path to be exercised end to end, and
nowhere near good enough to ship. Replace all five with real recorded samples
(or licensed library audio) before release - the loader tolerates any of them
being absent, so they can be swapped one at a time.

The five are deliberately distinguishable by LENGTH, BRIGHTNESS and ENVELOPE
so a tester can name which one fired from the sound alone:

  cheer     0.9 s  bright, fast swell, quick decay      (a whoop)
  groan     1.1 s  dark, slow sag, falling pitch        (a collective ugh)
  swell     1.6 s  mid, long slow rise, no transient    (a room warming up)
  applause  2.2 s  bright, dense clap grains, long tail (the payoff)
  awkward   1.8 s  near-silent, two lone sparse claps   (nobody clapped)

All synthesized from noise and sine - no sampled audio of any kind (spec 12).
"""

import os
import wave

import numpy as np

SR = 48000
OUT = os.path.join(os.path.dirname(__file__), "..", "dist", "SKSE",
                   "Plugins", "BardHero", "sfx", "crowd")

# name, seed, duration, band lo/hi Hz, peak (linear, -12 dBFS = 0.25):
# reactions sit UNDER the song, so they are quieter than the miss sfx.
VARIANTS = [
    ("cheer",    101, 0.90, 400.0, 3800.0, 0.25),
    ("groan",    202, 1.10, 120.0,  900.0, 0.22),
    ("swell",    303, 1.60, 250.0, 2000.0, 0.18),
    ("applause", 404, 2.20, 700.0, 6000.0, 0.28),
    ("awkward",  505, 1.80, 600.0, 5000.0, 0.12),
]


def band_noise(rng, n, lo, hi):
    """White noise shaped to [lo,hi] Hz via an FFT mask with soft skirts."""
    x = rng.standard_normal(n).astype(np.float64)
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    mask = np.zeros_like(freqs)
    mask[(freqs >= lo) & (freqs <= hi)] = 1.0
    below = (freqs < lo) & (freqs >= lo / 2)
    above = (freqs > hi) & (freqs <= hi * 2)
    mask[below] = ((freqs[below] - lo / 2) / (lo / 2)) ** 2
    mask[above] = (1.0 - (freqs[above] - hi) / hi) ** 2
    return np.fft.irfft(spec * mask, n)


def clap_grains(rng, n, count, spread, decay, jitter=True):
    """`count` short noise bursts scattered over `spread` of the buffer."""
    out = np.zeros(n)
    for _ in range(count):
        at = int(rng.uniform(0.0, spread) * n)
        length = min(int(SR * decay * 3), n - at)
        if length <= 0:
            continue
        t = np.arange(length) / SR
        g = rng.standard_normal(length) * np.exp(-t / decay)
        if jitter:
            g *= rng.uniform(0.5, 1.0)
        out[at:at + length] += g
    return out


def envelope(t, dur, kind):
    """The per-reaction amplitude shape - the main thing telling them apart."""
    if kind == "cheer":       # fast swell, quick fall
        return np.minimum(1.0, t / 0.12) * np.exp(-np.maximum(t - 0.12, 0) / 0.30)
    if kind == "groan":       # slow sag
        return np.minimum(1.0, t / 0.25) * np.exp(-np.maximum(t - 0.25, 0) / 0.55)
    if kind == "swell":       # long rise, no transient at all
        return (t / dur) ** 1.5
    if kind == "applause":    # burst then a long tail
        return np.minimum(1.0, t / 0.08) * np.exp(-np.maximum(t - 0.08, 0) / 1.1)
    return np.ones_like(t)    # awkward: the grains carry their own shape


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, seed, dur, lo, hi, peak in VARIANTS:
        rng = np.random.default_rng(seed)
        n = int(SR * dur)
        t = np.arange(n) / SR
        body = band_noise(rng, n, lo, hi)
        body /= np.max(np.abs(body)) + 1e-12
        if name == "applause":
            # dense grains ARE the applause; the noise bed is just glue
            x = 0.25 * body + 1.0 * clap_grains(rng, n, 90, 0.55, 0.010)
        elif name == "awkward":
            # the joke is the emptiness: two lone claps and a lot of nothing
            x = 0.05 * body + 1.0 * clap_grains(rng, n, 2, 0.45, 0.014,
                                                jitter=False)
        elif name == "groan":
            # a falling hum under the noise reads as disapproval
            sweep = 190.0 - 70.0 * (t / dur)
            x = 0.6 * body + 0.7 * np.sin(2 * np.pi * np.cumsum(sweep) / SR)
        else:
            x = body
        x *= envelope(t, dur, name)
        # slow amplitude wobble: a crowd is many voices, never one steady one
        x *= 1.0 + 0.20 * np.sin(2 * np.pi * 5.5 * t + seed)
        x = np.tanh(1.3 * x)
        x *= peak / (np.max(np.abs(x)) + 1e-12)
        # 8 ms fade-in/out so the one-shot never clicks
        f = int(SR * 0.008)
        x[:f] *= np.linspace(0.0, 1.0, f)
        x[-f:] *= np.linspace(1.0, 0.0, f)
        pcm = (x * 32767.0).astype(np.int16)
        path = os.path.join(OUT, f"{name}.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm.tobytes())
        print(f"wrote {path} ({dur:.2f}s, band {lo:.0f}-{hi:.0f} Hz, "
              f"peak {peak:.2f}) [PLACEHOLDER]")


if __name__ == "__main__":
    main()
