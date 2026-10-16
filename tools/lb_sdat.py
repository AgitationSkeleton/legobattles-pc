#!/usr/bin/env python3
"""`Sound/sound_data.sdat` -- the 62.9 MB sound archive, and its symbol tables.

This is the largest single file on the cartridge: 62,902,976 bytes, 51.9% of it
by weight. `docs/30` filed it as *known format, no parser here*, which was the
honest tier while nothing here read it.

It is read here now, and it comes with a check nothing else on this cartridge
offers. `docs/52` read the shipped C header `sound_data.sadl` and recovered
seven namespaces with exact counts -- 217 sequence archives, 200 banks, 8 wave
archives, 98 streams, 2 players, 2 stream players. **The `.sdat` carries its own
symbol tables, written by a different tool at a different time, and they must
agree.** If they do not, one of the two readings is wrong.

## The container

    +0x00  char[4]  "SDAT"
    +0x04  u16      byte order mark, 0xFEFF
    +0x06  u16      version
    +0x08  u32      file size
    +0x0C  u16      header size, 0x40
    +0x0E  u16      block count, 4
    +0x10  u32 x2   SYMB offset, size
    +0x18  u32 x2   INFO offset, size
    +0x20  u32 x2   FAT  offset, size
    +0x28  u32 x2   FILE offset, size

`FILE offset + FILE size == the file size`, exactly, which is the first check
that the header is being read and not merely plausible.

## SYMB and INFO

Both blocks are a magic, a size, and then **eight offsets, one per record
type**, in this order:

    SEQ  SEQARC  BANK  WAVEARC  PLAYER  GROUP  STRMPLAYER  STRM

Each offset is relative to its block's start and points at `u32 count` followed
by `count` entries. In SYMB the entries are name offsets; in INFO they are
offsets to per-record structures. `SEQARC` is the exception on the SYMB side:
its entries are **eight** bytes, a name offset plus an offset to that archive's
own table of sequence names -- which is exactly the nesting `docs/52` recovered
from the header by cutting the `SE` runs.

Usage:
    python tools/lb_sdat.py
    python tools/lb_sdat.py --tsv docs/data/sdat_symbols.tsv
    python tools/lb_sdat.py --type SEQARC --list
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

SDAT = "Sound/sound_data.sdat"

#: The eight record types, in the order both SYMB and INFO list them.
TYPES = ("SEQ", "SEQARC", "BANK", "WAVEARC", "PLAYER", "GROUP",
         "STRMPLAYER", "STRM")

#: What `docs/52` read out of the shipped C header. These are predictions about
#: a file the header does not mention; the point is that they can be wrong.
HEADER_COUNTS = {
    "SEQARC": 217, "BANK": 200, "WAVEARC": 8,
    "PLAYER": 2, "STRMPLAYER": 2, "STRM": 98,
}


def blocks(data):
    """{name: (offset, size)} for SYMB, INFO, FAT and FILE."""
    if data[:4] != b"SDAT":
        raise ValueError(f"magic is {data[:4]!r}, not SDAT")
    bom, ver, size, hdr, nblk = struct.unpack_from("<HHIHH", data, 4)
    if bom != 0xFEFF:
        raise ValueError(f"byte order mark is 0x{bom:04X}")
    if size != len(data):
        raise ValueError(f"declared size {size} != actual {len(data)}")
    out = {}
    for i, name in enumerate(("SYMB", "INFO", "FAT", "FILE")[:nblk]):
        o, s = struct.unpack_from("<II", data, 0x10 + i * 8)
        out[name] = (o, s)
    return out, ver, hdr, nblk


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


def symbols(data, base):
    """{type: [name, ...]} plus {SEQARC name: [sub-name, ...]}."""
    names = {}
    nested = {}
    for i, kind in enumerate(TYPES):
        rel = struct.unpack_from("<I", data, base + 8 + i * 4)[0]
        if not rel:
            names[kind] = []
            continue
        tbl = base + rel
        count = struct.unpack_from("<I", data, tbl)[0]
        stride = 8 if kind == "SEQARC" else 4
        out = []
        for k in range(count):
            e = tbl + 4 + k * stride
            noff = struct.unpack_from("<I", data, e)[0]
            nm = cstr(data, base + noff) if noff else ""
            out.append(nm)
            if stride == 8:
                soff = struct.unpack_from("<I", data, e + 4)[0]
                subs = []
                if soff:
                    st = base + soff
                    n = struct.unpack_from("<I", data, st)[0]
                    for j in range(n):
                        so = struct.unpack_from("<I", data, st + 4 + j * 4)[0]
                        subs.append(cstr(data, base + so) if so else "")
                nested[nm] = subs
        names[kind] = out
    return names, nested


def info_counts(data, base):
    """{type: count} from the INFO block's own tables."""
    out = {}
    for i, kind in enumerate(TYPES):
        rel = struct.unpack_from("<I", data, base + 8 + i * 4)[0]
        out[kind] = struct.unpack_from("<I", data, base + rel)[0] if rel else 0
    return out


def info_file_ids(data, base, kind):
    """[file id] for one record type. Every record begins with its file id."""
    i = TYPES.index(kind)
    rel = struct.unpack_from("<I", data, base + 8 + i * 4)[0]
    if not rel:
        return []
    tbl = base + rel
    count = struct.unpack_from("<I", data, tbl)[0]
    out = []
    for k in range(count):
        o = struct.unpack_from("<I", data, tbl + 4 + k * 4)[0]
        out.append(struct.unpack_from("<H", data, base + o)[0] if o else -1)
    return out


def fat(data, base):
    """[(offset, size)] for every file in the archive."""
    count = struct.unpack_from("<I", data, base + 8)[0]
    return [struct.unpack_from("<II", data, base + 12 + k * 16)
            for k in range(count)]


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
    ap.add_argument("--tsv")
    ap.add_argument("--type", help="list the names of one record type")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)

    path = os.path.join(args.extract, "plain", SDAT.replace("/", os.sep))
    if not os.path.exists(path):
        print(f"missing {path}", file=sys.stderr)
        return 2
    with open(path, "rb") as fh:
        data = fh.read()

    blk, ver, hdr, nblk = blocks(data)
    print(f"{SDAT}: {len(data):,} bytes, version 0x{ver:04X}, "
          f"header 0x{hdr:X}, {nblk} blocks")
    for name in ("SYMB", "INFO", "FAT", "FILE"):
        o, s = blk[name]
        own = struct.unpack_from("<I", data, o + 4)[0]
        print(f"   {name:<5} at 0x{o:08X}  size {s:>10,}  "
              f"block says {own:,}"
              + ("" if own == s else "  (padded)"))
    fo, fs = blk["FILE"]
    print(f"FILE ends the archive: 0x{fo:X} + {fs:,} = {fo + fs:,} "
          f"{'==' if fo + fs == len(data) else '!='} {len(data):,}")

    names, nested = symbols(data, blk["SYMB"][0])
    counts = info_counts(data, blk["INFO"][0])
    print()
    print(f"{'type':<12}{'SYMB':>7}{'INFO':>7}{'header':>8}  agreement")
    for kind in TYPES:
        sym = len(names[kind])
        inf = counts[kind]
        want = HEADER_COUNTS.get(kind)
        mark = "same" if sym == inf else "SYMB != INFO"
        if want is not None:
            mark += "" if want == sym else f"  header says {want}"
        print(f"{kind:<12}{sym:>7}{inf:>7}"
              f"{(want if want is not None else '-'):>8}  {mark}")

    entries = fat(data, blk["FAT"][0])
    nfile = struct.unpack_from("<I", data, fo + 8)[0]
    inside = sum(1 for o, s in entries if fo <= o and o + s <= fo + fs)
    print()
    print(f"FAT: {len(entries):,} entries, FILE block declares {nfile:,} "
          f"{'==' if len(entries) == nfile else '!='}")
    print(f"     {inside:,} of {len(entries):,} lie inside the FILE block")
    print(f"     {sum(s for _o, s in entries):,} bytes of payload")

    magics = collections.Counter()
    for o, s in entries:
        magics[data[o:o + 4].decode("latin-1", "replace")] += 1
    print("     payload magics: "
          + ", ".join(f"{m} {n}" for m, n in magics.most_common()))

    subs = sum(len(v) for v in nested.values())
    print()
    print(f"SEQARC sub-tables: {len(nested)} archives, {subs} sequence names")

    # 217 archives over 205 files and 200 banks over 188: twelve of each are
    # shared, and they are the same twelve. `docs/54` names them.
    print()
    print(f"{'type':<12}{'records':>8}{'distinct files':>16}{'shared':>8}")
    ib = blk["INFO"][0]
    shared_names = {}
    for kind in ("SEQARC", "BANK", "STRM", "WAVEARC"):
        ids = info_file_ids(data, ib, kind)
        seen = collections.Counter(ids)
        shared = sum(c - 1 for c in seen.values() if c > 1)
        print(f"{kind:<12}{len(ids):>8}{len(set(ids)):>16}{shared:>8}")
        shared_names[kind] = sorted(names[kind][i] for i, f in enumerate(ids)
                                    if seen[f] > 1)
    for kind, ns in shared_names.items():
        if ns:
            print(f"   {kind} sharing a file: {len(ns)} names, "
                  f"{ns[0]} .. {ns[-1]}")

    # The header (docs/52) and this archive were written by different tools.
    # Compare them name for name, at the same index, and report the rate.
    try:
        import lb_sound_symbols as lb_sound
        hpath = os.path.join(args.extract, "plain",
                             lb_sound.SADL.replace("/", os.sep))
        hdefs = lb_sound.parse(open(hpath, encoding="latin-1").read())
    except Exception as e:                                # noqa: BLE001
        print(f"\nheader comparison unavailable: {e}")
        return 1
    by_ns = collections.defaultdict(dict)
    for n, v in hdefs:
        by_ns[lb_sound.namespace(n)][v] = n
    print()
    print("against the shipped C header, name for name at the same index:")
    total = agree = 0
    for kind in TYPES:
        hk = {"WAVEARC": "WAVE"}.get(kind, kind)
        if not names[kind]:
            continue
        ok = sum(1 for i, nm in enumerate(names[kind])
                 if by_ns[hk].get(i) == nm)
        total += len(names[kind])
        agree += ok
        print(f"   {kind:<12}{ok:>5} of {len(names[kind])}")
    nok = ntot = 0
    for _k, owner, run in lb_sound.archives(hdefs):
        sd = nested.get("SEQARC__" + owner, [])
        for j, (sen, _v) in enumerate(run):
            ntot += 1
            if j < len(sd) and sd[j] == "SE_" + sen:
                nok += 1
    print(f"   {'nested SE':<12}{nok:>5} of {ntot}")
    print(f"   {'total':<12}{agree + nok:>5} of {total + ntot}"
          f"   ({len(hdefs)} #define lines in the header)")

    if args.type and args.list:
        print()
        for i, nm in enumerate(names.get(args.type, [])):
            print(f"   {i:>4}  {nm}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["type", "index", "name", "sub_index",
                               "sub_name"]) + nl)
            for kind in TYPES:
                for i, nm in enumerate(names[kind]):
                    fh.write(tab.join([kind, str(i), nm, "", ""]) + nl)
                    for j, sn in enumerate(nested.get(nm, [])):
                        fh.write(tab.join([kind, str(i), nm, str(j), sn]) + nl)
        total = sum(len(v) for v in names.values()) + subs
        print(f"\nwrote {args.tsv} ({total} names)")

    bad = [k for k in TYPES if len(names[k]) != counts[k]]
    mis = [k for k, v in HEADER_COUNTS.items() if len(names[k]) != v]
    return 0 if not bad and not mis and inside == len(entries) else 1


if __name__ == "__main__":
    raise SystemExit(main())
