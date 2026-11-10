"""Hand edits on top of the frozen v10 card masks.

edits.json: {card name: [edit, ...]}. An edit:
  {"op": "add" | "sub", "poly": [[x, y], ...]}   or "rect": [x0, y0, x1, y1] (inclusive)
  optional for "add": "hue": [centre_deg, half_width_deg], "cmin": chroma, "lmin"/"lmax": Lab L
  "sub" removes every pixel of the area from the mask.
Usage:
  py -3.13 edit.py view NAME [NAME...]   -> mine/view/NAME.png (8x grid + v10/edited previews)
  py -3.13 edit.py list                  -> card names
"""
import os, sys, json
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
V10 = os.path.join(os.path.dirname(HERE), "v10")
sys.path.insert(0, V10)
import render, cardv3  # noqa: E402

EDITS_PATH = os.path.join(HERE, "edits.json")


def load_edits():
    if not os.path.exists(EDITS_PATH):
        return {}
    with open(EDITS_PATH, encoding="utf-8") as fh:
        return json.load(fh)


def area(e, shape):
    h, w = shape
    m = np.zeros((h, w), bool)
    if "pixels" in e:
        for x, y in e["pixels"]:
            if 0 <= x < w and 0 <= y < h:
                m[y, x] = True
        return m
    if "rect" in e:
        x0, y0, x1, y1 = e["rect"]
        m[max(0, y0):min(h, y1 + 1), max(0, x0):min(w, x1 + 1)] = True
        return m
    yy, xx = np.mgrid[0:h, 0:w] + 0.5
    P = np.array(e["poly"], float)
    inside = np.zeros((h, w), bool)
    n = len(P)
    for i in range(n):
        x1, y1 = P[i]
        x2, y2 = P[(i + 1) % n]
        cond = (y1 > yy) != (y2 > yy)
        xint = (x2 - x1) * (yy - y1) / ((y2 - y1) if y2 != y1 else 1e-9) + x1
        inside ^= cond & (xx < xint)
    return inside


def select(e, c, base):
    lab = cardv3.rgb2lab(c["rgb"].astype(float))
    sel = base & c["opaque"]
    if "hue" in e:
        hc, hw = e["hue"]
        sel &= cardv3.hdist(cardv3.hue_deg(lab), hc) <= hw
    if "cmin" in e:
        sel &= np.hypot(lab[..., 1], lab[..., 2]) >= e["cmin"]
    if "lmin" in e:
        sel &= lab[..., 0] >= e["lmin"]
    if "lmax" in e:
        sel &= lab[..., 0] <= e["lmax"]
    return sel


def apply(n, R=None, edits=None):
    R = R or render.compute(n)
    edits = load_edits() if edits is None else edits
    c = R["c"]
    mask = R["mask"].copy()
    subj = R["subj"].copy()
    excl = R["excl"].copy()
    removed = np.zeros_like(mask)
    for e in edits.get(n, []):
        a = area(e, mask.shape)
        if e["op"] == "add":
            sel = select(e, c, a)
            if e.get("in_subject"):
                sel &= R["subj"] & ~R["bg"]   # solid fill, but never past the unit's outline
            mask |= sel
            subj |= sel
            excl &= ~sel
            removed &= ~sel
        else:
            if any(k in e for k in ("hue", "cmin", "lmin", "lmax")):
                a = select(e, c, a)   # remove only the matching pixels of the area
            mask &= ~a
            removed |= a
    if not edits.get(n):
        return R["mask"], R["alpha"], R
    alpha = cardv3.soft_alpha(mask, R["bg"] & ~subj, excl | removed)
    alpha[~subj] = 0
    return mask, alpha, R


def view(n, out_dir):
    mask, alpha, R = apply(n)
    c = R["c"]
    op = c["opaque"]
    orig = c["rgb"].copy()
    orig[~op] = 0
    S = 8
    big = Image.fromarray(orig).resize((96 * S, 96 * S), Image.NEAREST).convert("RGB")
    d = ImageDraw.Draw(big)
    for g in range(0, 97, 8):
        col = (255, 255, 255) if g % 16 == 0 else (110, 110, 110)
        d.line([(g * S, 0), (g * S, 96 * S)], fill=col, width=1)
        d.line([(0, g * S), (96 * S, g * S)], fill=col, width=1)
    # mask edge in cyan, v10 edge where it differs in orange
    def edges(m):
        e = m & ~(np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1))
        return np.argwhere(e)
    for y, x in edges(mask):
        d.rectangle([x * S + 2, y * S + 2, x * S + S - 3, y * S + S - 3], outline=(0, 255, 255))
    for y, x in np.argwhere(mask != R["mask"]):
        d.rectangle([x * S + 1, y * S + 1, x * S + S - 2, y * S + S - 2], outline=(255, 140, 0))
    W = 96 * S + 40
    im = Image.new("RGB", (W + 2 * 96 * 4 + 20, 96 * S + 40), (24, 24, 24))
    im.paste(big, (30, 30))
    D = ImageDraw.Draw(im)
    for g in range(0, 96, 8):
        D.text((30 + g * S + 1, 16), str(g), fill=(255, 255, 0))
        D.text((2, 30 + g * S + 1), str(g), fill=(255, 255, 0))
    D.text((2, 2), "%s  (cyan = edited mask edge, orange = changed vs v10)" % n, fill=(255, 255, 255))
    x0 = W + 10
    for row, team in enumerate(("purple", "navy")):
        r10 = cardv3.recolour(c, R["mask"], R["alpha"], cardv3.TEAMS[team], R["p"]); r10[~op] = 0
        rnew = cardv3.recolour(c, mask, alpha, cardv3.TEAMS[team], R["p"]); rnew[~op] = 0
        y = 30 + row * (96 * 4 + 24)
        D.text((x0, y - 12), team + ": v10 | edited", fill=(255, 255, 255))
        im.paste(Image.fromarray(r10).resize((96 * 4, 96 * 4), Image.NEAREST), (x0, y))
        im.paste(Image.fromarray(rnew).resize((96 * 4, 96 * 4), Image.NEAREST), (x0 + 96 * 4 + 6, y))
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, n + ".png")
    im.save(p)
    return p


if __name__ == "__main__":
    if sys.argv[1] == "list":
        print("\n".join(sorted(render.C)))
    elif sys.argv[1] == "view":
        for n in sys.argv[2:]:
            print(view(n, os.path.join(HERE, "view")))
