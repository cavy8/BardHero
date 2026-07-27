# Writes 4 rhythmically-distinct sine stems as WAV; ffmpeg encodes to ogg.
# Stems are distinguishable by ear so a stem failing to start is audible.
import wave, math, struct, os

RATE, DUR = 48000, 90
OUT = os.path.join(os.path.dirname(__file__), "..", "dist", "SKSE", "Plugins",
                   "BardHero", "spike")

def write(name, freq, pattern):
    os.makedirs(OUT, exist_ok=True)
    w = wave.open(os.path.join(OUT, name), "wb")
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
    frames = bytearray()
    for i in range(RATE * DUR):
        t = i / RATE
        beat = int(t * 2) % len(pattern)          # 120 BPM eighth grid
        amp = 0.22 if pattern[beat] == "x" else 0.0
        env = min(1.0, (t * 2 - int(t * 2)) * 20)  # click-free attack
        s = int(32000 * amp * env * math.sin(2 * math.pi * freq * t))
        frames += struct.pack("<hh", s, s)
    w.writeframes(bytes(frames)); w.close()

write("stem_song.wav", 220, "xxxx")
write("stem_guitar.wav", 330, "x.x.")
write("stem_bass.wav", 110, "x...")
write("stem_drums.wav", 65, "..x.")
print("done")
