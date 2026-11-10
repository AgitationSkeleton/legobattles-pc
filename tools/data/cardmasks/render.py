"""Round 3 panels: original | soft mask | purple | teal | navy | defects (leftover red, bleed yellow).
Usage: render.py OUTNAME card [card ...]  ('named', 'alien')."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cardv3, regions, params as P
import lbcard  # noqa: E402  (parent dir is on the path via cardv3)
C = cardv3.cardlib.load()
DATA = cardv3.HERE
H = lbcard.OUT   # previews and masks go to out/ (untracked), parameters stay in this folder
for d in ("panels", "work", "masks"): os.makedirs(os.path.join(H, d), exist_ok=True)
RECS = {r["index"]: r for r in lbcard.records()}
NAMED = ["Wall", "A_AlienKing", "A_Melee", "SpaceCriminalLeader", "SpaceCriminal", "NinjaKing", "W_Crossbowman", "P_Captain",
         "Santa", "ClassicSpace", "Shark", "IslanderChief", "A_Ranged"]
ALIEN = ["A_SiegeOne", "A_SiegeTwo", "A_SiegeThree", "A_TransportShip", "A_CraterBase", "A_Harvester", "A_WellCap",
         "A_Hatchery", "A_BreedingPit", "A_CreationPit", "A_Spire", "A_Spire2", "A_Spire3", "A_Shipyard", "A_Engineer", "A_Mounted"]
BUILDING_DEFAULTS = dict(excl=[[0, 86, 95, 95]], island=3, hole_max=12, grow_step=13.0, hue_tol=22.0, chroma_free=0.0)
SKY = ('NinjaKing', 'Ninja', 'WarJunk', 'NinjaTemple', 'DwarfKing', 'Dwarf', 'DwarfMineDefender', 'DwarvenHall')


# Faction buildings: the team parts are saturated LEGO bricks (roofs, banners, shields, domes),
# selected by hue, crisply, including the small ones. Lab hues: blue ~290, red ~40, orange ~55.
FACTION_BUILDING = {
    "K_": dict(mode="hue", hue=302.0, hue_tol=22.0, C_min=40.0, L_min=8.0, island=2, note="blue roofs, shields, flags (hue-selected)"),
    "I_": dict(mode="hue", hue=302.0, hue_tol=22.0, C_min=40.0, L_min=8.0, island=2, note="blue roofs and flags (hue-selected)"),
    "W_": dict(mode="hue", hue=30.0, hue_tol=14.0, C_min=50.0, L_min=15.0, island=2, note="red roofs and banners (hue-selected)"),
    "P_": dict(mode="hue", hue=30.0, hue_tol=14.0, C_min=50.0, L_min=15.0, island=2, note="red roofs and cloth (hue-selected)"),
    "E_": dict(mode="hue", hue=52.0, hue_tol=26.0, C_min=28.0, L_min=15.0, island=2, close=1, fill=2, note="orange domes and panels (hue-selected)"),
}


import json as _json
_TH = os.path.join(DATA, "team_hues.json")
TEAM_HUES = _json.load(open(_TH)) if os.path.exists(_TH) else {}


def params_for(n):
    c = C[n]; r = RECS.get(c["bp"])
    p = dict(P.DEFAULT)
    p["open"] = 0
    if r is None or r["category"] >= 7: p.update(BUILDING_DEFAULTS)
    if n[:2] in ('K_', 'I_') or n in SKY: p.update(bg_tol=15.0, bg_relax=1.9)
    if n.startswith("A_") and (r is None or r["category"] >= 5):
        # alien buildings and ships: the team parts are the yellow-green tubes, the yellow lights
        # and panels (the sprites' entries 12-14). Small and scattered: select them by hue, crisply.
        p.update(mode="hue", hue=108.0, hue_tol=36.0, C_min=25.0, L_min=25.0, island=2, close=1, fill=1,
                 note="yellow-green tubes, lights and panels (hue-selected)")
    if r is not None and r["category"] >= 7 and n[:2] in FACTION_BUILDING:
        p.update(FACTION_BUILDING[n[:2]])
    own = P.all_params().get(n, {})
    stock = TEAM_HUES.get(n, {}).get("stock_hue")
    if stock is None and not own:
        p["disabled"] = True
        p["note"] = "stock: " + TEAM_HUES.get(n, {}).get("evidence", "no team part")
    p.update(own)
    # every part seeds and grows from the card's STOCK team hue (team_hues.json), unless the
    # card's own parameters name a hue for that part
    if stock is not None:
        def pin(q, ownq):
            if q.get("mode") == "hue":
                if "hue" not in ownq: q["hue"] = stock
            elif "seed_hue" not in ownq:
                q["seed_hue"] = stock
        if p.get("parts"):
            p["parts"] = [dict(pt) for pt in p["parts"]]
            for pt in p["parts"]:
                merged = dict(p); merged.update(pt)
                if merged.get("mode") == "hue" and "hue" not in pt: pt["hue"] = stock
                elif merged.get("mode", "grow") != "hue" and "seed_hue" not in pt and "seed_hue" not in own: pt["seed_hue"] = stock
        else:
            pin(p, own)
    p["stock_hue"] = stock
    # hand-drawn parts: {"poly": [[x,y],...], ...colour limits}. The polygon bounds the part, the
    # backdrop flood is ignored inside it, and it is added to the unit's silhouette.
    polys = []
    if p.get("parts"):
        for pt in p["parts"]:
            if pt.get("lines"):   # hand-traced thin parts: polylines of width w, one quad per segment
                quads = []
                w = pt.get("w", 3.0) / 2.0
                for line in pt["lines"]:
                    for (x1, y1), (x2, y2) in zip(line[:-1], line[1:]):
                        dx, dy = x2 - x1, y2 - y1
                        L = max(1e-6, (dx * dx + dy * dy) ** 0.5)
                        nx, ny = -dy / L * w, dx / L * w
                        ex, ey = dx / L * w * 0.6, dy / L * w * 0.6
                        quads.append([[x1 + nx - ex, y1 + ny - ey], [x2 + nx + ex, y2 + ny + ey], [x2 - nx + ex, y2 - ny + ey], [x1 - nx - ex, y1 - ny - ey]])
                pt["within_poly"] = quads; pt["within"] = []; pt["ignore_bg"] = True
                pt.setdefault("rim", 0); pt.setdefault("mode", "hue"); pt.setdefault("island", 1)
                if "hue" not in pt: pt["hue"] = stock if stock is not None else 0
                polys.extend(quads)
            if pt.get("poly"):
                pt["within_poly"] = [pt["poly"]]; pt.setdefault("ignore_bg", False); pt.setdefault("rim", 0); pt.setdefault("within", [])
                pt.setdefault("mode", "hue")
                if pt["ignore_bg"] and not pt.get("outline_part"): polys.append(pt["poly"])
                if "hue" not in pt and pt.get("mode") == "hue": pt["hue"] = stock if stock is not None else 0
    if polys:
        p["subject_add_poly"] = list(p.get("subject_add_poly") or []) + polys
    return p


def subject_mask(c, p, bg):
    """The unit/structure silhouette: non-backdrop pixels, outline interiors filled, keeping the
    large components and any component with a dark outline (L < 22, 4+ px); small smooth
    components (backdrop shadows and brush blobs) are dropped. Hand fixes: subject_poly
    (replaces), subject_add / subject_cut (rects)."""
    from scipy import ndimage as ndi
    op = c["opaque"]
    if p.get("subject_poly"):
        s = cardv3.poly_mask(p["subject_poly"]) & op
    else:
        s = op & ~bg
        s[87:, :] &= ~bg[87:, :]
        lab = cardv3.rgb2lab(c["rgb"].astype(float))
        dark = lab[..., 0] < 22
        lb, k = ndi.label(s)
        if k:
            sz = ndi.sum(s, lb, range(1, k + 1)); big = sz.max()
            keep = np.zeros_like(s)
            for i, z in enumerate(sz, 1):
                comp = lb == i
                if z >= max(40, 0.10 * big) or (dark & comp).sum() >= 4:
                    keep |= comp
            s = keep
        s = ndi.binary_fill_holes(s) & op
    s |= (cardv3.rect_mask(p.get("subject_add")) | cardv3.poly_mask(p.get("subject_add_poly"))) & op
    s &= ~cardv3.rect_mask(p.get("subject_cut"))
    return s


def outline_backdrop(c, dark_L=32.0, gap=1, noseed=()):
    """Backdrop = everything reachable from the card border without crossing a dark outline pixel.
    The dark mask is dilated by `gap` to close small outline gaps, then the flood is grown back
    `gap` steps into non-dark pixels so the backdrop reaches the outline again."""
    from scipy import ndimage as ndi
    op = c["opaque"]
    L = cardv3.rgb2lab(c["rgb"].astype(float))[..., 0]
    dark = (L < dark_L) | ~op
    barrier = ndi.binary_dilation(dark, iterations=gap) if gap else dark
    free = ~barrier
    seed = np.zeros((96, 96), bool); seed[1:3, :] = True; seed[:, 1:3] = True; seed[:, 93:95] = True; seed[84:86, :] = True
    for (x0, y0, x1, y1) in noseed: seed[y0:y1 + 1, x0:x1 + 1] = False
    lab, _ = ndi.label(free)
    ids = set(np.unique(lab[seed & free]).tolist()) - {0}
    bgO = np.isin(lab, list(ids))
    for _ in range(gap + 1):
        bgO |= ndi.binary_dilation(bgO) & ~dark
    bgO[87:, :] = True
    return bgO


def compute(n):
    c = C[n]; p = params_for(n)
    bg = cardv3.backdrop(c, p)
    subj = subject_mask(c, p, bg)
    mask, infos, excl = regions.team_mask(c, p, bg, n)
    if p.get("outline_bg"):
        # the outline-backdrop limit applies to the whole-card part only; hand-drawn polygon /
        # line parts are traced by hand and kept as drawn
        bgO = outline_backdrop(c, p.get("outline_dark_L", 32.0), p.get("outline_gap", 1), p.get("outline_noseed", []))
        hand_parts = [pt for pt in p.get("parts", []) if not pt.get("outline_part")]
        auto_parts = [pt for pt in p.get("parts", []) if pt.get("outline_part")]
        if auto_parts:
            q = dict(p); q["parts"] = auto_parts
            m1, _, _ = regions.team_mask(c, q, bg, n)
            m2 = np.zeros_like(m1)
            if hand_parts:
                q2 = dict(p); q2["parts"] = hand_parts
                m2, _, _ = regions.team_mask(c, q2, bg, n)
            mask = (m1 & ~bgO) | m2
        else:
            mask &= ~bgO
        subj = subj | mask
    mask &= subj   # hard AND: nothing outside the silhouette is ever recoloured
    alpha = cardv3.soft_alpha(mask, bg & ~subj, excl)
    alpha[~subj] = 0
    outside = int(((alpha > 0) & ~subj).sum())
    lo, bl, lmap, bmap = regions.defects(c, mask, alpha, bg, excl, infos)
    return dict(c=c, p=p, bg=bg, mask=mask, alpha=alpha, infos=infos, excl=excl, left=lo, bleed=bl, lmap=lmap, bmap=bmap, subj=subj, outside=outside)


def panel(n, scale=4):
    R = compute(n); c = R["c"]; op = c["opaque"]
    orig = c["rgb"].copy(); orig[~op] = 0
    ov = orig.astype(float)
    ov[~R["subj"]] = ov[~R["subj"]] * 0.35 + np.array([0, 0, 90]) * 0.65
    A = R["alpha"][..., None]
    ov = ov * (1 - A * 0.75) + np.array([255, 0, 255]) * A * 0.75
    tiles = [orig, ov.astype(np.uint8)]
    for t in ("purple", "teal", "navy"):
        r = cardv3.recolour(c, R["mask"], R["alpha"], cardv3.TEAMS[t], R["p"]); r[~op] = 0
        tiles.append(r)
    d = (orig.astype(float) * 0.3).astype(np.uint8)
    d[R["mask"]] = (90, 0, 90); d[R["lmap"]] = (255, 40, 40); d[R["bmap"]] = (255, 230, 0)
    tiles.append(d)
    S = 96 * scale
    im = Image.new("RGB", (6 * (S + 4), S + 18), (24, 24, 24))
    for i, t in enumerate(tiles):
        im.paste(Image.fromarray(t).resize((S, S), Image.NEAREST), (i * (S + 4), 18))
    ImageDraw.Draw(im).text((2, 3), "%d %s - %s   leftover %d px, bleed %d px, outside %d px   [original | mask | purple | teal | navy | defects: red leftover, yellow bleed]"
                            % (c["bp"], n, R["p"].get("note", ""), R["left"], R["bleed"], R["outside"]), fill=(255, 255, 255))
    return im, R


def stack(ims, path):
    sh = Image.new("RGB", (ims[0].width, sum(i.height for i in ims)))
    y = 0
    for i in ims: sh.paste(i, (0, y)); y += i.height
    sh.save(path)


if __name__ == "__main__":
    out = sys.argv[1]; names = sys.argv[2:]
    if names == ["named"]: names = NAMED
    if names == ["alien"]: names = ALIEN
    ims = []
    for n in names:
        im, R = panel(n)
        im.save(os.path.join(H, "panels", n + ".png")); ims.append(im)
        print("%-24s mask %4d  leftover %3d  bleed %3d" % (n, R["mask"].sum(), R["left"], R["bleed"]))
    stack(ims, os.path.join(H, out + ".png"))
    stack([i.resize((i.width // 2, i.height // 2), Image.NEAREST) for i in ims], os.path.join(H, "work", out + "_2x.png"))
