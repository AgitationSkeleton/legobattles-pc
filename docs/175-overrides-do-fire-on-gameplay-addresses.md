# 175 — Dispatch overrides DO fire on gameplay addresses

Status: **measured.** Retracts the conclusion docs/143 and docs/144 drew, and
retires the `BLOCKED` badge entirely.

## What was believed

docs/143 counted which addresses the recompiler dispatches to, found gameplay
functions had none, and concluded an override registered for one "never runs".
docs/144 carried that forward. Three settings were given a `BLOCKED` badge on
that basis — described in `lb_overlay.cpp` as "not a to-do, it is a dependency on
recompiler coverage" — and stayed inert for that reason alone.

## What is actually true

`tier3_coverage` shows the override addresses executing under tier 3, which is
what the old conclusion predicted:

    kSetOwner   020598C4 : TIER-3, 16 hits
    kHealTick   02078770 : tier-3 inside the function (020787E0, ...)

But tier-3 execution and dispatch are not exclusive, and the way to know whether
an override runs is to have it say so. Counting inside each override, with
`LB_TRACE_DISPATCH=1`, after a warp and 120 frames of ordinary play:

    [lb_disp] set_owner_override FIRED
    [lb_disp] heal_tick_override FIRED

**Both fire.** So a dispatch override registered for a gameplay address does run,
and the premise behind `BLOCKED` is wrong.

`projectile_damage_override` and `hotwire_tick_override` did not appear, but
nothing in 120 frames of that save fires a projectile-damage or hotwire path.
That is "not exercised", which is a different statement from "cannot run", and
the two must not be conflated — conflating them is what produced the original
error.

## Badge changes

    Fix Hotwired Healing    READ    -> WIRED     its own override was seen firing
    Hotwire transport       BLOCKED -> WIRED     rides SetOwner, seen firing
    Hotwire targets         BLOCKED -> READ      not a dispatch problem at all:
                                                 docs/105 found no category test
                                                 in HotWireSpell, so the vanilla
                                                 filter is in an unlocated
                                                 targeting layer
    Attack boost (ranged)   BLOCKED -> READ      mechanism proven, but THIS path
                                                 has not been seen firing, so it
                                                 is not claimed as wired

Census: **9 wired, 6 read, 6 unread, 0 blocked**, plus the 18 cheats of docs/171.

## The lesson

`BLOCKED` was a confident category built on a negative inference — "we counted
the dispatch table and gameplay addresses were not in it, therefore an override
cannot run". The counting was right and the conclusion did not follow.
**Instrumenting the thing itself took one build and settled it**, where the
inference had stood unchallenged across several documents and left working
features switched off.

`LB_TRACE_DISPATCH=1` keeps the probes available, alongside `LB_BALANCE`,
`LB_CHEATS`, `LB_FIELD_DUMP` and `LB_WIDE_FIELD`.

---

# Trying to observe the other two, and what it cost

`projectile_damage_override` and `hotwire_tick_override` stayed unobserved, so
their settings stay `[read]`. Two attempts:

* **`LB_AUTOPLAY`** only walks menus -- it holds Start and A on a slow cycle and
  taps the screen centre. It cannot order units, so it cannot start a fight.
* **Moving the enemy next to the player's base**, by writing entity `+0x0EC` /
  `+0x0F0` through the debug server. The positions changed and 600 frames ran; no
  combat. The units in this save are a hero, a builder and a structure per side,
  and builders and structures do not attack -- and a teleported unit has no
  order, so nothing engages.

Recorded because it is a real limit of the headless harness: **combat cannot
currently be staged**, so any override on a combat path is unobservable here.
That is a gap in the test rig, not evidence about the override.

## A side finding: written positions do not immediately move what is drawn

Parking five entities at world (2000,2000) and leaving one in view left the other
five still drawn where they were, while the moved one DID appear at its new
place. So the render position is not read straight from `+0x0EC` / `+0x0F0` every
frame for every entity. Anything that writes entity positions should expect that.

## The anchor, cross-checked

With one unit placed deliberately at the screen's dead centre -- world
`(camera + (128,96))`, which removes the attribution problem entirely -- its
sprite lands at screen (139,96), an anchor of **(+11, 0)**. The constellation
match in docs/174 gave **(+19, -1)**.

Those agree to about 8 px. The constellation value is kept: it averages three
units rather than one, and 8 px is well inside the 40x40 capture patch. Worth
stating that the anchor is good to roughly ±8 px and no better.
