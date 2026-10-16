#!/usr/bin/env python3
"""Dependency-free Nintendo DS ROM extractor.

Parses the cartridge header, the ARM9/ARM7 static modules, the ARM9 overlay
table, the FNT/FAT pair and NitroFS, and writes everything to an output
directory. Also un-BLZ-compresses the ARM9 module and splits it into its
autoload sections, because the header's ARM9 size field is the *packed* size and
every downstream tool needs the unpacked image at its runtime address.

Usage:
    python tools/nds_extract.py <rom.nds> <outdir> [--no-nitrofs]

Writes:
    <outdir>/header.bin              0x200 cartridge header
    <outdir>/header.json             parsed header fields
    <outdir>/arm9/arm9_packed.bin    ARM9 exactly as stored in the ROM
    <outdir>/arm9/arm9_unpacked.bin  ARM9 after BLZ, if compressed
    <outdir>/arm9/section_*.bin      autoload sections at their runtime bases
    <outdir>/arm9/sections.json      autoload section table
    <outdir>/arm7/arm7.bin           ARM7 module as loaded from the ROM
    <outdir>/arm7/section_*.bin      its autoload sections at their runtime bases
    <outdir>/overlays/ov###.bin      each ARM9 overlay, decompressed if needed
    <outdir>/overlays/overlays.json  overlay table
    <outdir>/nitrofs/...             the filesystem, with real paths
    <outdir>/files.json              every file: id, path, offset, size, sha1
"""

import argparse
import hashlib
import json
import os
import struct
import sys


# ---------------------------------------------------------------- header ----

HEADER_FIELDS = [
    ("game_title", 0x000, 12, "str"),
    ("game_code", 0x00C, 4, "str"),
    ("maker_code", 0x010, 2, "str"),
    ("unit_code", 0x012, 1, "u8"),
    ("encryption_seed_select", 0x013, 1, "u8"),
    ("device_capacity", 0x014, 1, "u8"),
    ("rom_version", 0x01E, 1, "u8"),
    ("autostart", 0x01F, 1, "u8"),
    ("arm9_rom_offset", 0x020, 4, "u32"),
    ("arm9_entry_address", 0x024, 4, "u32"),
    ("arm9_ram_address", 0x028, 4, "u32"),
    ("arm9_size", 0x02C, 4, "u32"),
    ("arm7_rom_offset", 0x030, 4, "u32"),
    ("arm7_entry_address", 0x034, 4, "u32"),
    ("arm7_ram_address", 0x038, 4, "u32"),
    ("arm7_size", 0x03C, 4, "u32"),
    ("fnt_offset", 0x040, 4, "u32"),
    ("fnt_size", 0x044, 4, "u32"),
    ("fat_offset", 0x048, 4, "u32"),
    ("fat_size", 0x04C, 4, "u32"),
    ("arm9_overlay_offset", 0x050, 4, "u32"),
    ("arm9_overlay_size", 0x054, 4, "u32"),
    ("arm7_overlay_offset", 0x058, 4, "u32"),
    ("arm7_overlay_size", 0x05C, 4, "u32"),
    ("normal_card_control", 0x060, 4, "u32"),
    ("secure_card_control", 0x064, 4, "u32"),
    ("icon_banner_offset", 0x068, 4, "u32"),
    ("secure_area_crc", 0x06C, 2, "u16"),
    ("secure_transfer_timeout", 0x06E, 2, "u16"),
    ("arm9_autoload", 0x070, 4, "u32"),
    ("arm7_autoload", 0x074, 4, "u32"),
    ("secure_disable", 0x078, 8, "hex"),
    ("rom_size_used", 0x080, 4, "u32"),
    ("header_size", 0x084, 4, "u32"),
    ("nintendo_logo_crc", 0x15C, 2, "u16"),
    ("header_crc", 0x15E, 2, "u16"),
]


def parse_header(raw):
    out = {}
    for name, off, size, kind in HEADER_FIELDS:
        blob = raw[off:off + size]
        if kind == "str":
            out[name] = blob.decode("ascii", "replace").rstrip("\x00")
        elif kind == "u8":
            out[name] = blob[0]
        elif kind == "u16":
            out[name] = struct.unpack("<H", blob)[0]
        elif kind == "u32":
            out[name] = struct.unpack("<I", blob)[0]
        else:
            out[name] = blob.hex()
    return out


# ------------------------------------------------------------------ BLZ ----

def _blz_footer(data):
    """Read and validate the BLZ footer. Returns None when it is not BLZ.

    Layout, at the very end of the module:
        [-8..-5]  enc_len, 24 bits — size of the *compressed region*, which
                  ends at the end of the file and includes this footer
        [-5]      hdr_len, one byte, always 8..0x0B — the footer's own size
        [-4..]    inc_len — how many bytes the image gains when decompressed
    """
    if len(data) < 12:
        return None
    inc_len = struct.unpack("<I", data[-4:])[0]
    if inc_len == 0:
        return None                      # explicitly marked "not coded"
    hdr_len = data[-5]
    if not (0x08 <= hdr_len <= 0x0B):
        return None
    enc_len = struct.unpack("<I", data[-8:-4])[0] & 0x00FFFFFF
    if enc_len == 0 or enc_len > len(data) or len(data) <= hdr_len:
        return None
    return inc_len, hdr_len, enc_len


def looks_compressed(data):
    return _blz_footer(data) is not None


def blz_decompress(data):
    """Nintendo 'BLZ' backwards LZ, as used for DS ARM9 and ARM7 modules.

    The compressed region sits at the *end* of the module and is stored
    reversed, so the decoder mirrors it, runs an ordinary forward LZ over it,
    and mirrors the result back. Everything before that region is stored
    verbatim and is copied straight through.

    Returns ``(image, True)`` on success, or the input unchanged with ``False``
    when the module carries no valid footer.
    """
    footer = _blz_footer(data)
    if footer is None:
        return data, False
    inc_len, hdr_len, enc_len = footer

    dec_len = len(data) - enc_len        # the verbatim prefix
    pak_len = enc_len - hdr_len          # the compressed bytes themselves
    raw_len = dec_len + enc_len + inc_len

    raw = bytearray(raw_len)
    raw[:dec_len] = data[:dec_len]

    # Mirror the compressed block so it can be walked forwards.
    pak = bytearray(data[dec_len:dec_len + pak_len])
    pak.reverse()

    src = 0
    dst = dec_len
    mask = 0
    flags = 0
    while dst < raw_len:
        if mask == 0:
            if src >= pak_len:
                break
            flags = pak[src]
            src += 1
            mask = 0x80
        if not (flags & mask):
            if src >= pak_len:
                break
            raw[dst] = pak[src]
            src += 1
            dst += 1
        else:
            if src + 1 >= pak_len:
                break
            token = (pak[src] << 8) | pak[src + 1]
            src += 2
            length = (token >> 12) + 3
            disp = (token & 0x0FFF) + 3
            if dst + length > raw_len:
                length = raw_len - dst
            for _ in range(length):
                raw[dst] = raw[dst - disp]
                dst += 1
        mask >>= 1

    # Mirror the decoded block back into place.
    tail = raw[dec_len:]
    tail.reverse()
    raw[dec_len:] = tail
    return bytes(raw), True


# ------------------------------------------------------------- autoloads ----

NITROCODE = b"\x21\x06\xc0\xde"          # 0xDEC00621, little-endian
NITROCODE_BE = b"\xde\xc0\x06\x21"      # 0x2106C0DE, its byte-swapped twin


def find_module_params(image, ram_base, entry):
    """Locate the SDK MODULE_PARAMS structure inside an unpacked module.

    It is identified by the two magic words the linker plants at +0x1C and
    +0x20, not by a pointer, because the pointer's location moves between SDK
    revisions while the magic does not::

        +0x00 autoload_list_start   +0x10 static_bss_end
        +0x04 autoload_list_end     +0x14 compressed_static_end
        +0x08 autoload_start        +0x18 sdk_version
        +0x0C static_bss_start      +0x1C 0xDEC00621
                                    +0x20 0x2106C0DE
    """
    pos = 0
    while True:
        pos = image.find(NITROCODE, pos)
        if pos < 0:
            return None, None
        if image[pos + 4:pos + 8] != NITROCODE_BE:
            pos += 4
            continue
        base = pos - 0x1C
        if base < 0:
            pos += 4
            continue
        (list_start, list_end, auto_start, bss_start, bss_end,
         comp_end, sdk_version) = struct.unpack("<7I", image[base:base + 0x1C])
        limit = ram_base + len(image)
        if not (ram_base <= list_start <= list_end <= limit):
            pos += 4
            continue
        if (list_end - list_start) % 12 != 0:
            pos += 4
            continue
        if not (ram_base <= auto_start <= limit):
            pos += 4
            continue
        return ram_base + base, {
            "autoload_list_start": list_start,
            "autoload_list_end": list_end,
            "autoload_start": auto_start,
            "static_bss_start": bss_start,
            "static_bss_end": bss_end,
            "compressed_static_end": comp_end,
            "sdk_version": "0x%08x" % sdk_version,
            "found_by": "nitrocode magic",
        }


def find_autoload_table(image, ram_base):
    """Locate an autoload table with no magic to identify it.

    LEGO Battles' ARM7 has no `MODULE_PARAMS` magic — the pointer is simply a
    literal in the boot stub. Find the structure by its own shape instead: three
    consecutive words where the first two bound a whole number of 12-byte
    autoload entries inside the image, the third points into the image, and
    every entry it describes has a plausible RAM address and a size that fits in
    what is left of the image.

    This matters more than it sounds. The ARM7 keeps only 488 of its 159,528
    bytes at the address the cartridge header gives; everything else is copied
    elsewhere and runs there. A disassembler pointed at the header address is
    reading 99.7% of the module at the wrong address.
    """
    limit = ram_base + len(image)
    best = None
    for off in range(0, len(image) - 20, 4):
        list_start, list_end, data_start = struct.unpack_from("<3I", image, off)
        if not (ram_base <= list_start < list_end <= limit):
            continue
        span = list_end - list_start
        if span == 0 or span % 12 or span > 12 * 16:
            continue
        if not (ram_base <= data_start < limit):
            continue
        cursor = data_start - ram_base
        entries = []
        ok = True
        for e in range(list_start - ram_base, list_end - ram_base, 12):
            ram, size, bss = struct.unpack_from("<3I", image, e)
            # A destination anywhere in the DS address space, a size that fits,
            # and a non-empty section.
            if not (0x01000000 <= ram < 0x0A000000) or size == 0:
                ok = False
                break
            if cursor + size > len(image):
                ok = False
                break
            entries.append((ram, size, bss))
            cursor += size
        if not ok or not entries:
            continue
        # The copier must consume the image up to the table itself.
        if not (list_start - ram_base - 12 <= cursor <= list_start - ram_base):
            continue
        cand = {
            "autoload_list_start": list_start,
            "autoload_list_end": list_end,
            "autoload_start": data_start,
            "found_by": "structural scan (no nitrocode magic)",
        }
        bss = struct.unpack_from("<2I", image, off + 12)
        cand["static_bss_start"], cand["static_bss_end"] = bss
        if best is None or len(entries) > best[2]:
            best = (ram_base + off, cand, len(entries))
    return (best[0], best[1]) if best else (None, None)


# Section names are per-CPU: 0x027E0000 is the ARM9's DTCM, but the ARM7 has no
# DTCM and sees that address as an ordinary mirror of main RAM. Naming it "dtcm"
# in the ARM7's table would be actively misleading to anything reading it later.
SECTION_NAMES = {
    "arm9": {0x01FF8000: "itcm", 0x027E0000: "dtcm"},
    "arm7": {0x037F8000: "wram", 0x027E0000: "mainram"},
}


def split_autoloads(image, ram_base, entry, cpu="arm9"):
    """Split a module into its autoload sections.

    Returns a list of {name, ram, size, file_offset} plus the residual static
    section that stays at ram_base.
    """
    addr, params = find_module_params(image, ram_base, entry)
    if params is None:
        addr, params = find_autoload_table(image, ram_base)
    sections = []
    if params is None:
        sections.append({
            "name": "static",
            "ram_address": ram_base,
            "size": len(image),
            "source": "whole image (no MODULE_PARAMS found)",
        })
        return sections, None

    data_start = params["autoload_start"]

    # Everything before the autoload data block is the static module.
    sections.append({
        "name": "static",
        "ram_address": ram_base,
        "size": data_start - ram_base,
        "bss_start": params["static_bss_start"],
        "bss_end": params["static_bss_end"],
        "source": "static, up to the autoload data block",
    })

    cursor = data_start - ram_base
    for idx, off in enumerate(range(params["autoload_list_start"] - ram_base,
                                    params["autoload_list_end"] - ram_base, 12)):
        ram, size, bss = struct.unpack("<3I", image[off:off + 12])
        name = SECTION_NAMES[cpu].get(ram, "autoload_%08x" % ram)
        sections.append({
            "name": name,
            "ram_address": ram,
            "size": size,
            "bss_size": bss,
            "source": "autoload entry %d" % idx,
            "_image_offset": cursor,
        })
        cursor += size

    params["module_params_address"] = addr
    return sections, params


# ------------------------------------------------------------- overlays ----

OverlayEntry = struct.Struct("<8I")


def parse_overlay_table(raw):
    out = []
    for i in range(0, len(raw), 32):
        (oid, ram, size, bss, sinit_start, sinit_end,
         file_id, compressed_flag) = OverlayEntry.unpack(raw[i:i + 32])
        out.append({
            "id": oid,
            "ram_address": ram,
            "ram_size": size,
            "bss_size": bss,
            "static_init_start": sinit_start,
            "static_init_end": sinit_end,
            "file_id": file_id,
            "compressed_size": compressed_flag & 0x00FFFFFF,
            "compressed": bool(compressed_flag >> 24 & 1),
        })
    return out


# ---------------------------------------------------------------- FNT/FAT ---

def parse_fat(raw):
    return [struct.unpack("<2I", raw[i:i + 8]) for i in range(0, len(raw), 8)]


def parse_fnt(raw):
    """Return {file_id: full_path} and {dir_id: full_path}."""
    if len(raw) < 8:
        return {}, {}
    root_offset, root_first_file, total_dirs = struct.unpack("<IHH", raw[0:8])

    paths = {}
    dirs = {0xF000: ""}

    def walk(dir_id, prefix):
        idx = dir_id & 0x0FFF
        base = idx * 8
        if base + 8 > len(raw):
            return
        sub_offset, first_file_id, _parent = struct.unpack(
            "<IHH", raw[base:base + 8])
        p = sub_offset
        file_id = first_file_id
        while p < len(raw):
            ctrl = raw[p]
            p += 1
            if ctrl == 0:
                break
            length = ctrl & 0x7F
            name = raw[p:p + length].decode("ascii", "replace")
            p += length
            if ctrl & 0x80:
                sub_id = struct.unpack("<H", raw[p:p + 2])[0]
                p += 2
                child = prefix + name + "/"
                dirs[sub_id] = child
                walk(sub_id, child)
            else:
                paths[file_id] = prefix + name
                file_id += 1

    walk(0xF000, "")
    return paths, dirs


# -------------------------------------------------------------------- io ----

def sha1(data):
    return hashlib.sha1(data).hexdigest()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("outdir")
    ap.add_argument("--no-nitrofs", action="store_true",
                    help="parse the filesystem but do not write the files out")
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        rom = fh.read()

    out = args.outdir
    os.makedirs(out, exist_ok=True)

    hdr_raw = rom[:0x200]
    hdr = parse_header(hdr_raw)
    write(os.path.join(out, "header.bin"), hdr_raw)

    report = {
        "rom": os.path.basename(args.rom),
        "rom_bytes": len(rom),
        "rom_sha1": sha1(rom),
        "header": hdr,
    }

    # --- ARM9 --------------------------------------------------------------
    a9_off = hdr["arm9_rom_offset"]
    a9_size = hdr["arm9_size"]
    a9_ram = hdr["arm9_ram_address"]
    a9_packed = rom[a9_off:a9_off + a9_size]
    write(os.path.join(out, "arm9", "arm9_packed.bin"), a9_packed)

    a9, was_compressed = (blz_decompress(a9_packed)
                          if looks_compressed(a9_packed) else (a9_packed, False))
    write(os.path.join(out, "arm9", "arm9_unpacked.bin"), a9)

    report["arm9"] = {
        "rom_offset": a9_off,
        "packed_size": a9_size,
        "unpacked_size": len(a9),
        "ram_address": a9_ram,
        "entry": hdr["arm9_entry_address"],
        "blz_compressed": was_compressed,
        "packed_sha1": sha1(a9_packed),
        "unpacked_sha1": sha1(a9),
    }

    sections, params = split_autoloads(a9, a9_ram, hdr["arm9_entry_address"])
    for sec in sections:
        if "_image_offset" in sec:
            blob = a9[sec["_image_offset"]:sec["_image_offset"] + sec["size"]]
            del sec["_image_offset"]
        else:
            blob = a9[:sec["size"]]
        sec["sha1"] = sha1(blob)
        write(os.path.join(out, "arm9",
                           "section_%s_%08x.bin" % (sec["name"], sec["ram_address"])),
              blob)
    report["arm9"]["module_params"] = params
    report["arm9"]["sections"] = sections

    # --- ARM7 --------------------------------------------------------------
    a7_off = hdr["arm7_rom_offset"]
    a7_size = hdr["arm7_size"]
    a7_packed = rom[a7_off:a7_off + a7_size]
    write(os.path.join(out, "arm7", "arm7_packed.bin"), a7_packed)
    a7, a7_comp = (blz_decompress(a7_packed)
                   if looks_compressed(a7_packed) else (a7_packed, False))
    write(os.path.join(out, "arm7", "arm7.bin"), a7)
    a7_ram = hdr["arm7_ram_address"]
    report["arm7"] = {
        "rom_offset": a7_off,
        "packed_size": a7_size,
        "unpacked_size": len(a7),
        "ram_address": a7_ram,
        "entry": hdr["arm7_entry_address"],
        "blz_compressed": a7_comp,
        "sha1": sha1(a7),
    }

    a7_sections, a7_params = split_autoloads(a7, a7_ram,
                                             hdr["arm7_entry_address"],
                                             cpu="arm7")
    for sec in a7_sections:
        if "_image_offset" in sec:
            blob = a7[sec["_image_offset"]:sec["_image_offset"] + sec["size"]]
            del sec["_image_offset"]
        else:
            blob = a7[:sec["size"]]
        sec["sha1"] = sha1(blob)
        write(os.path.join(out, "arm7",
                           "section_%s_%08x.bin" % (sec["name"],
                                                    sec["ram_address"])), blob)
    report["arm7"]["module_params"] = a7_params
    report["arm7"]["sections"] = a7_sections

    # --- FAT / FNT ---------------------------------------------------------
    fat = parse_fat(rom[hdr["fat_offset"]:hdr["fat_offset"] + hdr["fat_size"]])
    fnt = rom[hdr["fnt_offset"]:hdr["fnt_offset"] + hdr["fnt_size"]]
    paths, dirs = parse_fnt(fnt)
    write(os.path.join(out, "fnt.bin"), fnt)

    # --- overlays ----------------------------------------------------------
    ov_raw = rom[hdr["arm9_overlay_offset"]:
                 hdr["arm9_overlay_offset"] + hdr["arm9_overlay_size"]]
    overlays = parse_overlay_table(ov_raw)
    write(os.path.join(out, "overlays", "overlay_table.bin"), ov_raw)
    for ov in overlays:
        fid = ov["file_id"]
        if fid >= len(fat):
            ov["error"] = "file id %d out of range" % fid
            continue
        start, end = fat[fid]
        blob = rom[start:end]
        ov["rom_offset"] = start
        ov["rom_size"] = end - start
        if ov["compressed"]:
            dec, ok = blz_decompress(blob)
            ov["blz_ok"] = ok
            blob = dec
        ov["sha1"] = sha1(blob)
        ov["decompressed_size"] = len(blob)
        write(os.path.join(out, "overlays", "ov%03d.bin" % ov["id"]), blob)
    report["overlays"] = overlays

    # --- NitroFS -----------------------------------------------------------
    files = []
    overlay_file_ids = {ov["file_id"] for ov in overlays}
    for fid, (start, end) in enumerate(fat):
        rec = {
            "id": fid,
            "path": paths.get(fid),
            "rom_offset": start,
            "size": end - start,
        }
        if fid in overlay_file_ids:
            rec["role"] = "overlay"
        blob = rom[start:end]
        rec["sha1"] = sha1(blob)
        files.append(rec)
        if not args.no_nitrofs and rec["path"]:
            write(os.path.join(out, "nitrofs", rec["path"].replace("/", os.sep)),
                  blob)

    with open(os.path.join(out, "files.json"), "w", encoding="utf-8") as fh:
        json.dump(files, fh, indent=1)
    report["file_count"] = len(files)
    report["named_file_count"] = sum(1 for f in files if f["path"])
    report["directory_count"] = len(dirs)

    with open(os.path.join(out, "header.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)

    print("rom          %s" % report["rom"])
    print("sha1         %s" % report["rom_sha1"])
    print("title/code   %s / %s (%s)" % (hdr["game_title"], hdr["game_code"],
                                         hdr["maker_code"]))
    print("arm9         packed 0x%06x -> unpacked 0x%06x @ 0x%08x  (blz=%s)"
          % (a9_size, len(a9), a9_ram, was_compressed))
    for sec in sections:
        print("  section    %-22s 0x%08x  size 0x%06x"
              % (sec["name"], sec["ram_address"], sec["size"]))
    print("arm7         0x%06x @ 0x%08x  (blz=%s)"
          % (len(a7), hdr["arm7_ram_address"], a7_comp))
    for sec in a7_sections:
        print("  section    %-22s 0x%08x  size 0x%06x"
              % (sec["name"], sec["ram_address"], sec["size"]))
    print("overlays     %d" % len(overlays))
    print("files        %d (%d named, %d directories)"
          % (len(files), report["named_file_count"], len(dirs)))
    print("written to   %s" % out)


if __name__ == "__main__":
    sys.exit(main())
