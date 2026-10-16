# The game rules: 30 toggles, and what each one actually changes

**Status: solved. The rule set, the id→flag mapping, and the consuming code are
all recovered from the ROM by `tools/lb_gamerules.py`. All 18 red bricks are
matched to the simulation code that implements them.**

`docs/27` recovered the eighteen red bricks and their display names but left
open which flag each one sets, explicitly refusing to guess. This closes it —
and the answer arrives with a check strong enough that no guess was needed.

## The dispatcher

`func_02007F3C` is a Thumb jump-table switch on a rule id:

```arm
ldrb r0, [r1]            ; the rule id
cmp  r0, #0x9a           ; 154
bls  +2
b    <default>
adds r0, r0, r0          ; id * 2
add  r0, pc
ldrh r0, [r0, #6]
lsls r0, r0, #0x10
asrs r0, r0, #0x10       ; sign-extend
add  pc, r0
```

155 ids, of which **30 are handled and 125 fall through to the default**.

### Two pitfalls, both of which produce plausible garbage

**The arms look like nonsense code.** Disassembling the function linearly gives
a long run of `lsls r0, r4, #0xe` and similar — that is the *halfword jump
table* being decoded as Thumb instructions. It is 310 bytes of data sitting in
the middle of a function, and nothing marks it.

**`add r0, pc` is not `adr`.** For ADD (register) with PC as the operand, PC
reads as *instruction address + 4*, **unaligned**. For ADR and LDR-literal it
would be `Align(PC, 4)`. Same register, two different rules, and picking the
wrong one shifts every computed target by two bytes — into the middle of real
instructions, which then disassemble into something that looks almost
reasonable.

The self-check that catches both: the table is 155 halfwords starting at
`0x02007F54`, so it must end at `0x02008089` — and the lowest arm target is
`0x0200808A`, the very next byte. Table and code are contiguous with no gap.
Any error in the PC arithmetic breaks that adjacency.

## The arms, and the toggle block

Every handled arm has one shape:

```arm
r0 = GetSettings()          ; func_020092A8
if (r0[flag] != 0) {
    GetSettings()
    toggle_flag()
}
```

and each `toggle_flag` is a 36-byte function in a **contiguous generated block
at `0x020092D4`**, one per flag, that does nothing but invert its byte:

```arm
ldrb  r1, [r0, #2]
cmp   r1, #0
moveq r1, #1
movne r1, #0
strb  r1, [r0, #2]
```

So the settings object is **one byte per rule**, and this dispatcher is the
path that turns a rule on or off — the Extras menu, not the simulation.

## How the red bricks were identified without guessing

Of the 30 handled ids, **0…17 are contiguous**, and `docs/27` found exactly 18
red bricks. That is suggestive and *nothing more* — 18 is a small number and
coincidences of count are cheap. `docs/27` declined to act on it, and was right
to.

The evidence that settles it is a different function entirely. Collecting, for
every flag, the set of functions that read it, exactly one function reads
**all 18 of the flags that ids 0…17 map to, and none of the other 12**:

```
functions reading EXACTLY the 18 red brick flags and no others: func_020d28ec
```

That is the red brick menu. A function that partitions the flag space precisely
along the red-brick boundary — discovered from the reader side, with no
reference to the id ordering that suggested the boundary — is independent
confirmation. `tools/lb_gamerules.py` recomputes this partition on every run
rather than asserting it.

## The eighteen red bricks, and the code each one changes

| id | flag | red brick | display name | what reads the flag |
| ---: | ---: | --- | --- | --- |
| 0 | `+0x00` | `TurnOffFow` | Reveal Map | 11 functions incl. minimap and view code |
| 1 | `+0x01` | `OneHitKill` | One Hit Kill (Heroes) | `func_0205079C`, `func_0206E970` |
| 2 | `+0x02` | `FastProduction` | Fast Building | **`ConstructProgressAction`, `RepairStructureAction`** |
| 3 | `+0x03` | `QuickMagic` | Fast Magic | `func_0205F124` |
| 4 | `+0x04` | `DoubleStuds` | Double LEGO Bricks | `func_0206D0E0` |
| 5 | `+0x05` | `RevealMinikit` | Show Minikit | **`func_020A6138`** |
| 6 | `+0x06` | `RevealCoins` | Show LEGO Studs | **`func_020A6138`** |
| 7 | `+0x07` | `Regeneration` | Regenerating Health | `func_0205F124` |
| 8 | `+0x08` | `IncreaseSpellRange` | Long Range Magic | `func_02002AD4` |
| 9 | `+0x09` | `IncreaseSpellTime` | Super Magic | `func_02002AA8` |
| 10 | `+0x0A` | `RevealRedBrick` | Show Red Bricks | **`func_020A6138`** |
| 11 | `+0x10` | `AutoUpgrade` | Upgraded Towers | **`ConstructStructureEntityCommand::virtual_09`** |
| 12 | `+0x11` | `SuperExplosions` | Super Explosions | `func_0206E60C` |
| 13 | `+0x0B` | `FastHarvest` | Fast Harvesting | `func_020531F8` |
| 14 | `+0x0C` | `FullAllowance` | Full Unit Cap | **`User::WorldViewTopScreen::virtual_09`** + 3 HUD functions |
| 15 | `+0x0D` | `InvincibleHero` | Invincible Hero | 5 functions in the damage region |
| 16 | `+0x0E` | `FastMining` | Fast Mining | `func_0206D7F0` |
| 17 | `+0x0F` | `EnemiesOnMinimap` | Show Enemies | `func_02025C14` |

Note that the id order is *not* the flag order — ids 11 and 12 map to `+0x10`
and `+0x11` while ids 13–17 map to `+0x0B`–`+0x0F`. A mapping assumed to be the
identity would have gone wrong on five of eighteen, and the four strongest
confirmations below are all in that shuffled region.

### The confirmations, which are the point

Every name lands on code that matches it, and several of the matches are of a
kind that cannot happen by accident:

- **Three "reveal" bricks share one reader.** `RevealMinikit`, `RevealCoins`
  and `RevealRedBrick` all read through `func_020A6138` and nothing else.
  Three semantically identical cheats landing on one shared function is not
  something a wrong mapping produces.
- **The two regeneration bricks share one reader.** `QuickMagic` (mana) and
  `Regeneration` (health) both read `func_0205F124` — one per-tick unit update
  handling both pools.
- **The two spell bricks are adjacent functions.** `IncreaseSpellRange` reads
  `func_02002AD4`, `IncreaseSpellTime` reads `func_02002AA8` — 0x2C apart, in
  the spell-parameter region.
- **The two resource bricks are adjacent functions.** `DoubleStuds` reads
  `func_0206D0E0`, `FastMining` reads `func_0206D7F0`.
- **"Fast Building" reads construction *and* repair.** `+0x02` is read by
  `Sim::ConstructProgressAction::virtual_02` and
  `Sim::RepairStructureAction::virtual_02` — precisely the two actions
  `docs/25` showed share the build-time field.
- **"Upgraded Towers" is read at construction time**, by
  `ConstructStructureEntityCommand::virtual_09`, which is where a structure is
  placed.
- **"Full Unit Cap" is read by the HUD**, including
  `User::WorldViewTopScreen::virtual_09` — the top-screen readout where a unit
  count is displayed.
- **"Invincible Hero" is read by five functions in the damage region**, sharing
  `func_0206E970` with `OneHitKill`. Both are damage modifiers; both touch the
  same code.

Eighteen names, eighteen plausible sites, and five shared-reader coincidences
that a wrong mapping would have to reproduce simultaneously.

## Fast Building is not the build-time halving

`docs/25` found that `ConstructProgressAction` **halves build time** when a
settings byte at `+0x14` is set, and it was tempting after `docs/27` to read
that as the Fast Building red brick. **It is not.** Reading the function
through, it consults *three* different rules:

```arm
bl   func_020092A8
ldrb r0, [r0, #0x13]      ; rule 78 -- checked FIRST, and skips ahead
cmp  r0, #0
bne  <skip the next test>
bl   func_020092A8
ldrb r0, [r0, #2]         ; rule 2 -- Fast Building
cmp  r0, #0
beq  <skip>
ldrb r1, [r6, #0xe4]      ; a team/owner id on the entity
ldrb r0, [r0]
cmp  r1, r0               ; ...compared against one in the settings
beq  <skip>
ldrh r6, [r0, #0x60]      ; the build time
...
bl   func_020092A8
ldrb r0, [r0, #0x14]      ; rule 101 -- a DIFFERENT rule
cmp  r0, #0
beq  <skip>
ldrh r0, [r0, #0x60]
lsr  r6, r0, #1           ; the halving
```

So the halving belongs to **rule 101**, which is not a red brick. Fast Building
acts through `+0x02` and is **gated on an owner comparison** — a byte at
`Entity+0xE4` against one held in the settings object. That is what a cheat that
must benefit the player and not the AI looks like, and it is a behaviour worth
knowing for a port: **red bricks are not global modifiers; at least this one is
filtered by owner.**

`docs/25`'s statement was correct as written — a game-rule flag halves build
time — and only the later temptation to attribute it to a red brick was wrong.
That attribution was never published; this note records it so nobody makes it
later.

## The other twelve rules — eleven of them now named

### Rule 78: everything costs 1

Fourteen functions read `+0x13`, spanning construction, repair, research and
production, and every one has the same shape:

```arm
bl   func_020092A8
ldrb r0, [r0, #0x13]
cmp  r0, #0
ldreq  r0, [r5, #0x20]    ; the queued entity
movne  r1, #1             ; <- flag set: cost is literally 1
ldreq  r0, [r0, #8]       ; its blueprint
ldrheq r1, [r0, #0x5e]    ; <- flag clear: cost is the blueprint's
bl   func_02086710        ; charge it
```

`+0x5E` is the cost field from `docs/11`. So rule 78 replaces every price in
the game with **1** — not zero, one. It is read at every site that charges for
anything, which is exactly why it has fourteen readers.

### Rules 80–92 and 99: the debug overlay names itself

`func_020F6730` is the **main loop** — called only from `main`, opening with
some fifty unconditional subsystem-init calls and then writing the two engines'
brightness registers at `0x0400006C`/`0x0400106C`. Inside its per-frame body,
six rules each guard a 64-byte buffer being formatted and pushed to a text
list, and the format strings name the rules outright:

| id | flag | the text it enables |
| ---: | ---: | --- |
| 80 | `+0x16` | `"FPS:%d"` |
| 81 | `+0x17` | `"Free Memory:%ld"` |
| 82 | `+0x18` | `"Build:%d"` |
| 91 | `+0x19` | — clears two buffers at `0x0215CD68`/`0x0215CDA8`; text written elsewhere |
| 92 | `+0x1A` | `"UserID 0 queued CMDs:%d"`, `"UserID 1 queued CMDs:%d"`, `"Balance count:%d"` |
| 99 | `+0x1C` | `"Spell: %s (%d)"`, `"Spell: On"` |

**A shipped debug overlay, still reachable.** Rule 92's readout is the most
interesting for this project: per-user queued command counts and a "balance
count" are precisely the instrumentation a **deterministic lockstep**
simulation needs — `docs/07` found `SyncCheckCommand` and `SyncStatusCommand`
in the command set, and `docs/25` explained why object identity must be an
index. This is the diagnostic that was used to debug exactly that, and a port
reimplementing the lockstep should keep it.

Rule 91 is left unnamed: it clears its buffers in the main loop but the text is
produced somewhere this pass did not find.

### Red bricks are player-gated, and each has an ungated twin

Three of the remaining rules turn out to be the *same effects as red bricks,
without the owner restriction*. The shape is unmistakable — two predicate
functions, one for harvesting and one for the unit cap, written identically:

```arm
bl   func_020092A8
ldrb r0, [r0, #0x12]      ; the ungated rule
cmp  r0, #0
movne r0, #1
popne {r4, pc}            ; -> true for ANYONE

bl   func_020092A8
ldrb r0, [r0, #0xb]       ; the red brick
cmp  r0, #0
beq  <false>
blx  func_020A186C        ; the local player
blx  func_020A2230
ldrb r1, [r4, #0xe4]      ; this entity's owner
ldrb r0, [r0]
cmp  r1, r0
moveq r0, #1              ; -> true only if it is MINE
```

| brick | ungated twin |
| --- | --- |
| 13 `FastHarvest` `+0x0B` | **97** `+0x12` |
| 14 `FullAllowance` `+0x0C` | **98** `+0x15` |
| 2 `FastProduction` `+0x02` | **101** `+0x14` (the build-time halving) |
| 0 `TurnOffFow` `+0x00` | **104** `+0x1E` (shares the reveal path in `func_02025C14`) |

This generalises the owner check `docs/25` and the earlier draft of this
document noticed in `ConstructProgressAction`. **A red brick is not a global
modifier: it is a predicate that returns true only for the local player's
entities**, and the game keeps a separate, ungated rule for each one it wanted
available in development or in scripted missions.

For a port this is a correctness requirement, not a detail. Applying a red
brick globally gives every AI opponent fast harvesting, an unlimited unit cap
and half-price building.

### Rule 103 has no reader

`+0x1D` is toggleable and nothing reads it. Recorded as a negative result
rather than smoothed over — with the caveat that the reader scan matches a load
within five instructions of the settings getter, so this means "no reader
through the direct idiom", not "provably unused".

## The settings object is larger than the menu

The dispatcher exposes 30 flags. The reader scan finds three more that code
reads and the dispatcher cannot toggle:

```
flags read by code but NOT toggleable through the dispatcher: +0x1B, +0x1F, +0x20
```

`+0x1F` is read in `func_02025C14` alongside the fog rules, `+0x20` in
`func_0203B3AC`, `+0x1B` in `func_0208B01C`. So **"30 rules" is the size of the
toggle menu, not the size of the settings object** — at least 33 bytes are live.
`tools/lb_gamerules.py` prints this set on every run so the gap cannot quietly
close or widen unnoticed.

## What this gives a port

`docs/data/game_rules.tsv` carries all 30 rules with flag offset, toggle
function, debug text and reader list. A port needs:

- the settings object as **one byte per rule at these exact offsets**, because
  thirty-odd call sites index it directly and three more bytes are live beyond
  the menu;
- **the owner gate on every red brick**, or the AI gets the cheats too;
- rule 78's cost substitution to be **1, not 0** — a zero would change whether
  affordability checks pass;
- ideally the rule-92 lockstep overlay, which is the only shipped instrument
  for the desync class of bug a port is most likely to introduce.
