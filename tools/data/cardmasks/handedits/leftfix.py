"""Add v10's own leftover pixels (stock team hue, inside the unit, unmasked) as exact-pixel edits.
py -3.13 leftfix.py NAME [NAME...]   -- rows >= 87 (the HP band) are never added.
Then: py -3.13 leftfix.py --sheet NAME... -> mine/view/_sheet.png (navy: v10 | edited, 3x)"""
import sys, os, json
import numpy as np
from PIL import Image, ImageDraw
import edit, render, cardv3

if sys.argv[1] == "--sheet":
    names = sys.argv[2:]
    S = 3
    tiles = []
    for n in names:
        mask, alpha, R = edit.apply(n)
        c = R["c"]; op = c["opaque"]
        row = []
        for team in ("navy", "purple"):
            r10 = cardv3.recolour(c, R["mask"], R["alpha"], cardv3.TEAMS[team], R["p"]); r10[~op] = 0
            rn = cardv3.recolour(c, mask, alpha, cardv3.TEAMS[team], R["p"]); rn[~op] = 0
            row += [r10, rn]
        tiles.append((n, row))
    W = 4 * (96 * S + 4) + 4
    im = Image.new("RGB", (W, len(tiles) * (96 * S + 16)), (24, 24, 24))
    d = ImageDraw.Draw(im)
    for i, (n, row) in enumerate(tiles):
        y = i * (96 * S + 16)
        d.text((2, y + 1), n + "   navy v10 | navy edited | purple v10 | purple edited", fill=(255, 255, 255))
        for j, t in enumerate(row):
            im.paste(Image.fromarray(t).resize((96 * S, 96 * S), Image.NEAREST), (2 + j * (96 * S + 4), y + 14))
    p = os.path.join(edit.HERE, "view", "_sheet.png")
    im.save(p)
    print(p)
    sys.exit(0)

edits = edit.load_edits()
for n in sys.argv[1:]:
    R = render.compute(n)
    left = R["lmap"] & ~R["mask"]
    left[87:, :] = False
    pix = [[int(x), int(y)] for y, x in zip(*np.nonzero(left))]
    if not pix:
        print(n, "no leftovers")
        continue
    lst = [e for e in edits.get(n, []) if e.get("why") != "v10 leftover pixels"]
    lst.append({"op": "add", "pixels": pix, "why": "v10 leftover pixels"})
    edits[n] = lst
    print(n, len(pix), "pixels added")
with open(edit.EDITS_PATH, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(edits, fh, indent=1)
