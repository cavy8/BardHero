"""Import bard-mod audio into playable SkyHero song folders.

One command: BSA extract -> ffmpeg transcode -> onset-detection auto-chart ->
<out>/<Artist> - <Title>/{notes.chart, song.ogg, song.ini}.

Inputs: audio files (.xwm/.wav/.mp3/.ogg/.opus), folders of them, or a
Skyrim .bsa (v104/v105) with --filter to select entries. Requires ffmpeg on
PATH (decodes xwm via wmav2 - verified) and numpy.

Auto-chart shape (v1, deliberately simple - bard material is sparse):
spectral-flux onsets -> pitch-centroid relative-motion lane walk (0..4) ->
single-BPM tempo estimate -> Expert/Hard/Medium/Easy by min-gap thinning.
No sustains/chords/SP. ConvertHero does NOT auto-chart audio (tempo markers
+ MIDI->chart only, GUI-only), hence this built-in charter.

Copyright rail: outputs are for the local install ONLY - never dist/.

Usage:
  python tools/import_bard_song.py INPUT... [--out DIR] [--filter SUBSTR]
         [--artist X] [--name Y] [--dry-run]
"""

import argparse
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
import zlib

import numpy as np

DEFAULT_OUT = (r"C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods"
               r"\BardHero\SKSE\Plugins\BardHero\songs")
AUDIO_EXTS = {".xwm", ".wav", ".mp3", ".ogg", ".opus", ".fuz"}
RESOLUTION = 192
SR = 22050  # analysis rate


# ---------------------------------------------------------------- BSA reader
def read_bsa_entries(path):
    """Yield (folder, name, data) for every file in a v104/v105 BSA."""
    with open(path, "rb") as f:
        if f.read(4) != b"BSA\0":
            raise SystemExit(f"{path}: not a BSA")
        (version, folder_off, archive_flags, folder_count, _file_count,
         _tot_fname_len, tot_filename_len, _file_flags) = struct.unpack(
            "<8I", f.read(32))
        compressed_default = bool(archive_flags & 0x4)
        embed_names = bool(archive_flags & 0x100)
        f.seek(folder_off)
        folders = []
        for _ in range(folder_count):
            if version >= 105:
                _h, cnt, _pad, off, _pad2 = struct.unpack("<QIIII", f.read(24))
            else:
                _h, cnt, off = struct.unpack("<QII", f.read(16))
            folders.append((cnt, off))
        entries = []
        for cnt, off in folders:
            f.seek(off - tot_filename_len)
            ln = f.read(1)[0]
            fname = f.read(ln).rstrip(b"\0").decode("cp1252", "replace")
            for _ in range(cnt):
                _h, sz, doff = struct.unpack("<QII", f.read(16))
                entries.append([fname, None, sz, doff])
        for e in entries:  # file-name block follows the folder records
            s = b""
            while (c := f.read(1)) not in (b"\0", b""):
                s += c
            e[1] = s.decode("cp1252", "replace")
        for fol, name, sz, doff in entries:
            comp = compressed_default != bool(sz & 0x40000000)
            size = sz & 0x3FFFFFFF
            f.seek(doff)
            if embed_names:
                nl = f.read(1)[0]
                f.read(nl)
                size -= nl + 1
            data = f.read(size)
            if comp:
                data = zlib.decompress(data[4:])
            yield fol, name, data


# ------------------------------------------------------------------- ffmpeg
def run_ffmpeg(args):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"ffmpeg failed: {r.stderr.strip()[:400]}")


def to_analysis_wav(src, dst):
    run_ffmpeg(["-i", src, "-ac", "1", "-ar", str(SR),
                "-acodec", "pcm_s16le", dst])


def to_song_ogg(src, dst):
    run_ffmpeg(["-i", src, "-c:a", "libvorbis", "-q:a", "5", dst])


def load_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        data = w.readframes(w.getnframes())
    x = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0
    return x


# -------------------------------------------------------------- auto-charter
FRAME, HOP = 1024, 256  # 11.6 ms hop @ 22050


def stft_mag(x):
    n = 1 + max(0, (len(x) - FRAME)) // HOP
    win = np.hanning(FRAME).astype(np.float32)
    frames = np.lib.stride_tricks.as_strided(
        x, shape=(n, FRAME), strides=(x.strides[0] * HOP, x.strides[0]))
    return np.abs(np.fft.rfft(frames * win, axis=1))


def detect_onsets(mag):
    """Spectral flux + adaptive threshold. Returns (times, flux_envelope)."""
    logm = np.log1p(10.0 * mag)
    flux = np.maximum(logm[1:] - logm[:-1], 0.0).sum(axis=1)
    flux = np.concatenate([[0.0], flux])
    # smooth (3-frame) then median-window adaptive threshold (~0.35 s)
    k = np.array([0.25, 0.5, 0.25], dtype=np.float32)
    sm = np.convolve(flux, k, mode="same")
    w = 15
    pad = np.pad(sm, w, mode="edge")
    med = np.array([np.median(pad[i:i + 2 * w + 1]) for i in range(len(sm))])
    thr = med + 0.35 * (np.percentile(sm, 90) - np.percentile(sm, 50) + 1e-6)
    hopsec = HOP / SR
    min_gap = int(0.1 / hopsec)
    onsets, last = [], -10 ** 9
    for i in range(1, len(sm) - 1):
        if (sm[i] > thr[i] and sm[i] >= sm[i - 1] and sm[i] >= sm[i + 1]
                and i - last >= min_gap):
            onsets.append(i)
            last = i
    return np.array(onsets) * hopsec, sm


def estimate_bpm(env):
    """Autocorrelation of the flux envelope over 60-180 BPM."""
    hopsec = HOP / SR
    e = env - env.mean()
    if len(e) < SR // HOP * 4 or not e.any():
        return 120.0
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    lo = int(60.0 / 180.0 / hopsec)   # 180 BPM period
    hi = int(60.0 / 60.0 / hopsec)    # 60 BPM period
    if hi >= len(ac):
        return 120.0
    lag = lo + int(np.argmax(ac[lo:hi]))
    bpm = 60.0 / (lag * hopsec)
    while bpm < 90.0:                  # prefer a danceable octave
        bpm *= 2.0
    while bpm > 180.0:
        bpm /= 2.0
    return round(bpm, 2)


def pitch_centroids(mag, onset_times):
    """Spectral centroid (80-1200 Hz band) shortly after each onset."""
    freqs = np.fft.rfftfreq(FRAME, 1.0 / SR)
    band = (freqs >= 80) & (freqs <= 1200)
    hopsec = HOP / SR
    out = []
    for t in onset_times:
        i0 = int(t / hopsec) + 1
        seg = mag[i0:i0 + 6, band]          # ~70 ms after the attack
        if seg.size == 0 or seg.sum() < 1e-9:
            out.append(0.0)
            continue
        m = seg.mean(axis=0)
        out.append(float((m * freqs[band]).sum() / (m.sum() + 1e-9)))
    return out


def assign_lanes(centroids):
    """Relative-motion walk: pitch up -> higher lane, down -> lower."""
    lanes, lane, prev = [], 2, None
    for c in centroids:
        if prev and c > 0:
            r = c / prev
            if r > 1.30:
                lane += 2
            elif r > 1.05:
                lane += 1
            elif r < 0.77:
                lane -= 2
            elif r < 0.95:
                lane -= 1
            lane = max(0, min(4, lane))
        if c > 0:
            prev = c
        lanes.append(lane)
    return lanes


def thin(times_lanes, min_gap):
    out, last = [], -10 ** 9
    for t, lane in times_lanes:
        if t - last >= min_gap:
            out.append((t, lane))
            last = t
    return out


# ------------------------------------------------------------------ emitters
def sanitize(s):
    return re.sub(r'[<>:"/\\|?*]', "", s).strip() or "Unknown"


def title_from(stem):
    stem = re.sub(r"^\d+[_\- ]*", "", stem)          # 01_secunda -> secunda
    stem = stem.replace("_", " ").strip()
    return " ".join(w.capitalize() for w in stem.split())


def write_chart(path, title, artist, bpm, diffs):
    tick = lambda t: int(round(t * bpm / 60.0 * RESOLUTION))
    lines = ["[Song]", "{",
             f'  Name = "{title}"', f'  Artist = "{artist}"',
             '  Charter = "SkyHero import"', f"  Resolution = {RESOLUTION}",
             '  MusicStream = "song.ogg"', "}",
             "[SyncTrack]", "{", "  0 = TS 4",
             f"  0 = B {int(round(bpm * 1000))}", "}"]
    for section, notes in diffs.items():
        lines += [f"[{section}]", "{"]
        prev_tick = -1
        for t, lane in notes:
            tk = tick(t)
            if tk <= prev_tick:      # collapse rounding collisions
                continue
            prev_tick = tk
            lines.append(f"  {tk} = N {lane} 0")
        lines.append("}")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_song_ini(path, title, artist, length_ms, single_instrument=True):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("[song]\n"
                f"name = {title}\n"
                f"artist = {artist}\n"
                f"charter = SkyHero import\n"
                f"song_length = {int(length_ms)}\n")
        if single_instrument:
            # solo-instrument source: the whole track IS the instrument, so
            # SkyHero may whole-mute it on a miss (miss-feel v1)
            f.write("single_instrument = 1\n")


def verify_chart(path):
    """Sanity: parses, ticks monotonic per section, has notes."""
    notes, section, prev = 0, None, -1
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("["):
            section, prev = line, -1
        m = re.match(r"(\d+) = N (\d) 0$", line)
        if m:
            tk, lane = int(m.group(1)), int(m.group(2))
            assert tk > prev, f"non-monotonic tick in {section}"
            assert 0 <= lane <= 4
            prev = tk
            notes += 1
    assert notes > 0, "chart has no notes"
    return notes


# --------------------------------------------------------------------- main
def import_one(audio_path, out_root, artist_opt, name_opt, songbook, dry,
               mixed=False):
    stem = os.path.splitext(os.path.basename(audio_path))[0]
    if " - " in stem and not (artist_opt and name_opt):
        a, t = stem.split(" - ", 1)
        artist = artist_opt or title_from(a)
        title = name_opt or title_from(t)
    else:
        title = name_opt or title_from(stem)
        artist = artist_opt or title_from(songbook or "Bard")
    folder = os.path.join(out_root, sanitize(f"{artist} - {title}"))

    with tempfile.TemporaryDirectory() as td:
        awav = os.path.join(td, "a.wav")
        to_analysis_wav(audio_path, awav)
        x = load_wav(awav)
        length_s = len(x) / SR
        mag = stft_mag(x)
        times, env = detect_onsets(mag)
        bpm = estimate_bpm(env)
        lanes = assign_lanes(pitch_centroids(mag, times))
        tl = list(zip(times.tolist(), lanes))
        diffs = {"ExpertSingle": tl,
                 "HardSingle":   thin(tl, 0.18),
                 "MediumSingle": thin(tl, 0.30),
                 "EasySingle":   thin(tl, 0.50)}
        print(f"  {os.path.basename(audio_path)}: {length_s:5.1f}s "
              f"bpm~{bpm:6.1f} onsets={len(tl)} "
              f"(H {len(diffs['HardSingle'])} / M {len(diffs['MediumSingle'])}"
              f" / E {len(diffs['EasySingle'])}) -> {folder}")
        if len(tl) < 12:
            print("    WARN: very sparse - likely ambient/quiet source")
        if dry:
            return
        os.makedirs(folder, exist_ok=True)
        to_song_ogg(audio_path, os.path.join(folder, "song.ogg"))
        write_chart(os.path.join(folder, "notes.chart"), title, artist,
                    bpm, diffs)
        write_song_ini(os.path.join(folder, "song.ini"), title, artist,
                       length_s * 1000, single_instrument=not mixed)
        verify_chart(os.path.join(folder, "notes.chart"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inputs", nargs="+",
                    help="audio files, folders, or a .bsa")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--filter", default="",
                    help="substring filter for BSA entries / folder walks")
    ap.add_argument("--artist", default=None)
    ap.add_argument("--name", default=None,
                    help="song title (single-input use)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--mixed", action="store_true",
                    help="source is a full mix, NOT a solo instrument - "
                         "suppresses the single_instrument song.ini flag "
                         "(so SkyHero won't whole-mute it on misses)")
    args = ap.parse_args()

    jobs = []  # (audio path on disk, songbook hint, cleanup dir or None)
    tmp = None
    for inp in args.inputs:
        low = inp.lower()
        if low.endswith(".bsa"):
            tmp = tmp or tempfile.mkdtemp(prefix="skyhero_import_")
            for fol, name, data in read_bsa_entries(inp):
                full = f"{fol}\\{name}"
                if args.filter.lower() not in full.lower():
                    continue
                if os.path.splitext(name)[1].lower() not in AUDIO_EXTS:
                    continue
                p = os.path.join(tmp, name)
                with open(p, "wb") as o:
                    o.write(data)
                jobs.append((p, os.path.basename(fol)))
        elif os.path.isdir(inp):
            for root, _dirs, files in os.walk(inp):
                for fn in sorted(files):
                    if os.path.splitext(fn)[1].lower() in AUDIO_EXTS:
                        full = os.path.join(root, fn)
                        if args.filter.lower() in full.lower():
                            jobs.append((full, os.path.basename(root)))
        else:
            jobs.append((inp, os.path.basename(os.path.dirname(inp))))

    if not jobs:
        raise SystemExit("no matching audio inputs")
    print(f"importing {len(jobs)} song(s) -> {args.out}"
          f"{'  [DRY RUN]' if args.dry_run else ''}")
    for p, songbook in jobs:
        import_one(p, args.out, args.artist, args.name, songbook,
                   args.dry_run, args.mixed)
    print("done.")


if __name__ == "__main__":
    main()
