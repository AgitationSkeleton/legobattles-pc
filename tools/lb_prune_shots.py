#!/usr/bin/env python3
"""Reclaim disk under screenshots/ WITHOUT losing any frame.

The goal is to clean files that take substantial space without providing
continued benefit, while keeping screenshots and logs for later review with a
timestamp and a description of what was being tested.

The single biggest cost is `.ppm`: **3.33 GB across 7926 files**, against 0.94 GB
of PNG and 0.31 GB of logs. PPM here is an uncompressed RGB dump.

THE OBVIOUS MOVE WAS WRONG. `tools/lb_bands.py` says it "writes a .png beside
each .ppm", which suggested the PPMs were redundant intermediates -- but a sample
of 200 found **194 with no PNG counterpart**. Deleting them would have destroyed
those frames outright.

So convert instead of delete. PNG is lossless, so nothing is lost, and the
frames become *more* reviewable, not less: `tools/lb_shots.py` records that PPM
"does not preview" in a file browser, which is exactly what review needs to do
with them.

A source .ppm is removed only after its .png has been written AND re-read to
confirm identical pixels. Anything that fails verification is left alone.

    python tools/lb_prune_shots.py --dry-run
    python tools/lb_prune_shots.py
"""

import argparse
import os
import struct
import sys
import zlib


def read_ppm(path):
    """Minimal binary PPM (P6) reader. Returns (w, h, rgb) or None."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if not data.startswith(b"P6"):
        return None
    # Header: P6 <w> <h> <maxval>, with # comments allowed between tokens.
    pos = 2
    fields = []
    while len(fields) < 3 and pos < len(data):
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        try:
            fields.append(int(data[start:pos]))
        except ValueError:
            return None
    if len(fields) != 3 or fields[2] != 255:
        return None
    pos += 1                      # exactly one whitespace byte after maxval
    w, h = fields[0], fields[1]
    need = w * h * 3
    rgb = data[pos:pos + need]
    if len(rgb) != need:
        return None
    return w, h, rgb


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = struct.pack(">I", len(payload)) + tag + payload
        return body + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(bytes([137, 80, 78, 71, 13, 10, 26, 10]))
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


def read_png_rgb(path):
    """Read back a PNG this script wrote, so the conversion can be verified."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return None
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    pos, w, h, idat = 8, 0, 0, b""
    while pos + 8 <= len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", payload[:8])
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
        pos += 12 + ln
    if not w or not h:
        return None
    raw = zlib.decompress(idat)
    stride = w * 3
    out = bytearray()
    for y in range(h):
        off = y * (stride + 1)
        if raw[off] != 0:            # this writer only emits filter 0
            return None
        out += raw[off + 1:off + 1 + stride]
    return w, h, bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="screenshots")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    ppms = []
    for base, _dirs, files in os.walk(args.root):
        for fn in files:
            if fn.endswith(".ppm"):
                ppms.append(os.path.join(base, fn))
    ppms.sort()

    before = sum(os.path.getsize(p) for p in ppms)
    print("%d .ppm files, %.2f GB" % (len(ppms), before / 1073741824.0))
    if args.dry_run:
        return 0

    converted = after = skipped = failed = 0
    for i, src in enumerate(ppms):
        dst = src[:-4] + ".png"
        img = read_ppm(src)
        if img is None:
            failed += 1
            continue
        w, h, rgb = img
        if os.path.exists(dst):
            # A PNG is already there; only drop the ppm if it truly matches.
            back = read_png_rgb(dst)
            if back and back == (w, h, rgb):
                os.remove(src)
                skipped += 1
            else:
                failed += 1
            continue
        try:
            write_png(dst, w, h, rgb)
        except OSError:
            failed += 1
            continue
        back = read_png_rgb(dst)
        if not back or back != (w, h, rgb):
            failed += 1                     # leave the source in place
            continue
        after += os.path.getsize(dst)
        os.remove(src)
        converted += 1
        if (i % 500) == 0:
            print("  %d/%d..." % (i, len(ppms)))

    print("converted %d, deduped %d, left alone %d" %
          (converted, skipped, failed))
    print("%.2f GB of ppm -> %.2f GB of png" %
          (before / 1073741824.0, after / 1073741824.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
