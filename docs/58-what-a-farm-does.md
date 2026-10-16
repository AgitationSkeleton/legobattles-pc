# What a Farm does

**Status: `docs/53`'s open question is answered, with numbers.** The Minifigure
allowance is `min(20, 4 + 4 × completed Farms)`, a tree chop pays 75 bricks or
100 with a Mill, "Special units" is exactly categories 5 and 6, and a structure
counts toward nothing until its construction progress reaches 100. Seven new
checks re-derive those constants from the instructions; **74 of 74 pass**.

`docs/53` ended with a flag rather than a guess:

> `resource_yield` is 25 on the `MINE` and **0 on the `MILL` and the `FARM`**
> for every faction. Whatever makes a lumber mill or a farm produce is not this
> field. Flagged, not guessed.

## The game answers it in its own words

`LOC/*.lng` carries a block of tutorial tooltips at IDs **930 through 1034**,
three consecutive IDs per line of text. `docs/07` decoded these files two years
of passes ago; nobody had read this block.

| building | what the game says |
| --- | --- |
| `FARM` | Farms increase the maximum number of Minifigures and Special units you can create. |
| `MILL` | Mills increase LEGO Bricks gained from harvesting trees. |
| `MINE` | Mines can be placed on special locations to harvest LEGO Bricks. |
| `BASE` | A Base produces Builders and Heroes, and also heals nearby Hero units. |
| `BARRACKS` | A Barracks can produce soldiers. |
| `VEHICLECENTER` | A Special Factory produces powerful units. |
| `SHIPYARD` | A Shipyard produces waterborne Special Units. |
| `TOWER` | Towers defend an area and can be upgraded in later missions. |
| `WALL` | Walls stop the enemy from closing in. |
| `BRIDGE` | Bridges can only be placed at specific locations on the map. |
| `TRANSPORT` | Transports can hold 4 Minifigures and 2 Specials. |
| — | Barracks, Special Factories and Shipyards can queue up to 3 units to build. |
| — | Only your Hero can collect Gold Bricks. |
| — | Heroes can heal nearby units. |

**So the Mill is a multiplier on harvesting, not a producer** — which is exactly
why its `resource_yield` is zero — and **the Farm raises a population cap**.

That is the answer, but it is a sentence, not a number. The numbers are in the
ARM9.

## The allowance, from the instructions

`func_02085E5C`:

```asm
bl   #0x2085db0        count the player's Farms
lsl  r0, r0, #2        x 4
add  r0, r0, #4        + 4
```

and `func_020863D8` clamps it:

```asm
cmp  r8, #1                    category 1
lsl  r2, r5, #2                farms x 4
cmp  r4, r5, lsl #2
movle r2, r4                   min(table[1], farms x 4)
strb r2, [r1, #0xcf]           player allowance array
```

**Minifigure allowance = `min(20, 4 + 4 × completed Farms)`.**

| farms | allowance |
| ---: | ---: |
| 0 | 4 |
| 1 | 8 |
| 2 | 12 |
| 3 | 16 |
| 4 | **20** |
| 5 | 20 |

**Four farms reach the cap. A fifth does nothing.** That is a concrete piece of
strategy the game never tells the player.

## Which units are "Special"

`func_02071DC4` routes to one of the two allowance functions:

```asm
cmp   r1, #6
cmpne r1, #5
bne   ...              anything else uses the Minifigure allowance
```

**Categories 5 and 6 — Transport and Siege — are the Specials.** Everything
else with a population cost is a Minifigure. That matches the roster in
`docs/53`: `SIEGEONE`, `SIEGETWO`, `SIEGETHREE` and `TRANSPORT` are the four
"Special" slots per faction, and the other six unit slots are Minifigures.

`func_02085F74` counts them with two calls, one per category, and adds them:
the two Special categories share **one** allowance rather than having one each.

## Two rules from the counting function

`func_020859F4(player, category, live)` walks the player's entity list —
handles at `player+0xF8`, count at `player+0xFC`, **two bytes per handle** — and
contains two identical 20-arm jump tables on `blueprint+0x5C`:

```asm
ldrb r1, [r0, #0x5c]
cmp  r1, #0x13
addls pc, pc, r1, lsl #2
```

Both tables send **categories 0–6 one way and 7–19 the other**.

### 1. The unit/structure split is at category 7

`lb_blueprints.py` has always written `is_structure = category_id >= 7`. This is
where that comes from. **It is not an independent confirmation** — the tool
already encoded the rule, and saying the data agrees with it would be circular —
but it upgrades the rule from an inference to a reading, and the verification
check re-derives the 7 from the jump table's arms rather than quoting it.

### 2. A structure counts only when it is finished

The structure arm ends:

```asm
ldrb r0, [r7, #0x22e]
cmp  r0, #0x64
bne  skip
```

**`entity+0x22E` is construction progress as a percentage, and only 100
counts.** A half-built Farm raises nobody's cap; a Barracks under construction
is not a Barracks yet. The same test appears in `func_02085DB0`, the farm
counter, so it is the general rule and not a special case.

Category `0x14` (20) is a wildcard — `if (category != 0x14 && ...)` skips the
filter — so 20 means "any category" in this API.

## A loose end from `docs/55`, tied off

`docs/55` walked the particle-system table at `0x02126BB8` and stopped at slot
59 with a note:

> slot 59 at 0x02126D90 has 0x07041401, out of family — treat the table as
> ending here

**It is out of family because it is not in that table.** `0x02126D94` is the
allowance table, and `01 14 04 07` is its first four bytes read as a word.
`docs/55` made the right call for the wrong-but-safe reason, and the boundary is
now known exactly rather than guessed at.

## The table, and what is not known about it

> **CORRECTED by `docs/68`.** The table below is **not indexed by blueprint
> category**. It is indexed by *build-limit class*, and `func_020860A8`
> dispatches on that class through a nine-arm jump table, each arm counting a
> whole *set* of categories. The section below noticed that most entries were
> not plausible caps and said so; that was the symptom. Read correctly the
> entries are: 0 Heroes 1, 1 Minifigures 20, 2 Specials 4, 3 Towers 7,
> 6 Walls 20, 7 Bridges 8 -- every one a sensible build limit. The category
> table below is wrong and is left standing so the mistake is visible.

```
addr   0x02126D94
bytes  01 14 04 07 0E 1D 14 08 00 05 00 00 00 00 00 00 00 00 01 00
```

| category | name | byte |
| ---: | --- | ---: |
| 0 | Hero | **1** |
| 1 | Builder | **20** |
| 2 | Melee | 4 |
| 3 | Ranged | 7 |
| 4 | Mounted | 14 |
| 5 | Transport | 29 |
| 6 | Siege | 20 |
| 7 | Base | 8 |
| 9 | Mine | 5 |
| 18 | Gate | 1 |
| all others | | 0 |

**Two entries are pinned by the code.** Entry 0 is 1 — one Hero, which is what
the game does. Entry 1 is 20, and it is the value `func_020863D8` takes the
minimum against, so it is the Minifigure ceiling.

**The rest are read, not explained.** 4 for Melee and 7 for Ranged are not
plausible unit caps — you can field more than four soldiers — so the byte means
something else for those categories, and this document does not know what.
`docs/44` is what guessing costs; the bytes are recorded and the question is
left open, the same way `docs/53` left this one.

## And what a Mill does

The tooltip says "Mills increase LEGO Bricks gained from harvesting trees". The
same trick that found the allowance finds the amount: `docs/28` lists
`RedBrick_DoubleStuds` -- "Double LEGO Bricks" -- with `func_0206D0E0` among its
readers, and that function **is** the tree payout.

```asm
mov  r1, #8                  category 8 = MILL
bl   #0x2085994              the player's Mills
...
ldrb r0, [r0, #0x22e]
cmp  r0, #0x64               fully built?
moveq r4, #0x19              -> 25, and BREAK OUT of the loop
...
bl   #0x205f114              a per-entity multiplier
add  r1, r4, #0x4b           75 + (25 if a Mill)
mul  r4, r1, r0
...
lsleq r4, r4, #1             "Double LEGO Bricks" doubles it
bl   #0x20866e0              credit the player
```

**A tree chop pays 75 bricks, or 100 with a Mill.** And the `beq` after
`moveq r4, #0x19` jumps *past* the loop: the search stops at the first completed
Mill, so **a second Mill adds nothing**. Like the Farm, the Mill has a ceiling
the game never mentions -- except the Mill's ceiling is one.

`func_020531F8`, the `RedBrick_FastHarvest` reader, returns **150** unless the
cheat is on. `docs/25` puts the simulation at 30 Hz, so that is **five seconds
per chop** -- and one tick with the cheat.

Mining is the same shape. `func_0206D7F0` reads the yield from the entity,
doubles it under `RedBrick_FastMining`, and credits it.

## Where the bricks go

`func_020866E0(player, amount)`:

```asm
ldr r2, [r0, #0x90]          the brick balance
add r2, r2, r1
ldr r1, =500000
cmp r2, r1
bls  ...                     clamp
```

**`player+0x90` is the LEGO Brick balance and it is hard-capped at 500,000.**
The setter behind it, `func_02086634`, compares the old and new values against
two thresholds at `player+0x94` and `+0x98` and raises a UI event when the
balance crosses one -- which is how "You need <1> more LEGO Studs" stops being
true.

## A field width, settled by the initialiser

`blueprint+0x6C` is `resource_yield`, and two functions read it at two widths:

```asm
func_02000C6C   ldr r0,[r0,#8] ; ldrh r0,[r0,#0x6c]     halfword
func_0205CC14   ldrb r0,[r0,#0x6c] ; strh r0,[r4,#0x16]  byte
```

That matters, because `blueprint+0x6D` is the attack interval (`docs/11`, and it
matches the rebalance sheet exactly). Six blueprints have a non-zero `0x6C` --
the six Mines, at 25 -- and **all six have 30 at `0x6D`**, so the halfword read
gives 7,705 rather than 25.

**The initialiser is authoritative.** `func_0205CC14` is what copies the record
into the live entity, and it uses `ldrb`; the mine payout then reads the entity's
copy at `+0x16`. So `resource_yield` is a `u8` and `lb_blueprints.py` is right.

What `func_02000C6C` is doing is **not resolved** -- it reads two adjacent
fields as one halfword, which is either a different meaning for that pair or a
latent bug in the game. Flagged, not guessed. The general rule this produces:
**when two sites disagree on a field's width, the one that copies the record
into the object defines it.**

> **Resolved by `docs/60`.** Neither -- the two functions are handed *different
> record types*. `+0x6C` is `resource_yield` (u8) in a `UnitBlueprint` and the
> unlock slot (u16, 0..288, `docs/22`) in a `CollectableItemBlueprint`, and
> `func_02000C6C` sits immediately after `func_02000C14`, the
> `CollectableItemBlueprint` constructor. The halfword is right for the record
> it gets. The sharper rule: **the same offset in two record types is two
> fields** -- ask which type a reader was handed before concluding it is wrong.

## Tips and pitfalls

* **Read the game's own text before reading its code.** A tutorial tooltip
  answered in one sentence what the blueprint fields could not, and it pointed
  at exactly which code to read next. `docs/07` had these strings decoded from
  the beginning.
* **The red-brick table is a cheat sheet for finding subsystems.** `docs/28`
  recorded `RedBrick_FullAllowance` — "Full Unit Cap" — with its readers listed.
  Those four functions *are* the unit cap. A cheat that modifies a system names
  the system's code for you, and this cartridge lists eighteen of them.
* **Structures are not structures until 100.** Anything counting buildings has
  to test `entity+0x22E`, and code that forgets to will over-count during
  construction.
* **When two sites disagree on a field width, the initialiser wins.** It is the
  code that defines the field; every other reader is using it.
* **Both economy buildings have a hidden ceiling.** Four Farms max the
  Minifigure cap and one Mill maxes the harvest bonus. Neither is stated
  anywhere in the game.
* **Say which checks are circular.** The category-7 split agrees with
  `is_structure` because `is_structure` is *defined* as the category-7 split.
  Reporting that as confirmation would have been the easiest available mistake
  in this pass.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.3% | 2,501 listed + 2,460 derived |
| decompilation | 0.157% | 377 functions byte-verified |
