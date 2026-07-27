"""Fetch BardHero's recommended charts onto THIS machine.

    python tools/fetch_songs.py search "through the fire and flames"
    python tools/fetch_songs.py sync <manifest.json> <songs-root> [--dry-run]

---- WHY THIS EXISTS, AND WHAT IT DELIBERATELY DOES NOT DO ----------------

BardHero ships a LIST of songs, never the songs. A Clone Hero chart bundles
the real audio stems plus someone else's charting work, so redistributing one
inside a mod is not ours to do. Shipping the list instead gets the player to
the same place - install the mod, get the songs - while the charts arrive
from Encore on their own machine, with the charter's own metadata intact.

---- BRIDGE IS NOT A DEPENDENCY ------------------------------------------

Bridge (Geomitron/Bridge) is a GUI client for the same public service. It
exposes nothing to automate: no protocol handler, no CLI beyond `--dev`, and
a single-instance lock that turns a second launch into "focus the window".
So this talks to the endpoints Bridge talks to, and players need no Bridge
install. Bridge stays useful as the browse-everything GUI for players who
want charts beyond the curated set - and it already drops them straight into
`songs\\guitar`, which ResolveInstrumentTag picks up with no conversion.

    search:   POST https://api.enchor.us/search
    download: GET  https://files.enchor.us/<md5>.sng   (`_novideo` variant)

---- THE MD5 IS AN IDENTIFIER, NOT A CHECKSUM ----------------------------

Encore's `md5` is the CHART hash and doubles as the CDN key. Nothing promises
it equals md5(the .sng bytes), so verifying downloads against it would fail
for reasons that have nothing to do with corruption. Integrity is pinned
SEPARATELY: `sha256` is recorded the first time a song is fetched and checked
on every fetch after that. An entry with no sha256 is reported as UNVERIFIED
rather than quietly trusted.

Pinning by hash is also what keeps players on the chart that was actually
tested. This project has already been bitten by charts whose four difficulty
tracks were byte-identical, and by a six-fret GHL chart the loader rejects.

Standard library only, so the tool can ship next to the mod without a pip
install.
"""
import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unpack_sng  # noqa: E402  - sibling tool, already the reference impl

# Charter names are full of non-ASCII ("Mickelraven" is really "Mickelräven")
# and a stock Windows console is cp1252, which raises UnicodeEncodeError on
# the way OUT and kills the tool after the work is already done. Reconfigure
# rather than sanitise the strings: the names have to stay printable and
# correct, because they are what gets pinned into the manifest.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):     # pre-3.7, or a redirected pipe
        pass

API = "https://api.enchor.us"
FILES = "https://files.enchor.us"

# Identify honestly. We are not Bridge, and claiming to be would corrupt
# whatever client analytics the service keeps.
UA = "BardHero-fetch_songs/1.0 (+https://github.com/; Skyrim mod helper)"

# Encore is a free community service. One request at a time, with a pause
# between charts - a mod helper has no business hammering it.
POLITE_DELAY_SEC = 1.0


def _post(path, payload):
    req = urllib.request.Request(
        API + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "User-Agent": UA},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def _screen(chart, instrument):
    """What the search response can prove about a chart, before downloading.

    Validated 2026-07-26 against the one chart whose ground truth we hold:
    the pinned AC/DC entry comes back 269/355/444/526 across four DISTINCT
    trackHashes, digit-for-digit the numbers SongLoadProbe measured in the
    2026-07-25 audit. So these fields can be trusted.

    This screen only ever rules a chart OUT. It cannot hear whether the audio
    is the right track or whether the chart is any fun, so a survivor still
    has to be PLAYED before it earns a pin.

    Deliberately NOT screened on: `chartIssues`. Measured on real charts, it
    is charting-style nits rather than defects - Through The Fire & Flames
    trips it 9350 times and every single one is babySustain/badSustainGap
    from Neversoft's original GH3 sustains. Weighting it would reject good
    charts for cosmetic reasons.

    CANNOT be screened on: AUDIO STEM LAYOUT. The search response carries no
    stem information at all - every key was dumped 2026-07-26 and there is
    nothing for it. That blind spot matters, because stems decide whether
    miss feedback works: FindStem("guitar") + FadeStem is the whole path, so
    a chart with no guitar.opus gives no audio penalty on a miss, and one
    where guitar.opus is the ONLY stem fades the entire song and hands the
    Star Power flanger the vocal track. Both have to be checked on a LOCAL
    copy. The play-gate already forces one to exist, so check it then.

    Returns (counts_text, [warnings]).
    """
    nd = chart.get("notesData") or {}
    counts = {n["difficulty"]: n["count"]
              for n in (nd.get("noteCounts") or [])
              if n.get("instrument") == instrument}
    hashes = {h["difficulty"]: h["hash"]
              for h in (nd.get("trackHashes") or [])
              if h.get("instrument") == instrument}

    warn = []

    # Six-fret GHL. The loader rejects it outright, and this has already cost
    # a stint - it is indistinguishable from a normal chart in a result list.
    instruments = nd.get("instruments") or []
    if instrument not in instruments:
        warn.append(f"NO {instrument} track (has: {', '.join(instruments)})"
                    + (" - six-fret GHL, the loader REJECTS it"
                       if any("ghl" in i for i in instruments) else ""))

    # Distinct track hashes are what separates a real difficulty reduction
    # from Expert wearing an Easy label. Identical hash = identical notes.
    if len(hashes) <= 1:
        warn.append(f"only {len(hashes)} difficulty track "
                    f"({', '.join(hashes) or 'none'}) - Expert or nothing")
    elif len(set(hashes.values())) < len(hashes):
        same = {}
        for d, h in hashes.items():
            same.setdefault(h, []).append(d)
        warn.append("IDENTICAL difficulty tracks: "
                    + "; ".join("/".join(v) for v in same.values()
                                if len(v) > 1))

    # `diff_guitar` feeds UnlockLogic::RequiredRank, and ABSENT means rank 1
    # there - so a missing value silently makes the hardest chart in the set
    # available from the very first rank.
    if chart.get(f"diff_{instrument}", -1) < 0:
        warn.append(f"no diff_{instrument} in song.ini - UnlockLogic treats "
                    f"that as rank 1, the easiest unlock tier")

    order = ["easy", "medium", "hard", "expert"]
    text = "/".join(str(counts[d]) for d in order if d in counts) or "?"
    return f"{text} notes ({len(set(hashes.values()))} distinct)", warn


def cmd_search(args):
    # `source` is DELIBERATELY ABSENT. Its schema is `z.enum(['api'])`
    # (src-shared/search-api.ts), so the only accepted value is "api" and
    # anything else is a 400 - which is what an honest "bardhero" got. The
    # field is optional, and omitting it beats both lying about being Bridge
    # and asserting a label the server does not recognise.
    body = {
        "search": args.query,
        "per_page": args.limit,
        "page": 1,
        "instrument": args.instrument,
        "difficulty": None,
        "drumType": None,
        "sort": None,
    }
    try:
        res = _post("/search", body)
    except urllib.error.HTTPError as e:
        print(f"search failed: HTTP {e.code} {e.reason}")
        if e.code == 400:
            print("  (a 400 here usually means the request shape changed - "
                  "re-check src-angular/app/core/services/search.service.ts "
                  "in the Bridge source)")
        return 1

    charts = res.get("data") or []
    if not charts:
        print("no results")
        return 0
    print(f"{len(charts)} result(s) - paste an entry into the manifest:\n")
    for c in charts:
        counts, warn = _screen(c, args.instrument)
        stems = (c.get("notesData") or {}).get("instruments") or []
        secs = (c.get("song_length") or 0) // 1000
        print(f"  {c.get('artist')} - {c.get('name')}")
        print(f"    charter    : {c.get('charter')}")
        print(f"    length     : {secs // 60}:{secs % 60:02d}")
        print(f"    {args.instrument:<10} : {counts}")
        print(f"    tracks     : {', '.join(stems) or 'none'}")
        print(f"    video bg   : {bool(c.get('hasVideoBackground'))}")
        for w in warn:
            print(f"    ⚠ {w}")
        print("    " + json.dumps({
            "md5": c.get("md5"),
            "artist": c.get("artist"),
            "name": c.get("name"),
            "charter": c.get("charter"),
            "novideo": bool(c.get("hasVideoBackground")),
        }))
        print()
    print("⚠ A chart with no warnings above is a CANDIDATE, not a pick. The\n"
          "  screen reads metadata; it cannot hear that the audio is the "
          "right\n  track or that the chart is any fun. PLAY IT, then pin it.")
    return 0


def _folder_for(entry):
    """The folder unpack_sng will create - used to skip existing songs."""
    folder = f"{entry.get('artist', 'Unknown')} - {entry.get('name', '')}"
    return "".join(c for c in folder.strip() if c not in '<>:"/\\|?*')


def _apply_unlock_rank(song_dir, rank, overwrite):
    """Write BardHero's `unlock_rank` into a downloaded chart's song.ini.

    WHY THIS IS NEEDED. UnlockLogic::RequiredRank derives a song's rank from
    song.ini's `diff_guitar`, and reads an ABSENT diff_guitar as rank 1 - the
    EASIEST tier. Several of the best uploads on Encore omit the key, so the
    hardest charts in the set would arrive unlocked from the very first rank.
    Measured 2026-07-26 on Through The Fire & Flames, Knights of Cydonia and
    both Neversoft cuts pinned here.

    WHY IT IS SAFE. `unlock_rank` is BardHero's own key: SongIni.cpp reads it
    and RequiredRank prefers it over diffGuitar. Clone Hero and every other
    reader ignore an unknown ini key, so the line costs the charter's metadata
    nothing and changes no chart, note or audio file.

    `overwrite` is False for a song that was already installed, so a rank the
    player set by hand is never clobbered - only a missing one is filled in.
    """
    if rank is None:
        return
    try:
        rank = int(rank)
    except (TypeError, ValueError):
        print(f"      WARN unlock_rank {rank!r} is not a number - ignored")
        return
    if not 1 <= rank <= 5:
        # RequiredRank clamps to 1..5 anyway; say so rather than silently
        # writing a value that cannot mean what the author intended.
        print(f"      WARN unlock_rank {rank} is outside 1..5 and will be "
              f"clamped - ignored")
        return

    ini = os.path.join(song_dir, "song.ini")
    try:
        with open(ini, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print(f"      WARN could not set unlock_rank: {e}")
        return

    if any(l.split("=")[0].strip().lower() == "unlock_rank" for l in lines) \
            and not overwrite:
        return

    out, written = [], False
    for line in lines:
        if line.split("=")[0].strip().lower() == "unlock_rank":
            if not written:                 # collapse any duplicates
                out.append(f"unlock_rank = {rank}")
                written = True
            continue
        out.append(line)
    if not written:
        # unpack_sng writes a single [song] section, so appending stays in it.
        out.append(f"unlock_rank = {rank}")

    try:
        with open(ini, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out) + "\n")
    except OSError as e:
        print(f"      WARN could not set unlock_rank: {e}")
        return
    print(f"      unlock_rank = {rank}")


def cmd_sync(args):
    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    songs = manifest.get("songs") or []
    if not songs:
        print(f"{args.manifest}: no songs pinned - nothing to do")
        return 0

    os.makedirs(args.songs_root, exist_ok=True)

    # ResolveInstrumentTag reads the instrument from an ANCESTOR DIRECTORY
    # when song.ini carries no supported tag, which is the normal case for a
    # Chorus download. Sync somewhere else and the charts still load - they
    # just arrive untagged, which is a silent, confusing failure rather than
    # a loud one (verified: SongLoadProbe reports `instrument=` empty). Warn
    # rather than refuse: a custom layout is the player's business.
    leaf = os.path.basename(os.path.normpath(args.songs_root)).lower()
    if leaf not in ("guitar", "lute", "flute", "drum"):
        print(f"WARN  '{leaf}' is not an instrument folder. Charts land "
              f"untagged unless song.ini names an instrument.\n"
              f"      For guitar charts you almost certainly want "
              f"...\\BardHero\\songs\\guitar\n")

    fetched = skipped = failed = 0
    dirty = False

    for entry in songs:
        md5 = entry.get("md5")
        label = f"{entry.get('artist')} - {entry.get('name')}"
        if not md5:
            print(f"SKIP  {label}: no md5 pinned")
            failed += 1
            continue

        dest = os.path.join(args.songs_root, _folder_for(entry))
        if os.path.isdir(dest):
            print(f"HAVE  {label}")
            # Fill in a rank the manifest gained since this song was
            # installed, but never overwrite one already on disk.
            _apply_unlock_rank(dest, entry.get("unlock_rank"), False)
            skipped += 1
            continue

        suffix = "_novideo" if entry.get("novideo") else ""
        url = f"{FILES}/{md5}{suffix}.sng"
        if args.dry_run:
            print(f"GET   {label}\n      {url}\n      -> {dest}")
            fetched += 1
            continue

        print(f"GET   {label}")
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=120) as r:
                blob = r.read()
        except urllib.error.HTTPError as e:
            print(f"      FAILED: HTTP {e.code} {e.reason}")
            if e.code == 404:
                print("      the pinned chart is gone from the CDN - the "
                      "charter probably re-uploaded a revision, so this "
                      "entry needs re-pinning")
            failed += 1
            continue
        except Exception as e:                       # noqa: BLE001
            print(f"      FAILED: {e}")
            failed += 1
            continue

        digest = hashlib.sha256(blob).hexdigest()
        pinned = entry.get("sha256")
        if pinned and digest != pinned:
            # Refuse rather than unpack. A changed chart is not necessarily
            # malicious, but it is NOT the one that was play-tested, which is
            # the whole reason the manifest pins anything.
            print(f"      REFUSED: sha256 mismatch\n"
                  f"        pinned   {pinned}\n"
                  f"        received {digest}")
            failed += 1
            continue
        if not pinned:
            print(f"      UNVERIFIED (first fetch) - recording sha256")
            entry["sha256"] = digest
            dirty = True

        tmp = os.path.join(args.songs_root, f".{md5}.sng.part")
        try:
            with open(tmp, "wb") as f:
                f.write(blob)
            song_dir = unpack_sng.unpack(tmp, args.songs_root)
            _apply_unlock_rank(song_dir, entry.get("unlock_rank"), True)
            fetched += 1
        except Exception as e:                       # noqa: BLE001
            print(f"      FAILED to unpack: {e}")
            failed += 1
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)

        time.sleep(POLITE_DELAY_SEC)

    if dirty and not args.dry_run:
        with open(args.manifest, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"\nrecorded new sha256 pins into {args.manifest}")

    print(f"\nfetched {fetched}, already had {skipped}, failed {failed}")
    if fetched and not args.dry_run:
        print("BardHero rescans without a restart - use Rescan songs in "
              "settings, or just reopen the Songbook.")
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("search", help="find charts to pin (authoring)")
    s.add_argument("query")
    s.add_argument("--limit", type=int, default=10)
    s.add_argument("--instrument", default="guitar")
    s.set_defaults(func=cmd_search)

    y = sub.add_parser("sync", help="download every pinned chart")
    y.add_argument("manifest")
    y.add_argument("songs_root")
    y.add_argument("--dry-run", action="store_true")
    y.set_defaults(func=cmd_sync)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
