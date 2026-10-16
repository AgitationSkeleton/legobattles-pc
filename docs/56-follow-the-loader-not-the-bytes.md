# Follow the loader, not the bytes

**Status: `.hps` is decoded. All ten particle files walk to their last byte,
and the framing came out of the ARM9's own seek loop rather than out of the
data. Data decoded 99.4% by file — 3,891 of 3,915 — with 24 files left, all of
them Mobiclip video.** 65 of 65 checks pass.

`docs/55`, an hour earlier in the same pass, read the `.hps` header as twelve
bytes, found the ten file sizes shared no divisor beyond 8, concluded there was
no record array to walk, and filed the format *partial*. It also wrote down what
to do about it:

> The way forward is written down rather than lost: follow the callback
> `func_02028F6C` installs, and read the parse routine it lands in.

Doing that took twenty minutes and settled the format completely. **The bytes
were never going to give it up, because the header is eight bytes and the third
"header field" was the first block's element count.** Statistics over ten files
cannot see that. One `add` instruction can.

## The path from a filename to a format

Four functions, each pointing at the next.

**1. `func_02028F6C` — request a particle system by index.**

```asm
ldr  r1, [r1, r7, lsl #3]        table 0x02126BB8, base + index * 8
```

`docs/55` used this for the table stride. It also allocates a 28-byte object,
and the object is what carries the answer forward.

**2. The object is a `Battles::ParticleDataEffect`.**

Its vptr, `0x02144020`, is in the shipped RTTI (`docs/07`). Its base vptr,
`0x0213AD20`, is `Battles::DListItem` — this is a list node, written in the
classic base-then-derived construction order:

```
+0x00  vptr        Battles::ParticleDataEffect, over Battles::DListItem
+0x04  next        the list link
+0x08  0
+0x0C  callback    invoked with (index, userdata) when the data is resident
+0x10  userdata
+0x14  index       the table index above
+0x18  buffer      the loaded file -- freed by both destructor slots
```

Both vtable slots are destructors, and both free `[obj+0x18]`. **That is what
identifies +0x18 as the file buffer**, before anything is known about its
contents.

**3. `func_0202914C` — index to buffer.**

```c
for (Item *it = mgr->head_64; it; it = it->next_04)
    if (it->index_14 == index) return it->buffer_18;
return NULL;
```

A linear walk of the same list. It confirms the layout from a second direction,
and it is the accessor every consumer goes through.

**4. `func_02029DAC` — bind the buffer to a `ParticleSystem`.**

```asm
ldr  r0, [r4, #0x54]      the system's particle index
bl   #0x202914c           -> the buffer
str  r0, [r4, #0x24]      base
add  r0, r0, #8
str  r0, [r4, #0x28]      cursor := base + 8
```

**`base + 8`.** The header is eight bytes, not twelve. That single instruction
is the correction `docs/55` could not have derived.

## The format, stated by `func_02029FF8`

```asm
ldr   r3, [r0, #0x24]      the file base
ldr   r2, [r3]             base[0]
cmp   r1, r2
movge r0, #0               n >= base[0] -> fail, so base[0] is the block count
bxge  lr
add   r2, r3, #8           the cursor starts eight bytes in
...
ldr   r3, [r2]             each block begins with an element count
add   r2, r2, #4
addne r2, r2, r3, lsl #4   then count * 16 bytes
str   r2, [r0, #0x28]
```

Every field of the layout is in those nine instructions:

```
+0x00  u32  block count          -- bounds-checked against the seek index
+0x04  u32  a 20.12 scalar: 40.0 (x7), 50.0 (x1), 80.0 (x2)
+0x08       block[count]:
                u32       element count
                byte[16]  element[count]
```

**Walking that consumes every byte of all ten files, exactly**, 470 blocks and
4,045 elements. A wrong stride drifts on every file; 16 drifts on none. The walk
is the check, and this time the code and the data agree — which is stronger than
either alone, because the code was read first and the data was not consulted
until the reading was fixed.

| file | bytes | blocks | scale | elements | peak | non-empty |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `DragonFireBall` | 19,584 | 50 | 40.0 | 1,211 | 28 | 50 |
| `TradeWindPart` | 10,160 | 50 | 40.0 | 622 | 14 | 50 |
| `HealthBoostStarBurst` | 8,416 | 50 | 40.0 | 513 | 11 | 50 |
| `LegoStudDestroy` | 7,632 | 50 | 40.0 | 464 | 13 | 49 |
| `DustConstruction` | 7,024 | 50 | 40.0 | 426 | 12 | 49 |
| `LegoSmallStudDestroy` | 3,624 | **20** | **50.0** | 221 | 12 | 20 |
| `LegoStudConstruction` | 3,456 | 50 | 40.0 | 203 | 6 | 50 |
| `SpellDecreaseEffect` | 2,656 | 50 | **80.0** | 153 | 4 | 50 |
| `SpellIncreaseEffect` | 2,656 | 50 | **80.0** | 153 | 4 | 50 |
| `HealthBoostTrailStar` | 1,472 | 50 | 40.0 | 79 | 4 | **24** |

## What a `.hps` actually is

Not a description of a particle system — **a recording of one**. The blocks are
frames and the elements are the particles alive in that frame.
`HealthBoostTrailStar` reads:

```
2 3 4 4 4 4 4 4 4 4 4 4 4 4 4 3 3 3 3 2 2 2 2 2 0 0 0 ... 0
```

A population rising to four, holding, decaying to two, then 26 empty frames. The
DS is not simulating this at runtime; it is playing it back. That is consistent
with the 30 Hz deterministic lockstep `docs/25` found — a runtime particle
simulation would be nondeterministic work in the middle of a lockstep frame, and
this game simply does not do one.

`LegoSmallStudDestroy` is the odd file at 20 blocks: a short effect, not a
different format.

Within a 16-byte element, from 4,045 samples:

* **word 2** is four channels that drift a step at a time between consecutive
  frames — `1E06061C`, `1E06061B`, `1E06061A` — with each byte in 0..31. That
  reads as a colour with 5-bit channels, fading.
* **word 3** takes only **14 distinct values across all 4,045 elements on the
  cartridge**, one of them covering 1,048. A field with 14 values over 4,045
  rows is a handle, not a quantity.
* **words 0 and 1** hold small signed quantities; word 1's high half advances by
  a near-constant step per frame, which is what a position under constant
  velocity looks like.

**Those four are characterisations, not conclusions**, and they are labelled as
such in `lb_particles.py` too. The framing is decoded; the element's field
meanings are not, and pretending otherwise would be the `docs/44` mistake again.

## The lesson, which is the point of this document

`docs/55` did the statistical thing: gather ten files, look for a common
divisor, find none, conclude the body is unwalkable. That reasoning was sound
and the conclusion was wrong, because **the ten files agreed on a structure that
had a variable stride, and no amount of staring at sizes recovers a variable
stride.**

The ROM was the source of truth the whole time. What this pass adds is the practical form of it:

> When a format resists the data, stop looking at the data. Find the code that
> reads it. A loader cannot be vague about a stride — it has to emit an `add`.

And the route in is mechanical, not lucky:

1. Find a string the format is named by, and the table that holds it.
2. Find the one function that indexes that table.
3. Read the object it allocates; the RTTI names its class for free.
4. Find the accessor that turns an index back into a buffer.
5. Its callers are the parsers.

Steps 3 and 5 are where this cartridge is unusually generous — `docs/07`'s RTTI
turned an anonymous 28-byte allocation into `Battles::ParticleDataEffect` at no
cost, which is what made the object layout readable at all.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| data decoded, by byte | **71.4%** | 86.6 MB of 121.2 MB |
| decompilation | 0.157% | 377 functions byte-verified |

24 files remain, all `.mods`, and they need a Mobiclip decoder rather than a
reading. **Data has gone as far as reading can take it.** The next segment by
closeness is annotation at 42.6%, and this pass leaves it 47 newly-attributable
functions better off: the whole particle module, 0x02028F00 through 0x0202AA40,
6,980 bytes, now has two class names and four named entry points anchoring it.
