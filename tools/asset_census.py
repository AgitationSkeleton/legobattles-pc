#!/usr/bin/env python3
"""Census the extracted NitroFS: group every file by extension and magic.

Answers the two questions that decide the asset track: which files are standard
Nintendo Nitro containers (already understood by public tooling) and which are
this game's own formats (which have to be reverse engineered).

Usage:
    python tools/asset_census.py out/eur [--json out/eur/asset_census.json]
"""

import argparse
import collections
import json
import os
import sys


# Magics that public tooling already understands. Nitro containers store their
# stamp reversed on disk because the DS is little-endian and the tools name them
# in the forward order.
KNOWN_MAGICS = {
    b"RGCN": ("NCGR", "Nitro character graphics (tiles)"),
    b"RLCN": ("NCLR", "Nitro colour palette"),
    b"RCSN": ("NSCR", "Nitro screen / tilemap"),
    b"RECN": ("NCER", "Nitro cell (OAM sprite assembly)"),
    b"RNAN": ("NANR", "Nitro animation"),
    b"RTFN": ("NFTR", "Nitro font"),
    b"BMD0": ("NSBMD", "Nitro 3D model"),
    b"BCA0": ("NSBCA", "Nitro 3D joint animation"),
    b"BTX0": ("NSBTX", "Nitro 3D texture bank"),
    b"BTP0": ("NSBTP", "Nitro 3D texture-pattern animation"),
    b"BTA0": ("NSBTA", "Nitro 3D texture-SRT animation"),
    b"BVA0": ("NSBVA", "Nitro 3D visibility animation"),
    b"SDAT": ("SDAT", "Nitro sound archive"),
    b"BNR\x00": ("BNR", "cartridge icon/banner"),
    b"RVBN": ("NBVR", "Nitro version stamp"),
}

# Magics this project has identified in LEGO Battles itself.
GAME_MAGICS = {
    b"PMOC": ("COMP", "LEGO Battles container: 'COMP', see docs/06"),
    b"sadl": ("SADL", "streamed audio archive"),
    b"SADL": ("SADL", "streamed audio archive"),
}


def classify(head):
    m4 = head[:4]
    if m4 in KNOWN_MAGICS:
        name, desc = KNOWN_MAGICS[m4]
        return "nitro", name, desc
    if m4 in GAME_MAGICS:
        name, desc = GAME_MAGICS[m4]
        return "game", name, desc
    return "unknown", m4.hex(), repr(bytes(m4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("extractdir")
    ap.add_argument("--json")
    args = ap.parse_args()

    root = os.path.join(args.extractdir, "nitrofs")
    if not os.path.isdir(root):
        sys.exit("no nitrofs under %s — run nds_extract.py first" % args.extractdir)

    rows = []
    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            size = os.path.getsize(full)
            with open(full, "rb") as fh:
                head = fh.read(16)
            kind, magic, desc = classify(head)
            rows.append({
                "path": rel,
                "size": size,
                "ext": os.path.splitext(name)[1].lower(),
                "kind": kind,
                "magic": magic,
                "note": desc,
                "head16": head.hex(),
            })

    rows.sort(key=lambda r: r["path"])

    # ext -> magic -> count, so a format that is a container in disguise shows up
    pairs = collections.Counter((r["ext"], r["kind"], r["magic"]) for r in rows)
    bytes_by = collections.Counter()
    for r in rows:
        bytes_by[(r["ext"], r["kind"], r["magic"])] += r["size"]

    width = max(len(e) for e, _, _ in pairs) if pairs else 8
    print("%-*s  %-8s %-8s %6s  %10s" % (width, "ext", "kind", "magic",
                                          "count", "MB"))
    print("-" * (width + 40))
    for (ext, kind, magic), count in sorted(pairs.items(),
                                            key=lambda kv: -kv[1]):
        print("%-*s  %-8s %-8s %6d  %10.2f"
              % (width, ext or "(none)", kind, magic, count,
                 bytes_by[(ext, kind, magic)] / 1048576))

    unknown = [r for r in rows if r["kind"] == "unknown"]
    print()
    print("unknown-magic files: %d of %d  (%.2f MB)"
          % (len(unknown), len(rows),
             sum(r["size"] for r in unknown) / 1048576))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=1)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
