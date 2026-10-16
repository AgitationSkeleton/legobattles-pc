# 258 — Item #5: settled pans are clean, and what panning actually reveals

Item #5: panning the camera causes a rendering collapse where it drags
on-screen terrain and unit sprites with it instead of moving the camera beyond
where they were. This can even affect the HUD.

## Settled frames translate correctly

Free Play, 1920x1080, Enhanced, `--pan --pan-steps 6 --pan-hold 0.35`. The
camera walks 453 → 1104 in map pixels. Tracking the map's right green boundary
(a real, non-periodic feature) at rows where it is unambiguous:

```
camdx  96   boundary moves -192      ratio 2.00
camdx 112   boundary moves -224      ratio 2.00
```

Exactly the magnification, both times. **No smear, no lag, no collapse** in
settled frames.

### Two measurement traps hit on the way, recorded so they are not hit again

**Cross-correlation is useless on this game's terrain.** A first pass
correlated a terrain strip between frames and produced per-band shifts of
`0, 0, -192, -192, -288` for one camera step — which reads as a spectacular
smear. It is an artifact: the grass and rock tiles repeat on a ~16 px period,
so any shift that is a multiple of the period scores almost identically and the
search returns whichever multiple it meets first. The `0` and `+16` results in
the upper bands are aliasing, not evidence.

**`max()` over a colour mask jumps between disjoint objects.** Tracking the
"rightmost green pixel in a row" gave `-314` and `-102` where neighbouring rows
gave a consistent `-192`, because the row contained two separate green patches
and the rightmost one changed identity between frames. Same failure the
centroid note warns about: measure a run, not an aggregate.

## The captures were all taken after the motion stopped

Every frame in the pan loop was captured 0.15 s **after** the key release, so
the sequence only ever contained settled frames. A complaint about what happens
*during* a pan cannot be tested that way. `lb_exp.py` now splits the hold and
captures `panmid-NN.png` in the middle of it.

The mid-motion frames examined show no tearing and no duplication. **Item #5 is
not reproduced**, and nothing here should be read as showing it does not exist —
only that this route, at this speed, does not produce it.

## RETRACTED: "the world edge is not black"

The section below was written from three things, and none of them supports it.

1. **A log line from a different moment.** `cam_tx=161 cols -31..385` was taken
   from the END of the run. The frame classified against it, `panmid-00.png`,
   was captured while the camera was still between 424 and 984 — where little
   or none of the screen is off-map at all. The polylist comment in
   lb_patches.cpp warns about exactly this ("a tool pairing polylist frame N
   with lb_layers-...-N.pgm was comparing different moments and could not say
   so"), and it was walked into anyway.

2. **A geometry calculation resting on an assumed `band_left`.** The off-map
   boundary was put at window x≈1184 from `band_left = (946-256)/2`. The band
   **clamps at a map edge**, which is the whole reason `lb_band_left` is not
   half the overhang (docs/256). The run had `LB_HUDLOG` unarmed, so the actual
   band was never read, and at the right edge a clamped band would move that
   boundary a long way — possibly off the screen entirely, which would mean no
   off-map is visible and there is nothing to explain.

3. **"Rock edge to edge with no black"**, which is equally consistent with the
   map's own rocky border terrain. Texture variance across rows 700..1000 in
   64-px blocks is *flat* — 46.6, 47.3, 47.9 repeating for the entire width
   from x=64 to x=1792, with no discontinuity anywhere. If the world's edge
   were on screen, the fill past it would have to differ from the terrain
   before it, and no such boundary exists in the picture.

The `offmap=4632 zero=4632` counters are also not evidence for it: 4632/24 =
193 columns, which is the **extension table's** off-map extent (absolute cols
192..385 of a table spanning -31..385), not the screen's. The screen is 118
tiles wide.

**What would settle it**, for the next attempt: arm `LB_HUDLOG=1` so `band=` is
in the same log, capture at a known camera, and compute the boundary from the
measured band rather than an assumed one — or open the whole-map panel
(`LB_FIELDPANEL=1`), which knows the map's true extent, and see whether the
map's right region is rocky terrain in the first place.

## The original (unsupported) section, kept for the numbers it cites

`screenshots/exp-pan5b/panmid-00.png`, camera at map x 1288 of a 1536-px-wide
map. The port's own geometry line:

```
cam_tx=161 cols -31..385 (map 192 tiles)
```

So of 417 surface columns, **cols 192..385 — 194 of them, nearly half the
screen — are past the map's right edge**, and cols -31..-1 are past its left.
Classifying row 900 across the window finds rock edge to edge and no black at
all.

The port's goal is explicit: a wide view of the whole game field, with the
only black space being that outside the actual world boundary --
and `lb_widemap_offmap()` exists precisely to answer OFF-MAP so the caller
emits transparent pixels.

**It is answering correctly.** The port's own counters for that frame:

```
offmap=4632 zero=4632
[lb_wideR] right zeros per col h32..47: 24 24 24 24 ... (24 = the full height)
```

Every off-map cell is being zeroed, the whole right band included. So the
terrain layer is doing the right thing and **something underneath it is still
painting tiled rock** — a lower BG layer still drawing the hardware tilemap's
wrap, or a backdrop that is not black. That is the next thing to find.

## The hypothesis that rested on it is withdrawn too

The retracted section proposed that the off-map fill might be the whole of item
#5 — that panning toward an edge fills the screen with terrain repeating as the
camera moves, which would be reported as "it drags on-screen terrain with it".
That reading needed the off-map region to be both visible and wrongly filled,
and neither is established. **Withdrawn.**

## Where item #5 actually stands

- Settled pans translate exactly, measured on a non-periodic feature. Verified.
- Mid-motion frames show no tearing. Verified, on one route at one pan speed.
- Item #5 as described is **not reproduced**, and no cause is identified.
- Nothing is known about the world-edge fill either way.

Two open questions, since the route as driven does not produce the fault:

1. Does the collapse happen away from map edges, or only when panning toward
   one?
2. Does it need a faster or longer pan than one 1.2 s key hold — mouse-drag
   scrolling, or holding a direction for several seconds?

The answers decide whether the next attempt changes the route or the pan speed;
guessing between them would waste another attempt.

## Update: the fault, seen on a watched test run

On a watched harness run, moving the camera shifted the base and unit sprites
with it -- identifiable by where they overlapped the crystal trees -- and the
fog textures were also wrong.

That is a **relative displacement between sprites and terrain**, not the
whole-frame smear the earlier reading looked for — which is why every
terrain-only measurement above came back clean. Terrain translating exactly
does not rule this out; it is precisely what you would see if only the sprites
were wrong.

Attempted and NOT caught. In the one frame pair where both were measurable
(`exp-spr5`, panel off so the Top Screen's red UI cannot be mistaken for a
unit), a unit blob and the terrain blob moved **together**:

```
SPRITE (red)        396px  dx = -374      384px  dx = -374
TREE  (dark green) 11864px dx = -374
```

Two failed attempts to do better, recorded so they are not repeated:

- **The first sprite measurement tracked the Top Screen panel.** Red blobs were
  byte-identical across every frame at x 357..611, y 210..320 — the ImGui
  panel's red HUD, not field units. `LB_NOTOP=1` removes it from the picture.
- **Blob-matching terrain across a large camera move does not work.** The
  "biggest tree blob" went 16964 → 11864 → 6102 px between frames: different
  objects each time, so its `dx` is the disjoint-aggregate trap again.

A burst capture (four frames back to back mid-hold) is now in `lb_exp.py`, but
a 0.5 s hold moves the camera 471 → 1288 and takes the units out of frame, so
the burst only had one usable frame.

**The open question is whether the misalignment persists after the camera
stops.** If it does, it is a static offset and trivially measurable. If it only
exists during motion, it is a one-or-two-frame transient and needs the burst at
a much shorter hold with the base kept in view.

## Two observations that change the experiment entirely

Whether the misalignment persists, and how far the camera had moved:

- **It stays wrong after stopping.** It is a STATIC offset, not a transient.
- **It happens throughout, with any movement.** Not specific to map edges or
  to long pans.

Both burst-capture and frame-resolution sampling are therefore unnecessary —
two settled frames are enough, which is a far cheaper measurement than the one
being built.

It also explains the earlier null results precisely. `pan-before` vs
`panmid-00` measured `dRED = dTREE = -374`: correct *for that pair*, because
both were compared against the same starting frame and the offset had not yet
accumulated. What was never measured is sprite-vs-terrain at the SAME camera
value before and after a pan.

### The test to run: pan away, pan back, diff

Drive the camera right N steps and left N steps so it returns to its starting
value, confirm the camera really did return (read it, do not assume), then
compare the final frame with `pan-before`. Same camera, both settled, nothing
else varying — so any sprite-vs-terrain difference is the defect, with no
transient, no aliasing, and no disjoint-blob matching involved.

Deliberately NOT using `--nudge-cam` for this. It writes the camera field
directly, which bypasses the game's own scroll bookkeeping; a mismatch produced
that way would not be evidence about what the player sees when panning with the
keys.

## The null test, and the ruler that was broken first

`--pan-return` pans back and captures, so the same camera can be photographed
before and after. Two attempts.

**Attempt one closed on a lie.** It reported `camx 455 -> 456, residual 1` and
the frames differed by 128 window px, which looked like a 128 px drift at a
1 px camera difference. It was not. `port_camx()` parses the port's own
`[lb_band]` line *precisely because* the hardcoded view pointer at `0x02152034`
is unreliable — its own docstring records that read returning `camx -7889`,
tile -987 on a 192-tile map, while the port said `cam_tx=17` in the same
frames. `[lb_band]` only exists under **`LB_BANDLOG`**, nothing set it,
`port_camx` returned `None`, and the pan **silently fell back to the very
pointer it was written to avoid**.

The port's own `cam_tx` went 49 → 57 across those frames: eight tiles, 64 map
px, exactly the 128 window px at magnification 2. The renderer was correct and
the ruler was broken. `lb_exp.py` now arms `LB_BANDLOG` for any pan, the same
fix the selection probes needed for `LB_ENTLOG`.

**Attempt two, with the camera readback working.** `camx 120 -> 120,
residual 0` — the camera genuinely returned after travelling out to 1288.

```
SPRITES (red)     396px 24x22  dx = +542  dy = 0
                  384px 24x26  dx = +542  dy = 0
                  164px 14x22  dx = +542  dy = 0
TERRAIN (green) 11864px        dx =  +86  dy = 0
```

**A 456 px sprite-vs-terrain discrepancy at an identical camera**, and the
frame is 59.9% different overall. Visually: the base sits mid-left of the green
field above the dirt patch in one frame, and at the field's right edge beside
the dirt patch in the other. Blob sizes match exactly across both, so these are
the same sprites rather than different animation frames.

### Two controls this still needs before it is called confirmed

1. **Units walk.** The builder moves on its own over the ~20 s the pan takes.
   The exact size match argues against it (a walking sprite would change animation
   frame and therefore blob size), but the BASE is the immobile object and the
   measurement should be restricted to it.
2. **Game state differed.** `pan-return` shows the brick menu and deselect
   button; `pan-before` does not. Something was selected in one frame and not
   the other, so the two are not identical states.

Both are closed by one run: deselect first, measure the base only, and compare.

## The cause: the geometry band disagrees with the terrain band

`[lb_band]` prints two quantities: `left`, the band this frame computed, and
`usedx`, the band the geometry pipeline actually used. If those differ, sprites
are placed against a different origin than the terrain is filled with — which
is exactly a sprite-vs-terrain displacement.

### First reading: 99%, and it was the selection rule

Counting `usedx != left` over the logged lines gave 99% while moving. **That is
not a defect and is retracted.** The log fires *when the band changes*, so the
sample is almost entirely transition frames — and a transition frame is
precisely where the geometry pipeline legitimately still carries the previous
value. The comment beside the trigger says so in as many words:

> "a transition frame is exactly where the geometry pipeline is legitimately
> still carrying the previous width's value -- so every line said `used`
> disagreed with `left` and none of them said whether the steady state was
> right. That read as a broken hand-off and was not one."

Third sampling-bias trap of this investigation, after the 1-in-1009 scheduler
sampler (docs/254) and the one-frame tile histogram (docs/259). The shape is
always the same: a sample selected by the very property being measured.

### Unbiased reading

`LB_BANDLOG_ALL=N` now prints every Nth frame regardless of whether anything
moved. Over 1291 frames at a stable 946-wide surface with the camera located:

```
camera MOVING :  211 frames, usedx != left in  83  (39.3%)
camera SETTLED: 1079 frames, usedx != left in 195  (18.1%)

settled diff histogram (usedx - left):
   0: 884    -345: 108    -261: 21    -16: 5    -6: 2    -118: 2
```

**18.1% of settled frames disagree**, and in 108 of those 195 the gap is
**-345** — the entire half-overhang, `(946 - 256) / 2`. Geometry placed 345
surface px (about 700 window px at this magnification) from where the terrain
was filled.

That is a real defect, it is not a transition artifact (the camera is unchanged
between consecutive every-frame samples), and it matches the observed fault -- it stays
wrong after stopping and happens with any movement: roughly one
settled frame in six is drawn with the wrong origin for its sprites.

The magnitude also matches the earlier same-camera capture, which measured a
456 px sprite-vs-terrain discrepancy in window pixels.

**Next:** find why `g_band_used` takes 0 (or a stale value) while `g_band_left`
is 345, in the latch chain `lb_band_latch -> lb_band_for_geometry ->
lb_band_promote -> lb_band_render -> g_wide_3d_band[g_front]`. The two controls
on the capture-side measurement (units walk; the frames differed in selection
state) are now moot for the diagnosis, since this is read from the port's own
per-frame state rather than from pictures.

## Correction: `usedx != left` was the wrong pair to compare

The previous section framed `usedx != left` as the cause. That comparison is
against the wrong operand and the framing is withdrawn.

The chain is *designed* to have those differ. `lb_band_latch` records the live
band in `g_band_left`; `lb_band_for_geometry` records what the geometry
transform used in `g_band_used`; `lb_band_promote` carries it to
`g_band_render` when the geometry banks swap. The compositor is then supposed
to use the promoted value, and it does — gpu2d's field `extra` is
`g_wide_3d_band[g_front]` when the width matches, else `lb_band_render()`. The
design comment says the 2D layers may lag the live camera by a frame and that
this is the safe direction. So `used` differing from `left` is the mechanism
working, not failing.

Measured on the same unbiased sample, splitting the pairs properly (1291
frames, 946-wide, `geom=1`, real map):

```
MOVING   n= 211   usedx!=left 39.3%   renderx!=left 39.8%   usedx!=renderx  6.2%
SETTLED  n=1079   usedx!=left 18.1%   renderx!=left 18.5%   usedx!=renderx  0.6%
```

`usedx != renderx` is **0.6%** when settled — the promote hand-off is tight,
which is the part that could actually shear. And `campick age=0` on every
sample in the run: the camera the picture is drawn from is never stale.

**So no defect in the band chain is established, and the "cause" claim is
withdrawn.**

## What survives, and the one thing still unexplained

- The capture-side reproduction stands on its own: 456 px of sprite-vs-terrain
  discrepancy at a verified-identical camera, three blobs agreeing, `dy=0`.
  (Its two controls — units walk, selection state differed — are still open.)
- `renderx` differs from `left` on 18.5% of settled gameplay frames, and **108
  of those are off by exactly -345**, the full half-overhang `(946-256)/2`.

That last number is the thing to chase, and it is not obviously the designed
lag: a one-frame lag at a map edge is a pan step, a few pixels. A full 345 is
the band being completely clamped in one value and completely centred in the
other. The next question is narrow and concrete — **does the terrain extension
table get built against the LIVE camera while the composite reads it with the
PAIRED band?** If so the table lookup is offset by exactly their difference,
which is this number, and sprites would sit 345 surface px off the terrain.

## Third candidate cause, also negative: the paired camera is diagnostic-only

`choose_camera()` picks the terrain's camera by comparing candidates against
the guest's own BG1 tilemap, so the extension table is built for `g_cam_tx` by
construction. The geometry is placed against `g_cam_used_x`, promoted to
`g_cam_render_x` beside the 3D snapshot. If those two cameras disagreed, sprites
would sit off the terrain by exactly that — which is the shape of item #5.

Both were added to one `[lb_band]` line so the pair can never be read from
different moments, and measured over 1388 gameplay frames:

```
MOVING   n= 224  rendercam != fillcam  97.8%   diffs -26 -25 9 3 7 8  (pan-step sized)
SETTLED  n=1163  rendercam != fillcam  28.4%   diffs 0:833  -658:105  -530:22
```

The moving diffs are pan-step sized, which is the designed one-frame lag. The
settled ones include 105 frames at **-658 px**, which is not a lag at all.

**It still does not establish a fault.** `g_cam_render_x` has exactly one
reader — `dump_polygon_list_if_asked()` under `LB_POLYLIST` — so it is
**diagnostic only and drives no pixel**. The compositor pairs on the BAND
(`g_wide_3d_band` / `lb_band_render`), not on this camera. A -658 px
disagreement in a variable nothing renders from cannot displace a sprite.

## Item #5: three candidate causes, three negatives, and a stop

- **Off-map world-edge fill** — retracted (d89cb035): a log line paired with a
  frame from a different moment, an assumed `band_left`, and flat texture
  variance across the full width.
- **`usedx` vs `left`** — withdrawn (52f6858d): the chain is designed for those
  to differ; the compositor uses the promoted value; `usedx != renderx` is only
  0.6% settled; `campick age=0` always.
- **`rendercam` vs `fillcam`** — this section: the variable is diagnostic-only.

Each was a real measurement of something that turned out not to drive the
picture. What still stands is the reproduction itself: **456 px of
sprite-vs-terrain discrepancy at a verified-identical camera**, three blobs
agreeing, `dy=0` — with its two controls (units walk; selection state differed)
still open.

Stopping here and moving to untouched items rather than spending a fourth
iteration. The next thing to try, when it is picked up again, is to stop
guessing at which paired quantity is wrong and instead instrument the ACTUAL
consumer: log the `extra` that gpu2d's field composite uses and the
`g_fill_cam_tx` the table was built with, on the same line, in the compositor
itself. Those two are what the picture is made from; everything measured so far
has been upstream of them.
