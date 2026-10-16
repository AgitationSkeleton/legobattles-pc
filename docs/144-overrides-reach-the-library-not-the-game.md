# Overrides reach the library layer, not the gameplay layer

## The hookable surface, measured

Every `runtime_dispatch_literal_call` target in `port/generated`:

```
distinct dispatched addresses: 2,159
total dispatching call sites:  9,229
addresses with 0 or 1 site:    1,227   (more than half are weak or unusable)
```

The most-dispatched addresses are:

```
386  0204CA50      358  0204C6E4      287  02004344
185  02121CBC      152  020F27D4      149  020F2BA0
144  020F27B8      124  020873C4      104  020F2BD4
```

and grouping all 2,159 by region:

```
48  020F1     47  020EF     46  0204D     45  020EE
41  020ED     39  0210E     39  0210B     39  02087
```

**These are library regions** -- allocators, containers, handle resolvers,
maths. `0204CA50` and `0204C6E4` are the heap; `020F2xxx` and `020ECxxx` are
containers; `02087xxx` is the handle layer.

## And the gameplay layer is not there

Every function these settings actually need:

```
0206E408  projectile damage        0 sites
0206E970  the damage applier       0 sites
02078D5C  HotWireSpell tick        0 sites
02078770  HealSpell tick           0 sites
0205DF94  (the take-then-kill helper) 0 sites
020EB910  the command submit       0 sites
02005B0C  the command factory      0 sites
0207AAB4  the spell base handler   0 sites
```

versus the library functions beside them:

```
0208727C  entity resolver         72 sites
02087404  handle equality         60 sites
020EC5A8  vector init             42 sites
02087350  handle resolve          39 sites
```

The split is not arbitrary. Library functions are called by direct `BL` from
everywhere, so the recompiler covered their call sites statically. Gameplay
functions are reached through vtables and indirect branches -- which is exactly
where the recompiler stops, as `docs/143` saw in every one of those
`end=...  indirect` headers.

**So dispatch overrides can hook this game's library, and cannot hook its
gameplay.** That is a property of how the port was generated, not of any
individual setting, and it explains every result in `docs/132` through
`docs/143` at once.

## What actually works, then

1. **Data patches.** The balance patch already does this -- it rewrites
   blueprint stats in RAM and needs no override at all. Any setting expressible
   as "different numbers" belongs here. Build-limit caps and unit stats qualify.

2. **The per-frame hook.** `lb_patches_on_vblank()` already runs every frame
   from `io.cpp`. A setting expressible as "observe state and correct it" can
   live there without hooking anything: it sees the whole entity pool, which
   `tools/lb_stage.py` shows is walkable and writable.

3. **Library hooks with discrimination.** A reachable library function can be
   hooked if the port can tell *why* it was called -- for instance by reading
   the object's vtable pointer at `[this+0]` and comparing it against a known
   class. This is how a spell-specific behaviour could still be reached through
   a base-class helper, and it is the only route that keeps per-call precision.

4. **Improving the recompiler's coverage** so gameplay call sites become
   static. That is upstream work on ndsrecomp, not on this port, and it would
   fix the whole class at once.

## Method note: the sixth trap

The first version of the reachability table showed dozens of matches for
`0206E970`. The lookup was `awk -v A=$a '$2==A'`, and **awk parses `0206E970` as
scientific notation** -- 206 x 10^970, which is infinity -- so every address
containing an `E` compared equal to every other. Forcing a string comparison
gave the true answer: zero.

Sixth measurement error in this investigation, and the third caused by a tool silently
reinterpreting a hex string. The check that caught it, again, was a value whose
answer was already known.

---

# Superseded by docs/175: overrides on gameplay addresses DO fire

The counting in this document is right; the conclusion drawn from it is not.
Registering a dispatch override for a gameplay address and having it report when
it runs shows that `set_owner_override` and `heal_tick_override` both fire during
ordinary play. Tier-3 execution and dispatch are not exclusive.

The `BLOCKED` badge that rested on this reading is retired, and two settings that
were switched off for that reason are now live. See docs/175.

