"""Regenerate "Bard Hero - Doom Lute.esp" from the PRISTINE original.

    python tools/patch_guitar_esp.py <original.esp> <output.esp>

Always runs from the untouched upstream plugin, never from a previously
patched one - patching a patch makes the result depend on how many times it
has been run, and there is no way to tell by looking.

What it does:
  - renames the Gibson SG records to Doom Lute
  - prices the Doom Lute at 666 gold (owner's ask, 2026-07-27 - of course
    it appraises at that)
  - deletes the regular-forge COBJ recipe (the Doom Lute is Atronach Forge
    only)
  - adds an Atronach Forge ingredient FormList

---- WHY THE VANILLA FORGE LISTS ARE NOT TOUCHED HERE --------------------

Adding a recipe means getting into `AtrFrgAtronachForgeRecipeList` and its
index-paired `...ResultList`, and THREE plugins in the target load order
already override both - `FormList-Patch-Collection_ITMs.esp`,
`mihailaegisofthesigil.esp` and `mihailpossesseddaedricarmors.esp`. An
override built from the vanilla contents would silently drop every recipe
those add, and one of them exists precisely to reconcile FormList conflicts.

So this plugin stays PURELY ADDITIVE - one new record, no vanilla record
overridden - and src/game/AtronachRecipe.cpp appends to whichever copy
actually won, at runtime. That conflicts with nobody, needs no new masters,
and does not care about load order.

---- THE RECIPE ---------------------------------------------------------

No sigil stone. The forge CONSUMES everything in the offering box, and a
sigil stone is a scarce Oblivion-gate drop - spending one on a cosmetic
instrument was not the intent (field 2026-07-26). Note this also keeps the
recipe in the ORDINARY forge list rather than the sigil-stone list, which is
a separate parallel pair with its own gating.
"""
import argparse
import os
import struct

# The item is called the Doom Lute and stays called that. A rename to
# "Strange Instrument" was made and reverted on 2026-07-27 at the owner's
# call - the name is not up for grabs, so change these only on an explicit
# ask. Note the plugin FILENAME must track any future rename too, and that
# reached TEN places last time, two of which fail SILENTLY (the
# OpenAnimationReplacer configs bind by pluginName and the InventoryInjector
# rules bind by "<plugin>|<formid>").
RENAME = {
    0x02000800: b'Doom Lute\0',
    0x02000801: b'Doom Lute (Performance Prop)\0',
    0x0200080A: b'Doom Lute (Band Prop)\0',
}
DELETE = {0x02000804}          # BardHeroElectricGuitarRecipe (forge COBJ)

# MISC DATA is <I value><f weight>; only the value is rewritten (upstream
# ships 750). 666 for the DOOM lute is the whole joke, and the owner asked
# for it by number - do not "fix" it to something sensible.
VALUE = {0x02000800: 666}

NEW_FLST_ID = 0x0200080D
NEW_FLST_EDID = b'BardHeroDoomLuteAtronachRecipe\0'
# All from Skyrim.esm, which is this plugin's first master, so the FormIDs
# go in as-is. EditorIDs kept beside them because a bare hex FormID is
# unreviewable.
INGREDIENTS = [
    0x0003AD60,   # INGR VoidSalts
    0x000DABAB,   # MISC Lute
    0x000F4983,   # MISC DwarvenCenturionDynamo  (Centurion Dynamo Core)
    0x0002E504,   # SLGM SoulGemBlackFilled
]


def sub_iter(body):
    o = 0
    while o + 6 <= len(body):
        s = body[o:o + 4]
        ln = struct.unpack_from('<H', body, o + 4)[0]
        yield s, body[o + 6:o + 6 + ln]
        o += 6 + ln


def rebuild_body(body, new_full):
    out = b''
    for s, v in sub_iter(body):
        if s == b'FULL':
            v = new_full
        out += s + struct.pack('<H', len(v)) + v
    return out


def rebuild_value(body, new_value):
    out = b''
    for s, v in sub_iter(body):
        if s == b'DATA':
            # First 4 bytes of MISC DATA are the gold value; weight stays.
            v = struct.pack('<I', new_value) + v[4:]
        out += s + struct.pack('<H', len(v)) + v
    return out


def mkrec(sig, body, formid, flags=0, ts=0, vc=0, ver=44, unk=0):
    return (sig + struct.pack('<IIIHHHH', len(body), flags, formid,
                              ts, vc, ver, unk) + body)


def patch(src, dst):
    data = open(src, 'rb').read()
    tes4_size = struct.unpack_from('<I', data, 4)[0]
    tes4_full = data[:24 + tes4_size]
    off = 24 + tes4_size

    groups = []
    while off < len(data):
        if data[off:off + 4] != b'GRUP':
            raise ValueError(f'expected GRUP at {off}')
        gsize = struct.unpack_from('<I', data, off + 4)[0]
        label = data[off + 8:off + 12]
        ghdr = data[off:off + 24]
        recs = []
        ro = off + 24
        while ro < off + gsize:
            rsize = struct.unpack_from('<I', data, ro + 4)[0]
            recs.append(data[ro:ro + 24 + rsize])
            ro += 24 + rsize
        groups.append([label, ghdr, recs])
        off += gsize

    renamed, deleted, revalued = [], [], []
    for g in groups:
        kept = []
        for r in g[2]:
            sig = r[:4]
            dsize = struct.unpack_from('<I', r, 4)[0]
            flags = struct.unpack_from('<I', r, 8)[0]
            formid = struct.unpack_from('<I', r, 12)[0]
            tail = struct.unpack_from('<HHHH', r, 16)
            body = r[24:24 + dsize]
            if formid in DELETE:
                deleted.append(formid)
                continue
            changed = False
            if formid in RENAME:
                body = rebuild_body(body, RENAME[formid])
                renamed.append(formid)
                changed = True
            if formid in VALUE:
                body = rebuild_value(body, VALUE[formid])
                revalued.append(formid)
                changed = True
            if changed:
                r = mkrec(sig, body, formid, flags, *tail)
            kept.append(r)
        g[2] = kept
    groups = [g for g in groups if g[2]]

    flst_body = b'EDID' + struct.pack('<H', len(NEW_FLST_EDID)) + NEW_FLST_EDID
    for fid in INGREDIENTS:
        flst_body += b'LNAM' + struct.pack('<H', 4) + struct.pack('<I', fid)
    flst_rec = mkrec(b'FLST', flst_body, NEW_FLST_ID)
    flst_hdr = (b'GRUP' + struct.pack('<I', 24 + len(flst_rec)) + b'FLST' +
                struct.pack('<iHHHH', 0, 0, 0, 0, 0))
    groups.append([b'FLST', flst_hdr, [flst_rec]])

    out = b''
    total = 0
    for label, ghdr, recs in groups:
        blob = b''.join(recs)
        total += len(recs)
        out += (b'GRUP' + struct.pack('<I', 24 + len(blob)) + label +
                ghdr[12:24] + blob)

    t_body = bytearray(tes4_full[24:])
    o = 0
    while o + 6 <= len(t_body):
        s = bytes(t_body[o:o + 4])
        ln = struct.unpack_from('<H', t_body, o + 4)[0]
        if s == b'HEDR':
            struct.pack_into('<I', t_body, o + 6 + 4, total)
            nxt = struct.unpack_from('<I', t_body, o + 6 + 8)[0]
            if nxt <= (NEW_FLST_ID & 0xFFFFFF):
                struct.pack_into('<I', t_body, o + 6 + 8,
                                 (NEW_FLST_ID & 0xFFFFFF) + 1)
            break
        o += 6 + ln
    tes4_out = (tes4_full[:4] + struct.pack('<I', len(t_body)) +
                tes4_full[8:24] + bytes(t_body))

    open(dst, 'wb').write(tes4_out + out)
    print(f"renamed : {[f'{x:08X}' for x in renamed]}")
    print(f"revalued: {[f'{x:08X}' for x in revalued]} -> "
          f"{[VALUE[x] for x in revalued]}")
    print(f"deleted : {[f'{x:08X}' for x in deleted]}")
    print(f"recipe  : FLST {NEW_FLST_ID:08X}, "
          f"{len(INGREDIENTS)} ingredients "
          f"{[f'{i:06X}' for i in INGREDIENTS]}")
    print(f"records : {total}")
    print(f"size    : {os.path.getsize(src)} -> {os.path.getsize(dst)} bytes")


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('original', help='PRISTINE upstream esp (never a patched one)')
    p.add_argument('output')
    a = p.parse_args()
    patch(a.original, a.output)
