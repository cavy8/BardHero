# Generates the highway atlas: 1024x1280 (8 cols x 10 rows of 128px cells;
# grew from 8 rows for the P5 win strips - AtlasUv.h's cellV/insetV MUST
# match ROWS), all-white sprites with alpha shapes (tinted at draw time).
# Cell order MUST match src/render/AtlasUv.h::Sprite. Original art -
# deliberately not a Guitar Hero look-alike (spec 12): beveled pucks,
# energy rings/plumes, no licensed silhouettes. v2 (2026-07-19): shaded
# art + juice-pack sprites.
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import os

CELL, GRID = 128, 8
ROWS = 10
OUT = os.path.join(os.path.dirname(__file__), "..", "dist", "SKSE",
                   "Plugins", "BardHero", "highway")

yy, xx = np.mgrid[0:CELL, 0:CELL]
cx = (xx - CELL / 2 + 0.5) / (CELL / 2)   # -1..1
cy = (yy - CELL / 2 + 0.5) / (CELL / 2)
r = np.sqrt(cx * cx + cy * cy)
theta = np.arctan2(cy, cx)

def smooth(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3 - 2 * t)

def glow(radius, softness=1.0):
    """Soft radial falloff to 0 at `radius` (fake-additive halo)."""
    return np.clip(1.0 - r / radius, 0, 1) ** (2.0 * softness)

# --- cells 0-8: upgraded originals (same enum meanings) -------------------
def cell_gem():
    # beveled puck: top-left light bias + dark rim + bright rim arc + halo
    disc = 1.0 - smooth(0.80, 0.88, r)
    light = np.clip(1.0 - np.sqrt((cx + 0.30) ** 2 + (cy + 0.35) ** 2), 0, 1)
    lum = 0.34 + 0.50 * light ** 1.3 + 0.22 * glow(0.45)
    lum = np.where(smooth(0.62, 0.80, r) > 0, lum * (1.0 - 0.55 * smooth(0.62, 0.80, r)), lum)  # dark bevel rim
    arc = (1.0 - smooth(0.05, 0.09, np.abs(r - 0.72))) * np.clip(-np.sin(theta - 0.7), 0, 1)
    lum = np.maximum(lum, 0.95 * arc)                      # specular rim arc
    halo = 0.35 * glow(1.0, 1.6) * smooth(0.80, 0.9, r)    # soft outer halo
    a = np.clip(disc + halo, 0, 1)
    return np.clip(lum, 0, 1), a

def cell_hopo_cap():
    core = 1.0 - smooth(0.30, 0.42, r)
    a = np.clip(core + 0.3 * glow(0.55, 1.5), 0, 1)
    return np.ones_like(r), a * 0.95

def cell_tap():
    band = 1.0 - smooth(0.30, 0.42, np.abs(cy))
    endcap = 1.0 - smooth(0.86, 0.97, np.abs(cx))
    core = np.clip(1.0 - np.abs(cy) / 0.16, 0, 1) ** 1.5
    lum = 0.45 + 0.30 * np.clip(1.0 - np.abs(cy) / 0.4, 0, 1) + 0.35 * core
    return np.clip(lum, 0, 1), band * endcap

def cell_open():
    band = 1.0 - smooth(0.22, 0.34, np.abs(cy))
    endcap = 1.0 - smooth(0.92, 1.0, np.abs(cx))
    core = np.clip(1.0 - np.abs(cy) / 0.10, 0, 1) ** 1.4
    lum = 0.50 + 0.20 * np.clip(1.0 - np.abs(cy) / 0.3, 0, 1) + 0.40 * core
    return np.clip(lum, 0, 1), band * endcap

def cell_fret_ring():
    band = 1.0 - smooth(0.09, 0.15, np.abs(r - 0.80))
    inner = 0.30 * (1.0 - smooth(0.04, 0.08, np.abs(r - 0.66)))
    a = np.clip(band + inner + 0.15 * glow(1.0, 2.0) * smooth(0.8, 0.9, r), 0, 1)
    return np.ones_like(r), a

def cell_fret_pressed():
    disc = 1.0 - smooth(0.84, 0.95, r)
    core = 0.55 * glow(0.6, 1.2)
    ring = 1.0 - smooth(0.08, 0.14, np.abs(r - 0.80))
    lum = np.clip(0.45 + core + 0.5 * ring, 0, 1)
    return lum, np.clip(disc + 0.3 * glow(1.0, 1.8), 0, 1)

def cell_flash():
    star = 1.0 + 0.60 * np.cos(6.0 * theta)          # 6-arm burst
    a = np.clip(np.clip(1.0 - r, 0, None) ** 1.9 * star, 0, 1)
    a = np.clip(a + 0.5 * glow(0.35, 0.8), 0, 1)     # hot core
    return np.ones_like(r), a

def cell_trail():
    band = 1.0 - smooth(0.30, 0.42, np.abs(cx))
    core = np.clip(1.0 - np.abs(cx) / 0.10, 0, 1) ** 1.5   # energy center line
    lum = 0.55 + 0.15 * np.clip(1.0 - np.abs(cx) / 0.42, 0, 1) + 0.35 * core
    return np.clip(lum, 0, 1), band

def cell_shimmer():
    d = np.abs(cx + cy) / np.sqrt(2)
    a = np.clip((1.0 - smooth(0.08, 0.28, d)) * (1.0 - smooth(0.85, 1.0, r)), 0, 1)
    core = np.clip((1.0 - smooth(0.0, 0.06, d)), 0, 1) * (1.0 - smooth(0.7, 1.0, r))
    return np.ones_like(r), np.clip(a * 0.85 + core * 0.5, 0, 1)

# --- cells 9-14: juice pack ----------------------------------------------
def cell_ring():
    # thin expanding-burst ring with a soft glow skirt
    band = 1.0 - smooth(0.045, 0.10, np.abs(r - 0.80))
    skirt = 0.35 * (1.0 - smooth(0.10, 0.30, np.abs(r - 0.80)))
    return np.ones_like(r), np.clip(band + skirt, 0, 1)

def cell_spark():
    # teardrop streak along +x: bright head at x=+0.5, tapering tail left
    hx = (cx - 0.5)
    head = np.clip(1.0 - np.sqrt(hx * hx + cy * cy) / 0.28, 0, 1) ** 1.6
    talong = np.clip((cx + 0.9) / 1.4, 0, 1)                 # 0 tail .. 1 head
    tail = np.clip(1.0 - np.abs(cy) / (0.05 + 0.16 * talong), 0, 1) * \
        np.clip(talong, 0, 1) ** 0.7 * (cx < 0.5)
    a = np.clip(head + 0.8 * tail, 0, 1)
    return np.ones_like(r), a

def cell_glow_dot():
    return np.ones_like(r), glow(1.0, 1.1)

def cell_star_glint():
    ax = np.clip(1.0 - np.abs(cy) / (0.06 + 0.001), 0, 1) * (1.0 - smooth(0.75, 1.0, np.abs(cx)))
    ay = np.clip(1.0 - np.abs(cx) / (0.06 + 0.001), 0, 1) * (1.0 - smooth(0.75, 1.0, np.abs(cy)))
    a = np.clip(ax + ay + 0.6 * glow(0.28, 1.0), 0, 1)
    return np.ones_like(r), a

def cell_flame():
    # abstract symmetric energy plume rising to -y: teardrop with wavy edge
    ty = (cy + 0.55) / 1.35                       # 0 at top .. 1 at base
    width = 0.16 + 0.34 * np.clip(ty, 0, 1) ** 0.8
    wave = 1.0 + 0.16 * np.sin(9.0 * cy)
    body = np.clip(1.0 - np.abs(cx) / (width * wave + 1e-6), 0, 1)
    vert = (1.0 - smooth(0.0, 0.15, -ty)) * (1.0 - smooth(0.92, 1.05, ty))
    core = np.clip(1.0 - np.abs(cx) / (0.4 * width + 1e-6), 0, 1) ** 1.6
    a = np.clip(body ** 1.4 * vert, 0, 1)
    lum = np.clip(0.55 + 0.45 * core, 0, 1)
    return lum, a

def cell_gem_under_glow():
    return np.ones_like(r), glow(1.0, 0.8) * 0.9

# --- cells 50-55: GH-feel spec P2 hit/sustain flames -----------------------
# Flipbook plume: 4 frames of the same rising fire-magic tongue with the
# wave phase, tip sway and side lick advancing per frame, so stepping
# 50->53 reads as licking animation. Same white+alpha rules as every cell.
def cell_flame_fb(frame):
    def fn():
        # round 3 (field): fuller body - the first cut's narrow tongue
        # read as thin wisps even on wide quads
        ph = frame * (np.pi / 2.0)
        ty = (cy + 0.62) / 1.42                    # 0 at tip .. 1 at base
        tyc = np.clip(ty, 0, 1)
        width = 0.22 + 0.44 * tyc ** 0.80
        wave = 1.0 + 0.16 * np.sin(7.0 * cy + ph) + \
            0.08 * np.sin(13.0 * cy - 1.7 * ph)
        sway = 0.08 * (1.0 - tyc) * np.sin(ph + 2.2 * cy)
        dx = np.abs(cx - sway)
        body = np.clip(1.0 - dx / (width * wave + 1e-6), 0, 1)
        # round 4 (field): the old base fade ran 0.90->1.04 in ty - a
        # ~15px ramp right under the plume's widest part, which reads as
        # a hard horizontal cut once the quad is drawn 300px tall. Fade
        # over the whole lower third instead, and melt the plume into a
        # soft elliptical glow pool at its anchor.
        vert = (1.0 - smooth(0.0, 0.16, -ty)) * (1.0 - smooth(0.68, 1.02, ty))
        lickx = 0.38 * (1.0 if frame % 2 == 0 else -1.0)
        licky = 0.05 - 0.12 * np.sin(ph)
        lick = np.clip(1.0 - np.sqrt((cx - lickx) ** 2 +
                                     (cy + licky) ** 2) / 0.28, 0, 1) ** 1.4
        pool = np.clip(1.0 - np.sqrt((cx / 0.60) ** 2 +
                                     ((cy - 0.55) / 0.34) ** 2), 0, 1) ** 1.6
        a = np.clip(body ** 1.15 * vert + 0.60 * lick * vert +
                    0.55 * pool, 0, 1)
        a = a * (1.0 - smooth(0.90, 1.0, cy))   # nothing touches the edge
        core = np.clip(1.0 - dx / (0.42 * width + 1e-6), 0, 1) ** 1.4
        lum = np.clip(0.50 + 0.50 * core, 0, 1)
        return lum, a
    return fn

def cell_ember():
    # irregular glowing blob (fountain ember): angular wobble on the rim
    wob = 1.0 + 0.25 * np.sin(3.0 * theta + 0.8) + 0.15 * np.sin(5.0 * theta)
    blob = np.clip(1.0 - r / (0.55 * wob + 1e-6), 0, 1) ** 1.4
    a = np.clip(blob + 0.35 * glow(0.9, 1.5), 0, 1)
    lum = np.clip(0.55 + 0.45 * np.clip(1.0 - r / 0.30, 0, 1) ** 1.3, 0, 1)
    return lum, a

def cell_needle():
    # long thin streak along +x with a hot head (fast burst spark)
    hx = (cx - 0.62)
    head = np.clip(1.0 - np.sqrt(hx * hx + cy * cy) / 0.16, 0, 1) ** 1.5
    talong = np.clip((cx + 0.95) / 1.55, 0, 1)
    tail = np.clip(1.0 - np.abs(cy) / (0.022 + 0.075 * talong), 0, 1) * \
        talong ** 0.8 * (cx < 0.62)
    a = np.clip(head + 0.85 * tail, 0, 1)
    return np.ones_like(r), a

def cell_highway_fade():
    # Cell 56: highway floor depth-fade, a real per-pixel alpha ramp
    # (replaces DrawSurface's 64 flat-alpha strips - visible banding at
    # 16 steps, still visible at 64; field 2026-08-19). White so it tints
    # like every other sprite. cy=-1 (top of cell, v=0) is the horizon
    # edge, cy=+1 (bottom, v=1) is the strikeline edge - endpoints match
    # the old strip formula's 0.10/0.65 exactly so the look doesn't shift.
    t = (cy + 1.0) / 2.0
    a = 0.10 + 0.55 * t
    return np.ones_like(r), np.clip(a, 0, 1)

# --- cells 30/31/38: GH-feel spec P3 lightning-bolt segments --------------
# Jagged bright segment along +x with a soft glow skirt; drawn in game as
# oriented quads chained into a polyline (BoltOffsets jitters the nodes).
# Ends fade so chained segments overlap-blend; three kink variants.
def cell_bolt(variant):
    def fn():
        ph = variant * 2.1
        path = 0.16 * np.sign(np.sin(3.1 * cx + ph)) * \
            np.abs(np.sin(3.1 * cx + ph)) ** 0.6 \
            + 0.07 * np.sign(np.sin(6.7 * cx - 1.3 * ph)) * \
            np.abs(np.sin(6.7 * cx - 1.3 * ph)) ** 0.6
        d = np.abs(cy - path)
        # round 4 (field): core at 5% of the cell rendered as a hairline
        # once the quad was 10-20px thick - "1 pixel strike". Fat core,
        # fuller skirt.
        corel = np.clip(1.0 - d / 0.16, 0, 1) ** 1.2
        skirt = np.clip(1.0 - d / 0.48, 0, 1) ** 1.8
        endfade = 1.0 - smooth(0.88, 1.0, np.abs(cx))
        a = np.clip(corel + 0.55 * skirt, 0, 1) * endfade
        lum = np.clip(0.55 + 0.45 * corel, 0, 1)
        return lum, a
    return fn

def cell_trail_cap():
    # rounded tip for sustain-trail ends (field 2026-07-25: the flat cut
    # read as a hard edge). Horizontal profile IDENTICAL to cell_trail so
    # the cap's bottom edge is alpha-continuous with the trail end; each
    # column then fades out over a dome height proportional to
    # sqrt(1-(x/band)^2), giving a soft elliptical tip.
    ny = (1.0 - cy) / 2.0                        # 0 bottom edge .. 1 top
    band = 1.0 - smooth(0.30, 0.42, np.abs(cx))
    core = np.clip(1.0 - np.abs(cx) / 0.10, 0, 1) ** 1.5
    lum = 0.55 + 0.15 * np.clip(1.0 - np.abs(cx) / 0.42, 0, 1) + 0.35 * core
    hcol = 0.90 * np.sqrt(np.clip(1.0 - (cx / 0.42) ** 2, 0, 1)) + 1e-4
    dome = 1.0 - smooth(hcol * 0.55, hcol, ny)
    return np.clip(lum, 0, 1), np.clip(band * dome, 0, 1)

def cell_solid():
    # uniform fill for dims/flashes (a stretched gradient band leaves
    # faint edge falloff; this cell has none)
    return np.ones_like(r), np.ones_like(r)

cells = [cell_gem, cell_hopo_cap, cell_tap, cell_open,
         cell_fret_ring, cell_fret_pressed, cell_flash, cell_trail,
         cell_shimmer,
         cell_ring, cell_spark, cell_glow_dot, cell_star_glint,
         cell_flame, cell_gem_under_glow,
         cell_solid]

# --- banner text strips + digits (GH-feel spec P1.5) -----------------------
# White-core glyphs with a DARK baked stroke: the runtime tints the whole
# quad, so core lum 1.0 and stroke lum ~0.14 give a bright face with a
# near-black outline from one draw. Impact is a stock Windows face - a
# chunky condensed grotesque of our own choosing, not Guitar Hero's font.
FONT_PATH = "C:/Windows/Fonts/impact.ttf"

def render_text_strip(text, px_w, px_h, font_px, stroke_px):
    font = ImageFont.truetype(FONT_PATH, font_px)
    core = Image.new("L", (px_w, px_h), 0)
    full = Image.new("L", (px_w, px_h), 0)
    dc, df = ImageDraw.Draw(core), ImageDraw.Draw(full)
    box = df.textbbox((0, 0), text, font=font, stroke_width=stroke_px)
    ox = (px_w - (box[2] - box[0])) // 2 - box[0]
    oy = (px_h - (box[3] - box[1])) // 2 - box[1]
    df.text((ox, oy), text, 255, font=font, stroke_width=stroke_px,
            stroke_fill=255)
    dc.text((ox, oy), text, 255, font=font)
    a = np.asarray(full, dtype=np.float32) / 255.0
    c = np.asarray(core, dtype=np.float32) / 255.0
    lum = 0.14 + 0.86 * c          # stroke dark, core bright
    return lum, a

# (col, row, cols_wide, text, font_px): rows 2-6 of the atlas grid.
STRIPS = [
    (0, 2, 8, "STAR POWER READY!", 104),  # cells 16-23
    (0, 3, 6, "STAR POWER!",       112),  # cells 24-29
    (0, 4, 6, "NOTE STREAK!",      108),  # cells 32-37
]
DIGIT_ROW, DIGIT_FONT = 5, 116            # cells 40-49 = digits 0-9

atlas = np.zeros((CELL * ROWS, CELL * GRID, 4), dtype=np.uint8)
for i, fn in enumerate(cells):
    lum, a = fn()
    gx, gy = (i % GRID) * CELL, (i // GRID) * CELL
    tile = np.zeros((CELL, CELL, 4), dtype=np.uint8)
    v = np.clip(lum * 255, 0, 255).astype(np.uint8)
    tile[..., 0] = v
    tile[..., 1] = v
    tile[..., 2] = v
    tile[..., 3] = np.clip(a * 255, 0, 255).astype(np.uint8)
    atlas[gy:gy + CELL, gx:gx + CELL] = tile

def write_tile(lum, a, x, y, w, h):
    v = np.clip(lum * 255, 0, 255).astype(np.uint8)
    tile = np.zeros((h, w, 4), dtype=np.uint8)
    tile[..., 0] = v
    tile[..., 1] = v
    tile[..., 2] = v
    tile[..., 3] = np.clip(a * 255, 0, 255).astype(np.uint8)
    atlas[y:y + h, x:x + w] = tile

for col, row, cols_wide, text, font_px in STRIPS:
    lum, a = render_text_strip(text, cols_wide * CELL, CELL, font_px, 7)
    write_tile(lum, a, col * CELL, row * CELL, cols_wide * CELL, CELL)

# P2 flame cells sit at fixed indices (50-55, row 6): APPEND ONLY - moving
# any earlier cell repoints every later sprite (known pitfall). Cell 56 is a
# low-resolution fallback reference; runtime uses highway_fade.png instead.
P2_CELLS = [(50 + i, cell_flame_fb(i)) for i in range(4)] + \
    [(54, cell_ember), (55, cell_needle), (39, cell_trail_cap)] + \
    [(30, cell_bolt(0)), (31, cell_bolt(1)), (38, cell_bolt(2))] + \
    [(56, cell_highway_fade)]
for i, fn in P2_CELLS:
    lum, a = fn()
    write_tile(lum, a, (i % GRID) * CELL, (i // GRID) * CELL, CELL, CELL)

# P5 win-screen catchphrase strips (user-picked 2026-07-25; spec 12 bans
# "You Rock!"). Full rows for hero-moment crispness; row 7 cells 57-63
# stay free for P6. GLORIOUS! ties to the Glory meter; FLAWLESS! replaces
# it on a full combo.
WIN_STRIPS = [
    (8, "GLORIOUS!", 118),   # row 8 = v 0.8-0.9
    (9, "FLAWLESS!", 118),   # row 9 = v 0.9-1.0
]
for row, text, font_px in WIN_STRIPS:
    lum, a = render_text_strip(text, GRID * CELL, CELL, font_px, 8)
    write_tile(lum, a, 0, row * CELL, GRID * CELL, CELL)

for d in range(10):
    lum, a = render_text_strip(str(d), CELL, CELL, DIGIT_FONT, 7)
    i = 40 + d
    write_tile(lum, a, (i % GRID) * CELL, (i // GRID) * CELL, CELL, CELL)

os.makedirs(OUT, exist_ok=True)
path = os.path.join(OUT, "atlas.png")
Image.fromarray(atlas).save(path)
print("wrote", path, atlas.shape)

# The highway fade is deliberately separate from the 128px atlas cells.
# FLICK's highway quad can cover hundreds of screen rows; sampling the small
# alpha ramp there exposes its quantization as stationary horizontal bands.
# Stochastic alpha rounding distributes each fractional alpha level across x
# instead, preserving one continuous draw quad without coherent row edges.
FADE_W, FADE_H = 1024, 2048
fade_y = np.linspace(0.10, 0.65, FADE_H, dtype=np.float32)[:, None]
fade_alpha = fade_y * 255.0
fade_floor = np.floor(fade_alpha)
fade_frac = fade_alpha - fade_floor
fade_rng = np.random.default_rng(0xBA4D)
fade_noise = fade_rng.random((FADE_H, FADE_W), dtype=np.float32)
fade_a = fade_floor + (fade_noise < fade_frac)
fade = np.full((FADE_H, FADE_W, 4), 255, dtype=np.uint8)
fade[..., 3] = fade_a.astype(np.uint8)
fade_path = os.path.join(OUT, "highway_fade.png")
Image.fromarray(fade).save(fade_path, optimize=True)
print("wrote", fade_path, fade.shape)
