#!/usr/bin/env python3
"""Probe a LIVE gameplay session started by `lb_exp.py --keep`.

    python tools/lb_probe.py entities
    python tools/lb_probe.py polys
    python tools/lb_probe.py camera

The route to Free Play costs about a minute and a half, and two cull experiments
have already been thrown away because the camera happened to sit somewhere with
no units in the widened area. Holding one instance open and asking it questions
is the cheap fix; a savestate would be the other one, and this runner cannot
have a cheap one (static recompiler, guest CPU state in the recompiled register
file, vendored Savestate.cpp unused).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dbg import Dbg, assert_single_runner  # noqa: E402

PORT = 19857
ENTITY_POOL = 0x02155028


def rd(d, addr, n):
    return bytes.fromhex(d.cmd("read_mem", cpu=9, addr=addr, len=n)["hex"])


def u32(b, o=0):
    return int.from_bytes(b[o:o + 4], "little")


def s32(b, o=0):
    return int.from_bytes(b[o:o + 4], "little", signed=True)


def cmd_entities(d, args):
    """Every live entity and where it is, in world pixels.

    +0x0EC / +0x0F0 are the world X and Y in 20.12 fixed point, and +0x155 is
    the active flag func_0205A9A8 returns. Knowing where the units actually are
    is the difference between a cull experiment that proves something and one
    that pans across empty sea.
    """
    pool = u32(rd(d, ENTITY_POOL, 4))
    head = rd(d, pool, 0x10)
    array, count = u32(head, 8), u32(head, 0xC)
    print("pool %08X  array %08X  count %d" % (pool, array, count))
    refs = rd(d, array, 4 * min(count, 256))
    shown = 0
    for i in range(min(count, 256)):
        e = u32(refs, i * 4)
        if not e:
            continue
        body = rd(d, e + 0xE8, 0x10)
        wx, wy = s32(body, 4) / 4096.0, s32(body, 8) / 4096.0
        active = rd(d, e + 0x155, 1)[0]
        owner = rd(d, e + 0xE4, 1)[0]
        print("  [%3d] %08X  world (%8.2f, %8.2f)  active %d  owner %d"
              % (i, e, wx, wy, active, owner))
        shown += 1
        if shown >= args.limit:
            print("  ... %d more" % (count - i - 1))
            break


def cmd_polys(d, args):
    r = d.cmd("gx_polygons")
    p = r["polygons"]
    outside = [q for q in p if q["min_x"] < 96 or q["max_x"] > 351]
    print("%d polygons, %d outside the native band (96..351)"
          % (len(p), len(outside)))
    for q in sorted(p, key=lambda q: q["min_z"]):
        print("  z %8d  x %5d..%-5d y %5d..%-5d"
              % (q["min_z"], q["min_x"], q["max_x"], q["min_y"], q["max_y"]))


def cmd_camera(d, args):
    """Locate the view object by watching which halfword pair tracks a pan.

    The view is a callback context, never a global this project has pinned down,
    so it is found by behaviour instead: hold a direction, and the camera is the
    s16 pair at +0x5E/+0x60 of whatever structure moved consistently.
    """
    lo, hi = 0x02150000, 0x02158000
    before = rd(d, lo, hi - lo)
    d.cmd("keys", mask=0x3FF & ~0x10)
    import time
    time.sleep(0.8)
    d.cmd("keys", mask=0x3FF)
    time.sleep(0.4)
    after = rd(d, lo, hi - lo)
    # A moving halfword is not enough on its own: the tilemap staging buffers
    # sit in this window and every entry in them moves on a scroll, which
    # buried the answer in 1127 hits. The view is a STRUCTURE, so require the
    # rest of it to look right too -- func_02038504 stores four callback
    # handles at +0x4C..+0x58, so those must be main-RAM pointers, and the Y
    # camera at +0x60 must NOT move when the pan is purely horizontal.
    def s16(buf, off):
        return int.from_bytes(buf[off:off + 2], "little", signed=True)

    def u32at(buf, off):
        return int.from_bytes(buf[off:off + 4], "little")

    hits = []
    for off in range(0, len(before) - 4, 2):
        a, b = s16(before, off), s16(after, off)
        if a == b or not (0 < abs(b - a) < 400):
            continue
        base = off - 0x5E
        if base < 0 or base + 0x62 > len(before):
            continue
        if s16(before, base + 0x60) != s16(after, base + 0x60):
            continue                                  # Y moved: not a pure X
        handles = [u32at(before, base + o) for o in (0x4C, 0x50, 0x54, 0x58)]
        if not all(0x02000000 <= h < 0x02400000 for h in handles):
            continue
        hits.append((lo + base, a, b, handles))

    print("%d candidates survived the structure test" % len(hits))
    for base, a, b, handles in hits[:12]:
        print("  view %08X  camx %6d -> %-6d  camy %6d  handles %s"
              % (base, a, b, s16(after, base - lo + 0x60),
                 " ".join("%08X" % h for h in handles)))


VIEW_PTR = 0x02152034           # func_02039A5C: str r0, [=0x02152030, #4]


def cmd_view(d, args):
    """The view object, and whether its camera really is at +0x5E/+0x60.

    func_02039A5C allocates 0x1B0 bytes, constructs it with func_0203810C and
    stores the pointer at 0x02152030+4. Everything else in this project that
    wanted the camera -- the cull rect, the tilemap fill, the scroll callback --
    reads it from that object, so this is the address the widening work needs.

    Verified by behaviour, not by reading the disassembly twice: pan, and the X
    must move while Y stays put.
    """
    import time
    view = u32(rd(d, VIEW_PTR, 4))
    print("view object @%08X (from %08X)" % (view, VIEW_PTR))
    if not (0x02000000 <= view < 0x02400000):
        print("  not a main-RAM pointer -- not in a battle?")
        return 1

    def cam():
        b = rd(d, view + 0x5E, 4)
        return (int.from_bytes(b[0:2], "little", signed=True),
                int.from_bytes(b[2:4], "little", signed=True))

    before = cam()
    d.cmd("keys", mask=0x3FF & ~0x10)              # right
    time.sleep(0.8)
    d.cmd("keys", mask=0x3FF)
    time.sleep(0.4)
    after = cam()
    print("  camera %s -> %s" % (before, after))
    if after[0] != before[0] and after[1] == before[1]:
        print("  CONFIRMED: +0x5E is camera X, +0x60 is camera Y")
    else:
        print("  NOT confirmed: X should move and Y should not")
    print("  dirty flag +0x66 = %d" % rd(d, view + 0x66, 1)[0])
    return 0


CULL_INSN = 0x02038FD0          # add r2, r2, #0x100 in func_02038F28
CULL_STOCK = 0xE2822C01


def arm_imm12(v):
    """Encode v as an ARM data-processing immediate, or None."""
    for rot in range(16):
        r = ((v << (2 * rot)) | (v >> (32 - 2 * rot))) & 0xFFFFFFFF
        if r <= 0xFF:
            return (rot << 8) | r
    return None


def cmd_cull(d, args):
    """Rewrite the entity-cull view WIDTH and report what changes.

    func_02038F28 builds the gather rectangle as
    (camx, camy) .. (camx + 0x100, camy + 0xC0) and hands it to
    func_020882BC, which collects the entities inside it; anything outside is
    never drawn. That 0x100 is one ARM immediate, so the bound can be tested
    without building anything.
    """
    cur = int.from_bytes(bytes.fromhex(
        d.cmd("read_mem", cpu=9, addr=CULL_INSN, len=4)["hex"]), "little")
    print("insn @%08X = %08X (stock %08X)" % (CULL_INSN, cur, CULL_STOCK))
    if args.limit <= 0:
        return 0
    imm = arm_imm12(args.limit)
    if imm is None:
        print("%d is not encodable as an ARM immediate" % args.limit)
        return 1
    # Accept any `add r2, r2, #imm` -- the same instruction with any immediate.
    # Insisting on the stock word made REVERTING impossible, which meant the
    # control run for this experiment silently did not happen and the "after"
    # reading was mistaken for a "before".
    if (cur & 0xFFFFF000) != 0xE2822000:
        print("refusing: %08X is not `add r2, r2, #imm`" % cur)
        return 1
    word = 0xE2822000 | imm
    d.cmd("write_mem", cpu=9, addr=CULL_INSN, value=word, width=32)
    back = int.from_bytes(bytes.fromhex(
        d.cmd("read_mem", cpu=9, addr=CULL_INSN, len=4)["hex"]), "little")
    print("wrote %08X (width %d), read back %08X" % (word, args.limit, back))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["entities", "polys", "camera",
                                       "cull", "view"])
    ap.add_argument("--limit", type=int, default=24)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    assert_single_runner(args.port)
    d = Dbg(port=args.port)
    return {"entities": cmd_entities, "polys": cmd_polys,
            "camera": cmd_camera, "cull": cmd_cull,
            "view": cmd_view}[args.action](d, args)


if __name__ == "__main__":
    raise SystemExit(main())
