# What "specials only" means, and the one thing still missing

## The category, from docs/72

`blueprint+0x5C` is the entity category, confirmed from the function that reacts
to unit counts rather than inferred from the table:

```
0  Heroes            5  Transport Ship
1  Builder           6  Siege/Special -- Ballista, Catapult, Dragon
2  Melee             7+ structures
3  Ranged
4  Mounted
```

So the hotwire combo's *"Patch the glitch (specials only)"* means **category 6**,
and plausibly category 5 as well, since a transport is the other thing a
hotwire-style ability conventionally steals. Category 6 is the one the name
matches exactly.

`lb_patches.cpp` already reads this field -- `bus_read_u8(info + 0x5Cu)` -- for
the ranged attack-boost patch, so the accessor is in place and proven.

## Where the filter would go

The overlay's own note says the vanilla rule "is in the targeting layer, not yet
located", and `docs/125` reached the same conclusion from the other direction
after examining and rejecting two candidates.

But mode 1 does not need the targeting layer. `HotWireSpell`'s tick
(`func_02078D5C`) calls `Entity::SetOwner` on the handle at `self+0x50`, and
that is the **effect** point: refusing the ownership change there enforces
"specials only" regardless of what the targeting layer offered.

The shape is the same bracket `docs/132` used for healing:

1. override the hotwire tick to set a flag around the original;
2. in the **existing** `set_owner_override`, when that flag is set and the mode
   is 1, check the target's category and refuse if it is not 6.

## The one thing still missing

**How to refuse a call safely.** The runtime has no documented "return from an
override without calling the original". `docs/132` sidestepped this for healing
because a heal has a natural no-op -- write the current hp -- and every path and
side effect stays as the cartridge has them.

`SetOwner` has no equivalent. Its natural no-op would be passing the entity's
current owner, since `docs/108` shows it early-returns when the owner is
unchanged, but the owner arrives as a **pointer** (`r1`), so producing that no-op
means writing to guest memory the caller owns, or pointing `r1` at scratch. Both
are more invasive than the change they serve.

The alternatives are worse: clearing the spell's target handle mutates spell
state the tick may re-derive, and returning from the override without calling
the original assumes a calling convention nothing documents.

## Decision

**Not implemented.** The evidence for *what* to filter is complete; the mechanism
for *how* to suppress the call is not established, and the current harness
cannot verify a hotwire behaviourally anyway (`docs/133`: a Free Play match has
no combat and no healer, and the same is true of a hotwire).

Writing it anyway would be shipping an unverifiable control-flow change built on
an assumed convention, which is the combination `docs/105` and `docs/125` both
came from.

What is now settled and was not before: the category value, the accessor, the
effect point, and the exact shape the patch takes once the suppression mechanism
is resolved.
