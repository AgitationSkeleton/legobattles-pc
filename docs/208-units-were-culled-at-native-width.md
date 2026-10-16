# 208 — Units were culled at the native width

Goal: units rendered in the widened columns.

## The standing explanation was only half of it

This project has said, repeatedly, that units are fine in the widened columns
because "units are 3D geometry, and 3D already reaches the widened columns"
(docs/197). Both halves of that are true and it is still the wrong conclusion,
because it only covers the DRAWING.

Confirmed by masking every 2D layer in the field: with BG1, BG2 and BG3 off,
the player's base and a unit still render and everything else is black. Units
are 3D. And the 3D layer really is produced at the full width -- the seam probe
reports `w=448 extra=96`, and the compositor indexes it by output column.

## What was missing: the game never submits them

`func_02038F28` gathers entities into a rectangle
`(camx, camy)..(camx + W, camy + 0xC0)`, and W is a single ARM immediate:

    02038FD0  E2822C01   add r2, r2, #0x100        ; 256 -- the native width

Anything outside that rectangle is never gathered, so no amount of widening the
renderer can put it on screen. **The port did not patch this.** `lb_probe_cull`
existed in the harness as an experiment and nothing applied it in normal play,
so terrain and fog extended into the widened columns while units could not.

## The fix

`apply_cull_width` in `lb_patches.cpp` rewrites that immediate to 448, the
surface's own width, on the same switch as the widening. It only ever rewrites
the exact stock encoding, so a build whose bytes differ is left alone rather
than patched blind, and a vanilla-width session is untouched.

Verified: the patch reports itself, the run reaches a battle, the terrain
reconstruction still matches the guest exactly (`0/768 differ`, eight report
frames), the shroud still reads `520/768`, and the frame rate is unchanged at
42.3 fps.

## What is still owed

**A capture with a unit actually standing in a widened column.** The mechanism
is in place and the game is demonstrably unharmed, but the visual confirmation
has not been obtained: the harness's camera drifts away from the player's base
before the pan begins, and the pan's shortest usable step still travels about
thirteen tiles.

The way to get it is a story-mode mission, which has special units, with the
shroud-removal cheat on so the whole field is visible.
That also covers the "modelled special unit rendering" item, which no Free Play
battle can exercise.

Until that capture exists this item stays **NOT AUDITED** rather than done. The
cull was found by reading the gather rect, not by seeing a unit appear.

---

## Correction (docs/209): this was half a fix

The rect was described above as `(camx, camy)..(camx + W, camy + 0xC0)` in
pixels, with `W` the single immediate at `0x02038FD0`. Disassembling the whole
of `func_02038F28` rather than that one instruction shows two things wrong with
that description.

**It is in metatile columns and rows, not pixels.** `func_020085a0` divides X by
24 and Y by 16 and hands both to `func_020f2bbc`, which does
`strb r1, [r0, #4]` and `strb r2, [r0, #5]` — so **+4 is X and +5 is Y**. That
matters because the two axes do not get the same margin, and guessing which byte
was which had even odds:

    02038FB0  E3500000  cmp   r0, #0        ; r0 = metatile column of camx
    02038FB4  03A00000  moveq r0, #0
    02038FB8  E2400001  subne r0, r0, #1    ; min X = col(camx) - 1
    ...
    02038FD0  E2822C01  add   r2, r2, #0x100
    02039004  E2801003  add   r1, r0, #3    ; +3 is on ROWS, not columns

**And the left edge was never widened.** The 448-wide surface shows
`camx-96 .. camx+352`, so rewriting only `0x02038FD0` widens the rect to the
RIGHT and leaves the left margin at the stock one column — 24 px, against a view
that reaches 96 px out. Units in the LEFT widened column stayed culled, and the
message this port printed said the rect was fixed.

96 px is exactly four metatile columns, so the left margin goes from 1 to 5. It
cannot be done by editing the literal: the stock block clamps at zero, and
`col - 5` with the camera in the first four columns underflows the byte to 0xFB
and sends the caller walking a nonsense range. The three-instruction block is
replaced whole, keeping a clamp:

    E3500005  cmp   r0, #5
    B3A00000  movlt r0, #0
    A2400005  subge r0, r0, #5

The right edge is now `camx + 352` — the surface's actual last column — rather
than the 448 first used, which was 96 px more than anything visible.

The "still owed" item at the end of this document — a capture with a unit
actually standing in a widened column — is now discharged: see docs/209 and
`screenshots/exp-story8/full.png`.

**Note on the null A/B in docs/198.** That result stands as recorded, and this
correction does not overturn it: docs/198 measured the immediate's effect with
`gx_polygons`, and the rect was never the only thing limiting what reached the
widened columns. What actually made the ground out there correct was the seam
and shroud work in docs/209, not this rect.
