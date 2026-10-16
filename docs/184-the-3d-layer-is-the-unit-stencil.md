# 184 — The 3D layer is the unit stencil

Status: **implemented and verified.** Removes the shroud rectangle from the wide
field and replaces the sprite learner's terrain differencing with an exact mask.

## What was wrong

`lb_field_composite_guest()` pasted the guest's whole 256x192 framebuffer over
the camera rect. That drags in everything the guest drew there — including the
shroud — so over unexplored ground the wide field showed a grey rectangle in the
middle of a map the host had drawn fully revealed (docs/179).

And `lb_field_learn_sprites()` found units by differencing that same framebuffer
against host-rendered terrain, which needed three separate heuristics to survive:
a connected-component crop, a minimum blob size, and a rejection for patches that
disagreed with terrain almost everywhere (i.e. shroud). Each was a workaround for
the same root problem — differencing is not a way to find a unit.

## What the 3D layer gives

docs/156 established by layer-toggling that BG0-3D carries the units and
structures. `nds_gpu3d_line(y)` returns that layer's rasterised scanline, and
dumping it during a live level confirms it directly: the castle, its two towers,
the builder and the hero, on a transparent background and nothing else.

The alpha channel is **strictly 0 or 31** across the whole frame. So the layer is
an exact stencil of the units, needing no differencing at all.

Format, read from the compositor rather than guessed (`gpu2d.cpp` masks `0x3F` at
shifts 0, 8 and 16, and expands with `(c6 << 2) | (c6 >> 4)`): six bits per
channel with **red in the low bits**, five bits of alpha at bit 24.

### A dump that lied, and why

The first dump fired on the first frame carrying 64 coloured pixels and produced
an image of brown brick bars — which read as proof that units are *not* 3D and
that docs/156 was wrong. It was nothing of the kind: the warp passes through the
army-customise screen, whose LEGO brick panels are themselves drawn in 3D, and
the dump caught that. Gating on `lb_field_locate()` succeeding — a level actually
being up — gives the units immediately.

Worth recording because the wrong image was *plausible*, and because the fix was
to make the trigger say what it meant rather than to doubt the earlier finding.

## The change

The camera rect is now composited from the 3D layer, drawing only pixels whose
alpha is non-zero and leaving host terrain everywhere else. The learner builds
its mask from the same alpha instead of from a terrain difference.

Three heuristics went with it. The centre-connected crop stays — units still
overlap one another in the 3D layer, so a patch centred on one entity can still
catch a neighbour — but the shroud rejection is gone, because there is no shroud
in the layer to reject.

**Verified**: `screenshots/20260831-170811-3dcomposite/composite3d_crop.png`,
camera scrolled into unexplored ground. The terrain runs continuously across the
camera boundary with no grey block, which is the artifact this started from.

## What this does not fix

The map is still drawn **fully revealed** — the shroud is culled, which is the
defect docs/179 opens with and remains open. What changed is that the
port no longer draws a shrouded rectangle in the middle of an unshrouded map; it
now draws consistently unshrouded, which is wrong in a tidier way.

Off-camera units are still reconstructions from cached patches, so a structure
whose entity anchor is off by tens of pixels still stamps in the wrong place.
The stencil is exact; the registration is not.
