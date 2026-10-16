#!/usr/bin/env python3
"""Build the definitive campaign table from the ARM9's RTTI.

`Battles::CampaignMissionBase` has exactly 90 subclasses — six campaigns of
fifteen missions — and each one's `.map` filename is stored in the read-only
data immediately **after** that class's typeinfo and vtable, which is to say
immediately *before* the next class's typeinfo. Getting that adjacency backwards
is the natural mistake and it shifts the whole table by one; the check that
catches it is that `CampaignMission1_CriminalMindsAct2` must pair with `ma2_1`,
mission 1 of act 2, and it does.

Output joins:
  * the internal class name, from RTTI;
  * the act and mission number, parsed out of the class name;
  * the `.map` file, from the adjacent string;
  * the displayed mission title, from `LOC/*.lng`;
  * the overlay id, where a source names one.

Usage:
    python tools/lb_campaign.py --rtti out/eur/rtti.json \\
        --image out/eur/arm9/section_static_02000000.bin \\
        [--lang out/eur/plain/LOC/UK_English.lng] \\
        [--overlay-map docs/data/overlay_missions.tsv] \\
        [--json out/eur/campaign.json]
"""

import argparse
import json
import os
import re
import struct
import sys

BASE = 0x02000000
MISSION_CLASS = re.compile(r"^Battles::CampaignMission(\d+)_(.+?)Act(\d+)$")
# Not anchored on a preceding NUL: these strings sit in a padded region and are
# as often preceded by alignment filler as by a terminator. Require instead that
# the byte before is not part of an identifier, so a match cannot be the tail of
# a longer name.
MAP_NAME = re.compile(rb"(?<![A-Za-z0-9_])((?:ck|cw|pi|pp|ma|mh|mp)\d_\d)\x00")

WORLD = {"c": "Castle", "p": "Pirates", "m": "Space"}
SIDE = {
    "ck": "King", "cw": "Wizard",
    "pi": "Imperial", "pp": "Pirate",
    "ma": "Alien", "mh": "Earth",
}


def load_lang(path):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from lb_lang import load
    _header, strings = load(path)
    return strings


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rtti", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--lang")
    ap.add_argument("--overlay-map",
                    help="TSV of overlay_id<TAB>map_name, from a prior source")
    ap.add_argument("--json")
    args = ap.parse_args()

    with open(args.image, "rb") as fh:
        img = fh.read()
    with open(args.rtti, encoding="utf-8") as fh:
        rtti = json.load(fh)

    tis = {t["address"]: t for t in rtti["typeinfos"]}
    base_named = lambda t, n: any(  # noqa: E731
        tis.get(b["typeinfo"], {}).get("name") == n for b in t["bases"])
    missions = sorted((t for t in rtti["typeinfos"]
                       if base_named(t, "Battles::CampaignMissionBase")),
                      key=lambda t: t["address"])
    print("CampaignMissionBase subclasses: %d" % len(missions))

    # Every map-name string in the image, by address.
    strings = sorted((BASE + m.start(1), m.group(1).decode())
                     for m in MAP_NAME.finditer(img))
    print("map-name strings in the image: %d" % len(strings))

    # The string belonging to class k lies between ti[k] and ti[k+1]; for the
    # last class, after its own record.
    rows = []
    for i, t in enumerate(missions):
        lo = t["address"]
        hi = missions[i + 1]["address"] if i + 1 < len(missions) else BASE + len(img)
        found = [s for a, s in strings if lo < a < hi]
        m = MISSION_CLASS.match(t["name"])
        rows.append({
            "typeinfo": t["address"],
            "class": t["name"],
            "short_name": m.group(2) if m else None,
            "mission": int(m.group(1)) if m else None,
            "act": int(m.group(3)) if m else None,
            "map": found[0] if len(found) == 1 else None,
            "map_candidates": found if len(found) != 1 else None,
            "vtable": (t.get("vtables") or [{}])[0].get("vtable"),
        })

    resolved = [r for r in rows if r["map"]]
    print("classes with exactly one adjacent map name: %d" % len(resolved))

    # Consistency check. The map filename encodes act_mission and so does the
    # class name, so the two derivations can be compared. They agree 82 times
    # out of 90; every disagreement is recorded on the row rather than being
    # silently resolved in favour of one of them.
    agree = []
    disagree = []
    for r in resolved:
        mm = re.match(r"^([a-z]{2})(\d)_(\d)$", r["map"])
        if not mm:
            continue
        if (int(mm.group(2)), int(mm.group(3))) == (r["act"], r["mission"]):
            r["corroborated"] = True
            agree.append(r)
        else:
            r["corroborated"] = False
            disagree.append(r)
    print("act/mission agreement between class name and map name: "
          "%d agree, %d disagree" % (len(agree), len(disagree)))
    for r in disagree:
        print("   %-7s %-58s class says act %d mission %d"
              % (r["map"], r["class"], r["act"], r["mission"]))

    for r in resolved:
        prefix = r["map"][:2]
        r["world"] = WORLD.get(prefix[0])
        r["side"] = SIDE.get(prefix)

    if args.lang:
        strings_l = load_lang(args.lang)
        # Titles are the first string of each mission's block; match by the
        # class's short name where the displayed title differs, which it often
        # does, so only an exact normalised match is accepted.
        norm = lambda s: re.sub(r"[^a-z0-9]", "", s.lower())  # noqa: E731
        index = {}
        for i, s in enumerate(strings_l):
            if s:
                index.setdefault(norm(s), i)
        hit = 0
        for r in rows:
            if r["short_name"] and norm(r["short_name"]) in index:
                r["title_id"] = index[norm(r["short_name"])]
                r["title"] = strings_l[r["title_id"]]
                hit += 1
        print("class short names that match a string verbatim: %d of %d"
              % (hit, len(rows)))

    if args.overlay_map:
        by_map = {}
        with open(args.overlay_map, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 2:
                    by_map[parts[1].strip()] = parts[0].strip()
        named = 0
        for r in rows:
            if r["map"] in by_map:
                r["overlay"] = by_map[r["map"]]
                named += 1
        print("overlay ids joined by map name: %d of %d" % (named, len(rows)))

    rows.sort(key=lambda r: (r["map"] or "zz"))
    width = max(len(r["class"]) for r in rows)
    print()
    print("%-8s %-6s %-8s %-3s %-3s  %s" % ("map", "ovl", "side", "act", "#",
                                            "class"))
    for r in rows:
        print("%-8s %-6s %-8s %-3s %-3s  %-*s  %s"
              % (r["map"] or "?", r.get("overlay", "?"), r.get("side", "?"),
                 r["act"], r["mission"], width, r["class"],
                 r.get("title", "")))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=1, ensure_ascii=False)
        print()
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
