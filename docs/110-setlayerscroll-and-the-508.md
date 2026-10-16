# SetLayerScroll, and why the 508 was never evidence

## Retraction first

Everything in this session that reasoned from "the BG scroll registers are
pinned at 508 under every input, therefore the camera never moved" is **wrong**,
and `docs/107` and `docs/109` are corrected by this file.

508 is a **hardcoded constant**. It is not a camera reading, so the registers
never carried information about the camera at all, and no conclusion about
scrolling could be drawn from them.

## What the registers actually are

The watch ring records a `pc` alongside every access -- a field this session had
been ignoring, which is what made all of the above avoidable. Reading it names
the writer immediately:

| register | writer |
| --- | --- |
| `04000014` BG1 | `0202D250` |
| `04000018` BG2 | `0202D27C` |
| `0400001C` BG3 | `0202D2A8` |

All three are inside one function, **`func_0202D1B4`**, which is
`SetLayerScroll(layer, x, y)`:

* `r0` is a layer object; the function switches on the layer id at `[r0+4]`
  (`cmp r0,#7` / `addls pc, pc, r0, lsl #2`, an eight-entry jump table);
* each case writes the HOFS/VOFS pair as a **single 32-bit store**, with
  `rsb rN, rN, #0` on both components -- so the register value is
  `(-x) & mask | ((-y) << 16)`, i.e. the hardware value is the **negated**
  scroll;
* `[r0+8]` non-zero diverts to `0202D364`, a deferred path.

It has exactly **four callers**, found by scanning the static ARM9 image for
`BL` with a matching target:

| caller | arguments |
| --- | --- |
| `0202AC9C` | `r1 = 0`, `r2 = 0` -- constants |
| `0203891C` | `r1 = 4`, `r2 = 0` -- constants |
| `0203974C` | computed, see below |
| `020397CC` | computed, see below |

`0203891C` passes the literal `4`. `(-4) & 0x1FF` is **508**, and its `y` of 0
is the VOFS of 0 seen beside it. That is the whole mystery: the constant this
session kept re-measuring was an argument in the instruction stream.

It is written **every frame**, not once at init. So the BG layers carrying the
field are re-pinned to `(4, 0)` on every frame, which means **the field cannot
scroll by moving BG scroll registers** -- the registers are held still on
purpose. That is independent support for `docs/109`'s finding that a view change
refreshes 32 tile columns: the field scrolls by **rewriting the tilemap**, with
the scroll registers fixed.

## The computed caller, and a camera-shaped thing that is not the camera

`func_020396E0` is the computed path. Given an object in `r6`:

* `[r6+0x5E]` and `[r6+0x60]` are loaded with `ldrsh` -- **signed 16-bit**;
* each is divided by a power of two using the standard signed-division idiom
  (`lsr`/`rsb`/`add` with `ror`): `#29` is **divide by 8** for the first,
  `#28` is **divide by 16** for the second;
* it forms `(4 - a/8, -(b/16))`, sign-extends both to 16 bits, passes them
  through `func_020F2A50`, and pushes the result through `SetLayerScroll`;
* it does so **only when the dirty byte `[r6+0x66]` (or `[r6+0x67]` for the
  second layer) is non-zero**, clearing it afterwards.

`func_020396E0` has **no `BL` callers at all** -- it is reached through a
vtable, so no literal names its `this` and no static scan can find it. It was
located live instead, with a new `LB_WATCH_PC` filter (below): in a Free Play
match the two `ldrsh` at `02039700` and `02039704` read

    0224B4A6   (s16, the /8 component)
    0224B4A8   (s16, the /16 component)

putting the object at **`0224B448`**.

### Negative result: writing that pair does not move the view

With `NDS_DEBUG_ALLOW_WRITES=1`, setting both fields and forcing both dirty
bytes to 1:

* the values **read back changed** and **persisted** across frames -- the game
  neither rejects nor recomputes them;
* the scroll registers stayed at 508 regardless (expected, given the constant
  above);
* the **framebuffer was unchanged**: 248 of 49152 pixels differed between a
  before and after capture, 0.5%, which is unit animation.

So `0224B448` is an object the scroll path reads, but it is **not** the field
camera, or that path is not the one driving the field. Values tried: (200,0),
(200,160), (0,0), (400,144), (136,400), (800,600), (600,400).

This is recorded as a negative result rather than a conclusion. What it does
establish is that finding the camera by looking at scroll registers cannot work
in this title, because those registers are deliberately held constant.

## New tool: `LB_WATCH_PC`

`LB_WATCH=lo:hi` answers "who touches this address?". Its mirror --  "what
address does *this instruction* touch?" -- is what locates a structure reached
only through a register, and it is now available:

    LB_WATCH=2000000:2400000 LB_WATCH_PC=2039700,2039704

Up to eight hex ARM9 PCs, and events are kept only when the range **and** the PC
match. Without the PC filter the ring keeps the most recent 512 accesses over
the watched range, and one interesting load per frame is drowned by the hundreds
of thousands around it. This is how `0224B4A6` was found in a single run.
