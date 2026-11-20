"""Compare two capture folders by DECODED PIXELS, not file bytes.

    py -3.13 tools/lb_pxcompare.py NEW_DIR REF_DIR REGEX [LIMIT]

The harness writes its PNGs with tools/lb_prune_shots.py's own encoder over
the running Python's zlib, and Python 3.14's zlib compresses differently:
docs/388's gate found four composites "differing" by hash, 59,587 against
60,891 bytes, with every pixel identical. So a file is the same when it
decodes to the same pixels (PNG through Pillow, PGM/PPM as raw bytes).
LIMIT drops files whose trailing frame number is >= LIMIT (0: none).
Used by tools/lb_gate.ps1.
"""
import os
import re
import sys

from PIL import Image


def pixels(path):
    if path.lower().endswith(".png"):
        im = Image.open(path)
        return (im.mode, im.size, im.tobytes())
    with open(path, "rb") as fh:
        return fh.read()


new, ref, pat = sys.argv[1], sys.argv[2], re.compile(sys.argv[3])
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 0
same = diff = missing = 0
for name in sorted(os.listdir(ref)):
    if not pat.search(name):
        continue
    if limit:
        m = re.search(r"-(\d+)\.\w+$", name)
        if m and int(m.group(1)) >= limit:
            continue
    g = os.path.join(new, name)
    if not os.path.exists(g):
        missing += 1
        continue
    if pixels(os.path.join(ref, name)) == pixels(g):
        same += 1
    else:
        diff += 1
        print("  DIFFERS " + name)
print("  vs %s: %d identical by pixels, %d differ, %d missing" %
      (os.path.basename(ref.rstrip("/\\")), same, diff, missing))
