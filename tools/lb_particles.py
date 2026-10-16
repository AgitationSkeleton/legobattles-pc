#!/usr/bin/env python3
"""`Particles/*.hps`, and the ARM9 table that names them.

Ten `.hps` files ship on the cartridge and `docs/04` filed them as opaque. Two
separate things are read here, and only one of them is the file format.

## The table, which is the better find

The ARM9 holds an array of `{const char *path, u32}` records at **0x02126BB8**,
8 bytes apart. Its one consumer is `func_02028F6C`, which does

    ldr  r1, [r1, r7, lsl #3]

-- base plus index times eight -- so the stride is stated by the code, not
guessed from the data. That function takes `(index, callback, userdata)`,
allocates a 28-byte request, stores the callback at +0x0C, the userdata at
+0x10 and the index at +0x14, and queues an asynchronous load. Particle systems
are therefore addressed **by index into this table**, never by name.

The table names **16 particle systems** in 60 slots; the other 44 point at an
empty string. Comparing those 16 against the files that shipped is where it gets
interesting -- see below.

## The file, read out of the code that reads it

The body was not guessed. `func_02029FF8` seeks to block *n* and its loop states
the whole layout:

    ldr  r3, [r0, #0x24]      the file base
    ldr  r2, [r3]             base[0] -- and n is bounds-checked against it
    add  r2, r3, #8           the cursor starts EIGHT bytes in
    ...
    ldr  r3, [r2]             each block begins with an element count
    add  r2, r2, #4
    addne r2, r2, r3, lsl #4  then count * 16 bytes

So:

    +0x00  u32  block count -- 50 in nine files, 20 in one
    +0x04  u32  a 20.12 scalar: 40.0 (x7), 50.0 (x1), 80.0 (x2)
    +0x08       block[count], each:
                    u32       element count
                    byte[16]  element[count]

**Walking that consumes every byte of all ten files, exactly.** A wrong stride
drifts, and 16 does not.

The blocks are **frames of a baked simulation** and the elements are the
particles alive in that frame. `HealthBoostTrailStar` runs 2, 3, 4, 4 ... 4, 3,
3, 3, 3, 2, 2, 2, 2, 2, then 26 empty frames -- a population rising, holding and
decaying. The file is not a description of a particle system; it is the
recording of one.

Within an element, word 2 reads as four 5-bit channels that drift smoothly frame
to frame -- a colour fade -- and word 3 takes only **14 distinct values across
all 4,045 elements on the cartridge**, which is a handle rather than a value.
Words 0 and 1 hold small signed quantities that accumulate at a constant rate.
**Those readings are characterisations, not conclusions**, and are marked as
such; the framing is what is decoded.

Usage:
    python tools/lb_particles.py
    python tools/lb_particles.py --tsv docs/data/particles.tsv
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

PARTICLES = "Particles"

#: The table of `{path, u32}` records, and the function that indexes it.
TABLE = 0x02126BB8
TABLE_STRIDE = 8
LOADER = 0x02028F6C

#: 20.12 is the game's fixed point everywhere else (`docs/25`, `docs/51`).
FIXED = 12

#: The header is EIGHT bytes: `func_02029FF8` sets the cursor to `base + 8`.
#: An earlier reading of this file called it twelve and treated the first block
#: count as a header field; the code says otherwise.
BODY = 8

#: Each element is 16 bytes -- `addne r2, r2, r3, lsl #4`.
ELEMENT = 16


def walk(data):
    """[[element bytes, ...], ...] one list per block, or raise ValueError."""
    if len(data) < BODY:
        raise ValueError("shorter than a header")
    count = struct.unpack_from("<I", data, 0)[0]
    off = BODY
    blocks = []
    for i in range(count):
        if off + 4 > len(data):
            raise ValueError(f"block {i} header runs past the end")
        n = struct.unpack_from("<I", data, off)[0]
        off += 4
        end = off + ELEMENT * n
        if end > len(data):
            raise ValueError(f"block {i} claims {n} elements, past the end")
        blocks.append([data[off + k * ELEMENT: off + (k + 1) * ELEMENT]
                       for k in range(n)])
        off = end
    if off != len(data):
        raise ValueError(f"walk consumed {off} of {len(data)} bytes")
    return blocks


def read_table(space, base, limit=256):
    """[(address, pointer, word, string)] until the pointer stops being text."""
    out = []
    addr = base
    for _ in range(limit):
        try:
            ptr = space.u32(addr)
            word = space.u32(addr + 4)
        except Exception:                                  # noqa: BLE001
            break
        text = space.cstring(ptr) if hasattr(space, "cstring") else None
        if text is None:
            text = _cstring(space, ptr)
        if text is None:
            break
        out.append((addr, ptr, word, text))
        addr += TABLE_STRIDE
    return out


def _cstring(space, addr, cap=80):
    """A NUL-terminated printable string at `addr`, or None."""
    try:
        raw = space.read(addr, cap)
    except Exception:                                      # noqa: BLE001
        return None
    if raw is None or b"\0" not in raw:
        return None
    s = raw[:raw.index(b"\0")]
    if any(c < 32 or c > 126 for c in s):
        return None
    return s.decode("latin-1")


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

    space = lb_code.AddressSpace(args.extract, None, "arm9")
    rows = read_table(space, TABLE)
    named = [r for r in rows if r[3]]
    print(f"table at 0x{TABLE:08X}, stride {TABLE_STRIDE}, indexed by "
          f"func_0x{LOADER:08X}")
    print(f"   {len(rows)} slots, {len(named)} named, "
          f"{len(rows) - len(named)} pointing at an empty string")
    words = collections.Counter(r[2] for r in rows)
    print("   second word: "
          + ", ".join(f"{v} x{n}" for v, n in words.most_common(4)))
    odd = [r for r in rows if r[2] not in (1, 2)]
    for a, _p, w, t in odd:
        print(f"   slot {(a - TABLE) // TABLE_STRIDE} at 0x{a:08X} has "
              f"0x{w:08X}, out of family -- treat the table as ending here")

    root = os.path.join(args.extract, "plain", PARTICLES)
    on_disk = sorted(os.listdir(root)) if os.path.isdir(root) else []
    in_arm9 = [t.rsplit("/", 1)[-1] for _a, _p, _w, t in named]

    print()
    print(f"{'file':<32}{'bytes':>8}{'blocks':>8}{'scale':>8}"
          f"{'parts':>7}{'peak':>6}{'live':>6}  walk")
    hdr = {}
    total_el = total_bk = 0
    bad = 0
    handles = collections.Counter()
    for name in on_disk:
        with open(os.path.join(root, name), "rb") as fh:
            data = fh.read()
        a, b = struct.unpack_from("<II", data, 0)
        try:
            blocks = walk(data)
            note = "exact"
        except ValueError as e:
            blocks = []
            note = str(e)
            bad += 1
        counts = [len(x) for x in blocks]
        live = sum(1 for c in counts if c)
        for blk in blocks:
            for el in blk:
                handles[struct.unpack_from("<I", el, 12)[0]] += 1
        total_el += sum(counts)
        total_bk += len(blocks)
        hdr[name] = (len(data), a, b, sum(counts), len(blocks))
        print(f"{name:<32}{len(data):>8,}{a:>8}"
              f"{b / (1 << FIXED):>8.1f}{sum(counts):>7}"
              f"{(max(counts) if counts else 0):>6}{live:>6}  {note}")

    scale = collections.Counter(v[2] for v in hdr.values())
    print()
    print(f"   {total_bk} blocks, {total_el} elements, "
          f"{len(on_disk) - bad} of {len(on_disk)} files walk exactly")
    print("   +0x04 values, all exact in 20.12: "
          + ", ".join(f"{v / (1 << FIXED):g} x{n}"
                      for v, n in sorted(scale.items())))
    print(f"   element word 3 takes {len(handles)} distinct values over "
          f"{total_el} elements -- a handle, not a value")

    cut = [n for n in in_arm9 if n not in on_disk]
    orphan = [n for n in on_disk if n not in in_arm9]
    print()
    print(f"named in the ARM9 but not on the cartridge: {len(cut)}")
    for n in cut:
        print(f"   {n}")
    print(f"on the cartridge but not named in the table: {len(orphan)}")
    for n in orphan:
        print(f"   {n}")
    print(f"both: {len(set(in_arm9) & set(on_disk))}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["slot", "address", "path", "word", "on_disk",
                               "bytes", "blocks", "scale_raw", "scale",
                               "elements"]) + nl)
            for a, _p, w, t in rows:
                base = t.rsplit("/", 1)[-1] if t else ""
                h = hdr.get(base)
                fh.write(tab.join([
                    str((a - TABLE) // TABLE_STRIDE), "0x%08X" % a, t,
                    "0x%08X" % w, "yes" if h else ("no" if t else ""),
                    str(h[0]) if h else "", str(h[1]) if h else "",
                    str(h[2]) if h else "",
                    ("%.1f" % (h[2] / (1 << FIXED))) if h else "",
                    str(h[3]) if h else "",
                ]) + nl)
            for name in orphan:
                h = hdr[name]
                fh.write(tab.join(["", "", PARTICLES + "/" + name, "", "yes",
                                   str(h[0]), str(h[1]), str(h[2]),
                                   "%.1f" % (h[2] / (1 << FIXED)),
                                   str(h[3])]) + nl)
        print(f"\nwrote {args.tsv} ({len(rows) + len(orphan)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
