"""Generate the miss/overstrum SFX: original synthesized fret-buzz one-shots.

Three variants -> dist/SKSE/Plugins/BardHero/sfx/miss{1,2,3}.wav
(48 kHz mono PCM16, ~0.25 s). Recipe per variant: a band-shaped noise burst
(the string scratch/buzz) + a damped downward thud (the muted low string),
soft-clipped and normalized to -6 dBFS. All parameters jittered per variant
so rapid misses don't machine-gun one sample. Entirely synthesized - no
GH/CH audio anywhere near this (spec 12).
"""

import os
import wave

import numpy as np

SR = 48000
DUR = 0.25
OUT = os.path.join(os.path.dirname(__file__), "..", "dist", "SKSE",
                   "Plugins", "BardHero", "sfx")

VARIANTS = [  # (seed, band_lo, band_hi, thud_f0, thud_f1)
    (11, 500.0, 3000.0, 150.0, 95.0),
    (23, 650.0, 3600.0, 135.0, 88.0),
    (37, 420.0, 2500.0, 165.0, 102.0),
]


def band_noise(rng, n, lo, hi):
    """White noise shaped to [lo,hi] Hz via an FFT mask with soft skirts."""
    x = rng.standard_normal(n).astype(np.float64)
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    mask = np.zeros_like(freqs)
    inband = (freqs >= lo) & (freqs <= hi)
    mask[inband] = 1.0
    # soft skirts (one octave) so the burst doesn't ring metallically
    below = (freqs < lo) & (freqs >= lo / 2)
    above = (freqs > hi) & (freqs <= hi * 2)
    mask[below] = ((freqs[below] - lo / 2) / (lo / 2)) ** 2
    mask[above] = (1.0 - (freqs[above] - hi) / hi) ** 2
    return np.fft.irfft(spec * mask, n)


def main():
    os.makedirs(OUT, exist_ok=True)
    n = int(SR * DUR)
    t = np.arange(n) / SR
    for i, (seed, lo, hi, f0, f1) in enumerate(VARIANTS, start=1):
        rng = np.random.default_rng(seed)
        # buzz: sharp attack, ~40 ms decay, light amplitude flutter (string
        # rattling against the fret)
        buzz = band_noise(rng, n, lo, hi)
        buzz *= np.exp(-t / 0.040)
        buzz *= 1.0 + 0.35 * np.sin(2 * np.pi * 31.0 * t + seed)
        buzz /= np.max(np.abs(buzz)) + 1e-12
        # thud: damped sine sweeping f0 -> f1 over the tail, ~60 ms decay
        sweep = f0 + (f1 - f0) * (t / DUR)
        phase = 2 * np.pi * np.cumsum(sweep) / SR
        thud = np.sin(phase) * np.exp(-t / 0.060)
        # mix, soft-clip, normalize to -6 dBFS
        x = 0.7 * buzz + 0.8 * thud
        x = np.tanh(1.6 * x)
        x *= 0.501 / (np.max(np.abs(x)) + 1e-12)
        # 5 ms fade-in/out so the one-shot never clicks
        f = int(SR * 0.005)
        x[:f] *= np.linspace(0.0, 1.0, f)
        x[-f:] *= np.linspace(1.0, 0.0, f)
        pcm = (x * 32767.0).astype(np.int16)
        path = os.path.join(OUT, f"miss{i}.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(pcm.tobytes())
        print(f"wrote {path} ({n} frames, band {lo:.0f}-{hi:.0f} Hz, "
              f"thud {f0:.0f}->{f1:.0f} Hz)")


if __name__ == "__main__":
    main()
