#!/usr/bin/env python3
"""LEGO Battles 'COMP' container: decode, encode, and verify.

Most of this game's own data — maps, entity and faction tables, tilesets, and
the great majority of its Nitro graphics — is wrapped in a container stamped
``COMP``. The wrapper is a block-oriented LZ11 archive:

    u32  magic                 'COMP', little-endian (bytes P M O C on disk)
    u32  decompressed_size
    u32  block_count N
    i32  scratch_hint          advisory; see below
    i32  block_packed_size[N]  each padded up to a 4-byte boundary
         N back-to-back blocks. A positive entry is a Nintendo LZ11 stream
         that declares its own decompressed length in its header; a NEGATIVE
         entry means the block is stored raw and |entry| is its length.

Two properties make the format worth knowing. Blocks are almost always 4 KB
decompressed, so the game can pull one block out of a large file without
touching the rest. And the packed sizes are all derived from the payload, so a
repack need not preserve any of them — replacements may change size freely.

``scratch_hint`` is the decompressor's buffer size and is *advisory*: it tracks
the largest packed block, but two of this ROM's 2,178 containers store a value
4 bytes below their table's true maximum (``UI/AllInOne/BG_EA3M2.NCGR`` and
``BG_WA1M1.NCGR``, both of which round the winning block's length down rather
than up). It is therefore checked and reported, never enforced.

Usage:
    python tools/lb_comp.py decode <in> <out>
    python tools/lb_comp.py encode <in> <out>       # store-only, round-trippable
    python tools/lb_comp.py verify <extractdir>     # every COMP file in a ROM
    python tools/lb_comp.py decode-all <extractdir> # mirror the tree, unwrapped
"""

import argparse
import os
import struct
import sys

MAGIC = b"PMOC"          # 'COMP' as a little-endian u32
BLOCK = 0x1000           # decompressed bytes per block
HEADER = struct.Struct("<4s2Ii")   # the max field is signed, like the table


class CompError(Exception):
    pass


# ------------------------------------------------------------------ LZ11 ----

def lz11_decompress(data, offset=0, expect=None):
    """Nintendo LZ11 (compression type 0x11). Returns (bytes, consumed).

    LZ11 extends LZ77 with two longer match encodings, selected by the top
    nibble of the first token byte:

        0        length 17..272,    3 bytes total
        1        length 273..65808, 4 bytes total
        2..15    length 3..16,      2 bytes total  (plain LZ77)
    """
    p = offset
    header = struct.unpack_from("<I", data, p)[0]
    p += 4
    if header & 0xFF != 0x11:
        raise CompError("not an LZ11 stream at 0x%x (type byte 0x%02x)"
                        % (offset, header & 0xFF))
    size = header >> 8
    if size == 0:                      # extended 32-bit length form
        size = struct.unpack_from("<I", data, p)[0]
        p += 4
    if expect is not None and size != expect:
        raise CompError("LZ11 at 0x%x declares %d bytes, expected %d"
                        % (offset, size, expect))

    out = bytearray()
    flags = 0
    remaining = 0
    while len(out) < size:
        if remaining == 0:
            flags = data[p]
            p += 1
            remaining = 8
        remaining -= 1
        if not (flags & 0x80):
            out.append(data[p])
            p += 1
        else:
            b0 = data[p]
            indicator = b0 >> 4
            if indicator == 0:
                # 0 L L L  D D D D | D D D D D D D D
                length = ((b0 & 0x0F) << 4 | data[p + 1] >> 4) + 0x11
                disp = ((data[p + 1] & 0x0F) << 8 | data[p + 2]) + 1
                p += 3
            elif indicator == 1:
                # 1 L L L  L L L L | L L L L D D D D | D D D D D D D D
                length = (((b0 & 0x0F) << 12) | (data[p + 1] << 4)
                          | (data[p + 2] >> 4)) + 0x111
                disp = ((data[p + 2] & 0x0F) << 8 | data[p + 3]) + 1
                p += 4
            else:
                length = indicator + 1
                disp = ((b0 & 0x0F) << 8 | data[p + 1]) + 1
                p += 2
            if disp > len(out):
                raise CompError("LZ11 back-reference %d beyond %d produced bytes"
                                % (disp, len(out)))
            start = len(out) - disp
            for i in range(length):
                out.append(out[start + i])
        flags = (flags << 1) & 0xFF
    return bytes(out[:size]), p - offset


def lz11_store(data):
    """Emit a valid LZ11 stream that stores ``data`` verbatim.

    Every eighth byte is a flag byte of zero, so the decoder copies literals.
    Larger than the input by 1/8, and the game decodes it identically to a
    properly compressed stream — which is what a repack tool needs and all it
    needs.
    """
    if len(data) < 0x1000000:
        out = bytearray(struct.pack("<I", 0x11 | (len(data) << 8)))
    else:
        out = bytearray(struct.pack("<II", 0x11, len(data)))
    for i in range(0, len(data), 8):
        out.append(0x00)
        out += data[i:i + 8]
    return bytes(out)


# ------------------------------------------------------------------ COMP ----

def is_comp(data):
    return len(data) >= 16 and data[:4] == MAGIC


def comp_decode(data, warnings=None):
    """Unwrap a COMP container. Raises CompError on any structural fault.

    Pass a list as ``warnings`` to collect advisory mismatches that do not
    affect the decoded bytes.
    """
    if not is_comp(data):
        raise CompError("not a COMP container (magic %r)" % data[:4])
    _magic, size, count, scratch_hint = HEADER.unpack_from(data, 0)
    table_off = HEADER.size
    # Signed, because a negative entry marks a block that is stored raw — the
    # packer's escape hatch for data LZ11 cannot shrink. Its magnitude is the
    # block's length, and the payload carries no LZ11 header at all.
    packed = list(struct.unpack_from("<%di" % count, data, table_off))
    slots = [abs(n) for n in packed]

    payload = table_off + 4 * count
    if payload + sum(slots) != len(data):
        raise CompError("payload %d + table sum %d != file size %d"
                        % (payload, sum(slots), len(data)))

    # Each block's LZ11 header declares its own decompressed length, so the
    # split is self-describing. It is usually 4 KB per block, but not always —
    # 2,125 of this ROM's 2,178 containers use some other split — so read the
    # length rather than assuming it.
    out = bytearray()
    p = payload
    for i, entry in enumerate(packed):
        slot = abs(entry)
        if entry < 0:
            out += data[p:p + slot]
        else:
            block, used = lz11_decompress(data, p)
            # A stream may stop short of its slot: the retail packer pads up to
            # a 4-byte boundary, and third-party packers pad differently — the
            # community rebalance patch's Entities.ebp gives a block 1,200 bytes
            # for a 1,196-byte stream, which is already aligned. So the slot is
            # only required to be large enough, and the slack is reported.
            if used > slot:
                raise CompError("block %d consumed %d bytes, table says %d"
                                % (i, used, slot))
            if warnings is not None and slot - used > 3:
                warnings.append("block %d has %d bytes of slack (stream %d, "
                                "slot %d)" % (i, slot - used, used, slot))
            out += block
        p += slot

    if len(out) != size:
        raise CompError("decoded %d bytes, header declares %d" % (len(out), size))

    if warnings is not None and slots and abs(scratch_hint) != max(slots):
        warnings.append("scratch_hint %d != max(table) %d"
                        % (scratch_hint, max(slots)))
    return bytes(out)


def comp_encode(data):
    """Wrap raw bytes in a COMP container using store-only LZ11 blocks."""
    blocks = [lz11_store(data[i:i + BLOCK]) for i in range(0, len(data), BLOCK)]
    if not blocks:
        blocks = [lz11_store(b"")]
    sizes = [len(b) for b in blocks]
    out = bytearray(HEADER.pack(MAGIC, len(data), len(blocks), max(sizes)))
    out += struct.pack("<%di" % len(sizes), *sizes)
    for b in blocks:
        out += b
    return bytes(out)


def unwrap(data):
    """Return the payload of a COMP container, or the data if it is not one."""
    return comp_decode(data) if is_comp(data) else data


# -------------------------------------------------------------------- cli ----

def cmd_verify(extractdir):
    root = os.path.join(extractdir, "nitrofs")
    if not os.path.isdir(root):
        sys.exit("no nitrofs under %s" % extractdir)

    total = ok = 0
    payload_bytes = packed_bytes = 0
    failures = []
    advisories = []
    inner = {}
    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            full = os.path.join(dirpath, name)
            with open(full, "rb") as fh:
                raw = fh.read()
            if not is_comp(raw):
                continue
            total += 1
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            warns = []
            try:
                out = comp_decode(raw, warnings=warns)
            except CompError as exc:
                failures.append((rel, str(exc)))
                continue
            ok += 1
            for w in warns:
                advisories.append((rel, w))
            packed_bytes += len(raw)
            payload_bytes += len(out)
            inner[out[:4]] = inner.get(out[:4], 0) + 1

    print("COMP files: %d   decoded: %d   failed: %d" % (total, ok, len(failures)))
    print("packed %.2f MB -> %.2f MB  (%.2fx)"
          % (packed_bytes / 1048576, payload_bytes / 1048576,
             payload_bytes / packed_bytes if packed_bytes else 0))
    print()
    print("first four bytes of the decoded payloads:")
    for magic, count in sorted(inner.items(), key=lambda kv: -kv[1]):
        printable = "".join(chr(c) if 32 <= c < 127 else "." for c in magic)
        print("   %-12s %-10s %5d" % (magic.hex(), printable, count))
    if advisories:
        print()
        print("advisory (decoded correctly, header field disagrees): %d"
              % len(advisories))
        for rel, why in advisories[:10]:
            print("   %s: %s" % (rel, why))
    for rel, why in failures[:20]:
        print("FAIL %s: %s" % (rel, why))
    return 1 if failures else 0


def cmd_decode_all(extractdir, dest):
    """Mirror nitrofs/ into dest with every COMP container unwrapped.

    Files that are not containers are copied through unchanged, so the result
    is a tree every downstream tool can read without knowing about COMP.
    """
    root = os.path.join(extractdir, "nitrofs")
    if not os.path.isdir(root):
        sys.exit("no nitrofs under %s" % extractdir)

    unwrapped = copied = 0
    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root)
            with open(full, "rb") as fh:
                raw = fh.read()
            if is_comp(raw):
                raw = comp_decode(raw)
                unwrapped += 1
            else:
                copied += 1
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as fh:
                fh.write(raw)
    print("unwrapped %d containers, copied %d other files -> %s"
          % (unwrapped, copied, dest))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode"); d.add_argument("src"); d.add_argument("dst")
    e = sub.add_parser("encode"); e.add_argument("src"); e.add_argument("dst")
    v = sub.add_parser("verify"); v.add_argument("extractdir")
    a = sub.add_parser("decode-all")
    a.add_argument("extractdir")
    a.add_argument("dest", nargs="?", default=None,
                   help="default: <extractdir>/plain")
    args = ap.parse_args()

    if args.cmd == "verify":
        return cmd_verify(args.extractdir)
    if args.cmd == "decode-all":
        return cmd_decode_all(args.extractdir,
                              args.dest or os.path.join(args.extractdir, "plain"))

    with open(args.src, "rb") as fh:
        raw = fh.read()
    out = comp_decode(raw) if args.cmd == "decode" else comp_encode(raw)
    with open(args.dst, "wb") as fh:
        fh.write(out)
    print("%s: %d -> %d bytes" % (args.cmd, len(raw), len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
