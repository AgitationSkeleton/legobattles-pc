# The field BG is exactly screen-sized, and docs/109 measured the wrong screen

> **WRONG, superseded by [docs/114](114-the-engines-were-inverted.md).** This
> document's central claim is inverted. The registers quoted are real but the
> engine attribution is backwards: the field is engine A, whose BG1 and BG2 are
> 512x256. Do not act on the plan below; the work it proposes is already done in
> the hardware. docs/109's original conclusion stands instead.

## The registers

Read live in a Free Play match:

```
engine A  DISPCNT=00012F08  mode 0, BG0-BG3 on      <- the TOP screen
    BG0  BGCNT=0002  256x256   screenbase 0  (06000000)  prio 2
    BG1  BGCNT=4187  512x256   screenbase 1  (06000800)  prio 3
    BG2  BGCNT=4081  512x256   screenbase 0  (06000000)  prio 1
    BG3  BGCNT=0284  256x256   screenbase 2  (06001000)  prio 0

engine B  DISPCNT=00011F00  mode 0, BG0-BG3 on      <- the GAME FIELD
    BG0  BGCNT=0D00  256x256   screenbase 13 (06206800)  prio 0
    BG1  BGCNT=0E02  256x256   screenbase 14 (06207000)  prio 2
    BG2  BGCNT=0F01  256x256   screenbase 15 (06207800)  prio 1
    BG3  BGCNT=0C04  256x256   screenbase 12 (06206000)  prio 0
```

**Every engine B layer is 256x256** -- one 32x32 screen block each, exactly the
hardware viewport. The field has **no off-screen tilemap columns at all**.

The 512x256 layers are on **engine A**, which `docs/111` established is the
**top** screen: the unit portrait, the minimap, gold and population.

## What this corrects

`docs/109` is built on "a 512-pixel-wide text BG is two 32x32 screen blocks",
and concludes that widening requires filling **block 1**, which it measured as
never written. Both halves were measured on **engine A** -- the top screen --
while the subject of the widescreen work is the field on engine B.

That explains the finding rather than contradicting it: block 1 is never written
because it belongs to a **top-screen HUD layer that does not scroll**. It was
never going to contain map data, and no amount of filling it would widen the
field.

The corresponding note in `lb_wide2d.inc` -- that decoding engine A past 256
columns produced a repeating block of stale tiles, and so
`lb_engineA_widening_allowed = false` -- was the correct call for the wrong
reason. The stale tiles are the top screen's HUD map, not a partially-filled
field.

## What it means for widescreen

The field cannot be widened by decoding further, on either engine, because the
data does not exist anywhere:

* engine B, where the field is, has 256x256 layers -- nothing past column 31;
* engine A's wider layers belong to the top screen.

So a wider field of view **requires a guest-side change**, and the shape of it
is now specific rather than speculative:

1. set the engine B field layers to **512x256** (BGCNT bits 15:14 = 01), which
   makes each one occupy **two** screen blocks instead of one;
2. give them the second block -- engine B currently uses screen blocks 12 to 15,
   `06206000` to `06207FFF`, and its BG VRAM bank runs to `0621FFFF`, so the
   space is there;
3. make the tile streamer fill the extra columns, which is the same work as the
   32-column refresh it already does, over a wider span.

This is a change to recompiled guest code, which the project allows when it
follows from decomp evidence, and `docs/85` found no Sim->User calls across 555
simulation virtuals, so a wider view stays lockstep-safe: it is presentation.

## Method note

This was one `read_io` sweep over `DISPCNT` and `BGCNT` for both engines. The
work had spent four techniques and several documents reasoning about tilemap
widths without ever reading the register that states the width. Read the
configuration before measuring the consequences of it.
