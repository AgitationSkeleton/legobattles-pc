# Attack Boost Affects Ranged cannot be implemented as the port stands

`docs/145` left one empirical question: does the attack boost's runtime value at
`entity+0x170` mean anything for a ranged unit? It is now answered from data,
and the answer closes the setting.

## +0x170 is the runtime melee damage

Read live, for every unit in a Free Play match:

```
i   entity    cat  +0x170  bp+0x68  bp+0x62  hp
9   02308530  0    30      30       1100     1100
10  02308774  1     5       5        160      160
11  023089B8  7     0       0       1500     1500
15  02309C30  0    40      40       1000     1000
```

`entity+0x170` equals the blueprint's `+0x68` **exactly**, for every unit. So
`+0x170` is the current melee damage, initialised from the blueprint, and a
boost raises it. Incidentally this confirms `docs/145`'s width fix: 30, 5, 40
and 0 all fit in the single byte the field table records.

## And ranged units have no melee damage

Reading blueprint records straight out of the live block at `0226A130`:

```
unit           cat  melee_dmg+0x68  spread+0x6A  range+0x6F  interval+0x6D
K_Swordsman    2    10              5            1           30
K_Archer       3     0              0            5           24
I_Rifleman     3     0              0            4           24
K_Builder      1     5              0            1           30
```

**Ranged units have `melee_damage = 0`.** Their damage does not come from
`+0x68` and therefore not from `+0x170` either -- it comes from the projectile.

## Which closes the question

A boost adds its delta to `+0x170`. For a swordsman that raises 10 to 10+delta
and matters. For an archer it raises 0 to delta, a number nothing reads, because
the archer's damage lives in its projectile.

So making the boost reach ranged units **must** happen where the projectile's
damage is computed -- which is exactly what the original override did, reading
`shooter+0x170` and adding the delta to the projectile's damage range.

And that address, `0206E408`, has **zero dispatching call sites** (`docs/143`),
so it cannot be hooked. Nor can the frame hook help: the value it would write,
`+0x170` on a ranged unit, is not read by anything.

**The setting is blocked by the port's architecture, not by missing evidence.**
Every fact needed to implement it is known; no available mechanism can apply it.

## Status

`Attack Boost Affects Ranged` should be badged **not implementable at present**
rather than merely unwired, and the reason recorded next to it: the only
correct hook point is statically unreachable.

It becomes possible under either of `docs/144`'s remaining routes -- a library
hook that can discriminate the projectile-creation path, or improved recompiler
coverage that makes `0206E408`'s callers static. The second would fix this and
the other two dead settings together, and is upstream work on ndsrecomp rather
than on this port.
