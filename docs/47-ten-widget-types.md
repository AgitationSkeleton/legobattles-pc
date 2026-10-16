# Ten widget types, and a check that came free

**Status: the UI screen layout format is decoded. 132 of 132 files parse, all
1,062 widgets carry a named type, and record size agrees with type on every
one. `.bin` is promoted from *partial* to *decoded*, and data-decoded moves
from 6.8% to 10.3%.** 21 of 21 verification checks pass.

`docs/46` left this format one step short: the container was validated but the
records were extents with no meaning. The step was smaller than it looked.

## The type is a nibble

Reading further into `ScreenType::virtual_08`, past the container parse:

```arm
ldrb r0, [r5]            ; the widget record's first byte
lsls r0, r0, #0x18
lsrs r0, r0, #0x1c       ; (byte << 24) >> 28  -- the HIGH NIBBLE
cmp  r0, #0xd
bls  +2
b    <default>
adds r0, r0, r0
add  r0, pc              ; a 14-arm jump table
```

**The widget type is the high nibble of byte 0**, and there are fourteen arms.

Each arm constructs its widget, and a constructor's first act is to store its
class's vtable pointer — the signal `docs/43` generalised into tier V. Following
each arm to that pointer and through `docs/07`'s typeinfos names the lot:

| type | class | | type | class |
| ---: | --- | --- | ---: | --- |
| 1 | `ButtonWidget` | | 8 | `ScrollWidget` |
| 3 | `ComboWidget` | | 9 | `RadioButtonWidget` |
| 4 | `PaletteButtonWidget` | | 11 | `TextWidget` |
| 5 | `SpriteWidget` | | 12 | `BackgroundWidget` |
| 6 | `SlidingSpriteWidget` | | 13 | `ListWidget` |

Codes **0, 2, 7 and 10 fall to the default arm** — four gaps in a fourteen-wide
table.

## Two checks the data gave for nothing

The type codes came from the **code**; the records come from the **data**. They
had no opportunity to be fitted to each other, which makes their agreement
worth more than either alone.

**No record carries an unhandled code.** All 1,062 widgets use one of the ten
named types, and not one uses 0, 2, 7 or 10 — the exact four the jump table
ignores. Had the nibble not been the type, records would carry codes the table
does not handle, and roughly a quarter would, since four of fourteen codes are
unused.

**Record size is a function of type**, and I did not go looking for that:

| size | type |
| ---: | --- |
| 28 | `BackgroundWidget` ×81 |
| 32 | `TextWidget` ×198, `SpriteWidget` ×159 |
| 36 | `ScrollWidget` ×30 |
| 40 | `ListWidget` ×43 |
| 52 | `ButtonWidget` ×529 |
| 56 | `RadioButtonWidget` ×22 |

`docs/46` had already observed six distinct sizes and called it "the signature
of six widget types" without being able to say which. The sizes partition the
types cleanly — only `Text` and `Sprite` share one — with **zero disagreements**
across all 1,062 records.

Three of the ten named types (`Combo`, `PaletteButton`, `SlidingSprite`) appear
in no layout at all. They are implemented and unused, which is ordinary in a
shipped UI toolkit and is the sort of thing only the code-side reading could
have told us.

## What is decoded, and the gap that remains

A consumer can now open any screen layout, iterate its widgets, and know what
each one *is*. What it cannot do is read a field inside a record — the bytes of
a `ButtonWidget` are still unnamed.

> **Mostly closed by `docs/48`.** The 32-byte widget header is named from
> `ButtonWidget`'s constructor, which copies the record field by field so the
> access widths give every type. The four `s16` at `+0x04` are a bounding
> rectangle, confirmed on 1,062 of 1,062 records.

That gap is real but it is the difference between "structured" and "fully
interpreted", not between "opaque" and "understood", so the format takes the
*decoded* tier by the standard `docs/30` sets: a parser here reads it. The
promotion moves 132 files:

```
decoded   402  10.3%    was 268 / 6.8%
partial     1   0.0%    was 135
opaque     35   0.9%
```

## The pattern that has now paid three times

Every widget type here was named by the same move: **find where the engine
constructs the object, and read the vtable pointer it stores.**

- `docs/32` used it by hand on thirteen objective factories.
- `docs/43` generalised it into a derivation over the whole ARM9, worth 448
  annotations.
- Here it decodes a *file format*, because the type codes in the data are the
  arms of the dispatch that constructs those classes.

The RTTI `docs/07` recovered for free has now paid out in class names, in
annotation, and in a data format. **A vtable pointer is a type name written into
the binary**, and anywhere the game stores one, the thing being built can be
named.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **10.3%** | 402 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |

What remains opaque is 35 files: 24 `.mods` videos (34.6 MB), 10 `.hps`
particle systems, and `Animations.abp`. The two `UIMgrData.bin` blobs are
excluded from the layout set and unexamined.
