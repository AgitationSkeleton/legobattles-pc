# A table I read wrong

**Status: `docs/58` mapped the table at `0x02126D94` onto blueprint categories.
It is indexed by *build-limit class*. Corrected in place, and every entry is now
a plausible cap where most of them were nonsense before.** Two new checks; **90
of 90 pass**. `docs/65`'s open-question list is down to two.

`docs/58` published this and flagged it:

> **The rest are read, not explained.** 4 for Melee and 7 for Ranged are not
> plausible unit caps — you can field more than four soldiers — so the byte
> means something else for those categories, and this document does not know
> what.

The instinct was right and the diagnosis was wrong. The byte does not mean
something else *for those categories*; **those are not categories.**

## What the index actually is

`docs/58` traced the table to `func_020863D8` (`ldrb r4, [r2, r8]`) and observed
that its only caller passes `r1 = r4` from further up. It did not follow that
chain. Doing so lands in `func_020E01F8`, the HUD, which asks for exactly two
values — `1` and `2` — and formats them into two on-screen counters.

Two constants, not a loop over twenty categories. That is the tell.

The other reader, `func_020860A8`, settles it:

```asm
ldr   r2, [pc, #0x2bc]      the table
ldrb  sl, [r2, fp]          sl = table[class]        <- the cap
cmp   fp, #8
addls pc, pc, fp, lsl #2    a nine-arm jump table on the class
```

Each arm counts a **set** of blueprint categories:

| class | cap | what it counts |
| ---: | ---: | --- |
| 0 | **1** | Heroes — category 0 |
| 1 | **20** | Minifigures — categories 1–4 (Builder, Melee, Ranged, Mounted) |
| 2 | **4** | Specials — categories 5–6 (Transport, Siege) |
| 3 | **7** | Towers — categories 0x0F, 0x0D, 0x0E summed |
| 4 | 14 | unresolved — a two-level jump table over 0x0D..0x13 |
| 5 | 29 | unused — the arm returns 0 without counting |
| 6 | **20** | Walls — category 0x13 |
| 7 | **8** | Bridges — category 0x11 |
| 8 | **0** | Gates — category 0x12 |

**One Hero, twenty Minifigures, four Specials, seven Towers, twenty Walls, eight
Bridges.** Every one of those is a build limit a designer would set. Read as
categories they were 4 Melee, 7 Ranged, 14 Mounted, 29 Transport — which is why
`docs/58` could not make sense of them.

The remaining figure is `cap - (live + pending)`, where pending is
`player[0xC5 + class]` — so a queued unit occupies its slot before it exists.

## Why the mistake happened, and what would have caught it

`docs/58` had the right evidence and stopped one hop short. It had:

* `func_020863D8` reading `table[r8]`, with `r8` unexplained;
* a caller passing `r1` from somewhere unexamined;
* **values that did not make sense as caps**, explicitly noted.

The third of those is the important one. **An implausible value is a signal
about the reading, not a fact about the game.** `docs/58` treated it as
something to flag and move past. Flagging was better than guessing, and it was
still not enough — the flag should have been a stop condition.

There is a rule here worth keeping: **when a table's values are implausible,
suspect the index before the values.** A table of caps that reads
`1, 20, 4, 7, 14, 29` against the wrong key is exactly what a right table with a
wrong index looks like.

`docs/60` had already stated the neighbouring rule — *the same offset in two
record types is two fields* — and this is its sibling: **the same table under two
index spaces is two tables.**

## What the check does now

Two checks replace the old one:

* the first three entries are 1, 20 and 4 — the Hero, Minifigure and Special
  caps the HUD displays;
* `func_020860A8` still dispatches on nine classes, counted from its jump table
  rather than quoted.

The second one failed on its first run, at 8 arms against 9 documented. That was
my scan window truncating the table at 0x40 bytes, not the claim being wrong —
widening it to 0x60 shows 10 arms (the nine classes plus the out-of-range
fall-through). **A check that fails deserves a diagnosis before a fix**, and in
this case the check was wrong and the claim was right.

## `docs/58`'s other findings are unaffected

The correction is contained. `min(20, 4 + 4 × completed Farms)` still holds —
`func_02085E5C` computes the farm term and `func_020863D8` clamps it against
`table[1]`, which is class 1 and is still 20. The tree payout, the harvest
interval, the brick cap and the construction rules were all read from
instructions, not from this table, and none of them move.

What changed is one paragraph that mapped nine bytes onto the wrong names.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.7% | 2,501 listed + 2,504 derived, 50 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
