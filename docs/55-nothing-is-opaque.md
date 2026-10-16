# Nothing is opaque

**Status: the *opaque* tier and the *known format* tier are both empty. Every
one of the 3,915 files on this cartridge is now framed by a reader in this
repository — 3,881 decoded, 34 partial, nothing unstructured.** Data decoded
99.1% by file, 71.4% by byte. Eight new checks; **64 of 64 pass**.

> `docs/56`, later in the same pass, took `.hps` from partial to decoded:
> **3,891 decoded, 24 partial, 99.4% by file.** The tier table and the `.hps`
> section below are updated; the reasoning that got them wrong is kept.

`docs/04` filed 35 files as *opaque — no known structure*, and one as *known
format, no parser here*. That was written when the project was three passes old
and nothing had been opened. Nothing had been opened since either.

All four turned out to have structure sitting in the first sixty-four bytes.

## `banner.bnr` — the only checksum on the cartridge

2,112 bytes, and the layout is settled by the size before a byte is
interpreted:

```
+0x000  u16       version, 0x0001
+0x002  u16       CRC-16 of 0x020..0x83F
+0x004  byte[28]  reserved, zero -- and they are
+0x020  byte[512] the 32x32 icon, 4bpp, as 4x4 tiles of 8x8
+0x220  byte[32]  16 BGR555 palette entries
+0x240  u16[128] x6  the six titles, UTF-16LE
```

`0x240 + 6 × 256 == 0x840 == 2112`.

**The `u16` at +0x002 is a CRC-16 over the entire body**, reflected polynomial
0xA001, initial value 0xFFFF. Computed: `0x758E`. Stored: `0x758E`.

This is different in kind from every other check in this repository. All the
others are structural identities — offsets landing on each other, counts
agreeing — and they establish that a *layout* is right. A matching CRC
establishes that **every one of 2,080 body bytes is exactly what the cartridge
author signed**. One wrong byte anywhere would break it.

The six titles:

| slot | title |
| --- | --- |
| Japanese, English, French, Italian, Spanish | `LEGO® Battles` |
| German | `LEGO® Strategie` |

The game was retitled for Germany. The icon uses 13 of its 16 palette entries.

## `Movies/*.mods` — 20 minutes of video, and its keyframe index

24 files, 34.6 MB. The first eight bytes are `MODS` then `N3\n\0`, and the
64-byte header states everything about the stream except how to decode it:

```
+0x08 frame count     +0x20 largest frame, in bytes
+0x0C width,   256    +0x24 offset of a 3,124-byte per-movie block
+0x10 height,  144    +0x28 offset of the keyframe index
+0x14 15 fps          +0x2C keyframe count
+0x1C 32,000 Hz       +0x30 0x4548
```

Six of those words are **identical in all 24 files**. Together: 18,130 frames,
**20.1 minutes** at 15 frames per second, 256×144 letterboxed onto the DS's
256×192 screen, with 32 kHz audio.

Four identities, each over all 24 files:

1. `file size == (+0x28) + 8 × (+0x2C)` — the keyframe index ends the file.
2. Index entries are `(frame, offset)` pairs, **both strictly increasing**.
3. The first entry is frame 0 at offset **52**, in every file.
4. The last keyframe's frame is below the frame count and its offset is below
   the block at +0x24.

Identity 3 is the one that had to be read rather than assumed. An offset table
whose first entry is 52 in a file whose frame data starts at 64 looks broken —
until the constant 12 falls out of it. **Keyframe offsets are measured from
header offset +0x0C, not from the file start**, and 52 is not an accident 24
times running.

### Why this stays *partial*

`MODS` is Mobiclip, a proprietary codec, and nothing here decodes a frame. The
rule `docs/49` set stands: a format is decoded when a parser here reads it *and
a consumer can use the result*. A frame table is not a picture. 24 files,
34.6 MB, **partial**.

## `Particles/*.hps` — the table is the better find

Ten files, and the more useful half of the work was in the ARM9 rather than in
them.

The ARM9 holds an array of `{const char *path, u32}` records at **0x02126BB8**.
Its stride is not inferred from the data — its one consumer, `func_02028F6C`,
states it:

```asm
ldr  r1, [r1, r7, lsl #3]        base + index * 8
```

That function takes `(index, callback, userdata)`, allocates a 28-byte request,
stores the callback at +0x0C, the userdata at +0x10 and the index at +0x14, and
queues an asynchronous load. **Particle systems are addressed by index into this
table and never by name.**

> **Resolved by `docs/58`.** The word at `0x02126D94` that made slot 59 look
> "out of family" is not part of this table at all -- it is the first four bytes
> of the per-category unit allowance table. The particle table ends at slot 59
> and the boundary is now exact rather than cautious.

The table has 60 slots. 16 carry a path; 44 point at an empty string. Comparing
those 16 against the ten files that shipped:

```
9  named in the ARM9 and present on the cartridge
7  named in the ARM9, NOT on the cartridge -- cut before release
1  on the cartridge, NOT named in the table -- unreachable through this path
```

The seven cut systems are `SpellMineEffect`, `SpellFireBall`,
`SpellLightningBolt`, `SpellChoppingBuff`, `SpellMonkeySwarm`,
`SpellForrestSpawn` and `TestPS`. `TestPS` is a developer test asset. The other
six are all *spell* effects — and those spells shipped. `docs/24` read
`Sim::ProjectileSpellFireBall`, `Sim::ProjectileSpellLightningBolt` and
`Sim::ForrestSpell` out of the RTTI, and `docs/52` found
`BANK_SPELL__MONKEYSWARM` and `BANK_SPELL__CHOPPINGBUFF` in the sound header. So
the spells kept their classes and their sounds and lost their particle files, or
their effects are produced some other way this pass has not looked at.

One detail worth keeping: the particle path is `SpellForrestSpawn.hps` and the
RTTI class is `Sim::ForrestSpell`. **The same misspelling, in the asset name and
in a C++ class name**, from two different parts of the build. Small, but it is
the kind of agreement that says these came from one codebase and neither name
was transcribed by anyone here.

The orphan is `HealthBoostTrailStar.hps`, which ships and is not in the table.

### The header

> **Superseded by `docs/56`, later the same pass.** This section originally read
> the header as twelve bytes -- `+0x00` a version-like word, `+0x04` a scalar,
> `+0x08` "a small count, 0 to 4" -- and concluded the body was unwalkable, so
> `.hps` was filed *partial*. Following the loader instead of the bytes settled
> it: **the header is eight bytes**, `+0x00` is a block count, and what looked
> like a third header field is the first block's element count. All ten files
> now walk to their last byte and `.hps` is **decoded**. The original wording is
> corrected here rather than elsewhere.

```
+0x00  u32  block count -- 50 in nine files, 20 in one
+0x04  u32  a 20.12 scalar: 40.0 (x7), 50.0 (x1), 80.0 (x2)
+0x08       the blocks; see docs/56
```

`+0x04` is exact in 20.12 in all ten — the game's fixed point again
(`docs/25`, `docs/51`).

## `BP/Animations.abp` — a walk that lands on the last byte

2,580 bytes. `docs/04` called it opaque and `docs/20` added that **the ARM9
never names it** — there is no `Animations.abp` string anywhere in the image, so
no loader can be found and read. Whatever comes out has to come from the bytes.

```
+0x00  u8   key a          +0x04  u8   value count
+0x01  u8   key b          +0x05  u8   0xA1, on every record
+0x02  u8   key c          +0x06  u16  value[count], little-endian
+0x03  u8   key d
```

**The walk is the check.** Chaining `6 + 2 × count` from offset zero consumes
**2,580 of 2,580 bytes** and yields 180 records, every one carrying `0xA1` at
+0x05. A wrong record length drifts — it runs off the end or stops short — and
this does neither.

And the keys factor:

```
key a   12 values: 0..5 and 17..22        two consecutive runs of six
key b    3 values: 0, 1, 2
(a, b)  36 groups, EACH holding exactly 5 records
        12 x 3 x 5 = 180
```

Six is the faction count everywhere else in this game (`docs/53`), and key `a`
is two runs of exactly six. That 36 groups are *all* the same size is not
something a wrong framing produces.

The `u16` values are **not** interpreted. In the low groups they are small and
step by three; in the high groups they run near 33,000 and step by four, which
reads like an offset into a resource this pass has not identified. The record
structure is decoded; the value semantics are not, and the two are kept apart.

## The tiers, and what killed them

```
             files      %          bytes       %
decoded      3,891   99.4%    86,585,152   71.4%   after docs/56
partial         24    0.6%    34,658,084   28.6%   24 .mods
known            0      -                          was 1
opaque           0      -                          was 35
```

Three passes ago there were 823 partial, 2 known and 35 opaque. What emptied
them was not a new technique. It was **opening the files**.

`docs/52` found the `.sadl` had been filed as "audio body not decoded" about a
file with no body. `docs/54` found the 62.9 MB `.sdat` had sat unread behind a
tier that said *somebody already understands this*. This pass found that four
"opaque" formats declare their structure in their first sixty-four bytes.

**Three passes in a row, the unread work was hiding in a tier label rather than
in a hard problem.** The label was accurate when written and nobody re-read it.
That is the pitfall worth carrying forward, and it is now recorded in `docs/30`
next to the tier table itself.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded, by file | **99.4%** | 3,891 of 3,915, after `docs/56` |
| data decoded, by byte | **71.4%** | 86.5 MB of 121.2 MB |
| decompilation | 0.157% | 377 functions byte-verified |

Data is now within 24 files of complete — `docs/56` took the ten `.hps` the
rest of the way in the same pass — and those 24 are the one format whose body is
a proprietary video codec. The next segment by closeness is **annotation at
42.6%**.
