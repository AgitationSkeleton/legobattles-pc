# Building and training

**Status: the build lifecycle is read end to end — what it costs, how long it
takes, what grows while it builds, and the six conditions that must hold before
a unit can be trained. Two game rules `docs/28` could only list as "other" are
named, and `docs/58`'s flagged field-overload is resolved.** Three new checks;
**81 of 81 pass**.

`docs/58` covered what a building does once it exists and `docs/59` covered how
it dies. This is the middle.

The way in was the same as both: `docs/28`'s red-brick table lists
`RedBrick_FastProduction` — "Fast Building" — with
`Battles::Sim::ConstructProgressAction::virtual_02` among its readers. That
function is the construction tick.

## Training a unit: six conditions, in order

`func_02071DC4`, `ProduceUnitEntityCommand`'s precondition check, in the order
the code evaluates them:

1. **The producing structure still exists.** Otherwise result code 5.
2. **`structure+0x22E == 100`.** A building under construction produces nothing
   — the same rule `docs/58` found in the counting function, here enforced at a
   second, independent site.
3. **Its owner still exists.** Result code 5 again.
4. **Allowance.** If the unit's category is 5 or 6 → the Specials allowance;
   otherwise the Minifigure allowance; and **category 0 (Hero) is exempt from
   the check entirely** — the Hero limit of one is enforced elsewhere
   (`docs/58`'s table entry 0).
5. **A per-blueprint availability count** must be at least 1.
6. **Cost.** `blueprint+0x5E` against `player+0x90` — the brick balance
   `docs/58` found capped at 500,000.

```asm
ldrb r0, [rules, #0x13]
cmp  r0, #0
ldreq r0, [r5, #0x20]        ; not cheating: read the real cost
movne r1, #1                 ; cheating: cost is 1
ldrheq r1, [r0, #0x5e]
ldr  r0, [r4, #0x90]         ; the purse
cmp  r0, r1
movlo r0, #0                 ; cannot afford
```

**That closes the economy loop.** `docs/58` found where bricks come from and
where they are stored; this is where they are spent, and the comparison is a
plain unsigned `cmp` against the blueprint's cost field.

## Construction

`func_020523A8` runs once per simulation tick per site.

```asm
ldr  r0, [r4, #0x1c]         ; the entity being built
ldr  r0, [r0, #8]            ; its blueprint
ldrh r6, [r0, #0x60]         ; build time, in ticks
cmp  r6, #1
movlo r6, #1                 ; floored at one tick
```

At 30 Hz (`docs/25`), a Castle's 900 ticks is **30 seconds** and a Farm's 360 is
**12**.

**Health grows as it builds**, and is clamped to the blueprint's maximum:

```asm
ldrh r0, [r5+0x100, #0xa0]   ; current health
add  r0, r0, r7, lsr #16     ; this tick's increment
ldrh r2, [blueprint, #0x62]  ; max health
cmp  r2, r0, lsr #16
movlo r1, r2                 ; clamp
bl   SetHealth
```

So a half-built Barracks really is half as tough, and finishing it is what makes
it durable — not a flag flip at the end.

Two more details worth having:

* **Progress above 98 fires a one-shot event** (`cmp r5, #0x62`, guarded by a
  byte at `+0x35` so it fires once). That is the "nearly finished" hook.
* **On completion, if the finished entity is category `0x11` — Bridge — an
  extra virtual call is made** that nothing else gets. Bridges do something on
  completion that other buildings do not, which fits `docs/58`'s tooltip:
  "Bridges can only be placed at specific locations on the map."

## Two rules named from the code that reads them

`docs/28` recovered 155 rule flags and could name the 18 red bricks. Two of the
"other" rows are named here, by what the construction tick does with them:

| flag | `docs/28` said | what it does |
| --- | --- | --- |
| `0x13` | rule 78, "other" | **build time forced to 1 tick, and cost forced to 1** |
| `0x14` | rule 101, "other" | **build time halved** (`lsr #1`) |

`0x13` appears again in `func_02071DC4` as the free-cost branch above, so it is
one rule meaning "building is instant and free" — a developer switch rather
than a player-facing brick, which is why it has no `RedBrick_` blueprint name.
`RedBrick_FastProduction`, the player-facing one, forces the time to 1 **for the
local player only**.

## `docs/58`'s flagged overload, resolved

`docs/58` recorded an unresolved discrepancy:

> `func_02000C6C   ldr r0,[r0,#8] ; ldrh r0,[r0,#0x6c]     halfword`
> `func_0205CC14   ldrb r0,[r0,#0x6c] ...                   byte`
> What `func_02000C6C` is doing is **not resolved** — it reads two adjacent
> fields as one halfword, which is either a different meaning for that pair or
> a latent bug in the game.

**Neither. The two functions read different record types.** `Entities.ebp`
holds three (`docs/11`):

```
UnitBlueprint            0x7C   +0x6C is resource_yield, a u8
CollectableItemBlueprint 0x70   +0x6C is the unlock slot,  a u16 (docs/22)
```

`docs/22` established the unlock slot as a 289-entry bitfield index, 0..288 —
a range that needs sixteen bits. And `func_02000C6C` sits immediately after
`func_02000C14`, which `docs/11` identified as **the
`CollectableItemBlueprint` constructor**: same translation unit, same record
type. The halfword read is correct for the record it is given.

The check confirms the split from the data side: **289 collectables carry an
unlock slot and 0 unit blueprints do.**

The general rule, which is worth more than the specific answer: **the same
offset in two record types is two fields.** A field-width disagreement between
two readers is a signal to ask which *type* each reader was handed, before
concluding either is wrong.

### A bug I nearly reported

Working through this I first read `lb_blueprints.py`'s
`unlock_slot: u16(0x6C) if u8(0x5C) in UNLOCKABLE`, computed it against
`UnitBlueprint` records, got 7,680 and 10,752 where `docs/22` says 0..288, and
was ready to call the tool wrong. It is not — that line is in the
`CollectableItemBlueprint` branch and `UNLOCKABLE` is an *item* category set. I
had applied it to the wrong record type, exactly the mistake the paragraph above
warns about.

**Check which branch a line is in before reporting it as a bug.** It cost five
minutes here; publishing it would have cost more.

## Tips and pitfalls

* **Preconditions are documentation.** A "can I do this?" function lists a
  system's rules in evaluation order, more completely than any single feature
  would.
* **Health during construction is not a flag.** Anything reimplementing this
  needs the per-tick increment and the clamp, or half-built structures will be
  wrong in a way that only shows up under fire.
* **An unnamed rule flag is still readable.** `docs/28` could not name rules 78
  and 101 because they have no blueprint entry; the code that reads them names
  them anyway.
* **The red-brick table has now named four subsystems** — the unit cap, the
  harvest payout, the damage applier, the construction tick. Four passes, four
  entry points, no searching.

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
