# 271 — The settings audit: nothing is read-but-not-wired

Purpose: check the ImGui settings for anything read but not wired, such as
hotwire healing.

## CORRECTION: a tool already does this, and had already answered it

`tools/lb_wiring.py` performs exactly this audit, and its 2026-09-13 run gave
the same verdict -- "25 WIRED, 1 UNSEEN, 0 READ, 0 BLOCKED; no orphan exports;
18 settings keys, symmetric". This document's
two hand passes rediscovered that the long way. Run:

```
$ python tools/lb_wiring.py
badge census (the panel's own claim about itself)
    WIRED    25
    READ     0
    UNSEEN   1
    BLOCKED  0
    total    26 call sites
exports defined in lb_overlay.cpp with no reference in any other translation unit
    none
settings.json symmetry
    18 keys, symmetric
OK: nothing is read-but-not-wired.
```

The tool also checks two things the hand passes did not: that no export in
lb_overlay.cpp is unreferenced elsewhere, and that settings.json's keys are
symmetric with the accessors. Use it rather than repeating the manual sweep.

What follows is kept because it names the one remaining control and why it is
UNSEEN rather than unwired, which the tool's census does not say.

## Method

Two passes, because the panel's own badge and the code can disagree and either
one alone would be a single-source answer.

1. Every `lb_setting_*` accessor enumerated from lb_overlay.cpp, then every
   reference to it OUTSIDE the panel listed, and each one opened to check it is
   a real call site rather than a bare `extern` declaration -- which is exactly
   what a read-but-not-wired control looks like from a grep.
2. Every `badge()` call in the panel read, since the file's own vocabulary
   (WIRED / READ / UNSEEN / BLOCKED) records what each control claims about
   itself.

## Result

Eighteen settings. Every one has a genuine consumer:

```
attack_boost_ranged   lb_patches.cpp:845   if (lb_setting_attack_boost_ranged() && projectile)
balance_patch         lb_patches.cpp:3675  int slot = lb_setting_balance_patch();
cheat / cheat_count   lb_patches.cpp:2065  drives the cartridge's flag block
fix_hotwired_healing  lb_patches.cpp:933   if (lb_setting_fix_hotwired_healing() && spell)
hotwire_anything      lb_patches.cpp:3374
hotwire_transport     lb_patches.cpp:619
music_mode            lb_music.cpp:163
pixel_scale           apply_hooks.py:4850  (the adaptive surface)
same_builders         lb_patches.cpp:3691
unlock_units/extras   lb_patches.cpp:1904-1905
enhanced, hud_anchor, hud_scale, view_width, cap   multiple
```

Badges: **no control is `[read, not wired]`, and none is `[blocked]`.** Exactly
one carries a non-WIRED badge:

```
Attack Boost Reaches Ranged Units    [wired, not yet seen firing]   (UNSEEN)
```

**Hotwired Healing -- the example in the request -- is WIRED.** The file records
why the old badge went away: "Both arms now write into the cartridge's own
validator ... which is what the READ badge was honestly recording." So the note
that prompted the question is stale, and docs/143's "the two the panel badged
[read, not wired]" no longer describes this build.

## The one that is left is untested, not unwired

`attack_boost_ranged`'s chain is complete and traced: widget -> accessor ->
`projectile_damage_override`. UNSEEN means it has never been WATCHED taking
effect, and the override already carries the instrumentation to tell the two
apart:

```
[lb_projectile]  ranged damage computed N times      -- did a ranged attack happen
[lb_attackboost] applied +%d to a ranged attack      -- did the boost fire
```

counted separately and deliberately, "so 'the attack boost never fired' cannot
be told apart from 'no ranged attack happened'".

What blocks the observation is staging, not wiring: the boost only applies when
`buffed > base`, so it needs a ranged unit that is CURRENTLY BUFFED to actually
shoot. `LB_ATTACKBOOST=1` forces the setting without the UI but cannot create
the buff. A run that reaches sustained combat with a buffed ranged unit would
close it; until one does, the badge is the honest state.
