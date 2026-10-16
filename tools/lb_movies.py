#!/usr/bin/env python3
"""`Movies/*.mods` -- 24 full-motion videos, 34.6 MB, and their keyframe index.

`docs/04` filed these as opaque: "no known structure". They are not opaque. The
first eight bytes are `MODS` followed by `N3\\n\\0`, and a 64-byte header
follows that states the frame count, the picture size, the frame rate, the audio
rate and the offsets of two trailing structures.

    +0x00  char[4]  "MODS"
    +0x04  char[4]  "N3" 0x0A 0x00
    +0x08  u32      frame count
    +0x0C  u32      width, 256 in all 24
    +0x10  u32      height, 144 in all 24
    +0x14  u32      0x0F000000 in all 24 -- 15 frames/second
    +0x18  u32      0x00010001 in all 24
    +0x1C  u32      32000 in all 24 -- the audio sample rate
    +0x20  u32      largest frame, in bytes
    +0x24  u32      offset of a 3,124-byte per-movie block
    +0x28  u32      offset of the keyframe index
    +0x2C  u32      keyframe count
    +0x30  u32      0x4548 in all 24
    +0x34  u32      per-movie, unexplained
    +0x38..0x3F     unexplained

## What is decoded and what is not

The **container** is decoded: header, sizes, keyframe index. The **bitstream**
is not -- `MODS` is Mobiclip, a proprietary codec, and nothing here decodes a
frame. So these files are counted **partial**, by the same rule `docs/49` used
for the Nitro formats: a format is decoded when a parser here reads it *and a
consumer can use the result*. A frame table is not a picture.

## The identities

Four, each over all 24 files:

1. `file size == (+0x28) + 8 * (+0x2C)` -- the keyframe index ends the file.
2. The index entries are `(frame, offset)` pairs, both **strictly increasing**.
3. The first entry is frame 0 at offset **52** in every file, and the frame data
   starts at 0x40, so **offsets are measured from +0x0C** (52 + 12 = 64).
4. The last keyframe's frame number is below the header's frame count, and its
   offset is below the block at +0x24.

Identity 3 is the one that had to be read rather than assumed: an offset table
whose first entry is 52 in a file whose data starts at 64 looks broken until the
constant 12 falls out of it, and 52 is not a plausible accident 24 times.

Usage:
    python tools/lb_movies.py
    python tools/lb_movies.py --tsv docs/data/movies.tsv
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

MOVIES = "Movies"
MAGIC = b"MODS"

#: Keyframe offsets are relative to header offset 12, not to the file start.
OFFSET_BASE = 0x0C

#: Header words this project has read out of the population, with the value
#: every one of the 24 files carries.
CONSTANTS = {0x0C: 256, 0x10: 144, 0x14: 0x0F000000, 0x18: 0x00010001,
             0x1C: 32000, 0x30: 0x4548}


def parse(data):
    if data[:4] != MAGIC:
        raise ValueError(f"magic is {data[:4]!r}, not MODS")
    tag = data[4:8]
    (frames, w, h, fps, audio, rate, biggest, blk, idx, keys, k30,
     x34) = struct.unpack_from("<12I", data, 8)
    if not (0x40 < blk < idx <= len(data)):
        raise ValueError(f"offsets out of order: 0x{blk:X}, 0x{idx:X}")
    ents = [struct.unpack_from("<II", data, idx + i * 8) for i in range(keys)]
    return {"tag": tag.decode("latin-1"), "frames": frames,
            "width": w, "height": h, "fps": fps >> 24, "fps_raw": fps,
            "audio": audio, "sample_rate": rate, "biggest_frame": biggest,
            "block_offset": blk, "block_size": idx - blk,
            "index_offset": idx, "keyframes": keys,
            "word_30": k30, "word_34": x34, "entries": ents}


def checks(data, info):
    """{name: bool} -- the four identities, evaluated on one file."""
    ents = info["entries"]
    fr = [e[0] for e in ents]
    of = [e[1] for e in ents]
    return {
        "index_ends_file":
            len(data) == info["index_offset"] + 8 * info["keyframes"],
        "index_increasing":
            fr == sorted(fr) and len(set(fr)) == len(fr)
            and of == sorted(of) and len(set(of)) == len(of),
        "first_is_frame_zero_at_52":
            bool(ents) and fr[0] == 0 and of[0] == 0x40 - OFFSET_BASE,
        "index_in_range":
            bool(ents) and fr[-1] < info["frames"]
            and of[-1] + OFFSET_BASE < info["block_offset"],
    }


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

    root = os.path.join(args.extract, "plain", MOVIES)
    if not os.path.isdir(root):
        print(f"missing {root}", file=sys.stderr)
        return 2

    rows = []
    tally = collections.defaultdict(collections.Counter)
    const_bad = collections.Counter()
    blocks = set()
    for name in sorted(os.listdir(root)):
        with open(os.path.join(root, name), "rb") as fh:
            data = fh.read()
        try:
            info = parse(data)
        except ValueError as e:
            print(f"   FAIL {name}: {e}")
            continue
        for key, want in CONSTANTS.items():
            got = struct.unpack_from("<I", data, key)[0]
            if got != want:
                const_bad[key] += 1
        for k, v in checks(data, info).items():
            tally[k][v] += 1
        blocks.add(hashlib.sha1(
            data[info["block_offset"]:info["index_offset"]]).hexdigest())
        rows.append((name, len(data), info))

    print(f"{len(rows)} MODS files")
    print()
    print(f"{'file':<22}{'bytes':>10}{'frames':>8}{'sec':>7}{'keys':>6}"
          f"{'biggest':>9}{'w x h':>10}")
    for name, size, i in rows:
        print(f"{name:<22}{size:>10,}{i['frames']:>8}"
              f"{i['frames'] / i['fps']:>7.1f}{i['keyframes']:>6}"
              f"{i['biggest_frame']:>9,}"
              f"{f'{i[chr(119) + chr(105) + chr(100) + chr(116) + chr(104)]}x{i[chr(104) + chr(101) + chr(105) + chr(103) + chr(104) + chr(116)]}':>10}")

    total_frames = sum(i["frames"] for _n, _s, i in rows)
    total_bytes = sum(s for _n, s, _i in rows)
    fps = rows[0][2]["fps"] if rows else 0
    print()
    print(f"total: {total_bytes:,} bytes, {total_frames:,} frames, "
          f"{total_frames / fps / 60:.1f} minutes at {fps} fps")

    print()
    print("header words constant across all files:")
    for key, want in sorted(CONSTANTS.items()):
        note = {0x0C: "width", 0x10: "height", 0x14: "15 fps, in the high byte",
                0x18: "", 0x1C: "audio sample rate", 0x30: ""}.get(key, "")
        print(f"   +0x{key:02X}  0x{want:08X} = {want:<12} "
              f"{len(rows) - const_bad[key]} of {len(rows)}   {note}")

    print()
    print("identities, over every file:")
    for k in ("index_ends_file", "index_increasing",
              "first_is_frame_zero_at_52", "index_in_range"):
        c = tally[k]
        print(f"   {k:<28}{c[True]:>4} hold, {c[False]:>3} do not")

    print()
    print(f"the 3,124-byte block at +0x24 is per-movie: "
          f"{len(blocks)} distinct in {len(rows)} files")
    print("the Mobiclip bitstream is NOT decoded; these count as partial")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["file", "bytes", "frames", "seconds", "width",
                               "height", "fps", "sample_rate",
                               "biggest_frame", "keyframes", "block_offset",
                               "index_offset"]) + nl)
            for name, size, i in rows:
                fh.write(tab.join([
                    name, str(size), str(i["frames"]),
                    "%.1f" % (i["frames"] / i["fps"]), str(i["width"]),
                    str(i["height"]), str(i["fps"]), str(i["sample_rate"]),
                    str(i["biggest_frame"]), str(i["keyframes"]),
                    "0x%X" % i["block_offset"], "0x%X" % i["index_offset"],
                ]) + nl)
        print(f"\nwrote {args.tsv} ({len(rows)} files)")

    bad = sum(c[False] for c in tally.values()) + sum(const_bad.values())
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
