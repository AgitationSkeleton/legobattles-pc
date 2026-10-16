#!/usr/bin/env python3
"""Label the ARM7's unreached code by the hardware it drives.

`docs/33` established that the ARM7's scanning coverage plateaus at ~70% under
every technique available, and argued the remainder is linked-in but unused
NitroSDK. That left the right question open: **unused SDK doing what?**

The ARM7 has no strings -- four printable runs in the whole image, assertions
stripped -- so it cannot be labelled by text. But an ARM7 subsystem is defined
by the **memory-mapped registers it touches**, and those are unmistakable:
`0x04000400` is sound, `0x040001C0` is the SPI bus, `0x04800000` is Wi-Fi. Every
driver has to name its own hardware.

## Attribution, done the right way

The first attempt at this was wrong in a way worth recording. It scanned each
unreached run for **words in the range** of a hardware register and attributed
them to that run. That double-counts badly, because a *reached* function keeps
its literal pool in the bytes after it -- which the walk reports as a gap. The
register constants of the reached drivers were being credited to the unreached
regions, and the result said reached code touched almost no hardware at all,
which is absurd for an ARM7.

The fix is to attribute a register to the code that **loads** it: disassemble
the run, follow its own PC-relative loads, and read what they point at. A
constant sitting in a region proves nothing; a constant the region *loads* is
that region's business.

Usage:
    python tools/lb_arm7map.py
    python tools/lb_arm7map.py --tsv docs/data/arm7_regions.tsv
"""

from __future__ import annotations

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

WRAM_LO = 0x037F8000
WRAM_SIZE = 62640

#: DS ARM7 memory-mapped I/O, by subsystem. Ranges are from the public hardware
#: documentation, not inferred from this ROM.
SUBSYSTEMS = [
    (0x04000000, 0x04000070, "video"),
    (0x040000B0, 0x040000F0, "DMA"),
    (0x04000100, 0x04000110, "timers"),
    (0x04000120, 0x04000130, "serial"),
    (0x04000130, 0x04000138, "keypad"),
    (0x04000138, 0x0400013C, "RTC"),
    (0x04000180, 0x04000194, "IPC"),
    (0x040001A0, 0x040001C0, "cartridge"),
    (0x040001C0, 0x040001C4, "SPI"),
    (0x04000200, 0x04000212, "IRQ"),
    (0x04000300, 0x0400030C, "power"),
    (0x04000400, 0x04000520, "sound"),
    (0x04100000, 0x04100004, "IPCFIFO"),
    (0x04800000, 0x04810000, "wifi"),
]


def subsystem_of(word):
    for lo, hi, name in SUBSYSTEMS:
        if lo <= word < hi:
            return name
    return None


def loads_by(space, engines, lo, hi, mode="arm"):
    """Subsystems the code in [lo, hi) LOADS, via its own PC-relative loads."""
    arm, thumb = engines
    hits = collections.Counter()
    data = space.read(lo, hi - lo)
    if not data:
        return hits
    cs = arm if mode == "arm" else thumb
    for ins in cs.disasm(data, lo):
        try:
            target = lb_code.literal_target(ins, mode != "arm")
        except Exception:
            continue
        if target is None:
            continue
        word = space.u32(target)
        if word is None:
            continue
        name = subsystem_of(word)
        if name:
            hits[name] += 1
    return hits


def read_corpus(path, lo, hi):
    rows = []
    with open(path, encoding="utf-8") as fh:
        fh.readline()
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if len(p) < 3 or not p[2]:
                continue
            a = int(p[0], 16)
            if lo <= a < hi:
                rows.append((a, int(p[2], 16), p[1]))
    return rows


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    space = lb_code.AddressSpace(args.extract, None, "arm7")
    engines = lb_code.make_engines()
    hi = WRAM_LO + WRAM_SIZE
    rows = read_corpus(os.path.join(args.extract, "arm7_corpus.tsv"),
                       WRAM_LO, hi)

    ivs = sorted((a, a + s) for a, s, _ in rows if s)
    merged = []
    for a, b in ivs:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    gaps = []
    prev = WRAM_LO
    for a, b in merged:
        if a > prev:
            gaps.append((prev, a))
        prev = b
    if prev < hi:
        gaps.append((prev, hi))

    reached = collections.Counter()
    for a, s, m in rows:
        if s:
            reached.update(loads_by(space, engines, a, a + s, m))
    unreached = collections.Counter()
    labelled = []
    for a, b in gaps:
        h = loads_by(space, engines, a, b)
        unreached.update(h)
        labelled.append((a, b, h))

    gap_bytes = sum(b - a for a, b in gaps)
    print(f"ARM7 wram 0x{WRAM_LO:08X}..0x{hi:08X}")
    print(f"   reached   {sum(b - a for a, b in merged):,} bytes in {len(rows)} functions")
    print(f"   unreached {gap_bytes:,} bytes in {len(gaps)} runs")
    print()
    print("subsystems LOADED by reached code  :",
          ", ".join(f"{k}x{v}" for k, v in reached.most_common()) or "-")
    print("subsystems LOADED by unreached code:",
          ", ".join(f"{k}x{v}" for k, v in unreached.most_common()) or "-")
    print()

    known = [(a, b, h) for a, b, h in labelled if h]
    known_bytes = sum(b - a for a, b, _ in known)
    print(f"{known_bytes:,} of {gap_bytes:,} unreached bytes "
          f"({100.0 * known_bytes / gap_bytes:.0f}%) sit in a run that names its "
          f"own hardware")
    print()
    print(f"{'range':<26}{'bytes':>7}  subsystem")
    for a, b, h in sorted(labelled, key=lambda r: -(r[1] - r[0]))[:20]:
        tag = ", ".join(f"{k}x{v}" for k, v in h.most_common(3)) or "(no register loads)"
        print(f"0x{a:08X}..0x{b:08X}{b - a:>7}  {tag}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["start", "end", "bytes", "state",
                               "subsystems"]) + nl)
            for a, b in merged:
                h = collections.Counter()
                for x, s, m in rows:
                    if a <= x < b and s:
                        h.update(loads_by(space, engines, x, x + s, m))
                fh.write(tab.join(["0x%08X" % a, "0x%08X" % b, str(b - a),
                                   "reached",
                                   ";".join(f"{k}:{v}" for k, v in h.most_common())]) + nl)
            for a, b, h in labelled:
                fh.write(tab.join(["0x%08X" % a, "0x%08X" % b, str(b - a),
                                   "unreached",
                                   ";".join(f"{k}:{v}" for k, v in h.most_common())]) + nl)
        print(f"\nwrote {args.tsv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
