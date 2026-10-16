#!/usr/bin/env python3
"""Tidy a screenshot session folder so the RESULTS are what you see.

    python tools/lb_shots.py <session-dir>

A session folder used to be fifteen identical warp-route screenshots at DS
resolution plus, buried among them, the one or two images that were the point of
the run -- and those in `.ppm`, which does not preview. Opening the folder gave
the impression that nothing had changed between runs, because the fifteen
identical files are exactly the ones that catch the eye.

So: route screenshots go into `route/`, `.ppm` dumps become `.png` at the top
level with a `result_` prefix, and the folder's first impression is the work.
"""

import argparse
import glob
import os
import re
import shutil
import struct
import sys
import zlib

ROUTE = re.compile(r"^\d\d_.*\.png$")


def ppm_to_png(src, dst):
    with open(src, "rb") as fh:
        data = fh.read()
    # P6 <w> <h> <max>\n then raw RGB.
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or parts[0] != b"P6":
        return False
    w, h = (int(v) for v in parts[1].split())
    rgb = parts[3]
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--keep-ppm", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.session):
        print("no such folder: %s" % args.session, file=sys.stderr)
        return 2

    moved = converted = 0
    route_dir = os.path.join(args.session, "route")
    for name in sorted(os.listdir(args.session)):
        path = os.path.join(args.session, name)
        if os.path.isfile(path) and ROUTE.match(name):
            os.makedirs(route_dir, exist_ok=True)
            shutil.move(path, os.path.join(route_dir, name))
            moved += 1

    for src in sorted(glob.glob(os.path.join(args.session, "*.ppm"))):
        stem = os.path.splitext(os.path.basename(src))[0]
        dst = os.path.join(args.session, "result_%s.png" % stem)
        if ppm_to_png(src, dst):
            converted += 1
            if not args.keep_ppm:
                os.remove(src)

    print("%s: %d route shots -> route/, %d dumps -> result_*.png"
          % (os.path.basename(args.session), moved, converted))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
