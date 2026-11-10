"""Proof render for docs/485 item 7: KingFaction.NCLR bank 13, the unused red
palette whose greys are dark blue. Draws stock sprite sheets with
  row 1: bank 0 (the stock palette, Red's shades in 12-14),
  row 2: bank 13 exactly as the file has it,
  row 3: the port's style 2: bank 0 with bank 13's entries 2-5 and 12-14,
plus swatch strips of the three palettes. Writes the PNG given as argv[1]."""
import os, struct, sys
from PIL import Image, ImageDraw

PLAIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'eur', 'plain', 'Sprites')


def nclr(path):
    d = open(path, 'rb').read()
    size = struct.unpack_from('<I', d, 0x20)[0]
    off = struct.unpack_from('<I', d, 0x24)[0]
    base = 0x18 + off
    n = min(size, len(d) - base) // 2
    cols = list(struct.unpack_from('<%dH' % n, d, base))
    return [cols[i:i + 16] for i in range(0, n - 15, 16)]


def rgb(c):
    f = lambda v: (v << 3) | (v >> 2)
    return (f(c & 31), f((c >> 5) & 31), f((c >> 10) & 31))


def unwrap(d):
    # The cartridge's COMP wrapper is already removed in out/eur/plain.
    return d


def ncbr(path):
    d = unwrap(open(path, 'rb').read())
    assert d[0x10:0x14] == b'RAHC', path
    th, tw = struct.unpack_from('<HH', d, 0x18)
    dsize, doff = struct.unpack_from('<II', d, 0x28)
    base = 0x18 + doff
    if tw == 0xFFFF or th == 0xFFFF:
        w = 64
        h = dsize * 2 // w
    else:
        w, h = tw * 8, th * 8
    px = []
    for b in d[base:base + w * h // 2]:
        px += [b & 15, b >> 4]
    return w, h, px


banks = nclr(PLAIN + r'\KingFaction.NCLR')
b0, b13 = banks[0], banks[13]
style2 = list(b0)
for i in (2, 3, 4, 5, 12, 13, 14):
    style2[i] = b13[i]
pals = [('bank 0 (stock, Red)', b0), ('bank 13 (as in the file)', b13), ('style 2: bank 0 + 13\'s 2-5, 12-14', style2)]

sheets = sys.argv[2:] or ['k_eng_0.NCBR']
imgs = []
for s in sheets:
    try:
        imgs.append(ncbr(PLAIN + '\\' + s))
    except Exception as e:
        print('skip', s, e)
if not imgs:
    sys.exit('no sheets')
crop = 64
scale = 3
W = (len(imgs) * crop + 16 * 6) * scale + 40
H = (len(pals) * (crop + 12)) * scale + 40
out = Image.new('RGB', (W, H), (52, 92, 52))
dr = ImageDraw.Draw(out)
y = 10
for name, pal in pals:
    x = 10
    for (w, h, px) in imgs:
        tile = Image.new('RGBA', (crop, crop), (0, 0, 0, 0))
        for yy in range(min(crop, h)):
            for xx in range(min(crop, w)):
                k = px[yy * w + xx]
                if k:
                    tile.putpixel((xx, yy), rgb(pal[k]) + (255,))
        tile = tile.resize((crop * scale, crop * scale), Image.NEAREST)
        out.paste(tile, (x, y), tile)
        x += crop * scale + 4
    for i, c in enumerate(pal):
        dr.rectangle([x + i * 6 * scale // 2, y, x + (i + 1) * 6 * scale // 2 - 1, y + 20], fill=rgb(c))
    dr.text((x, y + 26), name, fill=(255, 255, 255))
    y += (crop + 12) * scale
out.save(sys.argv[1])
print('wrote', sys.argv[1], 'bank 13:', ' '.join('%04X' % c for c in b13))
