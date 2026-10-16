# func_020872DC is not a hotwire function — a retraction, and the real spell

**Status: correction.** Docs 84, 86, 93, 95 and 102 call `func_020872DC`
`HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
wrong, and the port shipped a dispatch override built on them. The override is
removed. Retracted in place.

## What the two functions actually are

```
func_02087350  ResolveEntityHandle(EntityHandle* h)      ; ARM, 64 bytes
    r0 = *(u32*)0x02155028                 ; the entity pool
    e  = func_0205C9B4(pool, h)            ; look the handle up
    if (e) return e;
    n = func_020873C4();                   ; the null-handle constant
    h[0] = n[0]; h[1] = n[1];              ; stomp the caller's handle
    return NULL;

func_020872DC  ResolveLiveEntity(EntityHandle* h)        ; ARM, 44 bytes
    e = ResolveEntityHandle(h);
    if (e && *(u8*)(*(u32*)(*(u32*)(e+0xE0)+4)+8) == 0) return e;
    return NULL;
```

The argument is a **two-byte entity handle**, not a `TilePos`. The giveaway is
the tail of `func_02087350`: it writes a null handle back **through the caller's
pointer** when the lookup fails. A tile coordinate is an input; a handle is a
weak reference that gets invalidated in place. An entity's own handle lives at
`entity+0xDC` — `HotWireSpell::virtual_05` copies it from exactly there.

The byte at `[[e+0xE0]+4]+8` is a liveness/state test, not a target filter.

## Why the override was harmful

`func_020872DC` has **80 direct callers**; its wrapper `func_0208727C` has
**264**. Among them: `FreezeEffect`, `TeleportEffect`, `LoadAction`,
`RepairStructureAction`, `ConstructProgressAction`, `ArrowHighLightEffect`. It
is the engine's universal handle-to-entity resolver.

The port had installed, on that function:

* **Patch Hotwire Glitch** — return NULL unless the entity is category 5 or 6.
  Applied at 80 call sites. `func_020D1744`, the build/spell sidebar, resolves
  the selected entity through `func_0208727C`; a builder is not category 5 or 6,
  so **selecting a builder would have resolved to NULL and the red sidebar would
  never have opened.** Construction, repair, freeze and teleport fail the same
  way.
* **Hotwire Anything** — drop the `[[e+0xE0]+4]+8` test engine-wide, which is
  the liveness check, not a hotwire filter.

Neither toggle was ever observed running: the override announced itself on first
execution and no front-end test can cast a spell, so "installed" was mistaken
for "correct". *An override that cannot be exercised by the available tests is
not evidence of anything.*

## Where hotwire actually is

`Battles::Sim::HotWireSpell`, vtable `0x0214A920`.

| slot | address | size | role |
|---|---|---|---|
| 3 | `func_02078D5C` | 256 B | tick |
| 4 | `func_02078CE4` | 24 B | `return self->field_14 == -1` |
| 5 | `func_02078CFC` | 96 B | record the target |

Slot 5, on event kind 0, tests the stored target at `self+0x50` against the null
handle (`func_02087404` compares handles) and, if unset, copies the candidate's
handle from `entity+0xDC`.

Slot 3, state 0:

```c
self->orig_owner = *func_02087390(&self->target);        /* -> self+0x52 */
self->effect     = func_02016AE4(0x2A, &self->caster, &tmp);
Entity* e = func_020872DC(&self->target);                /* resolve the target */
if (e) {
    func_020598C4(e, func_02087390(&self->caster));      /* Entity::SetOwner */
    func_0205A03C(e, func_0206AFC4(e), 0x0214A938, 0x70);
}
self->state = 1;
```

* `func_020598C4` is **`Entity::SetOwner(entity, owner)`** — confirmed by use,
  and it is the function the "Hotwire Kills Transport Occupants" option needs.
* `func_02087390(EntityHandle*)` returns the **owner** of the referenced entity.
* Caster handle at `self+0x18`, target handle at `self+0x50`, state at `+0x54`.

**Neither slot applies a category filter.** So the vanilla "specials only" rule
is enforced in the targeting/validation layer — which entity a tap is allowed to
nominate — and that site has **not** been located. Until it is, the two hotwire
options are inert and the panel says `[read, not wired]`.

## What this cost, and the rule it earns

The name `HotWire_ResolveTarget` came from finding the function while reading
the hotwire spell, and it then propagated into five documents and one shipped
override without anyone asking who else calls it. Reading a function *through*
one caller says what that caller wants from it. It does not say what the
function is.

**A function's identity is its caller set, not the context you met it in.** For
anything that gets an override, enumerate the callers before naming it — one
`callers` query would have shown FreezeEffect and RepairStructureAction and
killed the name on sight.

---

# Refinement: the byte at `[[e+0xE0]+4]+8` is a kind discriminator

`docs/105` calls the test in `func_020872DC` a "liveness/state test". It is more
specific than that, and the sibling resolver shows why:

```c
func_020872DC(h): e = ResolveEntityHandle(h);
                  return (*(u8*)(*(u32*)(*(u32*)(e+0xE0)+4)+8) == 0) ? e : NULL;

func_02087250(h): e = ResolveEntityHandle(h);
                  return (*(u8*)(*(u32*)(*(u32*)(e+0xE0)+4)+8) == 1) ? e : NULL;
```

Same handle resolution, same field, **opposite constant**. So the byte selects a
kind rather than reporting liveness.

Which kind is which is settled by use: `func_0206E408` resolves `cmd+0x24`
through `func_02087250` and then reads `+0x6C` / `+0x6E` of the resulting
entity's blueprint as `damage_min` / `damage_max`, which are
**`ProjectileBlueprint`** fields (`docs/95`). So:

* **kind 1 = projectile**, and `func_02087250` is `ResolveProjectile`;
* **kind 0 = everything else**, so `func_020872DC` is better read as
  *resolve a non-projectile entity* than as a liveness check.

This tightens rather than overturns `docs/105`: the function is still the
engine-wide handle resolver with 80 callers, and overriding it was still wrong.
But the filter it applies is a type test, which is why every gameplay caller --
freeze, teleport, construct, repair, selection -- passes it: they all operate on
units and buildings, never on projectiles.
