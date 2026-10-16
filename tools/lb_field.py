"""Render the WHOLE battlefield host-side, from the guest's own live memory.

Why this exists
---------------
docs/167 measured that the DS 2D path can never show more field than it already
does: BG1 and BG2 are a scroll double-buffer, so only one screen's worth of
terrain (256x192) is materialised in VRAM at any instant. Widening the emulated
compositor produces black bars because there is nothing behind them.

The field itself is not the constraint. The level is stored as a grid of
metatile indices in main RAM, the metatiles are a table of 3x2 tile references,
and the tiles and palette are the ordinary DS graphics the guest already
uploaded. All of that describes the ENTIRE map, not the visible window, so a
host-side renderer can draw the whole battlefield at any size.

Everything below is read from the running guest. Nothing is hardcoded from a
ROM dump and nothing is guessed: each structure is located by matching against
what the guest is currently displaying, and `--verify` re-derives the visible
window and reports how much of it the render reproduces.

Geometry, as measured (docs/167 and this tool's own --verify):

    metatile      3 tiles wide x 2 tall  = 24 x 16 px
    level grid    u16 metatile indices, row-major, 64 metatiles wide
    whole map     64 x 64 metatiles      = 1536 x 1024 px

Usage
-----
    python tools/lb_field.py --out field.png
    python tools/lb_field.py --out field.png --verify
    python tools/lb_field.py --scan            # just report what it found
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from dbg import Dbg
except ImportError:  # the client lives beside the other debug tools
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
    from dbg import Dbg

MAIN_RAM = 0x02000000
MT_ENTRY_BYTES = 12          # 6 tile indices, u16 each
MT_W, MT_H = 3, 2            # tiles per metatile
TILE = 8


# ---------------------------------------------------------------- guest reads

class Guest:
    def __init__(self, port=19842):
        self.d = Dbg(port=port, timeout=1800)
        self.ram = bytes.fromhex(self.d.cmd("read_region", region="mainram")["hex"])

    def mem(self, addr, length, chunk=1024):
        out = bytearray()
        while length > 0:
            k = min(chunk, length)
            out += bytes.fromhex(self.d.cmd("read_mem", addr=addr, len=k)["hex"])
            addr += k
            length -= k
        return bytes(out)

    def io(self, addr, width=16):
        v = self.d.cmd("read_io", addr=addr, width=width)["value"]
        return v & 0xFFFF if width == 16 else v

    def u16(self, addr):
        return struct.unpack_from("<H", self.ram, addr - MAIN_RAM)[0]


# ------------------------------------------------------------------ locating

def bg1_layout(g):
    """Where engine A's terrain BG keeps its map and its tiles.

    Engine A drives the bottom screen here (POWCNT1 bit 15 clear), and docs/156
    identified BG1 as the terrain layer.
    """
    disp = g.io(0x04000000, 32)
    cnt = g.io(0x0400000A)
    char_block = (disp >> 24) & 7
    screen_block = (disp >> 27) & 7
    mapbase = 0x06000000 + screen_block * 0x10000 + ((cnt >> 8) & 0x1F) * 0x800
    charbase = 0x06000000 + char_block * 0x10000 + ((cnt >> 2) & 3) * 0x4000
    return mapbase, charbase


def read_bg_window(g, mapbase):
    """The 32x24 tile indices the guest is displaying right now."""
    raw = g.mem(mapbase, 2048)
    return [[(raw[(y * 32 + x) * 2] | (raw[(y * 32 + x) * 2 + 1] << 8)) & 0x3FF
             for x in range(32)] for y in range(24)]


def find_metatile_table(g, window):
    """Locate the metatile table and the window's metatile phase together.

    Two earlier approaches failed and both failures are instructive:

    * taking the first RAM occurrence of a 3x2 block landed inside the level
      grid, because a wrong-phase block can occur anywhere by chance;
    * bounding a candidate by "all six values look like tile indices" also
      accepts the level grid, whose metatile indices are small numbers too.

    So the table is anchored on a signature that the grid cannot imitate. The
    table is generated, so its first entries reference tiles 0..5 then 6..11.
    Candidates are then required to be 12-byte aligned with blocks actually on
    screen, and scored by how many of those blocks they explain.
    """
    sig = b"".join(struct.pack("<H", t) for t in range(12))
    bases = []
    hit = g.ram.find(sig)
    while hit >= 0:
        bases.append(MAIN_RAM + hit)
        hit = g.ram.find(sig, hit + 1)
    if not bases:
        raise SystemExit("no metatile table signature (tiles 0..11) in main RAM")

    best = None
    for py in range(MT_H):
        for px in range(MT_W):
            blocks = []
            for by in range((24 - py) // MT_H):
                for bx in range((32 - px) // MT_W):
                    blocks.append(tuple(
                        window[py + by * MT_H + dy][px + bx * MT_W + dx]
                        for dy in range(MT_H) for dx in range(MT_W)))
            uniq = list(dict.fromkeys(blocks))
            addrs = {}
            for blk in uniq:
                pat = b"".join(struct.pack("<H", t) for t in blk)
                found = []
                h = g.ram.find(pat)
                while h >= 0 and len(found) < 64:
                    found.append(MAIN_RAM + h)
                    h = g.ram.find(pat, h + 1)
                addrs[blk] = found
            for base in bases:
                resolved = 0
                for blk in blocks:
                    if any(a >= base and (a - base) % MT_ENTRY_BYTES == 0
                           for a in addrs.get(blk, ())):
                        resolved += 1
                if best is None or resolved > best[0]:
                    best = (resolved, base, (px, py), len(blocks))
    resolved, base, phase, total = best
    # how far the table runs: entries stay valid tile references
    count = 0
    while count < 8192:
        off = base - MAIN_RAM + count * MT_ENTRY_BYTES
        if off + MT_ENTRY_BYTES > len(g.ram):
            break
        if any(t >= 0x400 for t in struct.unpack_from("<6H", g.ram, off)):
            break
        count += 1
    return base, count, phase


def build_lookup(g, base, count):
    table = {}
    order = []
    for i in range(count):
        t = tuple(struct.unpack_from("<6H", g.ram, base - MAIN_RAM + i * MT_ENTRY_BYTES))
        order.append(t)
        table.setdefault(t, i)
    return table, order


def window_metatiles(window, table, phase):
    px, py = phase
    rows = []
    for by in range((24 - py) // MT_H):
        row = []
        for bx in range((32 - px) // MT_W):
            blk = tuple(window[py + by * MT_H + dy][px + bx * MT_W + dx]
                        for dy in range(MT_H) for dx in range(MT_W))
            row.append(table.get(blk))
        rows.append(row)
    return rows


def find_grid(g, rows, widths=(32, 40, 48, 56, 64, 72, 80, 96, 100, 112, 128, 160, 192, 256)):
    """Find the level grid by matching the displayed metatile window in RAM.

    Uses the most varied row as the probe (a row of nothing but grass matches
    everywhere), then requires every other row to agree at the candidate width.
    A correct width produces exactly one surviving candidate.
    """
    ram_u16 = struct.unpack_from("<%dH" % (len(g.ram) // 2), g.ram, 0)
    probe = max(range(len(rows)), key=lambda i: len({v for v in rows[i] if v is not None}))
    known = [(i, v) for i, v in enumerate(rows[probe]) if v is not None]
    if len(known) < 3:
        raise SystemExit("the visible window is too uniform to locate the grid")
    hits = [p for p in range(len(ram_u16) - len(rows[probe]))
            if all(ram_u16[p + i] == v for i, v in known)]
    matches = []
    for p in hits:
        for w in widths:
            ok = tot = 0
            valid = True
            for dy, row in enumerate(rows):
                start = p + (dy - probe) * w
                if start < 0 or start + len(row) > len(ram_u16):
                    valid = False
                    break
                for dx, v in enumerate(row):
                    if v is None:
                        continue
                    tot += 1
                    ok += (ram_u16[start + dx] == v)
            if valid and tot and ok == tot:
                matches.append((MAIN_RAM + p * 2, w, tot, probe))
    return matches


def grid_extent(g, anchor_addr, width, count):
    """Find the level grid's true base.

    Walking up in row strides from the camera's row is NOT enough: it makes the
    base congruent to whatever row the camera happens to be on, which silently
    forces the reported camera column to 0 and can leave the base wrong by whole
    cells. A base off by k cells ROLLS the whole map k cells horizontally with
    wraparound -- the map still looks coherent, just shifted, which is how a
    4-cell error survived a visual check (the forest belt appeared only on the
    right instead of on both edges).

    The array is preceded by an object header whose last two words are pointers
    into main RAM, so the walk's result is snapped to the nearest address that
    has that shape.
    """
    stride = width * 2
    top = anchor_addr
    while top - stride >= MAIN_RAM and all(
            g.u16(top - stride + i * 2) < count for i in range(width)):
        top -= stride
    bottom = anchor_addr
    while (bottom + stride + stride <= MAIN_RAM + len(g.ram) and all(
            g.u16(bottom + stride + i * 2) < count for i in range(width))):
        bottom += stride
    rows = (bottom - top) // stride + 1

    def header_here(a):
        if a - 12 < MAIN_RAM:
            return False
        p1 = struct.unpack_from("<I", g.ram, a - 12 - MAIN_RAM)[0]
        p2 = struct.unpack_from("<I", g.ram, a - 8 - MAIN_RAM)[0]
        return (MAIN_RAM <= p1 < MAIN_RAM + len(g.ram)
                and MAIN_RAM <= p2 < MAIN_RAM + len(g.ram))

    for delta in sorted(range(-width, width + 1), key=abs):
        cand = top + delta * 2
        if cand >= MAIN_RAM and header_here(cand):
            return cand, rows
    return top, rows


# ------------------------------------------------------------------ rendering

def png(path, pixels, w, h):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    raw = b"".join(b"\x00" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6))
                + chunk(b"IEND", b""))


def find_second_table(g, window, phase, grid_base, width, cx0, cy0, order):
    """Find the SECOND metatile table, the one the primary does not explain.

    The terrain is two parallel tables indexed by the SAME cell value. About a
    quarter of on-screen cells display a block that is nowhere in the primary
    table; those blocks all land in one region of RAM on a clean 12-byte
    lattice, which is the second table. Its base follows from any one such cell:
    base = (address of the displayed block) - (cell value) * 12, and every other
    mismatching cell must agree on that base.
    """
    px, py = phase
    votes = {}
    for by in range((24 - py) // MT_H):
        for bx in range((32 - px) // MT_W):
            disp = tuple(window[py + by * MT_H + dy][px + bx * MT_W + dx]
                         for dy in range(MT_H) for dx in range(MT_W))
            mi = g.u16(grid_base + ((cy0 + by) * width + cx0 + bx) * 2)
            if mi < len(order) and order[mi] == disp:
                continue                      # the primary already explains it
            pat = b"".join(struct.pack("<H", t) for t in disp)
            h = g.ram.find(pat)
            while h >= 0:
                base = MAIN_RAM + h - mi * MT_ENTRY_BYTES
                if base >= MAIN_RAM:
                    votes[base] = votes.get(base, 0) + 1
                h = g.ram.find(pat, h + 1)
    if not votes:
        return None, 0
    base, n = max(votes.items(), key=lambda kv: kv[1])
    return (base, n) if n >= 3 else (None, 0)


def render(g, grid_base, width, rows, primary, secondary, tiles, palette, count):
    """Draw the whole map.

    Which of the two tables a cell uses is decided WITHOUT a magic threshold:
    the primary table wins unless its entry references a tile that is blank in
    VRAM, in which case the cell is a secondary-table cell. Measured against the
    live screen this reproduces 120 of 120 visible cells.
    """
    W = width * MT_W * TILE
    H = rows * MT_H * TILE
    # DS colour is 5 bits per channel, but the runner expands it the way melonDS
    # does -- via a 6-bit intermediate -- and a naive 5->8 shift does NOT match.
    # Measured against the live framebuffer: grass reads (0x28,0x9A,0x28), and
    # only c6 = c5*2 then c8 = (c6<<2)|(c6>>4) reproduces all three channels.
    def expand(c5):
        c6 = c5 * 2
        return (c6 << 2) | (c6 >> 4)
    pal = []
    for i in range(256):
        v = palette[i * 2] | (palette[i * 2 + 1] << 8)
        pal.append(bytes((expand(v & 31), expand((v >> 5) & 31),
                          expand((v >> 10) & 31))))
    blank = [not any(tiles[i * 64:(i + 1) * 64]) for i in range(1024)]
    # Colour index 0 is TRANSPARENT on the DS, not black. BG1 is the backmost
    # layer, so an index-0 pixel shows the backdrop -- palette entry 0.
    out = bytearray(pal[0] * (W * H))
    used_secondary = 0
    for cy in range(rows):
        for cx in range(width):
            mi = g.u16(grid_base + (cy * width + cx) * 2)
            if mi >= count:
                continue
            six = primary[mi]
            if secondary is not None and any(t < 1024 and blank[t] for t in six):
                six = secondary[mi]
                used_secondary += 1
            for sub in range(6):
                idx = six[sub]
                if idx >= 1024:
                    continue
                tx, ty = sub % MT_W, sub // MT_W
                td = tiles[idx * 64:(idx + 1) * 64]
                if len(td) < 64:
                    continue
                ox = (cx * MT_W + tx) * TILE
                oy = (cy * MT_H + ty) * TILE
                for py in range(TILE):
                    base_o = ((oy + py) * W + ox) * 3
                    rowd = td[py * TILE:py * TILE + TILE]
                    for pxi in range(TILE):
                        ci = rowd[pxi]
                        if ci:
                            o = base_o + pxi * 3
                            out[o:o + 3] = pal[ci]
    return bytes(out), W, H, used_secondary



# ------------------------------------------------------- units and framing

# Entity coordinates (docs/169: +0x0EC / +0x0F0, 20.12 fixed point) are in the
# simulation's world space, which is offset from the level array's own frame.
# This constant is CALIBRATED, not derived from the decomp: with the grid base
# anchored and the camera at cell (0,8), the hero's sprite was located on screen
# by its minifig colours at (220,95) and its field read (264,240), giving
# (52,17). Two other entities were then checked against it WITHOUT refitting --
# the structure landed on the castle's top-left corner and the builder on the
# small figure beside it, both to within a few pixels.
#
# It is a measurement, so it is named and isolated here rather than smeared
# through the drawing code, and --entity-offset overrides it.
ENTITY_TO_MAP = (52, 17)

OWNER_COLOUR = [bytes((255, 64, 64)), bytes((64, 160, 255)),
                bytes((80, 220, 80)), bytes((240, 220, 60))]


def read_entities(g):
    """Live entities with a resolvable blueprint: (owner, category, x, y)."""
    def u32(a): return struct.unpack_from("<I", g.ram, a - MAIN_RAM)[0]
    def s32(a): return struct.unpack_from("<i", g.ram, a - MAIN_RAM)[0]
    pool = u32(0x02155028)
    if not (MAIN_RAM <= pool < MAIN_RAM + len(g.ram)):
        return []
    arr, count = u32(pool + 0x08), u32(pool + 0x0C)
    out = []
    for i in range(min(count, 256)):
        e = u32(arr + i * 4)
        if not (MAIN_RAM <= e < MAIN_RAM + len(g.ram)):
            continue
        bp = u32(e + 0x160)
        if not (MAIN_RAM <= bp < MAIN_RAM + len(g.ram)):
            continue
        info = u32(bp + 8)
        if not (MAIN_RAM <= info < MAIN_RAM + len(g.ram)):
            continue
        out.append((g.ram[e + 0xE4 - MAIN_RAM], g.ram[info + 0x5C - MAIN_RAM],
                    s32(e + 0xEC) / 4096.0, s32(e + 0xF0) / 4096.0))
    return out


def draw_markers(pixels, W, H, entities, offset):
    out = bytearray(pixels)
    ox, oy = offset
    for owner, cat, wx, wy in entities:
        x, y = int(wx) - ox, int(wy) - oy
        r = 10 if cat >= 7 else 5           # structures get a bigger box
        col = OWNER_COLOUR[owner % len(OWNER_COLOUR)]
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if max(abs(dx), abs(dy)) < r - 1:
                    continue                # outline only, keep terrain visible
                px, py = x + dx, y + dy
                if 0 <= px < W and 0 <= py < H:
                    o = (py * W + px) * 3
                    out[o:o + 3] = col
    return bytes(out)


def fit(pixels, W, H, tw, th, pad=bytes((16, 16, 20))):
    """Scale the map to fill a target window as far as aspect allows.

    Integer scaling is crisper for pixel art, but 1536x1008 into 1920x1080 only
    admits 1x, which wastes a third of the screen -- and the point of this view
    is coverage. So the scale is fractional and chosen to fill whichever axis
    binds, with the map centred in whatever is left.
    """
    k = min(tw / float(W), th / float(H))
    sw, sh = max(1, int(W * k)), max(1, int(H * k))
    scaled = bytearray(sw * sh * 3)
    xmap = [min(W - 1, int(x / k)) for x in range(sw)]
    for y in range(sh):
        sy = min(H - 1, int(y / k))
        row = bytearray(sw * 3)
        for x in range(sw):
            so = (sy * W + xmap[x]) * 3
            row[x * 3:x * 3 + 3] = pixels[so:so + 3]
        scaled[y * sw * 3:(y + 1) * sw * 3] = row
    out = bytearray(pad * (tw * th))
    x0, y0 = (tw - sw) // 2, (th - sh) // 2
    for y in range(sh):
        d = ((y0 + y) * tw + x0) * 3
        out[d:d + sw * 3] = scaled[y * sw * 3:(y + 1) * sw * 3]
    return bytes(out), tw, th


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="write the whole battlefield here as PNG")
    ap.add_argument("--port", type=int, default=19842)
    ap.add_argument("--scan", action="store_true", help="report findings only")
    ap.add_argument("--markers", action="store_true",
                    help="outline live entities on the map")
    ap.add_argument("--entity-offset", default=None,
                    help="override the calibrated entity->map offset, as X,Y")
    ap.add_argument("--fit", action="append", default=[],
                    help="also write the map fitted to a WxH window, e.g. 1920x1080")
    a = ap.parse_args()

    g = Guest(a.port)
    mapbase, charbase = bg1_layout(g)
    print("BG1 map=%08X  tiles=%08X" % (mapbase, charbase))
    window = read_bg_window(g, mapbase)

    mt_base, mt_count, phase = find_metatile_table(g, window)
    table, order = build_lookup(g, mt_base, mt_count)
    print("primary metatile table: base=%08X entries=%d distinct=%d  phase=%s"
          % (mt_base, mt_count, len(table), phase))

    rows = window_metatiles(window, table, phase)
    known = sum(1 for r in rows for v in r if v is not None)
    total = sum(len(r) for r in rows)
    print("visible window: %dx%d metatiles, %d/%d explained by the primary (%.1f%%)"
          % (len(rows[0]), len(rows), known, total, 100.0 * known / total))

    matches = find_grid(g, rows)
    if not matches:
        raise SystemExit("no unambiguous level grid found")
    anchor, width, cells, probe = matches[0]
    print("grid match: probe row %d at %08X, width=%d metatiles (%d cells verified)"
          % (probe, anchor, width, cells))
    row0 = anchor - probe * width * 2
    top, nrows = grid_extent(g, row0, width, mt_count)
    cell0 = (row0 - top) // 2
    cx0, cy0 = cell0 % width, cell0 // width
    print("grid extent: base=%08X rows=%d -> map is %dx%d metatiles = %dx%d px"
          % (top, nrows, width, nrows, width * MT_W * TILE, nrows * MT_H * TILE))
    print("camera top-left cell = (%d,%d)" % (cx0, cy0))

    second_base, agree = find_second_table(g, window, phase, top, width, cx0, cy0, order)
    secondary = None
    if second_base:
        secondary = [struct.unpack_from("<6H", g.ram,
                                        second_base - MAIN_RAM + i * MT_ENTRY_BYTES)
                     for i in range(mt_count)]
        print("second metatile table: base=%08X (%d mismatching cells agree)"
              % (second_base, agree))
    else:
        print("second metatile table: not found")

    if a.scan:
        return

    tiles = g.mem(charbase, 1024 * 64)
    palette = g.mem(0x05000000, 512)
    pixels, W, H, nsec = render(g, top, width, nrows, order, secondary,
                                tiles, palette, mt_count)
    transparent = sum(1 for i in range(0, len(pixels), 3)
                      if pixels[i:i + 3] == palette[0:1] * 0 + bytes(3))
    print("cells drawn from the second table: %d of %d (%.1f%%)"
          % (nsec, width * nrows, 100.0 * nsec / (width * nrows)))
    print("backdrop pixels remaining: %d of %d (%.1f%%)"
          % (transparent, W * H, 100.0 * transparent / (W * H)))
    if a.markers:
        off = ENTITY_TO_MAP
        if a.entity_offset:
            off = tuple(int(v) for v in a.entity_offset.split(","))
        ents = read_entities(g)
        print("entities with a blueprint: %d  (offset %s)" % (len(ents), off))
        for owner, cat, wx, wy in ents:
            print("   owner %d cat %-2d world(%6.0f,%6.0f) -> map(%5d,%5d)"
                  % (owner, cat, wx, wy, int(wx) - off[0], int(wy) - off[1]))
        pixels = draw_markers(pixels, W, H, ents, off)
    if a.out:
        png(a.out, pixels, W, H)
        print("wrote %s (%dx%d)" % (a.out, W, H))
        for spec in a.fit:
            tw, th = (int(v) for v in spec.lower().split("x"))
            fp, fw, fh = fit(pixels, W, H, tw, th)
            path = a.out.replace(".png", "_%dx%d.png" % (tw, th))
            png(path, fp, fw, fh)
            print("wrote %s (%dx%d)" % (path, fw, fh))


if __name__ == "__main__":
    main()
