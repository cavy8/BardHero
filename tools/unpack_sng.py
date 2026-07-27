# Unpacks Clone Hero .sng packages (Chorus Encore downloads) into the song
# FOLDERS SkyHero's scanner reads. Format per mdsitton/SngFileFormat:
#   header:  "SNGPKG" + u32 version + 16-byte xorMask
#   then 3 sections, each u64-length-prefixed: metadata, file index, file data
#   file bytes are masked: plain[i] = masked[i] ^ xorMask[i % 16] ^ (i & 0xFF)
# The metadata pairs ARE the song.ini keys; a song.ini is reconstructed from
# them (an [song] header + key = value lines).
#
# Usage: python unpack_sng.py <file.sng> [more.sng ...] <output_root>
#        each song lands in <output_root>/<Artist> - <Name>/
import os
import struct
import sys


def read_u32(f):
    return struct.unpack("<I", f.read(4))[0]


def read_u64(f):
    return struct.unpack("<Q", f.read(8))[0]


def read_i32(f):
    return struct.unpack("<i", f.read(4))[0]


def unmask(data, mask):
    out = bytearray(len(data))
    for i, b in enumerate(data):
        out[i] = b ^ mask[i % 16] ^ (i & 0xFF)
    return bytes(out)


def unpack(path, out_root):
    with open(path, "rb") as f:
        if f.read(6) != b"SNGPKG":
            raise ValueError(f"{path}: not an SNG file")
        version = read_u32(f)
        mask = f.read(16)

        # --- metadata section ---
        read_u64(f)  # metadataLen
        meta = {}
        for _ in range(read_u64(f)):
            klen = read_i32(f)
            key = f.read(klen).decode("utf-8")
            vlen = read_i32(f)
            meta[key] = f.read(vlen).decode("utf-8", errors="replace")

        # --- file index section ---
        read_u64(f)  # fileMetaLen
        entries = []
        for _ in range(read_u64(f)):
            nlen = f.read(1)[0]
            name = f.read(nlen).decode("utf-8")
            clen = read_u64(f)
            cidx = read_u64(f)  # absolute offset from file start
            entries.append((name, clen, cidx))

        # --- extract ---
        artist = meta.get("artist", "Unknown").strip() or "Unknown"
        name = meta.get("name", os.path.splitext(os.path.basename(path))[0])
        folder = f"{artist} - {name}".strip()
        folder = "".join(c for c in folder if c not in '<>:"/\\|?*')
        dest = os.path.join(out_root, folder)
        os.makedirs(dest, exist_ok=True)

        for fname, clen, cidx in entries:
            f.seek(cidx)
            data = unmask(f.read(clen), mask)
            safe = os.path.basename(fname.replace("\\", "/"))
            with open(os.path.join(dest, safe), "wb") as o:
                o.write(data)

        # song.ini from the metadata pairs (sng stores ini keys directly)
        with open(os.path.join(dest, "song.ini"), "w", encoding="utf-8",
                  newline="\n") as o:
            o.write("[song]\n")
            for k, v in meta.items():
                o.write(f"{k} = {v}\n")

        print(f"{os.path.basename(path)} (v{version}) -> {dest}")
        print(f"  {len(entries)} files: "
              f"{', '.join(e[0] for e in entries)}")
        return dest


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    root = sys.argv[-1]
    for sng in sys.argv[1:-1]:
        unpack(sng, root)
