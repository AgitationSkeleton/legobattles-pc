#!/usr/bin/env python3
"""Export Discord Rich Presence art from the player's own LEGO Battles ROM.

Writes 512x512 PNGs and a manifest.json to the output directory. Nothing from
the ROM is kept in the repository; the output is for the owner to upload to a
Discord application's Rich Presence assets.

    py -3.13 tools/lb_export_rpc_assets.py "<rom.nds>" [outdir]

Maps (map_mpNN_<name>.png): the Free Play map select's own picture,
`mpNNmini.NCGR` (4 bpp, 8x8 tiles, 16 tiles per row, index 0 clear). Its
palette is NOT a file. The map select colours it with a 16-entry palette per
world, in the ARM9's data: a 3-pointer table at 0x020C4A54 (world 0 King,
1 Pirate, 2 Mars) pointing at 0x02127A8C / 0x02127AAC / 0x02127ACC. The world
of each map is field 6 of the 28-byte map table at 0x02147DDC (name id is
field 2, LOC id into LOC/UK_English.lng). Colours were verified against the
port's own screenshots of the map select (30 of 30 indices, every colour).

Heroes (hero_<name>.png): the battle HUD's head icons, built exactly as
port/runner/src/lb_teams.cpp build_hud_atlas does: key table at 0x0214E4E0
(count at 0x0214E41C), UI/Game/UIMgrData.bin record 0x252 + position, cell
on UI/MiniHeadsGame.NCGR, sub-palette of UI/WorldViewTop_Back.NCLR.

The DS 5-bit colour is widened as the framework presents it (6-bit
c*2+(c>0), then v<<2|v>>4), which matches the port's screenshots.
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import lb_blueprints  # noqa: E402
import lb_comp        # noqa: E402
import nds_extract    # noqa: E402
from PIL import Image  # noqa: E402

BG = (12, 22, 52)
SIZE = 512
MAP_TABLE = 0x02147DDC
WORLD_PAL_PTRS = 0x020C4A54
WORLDS = {0: "king", 1: "pirate", 2: "mars"}
HEAD_COUNT = 0x0214E41C
HEAD_KEYS = 0x0214E4E0
HEAD_REC_BASE = 0x252


class Rom:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        h = nds_extract.parse_header(self.raw[:0x200])
        a9 = self.raw[h["arm9_rom_offset"]:h["arm9_rom_offset"] + h["arm9_size"]]
        if nds_extract.looks_compressed(a9):
            a9, _ = nds_extract.blz_decompress(a9)
        self.arm9 = a9
        self.base = h["arm9_ram_address"]
        fnt = self.raw[h["fnt_offset"]:h["fnt_offset"] + h["fnt_size"]]
        fat = self.raw[h["fat_offset"]:h["fat_offset"] + h["fat_size"]]
        paths, _ = nds_extract.parse_fnt(fnt)
        self.fat = nds_extract.parse_fat(fat)
        self.files = {p.lower(): i for i, p in paths.items()}

    def file(self, path):
        s, e = self.fat[self.files[path.lower()]]
        return lb_comp.unwrap(self.raw[s:e])

    def u32(self, addr):
        return struct.unpack_from("<I", self.arm9, addr - self.base)[0]

    def cstr(self, addr):
        o = addr - self.base
        return self.arm9[o:self.arm9.index(b"\0", o)].decode("latin-1")


def widen(c):
    out = []
    for ch in range(3):
        v = (c >> (5 * ch)) & 31
        v6 = v * 2 + (1 if v else 0)
        out.append(((v6 << 2) | (v6 >> 4)) & 255)
    return tuple(out)


def slug(s):
    s = re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")
    return s or "x"


def load_lang(rom, name="LOC/UK_English.lng"):
    d = rom.file(name)
    first = struct.unpack_from("<I", d, 0x0C)[0]
    offs = struct.unpack_from("<%dI" % ((first - 0x0C) // 4), d, 0x0C)
    out = []
    for o in offs:
        e = d.find(b"\0", o)
        out.append(d[o:e].decode("latin-1") if o < len(d) else "")
    return out


def tile_pixels(ncgr):
    """Return (pixel getter, tiles wide, tiles high) for a 4bpp RAHC bank."""
    assert ncgr[:4] == b"RGCN", "not an NCGR"
    th, tw = struct.unpack_from("<HH", ncgr, 0x18)
    bpp = struct.unpack_from("<I", ncgr, 0x1C)[0]
    assert bpp == 3, "not 4bpp"
    dsz, doff = struct.unpack_from("<II", ncgr, 0x28)
    pix = ncgr[0x18 + doff:0x18 + doff + dsz]

    def get(x, y):
        b = pix[((y >> 3) * tw + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)]
        return (b >> 4) if (x & 1) else (b & 15)
    return get, tw, th


def frame(img, bg=BG):
    """Centre `img` (RGBA) on a 512x512 background at the largest whole scale."""
    s = max(1, min(SIZE // img.width, SIZE // img.height))
    big = img.resize((img.width * s, img.height * s), Image.NEAREST)
    out = Image.new("RGBA", (SIZE, SIZE), bg + (255,))
    out.alpha_composite(big, ((SIZE - big.width) // 2, (SIZE - big.height) // 2))
    return out.convert("RGB")


def export_maps(rom, lang, outdir, manifest):
    ptrs = [rom.u32(WORLD_PAL_PTRS + 4 * i) for i in range(3)]
    pals = []
    for p in ptrs:
        o = p - rom.base
        pals.append([widen(struct.unpack_from("<H", rom.arm9, o + 2 * i)[0])
                     for i in range(16)])
    for n in range(1, 31):
        rec = MAP_TABLE - rom.base + 28 * (n - 1)
        stem_p, players, name_id, desc_id, _, unlock, world = struct.unpack_from(
            "<7I", rom.arm9, rec)
        stem = rom.cstr(stem_p)
        assert stem == "mp%02d" % n, stem
        name = lang[name_id]
        get, tw, th = tile_pixels(rom.file(stem + "mini.NCGR"))
        w, h = tw * 8, th * 8
        idx = [[get(x, y) for x in range(w)] for y in range(h)]
        mx = max((x + 1 for y in range(h) for x in range(w) if idx[y][x]), default=0)
        my = max((y + 1 for y in range(h) for x in range(w) if idx[y][x]), default=0)
        side = max(mx, my)
        pal = pals[world]
        im = Image.new("RGBA", (side, side), BG + (255,))
        px = im.load()
        for y in range(side):
            for x in range(side):
                v = idx[y][x]
                if v:
                    px[x, y] = pal[v] + (255,)
        key = "map_%s_%s" % (stem, slug(name))
        frame(im).save(os.path.join(outdir, key + ".png"))
        manifest[key] = {"kind": "map", "name": name, "map": n, "stem": stem,
                         "players": players, "world": WORLDS[world],
                         "loc_name_id": name_id, "source_px": side}


def export_heroes(rom, lang, outdir, manifest):
    ebp = rom.file("BP/Entities.ebp")
    bps, _ = lb_blueprints.parse(ebp, lang)
    heroes = [b for b in bps if b["type"] == 0 and b.get("category_id") == 0]
    count = rom.u32(HEAD_COUNT)
    keys = [rom.u32(HEAD_KEYS + 4 * k) for k in range(min(count, 166))]
    ui = rom.file("UI/Game/UIMgrData.bin")
    pal_f = rom.file("UI/WorldViewTop_Back.NCLR")
    # PLTT: section at 0x10; u32 size at +16, data offset at +20 (from section+8)
    pdsz, pdoff = struct.unpack_from("<II", pal_f, 0x10 + 16)
    pal = [struct.unpack_from("<H", pal_f, 0x10 + 8 + pdoff + 2 * i)[0]
           for i in range(pdsz // 2)]
    game, gw, _ = tile_pixels(rom.file("UI/MiniHeadsGame.NCGR"))
    front, _, _ = tile_pixels(rom.file("UI/MiniHeads.NCGR"))

    def icon_key(bp):
        for t in (16, 36, 56, 76, 96, 116):
            if bp in (t + 1, t + 2):
                return t + 1
        if bp in (39, 59, 79, 99, 119):
            return 19
        if 122 <= bp <= 126:
            return 121
        return bp

    n_ui = struct.unpack_from("<H", ui, 0)[0]
    used = set()
    for b in heroes:
        bp = b["index"]
        key = icon_key(bp)
        if key not in keys:
            print("hero %d %s: no head key" % (bp, b["name"]))
            continue
        r = HEAD_REC_BASE + keys.index(key)
        if r >= n_ui:
            continue
        fr, _, _, _, _, _, res = struct.unpack_from("<7I", ui, 8 + r * 0x1C)
        if fr == 0 or res != 0x2C or ui[fr + 0x0C] != 0:
            print("hero %d %s: empty HUD record" % (bp, b["name"]))
            continue
        sx, sy = struct.unpack_from("<HH", ui, fr + 0x14)
        sub = ui[fr + 0x0F]
        sheet = front if bp in (155, 169) else game
        im = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
        px = im.load()
        for y in range(24):
            for x in range(24):
                v = sheet(sx + x, sy + y)
                if v and sub * 16 + v < len(pal):
                    px[x, y] = widen(pal[sub * 16 + v]) + (255,)
        name = b.get("display_name") or b["name"]
        k = "hero_" + slug(name)
        if k in used:
            k = "hero_" + slug(b["name"])
        used.add(k)
        frame(im).save(os.path.join(outdir, k + ".png"))
        manifest[k] = {"kind": "hero", "name": name, "blueprint": bp,
                       "blueprint_name": b["name"]}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rom = Rom(sys.argv[1])
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "out", "discord-rpc")
    os.makedirs(outdir, exist_ok=True)
    lang = load_lang(rom)
    manifest = {}
    export_maps(rom, lang, outdir, manifest)
    export_heroes(rom, lang, outdir, manifest)
    with open(os.path.join(outdir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
    print("wrote %d images to %s" % (len(manifest), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
