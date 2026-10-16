# Number-key hotkeys for build and spell slots

**Status: researched, not implemented. The UI classes are identified, the list
is confirmed to be built at runtime, and the one design decision that matters is
settled — the hotkey must read the game's own list rather than any table.**

## The feature

Keys `1`–`0` (with `0` meaning ten) acting on the current selection:

* a **builder** selected -> build the nth structure its red-brick sidebar offers;
* a **building** selected -> produce the nth unit it offers (barracks: 1 foot,
  2 ranged, 3 mounted);
* a **hero** selected -> use the nth spell from the yellow sidebar.

## The lists are dynamic, and that is the whole design

This is the constraint that decides everything else. The offered set is **not
fixed per faction**:

* **Campaign missions restrict it.** What a builder or building may make varies
  per mission.
* **Free play restricts it by army composition.** No shipyard is offered if none
  of the chosen specials is a water craft.
* **Mods change it.** Heroes gain or lose spells, and a hero may have more or
  fewer than four — Kahuka reportedly has five in one of the balance patches.

So a table mapping "1 = HQ, 2 = farm" is wrong the moment it meets a campaign
mission or a romhack. **The hotkey must index whatever the sidebar is currently
showing**, which means reading the same list the UI reads.

That is also why this is the *right* shape for a port feature: it adds no
knowledge of its own and cannot disagree with the game.

## The classes

`Battles::User::` carries the whole in-game UI, and the relevant ones are:

| class | role |
| --- | --- |
| **`BuildTab`** | the sidebar tab itself; 13 vtable slots |
| **`BuildCommandItem`** | one entry in that sidebar |
| `RequestItem`, `TargetItem`, `TransportRequestItem` | queued-request entries |
| `DesiredSpellCategory` | the spell side of the same idea |
| `SelectionChangedEvent` | fires when the selection changes |
| `WorldViewScreen` | the in-game screen that owns them |

`BuildTab`'s substantial overrides:

```
slot 0  func_020D0348  180 B
slot 1  func_020D03FC  188 B
slot 4  func_020E159C  252 B
slot 7  func_020E16DC  420 B
slot 8  func_020D0764  572 B
slot 9  func_020D0B14  464 B
```

## The list really is built, not looked up

`BuildTab` slot 9 (`func_020D0B14`) calls **`func_020D1744`**, 1,912 bytes and
876 instructions, and that function:

* reads the entity **category** at `[[e+0x160]+8]+0x5C` three times;
* touches `+0xFC` four times;
* calls **`func_020ECDD4`** four times — the *same* two-byte container push that
  `docs/95` found behind the buff list and the transport occupant list.

So the offered set is assembled into a container of two-byte elements each time,
by the same primitive the rest of the game uses for per-entity lists. **A hotkey
can therefore read the built list rather than deriving one.**

## What implementing it would take

1. **Find the selection.** `SelectionChangedEvent` names the concept; the field
   holding the current selection has not been located.
2. **Find the built list's container.** `func_020D1744` writes one; which
   object owns it, and its offset, is not read yet. Given the pattern, expect
   `count` at `+8` and `data` at `+4` of that container.
3. **Reuse the click path.** The sidebar already turns "entry n" into a command —
   `ProduceEntityCommand` (id 25), `ConstructStructureCommand` (20),
   `ConstructMultipleCommand` (19) or `CastSpellCommand` (16), per `docs/88`.
   A hotkey should call whatever the click calls, not construct a command
   itself, so ordering, cost checks and queue limits stay the game's.
4. **Map key to index.** `1`-`9` -> 0-8, `0` -> 9, and do nothing when the list
   is shorter, which is the Kahuka-with-five case handled for free.

## Multiplayer: this one is safe

Unlike every enhancement toggle in `docs/86`, a hotkey is **client-local**. It
issues the same command the same click would, through the same command layer
that already synchronises. There is nothing to make lobby-carried and no desync
risk, provided step 3 is honoured and the hotkey never bypasses the command
path to poke the simulation directly.

## Honest scope

The trail is solid and the design constraint is settled, but three concrete
unknowns remain: where the current selection lives, which container holds the
built list, and which function the sidebar calls on click. None looks hard —
they are all in `BuildTab`'s six substantial slots — but none is read yet, and
the feature cannot be written until they are.

## Correction: there are THREE sidebars, not two

The note above treated the sidebar as one thing with two flavours. It is three,
and the third changes the design.

* **Blue brick** — on **every** unit and building: move, attack, patrol, stop,
  self-destruct and so on.
* **Red brick** — builders and factories: what to build or produce.
* **Yellow brick** — heroes: spells.

A selected builder therefore has **both** a blue sidebar and a red one, so
"press 1" is ambiguous until the port says which sidebar the numbers address.

## The tabs are siblings, and `InfoTab` is the blue one

`InfoTab` has the same 13-slot shape as `BuildTab`, overriding the same
positions:

```
              slot 0   slot 1   slot 4   slot 7   slot 8   slot 9
BuildTab      020D0348 020D03FC 020E159C 020E16DC 020D0764 020D0B14
InfoTab       020D2D60 020D2DFC 020E159C 020E16DC 020D43E4 020D48CC
UnitsTab               —        020E159C 020E16DC 020D8568 020E1BF4
```

Slots 4 and 7 are **identical addresses** across all three, so they are the
shared base's; each tab overrides **8 and 9** for its own content. Whatever
`BuildTab`'s slot 9 does to populate the build list, `InfoTab`'s slot 9
(`func_020D48CC`, 576 bytes) does for the action list.

That is convenient: one mechanism reads both, and a hotkey layer written against
the tab base works for either sidebar.

## The 26 command ids partition by sidebar

`docs/88`'s enumeration splits along exactly these lines, which corroborates the
grouping from the other direction:

```
BLUE   14 AssistStructure  15 Attack      17 CombatHoldPosition  18 CombatStandGround
       21 Garrison         22 HarvestResource  23 MoveEntities   24 Patrol
       27 RallyPoint       28 RepairStructure  30 Retire         31 Stop
       32 Ungarrison
RED    19 ConstructMultiple  20 ConstructStructure  25 ProduceEntity
       26 CancelProduceQueueItem  29 ResearchUpgrade
YELLOW 16 CastSpell
```

Thirteen blue, five red, one yellow. `RetireCommand` (30) is the self-destruct.

## The design question this raises

Numbers can only mean one sidebar at a time. Three options, none of them
obviously right:

1. **Numbers are always build/spell** (red or yellow, whichever the selection
   has), matching the specification above. Blue actions would need their own keys
   — letters, as most RTS games do.
2. **Numbers follow the open tab.** Consistent, but the same key does different
   things depending on UI state, which is the thing hotkeys exist to avoid.
3. **Two rows.** Numbers for build/spell, a modifier or a second row for blue
   actions.

This needs deciding before implementation, not during. The research does not
settle it because it is a preference rather than a fact about the ROM.

## HARD RULE: ownership and visibility, both, before anything is issued

A hotkey that skips a check the UI performs is not a convenience, it is an
**exploit**. The obvious failure is ordering an enemy unit around: select an
opposing unit — which the game permits, so a player can inspect it — press `1`,
and if the hotkey issues a command without asking whose unit it is, the port has
invented a cheat that the retail game does not have and that no opponent can
see coming.

Two gates, and **both** are required:

**1. The sidebar must actually be visible.** Not "a selection exists" — the red
or yellow sidebar must be on screen and populated. That is the state the click
path requires, so it is the state a keypress substituting for a click must
require. If the sidebar is closed, collapsed, or showing a different tab, the
key does nothing.

**2. The selected entity must be the local player's.** Checked explicitly,
against `Entity+0xE4`, the owner byte `docs/28` established and `docs/95` uses.
It is not enough to assume the sidebar would be empty for an enemy: the game
lets a player select an enemy unit to look at it, so an `InfoTab` may well be
populated for something they do not own. **Assuming the UI already filtered is
exactly the assumption that produces the exploit.**

The local player's own id is the remaining unknown. `func_02087634` returns a
pointer whose first byte is an owner id, and `docs/95` found the heal tick
comparing an entity's `+0xE4` against it — which makes it the obvious candidate
for "the viewing player", but that is inference from one call site and has not
been confirmed.

## Why this matters more in this port than in a normal RTS

`docs/104` above notes that a hotkey is client-local and therefore desync-safe.
That remains true **only while both gates hold**. A command is synchronised by
the command layer, so a forged command is not a desync — it is a *legal*
instruction that every client will faithfully execute. The lockstep would agree
perfectly about an enemy unit walking off a cliff.

So the safety here is not the netcode's to provide. It has to be in the gate,
and the gate has to be written before the feature, not after someone notices.

## Scope settled: red and yellow only

The blue action sidebar is **out of scope**. Numbers address the build/produce
sidebar for builders and factories, and the spell sidebar for heroes. That
resolves the ambiguity above without needing a modifier or a second row.

## The tab's item list, partially read

Both shared base slots — `func_020E159C` (slot 4) and `func_020E16DC` (slot 7) —
walk the same structure, which is the strongest lead so far:

```asm
ldr  r2, [r6, #0x1c]        ; the tab's ITEM LIST lives at tab+0x1C
ldrb r0, [r2, #8]           ; a COUNT
cmp  r0, #0 ; ble done      ; nothing to do when empty
...
adds r0, r2, r_i            ; list + index
ldrb r0, [r0, #5]           ; a per-item BYTE id
lsls r2, r0, #1 ; adds r2, r0, r2   ; id * 3
ldr  r3, [<global>]         ; a table base
adds r0, r3, r2             ; table + id*3
ldrb r5, [r0, <off>]        ; a validity byte
...
lsls r0, r0, #2 ; adds r0, r3, r0 ; ldr r1, [r0, #4]   ; -> an object
ldrb r0, [r1, #8]           ; ... which has its own count at +8
ldr  r0, [r1, #0x54]        ; ... and something at +0x54
```

So: **the tab keeps its items at `tab+0x1C`, with a count at `+8` of that
object, and each item carries a byte id used to index a global table at three
bytes per entry.** Slot 7 ends by calling `func_0202E624` — 9,112 bytes — with
that byte id, which is the natural candidate for "act on this item".

Both slots run the *same* validity test on each item before using it, which is
the availability filter the feature needs: the shipyard that is not offered,
the mission-restricted building, the spell a hero does not have.

### The one thing that does not add up yet

Items are read at `list + i + 5` while the count sits at `list + 8`. Taken
literally those collide at `i == 3`, which would cap any list at three entries —
implausible for a build menu and flatly contradicted by a hero with five spells.

So one of three things is true and **none has been established**: `r2` is not the
list base at the point of the count read, the element stride is not one byte, or
the count is not at `+8` of the same object. **This is not a detail to guess at**
— an off-by-one in the stride would make key `3` build the wrong thing, silently
and only sometimes.

## Where this leaves the feature

Known: which classes, which sidebar, which commands, that the list is built at
runtime, that both slots filter for availability, and that the list hangs off
`tab+0x1C`.

Not known: the true element layout, where the current selection is held, and
which of `func_0202E624`'s paths is the click. Plus the ownership check from the
section above, which is a requirement rather than a discovery.

The next read is `func_0202E624`'s prologue and the global table it indexes,
which should settle the stride question immediately.

---

# Correction: the stride question was malformed

The section above worries that items at `list + i + 5` collide with a count at
`list + 8`, capping any list at three. **That reading came from the wrong
function and the worry is void.**

`func_020E159C` is `Battles::User::ScreenType::virtual_04` — a *base-class*
method inherited by `Options`, `QuickDialog`, `WiFiScreen`,
`AchievementsTopScreen`, `CampaignBriefing` and `CampaignFactionSelect`. It is
not the build sidebar. It walks a small inline `u8 ids[3]` at `+5` with the
count at `+8`, tests each id against a 3-byte-record table at `[0x02151DD8] +
id*3 + 0x8FC`, and returns a boolean. It is a **screen precondition check** —
"are this screen's requirements met" — and the three-item cap is simply the
array's real size, not a contradiction.

The fact that **every vanilla hero has exactly four spells** is what
falsified it: a three-element array cannot hold four. That was the decisive
test, and it cost nothing to apply.

## The real list, from func_020D1744 (BuildTab)

```c
Entity* e = func_0208727C(&g_selected_handle);   /* 0x02157599 */
if (!e) return;
if (!func_02085D58(local_player, e + 0xE4)) return;   /* OWNERSHIP GATE */

Container* c = e + 0x1E4;
int   count = *(int*)(c + 8);       /* 32-bit word, ldr */
u8*   data  = *(u8**)(c + 4);
for (int i = 0, off = 0; i < count; i++, off += 2) {   /* STRIDE 2 */
    void* d = func_02087460(data + off);
    if (!d) continue;
    switch (*(u32*)(d + 8)) {
    case 0: /* buildings */ if (!func_020D04B8(this, &listA)) push2(&listA, *(u16*)(data+off)); break;
    case 1: /* units     */ push2(&listB, *(u16*)(data+off)); break;
    }
}
```

So: **elements are 2 bytes, the count is a 32-bit word at `+8`, the data pointer
is at `+4`.** No collision, no cap — four spells fit, and so does Kahuka's five.

Two further facts, both directly useful:

* **The game already gates on ownership**, at `0x020D1774`, before building the
  list at all: `func_02085D58(local_player, entity+0xE4)` and bail if false.
  This is the same `+0xE4` the security note demands. A hotkey that reads the
  tab's list therefore inherits the ownership check rather than reimplementing
  it — but the second gate still stands on its own (below).
* **`BuildTab+0x6C` is the container actually displayed**, with its count at
  `BuildTab+0x74`. The function rebuilds it only when the freshly-computed list
  differs. **This is the thing a number key should index**: slot *N* is element
  *N* of `BuildTab+0x6C`, which already reflects campaign restrictions, the
  free-play shipyard rule, and per-mod spell counts, because the game filtered
  it.

## Unresolved, and not to be guessed

`docs/95` states that `entity+0x1E4` is the **active buff list**, written by
`func_020ECDD4(e+0x1E4, pair)`. `func_020D1744` reads `entity+0x1E4` as the
**build/produce list**. Same offset, same 2-byte container primitive. **At most
one of these is right and it has not been determined which.**

Note also that `func_02087460` fails in the same shape as the handle resolver in
`docs/105` — it calls `func_0208750C` and nulls the caller's two bytes — so the
elements at `+0x1E4` are plausibly **handles**, not blueprint ids, which would
make `[d+8]` a kind tag on a pooled object rather than a blueprint category.

The next read is the set of writers to `+0x1E4`, which settles it directly. No
hotkey may be wired until it is: indexing the wrong container would make key 3
issue a command derived from a buff, silently.

## The security rule is unchanged

Both gates still required: (1) the red/yellow sidebar visible and populated, and
(2) the selected entity owned by the local player, checked against `Entity+0xE4`
— now known to be exactly what `func_02085D58` tests. Finding that the game
checks ownership is a reason to match it, not a reason to skip our own check.

---

# Correction 2: entity+0x1E4 is the buff list, and func_020D1744 is the status panel

The previous section read `entity+0x1E4` in `func_020D1744` as the build list and
recorded a conflict with `docs/95`, which calls it the active-buff list.
**`docs/95` is right and the build-list reading was wrong.** Settled by evidence,
not preference:

* `func_02087460` resolves each element through `func_0207D0D4`, which is a
  generation-checked handle lookup into the pool at `0x02155218`:
  `obj = pool->array[h[0]]`, bounds-checked against `pool->count`, then
  `obj[5] == h[1]`.
* That pool's other referents are `Battles::Sim::MagicInfluenceBase::virtual_00`
  and `virtual_01` and **`func_0207C290`, the `StatBufSpell` constructor**.

So `entity+0x1E4` holds handles into the **MagicInfluence pool**: active buffs.
`func_0205ED48` pushing there and setting `entity+0x226` is exactly consistent.

`func_020D1744` is therefore the **status panel**, not a build-list builder. Its
own code says so once read properly: the two `"%d/%d"` strings pair
`entity+0x1A0` with blueprint `+0x62` (health) and `entity+0x1A2` with blueprint
`+0x64` (mana). It draws **HP, mana and the active buff icons** for the selected
entity. `this+0x6C` / `this+0x74` is the displayed **buff icon** container.

Also retracted: the claim that the switch on `[desc+8]` is "0 = buildings,
1 = units". That label was invented, not read. It distinguishes two kinds of
magic influence, and which is which has not been determined.

What survives from the previous section: `func_0208727C` resolves the selected
entity from the handle at `0x02157599`, and `func_02085D58(local_player,
entity+0xE4)` at `0x020D1774` is the game's own ownership test. Both are real
and both are still what the hotkey work needs.

## Where the sidebar actually is

The tab family is six siblings — `BuildTab`, `InfoTab`, `MapTab`,
`ObjectivesTab`, `ScoreTab`, `UnitsTab` — sharing slots 4 (`func_020E159C`) and
7 (`func_020E16DC`) from `ScreenType`.

`tab+0x1C` is a **UI descriptor**, which reconciles the two shared slots that
looked contradictory:

```
+0x02  u8       widget_count          <- virtual_08 reads this
+0x05  u8[3]    precondition_ids      <- virtual_04 reads these
+0x08  u8       precondition_count    <- and this
+0x0C  Widget*  widgets[]             <- virtual_08 walks these
```

`BuildTab::virtual_08` (`func_020D0764`, 572 B) is the **button dispatcher**. It
takes each widget's element id — `u16` at `[[widget+0xC]+4]+2`, with `0xFFFF`
meaning none — subtracts `0x2C8`, bounds it with `cmp #0x5E`, and dispatches
through a 95-entry jump table. Element ids `0x2C8`..`0x326` are therefore the
sidebar's button ids, and this is the function a hotkey must ultimately reach.

**Still not established:** which element ids correspond to build/produce/spell
slots, and how a slot index maps to one. That is the next read, and no key is
wired until it is.

## Safety, restated with a case that matters

FactionWars leaves 21 of 25 heroes with an **empty** spell array (`docs/106`).
Any hotkey must treat "no such slot" as the normal case and do nothing: read the
live count, bounds-check the index, and return. Vanilla gives 4 spells,
Rebalance and Funlite give some heroes 5, FactionWars gives most heroes 0. A
fixed assumption of four would be wrong in all three mods.
