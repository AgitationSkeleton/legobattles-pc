"""Diff two LEGO Battles NDS images by NitroFS file content.

The community mods ship as whole ROMs rather than patches, so "what does REbalance change" is a question about
two 120 MB files. This answers it by name: parse each image's FNT and FAT, hash
every file's bytes, and report what was added, removed or altered.

It also diffs the ARM9, ARM7 and the overlay table, because a mod that changes
code rather than data will show up there and nowhere else.

Usage:
    python tools/lb_moddiff.py BASE.nds MOD.nds [--tsv out.tsv] [--quiet]

The mods come in two families, `MapBase` (4,006 files) and `MapDump` (4,011).
Diff within a family: across families every path shifts and the result is noise.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nds_extract


def load(path):
    """Return (header, {path: bytes-view}, raw) for one NDS image."""
    with open(path, "rb") as fh:
        raw = fh.read()
    hdr = nds_extract.parse_header(raw)
    fnt_off, fnt_len = struct.unpack("<II", raw[0x40:0x48])
    fat_off, fat_len = struct.unpack("<II", raw[0x48:0x50])
    paths, _dirs = nds_extract.parse_fnt(raw[fnt_off:fnt_off + fnt_len])
    fat = nds_extract.parse_fat(raw[fat_off:fat_off + fat_len])
    files = {}
    for fid, name in paths.items():
        if fid < len(fat):
            start, end = fat[fid]
            files[name] = (start, end)
    return hdr, files, raw


def region(raw, hdr, which):
    """Slice one of the fixed regions out of an image."""
    if which == "arm9":
        off = struct.unpack("<I", raw[0x20:0x24])[0]
        size = struct.unpack("<I", raw[0x2C:0x30])[0]
    elif which == "arm7":
        off = struct.unpack("<I", raw[0x30:0x34])[0]
        size = struct.unpack("<I", raw[0x3C:0x40])[0]
    elif which == "ovt9":
        off = struct.unpack("<I", raw[0x50:0x54])[0]
        size = struct.unpack("<I", raw[0x54:0x58])[0]
    else:
        return b""
    return raw[off:off + size]


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("mod")
    ap.add_argument("--tsv")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args(argv)

    hb, fb, rb = load(args.base)
    hm, fm, rm = load(args.mod)

    print("%-30s %d files" % (os.path.basename(args.base), len(fb)))
    print("%-30s %d files" % (os.path.basename(args.mod), len(fm)))

    for which in ("arm9", "arm7", "ovt9"):
        a, b = region(rb, hb, which), region(rm, hm, which)
        same = a == b
        print("  %-5s %8d vs %8d bytes  %s"
              % (which, len(a), len(b),
                 "identical" if same else "*** DIFFERS ***"))

    added = sorted(set(fm) - set(fb))
    removed = sorted(set(fb) - set(fm))
    common = sorted(set(fb) & set(fm))

    changed = []
    for name in common:
        s0, e0 = fb[name]
        s1, e1 = fm[name]
        if (e0 - s0) != (e1 - s1) or rb[s0:e0] != rm[s1:e1]:
            changed.append((name, e0 - s0, e1 - s1))

    print("\nadded   %d\nremoved %d\nchanged %d\nidentical %d"
          % (len(added), len(removed), len(changed),
             len(common) - len(changed)))

    if not args.quiet:
        for label, rows in (("added", added), ("removed", removed)):
            if rows:
                print("\n%s:" % label)
                for n in rows[:args.limit]:
                    print("   ", n)
                if len(rows) > args.limit:
                    print("    ... %d more" % (len(rows) - args.limit))
        if changed:
            print("\nchanged:")
            for n, a, b in changed[:args.limit]:
                delta = "%+d" % (b - a) if a != b else "same size"
                print("    %-52s %7d -> %7d  %s" % (n, a, b, delta))
            if len(changed) > args.limit:
                print("    ... %d more" % (len(changed) - args.limit))

    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write("status\tpath\tbase_size\tmod_size\n")
            for n in added:
                s, e = fm[n]
                fh.write("added\t%s\t\t%d\n" % (n, e - s))
            for n in removed:
                s, e = fb[n]
                fh.write("removed\t%s\t%d\t\n" % (n, e - s))
            for n, a, b in changed:
                fh.write("changed\t%s\t%d\t%d\n" % (n, a, b))
        print("\nwrote %s" % args.tsv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
