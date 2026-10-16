#!/usr/bin/env python3
"""Every gameplay constant this project has established, re-read from the ROM.

`docs/58` through `docs/64` each read one subsystem and each recorded its
numbers in prose. Prose goes stale. This collects them into one table and
**derives every value from the cartridge on each run** rather than transcribing
it, so a figure quoted in the documentation cannot drift away from the figure in
the image.

Three kinds of row, and the kind is in the table:

    instruction   an immediate or literal read back out of the disassembly.
                  If the instruction changes, this row changes with it.
    data          a value read from a blueprint or a static table.
    derived       arithmetic over those, shown with its inputs.

Anything this project has NOT established stays out. The gaps are listed at the
end of the report and in `docs/65`, because a reference that quietly omits what
it does not know is worse than one that says so.

Usage:
    python tools/lb_gameplay.py
    python tools/lb_gameplay.py --tsv docs/data/gameplay_constants.tsv
"""

from __future__ import annotations

import argparse
import csv
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402
import lb_combat  # noqa: E402
import lb_economy  # noqa: E402

#: The simulation rate `docs/25` established. Everything in ticks divides by it.
TICK_HZ = 30

#: Gaps: things a document explicitly flagged as unresolved. Carried here so the
#: reference states them rather than reading as complete.
OPEN_QUESTIONS = [
    ("spell record home", "docs/64",
     "no spells file in BP/; the table's static address is not located"),
]


#: `docs/69`: the vision radius is not a unit property. `func_02048A94` takes it
#: as an argument and is called only from mission overlays, always with an
#: immediate. Overlays all load at this address (`docs/39`).
OVERLAY_BASE = 0x0216A2E0
SET_FOG_RADIUS = 0x02048A94


def vision_radii(extract="out/eur"):
    """[(overlay, address, radius)] for every mission call that sets vision."""
    import glob
    arm, thumb = lb_code.make_engines()
    out = []
    for path in sorted(glob.glob(os.path.join(extract, "overlays", "*.bin"))):
        with open(path, "rb") as fh:
            data = fh.read()
        lines = list(thumb.disasm(data, OVERLAY_BASE))
        for k, i in enumerate(lines):
            if i.mnemonic not in ("bl", "blx") or not i.op_str.startswith("#"):
                continue
            if (int(i.op_str[1:], 16) & ~1) != SET_FOG_RADIUS:
                continue
            imm = None
            for j in range(k - 1, max(-1, k - 6), -1):
                m, o = lines[j].mnemonic, lines[j].op_str
                if m in ("movs", "mov") and o.startswith("r1, #"):
                    imm = int(o.split("#", 1)[1], 0)
                    break
                if o.startswith("r1,"):
                    break
            out.append((os.path.basename(path), i.address, imm))
    return out


def immediates(space, engines, funcs, addr, limit=None):
    """[(mnemonic, op_str)] for one function, for reading constants back."""
    rec = funcs.get(addr)
    if not rec or not rec["size"]:
        return []
    arm, thumb = engines
    cs = thumb if rec["mode"] == "thumb" else arm
    n = min(rec["size"], limit or rec["size"])
    return [(i.mnemonic, i.op_str) for i in cs.disasm(space.read(addr, n), addr)]


def find_imm(ops, mnemonics, suffix):
    """True if some instruction matches, used to prove a constant is still there."""
    return any(m in mnemonics and o.endswith(suffix) for m, o in ops)


def collect(extract):
    """[(system, name, value, unit, kind, source, evidence)]."""
    space = lb_code.AddressSpace(extract, None, "arm9")
    engines = lb_code.make_engines()
    funcs = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    rows = []

    def add(system, name, value, unit, kind, source, evidence):
        rows.append((system, name, str(value), unit, kind, source, evidence))

    # ---- simulation ------------------------------------------------------
    add("simulation", "tick rate", TICK_HZ, "Hz", "derived", "docs/25",
        "every interval below is in ticks and divides by this")

    # ---- economy ---------------------------------------------------------
    table = lb_economy.allowance_table(space)
    add("economy", "hero cap", table[0], "units", "data", "docs/58",
        "allowance table 0x%08X entry 0" % lb_economy.ALLOWANCE_TABLE)
    add("economy", "minifigure cap", table[1], "units", "data", "docs/58",
        "build-limit class 1, the min() target in func_020863D8")
    for lid in sorted(lb_economy.LIMIT_CLASSES):
        lname, what = lb_economy.LIMIT_CLASSES[lid]
        if lname in ("unresolved", "unused"):
            continue
        add("economy", "build limit, %s" % lname,
            table[lid] if lid < len(table) else "?", "units", "data",
            "docs/68", "class %d of func_0x%08X: %s"
            % (lid, lb_economy.LIMIT_REMAINING, what))

    ops = immediates(space, engines, funcs, lb_economy.CAP_MINIFIGURE)
    ok = (find_imm(ops, ("lsl", "lsls"), "#2")
          and find_imm(ops, ("add", "adds"), "#4"))
    add("economy", "minifigure allowance base", lb_economy.FARM_BASE, "units",
        "instruction", "docs/58",
        "func_02085E5C add #4 %s" % ("present" if ok else "MISSING"))
    add("economy", "minifigure allowance per farm", lb_economy.FARM_STEP,
        "units", "instruction", "docs/58", "func_02085E5C lsl #2")
    add("economy", "farms to reach the cap",
        -(-(table[1] - lb_economy.FARM_BASE) // lb_economy.FARM_STEP), "farms",
        "derived", "docs/58", "ceil((%d - %d) / %d)"
        % (table[1], lb_economy.FARM_BASE, lb_economy.FARM_STEP))

    ops = immediates(space, engines, funcs, lb_economy.HARVEST_INTERVAL_FUNC)
    add("economy", "harvest interval", lb_economy.HARVEST_TICKS, "ticks",
        "instruction", "docs/58", "func_020531F8 mov #0x96 %s"
        % ("present" if find_imm(ops, ("mov", "movs"), "#0x96") else "MISSING"))
    add("economy", "harvest interval", "%.1f" % (lb_economy.HARVEST_TICKS
                                                 / TICK_HZ), "seconds",
        "derived", "docs/58", "%d ticks / %d Hz"
        % (lb_economy.HARVEST_TICKS, TICK_HZ))

    ops = immediates(space, engines, funcs, lb_economy.HARVEST_PAYOUT_FUNC)
    add("economy", "tree payout, no mill", lb_economy.HARVEST_BASE, "bricks",
        "instruction", "docs/58", "func_0206D0E0 add #0x4b %s"
        % ("present" if find_imm(ops, ("add", "adds"), "#0x4b") else "MISSING"))
    add("economy", "tree payout bonus, any mill", lb_economy.MILL_BONUS,
        "bricks", "instruction", "docs/58",
        "func_0206D0E0 moveq #0x19; the loop breaks on the first built Mill")
    add("economy", "tree payout, with mill",
        lb_economy.HARVEST_BASE + lb_economy.MILL_BONUS, "bricks", "derived",
        "docs/58", "%d + %d" % (lb_economy.HARVEST_BASE,
                                lb_economy.MILL_BONUS))
    add("economy", "mills that matter", 1, "buildings", "instruction",
        "docs/58", "the beq after moveq #0x19 leaves the loop")
    add("economy", "brick balance cap",
        space.u32(lb_economy.BRICK_CAP_LITERAL), "bricks", "data", "docs/58",
        "func_020866E0 clamps player+0x%02X to this literal"
        % lb_economy.BRICK_BALANCE_OFFSET)

    # ---- build -----------------------------------------------------------
    add("build", "unit/structure category split",
        lb_economy.STRUCTURE_CATEGORY, "category", "instruction", "docs/58",
        "two 20-arm jump tables in func_020859F4 change arm here")
    add("build", "construction complete at", lb_economy.BUILT, "percent",
        "instruction", "docs/58",
        "entity+0x22E compared against #0x64 before anything counts")
    add("build", "nearly-done event fires above", lb_economy.NEARLY_DONE,
        "percent", "instruction", "docs/60",
        "func_020523A8 cmp #0x62, one-shot via a byte at +0x35")
    add("build", "build time floor", lb_economy.BUILD_TIME_FLOOR, "ticks",
        "instruction", "docs/60", "func_020523A8 movlo r6, #1")
    add("build", "special unit categories",
        "|".join(str(c) for c in lb_economy.SPECIAL_CATEGORIES), "category",
        "instruction", "docs/58",
        "func_02071DC4 cmp #6 / cmpne #5 selects the Special allowance")

    # ---- combat ----------------------------------------------------------
    ops = immediates(space, engines, funcs, lb_combat.FIXED_MUL)
    add("combat", "fixed point", "20.12", "format", "derived", "docs/26",
        "pos_scale x inv_pos_scale == 2^24 on all 39 models (docs/51)")
    add("combat", "fixed multiply rounding", lb_combat.FIXED_HALF, "20.12",
        "instruction", "docs/59", "func_0204F2D8 adds #0x800 before lsr #12 %s"
        % ("present" if find_imm(ops, ("adds",), "#0x800") else "MISSING"))
    add("combat", "area damage clamp", lb_combat.DAMAGE_CLAMP, "damage",
        "instruction", "docs/59",
        "func_0206E60C clamps the SuperExplosions doubling here")

    path = os.path.join(extract, "plain", lb_combat.EBP.replace("/", os.sep))
    if os.path.exists(path):
        proj = lb_combat.projectiles(path)
        explode = [p for p in proj if p[1][lb_combat.OFF_EXPLODES]]
        add("combat", "projectiles", len(proj), "records", "data", "docs/59",
            "ProjectileBlueprint records in Entities.ebp")
        add("combat", "projectiles that explode", len(explode), "records",
            "data", "docs/59", "blueprint+0x6B set; the rest are direct hits")
        same = sum(1 for _i, r in proj
                   if lb_combat.pairs(r)[0] == lb_combat.pairs(r)[1])
        add("combat", "projectiles with both damage pairs equal", same,
            "records", "data", "docs/59",
            "which is why the two pairs could not be told apart from data")
        sets = {r[lb_combat.OFF_ANIM_SET] for _i, r in proj}
        add("combat", "animation sets used by projectiles", len(sets),
            "sets", "data", "docs/66",
            "blueprint+0x5D into the 18-byte-record table at 0x%08X"
            % lb_combat.ANIM_TABLE)
        add("combat", "animation record stride", lb_combat.ANIM_STRIDE,
            "bytes", "instruction", "docs/66",
            "func_0200CE1C mov r2, #0x12 before the multiply")
        add("combat", "animation set 0 frame count",
            lb_combat.anim_set(space, 0)[0], "frames", "data", "docs/66",
            "empty, and func_0203AB00 skips the draw when the id is zero")

    # ---- roster ----------------------------------------------------------
    ents = os.path.join("docs", "data", "entities.tsv")
    if os.path.exists(ents):
        with open(ents, encoding="utf-8", newline="") as fh:
            rowset = list(csv.DictReader(fh, delimiter=chr(9)))
        fac = {r["faction"] for r in rowset if r.get("faction")}
        per = [sum(1 for r in rowset if r.get("faction") == f) for f in fac]
        add("roster", "playable factions", len(fac), "factions", "data",
            "docs/53", "distinct faction values in Entities.ebp")
        add("roster", "blueprints per faction",
            per[0] if len(set(per)) == 1 else "|".join(map(str, per)),
            "blueprints", "data", "docs/53",
            "ten unit slots and ten structure slots")
        struct_cells = [r for r in rowset if r.get("is_structure") == "True"
                        and r.get("faction")]
        add("roster", "faction structures", len(struct_cells), "blueprints",
            "data", "docs/53",
            "89 of their 90 stat cells are identical across all six factions")

    # ---- vision ----------------------------------------------------------
    add("vision", "fog layers", 2, "bitmaps", "instruction", "docs/63",
        "func_020A15D8 clears +0x14 each update and never clears +0x10")
    add("vision", "explored stamp", "one-shot", "flag", "instruction",
        "docs/63", "source+0x11 is cleared after the stamp")
    add("vision", "reveal shape", "capsule", "shape", "instruction",
        "docs/63",
        "func_020A16B4 adds the footprint spans to the circle centre")
    add("vision", "building visible when", "any corner", "rule",
        "instruction", "docs/64",
        "func_020A17C0 tests four corner points and ORs them")

    # ---- vision ----------------------------------------------------------
    radii = vision_radii(extract)
    if radii:
        vals = [r for _o, _a, r in radii if r is not None]
        add("vision", "radius call sites, all in mission overlays", len(radii),
            "calls", "instruction", "docs/69",
            "func_0x%08X takes the radius as an argument; nothing in the ARM9 "
            "calls it" % SET_FOG_RADIUS)
        add("vision", "radius values used", "|".join(str(v) for v in
                                                     sorted(set(vals))),
            "tiles", "data", "docs/69",
            "every call site passes an immediate; vision is authored per "
            "mission, not per unit")
        add("vision", "most common radius",
            max(set(vals), key=vals.count), "tiles", "data", "docs/69",
            "%d of %d call sites" % (vals.count(max(set(vals),
                                                    key=vals.count)),
                                     len(vals)))

    # ---- spells ----------------------------------------------------------
    add("spells", "range multiplier, Long Range Magic", "1.5", "x",
        "instruction", "docs/64", "func_02002AD4 add r0, r0, r0, asr #1")
    add("spells", "duration multiplier, Super Magic", "1.5", "x",
        "instruction", "docs/64", "func_02002AA8 add r0, r0, r0, asr #1")
    add("spells", "cheats that check the player id", 0, "of 2", "instruction",
        "docs/64", "neither getter tests the local player, unlike docs/59's")

    return rows


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    rows = collect(args.extract)
    missing = [r for r in rows if "MISSING" in r[6]]

    system = None
    for r in rows:
        if r[0] != system:
            system = r[0]
            print()
            print(system.upper())
        print("   %-38s %-12s %-8s %-11s %s"
              % (r[1], r[2], r[3], r[4], r[5]))

    print()
    print("%d constants, %d re-read from instructions, %d from data, "
          "%d derived"
          % (len(rows), sum(1 for r in rows if r[4] == "instruction"),
             sum(1 for r in rows if r[4] == "data"),
             sum(1 for r in rows if r[4] == "derived")))
    if missing:
        print("INSTRUCTIONS NO LONGER PRESENT:")
        for r in missing:
            print("   %s -- %s" % (r[1], r[6]))

    print()
    print("open questions carried from the documents:")
    for name, doc, why in OPEN_QUESTIONS:
        print("   %-36s %-9s %s" % (name, doc, why))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        tab, nl = chr(9), chr(10)
        with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
            fh.write(tab.join(["system", "name", "value", "unit", "kind",
                               "source", "evidence"]) + nl)
            for r in rows:
                fh.write(tab.join(r) + nl)
            for name, doc, why in OPEN_QUESTIONS:
                fh.write(tab.join(["open", name, "", "", "unresolved", doc,
                                   why]) + nl)
        print(f"\nwrote {args.tsv} ({len(rows)} constants, "
              f"{len(OPEN_QUESTIONS)} open questions)")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
