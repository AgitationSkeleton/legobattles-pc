#!/usr/bin/env python3
"""Which blueprint fields does each balance mod change, and does the Balance menu cover them?

    python tools/lb_balcover.py [rom.nds ...]       (default: the mods lb_genbalance.py bundles, plus FunLite2)

Each mod's BP/Entities.ebp is diffed against vanilla's, record by record (182 unit/building records of 0x7C
bytes, then 27 projectiles of 0x74: lb_balance.cpp's layout). Every changed byte is attributed to a field of
the menu (COVERED, below, mirrors kUnitFields/kAttackFields) or listed as UNCOVERED with how many records
change it and an example. docs/426.
"""
from __future__ import annotations

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lb_genbalance  # noqa: E402

UNIT_RECORDS, ATTACK_RECORDS, FIRST, UNIT_SIZE, ATTACK_SIZE = 182, 27, 4, 0x7C, 0x74
# (offset, width, name) -- the menu's fields (lb_balance.cpp kUnitFields / kAttackFields).
UNIT = [(0x5E, 2, "Cost"), (0x60, 2, "Build time"), (0x62, 2, "Health"), (0x64, 2, "Mana"),
        (0x0C, 4, "Speed"), (0x68, 2, "Melee damage"), (0x6A, 2, "Melee spread"),
        (0x6D, 1, "Attack interval"), (0x6F, 1, "Attack range"), (0x6C, 1, "Resource yield"),
        (0x72, 1, "Spell 1"), (0x73, 1, "Spell 2"), (0x74, 1, "Spell 3"), (0x75, 1, "Spell 4"), (0x76, 1, "Spell 5"),
        # docs/426: appended fields (Traits section)
        (0x17, 1, "Land"), (0x18, 1, "Water"), (0x19, 1, "Trees"), (0x1A, 1, "Building collision"),
        (0x1B, 1, "Land<->water"), (0x1D, 1, "Hitbox size"), (0x21, 1, "Uninteractable"),
        (0x66, 2, "Projectile"), (0x70, 1, "AI priority"), (0x71, 1, "Defogged area"),
        (0x16, 1, "Water-only")]
ATTACK = [(0x6C, 2, "Hit damage min"), (0x6E, 2, "Hit damage max"), (0x70, 2, "Impact damage min"),
          (0x72, 2, "Impact damage max"), (0x0C, 4, "Speed"), (0x5E, 1, "Hits buildings"),
          (0x5F, 1, "Hits flying"), (0x60, 1, "Hits ground"), (0x61, 1, "Hits walls"), (0x62, 1, "Hurts allies"),
          (0x63, 1, "Hurts on the way"), (0x64, 1, "Stopped by cliffs"), (0x66, 1, "Stopped by buildings"),
          (0x67, 1, "Stopped by allies"), (0x68, 1, "Stopped by enemies"), (0x69, 1, "Stopped by walls"),
          (0x6B, 1, "Explodes"), (0x65, 1, "Over trees")]


def field(table, off):
    for o, w, name in table:
        if o <= off < o + w:
            return name
    return None


def records(bp):
    out = []
    for r in range(UNIT_RECORDS):
        a = FIRST + r * UNIT_SIZE
        out.append(("unit", r, bp[a:a + UNIT_SIZE]))
    base = FIRST + UNIT_RECORDS * UNIT_SIZE
    for r in range(ATTACK_RECORDS):
        a = base + r * ATTACK_SIZE
        out.append(("attack", UNIT_RECORDS + r, bp[a:a + ATTACK_SIZE]))
    return out, base + ATTACK_RECORDS * ATTACK_SIZE


def name_of(rec: bytes) -> str:
    return rec[0x10:0x30].split(b"\0")[0].decode("latin-1", "replace") or "?"


def main() -> int:
    mods = lb_genbalance.MODS_DIR
    roms = sys.argv[1:] or [os.path.join(mods, m[1]) for m in lb_genbalance.MODS]
    van = lb_genbalance.blueprint_from_rom(os.path.join(mods, lb_genbalance.BASE))
    vrec, vend = records(van)
    for path in roms:
        bp = lb_genbalance.blueprint_from_rom(path)
        mrec, mend = records(bp)
        covered = collections.Counter()
        uncovered = collections.defaultdict(list)
        for (kind, r, v), (_, _, m) in zip(vrec, mrec):
            table = UNIT if kind == "unit" else ATTACK
            seen = set()
            for off in range(len(v)):
                if v[off] == m[off]:
                    continue
                f = field(table, off)
                key = (kind, f or f"+0x{off:02X}")
                if key in seen:
                    continue
                seen.add(key)
                if f:
                    covered[key] += 1
                else:
                    uncovered[key].append((r, name_of(v), v[off], m[off]))
        tail = sum(1 for a, b in zip(van[vend:], bp[mend:]) if a != b) + abs(len(van) - len(bp))
        print(f"== {os.path.basename(path)}: blueprint {len(bp)} bytes (vanilla {len(van)}); "
              f"{tail} byte(s) differ past the projectiles")
        for (kind, f), n in sorted(covered.items()):
            print(f"   covered    {kind:6} {f:22} {n:3} record(s)")
        for (kind, f), ex in sorted(uncovered.items()):
            r, nm, a, b = ex[0]
            print(f"   UNCOVERED  {kind:6} {f:22} {len(ex):3} record(s)  e.g. #{r} {nm}: {a:#04x} -> {b:#04x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
