# The screen layout container

**Status: the `UI/*.bin` container is decoded and validated on 132 of 132
screen layouts. The widget record bodies are not interpreted, so the format is
classified *partial* — but 133 files move out of *opaque*, which falls from 168
files to 35.**

`docs/44` failed at this format by guessing and said so. `docs/45` followed the
loader two hops and stopped at a virtual dispatch. This pass took the third hop.

## The hop that mattered

`func_020E484C` calls `[[r4]+0x20]` — vtable slot 8 of the screen. Slot 8 is
per-screen, so the layout parse looked like 54 different functions. It is not:
every screen's slot 8 **calls the base first**.

```arm
Battles::User::Options::virtual_08:
    bl   Battles::User::ScreenType::virtual_08   ; <- the base parses
    ldr  r0, [r4, #0x1c]                         ; then walk what it produced
    ldrb r1, [r0, #2]                            ; the widget count
```

So there is one parser after all, `ScreenType::virtual_08`, and the derived
implementations only wire up their own widgets afterwards. **A per-class virtual
that begins by calling its base is one function, not fifty-four** — and the
shape is visible in the first instruction.

## The container

```arm
if (buf == 0) { this->field_14 = 0; return; }
this->field_1C = buf;                 // used IN PLACE, never copied
id = buf[3];
if (id < 0x62) screen_registry[id] = this;
count = buf[2];
base  = buf + 0x0C + count * 4;       // widget records follow the table
```

which gives:

| offset | | |
| --- | --- | --- |
| `0x00` | `u8` | version — **6** in 107 files, **4** in 25 |
| `0x01` | `u8` | duplicates the widget count, in **132 of 132** |
| `0x02` | `u8` | **widget count** |
| `0x03` | `u8` | **screen id**, registered into a 98-entry table |
| `0x04` | `u8[8]` | not read by the base parser |
| `0x0C` | `u32[count]` | widget offsets, relative to `0x0C + count*4`; `[0]` is always 0 |

**132 of 132 parse**, with every check holding: `offsets[0] == 0`, offsets
strictly increasing, and the last widget starting inside the file.

`docs/44` had recorded "byte 1 equals byte 2 in 132 of 134" as a curiosity
without an explanation. It is the count, stored twice.

## Two things the guard explained

`if (id < 0x62)` looked like defensive coding. It is not — **38 files carry
screen id 255**, and 255 is the only id at or above `0x62` anywhere in the set.
So `0xFF` is the "not a registered screen" sentinel, and the guard exists to
skip exactly those. The other 94 ids are distinct, with a single collision.

And the layout is **used in place**: `this->field_1C = buf` stores the loaded
file itself, with no copy or transformation. A port must keep the buffer alive
for the screen's lifetime; the widgets are pointers into the cartridge data.

## Widget records: six sizes, not interpreted

1,062 widgets across the 132 layouts, in exactly **six distinct sizes**:

| size | count |
| ---: | ---: |
| 28 | 81 |
| 32 | 357 |
| 36 | 30 |
| 40 | 43 |
| 52 | 529 |
| 56 | 22 |

Six sizes, all multiples of four, is the signature of **six widget types** —
each a fixed-size record. That is a strong lead and it is not a decode: nothing
here names a single field.

> **Resolved in `docs/47`.** The type is the high nibble of a record's first
> byte, dispatched through a 14-arm jump table, and all ten live types are
> named from the vtable each arm's constructor stores. Record size turns out to
> be a function of type, with zero disagreements across 1,062 records. The
> format is now *decoded*.

The bodies plainly contain structure — the first six `u16` of a record look like
two rectangles, and the base parser reads a bitmask at `+0x12` — but reading
plausible numbers is what `docs/44` did wrong. **The record is reported as an
extent, not as fields.**

## Why this is *partial* and not *decoded*

`docs/30` counts a format as decoded when a parser here reads it. This parser
reads the container completely and validates it on every file, but a caller
still cannot ask it what a widget *is*.

That is the same standing as `Sound/*.sadl`, whose header is understood and
whose body is not, so it takes the same tier. The effect on the figures:

```
decoded    268   6.8%    (unchanged)
partial    135   3.4%    (was 1)
opaque      35   0.9%    (was 168)
```

**Data decoded does not move.** What moves is *opaque*, from 168 files to 35 —
and that is the honest way to report it, because the cartridge is better
understood while the specific claim the metric measures is not yet true.
Promoting `.bin` to decoded needs the six widget types named.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | 6.8% | unchanged; opaque 168 → 35 files |
| decompilation | 0.157% | 377 functions byte-verified |

## The method, three passes on

This format took three passes and the difference between them is the whole
lesson:

| pass | approach | result |
| --- | --- | --- |
| `docs/44` | hypothesise the layout from hex dumps | three rules, all wrong |
| `docs/45` | follow the loader | reached a virtual dispatch, stopped |
| here | notice the virtual calls its base | the container, exactly |

Guessing produced three confident wrong answers in one pass. Reading produced
the right one in three. **The reading was never blocked — it was only ever one
more hop**, and the hop looked like fifty-four functions until the first
instruction of one of them showed it was one.
