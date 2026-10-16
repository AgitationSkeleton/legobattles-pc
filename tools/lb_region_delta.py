"""The USA-to-Europe conversion data for the first-run setup (docs/394, docs/395).

    py -3.13 tools/lb_region_delta.py OUT.lbdelta --eu EU.nds --us US.nds
    (or set LB_ROM_EU / LB_ROM_US)

The port is built on the European cartridge: its recompiled code is registered
for that image's SHA-1 alone. The two releases share every game file byte for
byte and differ only in the first 0x126000 bytes (docs/04): the header, the
ARM9 image, the 91 ARM9 overlays, their table and the FAT entries that place
them.

WHAT THIS FILE CARRIES (docs/395). The first version carried the European
bytes of those regions -- 894 KB, almost all of it compressed European code.
This one is built so that the European code comes from the player's own USA
cartridge:

  * every code module (the ARM9 and each overlay) is unpacked from the USA
    image, and the unpacked European modules are rebuilt from them with
    bsdiff's operations -- copy n bytes from the USA module adding a per-byte
    difference, insert m bytes, move. The differences are address
    adjustments (the European build moved code by a few hundred bytes); they
    are stored sparse, as numbers to add, not as code. Only bytes that have no
    USA counterpart at all are inserted (a few hundred);
  * a module is packed again by replaying the cartridge's own BLZ parse: the
    flag bytes and back-reference tokens are stored, the literal bytes are
    stripped and taken back from the rebuilt module. So the packed image is
    reproduced exactly without storing what it packs (the overlays were packed
    by a different encoder than the ARM9, so no encoder is rerun);
  * what is left -- the header, the overlay table, the FAT entries and the
    padding -- is stored as byte runs.

The setup checks the result against the European SHA-1 before keeping it.
The file is still made from two dumps and kept out of git.

Format (little-endian), "LBDELTA2":
    8 magic, 20 source SHA-1 (USA), 20 target SHA-1 (Europe), u32 image size
    u32 fill start, u32 fill end            (0xFF filled before modules land)
    u32 M, then M modules:
        u32 USA offset, u32 USA length, u32 Europe offset, u32 Europe length,
        u32 Europe unpacked length, u8 USA packed, u8 Europe packed, u16 0,
        u32 T, T bytes  the Europe module's footer (its last hdr_len bytes),
        u32 S, S bytes  its BLZ parse with the literals stripped
    u32 N, then N x (i32 copy, i32 insert, i32 seek)   over the modules,
                                                       unpacked and concatenated
    u32 R, then R x (u32 skip, u32 len, len bytes)     sparse differences
    u32 E, then E bytes                                inserted bytes
    u32 K, then K x (u32 offset, u32 len, len bytes)   ROM runs
"""

import argparse
import bz2
import hashlib
import os
import struct
import sys

import bsdiff4

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nds_extract import blz_decompress  # noqa: E402

EU_SHA1 = "da60469bd143eb8f47ecf0d7951181b4783c91b4"
US_SHA1 = "f25d090feb840e9b4b07546b6929d897e3fb3965"
HEAD = 0x126000


def modules(rom):
    """(offset, length, packed) for the ARM9 and each ARM9 overlay, in
    overlay-id order. Packing is read from the header (the ARM9 always is)
    and from each overlay entry's flag byte."""
    out = [(struct.unpack_from("<I", rom, 0x20)[0], struct.unpack_from("<I", rom, 0x2C)[0], True)]
    fat = struct.unpack_from("<I", rom, 0x48)[0]
    off, size = struct.unpack_from("<II", rom, 0x50)
    for i in range(size // 32):
        e = struct.unpack_from("<8I", rom, off + 32 * i)
        s, t = struct.unpack_from("<II", rom, fat + 8 * e[6])
        out.append((s, t - s, bool((e[7] >> 24) & 1)))
    return out


def unpack(data, packed):
    if not packed:
        return data
    raw, ok = blz_decompress(data)
    assert ok
    return raw


def parse_of(data):
    """The BLZ parse of a packed module with its literal bytes stripped:
    flag bytes and back-reference tokens, in decoding order."""
    n = len(data)
    enc, hdr = int.from_bytes(data[n - 8:n - 5], "little"), data[n - 5]
    pak = data[n - enc:n - hdr][::-1]
    sk, i = bytearray(), 0
    while i < len(pak):
        f = pak[i]
        sk.append(f)
        i += 1
        for b in range(8):
            if i >= len(pak):
                break
            if f & (0x80 >> b):
                sk += pak[i:i + 2]
                i += 2
            else:
                i += 1
    return bytes(sk), data[n - hdr:]


def repack(raw, tail, sk):
    """Replay a stored parse over an unpacked module: the inverse of
    parse_of + blz_decompress."""
    enc = int.from_bytes(tail[-8:-5], "little")
    hdr = tail[-5]
    inc = struct.unpack("<I", tail[-4:])[0]
    dec = len(raw) - enc - inc
    rev = raw[dec:][::-1]
    pak_len = enc - hdr
    pak, s, r = bytearray(), 0, 0
    while len(pak) < pak_len:
        f = sk[s]
        s += 1
        pak.append(f)
        for b in range(8):
            if len(pak) >= pak_len:
                break
            if f & (0x80 >> b):
                tok = (sk[s] << 8) | sk[s + 1]
                pak += sk[s:s + 2]
                s += 2
                r += (tok >> 12) + 3
            else:
                pak.append(rev[r])
                r += 1
    return raw[:dec] + bytes(pak[::-1]) + bytes(tail)


def bsdiff_parts(old, new):
    p = bsdiff4.diff(old, new)
    assert p[:8] == b"BSDIFF40"
    lc, ld, _ = struct.unpack("<qqq", p[8:32])
    ctrl = bz2.decompress(p[32:32 + lc])
    diff = bz2.decompress(p[32 + lc:32 + lc + ld])
    extra = bz2.decompress(p[32 + lc + ld:])

    def offt(b):
        v = struct.unpack("<q", b)[0]
        return -(v & 0x7FFFFFFFFFFFFFFF) if v < 0 else v
    trip = [tuple(offt(ctrl[i + k * 8:i + k * 8 + 8]) for k in range(3))
            for i in range(0, len(ctrl), 24)]
    return trip, diff, extra


def sparse(diff):
    runs, i, n = [], 0, len(diff)
    while i < n:
        j = i
        while j < n and diff[j] == 0:
            j += 1
        if j >= n:
            break
        k = j
        while k < n and (diff[k] != 0 or (k + 1 < n and diff[k + 1] != 0)):
            k += 1
        runs.append((j - i, diff[j:k]))
        i = k
    return runs


def byte_runs(src, dst, gap=16):
    out, i, n = [], 0, len(dst)
    while i < n:
        if src[i] == dst[i]:
            i += 1
            continue
        start = last = i
        j = i + 1
        while j < n and j - last <= gap:
            if src[j] != dst[j]:
                last = j
            j += 1
        out.append((start, last + 1))
        i = last + 1
    return out


class Reader:
    def __init__(self, b, at=0):
        self.b, self.at = b, at

    def u(self, fmt):
        v = struct.unpack_from(fmt, self.b, self.at)
        self.at += struct.calcsize(fmt)
        return v

    def take(self, n):
        v = self.b[self.at:self.at + n]
        self.at += n
        return v


def convert(us_rom, blob, body=None):
    """The setup's conversion, in Python, to prove the file before writing it.
    With body, the modules are laid onto that image (used to compute the
    ROM runs); otherwise the runs are applied too."""
    rd = Reader(blob, 8 + 40)
    (size,) = rd.u("<I")
    fill_a, fill_b = rd.u("<II")
    (m,) = rd.u("<I")
    mods = []
    for _ in range(m):
        uo, ul, eo, el, eraw, upk, epk, _pad = rd.u("<IIIIIBBH")
        tail = rd.take(rd.u("<I")[0])
        sk = rd.take(rd.u("<I")[0])
        mods.append((uo, ul, eo, el, eraw, upk, epk, tail, sk))
    old = b"".join(unpack(us_rom[uo:uo + ul], upk) for uo, ul, _, _, _, upk, _, _, _ in mods)
    (nc,) = rd.u("<I")
    ctrl = [rd.u("<iii") for _ in range(nc)]
    (nr,) = rd.u("<I")
    diff = bytearray()
    for _ in range(nr):
        skip, ln = rd.u("<II")
        diff += bytes(skip) + rd.take(ln)
    extra = rd.take(rd.u("<I")[0])
    total = sum(c[0] for c in ctrl)
    diff += bytes(total - len(diff))
    new = bytearray(sum(md[4] for md in mods))
    np_ = op = dp = ep = 0
    for x, y, z in ctrl:
        for k in range(x):
            new[np_ + k] = (old[op + k] + diff[dp + k]) & 0xFF
        np_ += x
        op += x
        dp += x
        new[np_:np_ + y] = extra[ep:ep + y]
        np_ += y
        ep += y
        op += z
    img = bytearray(us_rom if body is None else body)
    assert len(img) == size
    img[fill_a:fill_b] = b"\xFF" * (fill_b - fill_a)
    at = 0
    for uo, ul, eo, el, eraw, upk, epk, tail, sk in mods:
        raw = bytes(new[at:at + eraw])
        at += eraw
        data = repack(raw, tail, sk) if epk else raw
        assert len(data) == el
        img[eo:eo + el] = data
    if body is not None:
        return bytes(img)
    (nk,) = rd.u("<I")
    for _ in range(nk):
        off, ln = rd.u("<II")
        img[off:off + ln] = rd.take(ln)
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--eu", default=os.environ.get("LB_ROM_EU"))
    ap.add_argument("--us", default=os.environ.get("LB_ROM_US"))
    args = ap.parse_args()
    if not args.eu or not args.us:
        ap.error("give --eu and --us (or set LB_ROM_EU and LB_ROM_US)")
    eu = open(args.eu, "rb").read()
    us = open(args.us, "rb").read()
    assert hashlib.sha1(eu).hexdigest() == EU_SHA1, "not the European dump"
    assert hashlib.sha1(us).hexdigest() == US_SHA1, "not the USA dump"
    assert len(eu) == len(us) and eu[HEAD:] == us[HEAD:]
    mu, me = modules(us), modules(eu)
    assert len(mu) == len(me)
    old = b"".join(unpack(us[o:o + n], p) for o, n, p in mu)
    raws = [unpack(eu[o:o + n], p) for o, n, p in me]
    trip, diff, extra = bsdiff_parts(old, b"".join(raws))
    adds = sparse(diff)
    fill_a = me[0][0]
    fill_b = struct.unpack_from("<I", eu, 0x30)[0]          # the ARM7's offset
    assert all(fill_a <= o and o + n <= fill_b for o, n, _ in me + mu)

    blob = bytearray(b"LBDELTA2")
    blob += bytes.fromhex(US_SHA1) + bytes.fromhex(EU_SHA1)
    blob += struct.pack("<III", len(eu), fill_a, fill_b)
    blob += struct.pack("<I", len(me))
    parse_bytes = 0
    for (uo, ul, up), (eo, el, ep), raw in zip(mu, me, raws):
        tail, sk = (b"", b"")
        if ep:
            sk, tail = parse_of(eu[eo:eo + el])
            assert repack(raw, tail, sk) == eu[eo:eo + el]
            parse_bytes += len(sk)
        blob += struct.pack("<IIIIIBBH", uo, ul, eo, el, len(raw), up, ep, 0)
        blob += struct.pack("<I", len(tail)) + tail + struct.pack("<I", len(sk)) + sk
    blob += struct.pack("<I", len(trip))
    for x, y, z in trip:
        blob += struct.pack("<iii", x, y, z)
    blob += struct.pack("<I", len(adds))
    for skip, b in adds:
        blob += struct.pack("<II", skip, len(b)) + b
    blob += struct.pack("<I", len(extra)) + extra
    laid = convert(us, bytes(blob) + struct.pack("<I", 0), body=us)
    rom_runs = byte_runs(laid[:HEAD], eu[:HEAD])
    blob += struct.pack("<I", len(rom_runs))
    for a, b in rom_runs:
        blob += struct.pack("<II", a, b - a) + eu[a:b]
    img = convert(us, bytes(blob))
    assert hashlib.sha1(img).hexdigest() == EU_SHA1, "the conversion does not rebuild the European image"
    open(args.out, "wb").write(blob)
    print("wrote %s: %d bytes -- %d modules, %d parse bytes (no literals), %d control triples, "
          "%d difference bytes in %d runs, %d inserted bytes, %d ROM bytes in %d runs "
          "(header, overlay table, FAT)"
          % (args.out, len(blob), len(me), parse_bytes, len(trip),
             sum(len(b) for _, b in adds), len(adds), len(extra),
             sum(b - a for a, b in rom_runs), len(rom_runs)))
    for a, b in rom_runs:
        if b - a > 64:
            print("  run 0x%06x +%d" % (a, b - a))


if __name__ == "__main__":
    main()
