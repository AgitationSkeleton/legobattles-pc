#!/usr/bin/env python3
"""`banner.bnr` -- the cartridge icon and titles, and the only stored checksum.

2,112 bytes, and the last file this project had sitting in the *known format, no
parser here* tier. It is the standard DS banner:

    +0x000  u16       version, 0x0001
    +0x002  u16       CRC-16 of 0x020..0x83F
    +0x004  byte[28]  reserved, zero
    +0x020  byte[512] the 32x32 icon, 4bpp, as 4x4 tiles of 8x8
    +0x220  byte[32]  the icon palette, 16 BGR555 entries
    +0x240  u16[128]  title, Japanese      (UTF-16LE, NUL-padded)
    +0x340  u16[128]  title, English
    +0x440  u16[128]  title, French
    +0x540  u16[128]  title, German
    +0x640  u16[128]  title, Italian
    +0x740  u16[128]  title, Spanish

`0x240 + 6 * 256 == 0x840 == 2112`, which is the whole file, so the layout is
checked by the size before a byte of it is interpreted.

## The checksum

**This is the only file on the cartridge that carries a checksum of itself.**
The `u16` at +0x002 is a CRC-16 (the reflected 0xA001 / MODBUS form, initial
value 0xFFFF) over bytes 0x020 through 0x83F -- the icon, the palette and all
six titles.

Every other "does this parse" check in this repository is a structural identity:
offsets landing on each other, counts agreeing. This one is different in kind. A
CRC that matches is not evidence that the *layout* is right, it is evidence that
**every byte of the body is exactly what the cartridge author signed**, and a
single wrong byte anywhere in 2,080 would break it.

Usage:
    python tools/lb_banner.py
    python tools/lb_banner.py --icon out/icon.txt
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

BANNER = "banner.bnr"

#: The six title slots, in the order the format fixes them.
LANGUAGES = ("Japanese", "English", "French", "German", "Italian", "Spanish")

CRC_RANGE = (0x020, 0x840)
ICON = (0x020, 0x220)
PALETTE = (0x220, 0x240)
TITLES = 0x240
TITLE_BYTES = 256


def crc16(buf, init=0xFFFF):
    """CRC-16, reflected polynomial 0xA001. The DS banner's own algorithm."""
    crc = init
    for byte in buf:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def parse(data):
    if len(data) != 0x840:
        raise ValueError(f"{len(data)} bytes, expected 2112")
    version, stored = struct.unpack_from("<HH", data, 0)
    got = crc16(data[CRC_RANGE[0]:CRC_RANGE[1]])
    titles = []
    for i in range(len(LANGUAGES)):
        o = TITLES + i * TITLE_BYTES
        titles.append(data[o:o + TITLE_BYTES].decode("utf-16-le").split("\0")[0])
    return {"version": version, "crc_stored": stored, "crc_computed": got,
            "crc_ok": stored == got, "titles": titles,
            "reserved_zero": data[4:0x20] == bytes(28)}


def icon_rows(data):
    """The 32x32 icon as 32 rows of 32 palette indices, from 4x4 8x8 tiles."""
    px = [[0] * 32 for _ in range(32)]
    tiles = data[ICON[0]:ICON[1]]
    for t in range(16):
        tx, ty = (t % 4) * 8, (t // 4) * 8
        for b in range(32):
            byte = tiles[t * 32 + b]
            y, x = divmod(b * 2, 8)
            px[ty + y][tx + x] = byte & 0xF
            px[ty + y][tx + x + 1] = byte >> 4
    return px


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
    ap.add_argument("--icon", help="write the icon as ASCII art")
    args = ap.parse_args(argv)

    path = os.path.join(args.extract, "plain", BANNER)
    if not os.path.exists(path):
        print(f"missing {path}", file=sys.stderr)
        return 2
    with open(path, "rb") as fh:
        data = fh.read()

    info = parse(data)
    print(f"{BANNER}: {len(data):,} bytes, version 0x{info['version']:04X}")
    print(f"CRC-16 stored 0x{info['crc_stored']:04X}, computed "
          f"0x{info['crc_computed']:04X} -- "
          + ("MATCH" if info["crc_ok"] else "MISMATCH"))
    print(f"reserved bytes 0x04..0x1F are zero: {info['reserved_zero']}")
    print()
    for lang, title in zip(LANGUAGES, info["titles"]):
        shown = title.replace("\n", " / ")
        print(f"   {lang:<10}{shown}")

    distinct = {t for t in info["titles"]}
    print()
    print(f"{len(distinct)} distinct titles across the six slots")
    for t in sorted(distinct):
        who = [l for l, x in zip(LANGUAGES, info["titles"]) if x == t]
        print(f"   {', '.join(who):<40}{t.splitlines()[0]}")

    pal = struct.unpack_from("<16H", data, PALETTE[0])
    used = len({i for row in icon_rows(data) for i in row})
    print()
    print(f"icon: 32x32 at 4bpp, {used} of 16 palette entries used")
    print("   palette: " + " ".join(f"{c:04X}" for c in pal))

    if args.icon:
        os.makedirs(os.path.dirname(os.path.abspath(args.icon)) or ".",
                    exist_ok=True)
        ramp = " .:-=+*#%@$&XW0O1?<>[]{}"
        with open(args.icon, "w", encoding="utf-8", newline="") as fh:
            for row in icon_rows(data):
                fh.write("".join(ramp[i % len(ramp)] for i in row) + chr(10))
        print(f"\nwrote {args.icon}")

    return 0 if info["crc_ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
