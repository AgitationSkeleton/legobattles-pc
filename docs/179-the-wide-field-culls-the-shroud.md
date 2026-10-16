# 179 — The wide field culls the shroud, and three sprite defects behind it

Status: **three defects fixed and verified; one root problem found and NOT yet
fixed.** The starting defect: in captures of the camera panning past the DS
culling boundary, units were cut off or repeated in fragments.

That was accurate, and chasing it found something larger than the fragments.

## The grey rectangle is the shroud

Scrolling the camera into unexplored ground and dumping the field produces a map
that is fully drawn everywhere **except** a grey rectangle exactly where the
camera is (`screenshots/20260831-144855-sprite-fragments/field.03_full.png`).

That is the wrong way round, and it says exactly what is broken:

* `lb_field_render()` draws the whole battlefield from the **level grid** — the
  raw metatile indices in main RAM. The grid records the terrain, and knows
  nothing about who has seen it. So the host draws the entire map revealed.
* `lb_field_composite_guest()` then pastes the guest's own 256x192 over the
  camera rect. The guest **does** apply the shroud, so unexplored ground inside
  the camera comes back as the grey shroud texture.

So the grey block is the only correctly-fogged part of the picture, sitting in
the middle of a map the port has revealed wholesale.

Verified as shroud rather than mis-registration: dumping over **explored** ground
(`explored_camera.png`) shows the guest paste blending seamlessly into the host
terrain — the treeline and the shoreline continue across the camera boundary
without a seam. The camera-to-map mapping is right; the difference is fog.

**This breaks a core requirement.** The widened camera must not cull units,
buildings, fog-of-war, shroud or particles. The wide field
culls the shroud completely: it reveals the whole battlefield, which is both a
rendering artifact and a gameplay cheat granted for free.

## What is known about the fog state, and what is not

docs/163 found a per-tile bitmask at `view+0x164`, sized `(width+7)/8` bytes per
row for 32x24 tiles. That is the **viewport** mask — 96 bytes for one screenful.
It is not the map-wide record of what the player has explored, which is what
drawing a 1536x1008 field needs.

The Reveal Map cheat (`flags[0]`, docs/171) does drive real map-wide state:
enabling it turns the minimap from a small blob around the base into the entire
battlefield (`shroud-hunt/minimap_revealed.png`) — and that revealed minimap
matches the host-rendered whole field closely, which is a pleasing independent
check on the field renderer itself.

Diffing 4 MB of main RAM across the cheat, **with a control diff taken over the
same number of cycles to subtract ordinary frame churn**, leaves 7040 bytes
attributable to the cheat. The largest coherent signal is a family of 384-byte
runs on a 0x200 stride from `022BA63C` to `022BBDBB`, every one of them going
from **all zero** to 4-bit-packed values drawn from {4,5}. Their addresses sit
immediately around the metatile tables (`primary=022BBA66`).

That is recorded as a **candidate, not a conclusion.** Rendering those regions at
several row widths shows banding on the 0x200 stride, not map-shaped structure,
so nothing here yet identifies a 64x63 visibility array. Guessing which of these
is the fog and drawing from it would be invention, not a reading of the ROM.

The next experiment is the one the cheat cannot perform: reveal ground
*naturally*, by ordering a unit into the dark, and diff. Natural exploration must
write the persistent array, and it writes a small disc rather than everything,
which is a far more diagnostic signature than a wholesale reveal.

## Three sprite defects, fixed

All three are in the learned-sprite reconstruction that draws units the guest no
longer renders once they leave the camera.

**1. Shroud was being cached as a unit.** The learner's premise is "the unit is
whatever differs from the terrain underneath it". Over unexplored ground that
premise fails outright — the game draws shroud over the tiles, so *every* pixel
differs, and a 40x40 square of shroud texture was cached and then stamped at that
entity's position for as long as it lived. Patches that disagree with terrain
almost everywhere (>85%) are now rejected: wholesale disagreement means the
terrain reference is invalid there, so the patch says nothing about the unit.

**2. Sparse noise was being cached as a unit.** The acceptance test was
`differing >= 24` — twenty-four pixels anywhere in sixteen hundred. Animating
water, flowers and the shroud edge all clear that bar, which is literally what
"fragments" looks like. The patch is now cropped to the blob of differing pixels
**connected to the centre**, which drops both scattered noise and any neighbour
standing inside the 40x40 window (docs/174 measured three units inside 80 px in
this very save), and the blob must be unit-sized.

**3. Stamps were painting over the guest's own rectangle.** The guard skipped an
entity whose *anchor* was inside the camera, but the guest clips sprites at the
screen edge, so an entity within 20 px of it was pasted in as a half unit — those
are the units that appeared **cut off**. Requiring the whole patch inside instead
let the cache complete the clipped half.

That first attempt then produced something worse, and it is worth recording why:
the castle appeared **twice**, once from the guest and once from a stamp offset
up and left of it. Point-unit anchors are good to about +/-8 px (docs/175) but
structure anchors are off by tens of pixels, so a stamp allowed over the camera
rect draws a second, misplaced copy of a building the game drew once. Learned
sprites are now clipped to strictly outside the camera rect: the guest's
rendering is authoritative where it exists, and the cache only fills ground the
guest does not draw.

Verified in `screenshots/20260831-150328-sprites-clipped/clipped_crop.png`: one
castle inside the camera box, clipped by the game's own screen edge, with the
soldier and builder each drawn once.

## What is still wrong, stated plainly

**RETRACTED in part -- see docs/187.** The diagnosis below blamed the entity
anchor. The actual cause was that the sprite learner flood-filled from a +/-8
disc around the patch centre and so captured almost nothing at all -- one
sprite in 640 frames -- and the few blobs it did catch were often a NEIGHBOUR
that happened to touch the disc. A neighbour stamped at this entity position is
the stray building. With blobs labelled properly the measured anchor error is
single digits, not tens of pixels, and the stray stamp is gone.

A stray castle-like stamp remains **outside** the box where no building is. The
compositing is now right; the registration is not. Entity records place
structures tens of pixels from where their sprites are drawn, so a structure's
learned patch is captured against the wrong terrain and stamped in the wrong
place.

More honestly still: reconstructing off-camera units by differencing the guest
against host terrain is a stopgap, and it will always be approximate. The durable
fix is the one docs/162 and docs/164 point at — stop the game culling units
outside the stock viewport so it renders them for real. Until then the
reconstruction should be judged as an approximation, and this document is the
record that it is one.

---

# Three failed attempts to locate the map-wide visibility array

Recorded because negative results are worth keeping, and because the next
person should not repeat them.

**1. Diff across the Reveal Map cheat.** Documented above. Left 7040 bytes after
control subtraction; the largest coherent family was the 0x200-stride runs near
the metatile tables, which render as banding rather than map structure.

**2. Diff across a teleported unit.** With `NDS_DEBUG_ALLOW_WRITES=1` a unit can
be moved anywhere by writing `+0x0EC` / `+0x0F0`, and the write sticks — the
entity reads back at its new map position. Moving one into unexplored ground and
diffing (control-subtracted) put `02197900..021988FF` at the top of both
positions' change lists. That region is **4096 bytes, exactly 64x64** for a
64-wide metatile grid, which looked decisive.

It is not the fog. Rendered as a 64x64 image it is noise, ~4000 of 4096 bytes are
nonzero at all times, and the byte at the unit's own predicted cell took values
249, 233, 248 and 19 across four positions. It is a buffer that churns every
frame and it appears in every diff for that reason.

**3. Filtering for zero -> nonzero transitions.** A newly explored cell should go
from clear to set, and that filter should cut through churn. It reduced the
change set to 103-191 bytes per move, but the gap histogram is dominated by 1 —
contiguous bytes, i.e. buffers being filled — with no row stride emerging at 32,
64, 63 or 128. No candidate survived.

**What this rules out, and what it does not.** It does not show there is no such
array. It shows that main-RAM diffing cannot find it here: too much of RAM churns
per frame, and the disc a single unit reveals is small enough to hide in that.

The method that has not been tried is reading code rather than memory. The
minimap **must** read the map-wide state to draw explored ground, and docs/180
has now measured that minimap's transform in detail. Finding what the minimap
renderer reads is a bounded, evidence-led search, and it is the next thing to do
rather than a fourth diff.

---

# A fourth failed search, and why the target changed anyway

Following docs/182's reading of C&C -- where visibility is per-CELL state inside
the map cell rather than a standalone array -- the search was narrowed from
"diff 4 MB" to "find something map-shaped".

The premise was stronger than any diff: at the start of a match the explored
region is a compact disc around the player's base, at metatile cell (8,9). Any
array recording exploration must show that blob and be empty elsewhere. That is a
SHAPE test, needing one sample and no cheat.

Scanned all of main RAM at three encodings:

    1 byte per cell   (64 x 63 = 4032 bytes)   29 candidates, ALL inside one
                                               array at 0231.., 64 bytes apart
    1 bit per cell    (8 bytes/row, 504 total) 15 candidates, scattered, 12-23 set
    2 bits per cell   (16 bytes/row, 1008)     35 candidates, scattered, 5-20 set

The byte-per-cell hits looked convincing -- 29 candidates on a 64-byte stride is
exactly what one 64-wide array produces under a sliding window. Rendered, it is
two symmetric tapering cones: a precomputed geometry table, not a map. It matched
only because its centroid happens to fall near the base cell.

The bit-per-cell and 2-bit hits are scattered singletons with five to twenty-odd
bits set, none forming a coherent array.

**So: no map-wide visibility array in main RAM at any of the three obvious
encodings.** Recorded as a fourth negative. The remaining possibilities are that
it is not 64-wide, not in main RAM when sampled, or -- following C&C properly --
a field inside a wider per-cell structure whose stride is not 1, 2 or 4 bits.

## Why this still matters

A mockup of the intended result (`docs/widescreenmockup.png`) settles the
architecture question that was driving this. It shows the whole battlefield
host-drawn across a ~1400x870 window, units and buildings of both teams at their
true positions, and the top screen as a small panel in the corner. The wide field
already has that structure. What it does not have is clean units.

The mockup shows no shroud, and that is **not** a statement that the shroud can
be dropped: the image assumes an already-revealed map. Fog remains required,
and drawing the whole battlefield unshrouded remains the defect this document
opens with. What the mockup changes is priority,
not scope -- unit fidelity is a separate problem from fog, and the sprite rip
gives a better answer to it than the differencing learner ever could.
