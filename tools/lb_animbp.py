#!/usr/bin/env python3
"""`BP/Animations.abp` -- 180 records, and a walk that lands on the last byte.

2,580 bytes. `docs/04` filed it opaque and `docs/20` added that **the ARM9 never
names it** -- there is no `Animations.abp` string anywhere in the image -- so it
is opaque for a second, independent reason and no loader can be read to learn
the format. Whatever is recovered has to come from the bytes.

The bytes are cooperative. The file is a flat sequence of variable-length
records:

    +0x00  u8   key a
    +0x01  u8   key b
    +0x02  u8   key c
    +0x03  u8   key d
    +0x04  u8   value count
    +0x05  u8   0xA1, on every record
    +0x06  u16  value[count], little-endian

**The walk is the check.** Chaining `6 + 2 * count` from offset zero consumes
**2,580 of 2,580 bytes** and yields 180 records, every one carrying 0xA1 at
+0x05. A wrong record length drifts: it would run off the end or stop short,
and it does neither.

## What the keys factor into

Key `a` takes twelve values -- **0 to 5 and 17 to 22, two runs of six** -- and
six is the faction count everywhere else in this game (`docs/53`). Key `b` takes
three. Those 36 combinations each carry **exactly five** records, and
`12 x 3 x 5 = 180`.

That the 36 groups are all the same size is not something a wrong framing
produces.

## What is not known

The `u16` values are not interpreted. In the low groups they are small and step
by three; in the high groups they run near 33,000 and step by four, which reads
like an offset into a resource this tool has not identified. **The record
structure is decoded, the value semantics are not**, and that distinction is
kept rather than papered over.

Usage:
    python tools/lb_animbp.py
    python tools/lb_animbp.py --tsv docs/data/animations_abp.tsv
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

ANIM = "BP/Animations.abp"

#: Present at +0x05 on all 180 records. Not a magic -- a per-record marker.
MARKER = 0xA1

#: Key `a`'s twelve values: two runs of six, and six is the faction count.
FACTIONS = 6


def parse(data):
    """[(a, b, c, d, values)] or raise ValueError if the walk does not fit."""
    out = []
    off = 0
    while off < len(data):
        if off + 6 > len(data):
            raise ValueError(f"record header runs past the end at {off}")
        a, b, c, d, count, mark = struct.unpack_from("<6B", data, off)
        if mark != MARKER:
            raise ValueError(f"marker at {off + 5} is 0x{mark:02X}, "
                             f"not 0x{MARKER:02X}")
        end = off + 6 + 2 * count
        if end > len(data):
            raise ValueError(f"record at {off} claims {count} values, "
                             f"which runs past the end")
        vals = list(struct.unpack_from("<%dH" % count, data, off + 6))
        out.append((a, b, c, d, vals))
        off = end
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
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    path = os.path.join(args.extract, "plain", ANIM.replace("/", os.sep))
    if not os.path.exists(path):
        print(f"missing {path}", file=sys.stderr)
        return 2
    with open(path, "rb") as fh:
        data = fh.read()

    recs = parse(data)
    used = sum(6 + 2 * len(r[4]) for r in recs)
    print(f"{ANIM}: {len(data):,} bytes, {len(recs)} records")
    print(f"the walk consumes {used:,} of {len(data):,} bytes -- "
          + ("EXACT" if used == len(data) else "MISMATCH"))
    print(f"every record carries 0x{MARKER:02X} at +0x05: "
          f"{all(True for _ in recs)}")

    a_vals = sorted({r[0] for r in recs})
    b_vals = sorted({r[1] for r in recs})
    groups = collections.Counter((r[0], r[1]) for r in recs)
    sizes = set(groups.values())
    print()
    print(f"key a: {len(a_vals)} values {a_vals}")
    print(f"key b: {len(b_vals)} values {b_vals}")
    print(f"groups (a, b): {len(groups)}, each holding "
          f"{sorted(sizes)} records")
    print(f"   {len(a_vals)} x {len(b_vals)} x {sorted(sizes)[0]} = "
          f"{len(a_vals) * len(b_vals) * sorted(sizes)[0]}, and there are "
          f"{len(recs)} records")

    runs = []
    for v in a_vals:
        if runs and v == runs[-1][-1] + 1:
            runs[-1].append(v)
        else:
            runs.append([v])
    print(f"   key a is {len(runs)} consecutive runs of "
          f"{[len(r) for r in runs]} -- the faction count is {FACTIONS}")

    counts = collections.Counter(len(r[4]) for r in recs)
    print()
    print("values per record: "
          + ", ".join(f"{k} x{v}" for k, v in sorted(counts.items())))
    lo = [r for r in recs if r[0] < 6]
    hi = [r for r in recs if r[0] >= 17]
    for label, rows in (("a = 0..5", lo), ("a = 17..22", hi)):
        vals = [v for r in rows for v in r[4]]
        print(f"   {label:<12}{len(rows):>4} records, values "
              f"{min(vals):,}..{max(vals):,}")
    print("the u16 values are NOT interpreted; see the module docstring")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["record", "a", "b", "c", "d", "count",
                               "values"]) + nl)
            for i, (a, b, c, d, vals) in enumerate(recs):
                fh.write(tab.join([str(i), str(a), str(b), str(c), str(d),
                                   str(len(vals)),
                                   "|".join(str(v) for v in vals)]) + nl)
        print(f"\nwrote {args.tsv} ({len(recs)} records)")

    return 0 if used == len(data) else 1


if __name__ == "__main__":
    raise SystemExit(main())
