"""Dump LEGO Battles UI tile sheets (NCGR+NCLR) and the font (NFTR) to PNG.

py -3.13 tools/lb_skin_dump.py sheet <name> [bank]   -> out/<name>_b<bank>.png (32 tiles a row)
py -3.13 tools/lb_skin_dump.py font                    -> out/font.png + out/font.json
"""
import os, sys, struct, json
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "out", "eur", "plain")   # the extracted cartridge (never committed)
OUT = os.path.join(HERE, "..", "out", "skin")   # ROM-derived pictures stay out of git
os.makedirs(OUT, exist_ok=True)


def sections(d):
    hdr, n = struct.unpack_from("<HH", d, 0x0C)
    off, out = hdr, {}
    for _ in range(n):
        mag = d[off:off + 4].decode("latin-1")
        size = struct.unpack_from("<I", d, off + 4)[0]
        out.setdefault(mag, []).append((off, size))
        off += size
    return out


def rgb555(v):
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31)


def palette(path):
    d = open(path, "rb").read()
    s = sections(d)
    o, _ = s["TTLP"][0]
    bpp, _, size, doff = struct.unpack_from("<IIII", d, o + 8)
    base = o + 8 + doff
    n = min(size, len(d) - base) // 2
    return [rgb555(struct.unpack_from("<H", d, base + 2 * i)[0]) for i in range(n)], bpp


def tiles(path):
    d = open(path, "rb").read()
    s = sections(d)
    o, _ = s["RAHC"][0]
    h, w, bpp, _a, _b, size, doff = struct.unpack_from("<HHIIIII", d, o + 8)
    raw = d[o + 8 + doff:o + 8 + doff + size]
    bits = 4 if bpp == 3 else 8
    tb = 8 * 8 * bits // 8
    out = []
    for t in range(len(raw) // tb):
        px = []
        chunk = raw[t * tb:(t + 1) * tb]
        if bits == 4:
            for byte in chunk:
                px += [byte & 15, byte >> 4]
        else:
            px = list(chunk)
        out.append(px)
    return out, bits, w, h


def sheet(name, bank=0, per_row=32, folder="UI/AllInOne"):
    pal, _ = palette(os.path.join(ROOT, folder, name + ".NCLR"))
    ts, bits, w, h = tiles(os.path.join(ROOT, folder, name + ".NCGR"))
    if 0 < w < 0x100 and w != 0xFFFF:
        per_row = w
    rows = (len(ts) + per_row - 1) // per_row
    im = Image.new("RGBA", (per_row * 8, rows * 8), (0, 0, 0, 0))
    off = bank * 16 if bits == 4 else 0
    for i, t in enumerate(ts):
        tx, ty = (i % per_row) * 8, (i // per_row) * 8
        for p, c in enumerate(t):
            if c == 0:
                continue
            k = off + c
            if k < len(pal):
                im.putpixel((tx + p % 8, ty + p // 8), pal[k] + (255,))
    p = os.path.join(OUT, "%s_b%d.png" % (name, bank))
    im.save(p)
    return p, len(ts), bits, len(pal)


def font(path=os.path.join(ROOT, "Font", "EN", "font.NFTR")):
    d = open(path, "rb").read()
    s = sections(d)
    fo = s["FNIF"][0][0] + 8
    go = s["PLGC"][0][0]
    cw, ch = d[go + 8], d[go + 9]
    cell = struct.unpack_from("<H", d, go + 10)[0]
    bpp = d[go + 14]
    gdata = go + 16
    wo = s["HDWC"][0][0]
    first, last = struct.unpack_from("<HH", d, wo + 8)
    widths = []
    for i in range(last - first + 1):
        l, gw, adv = struct.unpack_from("<bBB", d, wo + 16 + 3 * i)
        widths.append((l, gw, adv))
    # CMAPs: char code -> glyph index
    cmap = {}
    for mo, _ in s.get("PAMC", []):
        b = mo + 8
        lo, hi, typ = struct.unpack_from("<HHH", d, b)
        if typ == 0:
            g0 = struct.unpack_from("<H", d, b + 12)[0]
            for c in range(lo, hi + 1):
                cmap[c] = g0 + c - lo
        elif typ == 1:
            for c in range(lo, hi + 1):
                g = struct.unpack_from("<H", d, b + 12 + 2 * (c - lo))[0]
                if g != 0xFFFF:
                    cmap[c] = g
        else:
            n = struct.unpack_from("<H", d, b + 12)[0]
            for k in range(n):
                c, g = struct.unpack_from("<HH", d, b + 14 + 4 * k)
                cmap[c] = g
    n = last - first + 1
    per = 32
    im = Image.new("L", (per * cw, ((n + per - 1) // per) * ch), 0)
    maxv = (1 << bpp) - 1
    glyph_px = []
    for g in range(n):
        raw = d[gdata + g * cell: gdata + (g + 1) * cell]
        bitstr = "".join(format(x, "08b") for x in raw)
        px = [int(bitstr[i * bpp:(i + 1) * bpp], 2) for i in range(cw * ch)]
        glyph_px.append(px)
        gx, gy = (g % per) * cw, (g // per) * ch
        for i, v in enumerate(px):
            if v:
                im.putpixel((gx + i % cw, gy + i // cw), v * 255 // maxv)
    im.save(os.path.join(OUT, "font.png"))
    json.dump({"cw": cw, "ch": ch, "bpp": bpp, "line_feed": d[fo + 1], "first": first,
               "widths": widths, "cmap": {str(k): v for k, v in cmap.items()}},
              open(os.path.join(OUT, "font.json"), "w"))
    return cw, ch, bpp, n, len(cmap)


if __name__ == "__main__":
    if sys.argv[1] == "font":
        print(font())
    else:
        bank = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        folder = sys.argv[4] if len(sys.argv) > 4 else "UI/AllInOne"
        print(sheet(sys.argv[2], bank, folder=folder))
