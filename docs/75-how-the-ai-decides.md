# How the AI decides, and the generator it decides with

**Status: the six job kinds are traced from vtable to construction site. Each has
exactly one constructor, one one-line factory, and one decision site — and the
AI's choice between equally good targets is a draw from the game's 64-bit LCG,
which puts the opponent's decisions inside the deterministic simulation.**
Annotation 54.1% → 54.3%; **105 of 105 checks pass**.

## The chain, and the search that found it

`docs/74` left the event codes unnamed and the tick unread. Finding the emitter
first meant finding who owns jobs.

The first attempt was a generic scan for the slot-0 virtual call idiom — `ldr rA,
[rB]` with no offset, then `blx rA`. It returned **3145 call sites in 327
functions**, because that pattern is *every* first-slot virtual call in the game
plus every indirect call through a plain function pointer. Useless.

The method that worked runs the other way and is exact at every hop:

```
job vtable address   (from rtti.json)
  -> the one function whose literal pool references it   = the constructor
  -> the one function that calls that constructor        = the factory
  -> the functions that call the factory                 = the decision sites
```

**One constructor per job. One factory per job.** And the six factories sit at
`0x02090AA8`, `0x02090AC8`, `0x02090AE8`, `0x02090B08`, `0x02090B28`,
`0x02090B4C` — 32 bytes apart, **source order again**, exactly like `docs/71`'s
thirteen objective factories.

| job | factory | decides at |
| --- | --- | --- |
| `JobContractBuild` | `0x02090AA8` | `0x02095104` (240 B) |
| `JobRepair` | `0x02090AC8` | `0x02094938` (164 B) |
| `JobScout` | `0x02090AE8` | `0x0208B460` (1252 B) |
| `JobAttackTaskForce` | `0x02090B08` | `0x0208B460` (1252 B) |
| `JobAttack` | `0x02090B28` | `0x0208B9D4`, `0x0208BC60` |
| `JobDefend` | `0x02090B4C` | `0x0208B950` (132 B) |

**No overlay calls any of the six.** The AI is entirely ARM9 code; missions
cannot spawn jobs, only set up the conditions that make the AI spawn them.

`func_0208B460` is the only site that builds two kinds — Scout and
AttackTaskForce — which makes it the AI's main periodic think.

## The defend decision, end to end

`func_0208B950` is 132 bytes and complete:

```c
Vector_Init(&candidates, ...);
int pick = 0;
threshold = func_0208C1C8(ai);
CollectCandidates(&candidates, ..., threshold);

if (candidates.count <= 0) goto done;
if (candidates.count > 1)
    pick = Random_Below(candidates.count);      /* <-- the choice */

target = candidates.data[pick];
urgent = (func_0209DE9C(ai->obj_4C, 6) < 1);    /* see docs/76 -- NOT a category */
job    = New_JobDefend(ai + 8, urgent);
func_02099CC8(ai->obj_4C, target, job, -1);
done:
Vector_Free(&candidates);
```

Two things a port has to get exactly right:

* **`Random_Below` is called only when there is more than one candidate.** With
  exactly one, no draw happens. Calling the generator unconditionally would
  advance the shared RNG state by one extra step and desynchronise everything
  downstream — the classic lockstep bug, and `docs/25` says this game is
  deterministic lockstep.
* **The urgency flag is `count(kind 6) < 1`.** ~~whether the AI owns any siege
  or special unit (`docs/72`'s category 6)~~ — **corrected by `docs/76`.** The
  counter compares a **word** at `+0x24`, and a blueprint category is a **byte**,
  so this is not the category field. The key range 0–6 coincides with the unit
  categories but the access path does not. What it counts is recorded in
  `docs/76` with the interpretation left open.

## The generator

`Random_Below` is 24 bytes and does nothing but fetch the generator and forward
the bound. The generator itself, `func_020F11F8`, is a **64-bit linear
congruential generator** built from 32-bit parts:

```asm
umull ip, r3, r2, r1        ; 64-bit multiply of two state words
mla   r3, r2, r0, r3        ; cross terms folded into the high word
mla   r3, r0, r1, r3
adds  ip, lr, ip            ; add the increment
```

State lives at `+4` through `+0x18` of a `Battles::Random` — six words, so a
64-bit state, a 64-bit multiplier and a 64-bit increment. It is guarded by a
call to `func_0208330C` before touching any of it.

**The AI draws from the same generator the simulation uses.** That is the design
decision worth flagging above all others here: the opponent's choices are not
"AI randomness" sitting outside the model, they are simulation state. Reproduce
the LCG bit-for-bit, in the same call order, or replays and multiplayer diverge.

## The repair decision, and an offset trap

`func_02094938` caches a tile before deciding:

```c
if (!req->tile_x_4) {              /* not chosen yet */
    req->tile_x_4 = ai->byte_4C;   /* <-- a BYTE here */
    req->tile_y_5 = ai->byte_4D;
}
if (!req->tile_x_4) {              /* still nothing: derive one */
    e = EntityByIndex(ai->world_0, index);
    if (!e || (u8)e->[0x20]->[8] == 0) return 0;
    ...
}
job = New_JobRepair(ai + 0x21);
```

**`+0x4C` is a pointer in the player object and a byte here.** `docs/72` read
`player+0x4C` as a sub-object pointer and was right; this object is a different
class, and `+0x4C`/`+0x4D` are a tile `x`/`y` pair — the same two-byte tile shape
`docs/63` found at a fog circle's `+0x0C`/`+0x0D`.

That is `docs/60`'s rule for the third time — **the same offset in two record
types is two fields** — and the tell was the *access width*: `ldr` in one,
`ldrb` in the other. The width is in the disassembly and costs nothing to check.

## Tips and pitfalls

* **Search along the definition chain, not for the call shape.** vtable →
  constructor → factory → caller gave exactly one answer at each hop; a pattern
  scan for the same thing gave 3145 hits of noise.
* **A conditional RNG call is a specification, not an optimisation.** "Only draw
  when the choice is real" has to be reproduced exactly in a lockstep game.
* **Access width disambiguates a shared offset.** `ldr` versus `ldrb` at `+0x4C`
  separated two unrelated classes in one glance.
* **Consecutive one-line factories mean source order.** Third family found this
  way — objectives in `docs/71`, and now jobs.
* **"No overlay calls this" is a real finding here**, because the overlays were
  swept exhaustively (`docs/69`) rather than assumed.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.3%** | 2,501 listed + 2,553 derived, 109 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
