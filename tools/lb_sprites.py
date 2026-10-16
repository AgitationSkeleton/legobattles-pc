#!/usr/bin/env python3
"""Rip the cartridge's unit and building sprites to PNG.

The graphics are ordinary Nintendo Nitro containers, usually inside this game's
own COMP wrapper (tools/lb_comp.py), so the whole job is: unwrap, read the CHAR
section, expand 4bpp indices through an NCLR palette, and write a PNG.

Two details are worth stating because getting either wrong produces a plausible
but wrong picture:

* **CHAR width and height are in 8x8 TILES, not pixels.** `a_bld_mtd` reports
  32x32 and is a 256x256 image.
* **.NCBR is the LINEAR variant of .NCGR.** Same container, same section, but
  the pixels run left-to-right across the whole image rather than tile by tile.
  Decoding one as the other gives a scrambled image that still looks like it has
  the right colours, which is exactly the kind of wrong that goes unnoticed.

Palettes are 4bpp here, so an NCLR of 256 colours is sixteen 16-colour palettes
and a sheet has to say which one it wants. Nothing in the sheet does say, so
every palette is written unless one is named; `--palette-index` picks one.

Usage:
    python tools/lb_sprites.py                       # Sprites/ -> out/sprites
    python tools/lb_sprites.py --dir UI --dest out/ui
    python tools/lb_sprites.py --sheets              # also write contact sheets

Output goes under out/, which .gitignore keeps out of the repository: it is
cartridge-derived and is not ours to distribute.
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lb_comp
import lb_nitro


def load(path):
    """Read a file, unwrapping the COMP container when there is one."""
    raw = open(path, "rb").read()
    return lb_comp.unwrap(raw) if lb_comp.is_comp(raw) else raw


def read_palettes(path):
    """NCLR -> list of palettes, each a list of (r, g, b) tuples.

    DS colour is 5 bits per channel. Expanded the way the hardware does it,
    through a 6-bit intermediate -- a naive 5->8 shift is measurably wrong
    against the live framebuffer (docs/169 records the same trap in lb_field).
    """
    data = load(path)
    _fmt, _ver, sections, _end = lb_nitro.parse(data)
    for smag, off, _size in sections:
        if smag != "TTLP":
            continue
        bppflag, _pad, dsize, doff = struct.unpack_from("<IIII", data, off + 8)
        base = off + 8 + doff
        count = dsize // 2
        colours = []
        for i in range(count):
            v = struct.unpack_from("<H", data, base + i * 2)[0]
            r6, g6, b6 = (v & 31) * 2, ((v >> 5) & 31) * 2, ((v >> 10) & 31) * 2
            colours.append((((r6 << 2) | (r6 >> 4)) & 0xFF,
                            ((g6 << 2) | (g6 >> 4)) & 0xFF,
                            ((b6 << 2) | (b6 >> 4)) & 0xFF))
        per = 256 if bppflag == 4 else 16
        return [colours[i:i + per] for i in range(0, len(colours), per)] or [colours]
    return []


def read_graphic(path):
    """NCGR/NCBR -> (width_px, height_px, indices, bpp, linear)."""
    data = load(path)
    _fmt, _ver, sections, _end = lb_nitro.parse(data)
    for smag, off, _size in sections:
        if smag != "RAHC":
            continue
        th, tw, bppflag = struct.unpack_from("<HHI", data, off + 8)
        # The two words between the depth and the data describe the layout; the
        # low bit of the second is set for the linear (.NCBR) arrangement.
        _u1, layout = struct.unpack_from("<II", data, off + 16)
        dsize, doff = struct.unpack_from("<II", data, off + 24)
        base = off + 8 + doff
        blob = data[base:base + dsize]
        bpp = 8 if bppflag == 4 else 4
        linear = bool(layout & 1) or path.lower().endswith(".ncbr")

        # A sheet with an unknown height (0xFFFF) is sized from its data.
        w = tw * 8
        h = th * 8
        if tw in (0, 0xFFFF) or th in (0, 0xFFFF) or w <= 0 or h <= 0:
            w = 256
            h = max(1, (len(blob) * 8 // bpp) // w)

        px = bytearray(w * h)
        if bpp == 4:
            flat = bytearray(len(blob) * 2)
            for i, b in enumerate(blob):
                flat[i * 2] = b & 0xF
                flat[i * 2 + 1] = b >> 4
        else:
            flat = bytearray(blob)

        if linear:
            n = min(len(flat), w * h)
            px[:n] = flat[:n]
        else:
            # Tiled: 8x8 blocks in reading order.
            tiles_x = w // 8
            for t in range(len(flat) // 64):
                tx, ty = (t % tiles_x) * 8, (t // tiles_x) * 8
                if ty + 8 > h:
                    break
                for r in range(8):
                    row = flat[t * 64 + r * 8:t * 64 + r * 8 + 8]
                    start = (ty + r) * w + tx
                    px[start:start + 8] = row
        return w, h, px, bpp, linear
    return None


def write_png(path, w, h, rgba):
    """Minimal RGBA PNG writer -- avoids a Pillow dependency in this tool."""
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


def colourise(w, h, px, palette):
    """Index 0 is the transparent entry throughout this cartridge's sprites."""
    out = bytearray(w * h * 4)
    for i, v in enumerate(px):
        if v == 0:
            continue
        r, g, b = palette[v] if v < len(palette) else (255, 0, 255)
        out[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=os.path.join("out", "eur"))
    ap.add_argument("--dir", default="Sprites", help="NitroFS subdirectory to rip")
    ap.add_argument("--dest", default=os.path.join("out", "sprites"))
    ap.add_argument("--palette", help="explicit .NCLR to use for every sheet")
    ap.add_argument("--palette-index", type=int,
                    help="write only this sub-palette instead of all of them")
    args = ap.parse_args(argv)

    src = os.path.join(args.extract, "nitrofs", args.dir)
    if not os.path.isdir(src):
        print("no such directory: %s" % src, file=sys.stderr)
        return 2
    os.makedirs(args.dest, exist_ok=True)

    names = sorted(os.listdir(src))
    nclr = [n for n in names if n.lower().endswith(".nclr")]
    if args.palette:
        pal_path = args.palette
    elif nclr:
        pal_path = os.path.join(src, nclr[0])
    else:
        print("no .NCLR in %s; pass --palette" % src, file=sys.stderr)
        return 2

    palettes = read_palettes(pal_path)
    print("palette %s -> %d sub-palette(s) of %d colours"
          % (os.path.basename(pal_path), len(palettes),
             len(palettes[0]) if palettes else 0))

    wanted = range(len(palettes)) if args.palette_index is None else [args.palette_index]
    index = []
    ok = bad = 0
    for name in names:
        if not name.lower().endswith((".ncgr", ".ncbr")):
            continue
        path = os.path.join(src, name)
        try:
            got = read_graphic(path)
        except Exception as exc:                      # noqa: BLE001
            print("  %-28s FAILED: %s" % (name, exc))
            bad += 1
            continue
        if not got:
            print("  %-28s no CHAR section" % name)
            bad += 1
            continue
        w, h, px, bpp, linear = got
        stem = os.path.splitext(name)[0]
        for pi in wanted:
            if pi >= len(palettes):
                continue
            suffix = "" if len(list(wanted)) == 1 else ".p%02d" % pi
            out = os.path.join(args.dest, "%s%s.png" % (stem, suffix))
            write_png(out, w, h, colourise(w, h, px, palettes[pi]))
        index.append((stem, w, h, bpp, "linear" if linear else "tiled"))
        ok += 1

    with open(os.path.join(args.dest, "index.tsv"), "w", newline="") as fh:
        fh.write("name\twidth\theight\tbpp\tlayout\n")
        for row in index:
            fh.write("%s\t%d\t%d\t%d\t%s\n" % row)

    print("wrote %d sheet(s) to %s (%d failed)" % (ok, args.dest, bad))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
