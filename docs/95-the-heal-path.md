# The heal path

> **RETRACTED IN PART (docs/105).** This page calls `func_020872DC`
> `HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
> wrong. `func_02087350` resolves a two-byte **entity handle** against the pool
> at `0x02155028`; `func_020872DC` adds a **liveness** test. They are the
> engine-wide handle resolver (80 and 264 callers), not hotwire code and not a
> tile lookup. The override the port built on them is removed. Read anything
> below that rests on those names with that correction in mind.


**Status: `Sim::HealSpell` is read end to end — tick, enter/exit callback and
effect attach. The *Fix Hotwired Healing* option now has a mechanism, and it
is a plain absence: nothing in the heal path ever checks the target's owner.**

## The object

Fields established from the three functions below:

```
+0x14   cleared alongside +0x51 when the aura source disappears
+0x18   the source tile (u8 x at +0x18, u8 y at +0x19)
+0x3c   candidate array, u16 entity ids
+0x40   candidate count
+0x50   u8 tick countdown; reloaded from func_020F2600(0xFA)
+0x51   u8 aura mode - the spell is anchored to an entity on tile +0x18
+0x54   category filter; 0x14 means "any"
+0x58   an effect-handle container (func_020EDC90 inserts into it)
```

## Slot 3 - the tick (`func_02078770`, 600 B)

Counts `+0x50` down; on zero it sweeps the candidate list:

```c
for (i = 0; i < this->count_40; ++i) {
    e = ResolveEntity(this->list_3c[i]);            /* func_0208727C */
    cat = e->def_160->[8]->cat_5c;
    if (cat >= 8 && cat <= 0x13) continue;          /* jump table, 20 arms */
    if (this->filter_54 != 0x14 && this->filter_54 != cat) continue;
    cur = e->hp_1A0;  max = e->def_160->[8]->maxhp_62;
    if (cur >= max) { func_02078B94(...); continue; }
    ...
    amount = this->aura_51 ? (this->filter_54 == 0 ? 0x1E : 0xA)
                           : (max >> 2);            /* (max<<14)>>16 */
    newhp  = min(cur + amount, max);
    func_0205E7F4(e, newhp);                        /* <-- the HP write */
}
```

Then, if `+0x51`, it re-checks the source: `EntityAtTile(this+0x18)`. If nothing
is there it clears `+0x51` and `+0x14`; otherwise it reloads the countdown.

**Three filters: category, category-filter, and HP-not-full. No owner check.**
The source re-check tests *presence on a tile*, not *whose* entity it is.

## Slot 5 - enter / exit (`func_020785D0`, 416 B)

`(this, kind, entity)`. `kind == 0` is enter: same category jump table, same
`hp < max` test, then apply. `kind == 1` is exit: `func_02078B94`. Both then
call `func_0207AAB4`.

**Also no owner check**, on either edge.

## `func_020789C8` is the effect, not the heal

460 bytes that search the entity's attached-effect list (`func_0205E218`) for
effect ids **`0x2F`** and **`0x32`**, create the missing one via
`func_02016AE4` / `func_02016A48`, bind it to the entity's position (`+0xDC`)
and register the handle into `this+0x58`. The HP change is not here — it is
`func_0205E7F4` in the tick.

**No owner check here either.**

## The one owner comparison, and what it guards

`Entity+0xE4` is the owner byte (`docs/28`). It appears **exactly once** in the
whole heal path, in the tick at `0x02078900`:

```asm
add r0, sb, #0x18
bl  HotWire_ResolveTarget      ; func_020872DC - the healer's OWN tile
ldrb r0, [r0, #0xe4]           ; the source entity's owner
strb r0, [sp]
bl  func_02087634
ldrb r0, [r0]
cmp r1, r0
beq skip                       ; same owner -> skip
bl  func_020875C8              ; -> if 0, skip
bl  func_020851F8
sub r1, r6, r1                 ; r1 = heal delta
blx func_020A7F80
```

It compares the **source's** owner against a global, and what it gates is
`func_020A7F80` taking the heal *delta* — a statistics or attribution call. The
heal itself (`func_0205E7F4`) runs on the line after, unconditionally.

**So the only owner test in the heal path guards bookkeeping, not healing.**

## Why the bug happens

A hotwired unit changes owner but does not move. It therefore:

* never leaves the aura's trigger region, so **no exit callback fires**;
* stays in the candidate array at `+0x3c`;
* still passes the tick's three filters, none of which is ownership.

So the now-enemy unit keeps being healed by the original owner's source, which
is exactly the hotwired-healing bug.

That the same tick also calls `HotWire_ResolveTarget` is a coincidence of shared
tile-resolution helpers, not a shared defect. `docs/93` floated that the two
bugs might be one; **they are not** — the hotwire glitch is about which
occupant a tile resolves to, and this is about a filter that was never written.

## What is NOT established

* **Where `+0x3c` / `+0x40` are populated.** The enter/exit callback is a
  per-entity edge notification; the array is maintained elsewhere, and that code
  has not been located. Until it is, it is unknown whether the list is built
  once or rebuilt per tick.
* **What the spell's own owner field is.** A fix must compare the target
  against the *caster's* team, and no field on the spell object has been shown
  to hold it. `func_02087634` returns something whose first byte is an owner,
  but what object that is has not been read.
* **Whether the vanilla aura is team-scoped at creation.** If the trigger region
  only ever admits friendly units, the missing per-tick check is precisely the
  bug; if it admits anyone, then enemies walking into a heal aura would also be
  healed in vanilla, which would be a separate observable.

The fix cannot be written responsibly until the second point is resolved; the
field is read from the ROM, never guessed.

## `SpellBase`'s layout, from its constructor

`func_0207A8B0` is `SpellBase::SpellBase` — found by locating the four literal
references to the `SpellBase` vtable at `0x0214AAB4`. It stores the whole
header. Note that inside the callee `push {r4, lr}` shifts the stack by 8, so
the callee's `[sp+8]` is the caller's `[sp+0]`:

```
+0x00  vtable
+0x04  byte  r1[0]          a byte pair passed by address
+0x05  byte  r1[1]
+0x08  u32   r2             HealSpell passes the constant 3
+0x0C  ptr   caller [sp+0]  the spell DESCRIPTOR
+0x10  byte  0
+0x14  u32   caller [sp+8]
+0x18  byte  r3[0]          the anchor tile x
+0x19  byte  r3[1]          the anchor tile y
+0x1C  u32   0
+0x20  u32   caller [sp+4]
+0x24  ..    func_020F2BD4(this+0x24, ...)
+0x2C  byte  func_020873C4()[0]      a second byte pair, from a global getter
+0x2D  byte  func_020873C4()[1]
+0x30  u32   0
+0x34  u32   0
+0x38  ..    func_020EC5A8(this+0x38, 1)   container init
```

`HealSpell::HealSpell` (`func_0207845C`) then adds its own:

```
+0x50 = 1                 the tick countdown, so the first tick fires at once
+0x51 = arg               the aura flag
+0x54 = arg               the category filter
+0x58 = func_020EDAAC(this+0x58)     the effect-handle container
```

## `+0x0C` is the spell descriptor, not an owner — tested and rejected

`HealSpell` slot 5 loads `this+0x0C` and reads its first byte, which looked like
the owner-byte pattern (`ldr ptr; ldrb byte`). **It is not.** Two sibling spells
use the same byte as a **jump-table index**:

```asm
; DamageSpell func_02076BBC
ldr  r0, [r7, #0xc]
ldrb r0, [r0]
sub  r0, r0, #0x14
cmp  r0, #8
addls pc, pc, r0, lsl #2        ; 9-way switch

; StatBufSpell func_0207C5D4
ldr  r0, [r5, #0xc]
ldrb r0, [r0]
cmp  r0, #0x18
addls pc, pc, r0, lsl #2        ; 25-way switch
```

So `+0x0C` points to the spell's descriptor and its first byte is the **spell
id**. `HealSpell` passes that id to `func_02089230` when spawning the effect.
The hypothesis was formed from one call site and killed by checking two others.

## So where is the caster's owner?

**Nowhere on the spell.** The constructor stores no owner, and none of the nine
`SpellBase` slots reads one.

The design appears to derive it instead: the aura is defined by *whoever stands
on tile `+0x18`*. The tick resolves that entity twice — once via
`HotWire_ResolveTarget` for the attribution branch and once via `EntityAtTile`
at the end, where the aura **terminates** if nothing is standing there:

```c
if (this->aura_51 && !EntityAtTile(this + 0x18)) {
    this->aura_51 = 0;
    this->field_14 = 0;          /* the aura ends */
}
```

That makes the anchor entity's `+0xE4` the natural owner for a per-tick check,
and it needs no new state because the tick already resolves it.

**One caveat worth stating before anyone writes the patch:** the attribution
branch resolves the anchor with `HotWire_ResolveTarget`, which is the very
resolver whose one-occupant-per-tile behaviour causes the *other* hotwire bug.
A fix that reads the owner through that path inherits the ambiguity. The
end-of-tick `EntityAtTile` is the safer source, but the two have not been shown
to agree, and that is worth confirming before relying on either.

## The team switch is located, and it is not the fix

After *Patch Hotwire Glitch* turned out to need a much narrower change than
assumed, this one was re-examined the same way. **The answer here is the
opposite: the narrow intercept does not exist, and the tick really is the
place.** Recorded so the question is not reopened a third time.

### `func_020598C4` is `Entity::SetOwner`

**Exactly two functions in the image write `entity+0xE4`**, the owner byte:
`func_02059528`, which is the entity constructor, and `func_020598C4`, which is
the team switch. So there is one general path and it is 304 bytes:

```c
void Entity_SetOwner(Entity* e, OwnerRef* owner) {
    func_020875D4(owner);
    if (e->owner_E4 == owner[0]) return;          /* already theirs */
    func_02086FE8(g, pack(e->tile_DC, e->tile_DD));   /* leave the old owner */
    changed = (e->owner_E4 != *func_02087634());
    e->owner_E4 = owner[0];                        /* THE SWITCH */
    func_02086EF4(g, pack(e->tile_DC, e->tile_DD));   /* join the new one */
    if (... && e->def_160->[8]->cat_5C == 0)       /* category 0 = Hero */
        func_0205DF94(e, 1);
    func_020A6138(e + 4);
    if (changed) { ... }
}
```

It already special-cases heroes, which is a hint that team switching has known
consequences the game handles one at a time.

### Why hooking it does not fix the healing

The stale state is the aura's **candidate list** at `spell+0x3C` and the effect
handles in `spell+0x58` (`docs/95` above). `Entity::SetOwner` has the entity and
the new owner and **no handle to any spell**, so from there there is nothing to
remove the entity from.

The other candidate intercepts were checked and rejected too:

* **`func_0205E7F4`, the HP write** — 1,244 bytes with **17 callers**. It is the
  general HP path for damage and healing alike, not a heal-specific hook, so
  overriding it would put an ownership test in front of every hitpoint change in
  the game.
* **`func_020789C8`, the effect attach** — does receive the spell, so the
  healer's owner is reachable there. But it only attaches the visual; the HP
  change is `func_0205E7F4` in the tick. Gating it would stop the sparkles and
  not the healing, which is worse than doing nothing.

### So the tick it is

`Sim::HealSpell::tick` (`func_02078770`, 600 bytes) is the only place where the
spell and the target are both in scope at the moment HP is written. A faithful
override means reimplementing ~150 instructions and ten guest calls, with the
toggle off reproducing all of it exactly.

That is real work rather than a blocked question, and it is now the only route
recorded rather than one of several maybes.

## Transport occupants: what is known, and what is not

The options gained a *Hotwire Kills Transport Occupants* entry with three
states — off, destroy the occupants, or convert them to the transport's new
owner. The UI carries it; **the ROM work is not done and it is badged
`[unread]`**.

What was established:

* **Transports are category 5**, confirmed twice over: `docs/data/entities.tsv`
  lists 7 of them, and `Sim::LoadAction`'s slot 2 tests
  `entity->def_160->[8]->cat_5C == 5` explicitly at `0x020538F0` before taking a
  transport-specific branch.
* The classes are `Sim::LoadAction` / `Sim::UnloadAction` and
  `Sim::GarrisonEntityCommand` / `Sim::UngarrisonEntityCommand`.
* `UnloadAction`'s slot 2 (`func_02055CDC`, 252 bytes) is a **validity state
  machine** — it checks the destination tile is free and writes a result code to
  `action+0x14`. It never touches an occupant list.
* `LoadAction`'s slot 2 (`func_020537B0`, 576 bytes) ends by calling
  `func_0205B2FC(entity, &transport->tile_DC, flag)`, which is where the unit
  actually goes aboard.

**What is missing is the list itself.** Nothing read so far shows where a
transport records who is inside it, so neither "destroy the occupants" nor
"convert them" can be written. `func_0205B2FC` is the next read and is the
likely owner of that state.

Both requested behaviours need it equally: destroying occupants means walking
the list and removing each, and converting them means walking the same list and
calling `Entity::SetOwner` (`func_020598C4`, located above) on each. The second
is the more interesting one for the caps work, since converted units count
against the *new* owner's allowance.

### Why "consider the occupants lost" is the clean option

Domain knowledge from the community, recorded because the ROM cannot say it and
it is the reason the option is shaped the way it is.

**In vanilla, hotwiring a transport takes its occupants hostage.** They are
stuck aboard until the hotwirer destroys or unloads the vessel, and the area
around the hotwired transport darkens. So the shipped behaviour already leaves
the original owner with units they cannot see, cannot use, and cannot recover —
while those units still count against their cap.

That reframes the *On (destroy the occupants)* option. It is not introducing a
silent anomaly; it is **resolving one the game already has**, and refunding the
cap is the honest accounting for units that were already lost in every sense
that matters to the player.

It also explains why the three states are the right set:

* **Off** keeps the hostage state, for anyone who wants the shipped game.
* **On** writes the loss off immediately and frees the cap.
* **Hotwires them** hands the occupants to the new owner along with the vessel,
  which is the outcome the hostage state gestures at without ever delivering.

An earlier note in this file worried that silent deletion "might reasonably read
as a bug". That worry was wrong and is withdrawn: measured against a hostage
state with a darkened map, an immediate clean loss is the *less* surprising
behaviour.

### The occupant list, found

`entity+0xFC` is the occupant container. Three functions establish it and they
agree:

* the entity **constructor** (`func_02059528`) initialises exactly that offset
  with `func_020EC5A8(e + 0xFC, 1)`;
* the **boarding** function `func_0205B2FC(transport, &unitTile, flag)` ends by
  calling `func_020EC8DC(transport + 0xFC, packed(unitTile))`;
* the unit separately records *its* transport in `unit->0x114/0x115`, which is
  what `UnloadAction` reads back.

The container is a plain growable array of **2-byte elements**:

```
func_020EC5A8(c, grow):          func_020EC8DC(c, item):   /* push_back */
  c+0x00  vtable                   n = ++c->count_08
  c+0x04  data      = 0            ensure_capacity(c)          /* func_020ECA5C */
  c+0x08  count     = 0            d = c->data_04
  c+0x0C  0                        d[(n-1)*2 + 0] = item.x
  c+0x10  0x10  (grow step)        d[(n-1)*2 + 1] = item.y
  c+0x14  1  (byte)
  c+0x15  grow (byte)
```

So for a transport at `E`:

```
count = *(u32*)(E + 0xFC + 8)
array = *(u32*)(E + 0xFC + 4)      /* array[i] is two bytes: tile x, tile y */
```

**Occupants are identified by TILE, not by pointer**, which fits the rest of this
game's design — the same tile-keyed lookup that `EntityAtTile` and the hotwire
resolver use. Each entry resolves to an entity through `func_0208727C` or
`EntityAtTile`.

That is everything both requested behaviours need. Destroying means walking the
array and removing each entity; converting means walking it and calling
`Entity::SetOwner` (`func_020598C4`) on each.

### The remaining cost is the hook, not the data

The natural place is `Entity::SetOwner` itself: it is the single general path
that changes an owner, so a transport being hotwired passes through it. But it
is 304 bytes with roughly eight guest calls, so overriding it faithfully is
comparable work to the heal tick.

Two cheaper post-switch hooks were checked and both are shared, so neither can
be overridden without affecting unrelated callers:

```
func_020A6138   672 B   13 caller functions
func_0205DF94   108 B    6 caller functions
```

So the transport option now has **no unknowns left in its data and one known
cost in its implementation**, which is a better position than the heal fix: that
one needs a 600-byte tick, this one a 304-byte owner setter, and both are
bounded reimplementation rather than further searching.

## The attack boost and ranged units

The options gained *Attack Boost Spell Affects Ranged Units* [Off/On]. The
community rates the attack boost below the speed and defence spells because it
does not reach ranged units. **The spell is not what excludes them**, which is
worth stating because it is the obvious first guess and it is wrong.

### `StatBufSpell` applies to every unit category

Slot 5 (`func_0207C46C`, 360 bytes) is the enter/exit callback, the same slot
`HealSpell` filters in. Read:

```c
cat = entity->def_160->[8]->cat_5C;
building = (cat >= 8);                 /* a 20-arm jump table, 0-7 vs 8-0x13 */
if (building && !(spellId == 0x11 && cat == 9)) return;   /* Mines, spell 0x11 */
apply:  func_0205ED48(entity, this + 0x50);
```

Categories 0 to 7 — Hero, Builder, Melee, **Ranged**, Mounted, Transport, Siege,
Base — all reach `apply`. Ranged units are buffed like everything else. The only
exclusion in the whole function is for buildings, and even that has an exception
for Mines under one spell id.

### Where the buff actually goes

`func_0205ED48` is 52 bytes:

```c
void ApplyBuff(Entity* e, u8 pair[2]) {
    func_020ECDD4(e + 0x1E4, packed(pair));   /* the active-buff list */
    e->flag_226 = 1;
}
```

So a buff is recorded on the **entity** at `+0x1E4`, in the same 2-byte-element
container as the transport occupant list, with a flag at `+0x226`. Buffs are
keyed by tile, like everything else in this game.

### So the gap is in the damage path

If the spell buffs a ranged unit and the unit does not hit harder, then whatever
reads `+0x1E4` when computing damage is consulted on the melee path and not on
the projectile path. That is where the option has to act, and it is **not yet
located** — the option is badged `[read, not wired]` accordingly.

`docs/12` records that ranged units fire projectiles (`K_TransportShip` shoots
`Arrow`, the same as the Archer), so melee damage and projectile damage are
already separate paths in this game. Finding the projectile one and giving it
the same buff lookup is the shape of the fix.

---

# Correction: `+0x18` is the caster's HANDLE, and the tick already reads its owner

The section "So where is the caster's owner?" concludes **"Nowhere on the
spell"** and builds a story in which the aura is anchored to *whoever stands on
tile `+0x18`*. **Both are wrong**, and they are wrong for the reason `docs/105`
gives: `func_020872DC` and `func_02087350` take a two-byte **entity handle**,
not a `TilePos`. Reading them as tile lookups turned a handle field into a
location and invented an anchor that does not exist.

`HealSpell::tick` (`func_02078770`), read directly:

```
add  r0, sb, #0x18          ; the CASTER HANDLE, the same layout HotWireSpell uses
bl   func_020872DC          ; ResolveLiveEntity
cmp  r0, #0
beq  .skip
ldrb r0, [r0, #0xE4]        ; the caster's OWNER
strb r0, [sp]
bl   func_02087634          ; the local player
ldrb r1, [sp]
ldrb r0, [r0]
cmp  r1, r0
beq  .skip                  ; a presentation branch, not a heal filter
```

and at the end of the tick:

```
add  r0, sb, #0x18
bl   func_02087350          ; ResolveEntityHandle
strb r0, [sb, #0x51]        ; the aura ends when the caster stops resolving
str  r0, [sb, #0x14]
```

So the caster's owner **is** available every tick, derived from the caster
handle exactly as `HotWireSpell` does it (`docs/105`). No new state is needed
and no tile anchor is involved.

Two further readings from the same function, both matching known data:

* the target's current health is `entity+0x1A0` and its maximum is
  `[[entity+0x160]+8]+0x62` -- offset `0x62` is `health` in
  `docs/data/entities.tsv`, and `entity+0x1A0` is the same field the build
  sidebar formats as `"%d/%d"` (`docs/104`);
* the heal itself is applied by `func_0205E7F4(target, value)` **after** the
  owner comparison, so that comparison gates a floating-number effect
  (`func_020A7F80` on the difference), **not** whether the unit is healed.

**What this does not yet settle:** where the target list comes from, and whether
it is re-filtered by owner each tick. That is what "Fix Hotwired Healing"
actually turns on, and the caveat the old section raised -- that a fix must not
read the owner through an ambiguous resolver -- dissolves, because there is no
tile ambiguity here at all. The remaining question is the target selection loop
bounded by `spell+0x40`.

## The target loop, and what "Fix Hotwired Healing" actually is

`HealSpell::tick` in full shape:

```c
if (--self->timer /* +0x50 */) return;

caster = func_0208727C(&self->caster /* +0x18 */);
int aura_ok = 1;
if (caster && [[caster+0x160]+8]+0x5C == 7)      /* 7 = Base */
    aura_ok = (caster[0x22E] == 0x64);

for (int i = 0, off = 0; i < self->count /* +0x40 */; ++i, off += 2) {
    Entity* t = func_0208727C(self->targets /* +0x3C */ + off);
    if (!t) continue;
    const uint8_t cat = [[t+0x160]+8]+0x5C;
    if (cat >= 8) continue;                       /* 20-way switch: 0..7 heal, 8..0x13 skip */
    /* heal t towards [[t+0x160]+8]+0x62, its blueprint health */
}
```

* `self+0x3C` is an array of two-byte **entity handles**, count at `self+0x40`.
* Every target is **re-resolved each tick** through `func_0208727C`. So the bug
  is *not* a stale captured list, which was the obvious guess.
* Categories 0..7 are healed and 8..0x13 skipped -- units yes, structures no,
  matching the category ids in `docs/data/entities.tsv`.

**And there is no owner test on the target anywhere in the loop.** The caster's
owner is fetched (see the correction above) but only to gate a floating-number
effect. Nothing checks that the unit being healed still belongs to the player
whose aura is healing it.

That is precisely the reported bug: hotwire a unit standing in a friendly heal
aura and the aura keeps healing it for its new owner, because the aura only ever
asked "is this entity in my list and not a building".

**The fix is therefore one comparison** -- heal only when the target's `+0xE4`
matches the caster's `+0xE4` -- and both values are already resolved in the
function. What remains is finding the smallest honest place to insert it:
`func_020789C8(spell, target)` is called immediately before the heal arithmetic
and `func_0205E7F4(target, value)` applies it, so one of those is likely a
narrower override target than reproducing a 600-byte tick. Neither has been read
yet, and which one is used decides the shape.

## Neither candidate is the insertion point

**`func_0205E7F4` must NOT be overridden.** It has **17 callers**, among them
`ConstructProgressAction::virtual_02` and `RepairStructureAction::virtual_02`.
It is the generic "apply a health value to an entity" routine, not a heal
function -- construction progress and structure repair go through it too.
Overriding it to add a heal-ownership rule would apply that rule to building and
repairing, which is exactly the mistake `docs/105` records: naming a shared
routine after the one caller you met it through. Checked before writing code
this time, not after.

**`func_020789C8` is heal-specific but does not gate the heal.** It has exactly
two callers, both `HealSpell` (`virtual_03` and `virtual_05`), so it is safe to
override -- but the tick calls it *and then* computes the new health and applies
it:

```c
if (current_hp < max_hp) {
    func_020789C8(spell, target);      /* <- candidate */
    ...compute r6 = clamped new health...
}
func_0205E7F4(target, r6);             /* <- the heal actually lands here */
```

Returning early from `func_020789C8` would not stop `func_0205E7F4` from setting
the health. It is the wrong lever.

**So the insertion point is `HealSpell::virtual_03` itself**, and that is 600
bytes with roughly ten guest calls. Reproducing it to add one comparison is the
kind of wholesale re-implementation the project avoids (it invites invented behaviour),
and `lb_dispatch_original` does not help here: it allows a tail call, and this
change has to happen *in the middle*.

**Options, none yet chosen:**

* Override the tick and, before tail-calling the original, filter the target
  array at `spell+0x3C` / `spell+0x40` down to same-owner entities. Cheap and
  uses only the trampoline -- but it permanently drops a target the game added,
  and whether `virtual_05` re-adds targets every tick or only on aura entry has
  not been read. If it is only on entry, this silently breaks healing for units
  that legitimately rejoin.
* Read `virtual_05` (`func_020785D0`) first and find out. That is the next step
  and it is small.

Recording this rather than picking one: the difference between the two is a
fact about the cartridge, not a preference.

## Read, not guessed: targets are event-managed, so filtering the array is wrong

`HealSpell::virtual_05` (`func_020785D0`, 416 bytes) is
`(self, kind, entity)`:

```c
cat      = [[entity+0x160]+8]+0x5C;
eligible = (cat == 0) ? (self->state /*+0x54*/ == 0)
                      : (self->state != 0);
caster   = func_0208727C(&self->caster /*+0x18*/);
if (caster && [[caster+0x160]+8]+0x5C == 7 && caster[0x22E] != 0x64)
    eligible = 0;                       /* a Base aura below 100 heals nobody */

if (kind == 0) {            /* aura ENTRY  -> add to the list */
    if (!eligible) return;
    if (handles_equal(&self->caster, entity+0xDC)) ...   /* the caster itself */
    ...
} else if (kind == 1) {     /* aura EXIT   -> remove from the list */
    ...
}
```

**Membership is event-driven: added on entry, removed on exit, never rebuilt per
tick.** So the cheap fix -- filtering `spell+0x3C` before tail-calling the tick
-- is wrong: a dropped target is dropped for good, because no entry event will
fire again for a unit that never left the aura. A player hotwiring a unit back
would find it permanently unhealable.

This is exactly the question worth reading instead of guessing. Both options
looked reasonable from the outside and only one of them survives contact with
`virtual_05`.

## Where that leaves the fix

The heal must be skipped **per tick, without touching the list**, which means
the change belongs inside `HealSpell::virtual_03`'s target loop. That is 600
bytes with about ten guest calls, and `lb_dispatch_original` does not reach it
because it offers a tail call and this needs the middle.

`func_020789C8(spell, target)` cannot serve either, for a reason now precise:
the tick decides whether to heal from `current_hp < max_hp` read **before**
calling it, and applies the heal **after** it returns. It has no say.

So the honest options are a faithful reimplementation of `virtual_03` -- the
only one of these functions large enough that the risk of re-implementing
behaviour wrongly really bites -- or a narrower guest-side seam that has not been found yet. Recorded as
open rather than filled with the first thing that would compile.

## Attack Boost: the buff consumer is a single function

`docs/95` left "whatever reads `+0x1E4` when computing damage" as not located.
Scanning the simulation (`0x02040000`-`0x02090000`) for genuine
`entity+0x1E4` references -- **excluding `[pc, #0x1e4]`, which is a literal-pool
offset and produced a false hit on the combat function `func_0205AE90` on the
first pass** -- gives exactly nine:

| function | size | role |
| --- | ---: | --- |
| `func_0205D5D0`, `D6E0`, `DC74`, `DD68`, `DE64` | ~250 B each | constructors; `Unit`, `Bridge` and `Gate` virtual_00 among them. Container init, not reads. |
| `func_0205ED48` | 52 B | `ApplyBuff` -- pushes the pair and sets `+0x226` (already known) |
| `func_0205F08C` | 52 B | the same shape; the removal counterpart |
| `func_0205F678` | 72 B | small helper |
| **`func_0205F124`** | **1340 B** | **the consumer**: walks the list and recomputes |

`func_0205F124` writes `entity+0x226` (the buff flag `ApplyBuff` sets) and
`entity+0x194`. It has one caller, `func_0205A2E8`, itself called only from
`func_0205C888`.

**So the buffed stats are recomputed in one place and stored on the entity
around `+0x194`.** That reframes the option: the question is no longer "where is
the buff read during damage" but "does the projectile damage path read
`entity+0x194`, or does it read the `ProjectileBlueprint` directly?" If the
latter, the buff cannot reach it, which is exactly the reported behaviour.

**Next:** find where projectile damage is drawn from and compare it against
`entity+0x194`. That is a narrow, decidable question, and the option stays
`[read, not wired]` until it is answered.

## Attack Boost: the projectile damage choke point is `func_0206E408`

Both projectile commands impact through one 72-byte function:

```
callers of func_0206E408:
   Battles::Sim::MoveProjectileArcEntityCommand::virtual_14
   Battles::Sim::MoveProjectileDirectVectorEntityCommand::virtual_14
```

```c
r4 = func_02087250(cmd + 0x24);        /* resolve an entity from the command */
if (!func_0208727C(target_handle)) return;
def  = r4[0x160];
info = def[8];
func_0206E970(cmd, target_handle, info[0x6C], info[0x6E]);
                                /*     damage_min, damage_max      */
```

`+0x6C` and `+0x6E` are `damage_min` and `damage_max` of a
**`ProjectileBlueprint`** (`tools/lb_blueprints.py`; a `UnitBlueprint` uses
`+0x6C` for `resource_yield` instead, so the record type matters and this one is
the projectile's).

**The damage comes from the projectile's own blueprint. The shooter's buffed
stat block at `entity+0x194` is never consulted.** That is the gap the option
describes, now located rather than inferred: `StatBufSpell` does buff ranged
units (the earlier section established it applies to every category 0..7), the
buff is recomputed into `entity+0x194` by `func_0205F124`, and the projectile
path simply reads a different number.

### Why this is a good override target

72 bytes, two callers, both projectile impacts, and it computes exactly the two
values the option needs to change before handing them to `func_0206E970`. That
is small enough to reproduce faithfully -- unlike `HealSpell::virtual_03`'s 600
bytes -- so the objection to wholesale re-implementation does not bite here.

**Not yet established, and required before writing it:** which entity
`cmd + 0x24` resolves to. It is dereferenced for a blueprint whose `+0x6C` is
read as projectile damage, which says the blueprint is a `ProjectileBlueprint`
and therefore that the entity is the **projectile**, not the shooter. The buff
lives on the *shooter*, so the override needs the shooter handle from the
command, and which field holds it has not been read. Guessing that offset would
apply someone else's attack buff to the shot.

## The asymmetry, stated exactly

`func_0206E970(cmd, target_handle, dmg_min, dmg_max)` -- the applier
`func_0206E408` hands the blueprint numbers to:

```c
projectile = func_02087250(cmd + 0x24);       /* kind 1, docs/105 */
target     = func_0208727C(target_handle);
int range  = dmg_max - dmg_min;
if (range > 0)
    range = func_020833E4(func_0208330C(), 0x0214A138, 0x22B, range);  /* roll */
uint32_t damage = (dmg_min + range) << 12;    /* 20.12 fixed point */
r0 = func_0205F0D0(target);                   /* the TARGET's stat block */
func_0204F2D8(&out, &damage, &scale);         /* damage scaled by it */
```

**The target's buffed stats are consulted. The shooter's are not.** That is the
whole of the reported behaviour, and it matches what players describe: the
defence and speed buffs are felt on ranged units while the attack buff is not.
Defence is applied here, on the receiving side; attack would have to be applied
on the firing side, and nothing on the firing side is read.

`func_0205F0D0` is the counterpart to `func_0205E218` (the `+0x198` getter) and
belongs to the same `+0x194`/`+0x198`/`+0x19C` block that `func_0205F124`
recomputes from the buff list.

### What is still missing

The **shooter**. `cmd+0x24` is the projectile, not the unit that fired it, and
the attack buff lives on the firer. Neither the projectile entity nor the
command has been read for a creator/owner-entity field, and picking an offset
that merely *looks* like a handle would scale a shot by some unrelated unit's
buff -- silently, and only for players who cast the spell. Guessing here is
strictly worse than leaving the option unwired, which is what it stays.

### A handle on the projectile at `+0x166`

`MoveProjectileBaseEntityCommand` slot 0 (`func_0206E220`) resolves the
projectile from `cmd+0x24` and then tests a handle on it:

```c
proj = func_02087250(cmd + 0x24);
if (!handles_equal(proj + 0x166, null_handle())) {   /* func_02087404 */
    pos = func_0205BE04(proj, 0);
    if (cmd[0x20] == pos[4] && cmd[0x21] == pos[5]) { ... }
}
```

So a projectile carries a handle at `+0x166`. **Whether that is the shooter or
the intended target is not established** -- the code around it compares a tile
position from the command against the resolved position, which reads more like
target tracking than attribution. `func_0205BE04` is the position accessor from
`docs/108` (it defers to `entity+0x114`'s transport when there is one).

Naming it would be a guess, and it is the exact guess that decides whose attack
buff scales the shot. Left unnamed.

### `projectile+0x166` is the SHOOTER, established four ways

The `Projectile` constructor `func_0205D100` (vtable `0x021496F4`, blueprint at
`+0x160`) nulls **two** handle fields, `+0x164` and `+0x166`, so choosing between
them mattered. The firing path settles it.

`func_0205079C(action, target)` is called from **`Battles::Sim::AttackAction::virtual_02`**
and opens:

```c
r4 = target;                       /* its own handle read from +0xDC/+0xDD */
r5 = action;
r6 = func_020872DC(action + 0x18); /* resolve a NON-projectile entity, docs/105 */
```

and later, when it builds the projectile:

```c
proj[0x166] = action[0x18];        /* both bytes */
proj[0x167] = action[0x19];
```

So `projectile+0x166` carries `action+0x18`, and `action+0x18`:

1. is copied verbatim into the projectile's handle field;
2. is resolved with `func_020872DC`, which returns **kind 0 -- non-projectile**
   (`docs/105`), so it names a unit;
3. belongs to `AttackAction`, whose subject is the attacking unit;
4. matches the engine's `+0x18 = the owning entity` convention, the same slot
   `HotWireSpell` and `HealSpell` use for the caster.

**That is the shooter**, and it is reachable from inside `func_0206E408` with one
resolve. The last unknown blocking Attack Boost Affects Ranged is closed.

**Still needed before writing the override:** which slot of the buffed block
(`+0x194` / `+0x198` / `+0x19C`) carries the attack multiplier, and how
`func_0204F2D8` applies one, since the damage path scales by the *target's*
block through `func_0205F0D0` and the same machinery should serve the shooter's.

### Correction: `func_0205F0D0` reads `+0x164`, not the `+0x194` block

An earlier paragraph here called `func_0205F0D0` "the counterpart to
`func_0205E218` (the `+0x198` getter) ... in the same `+0x194`/`+0x198`/`+0x19C`
block". **That is wrong.** It is a two-instruction tail call:

```c
func_0205F0D0(e) { return func_0205CBF4(e + 0x164); }
```

The damage path scales by something reached through **`entity+0x164`**. Both the
melee branch and the projectile branch of `AttackAction` call the same
`func_0205F0D0` / `func_0204F2D8` pair, so that part is shared and is *not*
where melee and ranged diverge.

Two consequences:

* The `+0x194`/`+0x198`/`+0x19C` block that `func_0205F124` recomputes from the
  buff list and the `+0x164` value the damage path scales by are **different
  fields**, and the relationship between them is unread. Assuming the buff block
  feeds the damage scale would be a guess.
* Note also that `+0x164` is the offset the *`Projectile`* constructor nulls as a
  handle. On a unit it is evidently something else. Offsets do not carry meaning
  across record types, and reading one as the other is how `docs/101` went wrong
  about blueprints.

So the shooter is identified (`projectile+0x166`, four ways) but **how an attack
buff would be applied is still unread**, and the option stays unwired. What
remains is to follow `func_0205CBF4` and find where the melee branch differs
from the projectile branch, since the two share the scaling call and must
diverge somewhere earlier.

### RETRACTED: `projectile+0x166` is the TARGET, not the shooter -- and the real divergence

The "established four ways" section above is **wrong**. Its load-bearing reason
was reason 4, that `+0x18` is the engine's owning-entity slot by analogy with
`SpellBase`. An analogy is not evidence, and the melee branch of the same
function disproves it.

`func_0205079C(action, r1)`, melee branch:

```c
spread = [[r4+0x160]+8] + 0x6A;        /* r4's blueprint MELEE_SPREAD */
roll   = spread ? random(0..spread) : 0;
base   = func_0205F0F0(r4, r6);
damage = (roll + base) << 12;
scale  = *func_0205F0D0(r6);
```

`melee_spread` belongs to the **attacker**, so `r4` is the attacker -- and `r4`
is the function's `r1` argument, not `action+0x18`. Since
`r6 = func_020872DC(action + 0x18)` and `r6` is the one whose blueprint and
defence are consulted, **`action+0x18` is the TARGET**. And
`projectile[0x166] = action[0x18]`, so `projectile+0x166` is the target too.

That also fits `func_0206E220`, which resolves `proj+0x166` and compares its
position against a tile stored in the command: target tracking, not attribution.

**By elimination the shooter is the other handle the `Projectile` constructor
nulls, `+0x164` -- but that is an inference, and inference is what produced the
retraction above. It needs its writer found before it is used.**

### The divergence, finally located

```c
/* melee   */ base = func_0205F0F0(attacker, target);
/* ranged  */ base = projectile_blueprint[0x6C .. 0x6E];   /* func_0206E408 */
```

and

```c
func_0205F0F0(a, b) { return func_0205CC88(a + 0x164, b[0x160]); }
func_0205F0D0(e)    { return func_0205CBF4(e + 0x164); }   /* -> e + 0x174 */
```

`entity+0x164` is the **`Sim::MagicInfluencedUnitStats`** block. `+0x194` is a
field inside it at `+0x30`, which reconciles the previous correction here -- the
buff block and the damage-scaling block are the **same** structure, not
different ones as that note claimed.

**So melee damage comes from the attacker's buffed stats and ranged damage comes
from the projectile's blueprint.** That is the whole bug, in one line each, and
it explains precisely why the attack buff is felt on melee and not on ranged
while defence is felt on both.

### `projectile+0x164` is the shooter, by its writer

`func_0205C604` (144 bytes, one caller `func_0206B818`) spawns a projectile:

```c
pool = *(u32*)(0x02155024 + 4);
if (!func_02082DF4(pool, &handle)) return;   /* allocate */
func_0205C708(bp, &handle, r5);              /* construct */
proj  = func_02087250(&handle);              /* resolve as kind 1, docs/105 */
owner = func_02087390(r6);                   /* r6's owner */
func_020598C4(proj, owner);                  /* the projectile inherits it */
proj[0x164] = r6[0];
proj[0x165] = r6[1];                         /* r6's handle, both bytes */
```

`r6` is a handle pointer -- `func_02087390` takes one (`docs/105`) -- and it is
the handle whose **owner the projectile is given**. A projectile inherits its
shooter's owner, so `r6` is the shooter and `projectile+0x164` is the shooter's
handle. Evidenced, not inferred, which is what the previous retraction demands.

Note this does **not** conflict with `entity+0x164` being the
`MagicInfluencedUnitStats` block on a unit. `Projectile` and `Unit` are
different records; the constructor `func_0205D100` nulls `+0x164` and `+0x166`
as handles precisely because a projectile has no stat block there.

### Everything the override needs, and the one design question left

```c
proj    = func_02087250(cmd + 0x24);      /* already in func_0206E408 */
shooter = func_020872DC(proj + 0x164);    /* kind 0, a unit */
```

with the melee reference being `func_0205F0F0(attacker, target)`.

**The remaining question is not where but how much.** Melee takes its base damage
*entirely* from the attacker's buffed stats; ranged takes it *entirely* from the
projectile's blueprint. Simply substituting `func_0205F0F0` for the blueprint
value would replace a ranged unit's damage with its **melee** damage -- and
`docs/data/entities.tsv` shows heroes with `melee_damage` of 0 and a projectile,
so that would silently zero their shots.

The option therefore has to apply the buff's *delta* to the blueprint number, and
what that delta is means reading how `func_0205F124` changes the stat block
relative to its base. That is the next read, and the last one.

### Correction: `+0x194` and `+0x198` are `ArrayU32` pointers, not stats

Earlier notes here treated `entity+0x194` / `+0x198` / `+0x19C` as a "buffed stat
block". **They are not scalars at all.** `func_0205F124` reads a count from
`entity+0x1EC`, allocates through `func_0204C6E4`, stores the result at
`+0x194`, and drives `+0x198` with `Battles::ArrayU32::virtual_00`,
`func_020EDB10` and `func_020EDCC0` -- container operations. `func_0205E218`,
which returns `e[0x198]`, hands back an array pointer, not a stat.

The actual stat block is **`entity+0x164`**, the
`Sim::MagicInfluencedUnitStats`, reached by `func_0205F0D0` (`+0x164 + 0x10`)
and `func_0205F0F0` (`+0x164` against a blueprint). That part stands.

### An honest note on this thread

Attack Boost has now cost several corrections in a row -- `func_0205F0D0`
misplaced in the `+0x194` block, `projectile+0x166` called the shooter on the
strength of an analogy, and now `+0x194` read as a stat when it is a container.
Each was a plausible reading of a partial view, and each was caught only by
following one more reference.

The pattern is clear enough to name: **this structure has been characterised by
inference from call sites rather than by reading the type.** The remedy is to
read `Sim::MagicInfluencedUnitStats` itself -- its constructor, its size, and
what `func_0205CC88` does with `attacker+0x164` and a blueprint -- before any
further claim about how a buff changes damage.

Confirmed and standing, for the record:

* `func_0206E408` is the projectile damage choke point (72 bytes, two callers);
* projectile damage comes from the `ProjectileBlueprint`, melee from
  `func_0205F0F0(attacker, target)`; that is the divergence;
* `projectile+0x164` is the shooter's handle, by its writer `func_0205C604`;
* `projectile+0x166` is the target's handle;
* `entity+0x164` is the `MagicInfluencedUnitStats` block.

Not established: how a buff changes the numbers in that block.

## Attack Boost: the complete chain, read rather than inferred

Following the method change -- read the type, not the call sites -- the whole
path is now established.

**The stat block.** `entity+0x164` is `Sim::MagicInfluencedUnitStats`.

```c
/* func_0205CC14 -- ResetToBase, called at the top of the recompute */
s[0x0C].u16 = s[4][8][0x68].u16;   /* blueprint MELEE_DAMAGE   -> entity+0x170 */
s[0x14].u8  = s[4][8][0x6D].u8;    /* blueprint ATTACK_INTERVAL-> entity+0x178 */
```

`+0x68` and `+0x6D` are `melee_damage` and `attack_interval` in
`docs/data/entities.tsv`, which confirms the seeding both ways.

**The buff application.**

```
entity+0x1E4 (buff list)  ->  func_0205F678  ->  func_0205CCC4
                                                   ResetToBase, then apply
                                                   -> writes s[0x0C]
```

So `entity+0x170` holds **base melee damage plus whatever the buffs add**, and
the base is recoverable from the blueprint at any time.

**Where it is consumed.**

```c
/* melee  */ func_0205F0F0(atk, tgt) = func_0205CC88(atk+0x164, tgt[0x160])
             func_0205CC88(s, bp) = max(1, s[0x0C] + matchup(s, bp))
/* ranged */ func_0206E408: damage = ProjectileBlueprint[0x6C .. 0x6E]
```

Melee reads `s[0x0C]`, which carries the buff. **Ranged never touches the stat
block at all.** That is the entire defect.

## The override, fully specified

In `func_0206E408` (72 bytes, two callers, both projectile impacts):

```c
proj    = func_02087250(cmd + 0x24);          /* already there */
shooter = func_020872DC(proj + 0x164);        /* the shooter, docs above */
if (setting_on && shooter) {
    base   = [[shooter+0x160]+8][0x68].u16;   /* blueprint melee_damage */
    buffed = *(u16*)(shooter + 0x170);        /* base + buffs           */
    delta  = (int)buffed - (int)base;
    if (delta > 0) { dmg_min += delta; dmg_max += delta; }
}
func_0206E970(cmd, target, dmg_min, dmg_max);
```

**Adding the delta rather than substituting `func_0205F0F0` is the point.** A
ranged hero with `melee_damage` 0 buffs to `0 + delta`, so the delta is exactly
the spell's contribution and the projectile keeps its own blueprint damage.
Substituting would have zeroed those units' shots, which
`docs/data/entities.tsv` shows is not hypothetical.

Every offset above is read from a writer or a type, not inferred from a call
site -- which is what the three retractions on this option earned.
