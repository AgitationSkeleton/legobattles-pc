# 336 — The fog wall is a DS-sized reveal, not a renderer fault

On 2026-09-20 the deployed build was badly broken: a screenshot showed a
battlefield almost entirely grey shroud hatch, and the same build still had
broken crystal fields, broken fog and broken shroud.

The shroud-mask change was blamed, twice, and wrongly both times. This is
what it actually is, settled by recording the same moment at two window sizes.

## The control that settles it

Same demo, same replay point (`LB_DUMP_START=9000`, frame 8 of the sequence),
one variable changed — the window.

| window | guest surface | what the frame shows |
|---|---|---|
| **512x384** | 292x218 | clean red Mars-tileset terrain, a building, units, rock detail — **no fog at all** |
| **1855x988** | 1060x564 | **96% grey shroud hatch**, a wedge of red terrain at the bottom left |

The renderer is fine. The grey hatch is the shroud; the red hatch is the Mars
tileset's terrain — two similar scale patterns in different colours, which is
part of why this was misread for so long.

**The game reveals a DS-sized neighbourhood, and the widened window shows about
nine times the area.** Everything beyond that neighbourhood is legitimately
unexplored, so it is legitimately grey.

The minimap in the same frame agrees: the white camera box sits over dark,
unexplored ground with the player's base a short way off.

## Why it looked right four days ago

`kShroudW = 64, kShroudH = 63` on a 96x96 map. **57% of the cells had no entry
in the mask**, `lb_field_apply_shroud` skipped them, and that ground drew
*unshrouded whatever the game's record said*. The picture looked correct
because most of the screen was showing ground that should have been hidden.

Sizing the mask from the map (`9af03fa2`) fixed that and revealed the real
problem underneath. Both of my attempted reverts failed to restore the old look
and the reason is instructive:

* turning `LB_HOSTTERRAIN` off did nothing, because `lb_widemap`'s band
  synthesis reads the same mask through `lb_field_shroud_at()`;
* capping the mask back to 64x63 did nothing either, which says the shroud in
  the bands is not coming from `lb_field_apply_shroud` at all.

## The fix: widen the game's own visibility viewport

`Set_View_Dimensions` at `0x0203339C` takes the view in TILES —
`mov r1, #0x20` (32) and `mov r2, #0x18` (24), i.e. exactly 256x192 — and the
setter **reallocates the fog mask from them** (docs/183): stride
`(width + 7) / 8`, mask `stride * height`, plus a row-pointer table.

So the game's visibility record is sized for the DS screen. `LB_VIEW_WIDTH` and
`LB_VIEW_HEIGHT` already patch those immediates. Measured at the reported window:

    [lb_fov] view width  32 -> 116 tiles (928 px); fog mask -> 360 bytes
    [lb_fov] view height 24 ->  61 tiles (488 px); row table -> 244 bytes

and the same frame that showed a thin strip of terrain now shows a
**substantially larger revealed region with a ragged boundary** — ragged is
real exploration; the straight edges docs/183 complained about are a clip.

### Why the old verdict no longer holds

This knob was examined in September and set aside:

> it correctly widens the fog viewport and, on its own, does nothing a player
> would see

because *"the fog draw loop is still hard-coded to `tx < 0x20` (docs/202), so a
wider mask alone was never going to draw wider fog."*

**That was true of the guest's own draw loop and is no longer the whole
picture.** The port does not use the guest's fog draw for the widened columns:
`lb_widemap` synthesises the bands and takes their shroud from the game's
visibility record via `lb_field_shroud_at()` / `lb_fog.cpp` (docs/247). A wider
record therefore *does* now reach the screen, by a path that did not exist when
the knob was judged.

## What is not settled, and must be before it ships

1. **Selection.** The measured objection was real: `LB_VIEW_WIDTH=392` left the
   left band at **0 of 10** selections against 3 of 10, and 512 *"degrades the
   in-band control to 2 of 4"*. That was 2026-09-05/06 — before the thirty-site
   touch widening, before the order path's Y was repointed, and before the
   patch-table collision that had disabled all of it was found. It is being
   re-measured with `--drag-select`, on and off, at the reported window. **If
   selection still degrades, this does not ship**, however good the picture
   looks.
2. **Multiplayer.** It rewrites a guest instruction, so docs/85's argument that
   this port's rendering changes are client-local does not cover it. Nothing
   has tested two clients disagreeing. Until a lockstep run says otherwise it
   stays env-gated for multiplayer.

## What this does NOT explain

**The crystal fields.** Those were reported broken in the same build and
the DS-window capture shows the tileset rendering correctly with host terrain
off, so the likeliest cause is the host field's metatile resolution — the
pointer-chain relocation in `5263b07b` (split at `0x0214B9D0+2`, secondary
table at `map+0x33C`). A wrong split resolves every index above the threshold
to the wrong entry, which draws as one tile repeated across a region. Host
terrain is now off by default, so this should already be gone; **it has not
been confirmed against a frame from the failing play run.**
