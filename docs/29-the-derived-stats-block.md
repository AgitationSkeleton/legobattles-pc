# The derived stats block, and where the blueprint is actually read

**Status: solved. `Sim::MagicInfluencedUnitStats` is laid out, both of its
functions are read, and the blueprint→stats copy site — open since `docs/26` —
is found. One new blueprint field is named on the strength of it.**

`docs/24` established that a buff does not edit a unit's stats in place: an
influence object is pooled and the effective numbers are computed into a
`MagicInfluencedUnitStats`. `docs/26` then found that the blueprint idiom search
misses movement and combat scalars entirely, and concluded that the blueprint
must be *copied* at one site rather than sampled. This is that site.

## Finding it

`Sim::MagicInfluencedUnitStats`'s vtable slots are at `0x0205CB7C`/`0x0205CB80`,
which puts the class's own code in the `0x0205CB..`–`0x0205D0..` range. Listing
the corpus there gives a run of four **eight-byte** functions followed by one of
**932 bytes** — the shape of "several trivial getters and one function that does
the work".

The four getters give the layout for free:

```arm
func_0205CBF4:  add  r0, r0, #0x10 ; bx lr     -> &this[0x10]
func_0205CBFC:  ldrb r0, [r0, #0x14] ; bx lr
func_0205CC04:  ldrh r0, [r0, #0x16] ; bx lr
func_0205CC0C:  ldrh r0, [r0, #0x18] ; bx lr
```

Each has **exactly one caller**, and the four callers sit at `0x0205F0D0`,
`0x0205F0E0`, `0x0205F104`, `0x0205F114` — a 16-byte-strided forwarding block on
`Sim::Unit`. Immediately after it is `func_0205F124`, which `docs/28` identified
as the per-tick update reading the *Fast Magic* and *Regenerating Health* red
bricks. The neighbourhood is consistent: derived stats, their forwarders, and
the tick that consumes them.

## The layout

| offset | type | seeded from | what it is |
| --- | --- | --- | --- |
| `+0x04` | ptr | — | the owning entity (`[this+4]+8` is its blueprint) |
| `+0x08` | 20.12 | `[[entity+4]+4] + 0x0C` — *not* the blueprint | a buffable value from the team/faction side |
| `+0x0C` | u16 | `UnitBlueprint+0x68` | **melee damage** |
| `+0x10` | 20.12 | constant `0x1000` = **1.0** | a multiplier, default identity |
| `+0x14` | u8 | `UnitBlueprint+0x6D` | **attack interval**, ticks |
| `+0x16` | u16 | `UnitBlueprint+0x6C` | **resource yield** — see below |
| `+0x18` | u16 | seeded `1.0` in Recompute | a second multiplier |

## `ResetToBase` — the copy site

`func_0205CC14` is short enough to read whole, and it is the answer `docs/26`
was looking for:

```arm
ldr  r0, [r4, #4]         ; the entity
ldr  r2, [r0, #4]
ldr  r2, [r2, #0xc]       ; a non-blueprint source
str  r2, [sp, #4]
add  r0, r4, #8
bl   func_02004344        ; this[0x08] = that

ldr  r0, [r4, #4]
ldr  r0, [r0, #8]         ; the entity's BLUEPRINT
ldrh r3, [r0, #0x68]      ; melee damage
strh r3, [r4, #0xc]
mov  r2, #0x1000          ; 1.0
add  r0, r4, #0x10
bl   func_02004344        ; this[0x10] = 1.0

ldrb r0, [r0, #0x6d]      ; attack interval
strb r0, [r4, #0x14]

ldrb r0, [r0, #0x6c]      ; <- a field that had no name
strh r0, [r4, #0x16]
```

**Only four values are copied out of the blueprint into the buffable block**,
which is why `docs/26`'s idiom search found so little: the rest of a unit's
numbers — speed, health, cost, build time, ranges — are read from the blueprint
directly at their point of use, and only these are subject to influences.

That is a useful negative fact in its own right. **Speed is not buffable through
this block**, despite `LOC` listing Unit/Area Speed buffs; whatever
`StatBufSpell` does to movement, it does somewhere else, and `+0x10`/`+0x18` are
the multipliers to watch.

## `Recompute` — how a buff is applied

`func_0205CCC4` seeds six accumulators, loops the influence array, and shifts
back:

```arm
; seed -- integers are promoted to 20.12, fixed-point values pass through
[sp+0x84] = [this+0x08]              ; already 20.12
[sp+0x80] = [this+0x0C] << 12        ; melee damage
[sp+0x7C] = [this+0x10]              ; already 20.12
[sp+0x78] = [this+0x14] << 12        ; attack interval
[sp+0x74] = [this+0x16] << 12        ; resource yield
[sp+0x70] = 0x1000                   ; 1.0

; for each influence in the array at sb
r6 = func_020874AC(pool, index)      ; resolve the influence
func_0205D068(&scratch, &sp+0x84, func_0207D1F0(r6))
func_0205D068(&scratch, &sp+0x80, func_0207D200(r6))
func_0205D068(&scratch, &sp+0x7C, func_0207D210(r6))
func_0205D068(&scratch, &sp+0x78, func_0207D220(r6))

; store -- truncate back to integers
[this+0x16] = [sp+0x74] >> 12
[this+0x14] = [sp+0x78] >> 12
[this+0x0C] = [sp+0x80] >> 12
[this+0x18] = [sp+0x70] >> 12
```

Three things fall out of this.

**Every buff is computed in 20.12 fixed point**, the same scale `docs/26` proved
for movement speed, and truncated with `asr #12` on store. A port that does buff
arithmetic in floating point will diverge — and because `docs/25` established
the simulation is deterministic lockstep, divergence is a desync, not a rounding
nuisance.

**An influence carries exactly four modifier components**, read through
`func_0207D1F0`, `D200`, `D210`, `D220` — a 16-byte-strided accessor block on
`MagicInfluenceBase`, matching the four accumulators the loop touches.

**Two accumulators are seeded but not touched by the loop** — `[sp+0x74]` and
`[sp+0x70]` — yet both are stored. The rest of the 932-byte function was not
read, so whether a second pass (upgrades, per `Sim::UpgradeMagicInfluence`)
modifies them is **not established**.

## `UnitBlueprint+0x6C` is the resource yield

The field seeded into `+0x16` had no name. Its distribution settles it:

**It is `25` for exactly six units and `0` for the other 176.** The six:

| `K_Mine` | `W_Mine` | `P_Quarry` | `I_Quarry` | `E_WellCap` | `A_WellCap` |
| --- | --- | --- | --- | --- | --- |

**One resource building per faction, and nothing else in the game.** Six of six,
zero exceptions, across six factions whose rosters share no names — that is not
a distribution a coincidence produces.

It is named **`resource_yield`**. Supporting evidence:

- It lives in the *buffable* block, which is exactly what the **Fast Mining**
  and **Fast Harvesting** red bricks (`docs/28`) would need in order to work.
- `docs/25` found `Sim::IdleMineEntityCommand` — a mine produces by *idling*,
  so its yield has to be a stat the idle command reads.
- `Sim::HotWireSpell` from `docs/24` is a spell with no other obvious target.

**What is not established** is the unit: whether 25 is per harvest cycle, per
trip, or a capacity. The value and its exclusivity are solid; the denominator is
not, and the name is chosen to avoid implying one.

## Pitfall: one caller is a stronger signal than many

The four getters were identified in seconds because each has **exactly one
caller**. That is worth generalising. Throughout this project the productive
searches have been the ones that return *few* results — `docs/26`'s idiom filter
cutting 706 functions to 9, `docs/28`'s partition function reading exactly 18
flags. A search returning one answer is usually a real structure; a search
returning hundreds is usually a coincidence of displacement.

The corollary, learned the expensive way in `docs/26`: when a filter returns a
*small* wrong answer it is far more convincing than a large one, so the control
run matters most precisely when the result looks good.
