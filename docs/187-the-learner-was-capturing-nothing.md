# 187 — The sprite learner was capturing almost nothing

Status: **fixed and measured.** Retracts docs/179's claim that structure anchors
are off by tens of pixels — that number was produced by the bug, not by the
game.

## The bug

`lb_field_learn_sprites()` flood-filled from a **+/-8 pixel disc around the patch
centre**, on the assumption that an entity's record points at its own sprite.

It does not. With the 3D layer giving an exact stencil (docs/184), the blob can
be located precisely, and a category-0 sprite sits about `(-4,+5)` from where the
record predicts — with individual samples further out. A seed disc of +/-8
therefore lands on empty pixels whenever the offset exceeds it, the fill finds
nothing, and the entity is never learned.

**Measured: over 640 frames of ordinary play the learner captured ONE sprite.**
That is the real reason off-camera units looked wrong. Not a registration error
to be corrected with a better constant — the cache was essentially empty, and
the few entries in it came from whatever blob happened to touch the seed disc,
which is frequently a *neighbour*. A neighbour stamped at this entity's position
is exactly the ghost building reported.

## The fix

Label every blob in the patch, then take the one whose centroid is **nearest the
patch centre**:

* Not the largest. A neighbour standing nearby is a bigger blob than a distant
  minifig, so a largest-blob rule reproduces the ghost it is meant to remove.
* Not seeded from an assumed anchor. Proximity ranks candidates without needing
  the anchor to be right, which matters because the anchor is what is in doubt.

The capture window went from +/-20 to +/-28 so a sprite offset by a couple of
dozen pixels is inside it at all.

The chosen component's centroid is stored per entity and averaged per category,
so the anchor is now **measured rather than assumed**, and `LB_DIAG=1` reports it.

## What the measurement says

    category 0   mean sprite offset  (-4.0, +5.0)
    category 1   mean sprite offset  (+8.0, -11.0)

Both are small. docs/179 said structure anchors were off "by tens of pixels" and
treated that as a fact about the game; it was a fact about the broken fill, which
reported `(-16,-12)` for the one sprite it managed to capture and would have
reported whatever a stray neighbour's centroid happened to be. **That claim is
withdrawn.**

Stated honestly: these are **single logged samples**, not converged averages —
the log fires on every fortieth sample and neither category reached forty in the
run. They establish the order of magnitude, not the value.

## Verified

`screenshots/20260831-195338-anchor2/reg_crop.png`, camera scrolled off the base.
The structure and the unit outside the camera box each draw **once**, in the
right place, with terrain continuous across the boundary. The ghost building of
docs/179 is gone.

## Still short of the mockup

Two of the level's six entities are drawn off-camera, not all of them. An entity
is only learned while it is inside the camera with its blob big enough to clear
the 60-pixel floor, so anything that has never been on screen is still missing —
which the mockup, showing both sides' forces at once, expects to see.

---

# Coverage, as a number

"More units appear" was an impression. It is now counted, in
`lb_field_composite_guest()` under `LB_DIAG=1`:

    [lb_cover] 6 entities off-camera: 0 drawn, 4 of an unlearned type
    [lb_cover] 6 entities off-camera: 2 drawn, 4 of an unlearned type

The first line is before the camera leaves the base -- the two learned entities
are inside it, so the guest draws them and the cache is not used. The second is
after scrolling away: both are drawn from cache.

**Four of the six are of a type never seen.** Those are the opponent's, across
the map, which no amount of fixing the learner reaches: there is nothing to learn
from. They are also exactly the units fog of war should be hiding, so drawing
them would be wrong once docs/179's shroud problem is solved. The gap between
this and the mockup is, for those four, a fog gap rather than a sprite gap.

## The cache is keyed by (owner, category)

Previously keyed by entity slot, so a unit built after the camera moved away
could never be drawn however many identical units had already been learned.
Owner is part of the key because it selects the team palette -- the same
blueprint in red and in blue are different pictures.

Stated plainly: **this scene does not demonstrate the fix.** All of the player's
unit types had already been learned individually, so the counts are the same
either way. It removes a real limitation that this save does not exercise.

## A measurement that lied, briefly

The first version of the counter logged every 120th call. This function runs once
per field dump, so it fired exactly once, from a frame before the camera had
moved, and reported `0 drawn` while the dump image plainly showed two sprites
outside the camera box. A number contradicting a picture is worth more attention
than either alone: reporting on change rather than on a tick made the two agree.
