# Declining a call, and the hotwire specials-only filter

## The missing mechanism, found rather than invented

`docs/134` stopped short of implementing the hotwire filter for one reason: the
runtime had no documented way for an override to return *without* running the
function it replaced, and assuming a calling convention is how `docs/105` and
`docs/125` happened.

It is documented -- in the code generator. `arm_codegen.cpp` emits exactly this
for a returning `BX`:

```c
g_cpu.R[15] = <target>;
if (runtime_call_should_return(g_cpu.R[15])) return;
runtime_dispatch_with_exchange(target);
return;
```

`runtime_call_should_return` matches the address against the call-return stack
that `runtime_call_push_return` filled, pops that frame, and reports that the
host frame should simply unwind. `runtime_dispatch` itself is only:

```c
void (*fn)(void) = lookup...;
if (fn) { fn(); return; }
```

so an override **is** the function body, and returning from it the way a
generated epilogue returns is a normal guest return -- not an invented
convention. `lb_override_return()` is that sequence, and nothing more.

This unblocks every setting that needs to suppress a call, not just this one.

## The filter

`HotWireSpell`'s tick (`func_02078D5C`) calls `Entity::SetOwner` on the handle
at `self+0x50`. That is the **effect** point, so the rule can be enforced there
without the targeting layer the overlay notes as unlocated.

* a new override on the tick sets a flag around the original;
* the **existing** `set_owner_override` checks, only while that flag is set and
  only when the mode is *"Patch the glitch (specials only)"*, whether the
  target's category is 6 -- **Siege/Special**, per `docs/72`, confirmed from the
  function that reacts to unit counts rather than inferred from the table -- and
  declines the call if not.

Category 5, Transport Ship, is deliberately **excluded**: the mode is named for
specials, and transports have their own separate setting.

Every other caller of `SetOwner` -- capture, scripting, mission setup -- is
untouched, because the flag is only set inside the hotwire tick. That is the
same containment `docs/132` used for healing, and for the same reason.

## Status

Five overrides now install, the warp reaches a Free Play match, and the field
renders normally (48688/49152 non-black).

**Behaviourally unverified**, like the heal fix and for the same reason
(`docs/133`): a Free Play match with no combat never casts a hotwire, so the
tick never runs and the filter never fires. Verifying needs a hero with the
spell, an enemy special in range, and a cast -- gameplay staging rather than a
harness gap.

What is different from `docs/134` is that the mechanism is no longer assumed.
The code path is the one the recompiler itself generates.
