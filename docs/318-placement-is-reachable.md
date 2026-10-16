# 318 — Placement IS reachable, the ghost is cursor-driven, and the band has no BG3 at all

docs/307 closed with the placement indicator in the widened view *not
verifiable by this harness in any window size*, and docs/315 recorded it as
PARKED on that basis. **That is no
longer true**, and this page replaces both readings.

## Placement is entered by the icon tap alone

```
build-flow:   icon (44,52): hudquads 18 -> 8, footprint 22
build-flow: PLACEMENT ACTIVE on the icon alone -- footprint 22 cell(s)
[lb_fp] cells=88 box=0,12..31,25 placeable=24 blocked=0 other=64
        cam=265,144 touching-edge=y active=1 ...  FOOTPRINT 14,12..19,15
```

`view+0x78` reads **1**, and 24 cells carry `0x4F`. No field tap is needed --
the icon starts it, and docs/307's own instruction to *"ask before touching the
field"* turns out to be the whole answer: the field tap it was guarding against
was never required.

What made this pass work where three earlier ones did not was the delivery,
not the sequence: icon taps switched from `frontend_input` to the raw `touch`
command. That is the same correction that had already been made for the unit
selection, applied one layer later.

## The placement UI record is 0x02157958, not view+0x6C

Every `[lb_fp]` line of that run reported `origin=2,2 size=16x15 verdict=1`,
identical whether placement was active or not. Those fields are read from
`view+0x6C`, which docs/286 named as the placement state, and they plainly do
not move with the ghost.

The record the UI actually drives is at **`0x02157958`**, and the two functions
that own it say what each byte is.

**`func_020AA910` — ENTRY** (thumb, 0x8c bytes):

```asm
020aa916  ldr  r1, [pc, #0x70]   = 0x02152034      ; the view
020aa91e  movs r2, #0x80                           ; 128
020aa920  movs r3, #0x60                           ;  96
020aa922  blx  #0x2039f5c        -> func_02039f5c  ; screen -> world
020aa92c  strb r2, [r0, #0xc]    ; rec[0x0C] = cell x   r0 = 0x02157958
020aa930  strb r1, [r0, #0xd]    ; rec[0x0D] = cell y
020aa934  strb r1, [r0, #3]      ; rec[0x03] = kind
020aa93c  strb r1, [r0, #1]      ; rec[0x01] = 0
020aa93e  strb r1, [r0, #2]      ; rec[0x02] = 0
020aa942  str  r4, [r0, #4]      ; *(0x0214BB84+4) = the building definition
```

`0x80, 0x60` is **the screen centre, 128,96** — a constant, not an input. So
the ghost is seeded at the middle of the native screen and the player moves it
from there. That matches the measured `FOOTPRINT 14,12..19,15`: buffer cells
14..19 by 12..15 is native pixels 112..159 by 96..127, centred on 136,112.

**`func_020AA9F8` — TICK** (thumb, 0x228 bytes):

```asm
020aaa02  ldrb r1, [r0, #1]      ; rec[0x01] != 0 -> whole tick early-outs
020aaa0a  ldrb r0, [r0]          ; rec[0x00] == 0 -> first tick
020aaa14  bl   #0x20aa814        ; the cursor cell
020aaa1e  strb r2, [r0, #0x14]   ; rec[0x14]/rec[0x15] = cursor cell
   ... compares the cursor against [origin, origin + size) in BOTH axes ...
020aaad4  ldr  r0, [pc, #0x134]  = 0x0214bb84
020aaad6  movs r1, #1
020aaad8  strb r1, [r0]          ; INSIDE  -> hover flag 1, origin untouched
   ---
020aaac6  strb r2, [r0, #0xc]    ; OUTSIDE -> origin recomputed from the cursor
020aaad0  strb r1, [r0]          ;            hover flag 0
020aaade  strb r1, [r0]          ; rec[0x00] = 1, so later ticks take the
                                 ;   delta path at 0x020AAB02 and ACCUMULATE
```

So:

* `rec[0x00]` phase, `rec[0x01]` finished, `rec[0x03]` building kind,
  `rec[0x0C]/[0x0D]` the ghost's origin cell, `rec[0x14]/[0x15]` the cursor
  cell, `0x0214BB84[0]` "the cursor is on the ghost";
* **the ghost follows the cursor, and it drags** — later ticks add the cursor
  delta rather than snapping;
* **a press inside the ghost does not move it.** It sets the hover flag and
  leaves the origin exactly where it was.

## Which is why five deliveries "did nothing"

The last sweep computed the ghost's own centre and pressed there, on purpose,
because pressing at a fixed `(128,96)` had landed on its corner. Against this
function that is the one press in the whole screen that cannot move anything.

The sweep before it delivered through `frontend_input action=moveto` — and
that is a drag, not a hover: `nds_frontend_debug_move` pushes an
`SDL_MOUSEMOTION` with `SDL_BUTTON_LMASK` already set, so every "move the
cursor first" call was itself a pen-down. The geometry probe that ran before
the press was landing a touch on the ghost before the sweep had pressed
anything.

**And a touch is sticky, not a one-frame pulse.** `nds_set_touch` in io.cpp
sets `g_tsc_x/g_tsc_y` and clears the pen-up bit and returns; only an explicit
`down=false` clears it. The comment written into `lb_exp.py` last tick said the
opposite and drove a 20 Hz repeat that was never a hold. Corrected in place.

`[lb_fp]` now prints the whole record — `phase done kind org cur hover` — so
"the delivery never reached the guest" and "the delivery reached the guest and
the ghost correctly refused to move" are no longer the same line.

## The band has no BG3, so it cannot show a footprint at all

docs/306 named two stacked causes and attributed the port's half to
`lb_widemap_clear_tile(3, ...)` serving a clear tile to the band columns. The
code says something stronger.

`lb_widemap_extends(3)` returns **0**, and `decode_text_line` computes

```c
out.decoded_width = 256 + left_extra + (right_extra < 0 ? left_extra : right_extra);
```

so a declined layer decodes 256 columns and the compositor indexes it through
`hud_x`, which is negative in the left band and >= 256 in the right. The band
columns get **no BG3 source whatsoever** — not a clear tile, nothing. The
`clear_tile` call is reached only for `lb_ht < 0 || lb_ht >= 32 ||
lb_extra_row`, which for BG3 reduces to the extra ROWS, above and below.

BG3 engine A carries nothing but the placement footprint (docs/306 traced the
upload through BG3CNT). So:

**In the widened view the placement indicator exists only in the centre 256
columns. That is the placement-indicator defect, in port code, in one
sentence.**

The guest's own cull (`0203CD4C`/`0203CD54`, docs/286) is the second cause and
is independent: it never computes a footprint cell outside its 32x24 window, so
merely extending BG3 would extend a tilemap that has no data out there. Design
(3) in docs/306 — shift the camera by whole metatiles, call `func_020370B4`,
harvest, restore — is still the fix, and it is still unimplemented.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** placement is entered by the build-icon tap alone, reaches
`view+0x78 == 1`, and paints 24 placeable cells. docs/307's "the harness cannot
enter placement" is **retracted**.

**ESTABLISHED:** the placement UI record is `0x02157958`; the ghost is seeded
at screen centre 128,96 by `func_020AA910`; `func_020AA9F8` drives it from the
cursor and accumulates a drag; a press inside the ghost sets a hover flag and
moves nothing.

**ESTABLISHED:** BG3 is not extended, so the side bands have no BG3 source at
all and cannot show a footprint at any camera position.

**NOT IMPLEMENTED:** the fix. Still docs/306 design (3).

## The drag works, and the placement indicator is now measured end to end

Press off the ghost, then drag. Camera fixed at 265,144 throughout.

| cursor (native x) | rec org | FOOTPRINT box | placeable | blocked | verdict |
|---|---|---|---|---|---|
| 72 (press) | 12,15 | 2,12..7,15 | 22 | 0 | 1 |
| 136 | 15,15 | 11,12..16,15 | 22 | 0 | 1 |
| 12 | 10,15 | **0,12..1,15** CLIPPED | 4 | 4 | 0 |
| **-60 (LEFT BAND)** | 7,15 | **nothing painted** | 0 | 0 | 1 |
| 243 | 20,15 | **26,12..31,15** CLIPPED | 6 | 18 | 0 |
| **315 (RIGHT BAND)** | 23,15 | **nothing painted** | 0 | 0 | 0 |

**That table reproduced exactly**, in a later run on a different build, at
every one of the five positions: the same `org` walk 15, 10, 7, 20, 23, the
same boxes `11,12..16,15` / `0,12..1,15` / none / `26,12..31,15` / none, and
the same `placeable` 22, 4, 0, 6, 0 and `blocked` 0, 4, 0, 18, 0. Given how
many of this project's "not reproduced" answers came from probes that were
wrong the first time (docs/315 lists five of eight), a second identical run is
worth more here than any amount of argument.

Three things fall out of that table, and none of them needed a play session.

**The ghost tracks the cursor.** `org` walks 12 → 15 → 10 → 7 → 20 → 23 as the
drag goes centre, left, band, right, band. `cur` is always `org + (1,1)`, so
the building is two metatiles across and the origin is the cursor less half of
it. The five earlier "deliveries that did nothing" were the delivery and the
press position, exactly as the section above predicted.

`cur` is in MAP METATILES, not screen cells: 64 native px of drag moves it by
3, and a metatile is 24 px. Against `cam=265` (px, so metatile 11) every row
of the table predicts its own box.

**The footprint is culled at the native window, not faded.** At native x 12 the
6x4-cell ghost shows **8 cells of 24**, in buffer columns 0..1 — the rest is
simply not written. At 243 it shows columns 26..31. Both raise
`CLIPPED AT THE CULL EDGE`.

**In the bands nothing is painted at all, while the game keeps placing.** At
native -60 the cursor is world metatile 8 against a camera at 11, so the
footprint would occupy buffer columns -9..-4 and none of it is written. Yet
`verdict` still reads **1**: the guest evaluated the site, decided the building
*can* go there, and drew no indicator saying so. At 315 the same thing happens
with `verdict=0`.

That is the placement-indicator defect in one table. The indicator does not glitch in the
extended space — **it does not exist there**, and the placement it would be
describing is live and legal underneath it.

## What this leaves

Both causes are now measured rather than reasoned:

1. the guest culls every footprint cell outside its own 32x24 buffer;
2. the port gives BG3 no band columns to draw into even if it had data.

Design (3) in docs/306 answers (1) by making the guest paint the band region
itself under a shifted camera, and (2) by serving the harvest to those columns.
Nothing about the placement predicate is replicated, which is the constraint
that rejected designs (1) and (2).

**Still NOT IMPLEMENTED**, and the next step is the implementation rather than
another measurement.
