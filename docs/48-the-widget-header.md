# The widget header

**Status: the 32-byte widget header is named. The bounding rectangle is
confirmed on 1,062 of 1,062 records, and 22 of 22 verification checks pass.**

`docs/47` decoded the widget taxonomy and left one gap: a consumer could
identify a widget but not read a field inside it. This closes most of that.

## The constructor copies the record verbatim

`ButtonWidget`'s dispatch arm allocates `0x90` bytes and calls
`func_020E53A8(obj, record)`. That constructor allocates a second block and
copies the record into it **field by field**, which is as good as a struct
declaration:

```arm
ldrb  [r4+0x00] -> +0x00      ldrb  [r4+0x10] -> +0x10
ldrb  [r4+0x01] -> +0x01      ldrb  [r4+0x11] -> +0x11
ldrh  [r4+0x02] -> +0x02      ldrh  [r4+0x12] -> +0x12
ldrsh [r4+0x04] -> +0x04      ldr   [r4+0x14] -> +0x14
ldrsh [r4+0x06] -> +0x06      ldrh  [r4+0x18] -> +0x18
ldrsh [r4+0x08] -> +0x08      ldrh  [r4+0x1A] -> +0x1A
ldrsh [r4+0x0A] -> +0x0A      ldrb  [r4+0x1C] -> +0x1C
ldrb  [r4+0x0C] -> +0x0C      ldrb  [r4+0x1D] -> +0x1D
ldrb  [r4+0x0D] -> +0x0D      ldrb  [r4+0x1E] -> +0x1E
ldrb  [r4+0x0E] -> +0x0E      ldrb  [r4+0x1F] -> +0x1F
ldrb  [r4+0x0F] -> +0x0F
```

**The access width names every field's type**, without a single guess. Four
`ldrsh` in a row at `+0x04` — *signed* halfwords, where everything else is
unsigned — is the whole lead.

| offset | type | |
| --- | --- | --- |
| `+0x00` | `u8` | widget type in the high nibble (`docs/47`) |
| `+0x01` | `u8` | an index the base parser compares against a running counter |
| `+0x02` | `u16` | |
| `+0x04` | `s16` | **y1 (top)** -- CORRECTED, was called x1 |
| `+0x06` | `s16` | **x1 (left)** -- CORRECTED, was called y1 |
| `+0x08` | `s16` | **y2 (bottom)** -- CORRECTED, was called x2 |
| `+0x0A` | `s16` | **x2 (right)** -- CORRECTED, was called y2 |
| `+0x0C`…`+0x11` | `u8` × 6 | |
| `+0x12` | `u16` | a bitmask; the base parser tests it against a global |
| `+0x14` | `u32` | |
| `+0x18` | `u16` | a resource id, **`0xFFFF` = none** |
| `+0x1A` | `u16` | |
| `+0x1C`…`+0x1F` | `u8` × 4 | |

## The rectangle, confirmed on every record

`x2 >= x1 and y2 >= y1` holds on **1,062 of 1,062**.

That is the check, and its strength is worth stating: a random 8-byte field
satisfies both inequalities about a quarter of the time, so 1,062 consecutive
successes is not something an ordering coincidence produces. It is a rectangle.

Two details fall out that would otherwise look like errors:

**Coordinates go negative** — down to `-64`. Widgets are staged off the edge,
which is how a sliding panel starts.

**RETRACTED: the 376 is HORIZONTAL, and there is no two-screen coordinate
space.** This paragraph read the extent on the wrong axis, because the
rectangle fields were themselves named on the wrong axis (see the correction
below). Measured across all 132 layouts, the `+0x04`/`+0x08` pair spans
-64..216 and the `+0x06`/`+0x0A` pair spans -64..376. 216 fits one screen's
height; 376 only makes sense across the 256-pixel width. The 376 belongs to
`UI/FE/MiniGame1Top.bin` widgets 2/3/5/6, which slide from `left = -64` to
`left = 376` -- the same off-screen staging this document already describes on
the negative side, just on the far edge. Vertical never exceeds 216.

`docs/44`'s filename pairing of `…TopScreen.bin` stands on its own; it simply
is not corroborated by geometry, because each layout addresses ONE screen.

**`0xFFFF` at `+0x18` appears in 498 of 1,062 records**, and `ButtonWidget`'s
constructor tests for exactly that value before allocating — so it is the "no
resource" sentinel, confirmed from both sides.

## A blind spot I built and then removed

The first version required a record to be at least `0x20` bytes before reading
the header, and reported the rectangle check as **981 of 981**.

981 is not 1,062. The missing 81 are exactly the `BackgroundWidget` records,
which are 28 bytes — and the rectangle ends at `+0x0C`, the resource id at
`+0x1A`, both comfortably inside 28. The filter excluded a whole widget type
from its own check while still reporting a perfect score.

**A denominator that quietly shrinks to fit the numerator reports 100% and
means nothing.** The tell was that 981 is not a number the earlier passes had
ever produced; every widget count so far had been 1,062. Reading against the
minimum each *field* needs, rather than the maximum the header could occupy,
gives 1,062 of 1,062.

That is the fourth time this project has caught a metric whose denominator was
wrong — `docs/35`, `docs/36`, `docs/39`, and now here — and the first where I
introduced the fault in the same pass that found it.

## What is still unnamed

Eleven bytes of the header — `+0x02`, `+0x0C`…`+0x11`, `+0x14`, `+0x1A`,
`+0x1C`…`+0x1F` — have known widths and unknown meanings. Beyond the 32-byte
header, the type-specific tail (`ButtonWidget` carries 20 more bytes,
`RadioButtonWidget` 24) is untouched.

The format is already at the *decoded* tier from `docs/47`, so this does not
move the figure. What it changes is what a consumer can do: **place** a widget,
not merely count it.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | 10.3% | 402 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |

## CORRECTION — the rectangle is (top, left, bottom, right)

This document originally named the four `s16` fields `x1, y1, x2, y2` in order.
The axes are **swapped**: the order is `+0x04` top, `+0x06` left, `+0x08`
bottom, `+0x0A` right.

Why the original check could not catch it: the validity test used here was
`x2 >= x1 and y2 >= y1`, which is symmetric under swapping the two axes. It
passes either way, so nothing in this document ever tested the assignment.

The shipped data settles it:

* `UI/FE/Credits.bin` widget 1 is `0, 0, 192, 256`. A DS screen is 256 wide and
  192 tall, so the third field is the height and the fourth the width.
* `UI/FE/SkirmishSetup.bin` widget 0 is `-25, 0, -1, 255` -- a full-width title
  bar staged just above the top edge.
* Across all 132 layouts the first/third pair spans -64..216 and the
  second/fourth spans -64..376.

## Fields named since, from `CustomizeArmy`

| offset | meaning |
|---|---|
| `+0x02` | the **widget / control id** the screen switches on (`CustomizeArmy::virtual_08` jump-tables on it) |
| `+0x0C` | **palette index** -- `0x1F` normal, `0x0A` greyed for a locked entry |
| `+0x0E`..`+0x11` | **d-pad neighbours**: up, down, left, right, as widget indices, `0xFF` = none |
| `+0x14` | runtime back-pointer to the owning widget group, written into the file buffer in place |
| `+0x24` (ButtonWidget) | icon / sprite-cell id |

**Do not derive a screen's grid shape from the d-pad bytes.** In
`CustomizeArmy.bin` they are stale: they wire the first twelve icon widgets as a
4x3 block while the rectangles place them as the first twelve of a 5x5 grid, and
the remaining thirteen have no links at all. The rectangles and the fill code
agree with each other; the navigation bytes agree with neither.
