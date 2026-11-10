"""Card / sprite decoding helpers for the GamePlayerCards study (research only)."""
import os, struct, sys
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import lb_comp

FS = os.environ.get("LB_NITROFS", os.path.join(REPO, "out", "eur", "nitrofs"))
OUT = os.environ.get("LB_CARDMASKS_OUT", os.path.join(REPO, "out", "cardmasks"))   # untracked cache/output


def rom(path):
    p = os.path.join(FS, path.replace("/", os.sep))
    if not os.path.exists(p):
        # case-insensitive fallback
        d, f = os.path.split(p)
        if os.path.isdir(d):
            for n in os.listdir(d):
                if n.lower() == f.lower():
                    p = os.path.join(d, n)
                    break
    if not os.path.exists(p):
        return None
    raw = open(p, "rb").read()
    return lb_comp.unwrap(raw) if lb_comp.is_comp(raw) else raw


def u16(b, a): return b[a] | (b[a + 1] << 8)
def u32(b, a): return u16(b, a) | (u16(b, a + 2) << 16)


def ds_rgb(c):
    def ch(v):
        s = v * 2
        return ((s << 2) | (s >> 4)) & 255
    return (ch(c & 31), ch((c >> 5) & 31), ch((c >> 10) & 31))


def nclr(path):
    c = rom(path)
    if c is None: return None
    n = (len(c) - 0x28) // 2
    return [u16(c, 0x28 + 2 * i) for i in range(min(n, 256))]


KW = [[8, 16, 32, 64], [16, 32, 32, 64], [8, 8, 16, 32]]
KH = [[8, 16, 32, 64], [8, 8, 16, 32], [16, 32, 32, 64]]


def card(base):
    """Return (w, h, idx[][] (palette index, -1 transparent), pal[256], oams)."""
    e = rom(base + ".NCER"); g = rom(base + ".NCGR"); pal = nclr(base + ".NCLR")
    if e is None or g is None or pal is None: return None
    ncell = u16(e, 0x18); cattr = u16(e, 0x1A); coff = u32(e, 0x1C)
    cells = 0x18 + coff; esz = 16 if cattr == 1 else 8
    oams = cells + ncell * esz
    noam = u16(e, cells); ooff = u32(e, cells + 4)
    chars = 0x18 + u32(g, 0x2C); csize = u32(g, 0x28)
    linear = (u32(g, 0x24) & 0xFF) == 1
    os_ = []
    x0 = y0 = 1 << 20; x1 = y1 = -(1 << 20)
    for k in range(noam):
        at = oams + ooff + k * 6
        a0, a1, a2 = u16(e, at), u16(e, at + 2), u16(e, at + 4)
        shape = (a0 >> 14) & 3; size = (a1 >> 14) & 3
        y = a0 & 0xFF
        if y >= 128: y -= 256
        x = a1 & 0x1FF
        if x >= 256: x -= 512
        o = dict(x=x, y=y, w=KW[shape][size], h=KH[shape][size], tile=a2 & 0x3FF, pal=a2 >> 12,
                 bpp8=bool(a0 & 0x2000), a0=a0, a1=a1, a2=a2)
        os_.append(o)
        x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x + o["w"]); y1 = max(y1, y + o["h"])
    W, H = x1 - x0, y1 - y0
    idx = [[-1] * W for _ in range(H)]
    for o in os_:
        tb = 64 if o["bpp8"] else 32
        for y in range(o["h"]):
            for x in range(o["w"]):
                if linear:
                    p = o["tile"] * 32 * (1 if o["bpp8"] else 2) + y * o["w"] + x
                    byte = p if o["bpp8"] else p // 2
                else:
                    t = (y // 8) * (o["w"] // 8) + (x // 8)
                    inn = (y % 8) * 8 + (x % 8)
                    byte = o["tile"] * 32 + t * tb + (inn if o["bpp8"] else inn // 2)
                if byte >= csize: continue
                v = g[chars + byte]
                i = v if o["bpp8"] else ((v >> 4) if x % 2 else (v & 15))
                if not o["bpp8"]: i = o["pal"] * 16 + i if i else 0
                idx[o["y"] - y0 + y][o["x"] - x0 + x] = i if i else -1
    return W, H, idx, pal, os_


# --- Entities.ebp records (docs/11): 182 records of 0x7C from offset 4
KUNITS = 182
KPOOL = 4 + 182 * 0x7C + 27 * 0x74 + 342 * 0x70


def records():
    e = rom("BP/Entities.ebp")
    def s(off):
        p = KPOOL + off; o = bytearray()
        while p < len(e) and e[p] and len(o) < 64:
            o.append(e[p]); p += 1
        return o.decode("latin1")
    out = []
    for i in range(KUNITS):
        r = 4 + i * 0x7C
        out.append(dict(index=i, internal=s(u32(e, r)), asset=s(u32(e, r + 0x24)), category=e[r + 0x5C],
                        cell=u16(e, r + 0x20), cw=e[r + 0x1E], ch=e[r + 0x1F], flag8=e[r + 8]))
    return out


def sheet(name):
    d = rom("Sprites/" + name + ".NCBR")
    if d is None or d[0x10:0x14] != b"RAHC": return None
    th, tw = u16(d, 0x18), u16(d, 0x1A); dsize, doff = u32(d, 0x28), u32(d, 0x2C)
    base = 0x18 + doff
    if tw in (0, 0xFFFF): return None
    w, h = tw * 8, th * 8
    px = bytearray(w * h)
    for i in range(min(dsize, w * h // 2)):
        px[2 * i] = d[base + i] & 15; px[2 * i + 1] = d[base + i] >> 4
    return w, h, px


def front_frame(rec):
    """Direction-4 idle frame of a unit's sprite: (w, h, idx) or None."""
    a = rec["asset"]
    if not a.startswith("Sprites/"): return None
    b = a[len("Sprites/"):]
    c = rec["category"]; d = 4
    if c == 0: sh, x, y, w, h = b + "_w4", 0, 0, 24, 24
    elif 1 <= c <= 3: sh, x, y, w, h = b + "_0", d * 24, 0, 24, 24
    elif c == 4: sh, x, y, w, h = b, 0, 96 + d * 32, 32, 32
    else:
        sh, x, y, w, h = b, (rec["cell"] & 31) * 8, (rec["cell"] >> 5) * 8, rec["cw"] * 8, rec["ch"] * 8
    s = sheet(sh)
    if s is None: return None
    W, H, px = s
    out = [[(px[(y + j) * W + (x + i)] if (x + i) < W and (y + j) < H else 0) for i in range(w)] for j in range(h)]
    return w, h, out, sh
