#!/usr/bin/env python3
"""Parse `BP/Factions.fbp` — the roster of every faction, and every variant.

Derived from the ARM9 loader exactly as `Entities.ebp` was (`docs/11`):
`func_020025D4` requests the file, `func_02002644` dispatches on the completion
code, `func_0200268C` stores the buffer, and `func_02002974` is the loop:

    r5 = 0; r6 = 0
    loop:  r0 = buffer + 4 + r6;  func_020029AC(r0)
           r5 += 1;  r6 += 0x40
           if r5 < 0x82 goto loop

so **130 records of 0x40 bytes**, starting past a 4-byte magic. The per-record
fixup resolves the name the same way as the entity file, and names its own pool
base as a literal:

    func_020029B8:  *(u32*)rec += buffer + 0x2084

`0x2084` is `4 + 130 * 0x40` exactly, so the string pool begins immediately after
the last record — a satisfying confirmation that the count and the stride are
both right.

    0x00  u32       name offset, from the pool at 0x2084
    0x04  u8        variant index
    0x05  u8        base faction — a record index in this same file, 124..129
    0x06  u16       display-name string id in LOC/*.lng
    0x08  u32       unknown, zero throughout
    0x0C  u16[26]   entity blueprint index per role slot, 0xFFFF = unavailable

The 26 slots are a fixed role template, identical in every faction; a variant
record is the same template with entries removed. That is how the campaign
gates a faction's tech: `KCM03_User_LockedOut` is the King with all but three
slots set to `0xFFFF`.

Usage:
    python tools/lb_factions.py out/eur/plain/BP/Factions.fbp
        [--entities out/eur/entities.json] [--lang out/eur/plain/LOC/UK_English.lng]
        [--tsv docs/data/factions.tsv] [--show King]
"""

import argparse
import json
import os
import struct
import sys

MAGIC = b"FANZ"
RECORD_COUNT = 0x82
RECORD_SIZE = 0x40
SLOT_COUNT = 26
SLOT_BASE = 0x0C
NONE = 0xFFFF

# Read off the base factions, all six of which fill the template identically.
# The role names are the developers' own, from Sound/sound_data.sadl (docs/16);
# the slot ORDER is this file's, and the two agree on every role they share.
SLOT_ROLE = [
    "Hero", "Builder", "Melee", "Ranged", "Mounted",
    "SiegeOne", "SiegeTwo", "SiegeThree", "Transport",
    "Base", "Mill", "Mine", "Farm", "Barracks", "VehicleCenter",
    "Shipyard", "Tower", "Tower2", "Tower3", "Wall",
    "BridgeSmallH", "BridgeSmallV", "BridgeMediumH", "BridgeMediumV",
    "BridgeLargeH", "BridgeLargeV",
]


class FactionError(Exception):
    pass


def parse(data, entities=None, lang=None):
    if data[:4] != MAGIC:
        raise FactionError("not a FANZ file (magic %r)" % data[:4])
    pool = 4 + RECORD_COUNT * RECORD_SIZE
    if pool > len(data):
        raise FactionError("130 records of 0x40 do not fit in %d bytes" % len(data))

    def cstr(off):
        end = data.index(b"\x00", off)
        return data[off:end].decode("latin-1")

    recs = []
    for i in range(RECORD_COUNT):
        p = 4 + i * RECORD_SIZE
        slots = struct.unpack_from("<%dH" % SLOT_COUNT, data, p + SLOT_BASE)
        rec = {
            "index": i,
            "name": cstr(pool + struct.unpack_from("<I", data, p)[0]),
            "variant": data[p + 4],
            "base": data[p + 5],
            "name_string_id": struct.unpack_from("<H", data, p + 6)[0],
            "slots": [None if s == NONE else s for s in slots],
        }
        if lang and rec["name_string_id"] < len(lang):
            rec["display_name"] = lang[rec["name_string_id"]]
        rec["available"] = sum(1 for s in rec["slots"] if s is not None)
        recs.append(rec)

    for r in recs:
        r["base_name"] = recs[r["base"]]["name"] if r["base"] < len(recs) else "?"
        if entities:
            r["slot_names"] = [entities.get(s, {}).get("name") if s is not None
                               else None for s in r["slots"]]
    return recs, {"pool_offset": pool, "pool_bytes": len(data) - pool,
                  "records": len(recs)}


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--entities", help="out/eur/entities.json, for slot names")
    ap.add_argument("--lang")
    ap.add_argument("--tsv")
    ap.add_argument("--show", help="print one faction's full roster by name")
    args = ap.parse_args()

    entities = None
    if args.entities:
        with open(args.entities, encoding="utf-8") as fh:
            entities = {r["index"]: r for r in json.load(fh)["records"]}
    lang = None
    if args.lang:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from lb_lang import load
        _header, lang = load(args.lang)

    with open(args.file, "rb") as fh:
        data = fh.read()
    recs, stats = parse(data, entities, lang)
    print("records %d  pool at 0x%04x  pool bytes %d"
          % (stats["records"], stats["pool_offset"], stats["pool_bytes"]))

    bases = sorted({r["base"] for r in recs})
    print()
    print("base factions (the records every variant points at):")
    for b in bases:
        r = recs[b]
        print("   %3d  %-10s %-10s %d of %d slots filled"
              % (b, r["name"], r.get("display_name", ""), r["available"],
                 SLOT_COUNT))

    if args.show:
        match = [r for r in recs if r["name"] == args.show]
        if not match:
            sys.exit("no faction named %r" % args.show)
        r = match[0]
        print()
        print("=== %s (record %d, base %s) ===" % (r["name"], r["index"],
                                                   r["base_name"]))
        for i, s in enumerate(r["slots"]):
            name = (entities.get(s, {}).get("name", "?") if entities and s
                    is not None else ("---" if s is None else str(s)))
            print("   slot %2d  %-16s %s" % (i, SLOT_ROLE[i], name))

    print()
    print("variants with the fewest slots (the campaign's tech gates):")
    for r in sorted(recs, key=lambda r: r["available"])[:8]:
        print("   %-24s base=%-9s %2d slots" % (r["name"], r["base_name"],
                                                r["available"]))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("index\tname\tdisplay_name\tbase\tvariant\tavailable\t"
                     + "\t".join(SLOT_ROLE) + "\n")
            for r in recs:
                cells = []
                for s in r["slots"]:
                    if s is None:
                        cells.append("")
                    elif entities:
                        cells.append(entities.get(s, {}).get("name", str(s)))
                    else:
                        cells.append(str(s))
                fh.write("\t".join([str(r["index"]), r["name"],
                                    r.get("display_name", ""), r["base_name"],
                                    str(r["variant"]), str(r["available"])]
                                   + cells) + "\n")
        print("wrote %s" % args.tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
