# 245 — The view height was never widened, and why that alone does not fix the fog

Status: **one asymmetry closed, the fog cause isolated.** Follows docs/244.

## The asymmetry

`Set_View_Dimensions` (docs/183) passes both dimensions:

    020333A4  mov r1, #0x20     width  in tiles    <- apply_view_width
    020333A8  mov r2, #0x18     height in tiles    <- nothing, until now

The setter frees the old buffers, stores width and height, computes the fog
mask stride as `(width + 7) / 8`, and allocates the mask at `stride * height`
and the row table at `height * 4`. The height is as much a part of that
reallocation as the width, so raising only the width gives a view that is
wider and still 24 tiles tall.

This is the same shape of bug the entity gather rect had, where only the
horizontal half was widened. Fixing that one did not fix this one: the gather
rect decides what is **submitted**, the view dimensions decide what the game
considers **visible**.

`apply_view_height` now patches `020333A8` the same way, on its own switch
(`LB_VIEW_HEIGHT`). Kept separate from `LB_VIEW_WIDTH` deliberately — docs/231
measured the width knob degrading click selection, and until that is understood
the height must not ride in on the same variable, or an A/B of the width is
really an A/B of both and neither can be read.

Verified firing:

    [lb_fov] view width 32 -> 64 tiles (512 px); the setter reallocates the fog mask to 192 bytes
    [lb_fov] view height 24 -> 48 tiles (384 px); the setter reallocates the row table to 192 bytes

## What it did not fix, and what that proves

The measurement that sent me here: with `LB_ASPECTBOX=1` outlining the guest's
own 256x192 inside the widened surface, freeplay shows revealed terrain whose
**top and bottom edges are perfectly straight and lie exactly on the box**,
while its right edge is ragged. A ragged edge is real exploration; a straight
edge on the native aspect is a clip.

Running the same route with the view widened to 64x48 tiles changed the
picture by 19.8% of pixels — and **the fog boundary did not move**. The
difference is entity animation and the hero's position, not fog.

That is the useful result. It rules out the game's view size as the cause and
names the real one: the port's widened columns do not read the game's fog at
all. They read `g_seen`, a **window-derived cache** of which map tiles the
guest's own 32x24 window has personally covered, defaulting everything else to
shrouded. An RTS camera pans sideways far more than vertically, so the rows
above and below the camera's track were never "covered" and are drawn as solid
cloud however explored the game considers them.

So the fog fault is a **memory** fault, not a rendering one, and widening the
game's field of view is necessary but not sufficient: something must also read
the wider record back.

## Why not simply switch to the map-space source

`g_fogsrc_map` exists for that and **defaults to 0**, contradicting the long
comment directly above it which says it is on. Both settings are wrong, in
opposite and separately-reported ways:

* **window cache** (current default) — fog square around the original aspect,
  base and units cut off at it.
* **map grid** — the candidate search adopts a wrong buffer, paints scattered
  cloud over explored ground, drops it, adopts the next one. Reported as
  "spotty fog … it also creeps onto the traversed area where the base and
  units are".

Metatile-phase misalignment was already ruled out as the grid's fault
(88.6 / 94.6 / 94.3% agreement across the three residues — all alike, which is
the signature of a wrong record rather than a misaligned one).

The candidates are found by correlating against `g_seen`, which is itself the
approximation. That is the flaw: the search grades candidates against a proxy,
so a buffer that matches the proxy's own blind spots scores well.

## Next

With the view now widened in both axes the game maintains its fog mask over
the full widened area, and the mask's descriptor is reachable: `0x02151F28`,
stride at `+4`, width at `+8`, row table at `+0x14`, read bit-wise as
`row[tx>>3] & (1 << (tx & 7))` (`func_020EEBF4`). Reading *that* for the
extended columns, instead of `g_seen`, is the fix — and it needs no candidate
search, because the address is the one `Set_View_Dimensions` is handed.

One caution before building on it: the harness currently reports
`fog after pan: 0 bits set` from that descriptor, so either it is a
currently-visible mask rather than an explored one, or the read is wrong.
That has to be settled first; it is the difference between fog and shroud.
