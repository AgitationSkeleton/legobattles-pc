# 195 — How the field scrolls, and the 3D HUD's screen lock

Status: **measured**, from the guest's own code and from the geometry the
renderer actually receives. Two things that were previously assumed are settled,
and one plausible theory is recorded as dead.

## The HUD is screen-locked at a constant depth

docs/194 established the in-field HUD is 3D. `gx_polygons`, driven through
`lb_exp.py --select`, says exactly which polygons and pins the discriminator
down. The whole scene is about ten quads — LEGO Battles draws units and
buildings as textured billboards, so there is very little geometry.

Nothing selected, three polygons, all world:

    z 7773696   x 212..267  y   8..64     castle
    z 7642624   x 203..227  y  56..80     unit
    z 7380480   x 227..252  y  88..112    unit

With a unit selected, seven more appear:

    z 4267520   x 100..108  y  16..40     HUD
    z 4267520   x 108..115  y  16..40     HUD
    z 4267520   x 100..108  y  40..64     HUD
    z 4267520   x 108..115  y  40..64     HUD
    z 4267520   x 100..123  y 168..192    HUD (cancel)
    z 4718080   x 227..249  y  78..82     health bar
    z 4718080   x 227..249  y  82..86     health bar

Coordinates are in the **widened 448 space**, so `x 100` is native x 4 — the
buttons are pinned against the DS screen's own left edge.

Then the camera was panned away. The world polygons leave the view entirely and
**the five HUD quads remain at byte-identical screen positions**:

    after pan, 5 polygons, all z 4267520, x 100..115 and 100..123

So the HUD is screen-locked at a constant z, observed rather than assumed. The
health bar is NOT: it shares a low-ish z but tracks its unit and disappears with
it, so any rule of the form "low z means UI" would wrongly anchor it. The two
constants are distinct — 4267520 against 4718080 — and the health bar sits in
the centre band, where the framework's existing left/centre/right anchor
semantics would leave it alone anyway.

**Why this is not being anchored yet.** At 16:9 the visible crop is roughly
columns 53..394 of the 448 surface. Anchoring a button to widened column 0 would
put it *outside* the crop, and anchoring it to the crop edge would put it in the
black pillarbox where there is no game. Bug 4 is therefore downstream of the
pillarbox: the field has to fill the window before "move the HUD to the corner"
describes anything a player would want.

## The field scrolls by rewriting the tilemap, not by scrolling the BG

`func_0202d1b4`, called from the scroll callback at `020396E0`, is a switch on a
layer index whose cases each write one register:

    0x04000010  BG0 H/V offset
    0x04000014  BG1 H/V offset
    0x04000018  BG2 H/V offset

as a single 32-bit store of `((-x) & 0x1FF) | (((-y) & 0x1FF) << 16)`.

The caller passes a value derived from the camera at `[view + 0x5E]` and
`[view + 0x60]` through the usual signed shift-and-rotate idiom, and the
callback then DMAs `0x680` bytes — 832 entries, **32 columns x 26 rows** — into
each of two staging buffers at `021523A0` and `02152A20`.

So the scroll is: rewrite the tilemap when the camera crosses a tile, and use
`BGxOFS` for whatever is left over. Measured across a full pan, sampling
`BG1HOFS` twelve times while the camera moved from grass out to open sea:

    BG1HOFS during pan: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]

**The terrain layer is never fine-scrolled.** Its offset is zero throughout, so
the camera is tile-aligned horizontally and the tilemap rewrite is doing all of
the work.

## Dead theory: the half-tile bias

The disassembly of the callback reads as computing `4 - (camx % 8)` before
handing the value to the setter. That looked like a complete explanation for the
left-edge sliver: a `+4` bias would place the visible window four pixels to the
LEFT of the written 32-column rectangle, so the first four pixels would come
from a tilemap column the game never writes — which matches the sliver's width
exactly, matches its staleness, and matches the earlier observation that the
first run of a tile decode covers pixels 4..7.

It is wrong, or at least it is not what happens. If the bias were reaching the
register, `BG1HOFS` would take a non-zero value for seven camera positions out
of eight. It is zero at every sample across a whole pan. Whatever the four-pixel
sliver is, it is not this, and the theory is recorded here so it is not
rediscovered and believed a second time.

Still standing about the sliver, from docs/193: it is columns 1 and 3 only, on
the top tile-row of each metatile for the first 7.5 metatile rows, screen
anchored, and stale across a pan. With `BGxHOFS` at zero, screen column 0 is
tilemap column 0 — which is inside the rewritten rectangle — so it is not a
partial tile scrolled in from outside either.

## What this means for the pillarbox

The BGs are **already 512x256 in VRAM** (`BG1CNT 4187`, `BG2CNT 4081`, size bits
= 1), which is 64 tile columns, and the game writes 32 of them. The space to
show more field already exists on the hardware side; what does not exist is
tile data in it.

Filling it means either making the guest write 56 columns instead of 32 — a
relocation of both staging buffers, the fill loop's column count, and the DMA
length — or having the host write the extra columns itself from the map data,
which this project already knows how to read (`lb_field.cpp` draws the whole
battlefield from it). The second is view-only and cannot desync (docs/85).
Neither is started.
