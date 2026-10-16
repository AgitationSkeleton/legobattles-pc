# A width bug, and what the frame hook can actually carry

## The width bug

`docs/144` recommended rebuilding the dead settings through the per-frame hook.
Checking the offsets the dead projectile override used turned up a defect in it:

```c
const int32_t base = (int32_t)bus_read_u16(sinfo + 0x68u);   // WRONG
```

The project's own field table, from `tools/lb_blueprints.py`, records:

```c
{0x68u, 1, "melee_damage"},   {0x6Au, 1, "melee_spread"},
```

`melee_damage` is **one byte**. Reading it as a halfword folds `+0x69` into the
value, inflating the base, so the computed `buffed - base` delta comes out too
small or negative -- and the attack boost would have been silently under-applied
or skipped even if the override had been reachable.

Corrected to `bus_read_u8`. This is the width trap this project has recorded
before: the offset was right and the access width was not, and the two must both
come from the same source.

## What the frame hook can and cannot do

`lb_patches_on_vblank()` runs every frame and can read and write the whole
entity pool, which `tools/lb_stage.py` proves is walkable. That makes it the
right home for any setting expressible as **"look at the state and correct it"**.

But the three dead settings are not of that shape, and it is worth being precise
about why rather than discovering it mid-implementation:

* **Attack Boost Affects Ranged.** The boost writes a buffed value at
  `entity+0x170`; projectile damage is supposed to read it and does not. Whether
  a frame hook can fix this depends on whether the boost already populates
  `+0x170` for ranged units. If it does, the fix must be at projectile creation,
  which is unreachable; if it does not, the frame hook can write it. **That is
  an empirical question and the current match cannot answer it** -- it contains
  a Hero, a Builder and a Base per side, and **no ranged units at all**.

* **Fix Hotwired Healing.** The rule is "do not heal a unit whose owner differs
  from the healing aura's owner". A frame hook sees hp change but not *who*
  changed it, so it cannot distinguish an aura heal from any other gain. Any
  frame-hook version would be an approximation, and approximating a simulation
  rule is guessing, which this project does not do.

* **Hotwire targets: specials only.** The rule applies at the moment of a cast.
  A frame hook could detect an owner change after the fact and revert it, but
  reverting a completed hotwire is a different behaviour from refusing one, and
  in multiplayer it would diverge from the other client.

**Only the first is a candidate**, and it is blocked on an observation this
scenario cannot make.

## What this leaves

The frame hook is genuinely useful -- it already carries the build-limit caps
and the blueprint patching -- but it is not a general substitute for call
interception. `docs/144`'s four routes stand, and for these three settings the
honest ordering is now:

1. get a match containing **ranged units** and check whether `+0x170` is
   populated for them, which decides Attack Boost outright;
2. for the other two, the only faithful route is a **library hook with vtable
   discrimination** (`docs/144` route 3) or improved recompiler coverage
   (route 4);
3. do not approximate them in the frame hook.
