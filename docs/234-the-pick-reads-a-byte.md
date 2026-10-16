# 234 — The pick reads a byte

**Goal within the project:** clicks in the widened window must work. This is the
root cause, located exactly, after docs/230–233 excluded the coordinate's
delivery, four truncations and a gate on the way.

## The answer in one line

The routine that decides *what is under the touch* builds its query point from a
pair of **single-byte** fields:

```
020AF29C  7A29  ldrb r1, [r5, #0x8]     touch x, zero-extended from a byte
020AF29E  7A6A  ldrb r2, [r5, #0x9]     touch y
020AF2A0  A801  add  r0, sp, #4
020AF2A2  bl    020F2A52                make point(x, y)
020AF2AA  bl    020A9570                classify what is under it
```

A native x of −85 is stored as the byte `0xAB` and read back as **171**. A
native x of −53 becomes **203**. The sign is not merely lost — the value is
wrong, and it is wrong in a way that puts the query somewhere else entirely on
the map.

## How it was measured, end to end

Nothing here is read off the disassembly alone.

**1. The classifier says "nothing found".** `FUN_020A9570` iterates entities
under the cursor and, when none classifies, sets bit `0x2F` — word 1, bit 15,
i.e. `0x8000`. The flag trace shows exactly that for a band click and not for an
in-band one, and that missing bit is what fails the handler mask `0x40` and
stops the dispatch (docs/233).

**2. The pick's world point is wrong.** The point is built as camera + touch by
`FUN_02023FD8`, whose `STRH r0,[r4,#4]` at `02023FF8` writes the world x.
Watching it:

```
in band:  world x = 250, 258, 259
band:     world x = 515, 555
```

**3. And it should not be.** `[lb_base]` now prints the entity's world position
and the camera beside its screen x. The player's base is a building, so its
world x is constant at **211**, and the camera trace over the run reads:

```
cam  327  base sx -116      cam + sx = 211   correct
cam  368  base sx -157      cam + sx = 211   correct
cam  128  base sx   83      cam + sx = 211   correct
```

Clicking the base should therefore give a world x of 211 every time. In band it
gives 250–259 (the base is 46 px wide and the click lands within it). Out in the
band it gives **515** — three hundred pixels away, over empty ground.

**4. The two operands, read directly.** `LDRSH r1,[r6,#4]` at `02023FEC` and
`LDRSH r0,[r5,#4]` at `02023FF0` are the touch and the camera:

```
in band:   touch = 122      camera = 136, 128
band:      touch = 171, 203 camera = 344
```

`122` is exactly where the in-band click was aimed. `171` and `203` are not
where the band clicks were aimed — they are `0xAB` and `0xCB`, the **low bytes**
of −85 and −53, read as unsigned.

Both loads are `LDRSH`, so the sign is not lost *here*. It was lost earlier,
when the coordinate was written into a byte.

## Where the byte comes from

`FUN_020AF040`, the gesture recogniser, stores the event's 16-bit x into a
one-byte field:

```c
*(char *)(param_1 + 6) = (char)*(undefined2 *)(param_3 + 0x1c);   // x
*(char *)(param_1 + 7) = (char)*(undefined2 *)(param_3 + 0x1e);   // y
*(undefined1 *)(param_1 + 8) = *(undefined1 *)(param_1 + 6);
*(undefined1 *)(param_1 + 9) = *(undefined1 *)(param_1 + 7);
```

docs/233 named this routine as "the first place in the whole chain that stops
treating the coordinate as sixteen signed bits" and then set it aside, on the
grounds that the bytes only feed the drag threshold. **That was wrong**, and it
is corrected here: `+8`/`+9` are exactly what `020AF29C` loads to build the
pick's point.

For the DS this is not a bug. The panel is 256 px wide, x is 4..251, and a byte
holds it exactly. It is only a bug for a field wider than the hardware's.

## Why this one cannot be fixed with a shift

The four fixes before it were all one instruction: a mask widened, a clamp
removed, a shift pair changed, a branch dropped. This one is a **storage width**,
and no instruction substitution makes a byte hold ±345.

`LDRSB` would recover the sign and give ±127 — a band of 127 px each side, so a
guest surface up to 510 wide, which covers a 1020-px window at the default
magnification. Real, but it clips exactly where the larger target windows
begin. It is also not free to write: ARMv5TE Thumb has no `SXTB`, and its
`LDRSB` is register-offset only, so the three halfwords at `020AF29C..020AF2A0`
cannot hold the loads plus the register setup they would need.

## The fix, designed — and it needs no thunk

The first design was to redirect the `bl` at `020AF2A2` to a hand-assembled
Thumb thunk in free RAM that reads the coordinate from the port's own slot.
That would work, but it is not necessary, and the reason is worth stating
because it also disposes of the `LDRSB` range limit above.

**Everything downstream of the byte is already 16-bit and already signed.**
`FUN_020F2A52` stores its x argument as a *halfword* at `point+4`:

```c
*(undefined2 *)(param_1 + 1) = param_2;        // point->x, 16 bits
*(undefined2 *)((int)param_1 + 6) = param_3;   // point->y
```

and the consumer reads it back with `LDRSH r1,[r6,#4]` at `02023FEC` — measured,
not assumed; that is the instruction whose values are quoted above.

So a **zero-extended** 16-bit load at `020AF29C` is enough. Loading `0xFFAB`
zero-extended puts `0xFFAB` in the halfword, and the `LDRSH` on the far side
reads it back as −85. The sign survives without any sign-extending load, which
is exactly what ARMv5TE Thumb cannot cheaply provide.

`LDRH Rd,[Rn,#imm5]` **does** exist in Thumb (`0x8800`), unlike `LDRSH`. So the
patch is one halfword:

```
020AF29C  7A29  ldrb r1,[r5,#0x8]   ->  89E9  ldrh r1,[r5,#0x0E]
```

with the y load left alone, because y is 0..191 and a byte holds it.

That leaves one thing to arrange: something must put the true 16-bit x at
`+0x0E`. The recogniser's object is 16 bytes — `+4`/`+5` flags, `+6`/`+7` the
press position, `+8`/`+9` the current position, `+0x0A`..`+0x0D` the camera —
so `+0x0E`/`+0x0F` is the one free halfword, and x is the only field that needs
more than a byte.

The candidate is `FUN_020AF040`'s own copy `*(p+8) = *(p+6)`, which is a `ldrb`
and a `strb` and is redundant while the register still holds the value: two
halfwords, where one `STRH Rd,[Rn,#0x0E]` (`0x81C0 | …`, which Thumb does have
with an immediate) would do. **That pairing has not been verified against the
disassembly yet** and is the first thing to check next — the copy must
really be redundant there, and the drag threshold that reads `+6` and `+8` must
keep working, or taps start being classified as drags.

## Status of the chain

Everything from the window to the pick's world point is now measured and
correct, except this one byte:

| stage | state |
|---|---|
| window pixel → guest → native coordinate | correct, 66 of 66 columns (docs/230) |
| native → the cartridge's touch struct | correct, signed (docs/230) |
| touch struct → the event object | correct since the clamp was removed (docs/233) |
| event → the widget hit test | correct since the shift pair was fixed (docs/233) |
| event → the field-selection branch | reached since the gate was dropped (docs/233) |
| **the gesture recogniser's byte fields** | **truncates — this document** |
| byte fields → the pick's world point | faithfully propagates the wrong value |
| world point → entity query | correct, and finds nothing because the point is wrong |

## The fix, applied — and what it moved

Both halves went in as designed.

**ROM, one halfword per site.** `020AF29C ldrb r1,[r5,#0x08]` becomes
`ldrh r1,[r5,#0x0E]` (`0x89E9` = `0x8800 | (7<<6) | (5<<3) | 1`), moving x to the
object's one free halfword and leaving the y load alone.

**Port side.** Nothing in the cartridge writes `+0x0E`, and there is no spare
instruction slot to make it: at the press store the 16-bit x is live for exactly
one instruction, and the held store needs both of its slots to keep `+8` and
`+9` correct for the drag threshold. So the port writes it beside `kTouchSlot`
each frame. The object's address is not guessed — `FUN_020A8794` passes
`*DAT_020a8824` as the recogniser's `this`, and `DAT_020a8824` is `0x02157AF4`.
Re-reading it every frame follows a reallocated object, the same self-healing
rule `apply_cull_width` uses.

**It is six sites, not one.** The recogniser reads the current-position pair in
four places and the press pair in two more. Patching one moved the classifier's
query and left the click handler's on the old value. Five are patched now;
`020AF3AA` is deliberately not, because there press and current can genuinely
differ and substituting one for the other would collapse a real drag's
rectangle — including in band, where drag-select works today.

### What it moved, measured

| | before | after |
|---|---|---|
| the pick's touch operand (`LDRSH` at `02023FEC`) | 171, 203 — low bytes of −85, −53 | `FF8A`, `FF92` — −118, −110 |
| the classifier's flag word for a band click | took the "nothing found" arm every time, never set bit `0x40` | takes the entity-found arm 4 of 4, reaching `0x41C0` |
| the dispatch chain `020AA0FC → 020DD3B4 → 020E3518 → 020E2408` | none of it ran | **all four run** |
| the classifier's world query | 515 (camera + 171) | **262** — the target's own world position |
| the in-band control | 2–3 of 4 | **4 of 4**, its best yet |

## The one that is left

The band still selects 0 of 4, and the reason is now narrow and specific.
`FUN_020E2408` does not use the classifier's point. It builds its own rect from
`param_2 + 0x0C` and `param_2 + 0x14`, and that still reads **518 = 262 + 256**
— the same −106 taken as the unsigned byte 150.

So one more byte-derived value reaches it, by a path that is not any of the six
above. A scan of every remaining `ldrb` from `[r5,#6]` or `[r5,#8]` in
`020A0000..020B0000` shows **none of them feeds the point constructor within
sixteen bytes**, so the event's stored rect is filled somewhere else again —
most likely where the click event itself is constructed, rather than in the
gesture recogniser at all.

Finding that write is the whole of what remains. Everything else in the chain,
from the window pixel to the entity query, is now measured and correct.
