"""Find every place the cartridge adds a SCREEN-SIZED constant to a coordinate.

GOAL WITHIN THE PROJECT: the adaptive (widened) view. It only works where the
game agrees to consider things outside its own 256x192. docs/226 found one place
that decides that -- func_02038F28's entity gather rect, `add r2, r2, #0x100` --
and the widened band was empty of units for a long stretch because the patch
that widens it was gated off.

One such rect was found by reading a function that happened to be under
investigation. That is not a search. If there is a second one -- for particles,
for effects, for a minimap, for anything that asks "is this on screen?" -- the
same symptom would appear for that subsystem alone and would be just as hard to
attribute.

So enumerate them from the binary instead of stumbling on them.

WHAT IS MATCHED. ARM data-processing immediates are an 8-bit value rotated right
by an even amount, so 256 and 192 each have exactly one canonical encoding:

    256 -> imm12 0xC01   (0x01 ror 24)
    192 -> imm12 0x0C0   (0xC0 ror 0)

and the instruction word for `add Rd, Rn, #imm` is 0xE28nd000 | imm12. Rather
than guess which registers, match on the opcode and the immediate and report the
registers found. `cmp` (0xE35n0000 | imm12) is included because a bounds test is
as interesting as a rect: "is x past the right edge" is exactly the shape of
thing that would refuse an out-of-band coordinate.

Thumb is NOT covered. Its immediates are 8-bit unrotated, so 256 cannot appear
as one at all, and 192 only via a different encoding; the rects this is looking
for are computed in ARM code in this game. Recorded so the gap is known rather
than assumed absent.

    python tools/lb_find_screen_rects.py
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "out", "eur", "arm9", "section_static_02000000.bin")
BASE = 0x02000000

# (label, imm12, decimal) -- the two screen dimensions.
IMMS = [("256", 0xC01, 256), ("192", 0x0C0, 192)]


def main():
    if not os.path.exists(BIN):
        print("missing %s" % BIN)
        return 2
    with open(BIN, "rb") as fh:
        data = fh.read()
    print("scanning %s, %d bytes at %08X" % (os.path.basename(BIN),
                                             len(data), BASE))
    hits = {}
    for off in range(0, len(data) - 3, 4):
        word = struct.unpack_from("<I", data, off)[0]
        if (word >> 28) != 0xE:                 # unconditional only
            continue
        imm = word & 0xFFF
        for label, want, _dec in IMMS:
            if imm != want:
                continue
            # add and sub are matched with or without S (bit 20). cmp has
            # S set by definition, so its test must keep bit 20: under the
            # 0x0FE00000 mask this used to apply, every `cmp #imm` read as
            # 0x03400000 and the cmp branch could never match -- which is why
            # the scan never found the effect culls docs/350 fixed (the
            # projectile agent, docs/353).
            op = word & 0x0FE00000
            if op == 0x02800000:                # add Rd, Rn, #imm
                kind = "add"
            elif (word & 0x0FF00000) == 0x03500000:   # cmp Rn, #imm
                kind = "cmp"
            elif op == 0x02400000:              # sub Rd, Rn, #imm
                kind = "sub"
            else:
                continue
            rn = (word >> 16) & 0xF
            rd = (word >> 12) & 0xF
            addr = BASE + off
            hits.setdefault(label, []).append((addr, word, kind, rn, rd))

    # PAIRS, not sites. 256 on its own is one of the most common constants in
    # any binary -- buffer sizes, stack frames, alignment -- and this scan found
    # 334 of them, which is a list nobody can act on. A screen RECT is different
    # and recognisable: it computes both dimensions within a few instructions of
    # each other, which is exactly what func_02038F28 does with `+0x100` for x
    # and `+0xC0` for y. Requiring the pair is what turns a haystack into a
    # short list worth reading.
    WINDOW = 64                                # bytes either side
    wide = hits.get("256", [])
    tall = hits.get("192", [])
    tall_addrs = [r[0] for r in tall]
    print("")
    print("== %d sites use #256, %d use #192" % (len(wide), len(tall)))
    print("== PAIRS within %d bytes -- the shape of a screen rect:" % WINDOW)
    pairs = 0
    for addr, word, kind, rn, rd in wide:
        near = [a for a in tall_addrs if abs(a - addr) <= WINDOW]
        if not near:
            continue
        pairs += 1
        note = ""
        if addr == 0x02038FD0:
            note = "   <-- the entity gather rect, docs/226"
        print("   %08X  %08X  %-3s r%-2d <- r%-2d   with #192 at %s%s"
              % (addr, word, kind, rd, rn,
                 ",".join("%08X" % a for a in near), note))
    if not pairs:
        print("   (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
