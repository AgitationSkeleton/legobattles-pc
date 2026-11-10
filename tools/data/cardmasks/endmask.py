"""End-screen masks: per image a stock team hue window inside hand-drawn figure polygons (256x192),
minus exclusion polygons; tidy + soft alpha; recolour with the same cardv3.recolour as the cards.
Params in endparams.json. Writes endscreen.png (original | mask | purple | teal | navy, 2x) and
endmasks/<name>.png (alpha)."""
import os, sys, json
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
H = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, H)
import cardv3, endcards
ST4 = ndi.generate_binary_structure(2, 1)


def polymask(polys, h, w):
    im = Image.new("L", (w, h), 0); d = ImageDraw.Draw(im)
    for p in polys or []:
        d.polygon([tuple(v) for v in p], fill=255)
    return np.asarray(im) > 0


def compute(name, P):
    c = endcards.load(name); h, w = c["I"].shape
    lab = cardv3.rgb2lab(c["rgb"].astype(float)); L = lab[..., 0]; C = np.hypot(lab[..., 1], lab[..., 2]); Hh = cardv3.hue_deg(lab)
    mask = np.zeros((h, w), bool)
    for part in P.get("parts", []):
        q = dict(P); q.pop("parts", None); q.update(part)
        reg = polymask([q["poly"]] if q.get("poly") else [], h, w)
        if q.get("hue") is not None and q.get("hue_tol", 181) < 180:
            reg &= cardv3.hdist(Hh, q["hue"]) < q["hue_tol"]
        reg &= (C >= q.get("C_min", 0)) & (C <= q.get("C_max", 999)) & (L >= q.get("L_min", 0)) & (L <= q.get("L_max", 101))
        if q.get("drop_hue") is not None:
            reg &= ~((cardv3.hdist(Hh, q["drop_hue"]) < q.get("drop_tol", 20)) & (C >= q.get("drop_Cmin", 15)))
        if not q.get("poly"): continue
        if q.get("close", 1): reg = ndi.binary_closing(reg, ST4, iterations=q.get("close", 1)) & polymask([q["poly"]], h, w)
        lb, n = ndi.label(reg)
        if n:
            sz = ndi.sum(reg, lb, range(1, n + 1))
            for i, s in enumerate(sz, 1):
                if s < q.get("island", 4): reg[lb == i] = False
        mask |= reg
    excl = polymask(P.get("excl_poly"), h, w)
    mask &= ~excl
    a = ndi.gaussian_filter(mask.astype(float), 0.65)
    a = np.where(mask, np.clip(a * 1.25, 0, 1), a * 0.8); a[~ndi.binary_dilation(mask)] = 0; a[excl] = 0
    return c, mask, np.clip(a, 0, 1)


if __name__ == "__main__":
    P = json.load(open(os.path.join(H, "endparams.json"), encoding="utf-8-sig"))
    only = sys.argv[1:]
    import lbcard; OUTD = lbcard.OUT; os.makedirs(os.path.join(OUTD, "endmasks"), exist_ok=True); os.makedirs(os.path.join(OUTD, "work"), exist_ok=True)
    rows = []
    for name in (only or sorted(P)):
        p = P[name]
        c, m, a = compute(name, p)
        Image.fromarray((a * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUTD, "endmasks", name + ".png"))
        o = c["rgb"]
        ov = o.astype(float); A = a[..., None]; ov = (ov * (1 - A * 0.75) + np.array([255, 0, 255]) * A * 0.75).astype(np.uint8)
        q = dict(chroma_keep=p.get("chroma_keep", 0.35))
        tiles = [o, ov] + [cardv3.recolour(c, m, a, cardv3.TEAMS[t], q) for t in ("purple", "teal", "navy")]
        row = Image.new("RGB", (5 * 516, 384 + 16), (20, 20, 20))
        for i, t in enumerate(tiles): row.paste(Image.fromarray(t).resize((512, 384), Image.NEAREST), (i * 516, 16))
        ImageDraw.Draw(row).text((2, 2), "%s  [original | mask | purple | teal | navy]  %s" % (name, p.get("note", "")), fill=(255, 255, 255))
        rows.append(row)
    out = "work/end_%s.png" % ("_".join(only)[:40] if only else "all")
    sh = Image.new("RGB", (rows[0].width, rows[0].height * len(rows)))
    for i, r in enumerate(rows): sh.paste(r, (0, i * r.height))
    sh.save(os.path.join(OUTD, out if only else "endscreen.png"))
    for k in range(0, len(rows), 6):
        s2 = sh.crop((0, k * rows[0].height, sh.width, min(sh.height, (k + 6) * rows[0].height)))
        s2.resize((s2.width // 3, s2.height // 3)).save(os.path.join(OUTD, "work", "endrev_%d.png" % (k // 6)))
    print(len(rows))
