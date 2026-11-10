"""The GamePlayerCards cards, decoded from the extracted ROM (lbcard.py), as RGB arrays.

No cartridge art is stored in the repository: the decode is cached under out/cardmasks/cards.npz
(out/ is not tracked). Card set and blueprint numbers as in the study: every blueprint record whose
internal name has a card file, plus Bridge (-1), K_Unknown (-2) and A_LaserTrooper (-3)."""
import os
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))


def ch(v):
    s = v * 2
    return ((s << 2) | (s >> 4)) & 255


def ds_rgb_arr(c):
    c = np.asarray(c, dtype=np.int64)
    return np.stack([ch(c & 31), ch((c >> 5) & 31), ch((c >> 10) & 31)], -1).astype(np.uint8)


def _cache_path():
    import lbcard
    return os.path.join(lbcard.OUT, "cards.npz")


def _build():
    import lbcard
    cd = os.path.join(lbcard.FS, "UI", "GamePlayerCards")
    files = {os.path.splitext(n)[0].lower(): os.path.splitext(n)[0] for n in os.listdir(cd) if n.endswith(".NCGR")}
    items = []
    for r in lbcard.records():
        c = files.get(r["internal"].lower())
        if c: items.append((r["index"], c))
    items += [(-1, "Bridge"), (-2, "K_Unknown"), (-3, "A_LaserTrooper")]
    arrs = {}
    for bp, name in items:
        W, H, idx, pal, _ = lbcard.card("UI/GamePlayerCards/" + name)
        I = np.array(idx, dtype=np.int16)
        P = np.array(pal + [0] * (256 - len(pal)), dtype=np.uint16)
        arrs[name] = np.concatenate([[bp], I.ravel().astype(np.int32), P.astype(np.int32)])
    os.makedirs(os.path.dirname(_cache_path()), exist_ok=True)
    np.savez_compressed(_cache_path(), **arrs)


def load():
    if not os.path.exists(_cache_path()):
        _build()
    z = np.load(_cache_path())
    out = {}
    for n in z.files:
        a = z[n]
        bp = int(a[0]); I = a[1:1 + 9216].reshape(96, 96); P = a[1 + 9216:].astype(np.int64)
        rgb = ds_rgb_arr(P[np.clip(I, 0, 255)])
        out[n] = dict(bp=bp, I=I, P=P, rgb=rgb, opaque=I >= 0)
    return out
