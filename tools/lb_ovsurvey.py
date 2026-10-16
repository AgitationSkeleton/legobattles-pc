#!/usr/bin/env python3
"""Overlay survey: classify every byte of every ARM9 overlay and write
docs/data/overlay_inventory.tsv.

Classification is exhaustive -- the six buckets sum to ram_size for every
overlay -- so "overlay bytes" can be split into code and data honestly.

Differences from tools/lb_overlays.py, which this does not replace:
  * follows the THUMB PC-relative halfword switch table. lb_scan implements
    the ARM idiom only, and every overlay's event handler uses the Thumb one,
    so the walk previously stopped at the dispatch and never entered the
    per-event arms.
  * marks literal pools, switch tables, the static-init array and tail
    padding as DATA rather than counting them in a coverage denominator.
  * takes overlay -> mission from vtable slot 11 (GetOverlayId), which is
    recovered from the ROM for all 90 and needs no third-party table.
"""
import argparse
import collections
import csv
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import capstone  # noqa: E402
import lb_code  # noqa: E402
import lb_scan  # noqa: E402

R = capstone.arm
OPEN_DIALOG = 0x020E3BE4
OBJECTIVE_FACTORIES = {
    0x02047308: "UserUIPenObjective", 0x0204732C: "DestroyMarkedUnitsObjective",
    0x02047358: "RepairMarkedUnitsObjective", 0x02047384: "ProtectMarkedUnitsObjective",
    0x020473B0: "TimerObjective", 0x020473D8: "DestroyUnitsObjective",
    0x0204740C: "CreateUnitsObjective", 0x02047444: "CollectSomeItemsObjective",
    0x0204746C: "CollectMarkedItemsObjective", 0x02047494: "CollectMarkedUnitsObjective",
    0x020474C0: "RetrieveMarkedUnitsObjective", 0x020474EC: "TriggerAreaObjective",
    0x02047514: "ManualCompletionObjective",
}


def ops_of(ins):
    try:
        return ins.operands
    except Exception:
        return []


def walk(space, cs, addr, lo, hi):
    """Thumb recursive descent with switch-table support.

    Returns (visited_bytes, calls, pool_bytes, table_bytes)."""
    pending = collections.deque([addr])
    seen, calls, pool, tables = set(), [], set(), set()
    while pending:
        pc = pending.popleft()
        cmpval, pcadd, tbase = {}, {}, {}
        while True:
            if pc in seen or not (lo <= pc < hi):
                break
            data = space.read(pc, 4)
            if not data or len(data) < 2:
                break
            ins = list(cs.disasm(data, pc, count=1))
            if not ins:
                break
            i = ins[0]
            for k in range(i.size):
                seen.add(pc + k)
            m = i.mnemonic.split(".")[0]
            o = ops_of(i)

            lit = lb_scan.literal_of(i, True)
            if lit is not None and lo <= lit < hi:
                pool.update(range(lit, lit + 4))

            if m == "cmp" and len(o) == 2 and o[1].type == R.ARM_OP_IMM:
                cmpval[o[0].reg] = o[1].imm
            elif m in ("add", "adds") and len(o) == 2 and \
                    o[1].type == R.ARM_OP_REG and o[1].reg == R.ARM_REG_PC:
                pcadd[o[0].reg] = i.address
            elif m == "ldrh" and len(o) == 2 and o[1].type == R.ARM_OP_MEM and \
                    o[1].mem.base in pcadd:
                tbase[o[0].reg] = pcadd[o[1].mem.base] + 4 + o[1].mem.disp
            elif m in ("lsls", "asrs") and len(o) >= 2 and \
                    o[1].type == R.ARM_OP_REG and o[1].reg in tbase:
                tbase[o[0].reg] = tbase[o[1].reg]

            if m in ("bl", "blx") and o and o[0].type == R.ARM_OP_IMM:
                calls.append(o[0].imm & ~1)
            elif m.startswith("b") and m not in ("bl", "blx", "bx", "bic") \
                    and o and o[0].type == R.ARM_OP_IMM:
                t = o[0].imm
                if lo <= t < hi:
                    pending.append(t)
                if m == "b":
                    break
            elif m in ("add", "adds") and o and o[0].type == R.ARM_OP_REG and \
                    o[0].reg == R.ARM_REG_PC:
                # Thumb PC-relative switch: `add pc, rN` indexes a halfword
                # table of signed offsets from (this instruction + 4).
                #
                # The table's LENGTH is not taken from the preceding `cmp`.
                # The index register is usually not the compared one -- ov028
                # is `cmp r4,#5` then `adds r0,r4,r4` -- so a cmp lookup keyed
                # on the index register silently finds nothing and the whole
                # switch goes unwalked. Instead the table is self-limiting:
                # every arm lies after the table, so the table cannot extend
                # past the lowest target seen so far.
                src = o[1].reg if len(o) > 1 else None
                base = tbase.get(src)
                if base is not None:
                    cap = cmpval.get(src)
                    limit = hi
                    k = 0
                    while base + 2 * k < limit and (cap is None or k <= cap):
                        ent = base + 2 * k
                        w = space.read(ent, 2)
                        if not w or len(w) < 2:
                            break
                        t = i.address + 4 + struct.unpack("<h", w)[0]
                        if not (lo <= t < hi) or t <= ent:
                            break
                        limit = min(limit, t)
                        tables.update((ent, ent + 1))
                        pending.append(t)
                        k += 1
                break

            if m == "bx" or lb_scan.is_return(i, True):
                break
            pc += i.size
    return seen, calls, pool, tables


def main(argv=None):
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--missions", default=os.path.join("docs", "data",
                                                       "campaign_missions.tsv"))
    ap.add_argument("--tsv", default=os.path.join("docs", "data",
                                                  "overlay_inventory.tsv"))
    args = ap.parse_args(argv)
    extract = args.extract
    hdr = json.load(open(os.path.join(extract, "header.json"), encoding="utf-8"))
    overlays = hdr["overlays"]
    funcs9 = lb_code.load_corpus(os.path.join(extract, "corpus.tsv"))
    arm, thumb = lb_code.make_engines()

    # ---- overlay -> mission, from vtable slot 11 (GetOverlayId) -----------
    BASE = 0x02000000
    img = open(os.path.join(extract, "arm9",
                            "section_static_02000000.bin"), "rb").read()

    def u32(a):
        return struct.unpack("<I", img[a - BASE:a - BASE + 4])[0] \
            if BASE <= a < BASE + len(img) - 3 else None

    def const_return(a):
        ins = list(thumb.disasm(img[a - BASE:a - BASE + 12], a, count=2))
        if len(ins) < 2 or ins[1].mnemonic.split(".")[0] != "bx":
            return None
        o = ops_of(ins[0])
        m = ins[0].mnemonic.split(".")[0]
        if m == "movs" and len(o) == 2 and o[1].type == R.ARM_OP_IMM:
            return o[1].imm
        if m == "ldr" and len(o) > 1 and o[1].type == R.ARM_OP_MEM and \
                o[1].mem.base == R.ARM_REG_PC:
            return u32(((ins[0].address + 4) & ~3) + o[1].mem.disp)
        return None

    # Every vtable in the RTTI, not just the 90 campaign ones: exactly 91
    # have a slot-11 constant return in 0..90, and the 91st is CampaignBase.
    mission = {}
    for line in open(os.path.join(extract, "rtti_symbols.txt"),
                     encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if not p[0].startswith("vtable_"):
            continue
        va = int(p[1], 16)
        oid = const_return((u32(va + 44) or 0) & ~1)
        if oid is not None and 0 <= oid <= 90:
            mission[oid] = {"class": p[0][7:].replace("__", "::"),
                            "vtable": "0x%08X" % va, "map": "", "world": "",
                            "side": "", "act": "", "mission": "",
                            "corroborated": ""}
    for r in csv.DictReader(open("docs/data/campaign_missions.tsv", newline="",
                                 encoding="utf-8"), delimiter="\t"):
        oid = const_return(u32(int(r["vtable"], 16) + 44) & ~1)
        if oid is not None:
            mission[oid] = r

    # ---- language table ---------------------------------------------------
    lraw = open(os.path.join(extract, "plain", "LOC",
                             "UK_English.lng"), "rb").read()
    first = struct.unpack("<I", lraw[0x0C:0x10])[0]
    nids = (first - 0x0C) // 4
    loffs = struct.unpack("<%dI" % nids, lraw[0x0C:0x0C + 4 * nids])
    lstr = [lraw[o:lraw.find(b"\x00", o)].decode("latin-1") for o in loffs]

    rows, tot = [], collections.Counter()
    api = collections.defaultdict(set)

    for ov in overlays:
        oid, lo, size = ov["id"], ov["ram_address"], ov["ram_size"]
        hi = lo + size
        space = lb_code.AddressSpace(extract, oid, "arm9")
        raw = space.find(lo).data

        seeds = [lo]                      # the +0 entry thunk
        n = (ov["static_init_end"] - ov["static_init_start"]) // 4
        for k in range(n):
            w = space.u32(ov["static_init_start"] + k * 4)
            if w:
                seeds.append(w & ~1)

        visited, pool, tables, calls_out = set(), set(), set(), set()
        entries, pending, tried = set(), collections.deque(seeds), set()
        while pending:
            a = pending.popleft()
            if a in tried:
                continue
            tried.add(a)
            if not (lo <= a < hi):
                if a in funcs9 or 0x02000000 <= a < 0x02160000:
                    calls_out.add(a)
                continue
            s, c, p, t = walk(space, thumb, a, lo, hi)
            if not s:
                continue
            entries.add(a)
            visited |= s
            pool |= p
            tables |= t
            pending.extend(c)
            for q in sorted(p):
                if q % 4 == 0:
                    w = space.u32(q)
                    if w and lo <= (w & ~1) < hi:
                        pending.append(w & ~1)

        # unreached spans that open like a Thumb function and call something real
        for a in range(lo, hi - 1, 2):
            if a in visited or a in pool or a in tables:
                continue
            if not lb_scan.prologue(space, arm, thumb, a, "thumb"):
                continue
            s, c, p, t = walk(space, thumb, a, lo, hi)
            if not (s - visited) or not any(x in funcs9 for x in c):
                continue
            entries.add(a)
            visited |= s
            pool |= p
            tables |= t
            for x in c:
                if not (lo <= x < hi) and (x in funcs9 or 0x02000000 <= x < 0x02160000):
                    calls_out.add(x)

        pool -= visited
        tables -= visited
        sinit = set(range(ov["static_init_start"], ov["static_init_end"])) - visited
        pad = 0
        while pad < size and raw[size - 1 - pad] == 0:
            pad += 1
        padset = set(range(hi - pad, hi)) - visited - pool - tables - sinit
        unknown = [a for a in range(lo, hi)
                   if a not in visited and a not in pool and a not in tables
                   and a not in sinit and a not in padset]
        unk_zero = sum(1 for a in unknown if raw[a - lo] == 0)

        # linear sweep: call census + mission-dialog string ids.
        #
        # A `bl` only counts if its target is already a known ARM9 function,
        # which keeps decoded data out of the tally. The register tracking
        # that recovers OpenMissionDialog's string id has no such filter, so
        # it is confined to bytes the walk classified as CODE -- otherwise a
        # literal pool decoding as `ldr r0,[pc,#..]` seeds r0 with a word that
        # is not a string id at all, and the recovered id is a UI string that
        # happens to share the number.
        sweep = collections.Counter()
        regs, dialog_ids = {}, []
        for ins in thumb.disasm(raw, lo):
            code_here = ins.address in visited
            if not code_here:
                regs = {}
            o = ops_of(ins)
            if not o:
                regs = {}
                continue
            m = ins.mnemonic.split(".")[0]
            # Evaluate this instruction's effect on the tracked registers.
            # `new` is the value it leaves in o[0]; KILL means "it writes a
            # value we cannot follow", and that has to clear the register
            # rather than leave a stale one behind.
            KILL = object()
            new = None
            if any(op.type == R.ARM_OP_MEM and op.mem.base == R.ARM_REG_PC
                   for op in o):
                ta = ((ins.address + 4) & ~3) + \
                    next(op.mem.disp for op in o
                         if op.type == R.ARM_OP_MEM
                         and op.mem.base == R.ARM_REG_PC)
                new = struct.unpack("<I", raw[ta - lo:ta - lo + 4])[0] \
                    if lo <= ta < hi - 3 else KILL
            elif m == "movs" and len(o) == 2 and o[1].type == R.ARM_OP_IMM:
                new = o[1].imm
            elif m in ("lsls", "adds", "subs") and len(o) == 3 and \
                    o[1].type == R.ARM_OP_REG and o[2].type == R.ARM_OP_IMM:
                # A Thumb immediate is 8 bits, so any string id above 255 is
                # built as `movs rN,#imm` then `lsls rN,#k`. Dropping the
                # shift reports the seed -- id 99 "Info" for what is really
                # id 1584 -- and the objective text comes out as an unrelated
                # UI string that happens to own the small number.
                v = regs.get(o[1].reg)
                new = KILL if v is None else {"lsls": v << o[2].imm,
                                              "adds": v + o[2].imm,
                                              "subs": v - o[2].imm}[m]
            elif m in ("lsls", "lsrs", "asrs", "adds", "subs", "adcs", "sbcs",
                       "movs", "mov", "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh",
                       "add", "sub", "muls", "ands", "orrs", "eors", "bics",
                       "mvns", "negs", "rsbs", "pop"):
                new = KILL
            if new is not None and o and o[0].type == R.ARM_OP_REG:
                if new is KILL:
                    regs.pop(o[0].reg, None)
                else:
                    regs[o[0].reg] = new
            if m in ("bl", "blx") and o and o[0].type == R.ARM_OP_IMM:
                t = o[0].imm & ~1
                if t in funcs9:
                    sweep[t] += 1
                if t == OPEN_DIALOG and code_here:
                    v = regs.get(R.ARM_REG_R0)
                    if v is not None and 0 < v < nids:
                        dialog_ids.append(v)
                regs = {}
        calls_out |= set(sweep)
        for t in calls_out:
            api[t].add(oid)

        objs = {OBJECTIVE_FACTORIES[t]: c for t, c in sweep.items()
                if t in OBJECTIVE_FACTORIES}
        nstr = 0
        run = 0
        for b in raw:
            if 0x20 <= b < 0x7F:
                run += 1
            else:
                nstr += (run >= 4 and b == 0)
                run = 0

        mi = mission.get(oid)
        code = len(visited)
        data = len(pool) + len(tables) + len(sinit)
        row = dict(
            file="ov%03d.bin" % oid, overlay=oid,
            ram_size=size, file_bytes=len(raw), bss_size=ov["bss_size"],
            load_address="0x%08X" % lo, load_end="0x%08X" % hi,
            rom_offset="0x%X" % ov["rom_offset"], rom_bytes=ov["rom_size"],
            compressed="yes" if ov["compressed"] else "no",
            code_bytes=code, pool_bytes=len(pool), jumptable_bytes=len(tables),
            static_init_bytes=len(sinit), pad_bytes=len(padset),
            unclassified_bytes=len(unknown), unclassified_zero=unk_zero,
            code_pct=round(100.0 * code / size, 1),
            data_pct=round(100.0 * data / size, 1),
            functions=len(entries),
            arm9_call_sites=sum(sweep.values()),
            distinct_arm9_targets=len(calls_out),
            objective_instances=sum(objs.values()), objective_kinds=len(objs),
            objectives=";".join("%s=%d" % kv for kv in sorted(objs.items())),
            ascii_strings=nstr,
            dialog_string_ids=";".join(str(x) for x in dialog_ids),
            first_objective_text=(lstr[dialog_ids[0]][:70] if dialog_ids else ""),
            map=mi["map"] if mi else "", world=mi["world"] if mi else "",
            side=mi["side"] if mi else "", act=mi["act"] if mi else "",
            mission_no=mi["mission"] if mi else "",
            mission_class=mi["class"] if mi else "",
            arm9_vtable=mi["vtable"] if mi else "",
            map_corroborated=mi["corroborated"] if mi else "",
            evidence=("mission script" if dialog_ids or objs else "no objectives seen"),
        )
        rows.append(row)
        for k in ("ram_size", "rom_bytes", "code_bytes", "pool_bytes",
                  "jumptable_bytes", "static_init_bytes", "pad_bytes",
                  "unclassified_bytes", "unclassified_zero", "functions",
                  "arm9_call_sites", "objective_instances", "ascii_strings"):
            tot[k] += row[k]
        assert code + len(pool) + len(tables) + len(sinit) + len(padset) + \
            len(unknown) == size, oid

    os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
    with open(args.tsv, "w", encoding="utf-8", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]), delimiter="\t",
                           lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    t = tot
    print("overlays: %d   ram bytes: %d   rom bytes: %d (%.1f%% of ram)" %
          (len(rows), t["ram_size"], t["rom_bytes"],
           100.0 * t["rom_bytes"] / t["ram_size"]))
    print("code        %7d  %5.1f%%" % (t["code_bytes"], 100.0 * t["code_bytes"] / t["ram_size"]))
    for k in ("pool_bytes", "jumptable_bytes", "static_init_bytes", "pad_bytes",
              "unclassified_bytes"):
        print("%-12s%7d  %5.1f%%" % (k, t[k], 100.0 * t[k] / t["ram_size"]))
    data = t["pool_bytes"] + t["jumptable_bytes"] + t["static_init_bytes"]
    print("=> code %d  data %d  padding %d  unclassified %d (%d of it zero)" %
          (t["code_bytes"], data, t["pad_bytes"], t["unclassified_bytes"],
           t["unclassified_zero"]))
    print("functions %d  arm9 call sites %d  distinct arm9 targets %d" %
          (t["functions"], t["arm9_call_sites"], len(api)))
    print("objective instances %d  ascii strings %d" %
          (t["objective_instances"], t["ascii_strings"]))
    print("overlays with no mission class:",
          [r["overlay"] for r in rows if not r["mission_class"]])
    print("overlays with no .map (not a campaign mission):",
          [r["overlay"] for r in rows if not r["map"]])
    print("overlays with zero objectives:", [r["overlay"] for r in rows if not r["objective_instances"]])
    worst = sorted(rows, key=lambda r: r["code_pct"])[:6]
    print("lowest code%%:", [(r["overlay"], r["code_pct"], r["unclassified_bytes"]) for r in worst])
    print("wrote %s (%d rows)" % (args.tsv, len(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
