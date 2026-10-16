#!/usr/bin/env python3
"""Parse `Maps/*.map` — the battlefield container and its terrain.

Derived from `Battles::Map::LoadTerrain` at `0x020A3A08`, whose name comes from
the third-party `ds-decomp` config (`docs/02`) and whose body is read directly.

The container is tag-framed:

    'MAP'                     3-byte magic
    'TERR' … 'RRET'           terrain — fixed size, closed by its reversed tag
    'EVNT' … 0x21             events
    'TRIG' … 0x21             triggers
    'MARK' … 0x21             markers
    'MINE' … 0x21             mines
    'PAM'                     3-byte terminator — the magic, reversed

The terrain chunk, straight out of the loader:

    +0x00  char[4]    'TERR'
    +0x04  u8         width
    +0x05  u8         height
    +0x06  u8         unknown
    +0x07  u8         unknown
    +0x08  char[32]   tileset name, NUL-padded — the loader builds a string
                      from it and stores it at map+0x18
    +0x28  u8[w*h]    plane A   -> map+0x2E8
           u8[w*h]    plane B   -> map+0x2F0
           u8[w*h]    plane C   -> map+0x2EC
           u16        n
           u8[n]      block D   -> map+0x2F4
           u16[w*h]   plane E   -> map+0x30, with per-row pointers at map+0x34
    'RRET'

Validated on all 122 maps: the computed end of the terrain chunk lands exactly
on `RRET` every time, the four record chunks always appear in the order above,
and each is preceded by the previous one's `0x21`.

Usage:
    python tools/lb_map.py out/eur/plain/Maps/ck1_1.map
    python tools/lb_map.py out/eur/plain/Maps --all [--tsv docs/data/maps.tsv]
    python tools/lb_map.py <file> --pgm out/eur/terrain     # planes as images
"""

import argparse
import collections
import os
import struct
import sys

MAGIC = b"MAP"
END = b"PAM"
TERRAIN_END = b"RRET"
CHUNK_END = 0x21              # '!'
RECORD_MARK = 0x4C            # 'L'
RECORD_CHUNKS = [b"EVNT", b"TRIG", b"MARK", b"MINE"]
NAME_FIELD = 0x20
HEADER = 0x28


class MapError(Exception):
    pass


# ---------------------------------------------------------------- records ----
#
# Every record chunk is a stream of records introduced by RECORD_MARK ('L') and
# closed by CHUNK_END ('!'). The per-record grammars below are read from the
# loaders, and each sub-record size is the number of times its reader advances
# the shared cursor — counted from the disassembly, and unconditional in every
# case (all the cursor stores precede the first branch).
#
#   EVNT  Battles::Map::LoadEvents   -> func_020A4B90 per record
#   TRIG  Battles::Map::LoadTriggers -> func_020A5494 per record
#   MARK  Battles::Map::LoadMarkers  -> Battles::Map::LoadMarker per marker
#   MINE  Battles::Map::LoadMines    -> inline
#
# Every one of these parses to the terminator exactly on all 122 maps.

EVENT_COND = 9        # func_020A5C2C, 9 cursor advances
EVENT_ACTION = 8      # func_020A4610, 8
EVENT_THIRD = 3       # func_020A5DE0, 3
MARKER = 3            # Battles::Map::LoadMarker, 3
TRIGGER_BODY = 10     # func_020A5494 after its gate byte


def read_events(d, p, end):
    """'L' id, n points, gate + 4, then three counted lists."""
    out = []
    while p < end:
        if d[p] != RECORD_MARK:
            raise MapError("EVNT frame byte 0x%02x at 0x%x" % (d[p], p))
        p += 1
        rec = {"id": d[p]}
        p += 1
        n = d[p]
        p += 1
        rec["points"] = [(d[p + 2 * i], d[p + 2 * i + 1]) for i in range(n)]
        p += 2 * n
        rec["area_flag"] = d[p]
        p += 1
        if rec["area_flag"]:
            rec["area"] = tuple(d[p:p + 4])
            p += 4
        for key, size in (("conditions", EVENT_COND),
                          ("actions", EVENT_ACTION),
                          ("third", EVENT_THIRD)):
            count = d[p]
            p += 1
            rec[key] = [d[p + size * i:p + size * (i + 1)].hex()
                        for i in range(count)]
            p += size * count
        out.append(rec)
    return out, p


def read_triggers(d, p, end):
    """'L' id, a gate byte, and ten more bytes only when it is set."""
    out = []
    while p < end:
        if d[p] != RECORD_MARK:
            raise MapError("TRIG frame byte 0x%02x at 0x%x" % (d[p], p))
        rec = {"id": d[p + 1], "flag": d[p + 2]}
        p += 3
        if rec["flag"]:
            rec["body"] = d[p:p + TRIGGER_BODY].hex()
            p += TRIGGER_BODY
        out.append(rec)
    return out, p


def read_markers(d, p, end):
    """'L' id, a count, then that many 3-byte markers."""
    out = []
    while p < end:
        if d[p] != RECORD_MARK:
            raise MapError("MARK frame byte 0x%02x at 0x%x" % (d[p], p))
        count = d[p + 2]
        rec = {"id": d[p + 1],
               "markers": [tuple(d[p + 3 + MARKER * i:p + 3 + MARKER * (i + 1)])
                           for i in range(count)]}
        p += 3 + MARKER * count
        out.append(rec)
    return out, p


def read_mines(d, p, end):
    """Four groups — one per team — each a count and that many x,y pairs."""
    out = []
    while p < end:
        if d[p] != RECORD_MARK:
            raise MapError("MINE frame byte 0x%02x at 0x%x" % (d[p], p))
        count = d[p + 1]
        out.append({"team": len(out),
                    "mines": [(d[p + 2 + 2 * i], d[p + 3 + 2 * i])
                              for i in range(count)]})
        p += 2 + 2 * count
    return out, p


READERS = {
    b"EVNT": read_events,
    b"TRIG": read_triggers,
    b"MARK": read_markers,
    b"MINE": read_mines,
}


def parse(data):
    if data[:3] != MAGIC:
        raise MapError("not a MAP file (magic %r)" % data[:3])
    p = 3
    if data[p:p + 4] != b"TERR":
        raise MapError("no TERR chunk at offset 3")

    width, height = data[p + 4], data[p + 5]
    cells = width * height
    tileset = data[p + 8:p + 8 + NAME_FIELD].split(b"\x00")[0].decode("latin-1")

    q = p + HEADER
    planes = {}
    for name in ("A", "B", "C"):
        planes[name] = data[q:q + cells]
        q += cells
    extra_len = struct.unpack_from("<H", data, q)[0]
    q += 2
    extra = data[q:q + extra_len]
    q += extra_len
    plane_e = data[q:q + cells * 2]
    q += cells * 2

    if data[q:q + 4] != TERRAIN_END:
        raise MapError("terrain ends at 0x%x with %r, expected 'RRET'"
                       % (q, data[q:q + 4]))
    q += 4

    chunks = []
    for tag in RECORD_CHUNKS:
        if data[q:q + 4] != tag:
            raise MapError("expected %r at 0x%x, found %r"
                           % (tag, q, data[q:q + 4]))
        start = q
        end = data.find(bytes([CHUNK_END]), q + 4)
        # The terminator cannot be found by scanning for 0x21 alone — record
        # payloads contain it. Locate the next tag instead and take the byte
        # before it, which is the terminator by construction.
        nxt = RECORD_CHUNKS[RECORD_CHUNKS.index(tag) + 1] \
            if tag != RECORD_CHUNKS[-1] else END
        i = data.find(nxt, q + 4)
        if i < 0:
            raise MapError("no %r after the %r chunk" % (nxt, tag))
        if data[i - 1] != CHUNK_END:
            raise MapError("%r chunk is not closed by 0x21 before %r"
                           % (tag, nxt))
        end = i - 1
        records, consumed = READERS[tag](data, start + 4, end)
        if consumed != end:
            raise MapError("%r records consumed to 0x%x, terminator at 0x%x"
                           % (tag, consumed, end))
        chunks.append({
            "tag": tag.decode(),
            "offset": start,
            "size": end - start,
            "records": records,
        })
        q = i

    if data[q:q + 3] != END:
        raise MapError("file does not end with 'PAM' (found %r)" % data[q:q + 3])

    return {
        "width": width, "height": height, "cells": cells,
        "unknown_06": data[p + 6], "unknown_07": data[p + 7],
        "tileset": tileset,
        "planes": planes, "plane_e": plane_e,
        "extra_len": extra_len, "extra": extra,
        "terrain_size": (p + HEADER + 3 * cells + 2 + extra_len
                         + 2 * cells + 4) - p,
        "chunks": chunks,
        "size": len(data),
    }


def write_pgm(path, data, width, height):
    with open(path, "wb") as fh:
        fh.write(b"P5\n%d %d\n255\n" % (width, height))
        fh.write(data[:width * height])


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--all", action="store_true",
                    help="path is a directory; parse and validate every map")
    ap.add_argument("--tsv")
    ap.add_argument("--pgm", help="directory to write terrain planes into")
    args = ap.parse_args()

    if args.all:
        rows, fails = [], []
        dims, tilesets = collections.Counter(), collections.Counter()
        for fn in sorted(os.listdir(args.path)):
            with open(os.path.join(args.path, fn), "rb") as fh:
                data = fh.read()
            try:
                m = parse(data)
            except MapError as exc:
                fails.append((fn, str(exc)))
                continue
            dims[(m["width"], m["height"])] += 1
            tilesets[m["tileset"]] += 1
            rows.append((fn, m))
        print("maps parsed: %d   failed: %d" % (len(rows), len(fails)))
        for fn, why in fails[:10]:
            print("   FAIL %s: %s" % (fn, why))
        print()
        print("dimensions: %s" % sorted(dims.items(), key=lambda kv: -kv[1]))
        print("tilesets:   %s" % dict(tilesets))
        print()
        print("%-14s %5s %-14s %7s %6s %6s %6s %6s %6s"
              % ("map", "size", "tileset", "terrain", "extra",
                 "EVNT", "TRIG", "MARK", "MINE"))
        for fn, m in rows[:12]:
            c = {ch["tag"]: len(ch["records"]) for ch in m["chunks"]}
            print("%-14s %2dx%-2d %-14s %7d %6d %6d %6d %6d %6d"
                  % (fn, m["width"], m["height"], m["tileset"],
                     m["terrain_size"], m["extra_len"],
                     c["EVNT"], c["TRIG"], c["MARK"], c["MINE"]))
        if len(rows) > 12:
            print("   … %d more" % (len(rows) - 12))
        if args.tsv:
            os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
            with open(args.tsv, "w", encoding="utf-8") as fh:
                fh.write("map\twidth\theight\ttileset\tfile_size\tterrain_size\t"
                         "extra_len\tEVNT\tTRIG\tMARK\tMINE\n")
                for fn, m in rows:
                    c = {ch["tag"]: len(ch["records"]) for ch in m["chunks"]}
                    fh.write("%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n"
                             % (os.path.splitext(fn)[0], m["width"], m["height"],
                                m["tileset"], m["size"], m["terrain_size"],
                                m["extra_len"], c["EVNT"], c["TRIG"], c["MARK"],
                                c["MINE"]))
            print("wrote %s" % args.tsv)
        return 1 if fails else 0

    with open(args.path, "rb") as fh:
        m = parse(fh.read())
    print("%s: %dx%d, tileset %r, %d bytes"
          % (os.path.basename(args.path), m["width"], m["height"],
             m["tileset"], m["size"]))
    print("  header bytes +0x06/+0x07: %d, %d" % (m["unknown_06"], m["unknown_07"]))
    print("  terrain chunk: %d bytes (extra block %d)"
          % (m["terrain_size"], m["extra_len"]))
    for name in ("A", "B", "C"):
        vals = collections.Counter(m["planes"][name])
        print("  plane %s: %d distinct values, most common %s"
              % (name, len(vals), vals.most_common(5)))
    for ch in m["chunks"]:
        print("  %-5s at 0x%05x, %5d bytes, %4d records"
              % (ch["tag"], ch["offset"], ch["size"], len(ch["records"])))

    if args.pgm:
        os.makedirs(args.pgm, exist_ok=True)
        stem = os.path.splitext(os.path.basename(args.path))[0]
        for name in ("A", "B", "C"):
            out = os.path.join(args.pgm, "%s_%s.pgm" % (stem, name))
            write_pgm(out, m["planes"][name], m["width"], m["height"])
        print("wrote terrain planes to %s" % args.pgm)
    return 0


if __name__ == "__main__":
    sys.exit(main())
