# The cheats are all ×1.5

**Status: both spell red bricks are multipliers, not flat bonuses, and both use
the same `x + (x >> 1)` idiom. Annotation 53.6% → 53.7%.** Four functions into
tier R; 84 of 84 checks pass. Two threads were chased and abandoned in this
pass, and both are recorded rather than hidden.

## Long Range Magic and Super Magic

`func_02002AD4`:

```asm
bl    rules
ldrb  r0, [r0, #8]           RedBrick_IncreaseSpellRange
cmp   r0, #0
ldrb  r0, [r4, #6]           the spell's range
addne r0, r0, r0, asr #1     x1.5
andne r0, r0, #0xff
```

`func_02002AA8`:

```asm
ldrb  r0, [r0, #9]           RedBrick_IncreaseSpellTime
cmp   r0, #0
ldrh  r0, [r4, #0x12]        the spell's duration
popeq                        unchanged
add   r0, r0, r0, asr #1     x1.5
```

**Spell range is a byte at `+0x06`; spell duration is a halfword at `+0x12`.**
"Long Range Magic" and "Super Magic" are both **×1.5, rounded down**, computed as
`x + (x >> 1)`.

That is worth stating precisely because "Super Magic" sounds like it might
double, and a port that assumed ×2 would be wrong by a third on every buff
duration in the game. Both cheats also apply **to every player**, not just the
local one — unlike the hero cheats in `docs/59`, which test the player id. The
getters have no such test.

The same idiom turns up in `docs/58`'s harvest path and `docs/59`'s damage
scaling. **`x + (x >> 1)` is this codebase's ×1.5**, and it is worth recognising
on sight.

### What is not established

Which record type holds `+0x06` and `+0x12` is **not** settled. There is no
spells file in `BP/` — the directory holds `Entities.ebp`, `Factions.fbp`,
`Animations.abp` and 130 `.tbp` tile files and nothing else — so the spell
records are most likely a static table inside the ARM9. I tested the guess that
they were `Factions.fbp` records at a `0x40` stride, and the result was
nonsense: the byte at `+0x06` was `129` in 124 of the records, which is a
faction index, not a range. **`docs/13` had already said `Factions.fbp` is 130
records with a string pool at `0x2084`, not a flat array**, so the stride was
wrong before the data was read.

Recorded as a pitfall rather than deleted: **re-read the document that solved a
file before assuming its layout.**

## Two threads chased and abandoned

`docs/63` left an explicit open question — where a vision source's radius comes
from — and this pass went after it and did not get there.

What the attempt did produce:

* **`func_020A17C0` = `Fog_IsAnyCornerVisible`.** It runs `BitArray2D_Get` on
  **four** corner points of the object it is handed — `(+4,+5)`, `(+0x1C,+0x1D)`,
  `(+0xC,+0xD)`, `(+0x14,+0x15)` — and returns true if any is visible, with the
  same four override flags `docs/63` found in the per-entity test. So a
  multi-tile building is visible when **any** corner is, not when its centre is.
* A search for functions writing both `+0x0C` and `+0x10` as bytes returned 12
  candidates, all generic struct initialisers. The signature was not specific
  enough and the radius is still unfound.

The second thread — locating the spell record — ended the same way, above.

**Two dead ends in one pass is a result, not a failure**, provided they are
written down: the next attempt should start from the vision-source *constructor*
rather than from field writes, and from the spell table's *static address*
rather than from a data file.

## A third function, from the same sweep

`func_0205F124` is the per-entity tick, and it is `RedBrick_Regeneration`'s
reader. Two phases:

1. **Expire timed effects.** A list at `+0x1E8` with a count at `+0x1EC`, two
   bytes per entry, walked with the index decremented on removal so the sweep
   does not skip the entry that shuffles down. Setting `+0x226` marks the entity
   dirty.
2. **Regenerate mana**, but only when `blueprint+0x5C == 0` — the Hero category
   from `docs/58`. It reads `entity+0x1A2` against `blueprint+0x64`.

`docs/11` established `blueprint+0x64` as mana and found it is **1000 for all 25
Hero-category units and 0 for everything else**. The code agrees from the other
direction: only category 0 regenerates. And `entity+0x1A0` is health
(`docs/59`), so `+0x1A2` sitting next to it as mana is consistent — though that
is an inference from adjacency, not a proof, and is marked as such.

## Tips and pitfalls

* **`x + (x >> 1)` is ×1.5.** It appears in three subsystems now. A cheat named
  "Super" need not be ×2.
* **Check which cheats test the player id.** The hero cheats in `docs/59` do;
  these two do not, so they apply to the AI as well.
* **Re-read the document that solved a file before assuming its layout.** The
  `Factions.fbp` stride was wrong and `docs/13` said so.
* **Write down where a dead end stopped.** "The radius is not a blueprint field"
  is worth as much to the next attempt as a positive result would be.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **53.7%** | 2,501 listed + 2,497 derived, 39 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
