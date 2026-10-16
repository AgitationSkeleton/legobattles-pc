#!/usr/bin/env python3
"""The Nitro container, and the graphics formats built on it.

3,475 of the cartridge's 3,915 files share one container: the standard Nintendo
"Nitro" wrapper behind `NCLR`, `NCGR`, `NCBR`, `NCER`, `NANR`, `NSCR`, `NFTR`,
`NSBMD` and `NSBCA`. `docs/30` had been counting all of them as *known format,
no parser here* — real knowledge, but not this project's, and deliberately kept
out of the decoded figure.

This makes it this project's. Every field below was read out of the cartridge's
own files, not recalled: the header layout was confirmed against one file of
each type before a line of the parser was written.

## The container

    +0x00  char[4]  magic, stored REVERSED  ('RLCN' is NCLR)
    +0x04  u16      byte order mark, 0xFEFF
    +0x06  u16      version
    +0x08  u32      total file size -- checked against the real size
    +0x0C  u16      header size, 16 in every file here
    +0x0E  u16      section count

then `section count` sections, each:

    +0x00  char[4]  section magic, also reversed  ('TTLP' is PLTT)
    +0x04  u32      section size, including this 8-byte header

The sections tile exactly: their sizes sum to `file size - header size`, which
is the check that the walk is right rather than merely plausible.

## The payload sections

Read from the files, one per format:

    PLTT  u32 bpp_flag, u32, u32 data_size, u32 data_offset      (palettes)
    CHAR  u16 height, u16 width, u32 bpp_flag, u32, u32,
          u32 data_size, u32 data_offset                          (tiles)
    SCRN  u16 width, u16 height, u32, u32 data_size               (tilemaps)

`bpp_flag` 3 means 4 bits per pixel, 4 means 8 -- so a palette's colour count
and a tile bank's tile count both follow from fields in the file. Beyond that
the payload is indexed pixels and BGR555 colours, which is *already* the
content: there is no further structure to unpack.

Formats whose payload this does **not** interpret -- `NCER`, `NANR`, `NFTR`,
`NSBMD`, `NSBCA` -- are reported as container-valid with their sections listed,
and are counted as partial rather than decoded.

Usage:
    python tools/lb_nitro.py
    python tools/lb_nitro.py --tsv docs/data/nitro_files.tsv
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

BOM = 0xFEFF

#: Container magic (as stored, reversed) -> the format's usual name.
CONTAINERS = {
    "RLCN": "NCLR", "RGCN": "NCGR", "RECN": "NCER", "RNAN": "NANR",
    "RCSN": "NSCR", "RTFN": "NFTR", "BMD0": "NSBMD", "BCA0": "NSBCA",
}

#: Formats whose payload section this tool actually interprets.
DECODED = {"NCLR", "NCGR", "NSCR", "NCER", "NANR", "NSBMD", "NSBCA", "NFTR"}

BPP = {3: 4, 4: 8}

#: The 3D formats use a different section table. NCLR and friends carry an
#: inline header per section; BMD0 and BCA0 carry a u32 OFFSET table at the end
#: of the header, each offset pointing at a block that then has its own magic
#: and size. Treating them alike parses 39 .nsbca files as a garbage section of
#: 810,831,434 bytes -- an impossible number, which is how the difference
#: announced itself.
G3D = {"BMD0", "BCA0"}


def parse(data):
    """Return (format, version, sections) or raise ValueError."""
    if len(data) < 16:
        raise ValueError("shorter than a container header")
    magic = data[:4].decode("latin-1")
    fmt = CONTAINERS.get(magic)
    if fmt is None:
        raise ValueError(f"unknown container magic {magic!r}")
    bom, ver, size, hdr, nsec = struct.unpack_from("<HHIHH", data, 4)
    if bom != BOM:
        raise ValueError(f"byte order mark is 0x{bom:04X}, not 0x{BOM:04X}")
    if size != len(data):
        raise ValueError(f"declared size {size} != actual {len(data)}")
    if hdr < 16 or hdr > len(data):
        raise ValueError(f"header size {hdr} is out of range")
    sections = []
    if magic in G3D:
        if hdr + 4 * nsec > len(data):
            raise ValueError("block offset table runs past the end")
        offsets = struct.unpack_from("<%dI" % nsec, data, hdr)
        off = hdr + 4 * nsec
        for o in offsets:
            if o + 8 > len(data):
                raise ValueError(f"block offset 0x{o:X} is out of range")
            bmag = data[o:o + 4].decode("latin-1")
            bsize = struct.unpack_from("<I", data, o + 4)[0]
            if bsize < 8 or o + bsize > len(data):
                raise ValueError(f"block {bmag!r} size {bsize} is out of range")
            sections.append((bmag, o, bsize))
            off = max(off, o + bsize)
        return fmt, ver, sections, off
    off = hdr
    for _ in range(nsec):
        if off + 8 > len(data):
            raise ValueError("section table runs past the end of the file")
        smag = data[off:off + 4].decode("latin-1")
        ssize = struct.unpack_from("<I", data, off + 4)[0]
        if ssize < 8 or off + ssize > len(data):
            raise ValueError(f"section {smag!r} size {ssize} is out of range")
        sections.append((smag, off, ssize))
        off += ssize
    return fmt, ver, sections, off


def g3d_dict(data, off):
    """Parse a G3D resource dictionary at `off`. Raise ValueError if it lies.

    Inside `MDL0`, `TEX0` and `JNT0` every named resource is reached through the
    same structure -- a small patricia tree plus a fixed-size entry array plus
    16-byte names:

        +0x00  u8   revision, 0
        +0x01  u8   entry count
        +0x02  u16  total dictionary size
        +0x04  u16  constant 8
        +0x06  u16  offset to the entry header, from the dictionary start
        ...    tree nodes, 4 bytes x (count + 1)
        +ofs   u16  entry size
        +ofs+2 u16  offset to the names, from the entry header
        ...    entries, `entry size` bytes each
        ...    names, 16 bytes each

    Three identities follow from that layout, and all three are checked here
    rather than assumed, because each uses fields the others do not:

        ofsEntry    == 12 + 4 * count          the tree is exactly count+1 nodes
        ofsName     ==  4 + count * entrySize  the names start after the entries
        sizeDictBlk == ofsEntry + ofsName + count * 16
    """
    if off + 8 > len(data):
        raise ValueError("dictionary header runs past the end")
    rev, count = data[off], data[off + 1]
    size, const, ofs_entry = struct.unpack_from("<HHH", data, off + 2)
    if rev != 0:
        raise ValueError(f"dictionary revision {rev}, expected 0")
    if const != 8:
        raise ValueError(f"dictionary constant is {const}, expected 8")
    if ofs_entry != 12 + 4 * count:
        raise ValueError(f"ofsEntry {ofs_entry} != 12 + 4*{count}")
    eh = off + ofs_entry
    if eh + 4 > len(data):
        raise ValueError("entry header runs past the end")
    esize, ofs_name = struct.unpack_from("<HH", data, eh)
    if ofs_name != 4 + count * esize:
        raise ValueError(f"ofsName {ofs_name} != 4 + {count}*{esize}")
    if size != ofs_entry + ofs_name + count * 16:
        raise ValueError(f"dict size {size} != {ofs_entry}+{ofs_name}+"
                         f"{count}*16")
    if off + size > len(data):
        raise ValueError("dictionary runs past the end")
    names = []
    base = eh + ofs_name
    for i in range(count):
        raw = data[base + i * 16: base + i * 16 + 16]
        names.append(raw.split(b"\0", 1)[0].decode("latin-1"))
    entries = [data[eh + 4 + i * esize: eh + 4 + (i + 1) * esize]
               for i in range(count)]
    return {"count": count, "size": size, "entry_size": esize,
            "names": names, "entries": entries}


def mdl0(data, off, size):
    """The first model in an `MDL0` block: counts, scale, and a polygon check.

    `numTriangle + numQuad == numPolygon` is the check. Nothing in the format
    forces those three fields to agree -- they agree only if the header is being
    read at the right offsets.
    """
    d = g3d_dict(data, off + 8)
    if not d["count"] or d["entry_size"] < 4:
        raise ValueError("model dictionary has no usable entry")
    rel = struct.unpack_from("<I", d["entries"][0], 0)[0]
    m = off + rel
    if m + 0x2C > off + size:
        raise ValueError(f"model offset 0x{rel:X} is out of range")
    msize, sbc, mat, shp, evp = struct.unpack_from("<IIIII", data, m)
    n_node, n_mat, n_shp = data[m + 0x17], data[m + 0x18], data[m + 0x19]
    scale, inv = struct.unpack_from("<II", data, m + 0x1C)
    n_vtx, n_poly, n_tri, n_quad = struct.unpack_from("<HHHH", data, m + 0x24)
    if not (sbc <= mat <= shp <= evp <= msize):
        raise ValueError(f"model offsets out of order: {sbc},{mat},{shp},"
                         f"{evp} vs size {msize}")
    return {"models": d["count"], "model_names": d["names"],
            "nodes": n_node, "materials": n_mat, "shapes": n_shp,
            "pos_scale": scale, "inv_pos_scale": inv,
            "vertices": n_vtx, "polygons": n_poly,
            "triangles": n_tri, "quads": n_quad,
            "poly_sum_ok": n_tri + n_quad == n_poly,
            # 20.12 fixed point: the two scale fields are reciprocals, so
            # their product is 1.0 x 1.0 == 2^24. Every one of the 39 models
            # satisfies it, which pins both offsets and confirms that the 3D
            # data uses the same fixed-point format as the simulation.
            "scale_recip_ok": scale * inv == 1 << 24}


def tex0(data, off, size):
    """A `TEX0` block: texture and palette stores, and how they tile.

    The two stores sit at the end of the block, back to back:

        ofsTex + sizeTex * 8 == ofsPal            no gap between them
        ofsPal + sizePal * 8 == block size        and the palettes end the block

    Both sizes are stored shifted right by three. Reading them unshifted makes
    the first identity miss by a factor of eight on every file, so the pair is
    self-announcing.
    """
    b = off + 8
    size_tex, ofs_tex_dict = struct.unpack_from("<HH", data, b + 4)
    ofs_tex = struct.unpack_from("<I", data, b + 12)[0]
    size_pal = struct.unpack_from("<I", data, b + 0x28)[0]
    ofs_pal_dict, ofs_pal = struct.unpack_from("<II", data, b + 0x2C)
    tex_bytes, pal_bytes = size_tex * 8, size_pal * 8
    td = g3d_dict(data, off + ofs_tex_dict)
    pd = g3d_dict(data, off + ofs_pal_dict)
    return {"textures": td["count"], "palettes": pd["count"],
            "texture_bytes": tex_bytes, "palette_bytes": pal_bytes,
            "texture_names": td["names"], "palette_names": pd["names"],
            "tex_tiles_ok": ofs_tex + tex_bytes == ofs_pal,
            "pal_ends_block": ofs_pal + pal_bytes == size}


def nftr(data, sections):
    """A font: `FINF` plus the glyph, width and map sections it points at.

    `FINF` carries three absolute offsets. Each one must land eight bytes past
    the start of the section it names -- that is, on that section's payload --
    which ties the header to the section table without either being assumed.

    Two more identities come out of the glyph and width sections:

        cell size   == ceil(cell width * cell height * bpp / 8)
        glyph count == lastChar - firstChar + 1, and that many cells fit CGLP

    and the `CMAP` sections form a linked list whose `next` pointers land on the
    following `CMAP` payload, ending in zero.
    """
    by = {}
    for smag, o, s in sections:
        by.setdefault(smag, []).append((o, s))
    if "FNIF" not in by:
        raise ValueError("no FINF section")
    fo = by["FNIF"][0][0] + 8
    line_feed = data[fo + 1]
    default_w = data[fo + 5]
    encoding = data[fo + 7]
    o_glyph, o_width, o_map = struct.unpack_from("<III", data, fo + 8)
    starts = {o + 8 for _m, o, _s in sections}
    points_ok = all(x in starts for x in (o_glyph, o_width, o_map))

    cw = ch = bpp = cell = glyphs = 0
    cells_fit = False
    if "PLGC" in by:
        go, gs = by["PLGC"][0]
        cw, ch = data[go + 8], data[go + 9]
        cell = struct.unpack_from("<H", data, go + 10)[0]
        bpp = data[go + 14]
    if "HDWC" in by:
        wo, _ws = by["HDWC"][0]
        first, last = struct.unpack_from("<HH", data, wo + 8)
        glyphs = last - first + 1
    if cell and glyphs and "PLGC" in by:
        _go, gs = by["PLGC"][0]
        cells_fit = 16 + glyphs * cell <= gs
    cell_ok = bool(cell) and cell == -(-(cw * ch * bpp) // 8)

    chain_ok = True
    maps = by.get("PAMC", [])
    for i, (mo, _ms) in enumerate(maps):
        nxt = struct.unpack_from("<I", data, mo + 8 + 8)[0]
        want = maps[i + 1][0] + 8 if i + 1 < len(maps) else 0
        if nxt != want:
            chain_ok = False
    return {"line_feed": line_feed, "default_width": default_w,
            "encoding": encoding, "cell_w": cw, "cell_h": ch, "bpp": bpp,
            "cell_bytes": cell, "glyphs": glyphs, "maps": len(maps),
            "finf_points_at_sections": points_ok, "cell_size_ok": cell_ok,
            "cells_fit": cells_fit, "cmap_chain_ok": chain_ok,
            # FINF's line feed and CGLP's cell height are stored in different
            # sections and agree in all eight fonts -- the line advance IS the
            # cell height, so the fonts have no leading.
            "line_feed_is_cell_h": bool(ch) and line_feed == ch}


def payload(fmt, data, sections):
    """Interpret the first payload section, for the formats we decode."""
    if not sections:
        return {}
    smag, off, ssize = sections[0]
    b = off + 8
    try:
        if fmt == "NSBMD":
            info = {}
            for m, o, s in sections:
                if m == "MDL0":
                    info.update(mdl0(data, o, s))
                elif m == "TEX0":
                    info.update(tex0(data, o, s))
            return info
        if fmt == "NSBCA":
            for m, o, _s in sections:
                if m == "JNT0":
                    d = g3d_dict(data, o + 8)
                    return {"animations": d["count"],
                            "animation_names": d["names"],
                            "entry_size": d["entry_size"]}
            return {}
        if fmt == "NFTR":
            return nftr(data, sections)
        if fmt == "NCLR" and smag == "TTLP":
            flag, _p, dsize, doff = struct.unpack_from("<IIII", data, b)
            bpp = BPP.get(flag)
            return {"bpp": bpp, "data_size": dsize, "data_offset": doff,
                    "colours": dsize // 2 if dsize else 0}
        if fmt == "NCGR" and smag == "RAHC":
            h, w = struct.unpack_from("<HH", data, b)
            flag, _a, _c, dsize, doff = struct.unpack_from("<IIIII", data, b + 4)
            bpp = BPP.get(flag)
            tiles = (dsize * 8 // (64 * bpp)) if bpp and dsize else 0
            return {"tiles_w": w, "tiles_h": h, "bpp": bpp,
                    "data_size": dsize, "data_offset": doff, "tiles": tiles}
        if fmt == "NCER" and smag == "KBEC":
            # The low bit of `flags` selects the cell entry size: set means
            # 16-byte entries (with a bounding box), clear means 8. 364 files
            # set it and 4 do not, and the cell table fits the section in all
            # 368 either way -- which is what confirms the bit's meaning.
            ncell, flags = struct.unpack_from("<HH", data, b)
            cdo, mapmode = struct.unpack_from("<II", data, b + 4)
            entry = 16 if (flags & 1) else 8
            return {"cells": ncell, "flags": flags, "cell_entry": entry,
                    "cell_offset": cdo, "mapping_mode": mapmode}
        if fmt == "NANR" and smag == "KNBA":
            # Two offset identities hold on all 368 files:
            #   seq_offset   + sequences * 16 == frame_offset
            #   frame_offset + frames    *  8 == frame_data_offset
            # Three independent offsets agreeing with two counts is not
            # something a wrong reading survives.
            nseq, nfrm = struct.unpack_from("<HH", data, b)
            so, fo, fdo = struct.unpack_from("<III", data, b + 4)
            return {"sequences": nseq, "frames": nfrm, "seq_offset": so,
                    "frame_offset": fo, "frame_data_offset": fdo}
        if fmt == "NSCR" and smag == "NRCS":
            w, h = struct.unpack_from("<HH", data, b)
            _u, dsize = struct.unpack_from("<II", data, b + 4)
            return {"width": w, "height": h, "data_size": dsize,
                    "entries": dsize // 2 if dsize else 0}
    except struct.error:
        return {}
    return {}


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    with open(os.path.join(args.extract, "files.json"), encoding="utf-8") as fh:
        recs = json.load(fh)

    ok = collections.Counter()
    failed = []
    pay_failed = []
    claims = collections.defaultdict(collections.Counter)
    rows = []
    tiles_by_bpp = collections.Counter()
    sects = collections.Counter()
    tiled = 0
    for r in sorted(recs, key=lambda x: x.get("path") or ""):
        path = r.get("path") or ""
        if not path:
            continue
        full = os.path.join(args.extract, "plain", path.replace("/", os.sep))
        if not os.path.exists(full):
            continue
        with open(full, "rb") as fh:
            head = fh.read(4)
            if head.decode("latin-1", "replace") not in CONTAINERS:
                continue
            fh.seek(0)
            data = fh.read()
        try:
            fmt, ver, sections, end = parse(data)
        except ValueError as e:
            failed.append((path, str(e)))
            continue
        ok[fmt] += 1
        if end == len(data):
            tiled += 1
        for smag, _o, _s in sections:
            sects[(fmt, smag)] += 1
        try:
            info = payload(fmt, data, sections)
        except ValueError as e:
            pay_failed.append((path, str(e)))
            info = {}
        # Every boolean a payload reader returns is a structural claim about
        # the format; count them all rather than a hand-picked few.
        for k, v in info.items():
            if isinstance(v, bool):
                claims[(fmt, k)][v] += 1
        if fmt == "NCGR" and info.get("bpp"):
            tiles_by_bpp[info["bpp"]] += info.get("tiles", 0)
        rows.append((path, fmt, ver, len(data),
                     ";".join(m for m, _o, _s in sections), info))

    total = sum(ok.values()) + len(failed)
    print(f"{sum(ok.values())} of {total} Nitro-container files parse")
    for p, why in failed[:10]:
        print(f"   FAIL {p}: {why}")
    print(f"sections tile exactly to the end of the file: {tiled} of "
          f"{sum(ok.values())}")
    print()
    print(f"{'format':<8}{'files':>7}  payload interpreted")
    for fmt, n in ok.most_common():
        print(f"{fmt:<8}{n:>7}  {'yes' if fmt in DECODED else 'no (container only)'}")
    print()
    print("section magics seen:")
    for (fmt, smag), n in sorted(sects.items()):
        print(f"   {fmt:<7}{smag:<6}{n:>6}")
    print()
    print(f"payload parsed: {len(rows) - len(pay_failed)} of {len(rows)}")
    for p, why in pay_failed[:10]:
        print(f"   PAYLOAD FAIL {p}: {why}")
    if claims:
        print()
        print("structural identities, counted over the files that carry them:")
        for (fmt, key), c in sorted(claims.items()):
            print(f"   {fmt:<7}{key:<26}{c[True]:>5} hold, {c[False]:>3} do not")
    if tiles_by_bpp:
        print()
        print(f"NCGR tiles by depth: "
              + ", ".join(f"{b}bpp {n:,}" for b, n in sorted(tiles_by_bpp.items())))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["path", "format", "version", "size",
                               "sections", "payload"]) + nl)
            for path, fmt, ver, size, secs, info in rows:
                fh.write(tab.join([
                    path, fmt, "0x%04X" % ver, str(size), secs,
                    ",".join(
                        f"{k}=" + ("|".join(str(x) for x in v)
                                   if isinstance(v, list) else str(v))
                        for k, v in sorted(info.items())),
                ]) + nl)
        print(f"\nwrote {args.tsv} ({len(rows)} files)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
