# 344 — The drag-select crash: the box drew itself into the interrupt handler

The defect (docs/343): dragging the yellow selection box across the game world
to select multiple units still crashed -- the game slowed to a crawl and then
failed.

Status: **root-caused, reproduced, fixed, deployed** (`cc643cc7`). One thing
not yet verified by eye: that units in the rows above and below the native
field are still selected by a drag that reaches them.

## The evidence the crashed run left

The crashed run's log (`build/logs/session-20260922-144500.log`) ends with an
ITCM dump and:

    [lb_itcm] REJECTED pc=01FF8000 ... first differing byte at +0
    [lb_itcm]   01FF8000 live 002E002D expected E92D4000  <-- DIFFERS
    [lb_itcm]   01FF8004 live 002E002E expected E3A0C301  <-- DIFFERS
    ...
    [lb_itcm] MISS pc=01FF8000 thumb=0 candidates=1 inactive=1

ITCM holds the interrupt dispatcher and the DMA helpers (decomp report). It had
been overwritten with halfwords `0x002D`, `0x002E` -- not code. The recorded
demo of that run (`build/lb-session-1790113503.lbdemo`) ends **mid-drag**,
with no release, at raw x -254..-220, y near the bottom of the field.

## The mechanism, from the decompilation

`func_0203B7FC` draws the selection box as a nine-slice of halfword tiles --
`0x0d/0x0e/0x0f` top, `0x1d/0x1e` sides, `0x2d/0x2e/0x2f` bottom -- through
`func_0203D8E8`:

    cell = table[y >> 3] + x * 2          table = 24 rows at 0x021520F8

It clamps the box's **start** corner to 0..255 / 0..191. It reads the
**current** corner's y from the drag object's byte at `+9` and does not clamp
it. On a DS that byte cannot exceed 191: the touch panel has no row 192.

This port added an extended-Y touch channel (kOrderFix group 128) so clicks in
the widened rows land where they are aimed. It writes the true row into that
byte. A drag below the native rows therefore put **205..211** there
(`LB_MQTRACE`), row indices 25..26, and the words past the table are:

    table[24] = 0x00000000     table[25] = 0xFF000000

The bottom border was written through a null row pointer at `0 + x*2`. ITCM is
mirrored at address 0, so `0x0000000C` is `0x01FF800C` -- exactly where the
crash log found `002E002D`. The dispatcher's lifted code no longer matched
its bytes, the runner correctly refused it, the game fell back to the
interpreter (the chug: 28 fps, 33 ms a frame emulating) and died on the miss.

## Reproduction

`tools/lb_exp.py --dragbox X0,Y0,X1,Y1` in native coordinates. It now also
samples the frame rate during and after the drag, because the report was a
slowdown that the harness's pre-drag fps could not see.

| drag (native) | before the fix |
|---|---|
| 60,50 -> 190,150 (inside) | survives |
| 60,120 -> 130,191 (to the last row) | survives |
| 60,120 -> 130,230 (below) | ITCM corrupted, crash |
| 100,100 -> 200,300 (deep below) | ITCM corrupted, 28 fps, crash |

## The fix, and the two that did not work

1. **Clamping the touch panel to 0..191** in `nds_set_touch`. Did not help:
   the row reaches the game through the extended-Y channel, which bypasses the
   panel. Kept anyway -- a real panel cannot report y >= 192 -- with its comment
   corrected to say it is not the fix.
2. **Clamping byte +9 around the call and restoring it.** The clamp took (read
   back 191) and ITCM was still corrupt by the next call; skipping the routine
   outright kept ITCM intact. Something later in the frame reads the restored
   value. Which, is not identified.
3. **Clamping byte +9 and leaving it** -- the fix. To 191 when the true row is
   below the field; to **0** when it is above, because a negative row arrives
   as 256 + y, also above 191, and clamping that to 191 would pick the wrong
   edge. Selection is unaffected: the extended-Y readers take the row from the
   16-bit shadows, not from this byte.

   > **CORRECTED 2026-09-23 (docs/348).** That was not true when written. The
   > selection rectangle's CURRENT corner row is loaded from this very byte at
   > 020AF1BC (drag in progress) and 020AF310 (pen-up); only the press corner
   > and the x values were on the shadows. So after this fix a drag reaching
   > above or below the native rows selected against row 0 or 191 -- one cause
   > of the selection not matching the drawn bounding box. Both
   > reads (and four siblings) are now in kOrderFix group 128.

Verified after the fix, 1280x720:

| drag | ITCM rejections | misses | fps after release |
|---|---:|---:|---:|
| below (y 230) | 0 | 0 | 58.4 |
| deep below (y 300) | 0 | 0 | 59.9 |
| above (y -60), byte 255 clamped to 0 | 0 | 0 | 59.7 |
| inside -- the clamp never fires | 0 | 0 | 54.7 |

## A lesson for the next extension of an input axis

Widening a coordinate the cartridge stores in a byte is not finished when its
readers are repointed. Every routine that uses the old byte as an **index**
into a table sized for the DS screen has to be found too -- the selection box
was one, and a byte past 191 turned it into a wild write.
