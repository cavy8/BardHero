# tools/gen_test_song.py
# Builds the test song folder: a real SongEntry (notes.chart + song.ini +
# 4 stems) for in-game field runs. M3 rework: the M2 version copied the
# spike's bare sine blips ("strange music" - field feedback) and charted
# cycling GRYBO lanes, which is unplayable blind (no highway until M4).
# Now the stems are an actual 120 BPM A-minor groove (drums/bass/melody/pad,
# numpy-synthesized, ffmpeg-encoded to vorbis ogg) and the chart is all
# GREEN quarters: hold fret 1 + strum on the beat scores. Regenerate a
# lane-varied chart when the M4 highway lands.
import math
import os
import shutil
import subprocess
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, "..", "dist", "SKSE", "Plugins", "BardHero", "songs",
                   "SkyHero Test Song")
os.makedirs(OUT, exist_ok=True)

RATE = 48000
DUR = 90.0            # seconds; 180 beats @ 120 BPM
BPM = 120.0
BEAT = 60.0 / BPM     # 0.5s
N = int(RATE * DUR)
T = np.arange(N) / RATE

# Am - F - C - G, one chord per bar (4 beats), the classic loop.
CHORDS = [
    {"bass": 110.00, "gtr": [440.00, 523.25, 659.25, 880.00],
     "pad": [220.00, 261.63, 329.63]},                          # Am
    {"bass": 87.31,  "gtr": [349.23, 440.00, 523.25, 698.46],
     "pad": [174.61, 220.00, 261.63]},                          # F
    {"bass": 130.81, "gtr": [523.25, 659.25, 783.99, 1046.50],
     "pad": [261.63, 329.63, 392.00]},                          # C
    {"bass": 98.00,  "gtr": [392.00, 493.88, 587.33, 783.99],
     "pad": [196.00, 246.94, 293.66]},                          # G
]


def place(buf, start_s, sig):
    i = int(start_s * RATE)
    if i >= N:
        return
    j = min(N, i + len(sig))
    buf[i:j] += sig[: j - i]


def pluck(freq, dur=0.45, amp=0.30):
    t = np.arange(int(RATE * dur)) / RATE
    env = np.exp(-t * 6.0) * np.minimum(1.0, t * 400.0)
    return amp * env * (np.sin(2 * np.pi * freq * t)
                        + 0.5 * np.sin(4 * np.pi * freq * t)
                        + 0.25 * np.sin(6 * np.pi * freq * t))


def bass_note(freq, dur=0.22, amp=0.32):
    t = np.arange(int(RATE * dur)) / RATE
    env = np.exp(-t * 8.0) * np.minimum(1.0, t * 300.0)
    return amp * env * (np.sin(2 * np.pi * freq * t)
                        + 0.4 * np.sin(4 * np.pi * freq * t))


def kick(amp=0.55):
    t = np.arange(int(RATE * 0.13)) / RATE
    f = 110.0 * np.exp(-t * 26.0) + 42.0
    return amp * np.exp(-t * 22.0) * np.sin(2 * np.pi * np.cumsum(f) / RATE)


def snare(amp=0.30):
    t = np.arange(int(RATE * 0.16)) / RATE
    rng = np.random.default_rng(7)  # deterministic asset
    noise = rng.standard_normal(len(t))
    return amp * np.exp(-t * 24.0) * (0.7 * noise
                                      + 0.5 * np.sin(2 * np.pi * 185.0 * t))


def hat(amp=0.10):
    t = np.arange(int(RATE * 0.045)) / RATE
    rng = np.random.default_rng(11)
    noise = np.diff(rng.standard_normal(len(t) + 1))  # crude highpass
    return amp * np.exp(-t * 60.0) * noise


guitar = np.zeros(N)
bass = np.zeros(N)
drums = np.zeros(N)
pad = np.zeros(N)

total_beats = int(DUR / BEAT)          # 180
for b in range(total_beats):
    t0 = b * BEAT
    bar = b // 4
    ch = CHORDS[bar % 4]
    # guitar: one melody pluck on EVERY beat - the strum cue
    place(guitar, t0, pluck(ch["gtr"][b % 4]))
    # bass: root eighths, accent on the downbeat
    place(bass, t0, bass_note(ch["bass"], amp=0.36 if b % 4 == 0 else 0.28))
    place(bass, t0 + BEAT / 2, bass_note(ch["bass"], amp=0.22))
    # drums: kick 1+3, snare 2+4, hats on eighths
    place(drums, t0, kick() if b % 2 == 0 else snare())
    place(drums, t0, hat())
    place(drums, t0 + BEAT / 2, hat())

for bar in range(int(total_beats / 4)):
    ch = CHORDS[bar % 4]
    t = np.arange(int(RATE * BEAT * 4)) / RATE
    env = np.minimum(1.0, t * 2.0) * np.minimum(
        1.0, np.maximum(0.0, (BEAT * 4 - t) * 2.0))
    sig = sum(np.sin(2 * np.pi * f * t) for f in ch["pad"])
    place(pad, bar * BEAT * 4, 0.05 * env * sig)


def write_stem(name, mono):
    peak = np.max(np.abs(mono))
    if peak > 0:
        mono = mono * (0.5 / peak)
    pcm = (mono * 32767.0).astype(np.int16)
    stereo = np.column_stack([pcm, pcm]).ravel()
    tmp = os.path.join(tempfile.gettempdir(), name + ".wav")
    with wave.open(tmp, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(stereo.tobytes())
    ogg = os.path.join(OUT, name + ".ogg")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", tmp,
                    "-c:a", "libvorbis", "-q:a", "5", ogg], check=True)
    os.remove(tmp)
    print("wrote", ogg)


write_stem("guitar", guitar)
write_stem("bass", bass)
write_stem("drums", drums)
write_stem("song", pad)

# Chart (M4 highway edition, handoff item "restore lane variety"): 176
# beats. 8-bar sections: A all-green anchor (blind-friendly start), B lane
# walk, C two-note chords with 2-beat sustains, D eighth-note HOPO run.
# One SP phrase in B and one in D per 32-bar cycle. Still 88s in the 90s
# stems, still beat-aligned with the groove.
RES = 192
BEATS = 176
lines = []
sp_marks = []
b = 0
while b < BEATS:
    bar = b // 4
    sec = (bar // 8) % 4
    tick = b * RES
    if sec == 0:                       # A: green quarters, sustain on beat 5
        sus = RES // 2 if b % 8 == 4 else 0
        lines.append(f"  {tick} = N 0 {sus}")
    elif sec == 1:                     # B: lane walk G R Y B
        lane = [0, 1, 2, 3][b % 4]
        lines.append(f"  {tick} = N {lane} 0")
    elif sec == 2:                     # C: chords + 2-beat sustains
        lo = [0, 1][bar % 2]
        if b % 4 == 0:
            lines.append(f"  {tick} = N {lo} {2 * RES}")
            lines.append(f"  {tick} = N {lo + 1} {2 * RES}")
        elif b % 4 == 2:
            lines.append(f"  {tick} = N {lo + 2} 0")
    else:                              # D: eighth HOPO run G-R-Y-B-O ladder
        for e in range(2):
            t8 = tick + e * (RES // 2)
            lane = (b * 2 + e) % 5
            lines.append(f"  {t8} = N {lane} 0")
    # SP phrases: bars 12-13 (in B) and 28-29 (in D) of each 32-bar cycle
    if bar % 32 in (12, 28) and b % 4 == 0:
        sp_marks.append(f"  {tick} = S 2 {8 * RES}")
    b += 1
ev = sorted(lines + sp_marks, key=lambda ln: int(ln.split(" = ")[0]))
chart = (
    "[Song]\n{\n  Resolution = 192\n  Offset = 0\n"
    "  Name = SkyHero Test Song\n}\n"
    "[SyncTrack]\n{\n  0 = B 120000\n  0 = TS 4\n}\n"
    "[Events]\n{\n}\n"
    "[ExpertSingle]\n{\n" + "\n".join(ev) + "\n}\n"
)
with open(os.path.join(OUT, "notes.chart"), "w", newline="\n") as f:
    f.write(chart)
with open(os.path.join(OUT, "song.ini"), "w", newline="\n") as f:
    f.write("[song]\nname = SkyHero Test Song\nartist = bardcore\n"
            "charter = SkyHero\nsong_length = 90000\n")
print("done:", OUT)
