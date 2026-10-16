#!/usr/bin/env python3
"""Parse `BP/Entities.ebp` — every unit, building, projectile and pickup.

The file is the serialised form of the `Battles::EntityBlueprint` family, and
its loader is `func_02001F3C` in the ARM9. That loader is the source for
everything asserted here; see `docs/11-the-blueprint-format.md` for the
disassembly and the reasoning.

    'BPNZ'                        4-byte magic
    record[551]                   variable length, tagged by a byte at +0x08
    string pool                   NUL-terminated, name and asset path per record

A record's length is decided by its type byte:

    0  Battles::UnitBlueprint             0x7C   182 records — units AND buildings
    1  Battles::ProjectileBlueprint       0x74    27 records
    2  Battles::CollectableItemBlueprint  0x70   342 records

The two string fields hold **offsets from the pool base**, which the loader
converts to pointers in place (`func_02001674`: `*field += pool_base`). Reading
the file rather than RAM, they are still offsets.

Usage:
    python tools/lb_blueprints.py out/eur/plain/BP/Entities.ebp
        [--lang out/eur/plain/LOC/UK_English.lng] [--tsv docs/data/entities.tsv]
        [--json out/eur/entities.json] [--type unit|projectile|item]
"""

import argparse
import json
import os
import struct
import sys

MAGIC = b"BPNZ"
RECORD_COUNT = 0x227          # the loop bound in func_02001F3C
SIZES = {0: 0x7C, 1: 0x74, 2: 0x70}
CLASSES = {
    0: "Battles::UnitBlueprint",
    1: "Battles::ProjectileBlueprint",
    2: "Battles::CollectableItemBlueprint",
}

# Category enum at +0x5C.
#
# The *grouping* was established from the roster: each of the six factions
# contributes exactly one member to categories 1, 4, 9, 13, 14 and 15, and the
# members of each are the same thing in six costumes (docs/12).
#
# The *names* are the developers' own, from `Sound/sound_data.sadl` — a C header
# the cartridge ships beside its sound archive, whose `BANK_UNIT__*` and
# `BANK_STRUCTURE__*` symbols name ten unit roles and ten structure roles,
# identically for all six factions (docs/16). They replace four names this
# project had inferred and got slightly wrong: CloseCombat is Melee, Special is
# Siege, LumberMill is Mill, and SpecialFactory is VehicleCenter.
#
# NOTE: this enum is still NOT the order of the game's own category *strings*
# (LANG 242..262). The permutation is in docs/12; the table performing it has
# not been located.
CATEGORY = {
    0: "Hero", 1: "Builder", 2: "Melee", 3: "Ranged", 4: "Mounted",
    5: "Transport", 6: "Siege", 7: "Base", 8: "Mill", 9: "Mine",
    10: "Farm", 11: "Barracks", 12: "VehicleCenter", 13: "Tower",
    14: "Tower2", 15: "Tower3", 16: "Shipyard", 17: "Bridge", 18: "Gate",
    19: "Wall", 255: "None",
}

FACTION = {
    "K": "King", "W": "Wizard", "P": "Pirate",
    "I": "Imperial", "E": "Earth", "A": "Alien",
}

# CollectableItemBlueprint category at +0x5C, read off the roster: each value
# holds one coherent kind and nothing else. See docs/22.
ITEM_CATEGORY = {
    0: "HealthPowerup", 1: "ManaPowerup", 2: "Hint", 3: "MissionObject",
    4: "ConceptArt", 5: "Map", 7: "RedBrick", 8: "Ticket", 9: "Minikit",
    10: "Stud", 11: "Minifig",
}

# The five categories whose records occupy the global unlock slot space.
UNLOCKABLE = {4, 5, 7, 9, 11}

#: What ``CollectableItemBlueprint+0x60`` points at, per category at +0x5C.
#: Established in docs/27; the five categories absent here read 0xFFFFFFFF.
ITEM_REF_KIND = {
    2: "loc_string_id",      # Hint
    4: "conceptart_table",   # ConceptArt -> 0x021269A4, stride 8
    5: "loc_string_id",      # Map (the map's display name)
    7: "redbrick_table",     # RedBrick -> 0x02125FF4, stride 16
    9: "blueprint_index",    # Minikit  -> the Minifig this act's set awards
    11: "blueprint_index",   # Minifig  -> the unit it depicts
}


NO_PROJECTILE = 0xFFFF


class BlueprintError(Exception):
    pass


def parse(data, lang=None):
    if data[:4] != MAGIC:
        raise BlueprintError("not a BPNZ file (magic %r)" % data[:4])

    # Walk the records first: the pool base is wherever the walk lands, and a
    # walk that does not land exactly on a NUL-terminated string is a walk that
    # went wrong. That equality is the format's own self-test.
    offsets = []
    p = 4
    for i in range(RECORD_COUNT):
        t = data[p + 8]
        if t not in SIZES:
            raise BlueprintError("record %d at 0x%x has unknown type %d"
                                 % (i, p, t))
        offsets.append((p, t))
        p += SIZES[t]
    pool = p

    def cstr(off):
        end = data.index(b"\x00", off)
        return data[off:end].decode("latin-1")

    out = []
    for i, (p, t) in enumerate(offsets):
        u8 = lambda o: data[p + o]                                  # noqa: E731
        u16 = lambda o: struct.unpack_from("<H", data, p + o)[0]    # noqa: E731
        u32 = lambda o: struct.unpack_from("<I", data, p + o)[0]    # noqa: E731

        rec = {
            "index": i,
            "offset": p,
            "type": t,
            "class": CLASSES[t],
            "size": SIZES[t],
            "name": cstr(pool + u32(0x00)),
            "asset": cstr(pool + u32(0x24)),
            # u16, NOT u8: the game reads this with ldrh (func_02002438,
            # func_020B83D4). It is the record's own index over 0..550, so a
            # u8 read silently returns index & 0xFF for the 369 records at
            # index >= 256. docs/11.
            "self_index": u16(0x04),
            "name_string_id": u16(0x06),
        }
        if lang and rec["name_string_id"] < len(lang):
            rec["display_name"] = lang[rec["name_string_id"]]

        if t == 0:
            proj = u16(0x66)
            rec.update({
                "category": CATEGORY.get(u8(0x5C), "?%d" % u8(0x5C)),
                "category_id": u8(0x5C),
                # Movement speed, as a u32 in 20.12 fixed point giving tiles
                # per tick. The whole 8-byte region 0x0C..0x13 is 0xFFFFFFFF
                # twice over for every one of the 92 structures, which is what
                # establishes the width -- see docs/26. Reading it as a u16 (as
                # this parser did through docs/11 and docs/12) yields the same
                # number for all 90 mobile units, because their high half is
                # always zero; only the sentinel differs.
                #
                # +0x15 is a five-value enum that is also zero for every
                # structure but is NOT a speed - see docs/11.
                "speed": u32(0x0C) if u32(0x0C) != 0xFFFFFFFF else 0,
                "speed_tiles_per_sec": (round(u32(0x0C) / 4096 * 30, 2)
                                        if u32(0x0C) != 0xFFFFFFFF else 0),
                # A rate limit in the same 20.12 fixed point: 12.0 for 81 of
                # the 90 mobile units, 0.3333 for nine, 0xFFFFFFFF for every
                # structure. Characterised but NOT named - nothing reads it
                # through the blueprint idiom. See docs/26.
                "field_10": u32(0x10) if u32(0x10) != 0xFFFFFFFF else None,
                "move_class": u8(0x15),
                "cost": u16(0x5E),
                # Build time. Both Sim::ConstructProgressAction and
                # Sim::RepairStructureAction read it, clamp it to at least 1,
                # halve it under a game-rule flag, and use it as a fixed-point
                # divisor (<< 12). See docs/25.
                "build_time": u16(0x60),
                "health": u16(0x62),
                "mana": u16(0x64),
                "projectile": None if proj == NO_PROJECTILE else proj,
                # Melee damage is a RANGE: minimum at +0x68, spread at +0x6A,
                # so the maximum is min + spread. Confirmed by the DPS identity
                # in docs/20 - average damage * 30 / interval - which reproduces
                # the community sheet's DPS column exactly on 19 of 22 units.
                "melee_damage": u8(0x68),
                "melee_damage_max": u8(0x68) + u8(0x6A) if u8(0x68) else 0,
                "melee_spread": u8(0x6A),
                # Resource yield. Sim::MagicInfluencedUnitStats::ResetToBase
                # seeds its +0x16 slot from this field, alongside melee damage
                # and attack interval, so it is a BUFFABLE stat. It is 25 for
                # exactly six units - K_Mine, W_Mine, P_Quarry, I_Quarry,
                # E_WellCap, A_WellCap, one resource building per faction - and
                # zero for the other 176. See docs/29.
                "resource_yield": u8(0x6C),
                # Attack interval in 1/30 s ticks - the simulation tick.
                "attack_interval": u8(0x6D),
                "attack_range": u8(0x6F),
                # +0x70 is accumulated into Entity+0x44 in the combat path;
                # characterised but not named. +0x71 is compared against a
                # distance and the action aborts when it is smaller, so it is
                # an interaction range. See docs/25.
                "field_70": u8(0x70),
                "interaction_range": u8(0x71),
            })
            rec["faction"] = FACTION.get(rec["name"].split("_")[0]
                                         if "_" in rec["name"] else "", "")
            rec["is_structure"] = rec["category_id"] >= 7 and u8(0x15) == 0

        elif t == 1:
            # Damage is a RANGE, not a number, and the record carries two
            # ranges. They are identical in all 27 cartridge records and in
            # every one the community patch edits, so whatever distinguishes
            # them is unobserved - see docs/20.
            rec.update({
                "damage_min": u8(0x6C),
                "damage_max": u8(0x6E),
                "damage_min_2": u8(0x70),
                "damage_max_2": u8(0x72),
                "effect_id": u8(0x5D),
                "flags": "".join(str(u8(o)) for o in range(0x5E, 0x6C)),
            })

        elif t == 2:
            rec.update({
                "item_category": ITEM_CATEGORY.get(u8(0x5C), "?%d" % u8(0x5C)),
                "item_category_id": u8(0x5C),
                # A global unlock slot. The 289 records in the five unlockable
                # categories take values 0..288, all distinct and contiguous;
                # everything else reads 0. Slot 0 IS a real slot (Map_01), so
                # the field is only meaningful for an unlockable category -
                # `x or None` silently loses it, which it did once.
                "unlock_slot": (u16(0x6C) if u8(0x5C) in UNLOCKABLE else None),
                # +0x60 is a POLYMORPHIC reference: a u32 whose meaning is
                # chosen by the category at +0x5C, with 0xFFFFFFFF for the five
                # categories that have no target. See docs/27.
                #
                #   Minifig    global blueprint index of the unit it depicts
                #   Minikit    global blueprint index of the Minifig awarded
                #              for completing that act's whole set
                #   Hint, Map  a LOC string id
                #   ConceptArt index into the {u32, char*} table at 0x021269A4
                #   RedBrick   index into the 16-byte table at 0x02125FF4,
                #              whose third word is the LOC id of its name
                "ref": (None if u32(0x60) == 0xFFFFFFFF else u32(0x60)),
                "ref_kind": ITEM_REF_KIND.get(u8(0x5C)),
            })
        out.append(rec)

    # Resolve the blueprint-index form of +0x60 to a name. This needs every
    # record parsed first, because a Minikit points forward at a Minifig.
    by_index = {r["index"]: r for r in out}
    for r in out:
        if r.get("ref_kind") == "blueprint_index" and r.get("ref") is not None:
            target = by_index.get(r["ref"])
            r["ref_name"] = target["name"] if target else None

    # Every pool entry should be reachable from exactly one record field.
    used = set()
    for r, (p, t) in zip(out, offsets):
        used.add(pool + struct.unpack_from("<I", data, p)[0])
        used.add(pool + struct.unpack_from("<I", data, p + 0x24)[0])
    entries = set()
    q = pool
    while q < len(data):
        entries.add(q)
        q = data.index(b"\x00", q) + 1

    stats = {
        "records": len(out),
        "pool_offset": pool,
        "pool_entries": len(entries),
        "pool_referenced": len(entries & used),
        "pool_unreferenced": len(entries - used),
        "by_type": {CLASSES[t]: sum(1 for _p, tt in offsets if tt == t)
                    for t in sorted(SIZES)},
    }
    return out, stats


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--lang")
    ap.add_argument("--tsv")
    ap.add_argument("--json")
    ap.add_argument("--type", choices=["unit", "projectile", "item"])
    args = ap.parse_args()

    lang = None
    if args.lang:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from lb_lang import load
        _header, lang = load(args.lang)

    with open(args.file, "rb") as fh:
        data = fh.read()
    recs, stats = parse(data, lang)

    print("records %d  pool at 0x%05x  entries %d  referenced %d  unreferenced %d"
          % (stats["records"], stats["pool_offset"], stats["pool_entries"],
             stats["pool_referenced"], stats["pool_unreferenced"]))
    for k, v in stats["by_type"].items():
        print("   %-40s %d" % (k, v))

    want = {"unit": 0, "projectile": 1, "item": 2}.get(args.type)
    shown = [r for r in recs if want is None or r["type"] == want]

    if args.type == "unit" or want == 0:
        print()
        print("%-24s %-10s %-14s %5s %6s %6s %5s %5s %5s %-16s"
              % ("name", "faction", "category", "cost", "health", "buildtm",
                 "speed", "dmg", "range", "projectile"))
        by_proj = {r["index"]: r["name"] for r in recs}
        for r in shown:
            print("%-24s %-10s %-14s %5d %6d %6d %5d %5d %5d %-16s"
                  % (r["name"], r["faction"], r["category"], r["cost"],
                     r["health"], r["build_time"], r["speed"], r["melee_damage"],
                     r["attack_range"],
                     by_proj.get(r["projectile"], "") if r["projectile"]
                     is not None else "-"))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        cols = ["index", "class", "name", "display_name", "faction", "category",
                "damage_min", "damage_max", "effect_id", "flags",
                "item_category", "unlock_slot", "ref", "ref_kind",
                "ref_name",
                "category_id", "is_structure", "cost", "health", "mana", "build_time",
                "melee_damage_max", "melee_spread",
                "speed", "speed_tiles_per_sec", "field_10",
                "move_class", "melee_damage", "attack_interval",
                "attack_range", "projectile", "field_6a",
                "resource_yield", "field_70",
                "interaction_range", "name_string_id", "asset", "offset"]
        by_name = {r["index"]: r["name"] for r in recs}
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("\t".join(cols) + "\n")
            for r in recs:
                row = []
                for c in cols:
                    v = r.get(c, "")
                    if c == "projectile" and isinstance(v, int):
                        v = by_name.get(v, v)
                    row.append("" if v is None else str(v))
                fh.write("\t".join(row) + "\n")
        print("wrote %s" % args.tsv)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"stats": stats, "records": recs}, fh, indent=1,
                      ensure_ascii=False)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
