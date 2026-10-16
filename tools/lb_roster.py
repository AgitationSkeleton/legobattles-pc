#!/usr/bin/env python3
"""Join the faction roster to the sound slots, and print what each entity does.

Two files describe the same twenty things per faction, and neither one knows
about the other:

* `BP/Entities.ebp` -- the simulation's blueprints, read by `lb_blueprints.py`.
  120 of its 551 records carry a faction, exactly 20 per faction (`docs/12`).
* `Sound/sound_data.sadl` -- the shipped C header, read by `lb_sound_symbols.py`.
  It declares `BANK_UNIT__<FACTION>_<SLOT>` and `BANK_STRUCTURE__<FACTION>_<SLOT>`,
  ten slots each, for the same six factions (`docs/16`, `docs/52`).

**The two agree cell for cell.** Take a faction's twenty blueprints in index
order and lay them against the header's ten unit slots followed by its ten
structure slots, and the blueprint's own `category` field names the slot, in
order, 120 times out of 120. Nothing in either file points at the other; the
agreement is the check, and it is what licenses using the header's vocabulary
for the blueprint rows.

Two slots need a rule, and the rule is the same in both files: `HEROM`/`HEROF`
are the two `Hero` rows in order, and `SIEGEONE`/`SIEGETWO`/`SIEGETHREE` are the
three `Siege` rows in order.

Usage:
    python tools/lb_roster.py
    python tools/lb_roster.py --tsv docs/data/faction_roster.tsv
    python tools/lb_roster.py --faction King
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_sound_symbols as lb_sound  # noqa: E402

#: The twenty slots a faction has, in the order both files list them.
SLOTS = ([(s, "unit") for s in lb_sound.UNIT_ROLES]
         + [(s, "structure") for s in lb_sound.STRUCTURE_SLOTS])

#: The blueprint `category` each slot must carry. Read off the roster, then
#: checked against all 120 cells rather than assumed.
SLOT_CATEGORY = {
    "HEROM": "Hero", "HEROF": "Hero", "BUILDER": "Builder", "MELEE": "Melee",
    "RANGED": "Ranged", "MOUNTED": "Mounted", "SIEGEONE": "Siege",
    "SIEGETWO": "Siege", "SIEGETHREE": "Siege", "TRANSPORT": "Transport",
    "BASE": "Base", "MILL": "Mill", "MINE": "Mine", "FARM": "Farm",
    "BARRACKS": "Barracks", "VEHICLECENTER": "VehicleCenter",
    "TOWER": "Tower", "TOWER2": "Tower2", "TOWER3": "Tower3",
    "SHIPYARD": "Shipyard",
}

#: `Entities.ebp` spells the factions in mixed case; the header shouts them.
FACTION_KEY = {f.capitalize(): f for f in lb_sound.FACTIONS}
FACTION_KEY["King"] = "KING"

#: Columns carried through from the blueprint, in the order they are printed.
STATS = ("cost", "health", "mana", "build_time", "melee_damage",
         "attack_range", "attack_interval", "speed_tiles_per_sec",
         "resource_yield", "projectile")


def load_roster(path):
    """{faction: [row, ...]} in blueprint index order, factions only."""
    by_faction = collections.defaultdict(list)
    with open(path, encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh, delimiter=chr(9)):
            if row.get("faction"):
                by_faction[row["faction"]].append(row)
    for rows in by_faction.values():
        rows.sort(key=lambda r: int(r["index"]))
    return by_faction


def pair(by_faction):
    """[(faction, slot, kind, row, category_matches)] over all 120 cells."""
    out = []
    for faction in sorted(by_faction):
        rows = by_faction[faction]
        for (slot, kind), row in zip(SLOTS, rows):
            ok = row.get("category") == SLOT_CATEGORY[slot]
            out.append((faction, slot, kind, row, ok))
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
    ap.add_argument("--entities", default=os.path.join("docs", "data",
                                                       "entities.tsv"))
    ap.add_argument("--tsv")
    ap.add_argument("--faction")
    args = ap.parse_args(argv)

    if not os.path.exists(args.entities):
        print(f"missing {args.entities}; run lb_blueprints.py --tsv first",
              file=sys.stderr)
        return 2
    by_faction = load_roster(args.entities)
    print(f"{len(by_faction)} factions, "
          + ", ".join(f"{f} {len(r)}" for f, r in sorted(by_faction.items())))

    cells = pair(by_faction)
    good = sum(1 for c in cells if c[4])
    print(f"slot/category agreement: {good} of {len(cells)} cells")
    for faction, slot, _k, row, ok in cells:
        if not ok:
            print(f"   {faction:<10}{slot:<16}blueprint says "
                  f"{row.get('category')!r}")

    wrong_kind = [(f, s, r) for f, s, k, r, _o in cells
                  if (r.get("is_structure") == "True") != (k == "structure")]
    print(f"is_structure agrees with the slot's kind: "
          f"{len(cells) - len(wrong_kind)} of {len(cells)}")
    for f, s, r in wrong_kind[:6]:
        print(f"   {f:<10}{s:<16}is_structure={r.get('is_structure')}")

    show = args.faction or "King"
    rows = [c for c in cells if c[0] == show]
    if rows:
        print()
        print(f"{show}:")
        head = f"{'slot':<15}{'blueprint':<20}{'name':<16}"
        head += "".join(f"{s[:9]:>10}" for s in STATS[:6])
        print(head)
        for _f, slot, _k, r, _o in rows:
            line = f"{slot:<15}{r['name']:<20}{r['display_name'][:15]:<16}"
            line += "".join(f"{(r.get(s) or ''):>10}" for s in STATS[:6])
            print(line)

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        cols = (["faction", "slot", "kind", "bank_symbol", "blueprint",
                 "display_name", "category", "is_structure"] + list(STATS)
                + ["asset", "unlock_slot"])
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(cols) + nl)
            for faction, slot, kind, r, _ok in cells:
                bank = (f"BANK_{'UNIT' if kind == 'unit' else 'STRUCTURE'}"
                        f"__{FACTION_KEY.get(faction, faction.upper())}_{slot}")
                vals = [faction, slot, kind, bank, r["name"],
                        r["display_name"], r.get("category", ""),
                        r.get("is_structure", "")]
                vals += [r.get(s) or "" for s in STATS]
                vals += [r.get("asset") or "", r.get("unlock_slot") or ""]
                fh.write(tab.join(vals) + nl)
        print(f"\nwrote {args.tsv} ({len(cells)} rows)")

    return 0 if good == len(cells) else 1


if __name__ == "__main__":
    raise SystemExit(main())
