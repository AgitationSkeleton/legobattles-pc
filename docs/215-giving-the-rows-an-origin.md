# 215 — giving the rows an origin

**Goal this serves.** An adjustable window size with every component scaling
consistently across window sizes: a larger window must not just zoom the game
in or preserve its aspect. The window has to buy field
of view in **both** axes, not just horizontally.

## Where this picked up

docs/213 got the vertical render path working. `LB_VFOV=1` gave `guest=472x256`
filling a 1892x1031 window at constant magnification. It was left **opt-in**
because the extra rows showed a band of stale sprite tiles across the top of the
screen: `lb_widemap` gave COLUMNS an origin (`kExtLo = -16`, indexed
`h - kExtLo`) and ROWS started at zero, so any row outside the guest's own
window fell through to whatever the tilemap happened to hold there.

That band is now gone and the height adapts by default.

## What was actually wrong

Two separate things, and only the first was the one that had been written down.

**1. The table had no row origin.** `const int kExtRows = 26;` with every
supplier doing `if (trow < 0 || trow >= kExtRows) return 0;`. Fixed the same way
the columns were: `kExtRowLo = -8`, `kExtRowHi = 33`, indexed `trow - kExtRowLo`,
with `g_ext`/`g_ext2`/`g_ext3` widened to match and the fill loops extended.

**2. The row index handed to the table was WRAPPED.** This is the one that
mattered more, and it was not in the plan.

```c
const int lb_tr = (int)(line.sy >> 3);
```

`line.sy` is `(bgy + y) & 0x1FF`, then `& 0xFF` for a 256-tall map. It is the
**hardware** row, 0..31. Guest scanline −1 arrives as `sy = 0x1F8` and keys to
row **31** — a row the game never maintains — so the table was being asked about
row 31 rather than row −1, and would have answered about row 31 even with an
origin in place. It is unwrapped now exactly as the column already was:

```c
const uint32_t lb_sy_mask = (size & 2u) ? 0x1FFu : 0xFFu;
const int lb_yspan  = (int)lb_sy_mask + 1;
int lb_ybias = (int)(u.bgy[bg] & lb_sy_mask);
if (lb_ybias >= lb_yspan / 2) lb_ybias -= lb_yspan;
const int lb_hwy = y + lb_ybias;
const int lb_tr  = (lb_hwy >= 0) ? (lb_hwy >> 3) : -((-lb_hwy + 7) >> 3);
```

## A widened ROW is not a widened COLUMN

A widened column is outside the native picture by definition, so the rule
"hardware tile columns 0..31 are the guest's, everything else is ours" holds
without qualification. A widened **row crosses straight through the middle of
the native picture**, so the same rule cannot be stated in columns alone — the
extra rows need the native 32 columns supplied too, or the top and bottom bands
draw stale tilemap through the middle third of the screen.

That raised the question the plan expected to answer with a number: how many
tile rows does the guest maintain?

## The number was a trap, and the measurement says why

The alignment argument gives 26. `func_020A3234` forces the camera's tile row
even (`cam_ty = (camy / 8) & ~1`) because a metatile is two tile rows tall; the
0..15 px that throws away comes back as `BG*VOFS`, so 192 visible pixels can
need `ceil((192 + 15) / 8) = 26` rows.

The first measurement appeared to refute it. A per-row diff of this port's own
terrain reconstruction against the guest's BG1, over all 32 rows of the screen
block:

```
[lb_rows] guest BG1 row agreement (0=exact 1=few 2=many 9=offmap):
          00000000000000000000000022222222  last exact row 23
```

Exact through 23, wholesale disagreement from 24 on, on every report of a
five-second run. **Real reading, wrong conclusion.** The camera had come to rest
at an aligned position. Panning vertically settles it:

```
[lb_3dedge] bgx 0 508 508 508  bgy 0 8 8 0
[lb_3dedge] bgx 0 508 508 508  bgy 0 13 13 0
[lb_rows] ... 00000000000000000000000000222222  last exact row 25
```

`bgy` goes to 8 and 13 on BG1 and BG2 while the camera moves, and the same diff
then reads exact through row 25. So 26 is right — **while the camera is
moving**. Parked, rows 24 and 25 are never sampled and hold whatever was last
written there.

A fixed count would therefore have to be the maximum, 26, which then defers rows
24..25 to stale data whenever the camera is still — a strip of old terrain and
old fog across the bottom of any window taller than the DS. Exactly the artefact
this step exists to remove, moved from the top of the screen to the bottom.

BG3 makes it worse: its `bgy` stays 0 throughout, so it would want a different
count from BG1 and BG2.

## The rule that replaced it

A maintained row is not a property of the layer. It is a property of the moment:
**the guest keeps a row up to date exactly while its own 192-row picture reaches
it.** The decoder already knows that directly — it is handed the guest scanline
`gy`, and `gy` outside `0..191` is a row the game does not draw and therefore
does not maintain, whatever the scroll offset happens to be.

```c
inline bool guest_cell(int htile, int gy) {
    return htile >= 0 && htile < 32 && gy >= 0 && gy < 192;
}
```

No per-layer table, no count to keep in sync with the cartridge, and the same
guarantee the columns have always given, stated for rows: inside the guest's own
scanlines nothing is substituted in the native columns, so the game's picture is
bit-for-bit identical. `[lb_terrain] mine vs guest BG1: 0/768 differ` holds on
every frame of every run since.

One consequence worth stating: with a non-zero scroll offset, guest scanline −1
can land on tile row 1, which IS a row the guest maintains. Substituting there
is still correct — the reconstruction is bit-exact for the terrain and comes
from the same `g_seen` record for the fog — but it does mean **no cell can be
skipped when filling the tables**, native columns included.

## What else moved with the height

* `lb_wide2d.inc`'s two `for (int y = 0; y < 192; ++y)` loops now write at
  `y + extra_y`, so a 2D scene is CENTRED in a taller output instead of parked
  at the top, and `lb_fill_rows_outward` carries the top and bottom rows outward
  — the vertical counterpart of `lb_edge_fill`'s horizontal carry.
* The two "copy the native picture" paths inside this port's own hooks (engine B
  and a non-3D engine A scene) call it too. docs/213 flagged both as owed.
* `lb_hud.cpp` needed nothing: it indexes by OUTPUT row throughout, and melonDS's
  viewport shift has already moved the 3D geometry down by `extra_y`.

## Measured

| window | guest surface | vs the DS |
|---|---|---|
| 640x480   | 256x192 | 1.00x (pure vanilla) |
| 1024x768  | 266x192 | 1.04x |
| 1280x720  | 358x192 | 1.40x |
| 1600x900  | 392x212 | 1.69x |
| 1920x1080 | 472x256 | **2.46x** |

42 fps in gameplay. `[lb_terrain] 0/768 differ` throughout. The 1920x1080
capture (`screenshots/exp-vfov6/game-1920x1080.png`) shows clean terrain from
the top edge to the bottom edge with no stale-tile band.

The ceiling is the vertex format, not a preference: `FinalPosition[0] & 0x1FF`
and `FinalPosition[1] & 0xFF` cap the raster at 512x256, which is why 1080p
lands on 256 rows exactly and scales up from there.

## Still open on the height

* **Vertical HUD anchoring** — done in part two below, along with the two things
  it turned up.
* Fog in the new rows defaults to shrouded until the camera has covered that
  ground, which is the same conservative default the widened columns have and
  errs in the safe direction. In a run that never pans vertically that shows as
  a solid grey band top and bottom; a single vertical pan clears it.

---

## Part two: the HUD, vertically

Anchoring the HUD sideways was docs/212. The vertical twin of that problem
arrived with the rows: melonDS centres all 3D geometry in a taller raster, and
the in-field HUD **is** 3D geometry, so a button the game put 16 px from the top
of its 192-row screen is drawn `extra_y` rows further down and floats away from
the window edge. Measured at 1920x1080 (`shift=107,32`):

```
[lb_hud] 5 corner element(s) moved, surface=472x256 visible=470x256 shift=107,32
[lb_hud]   x 112..119 y 48..72  -> -107,-32     top-left bricks
[lb_hud]   x 112..119 y 72..96  -> -107,+0      middle: neither edge
[lb_hud]   x 112..135 y 200..224 -> -107,+32    bottom-left icon
```

`HudBox` gained a `dy`, `lb_hud_scan` an `output_height`, and the classifier the
same edge test on `ny0`/`ny1` that it already applied to `nx0`/`nx1` — with its
own cap, `kCornerMaxHeight = 32` rather than 64, because the native screen is
192 rows against 256 columns and 64 would have admitted the health bars.

A purely horizontal move happens inside the line the pixels arrive on. A
vertical one cannot, so it goes through the same previous-frame capture that
scaling already used, for the same reason and with the same invisible one frame
of latency on a screen-locked HUD.

### The measurement that made the probe honest

Every "select a hero" run since docs/212 came back with the in-field HUD absent
and only the always-present quads in the polygon list, and the reason was in the
harness rather than the port: `aim_hero` read only the hero's `sx`, and its
caller then tapped a hard-coded `(128, 100)` hoping the hero was near it. The
port prints `sy` and `owner` as well. `hero_pos()` now reads all three, taps
where the player's own hero actually is, and re-reads before each attempt
because a hero walks. The first run with it selected the Space Criminal Leader
on the first try — portrait and 1100/1100 on the top screen, and the blue and
orange bricks anchored to the window's left edge.

### ...and then the anchored button vanished

Measured, not inferred, because two readings of the same capture fitted either
answer:

```
[lb_prio] BG0 2  BG1 3  BG2 1  BG3 0  (0 = frontmost)
```

The in-field HUD is 3D geometry on **BG0 at priority 2**; the shroud is **BG2 at
priority 1**, in front of it. On the DS that never shows, because the HUD's
corners sit over the player's own ground. Anchor a corner into a widened band
and it lands on cloud: the bottom-left element at surface y 232..255 measured
100% fog with not one pixel of the button in it, and the top-left brick showed
only the 8 rows that happened to fall inside the guest's own picture. Anchoring
a button into invisibility is worse than leaving it floating.

The fog out there is not the game's — it is this port's conservative default for
ground the guest's 32x24 window has never covered — so declining to draw it
under an element this port has RELOCATED reveals nothing: the element is opaque
and covers those pixels anyway. `lb_hud_cover_row` marks only the pixels an
opaque HUD pixel was actually written to, and the compositor skips BG2 and BG3
there. The brick now draws solid across all 24 of its rows in the band.

### The touch mapping had a 192 in it too

```c
const float gy = (window_y - fy) * (float)kScreenHeight / (float)fh;
```

Right only while every surface was 192 rows tall. On a 256-row surface every
click landed a third of a screen below where the player aimed. It divides by the
surface's own height now, and the guest gets `gy - (gh - 192) / 2` for the same
reason x gets `gx - (gw - 256) / 2`. `lb_overlay_set_field_rect` carries the
guest height for it, and `lb_hud_unmap_touch` takes `gy` by pointer so it can
undo a vertical move as well as a horizontal one.

Verified end to end: the hero is selected by a tap that goes through the new
mapping, on the first attempt, at 472x256.

### Open, and cosmetic

The widened band draws its fog with a 1-px dither of cloud over grass where the
native columns show none — a fog-edge variant being chosen for interior cells.
Pre-existing (docs/203's edge learner is the attempt at it) and unrelated to the
rows.
