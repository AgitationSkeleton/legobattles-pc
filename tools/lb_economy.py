#!/usr/bin/env python3
"""What buildings actually do: the unit allowance, read out of the ARM9.

`docs/53` closed with an open question and marked it open:

> `resource_yield` is 25 on the `MINE` and **0 on the `MILL` and the `FARM`**
> for every faction. Whatever makes a lumber mill or a farm produce is not this
> field. Flagged, not guessed.

The game answers it in its own words, and then the code gives the numbers.

## What the game says

`LOC/UK_English.lng` carries a contiguous block of tutorial tooltips, IDs 930
through 1034, three consecutive IDs per line of text. They describe every
building:

    FARM           "Farms increase the maximum number of Minifigures and
                    Special units you can create."
    MILL           "Mills increase LEGO Bricks gained from harvesting trees."
    MINE           "Mines can be placed on special locations to harvest
                    LEGO Bricks."
    BASE           "A Base produces Builders and Heroes, and also heals
                    nearby Hero units."
    BARRACKS       "A Barracks can produce soldiers."
    VEHICLECENTER  "A Special Factory produces powerful units."
    SHIPYARD       "A Shipyard produces waterborne Special Units."
    TOWER          "Towers defend an area and can be upgraded in later
                    missions."
    WALL           "Walls stop the enemy from closing in."
    BRIDGE         "Bridges can only be placed at specific locations."

So the Mill is a **multiplier on harvesting**, not a producer -- which is why
its `resource_yield` is zero -- and the Farm raises a **population cap**.

## What the code says

`func_02085E5C` is the Minifigure cap:

    bl   func_02085DB0        count the player's Farms
    lsl  r0, r0, #2           x 4
    add  r0, r0, #4           + 4

and `func_020863D8` caps that at the table entry for the category:

    cmp  r8, #1
    ...  min(table[1], farms * 4)      table[1] is 20

**Minifigure allowance = min(20, 4 + 4 x completed Farms).** Four farms reach
the cap of twenty; a fifth adds nothing.

`func_02085F74` is the same for Specials, and its caller `func_02071DC4` says
which categories those are:

    cmp   r1, #6
    cmpne r1, #5
    bne   ...                 anything else uses the Minifigure allowance

**"Special units" is categories 5 and 6 -- Transport and Siege.**

## Two rules that fall out of the counting function

`func_020859F4(player, category, live)` walks the player's entity handle list
(`player+0xF8`, count at `player+0xFC`, **two bytes per handle**) and contains
two identical 20-entry jump tables on `blueprint+0x5C`, the category:

    categories  0..6   ->  one arm
    categories  7..19  ->  the other

**That is the game's own unit/structure split, and it is at category 7.**
`lb_blueprints.py` already used `category_id >= 7` for `is_structure`; this is
where that rule comes from, read rather than inferred.

The structure arm then does:

    ldrb r0, [r7, #0x22e]
    cmp  r0, #0x64
    bne  skip

**A structure only counts once `entity+0x22E` reaches 100** -- construction
progress, as a percentage. A half-built Farm raises nobody's cap.

Category `0x14` (20) is the wildcard: `if (category != 0x14 && ...)` skips the
category filter entirely, so 20 means "any".

Usage:
    python tools/lb_economy.py
    python tools/lb_economy.py --tsv docs/data/economy.tsv
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

#: `ldr r2, [pc, #0x154]` in func_020863D8 resolves here. It is also the word
#: that terminated the particle table in `docs/55`: that walk stopped at slot 59
#: because 0x02126D94 holds 0x07041401, "out of family". It was out of family
#: because it is not part of that table at all -- it is this one.
ALLOWANCE_TABLE = 0x02126D94
ALLOWANCE_LEN = 20

#: `docs/68` CORRECTS `docs/58`: this table is indexed by **build-limit class**,
#: not by blueprint category. `func_020860A8` reads `table[class]` as the cap
#: and then dispatches through a 9-arm jump table, each arm counting a *set* of
#: categories. `docs/58` mapped the entries onto categories one-for-one and got
#: values that were not plausible caps -- and said so. This is why they were not.
LIMIT_CLASSES = {
    0: ("Heroes", "category 0"),
    1: ("Minifigures", "categories 1-4: Builder, Melee, Ranged, Mounted"),
    2: ("Specials", "categories 5-6: Transport, Siege"),
    3: ("Towers", "categories 0x0F, 0x0D and 0x0E summed"),
    4: ("unresolved", "a two-level jump table over categories 0x0D..0x13"),
    5: ("unused", "the arm returns 0 without counting"),
    6: ("Walls", "category 0x13"),
    7: ("Bridges", "category 0x11"),
    8: ("Gates", "category 0x12"),
}

#: The function that reads the table, and how many classes it dispatches on.
LIMIT_REMAINING = 0x020860A8
LIMIT_CLASS_COUNT = 9

#: `player[0xC5 + class]` -- the pending count subtracted alongside the live one.
PENDING_OFFSET = 0xC5

#: The three functions that compute an allowance, and the one that counts.
CAP_MINIFIGURE = 0x02085E5C
CAP_SPECIAL = 0x02085DB0
ALLOWANCE_SET = 0x020863D8
COUNT_BY_CATEGORY = 0x020859F4

#: Read off `func_02085E5C`: `lsl #2` then `add #4`.
FARM_STEP = 4
FARM_BASE = 4

#: Where the unit categories stop and the structure categories start, per the
#: jump tables in `func_020859F4`.
STRUCTURE_CATEGORY = 7

#: The wildcard the counting function accepts in place of a category.
ANY_CATEGORY = 0x14

#: A structure is only counted at this construction progress.
BUILT = 100

#: The categories `func_02071DC4` routes to the Special allowance.
SPECIAL_CATEGORIES = (5, 6)

#: The harvest and mining path, found the same way -- `docs/28` lists the red
#: bricks with their readers, and a cheat that modifies a system names that
#: system's code. `RedBrick_FastHarvest` -> func_020531F8,
#: `RedBrick_DoubleStuds` -> func_0206D0E0, `RedBrick_FastMining` ->
#: func_0206D7F0.
HARVEST_INTERVAL_FUNC = 0x020531F8
HARVEST_PAYOUT_FUNC = 0x0206D0E0
MINE_PAYOUT_FUNC = 0x0206D7F0
CREDIT_FUNC = 0x020866E0

#: `mov r0, #0x96` -- the harvest interval in simulation ticks. `docs/25` puts
#: the simulation at 30 Hz, so this is five seconds per chop.
HARVEST_TICKS = 0x96

#: `add r1, r4, #0x4b` in func_0206D0E0: the base tree payout.
HARVEST_BASE = 0x4B

#: `moveq r4, #0x19` in the same loop: what a completed Mill adds. The loop
#: BREAKS on the first completed Mill (`beq` past the loop), so it is a flat
#: bonus and a second Mill adds nothing.
MILL_BONUS = 0x19

#: The literal func_020866E0 clamps against: `player+0x90` is the brick
#: balance and it stops here.
BRICK_CAP_LITERAL = 0x0208670C
BRICK_BALANCE_OFFSET = 0x90

#: Building and training, read the same way. `RedBrick_FastProduction` names
#: `Battles::Sim::ConstructProgressAction::virtual_02` and
#: `Battles::Sim::RepairStructureAction::virtual_02` in `docs/28`.
CONSTRUCT_TICK = 0x020523A8       # ConstructProgressAction::virtual_02
REPAIR_TICK = 0x02054B84          # RepairStructureAction::virtual_02
CAN_PRODUCE = 0x02071DC4          # ProduceUnitEntityCommand, the precondition

#: `UnitBlueprint` offsets the build path reads, cited by the instructions
#: rather than inferred: cost is compared against the brick balance, build time
#: is the tick target, and health is the clamp construction grows toward.
OFF_COST = 0x5E
OFF_BUILD_TIME = 0x60
OFF_MAX_HEALTH = 0x62

#: Build time is floored here and forced to this by the cheats.
BUILD_TIME_FLOOR = 1

#: Two rules `docs/28` could only list as "other". Named from the code that
#: reads them: 0x13 forces the build time to one tick AND the cost to one,
#: 0x14 halves the build time (`lsr #1`).
RULE_INSTANT_BUILD = 0x13
RULE_HALF_BUILD = 0x14

#: Progress above this fires a one-shot event in the construction tick.
NEARLY_DONE = 0x62

#: On completion the built entity gets an extra virtual call if it is this
#: category -- Bridge.
BRIDGE_CATEGORY = 0x11

#: The tooltip block in `LOC/*.lng`, three consecutive IDs per line of text.
TOOLTIPS = {
    930: "BARRACKS", 933: "FARM", 972: "MILL", 975: "MINE", 978: "VEHICLECENTER",
    984: "TOWER", 987: "SHIPYARD", 993: "BRIDGE", 996: "BASE", 1005: "WALL",
    1029: "TRANSPORT", 1026: "HERO", 1023: "GOLDBRICK", 939: "QUEUE",
}


def allowance_table(space):
    """The 20-byte per-category allowance table."""
    raw = space.read(ALLOWANCE_TABLE, ALLOWANCE_LEN)
    return list(raw) if raw else []


def minifigure_cap(farms, table):
    """min(table[1], FARM_BASE + FARM_STEP * farms), as the ARM9 computes it."""
    return min(table[1] if len(table) > 1 else 255,
               FARM_BASE + FARM_STEP * farms)


def categories(entities):
    """{category id: (name, count, is_structure)} from the blueprint table."""
    out = {}
    seen = collections.defaultdict(collections.Counter)
    struct = collections.defaultdict(set)
    for r in entities:
        if r["class"] != "Battles::UnitBlueprint":
            continue
        cid = r.get("category_id")
        if cid in ("", None):
            continue
        cid = int(cid)
        seen[cid][r["category"] or "(blank)"] += 1
        struct[cid].add(r["is_structure"] == "True")
    for cid in seen:
        name = seen[cid].most_common(1)[0][0]
        out[cid] = (name, sum(seen[cid].values()), struct[cid])
    return out


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
    ap.add_argument("--lang", default=None,
                    help="a .lng file to read the tooltips from")
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    space = lb_code.AddressSpace(args.extract, None, "arm9")
    table = allowance_table(space)
    print(f"allowance table at 0x{ALLOWANCE_TABLE:08X}, {len(table)} bytes")
    print("   " + " ".join("%02X" % b for b in table))

    with open(args.entities, encoding="utf-8", newline="") as fh:
        entities = list(csv.DictReader(fh, delimiter=chr(9)))
    cats = categories(entities)

    print()
    print("build-limit classes (docs/68) -- how the table is REALLY indexed:")
    print("  %5s %-14s%6s  %s" % ("class", "name", "cap", "counts"))
    for lid in sorted(LIMIT_CLASSES):
        lname, what = LIMIT_CLASSES[lid]
        cap = table[lid] if lid < len(table) else "-"
        print("  %5d %-14s%6s  %s" % (lid, lname, cap, what))

    print()
    print("%4s  %-15s%11s%11s"
          % ("cat", "name", "blueprints", "kind"))
    for cid in sorted(cats):
        name, n, st = cats[cid]
        kind = ("unit" if cid < STRUCTURE_CATEGORY else "structure") \
            if cid <= 0x13 else "-"
        flag = "" if len(st) == 1 else "  MIXED is_structure"
        print("%4d  %-15s%11d%11s%s"
              % (cid, name, n, kind, flag))

    split_ok = all((cid >= STRUCTURE_CATEGORY) in cats[cid][2]
                   and len(cats[cid][2]) == 1
                   for cid in cats if cid <= 0x13)
    print()
    print(f"every category <= 0x13 is uniformly unit or structure, split at "
          f"{STRUCTURE_CATEGORY}: {split_ok}")
    print("   NOTE: lb_blueprints.py already defines is_structure as "
          "category_id >= 7,")
    print("   so this is not an independent check. What is new is the ARM9 "
          "jump table")
    print(f"   in func_0x{COUNT_BY_CATEGORY:08X} that the rule comes from.")

    print()
    print("Minifigure allowance = min(class-1 cap, "
          f"{FARM_BASE} + {FARM_STEP} x completed Farms)")
    print(f"{'farms':>7}{'allowance':>11}")
    for f in range(0, 7):
        print(f"{f:>7}{minifigure_cap(f, table):>11}")
    if len(table) > 1:
        print("   the cap is reached at %d farms"
              % -(-(table[1] - FARM_BASE) // FARM_STEP))
    print(f"Special units are categories "
          + " and ".join(f"{c} ({cats[c][0]})" for c in SPECIAL_CATEGORIES
                         if c in cats))
    print(f"A structure counts only at entity+0x22E == {BUILT}")
    print(f"Category 0x{ANY_CATEGORY:02X} is the wildcard 'any category'")

    cap = space.u32(BRICK_CAP_LITERAL)
    print()
    print("harvesting and mining:")
    print(f"   tree payout    = ({HARVEST_BASE} + {MILL_BONUS} if any "
          f"completed Mill) x entity[+0x18]")
    print(f"                    = {HARVEST_BASE} without a Mill, "
          f"{HARVEST_BASE + MILL_BONUS} with one -- and only the FIRST "
          f"completed Mill counts")
    print(f"   harvest every    {HARVEST_TICKS} ticks = "
          f"{HARVEST_TICKS / 30:.1f} s at 30 Hz (docs/25)")
    print(f"   mine payout    = entity[+0x16], seeded from blueprint+0x6C "
          f"(resource_yield)")
    print(f"   brick balance  = player+0x{BRICK_BALANCE_OFFSET:02X}, "
          f"capped at {cap:,}")
    print("   red bricks that double a payout: DoubleStuds on trees, "
          "FastMining on mines")

    print()
    print("building and training:")
    print(f"   cost         = blueprint+0x{OFF_COST:02X}, checked against "
          f"player+0x{BRICK_BALANCE_OFFSET:02X}")
    print(f"   build time   = blueprint+0x{OFF_BUILD_TIME:02X} ticks, "
          f"floored at {BUILD_TIME_FLOOR}")
    print(f"                  rule 0x{RULE_INSTANT_BUILD:02X} forces it to 1 "
          f"and the cost to 1; rule 0x{RULE_HALF_BUILD:02X} halves it")
    print(f"                  RedBrick_FastProduction forces it to 1 for the "
          f"local player")
    print(f"   health       grows during construction, clamped to "
          f"blueprint+0x{OFF_MAX_HEALTH:02X}")
    print(f"   progress > {NEARLY_DONE} fires a one-shot event; a structure is "
          f"only 'built' at {BUILT}")
    print(f"   on completion, category 0x{BRIDGE_CATEGORY:02X} (Bridge) gets "
          f"an extra virtual call")
    print()
    print("   ProduceUnitEntityCommand preconditions, in the order it checks:")
    for line in ("the producing structure exists",
                 f"its progress is {BUILT}",
                 "its owner exists",
                 "allowance: Specials for categories 5/6, Minifigures "
                 "otherwise, and category 0 (Hero) is exempt",
                 "a per-blueprint availability count is at least 1",
                 "the player can afford blueprint+0x%02X" % OFF_COST):
        print(f"      - {line}")

    if args.lang:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import lb_lang
        _h, strings = lb_lang.load(args.lang, "latin-1")
        print()
        print("what the game says each building does:")
        for sid, slot in sorted(TOOLTIPS.items(), key=lambda kv: kv[1]):
            t = strings[sid] if sid < len(strings) else None
            if t:
                print("   %-14s%s" % (slot, t))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["category_id", "name", "blueprints", "kind",
                               "limit_class_cap"]) + nl)
            for cid in sorted(cats):
                name, n, _st = cats[cid]
                kind = ("unit" if cid < STRUCTURE_CATEGORY else "structure") \
                    if cid <= 0x13 else ""
                # docs/68: the table is NOT indexed by category, so this
                # column is only meaningful for cid < 9, where it happens to
                # be the build-limit class of the same number.
                val = (str(table[cid]) if cid < LIMIT_CLASS_COUNT
                       and cid < len(table) else "")
                fh.write(tab.join([str(cid), name, str(n), kind, val]) + nl)
        print(f"\nwrote {args.tsv} ({len(cats)} categories)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
