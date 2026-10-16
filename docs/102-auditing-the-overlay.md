# Auditing the overlay against the decomp

> **RETRACTED IN PART (docs/105).** This page calls `func_020872DC`
> `HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
> wrong. `func_02087350` resolves a two-byte **entity handle** against the pool
> at `0x02155028`; `func_020872DC` adds a **liveness** test. They are the
> engine-wide handle resolver (80 and 264 callers), not hotwire code and not a
> tile lookup. The override the port built on them is removed. Read anything
> below that rests on those names with that correction in mind.


**Every setting that claims to be wired was checked against the ROM analysis it
cites. One was verified complete by a method stronger than the one used to build
it; none was found wrong.**

## What "wired" claims, per control

| control | mechanism | evidence | verdict |
| --- | --- | --- | --- |
| Vanilla / Enhanced | gates the two per-frame `adaptive_screens` reads | `frontend.cpp:1974/1980` | **OK** |
| Top-screen overlay | re-uploads the top framebuffer to a texture the overlay owns | draw log: 2 command lists | **OK** |
| Keyboard bindings | replaces `key_bit()` wholesale | startup self-check, 12/12 | **OK** |
| Controller bindings | replaces `controller_bit()` wholesale | same shape as above | **OK, untested at runtime** |
| Hotwire targets | ~~`HotWire_ResolveTarget` override~~ **removed** | misread `func_020872DC` | **WRONG, see docs/105** |
| Build limits | writes the allowance table | runtime log: class 1 = 77 vs shipped 20 | **OK** |
| Make All Builders The Same | copies 14 stat fields | see below | **OK and complete** |
| Lobby browse / host | HTTP against the master server | end-to-end round trip | **OK** |

"Unobserved" means installed and announced but never executed, because the code
path needs a match in progress and no headless driver reaches one.

## The builder patch, checked two ways

The 14 field offsets were transcribed from `tools/lb_blueprints.py`, so
transcription error was the obvious risk. All 14 were re-derived from the parser
and compared:

```
speed 0x0c/4  field_10 0x10/4  move_class 0x15/1  cost 0x5e/2
build_time 0x60/2  health 0x62/2  mana 0x64/2  melee_damage 0x68/1
melee_spread 0x6a/1  resource_yield 0x6c/1  attack_interval 0x6d/1
attack_range 0x6f/1  field_70 0x70/1  interaction_range 0x71/1
                                                    0 mismatches
```

**A stronger check was then available and is the one that matters.** Rather than
asking whether the copied fields are right, ask which fields actually *differ*
between the six builders — anything that differs and is not copied would be a
real bug:

```
cost              copied     45 / 50 / 55
health            copied     140 / 150 / 160
speed             copied     375 / 410 / 444
attack_interval   copied     24 / 30 / 40
speed_tiles_per_sec  derived from speed
index, name, faction, name_string_id, asset, offset   identity

uncovered differing fields: NONE
```

Four gameplay fields differ and all four are copied. The patch is complete, not
merely correct, and the ten other copied fields are identical across builders so
copying them is a no-op that costs nothing and guards against a future mod
changing them.

## A near-miss worth recording

`damage_min` and `damage_max` sit at **0x6C and 0x6E**, which collides with
`resource_yield` at 0x6C in the builder patch. They do not conflict: those
offsets belong to the **projectile** record type, and `lb_blueprints.py` parses
the three record types separately — units are 0x7C bytes, projectiles 0x74,
items 0x70. The builder patch only ever addresses unit records (indices 2, 22,
42, 62, 82, 102), so 0x6C there is unambiguously `resource_yield`.

Had the patch been written against a flat field list rather than the per-type
parser, this is exactly where it would have gone wrong.

## Settings that are honest about doing nothing

The remaining controls are badged and do not pretend:

* `[read, not wired]` — Balance Patch (`docs/101`), Fix Hotwired Healing
  (`docs/95`), Attack Boost Affects Ranged (`docs/95`), master-server address
  and name.
* `[unread]` — Hotwire Kills Transport Occupants, the unlock cheats, zoom.

The badge is not decoration. It is the difference between a port that is 42%
built and one that merely looks finished.
