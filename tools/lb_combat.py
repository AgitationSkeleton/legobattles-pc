#!/usr/bin/env python3
"""How a hit resolves: the damage formula, and the flag that makes it splash.

`docs/20` established the damage *fields* and left two things open:

> `ProjectileBlueprint` also carries **a second damage pair** at `+0x70`/`+0x72`
> that is byte-identical to the first in all 27 cartridge records [...] Whatever
> distinguishes the two is unobserved, so it is recorded rather than named.

and the 14 boolean flags at `+0x5E`-`+0x6B`. Both are answered by reading the
impact path, which `docs/28`'s red-brick table points straight at:
`RedBrick_OneHitKill` and `RedBrick_InvincibleHero` both list `func_0206E970`.

## The formula

`func_0206E970(projectile, target, dmgMin, dmgMax)`:

    span   = dmgMax - dmgMin
    roll   = dmgMin + (span > 0 ? rand(0, span) : 0)
    scaled = fixmul(roll << 12, target[+0x174])       20.12
    dmg    = scaled >> 12
    newHP  = hp > dmg ? hp - dmg : 0                  hp is u16 at target+0x1A0
    SetHealth(target, newHP)

`target[+0x174]` is a 20.12 multiplier initialised to `0x1000` -- 1.0 -- by
`func_0205AE90`, which tier L (`docs/57`) places in `Sim::Entity`'s translation
unit. An unbuffed unit therefore takes exactly the roll.

It scales damage **upward** as it grows, so it is a damage-taken multiplier
rather than an armour value. Which of `docs/24`'s buffs write it is **not
established here** and is not guessed at.

**`func_0204F2D8` is this game's 20.12 multiply and it rounds to nearest:**

    smull ip, r2, r3, r2
    adds  r3, ip, #0x800        + 0.5
    lsr   r3, r3, #0xc

A 64-bit `smull` and a `+0x800` before the shift. Anything reimplementing this
with a truncating shift will drift from the original by up to one point of
damage per hit.

## The second damage pair, answered

Two functions call the applier with two different pairs:

    func_0206E408   ldrh r2,[bp,#0x6c] ; ldrh r3,[bp,#0x6e]   the direct hit
    func_0206E450   ldrh r2,[bp,#0x70] ; ldrh r3,[bp,#0x72]   on impact
    func_0206E60C   ldrh r3,[bp,#0x70] ; ldrh sb,[bp,#0x72]   in the blast

**The data cannot show the difference and the code can.** All 27 records carry
the two pairs equal, which is exactly why `docs/20` could not name them; the
distinction is which code path reads which.

## `+0x6B` is "explodes"

`func_0206E450` branches on it:

    ldrb r1, [bp, #0x6b]
    cmp  r1, #0
    beq  single_target        otherwise -> func_0206E60C, the area path

The flag was read from the branch **before** the data was looked at. It is set
on 11 of the 27 projectiles and they are exactly the ones that should explode:
`Boulder`, `OgreBoulder`, `TBoulder`, `Fireball`, `TFireball`, `AirFireball`,
`ICannonBall`, `PCannonBall`, `PlasmaBall`, `LaserCannon`, `Gift`. Clear on all
16 arrows, bolts, shots and lasers.

## The blast

`func_0206E60C` sweeps the tiles around the impact in **three rounds**, and for
each entity it finds scales the damage by a distance falloff before applying it,
so a splash weapon does not hit everything for full. `RedBrick_SuperExplosions`
doubles the result, clamped to 65,535.

## The two hero cheats are asymmetric, and both are local-player only

    RedBrick_OneHitKill      attacker is category 0 (Hero) and the local player
                             -> resulting health := 0
    RedBrick_InvincibleHero  TARGET is category 0 and the local player
                             -> resulting health := current health

Both overwrite the **resulting health**, not the damage, which is why they are
exact rather than "a very large number". The category test is why the cheat is
named "One Hit Kill (Heroes)": it does nothing for a Guardsman.

Usage:
    python tools/lb_combat.py
    python tools/lb_combat.py --tsv docs/data/projectiles.tsv
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

EBP = "BP/Entities.ebp"
RECORD_SIZES = {0: 0x7C, 1: 0x74, 2: 0x70}
PROJECTILE_TAG = 1
RECORDS = 551

#: `ProjectileBlueprint` offsets read in this pass.
OFF_EXPLODES = 0x6B
OFF_DAMAGE_A = 0x6C      # passed by func_0206E408, the direct hit
OFF_DAMAGE_B = 0x70      # passed by func_0206E450 and func_0206E60C

#: The impact path, from `docs/28`'s red-brick readers.
APPLY_DAMAGE = 0x0206E970
DIRECT_HIT = 0x0206E408
ON_IMPACT = 0x0206E450
AREA_DAMAGE = 0x0206E60C
FIXED_MUL = 0x0204F2D8
SCALE_INIT = 0x0205AE90

#: 20.12 again (`docs/25`, `docs/51`, `docs/58`), and the rounding constant the
#: multiply adds before shifting: 0x800 is 0.5.
FIXED = 12
FIXED_ONE = 1 << FIXED
FIXED_HALF = FIXED_ONE // 2

#: Entity offsets. Health is a u16 reached as `[entity+0x100] + 0xA0`.
OFF_HEALTH = 0x1A0
OFF_DAMAGE_SCALE = 0x174

#: The clamp `RedBrick_SuperExplosions` doubles up to.
DAMAGE_CLAMP = 0xFFFF

#: `ProjectileBlueprint+0x5D` is an animation-set id (`docs/66`). It indexes a
#: table of 18-byte records -- a frame count then up to eight halfword frame
#: ids -- reached through func_0200CE1C, whose `mul r2, r0, #0x12` states the
#: stride. Set 0 is empty, and func_0203AB00 skips drawing entirely when the id
#: is zero, so 0 means "no sprite".
OFF_ANIM_SET = 0x5D
ANIM_TABLE = 0x02127398
ANIM_STRIDE = 0x12
ANIM_MAX_FRAMES = 8
ANIM_ACCESSOR = 0x0200CE1C
ANIM_DRAW_GATE = 0x0203AB00


#: `UnitBlueprint+0x70` is the target-priority score (`docs/67`). The four
#: combat commands walk the candidate targets keeping the highest, and copy that
#: candidate's tile into the best-target slot -- so this is the quantity being
#: maximised, not a range or a damage term.
OFF_THREAT = 0x70
THREAT_READERS = (0x020631D4, 0x020638C8, 0x020651D0, 0x02065974,
                  0x02066DD0, 0x0206741C, 0x0206FF04, 0x020709BC)
UNIT_TAG = 0


def unit_blueprints(path):
    """[(index, record bytes)] for every UnitBlueprint."""
    with open(path, "rb") as fh:
        data = fh.read()
    off = 4
    out = []
    for i in range(RECORDS):
        tag = data[off + 8]
        size = RECORD_SIZES[tag]
        if tag == UNIT_TAG:
            out.append((i, data[off:off + size]))
        off += size
    return out


def anim_set(space, ident):
    """(frame count, [frame ids]) for one animation set."""
    rec = space.read(ANIM_TABLE + ident * ANIM_STRIDE, ANIM_STRIDE)
    if not rec:
        return 0, []
    n = rec[0]
    frames = struct.unpack_from("<%dH" % ANIM_MAX_FRAMES, rec, 2)
    return n, list(frames[:min(n, ANIM_MAX_FRAMES)])


def projectiles(path):
    """[(blueprint index, record bytes)] for every ProjectileBlueprint."""
    with open(path, "rb") as fh:
        data = fh.read()
    off = 4
    out = []
    for i in range(RECORDS):
        tag = data[off + 8]
        size = RECORD_SIZES[tag]
        if tag == PROJECTILE_TAG:
            out.append((i, data[off:off + size]))
        off += size
    return out


def pairs(rec):
    """((minA, maxA), (minB, maxB)) -- the two damage ranges."""
    return (struct.unpack_from("<HH", rec, OFF_DAMAGE_A),
            struct.unpack_from("<HH", rec, OFF_DAMAGE_B))


def fixmul(a, b):
    """The game's 20.12 multiply, rounding to nearest as func_0204F2D8 does."""
    return (a * b + FIXED_HALF) >> FIXED


def resolve(dmg_min, dmg_max, roll, scale=FIXED_ONE):
    """Damage for a given roll in [0, max-min], as func_0206E970 computes it."""
    span = dmg_max - dmg_min
    value = dmg_min + (roll if span > 0 else 0)
    return fixmul(value << FIXED, scale) >> FIXED


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--entities", default=os.path.join("docs", "data",
                                                       "entities.tsv"))
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    path = os.path.join(args.extract, "plain", EBP.replace("/", os.sep))
    recs = projectiles(path)
    names = {}
    if os.path.exists(args.entities):
        with open(args.entities, encoding="utf-8", newline="") as fh:
            for row in csv.DictReader(fh, delimiter=chr(9)):
                names[int(row["index"])] = row["name"]

    print(f"{len(recs)} ProjectileBlueprints")
    print()
    print("%-20s%9s%14s%14s%7s" % ("name", "explodes", "damage +0x6C",
                                   "damage +0x70", "same"))
    same = 0
    explode = []
    direct = []
    for i, rec in recs:
        a, b = pairs(rec)
        flag = rec[OFF_EXPLODES]
        same += a == b
        (explode if flag else direct).append(names.get(i, "?"))
        print("%-20s%9d%14s%14s%7s"
              % (names.get(i, f"#{i}"), flag, f"{a[0]}-{a[1]}",
                 f"{b[0]}-{b[1]}", a == b))

    space0 = lb_code.AddressSpace(args.extract, None, "arm9")
    sets = collections.defaultdict(list)
    for i, rec in recs:
        sets[rec[OFF_ANIM_SET]].append(names.get(i, "?"))
    print()
    print("animation sets (+0x%02X), from the table at 0x%08X:"
          % (OFF_ANIM_SET, ANIM_TABLE))
    for ident in sorted(sets):
        n, frames = anim_set(space0, ident)
        print("  %3d  %d frames %-34s %s"
              % (ident, n, str(frames), ", ".join(sets[ident])))

    print()
    print(f"the two damage pairs are equal in {same} of {len(recs)} records "
          f"-- which is why docs/20 could not tell them apart")
    print(f"+0x{OFF_EXPLODES:02X} set: {len(explode)} -- "
          + ", ".join(explode))
    print(f"+0x{OFF_EXPLODES:02X} clear: {len(direct)} -- "
          + ", ".join(direct))

    space = lb_code.AddressSpace(args.extract, None, "arm9")
    arm, _thumb = lb_code.make_engines()
    ops = [(i.mnemonic, i.op_str)
           for i in arm.disasm(space.read(FIXED_MUL, 40), FIXED_MUL)]
    has = {m for m, _ in ops}
    rounds = any(m == "adds" and o.endswith("#0x800") for m, o in ops)
    print()
    print(f"func_0x{FIXED_MUL:08X} is the 20.12 multiply: "
          f"smull {'yes' if 'smull' in has else 'NO'}, "
          f"rounds by +0x800 {'yes' if rounds else 'NO'}")
    print(f"   fixmul(3 << 12, 0x1800) = {fixmul(3 << FIXED, 0x1800)} "
          f"(3 x 1.5 = 4.5 in 20.12); the conversion back to an integer then "
          f"truncates to {fixmul(3 << FIXED, 0x1800) >> FIXED}")

    print()
    print("a hit, worked through:")
    for nm, lo, hi, scale in (("Arrow", 15, 20, FIXED_ONE),
                              ("Arrow, scale 1.5", 15, 20, 0x1800),
                              ("Fireball", 50, 60, FIXED_ONE)):
        vals = [resolve(lo, hi, r, scale) for r in range(hi - lo + 1)]
        print(f"   {nm:<22}{lo}-{hi} -> {vals}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["index", "name", "explodes", "min_a", "max_a",
                               "min_b", "max_b", "pairs_equal"]) + nl)
            for i, rec in recs:
                a, b = pairs(rec)
                fh.write(tab.join([str(i), names.get(i, ""),
                                   str(rec[OFF_EXPLODES]), str(a[0]),
                                   str(a[1]), str(b[0]), str(b[1]),
                                   str(a == b)]) + nl)
        print(f"\nwrote {args.tsv} ({len(recs)} projectiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
