# Which overrides can fire, counted

`docs/142` argued that an override fires only when statically generated code
emits a `runtime_dispatch` call for its address. That is now **counted**, per
address, across `port/generated`:

| address | what it is | dispatching call sites | can it fire? |
| --- | --- | ---: | --- |
| `0208727C` | entity resolver (`docs/133`'s probe) | **72** | yes -- and it was observed firing |
| `0205E7F4` | the hp setter (`docs/141`) | **4** | sometimes, but **not** on the healing path |
| `020598C4` | `Entity::SetOwner` | **1** | rarely |
| `0206E408` | projectile damage | **0** | **never** |
| `02078D5C` | `HotWireSpell` tick | **0** | **never** |
| `02078770` | `HealSpell` tick | **0** | **never** |

Every one of these functions has the same shape in the generated output -- a
short entry fragment ending at an indirect branch -- so the difference is not in
the callee. It is entirely in **how many of its callers were statically
recompiled**.

## What this means for the shipped settings

Three overrides **cannot fire at all**, so the settings resting on them do
nothing:

* **Attack Boost Affects Ranged** -- `lb_setting_attack_boost_ranged`, via the
  projectile-damage override;
* **Hotwire targets: specials only** -- `docs/135`'s filter, via the hotwire tick;
* **Fix Hotwired Healing**'s owner bracket -- `docs/132`, via the HealSpell tick.

Two can fire, but not necessarily where it matters:

* **Hotwire Kills Transport Occupants (convert)** rests on `SetOwner`, which has
  a single dispatching call site -- so it works only if hotwire reaches
  `SetOwner` through that one site, which is unknown;
* the hp-write half of the heal fix has four, and the healing observed in
  `docs/140` used none of them.

**These settings are badged as wired and are not.** That is the honest status,
and it is worse than `docs/123`'s audit reported, because that audit checked
whether a getter had a consumer -- not whether the consumer could ever run.

## The remedy is now measurable

Before wiring any future override, count its dispatching call sites:

```sh
grep -rhoF "runtime_dispatch_literal_call(0xADDRESSu)" port/generated --include=*.c | wc -l
```

Zero means the override is decorative. A high count means it is reachable. This
is a one-line check that would have prevented three of the six above.

Where the count is zero, the alternatives from `docs/142` apply: patch data
rather than code, or find a statically covered address on the same path -- for
example, `SetOwner` is reachable and is what hotwire ultimately calls, so a
filter there may work where one on the hotwire tick cannot.

## Method note: the fifth measurement trap

The first version of this table reported **zero** for every address, including
the one known to fire. The count came from `grep -rc` piped through
`awk -F: '{s+=$2}'` -- and on Windows the paths begin `D:`, so field 2 was the
rest of the path rather than the count.

Five measurement errors in this investigation now: constant registers, a saturated
ring, a defaulted dictionary key, an anchored grep, and a drive letter. Every
one produced a confident wrong answer that looked like data. The habit that
catches them is the one used here: **check the instrument against a case whose
answer is already known** -- `0208727C` had to come back non-zero, and when it
came back zero the tool was wrong, not the world.

---

# Superseded by docs/175: overrides on gameplay addresses DO fire

The counting in this document is right; the conclusion drawn from it is not.
Registering a dispatch override for a gameplay address and having it report when
it runs shows that `set_owner_override` and `heal_tick_override` both fire during
ordinary play. Tier-3 execution and dispatch are not exclusive.

The `BLOCKED` badge that rested on this reading is retired, and two settings that
were switched off for that reason are now live. See docs/175.

