# How the game works

**Status: one reference for every gameplay constant this project has
established — 47 of them, each re-derived from the cartridge on every run
rather than transcribed, plus the one question that is still open.** Two new
checks; **88 of 88 pass**, and `docs/66` closed one of the questions that was
open when this was written. The table is
[`docs/data/gameplay_constants.tsv`](data/gameplay_constants.tsv).

`docs/58` through `docs/66` each read one subsystem and each left its numbers in
prose. Nine documents is where a reader gives up, and prose goes stale. This is
the synthesis, and `tools/lb_gameplay.py` is what stops it rotting: every row is
computed from the image, and if an instruction a constant was read from stops
being there, the row says `MISSING` and the check fails.

Three kinds of row, and the kind is in the table:

| kind | meaning |
| --- | --- |
| `instruction` | an immediate or literal read back out of the disassembly |
| `data` | a value read from a blueprint or a static table |
| `derived` | arithmetic over those, shown with its inputs |

22 of the 47 are `instruction` rows. That is the point: they cannot drift.

## The simulation

**30 Hz, deterministic lockstep** (`docs/25`). Every interval below is in ticks
and divides by 30.

Fixed point is **20.12** throughout — established from the simulation's own
arithmetic in `docs/26` and confirmed independently by the 3D exporter in
`docs/51`, where `pos_scale × inv_pos_scale == 2²⁴` on all 39 models.

The 20.12 multiply **rounds to nearest**: `func_0204F2D8` is a 64-bit `smull`
that adds `0x800` before shifting. A port that truncates will drift, and in a
lockstep game a one-point drift is a desync.

## The roster

**Six factions × twenty blueprints** — ten unit slots and ten structure slots,
named identically across all six by the game's own shipped C header (`docs/52`,
`docs/53`).

**The buildings are one row six times.** 89 of the 90 structure stat cells are
byte-identical across all six factions; the sole exception is the King's basic
Tower at attack range 5 where everyone else is 6. `mana`, `build_time` and
`resource_yield` are identical across all six on all twenty slots, and `cost` on
17 of 20. **The economy is symmetric; differentiation is entirely in what units
do once built.**

The campaign's narrative pairing of the factions leaves **no trace** in the
balance data — identical-cell counts run 59 to 71 out of 90 with no gap
anywhere (`docs/53`).

## The economy

```
tree chop           75 bricks, or 100 with a Mill
                    only the FIRST completed Mill counts; a second adds nothing
harvest interval    150 ticks = 5.0 seconds
brick balance       player+0x90, hard-capped at 500,000
```

**The Mill is a multiplier on harvesting and the Farm is a population cap** —
neither produces, which is why `resource_yield` is 0 on both (`docs/58`).

```
build limits, by class (docs/68 -- NOT by blueprint category):
  Heroes        1     Minifigures  20     Specials      4
  Towers        7     Walls        20     Bridges       8

minifigure allowance   min(20, 4 + 4 x completed Farms)
                       four Farms reach the cap; a fifth does nothing
special units          categories 5 and 6 (Transport, Siege), one shared limit
```

Both economy buildings have a ceiling the game never states: **four Farms, one
Mill.**

## Building and training

Six preconditions, in the order `func_02071DC4` evaluates them (`docs/60`):

1. the producing structure exists
2. **its construction progress is 100** — a building under construction
   produces nothing, and counts toward nothing
3. its owner exists
4. the allowance, with **category 0 (Hero) exempt entirely**
5. a per-blueprint availability count
6. **cost `blueprint+0x5E` against the purse `player+0x90`**

Build time is `blueprint+0x60` ticks, floored at 1 — a Castle's 900 is **30
seconds**, a Farm's 360 is **12**. **Health grows as a building builds** and is
clamped to `blueprint+0x62`, so a half-built Barracks really is half as tough.
Progress above **98** fires a one-shot event, and on completion **category 0x11
(Bridge) gets an extra virtual call nothing else does**.

**The unit/structure split is at category 7**, read from two 20-arm jump tables
in `func_020859F4`.

## Combat

```
roll   = dmgMin + rand(0, dmgMax - dmgMin)
scaled = fixmul(roll << 12, target+0x174)      20.12, rounds to nearest
newHP  = max(0, hp - (scaled >> 12))           hp is u16 at target+0x1A0
```

`target+0x174` is a damage-taken multiplier initialised to 1.0, so an unbuffed
unit takes exactly the roll (`docs/59`).

**`ProjectileBlueprint+0x6B` is "explodes"** — set on exactly the 11 splash
weapons (boulders, fireballs, cannonballs, plasma, the Gift) and clear on all 16
arrows, bolts, shots and lasers. The flag's meaning was read off a `cmp/beq`
*before* the data was looked at, so the 11-vs-16 split was a prediction the data
could have falsified.

The **second damage pair** at `+0x70`/`+0x72` is the *impact* damage where
`+0x6C`/`+0x6E` is the *direct hit*. All 27 records carry them equal, which is
why `docs/20` could not tell them apart — the difference is which code path
reads which, not the values.

Splash is swept in three rounds with a distance falloff and clamped to 65,535.

## Vision

**Two bitmaps, one bit per tile**: `+0x14` is what is visible now and is cleared
every update; `+0x10` is what has ever been explored and is only ever written
(`docs/63`). The explored stamp is **one-shot** — a unit standing still does not
re-stamp permanent territory every frame.

The reveal is a **capsule, not a circle**: `func_020A16B4` is a midpoint circle
that also takes the source's footprint spans and adds them to the centre, so a
2×2 building reveals a rounded square rather than being treated as a point.

A multi-tile building is visible when **any** of its four corners is
(`docs/64`).

**The radius is a mission-script literal**, not a unit stat: `func_02048A94`
takes it as an argument and all 53 call sites are in the overlays, passing 1 to
15 tiles with 2 and 5 dominating (`docs/69`).

**`RedBrick_TurnOffFow` does not clear the fog map — it ORs into the query.**
That is what keeps it lockstep-safe; a cheat that wrote to the map would desync.

## The cheats

The red-brick table (`docs/28`) has been this project's best index into the
code: five subsystems found through it in five passes — the unit cap, the
harvest payout, the damage applier, the construction tick, and the fog query.
**A cheat that modifies a system names that system's code.**

```
Long Range Magic    spell range    x1.5   via x + (x >> 1)
Super Magic         spell duration x1.5   the same idiom
Double LEGO Bricks  tree payout    x2
Fast Mining         mine payout    x2
Fast Harvesting     interval       150 ticks -> 1
Fast Building       build time     -> 1 tick, local player only
One Hit Kill        resulting health := 0, attacker must be a Hero, local player
Invincible Hero     resulting health := current, target must be a Hero, local player
```

**`x + (x >> 1)` is this codebase's ×1.5** and appears in three subsystems.
"Super Magic" does *not* double — a port assuming ×2 would be a third off on
every buff duration.

The two hero cheats test the player id; **the two spell cheats do not**, so the
AI gets them too.

Two rules `docs/28` could only list as "other" are named by what the
construction tick does with them: **`0x13` forces build time and cost to 1** (a
developer switch, hence no `RedBrick_` name) and **`0x14` halves build time**.

## What is still open

A reference that quietly omits what it does not know is worse than one that says
so. Carried in the table as `unresolved` rows:

| question | flagged in |
| --- | --- |
| where the spell record lives — no spells file in `BP/` | `docs/64` |
| `UnitBlueprint+0x70` and `+0x71` | `docs/20` |

Both `docs/64` dead ends have a stated restart: the vision radius from the
source **constructor** rather than field writes, and the spell record from the
table's **static address** rather than a data file.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | 53.7% | 2,501 listed + 2,497 derived, 39 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured in `docs/62` |
