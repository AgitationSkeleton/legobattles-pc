#!/usr/bin/env python3
"""Scan all 91 overlays, one address space at a time, and recover the mission API.

## Why the overlays needed their own tool

`docs/06` established that the overlays are the campaign missions and that
**all 91 load at the same address, `0x0216A2E0`**. That single fact is why they
were excluded from the corpus (`docs/09`) and from the coverage figures
(`docs/30`): an address in overlay space is meaningless without saying *which*
overlay, so mapping two at once would silently answer a question about one with
bytes from another. `lb_code.AddressSpace` refuses to map them by default for
exactly this reason.

The fix is not clever, just disciplined: build **91 separate address spaces**,
each holding the ARM9 static sections plus exactly one overlay, and scan each
independently.

## Seeds

Every overlay's table entry carries a `static_init` range — a pointer array of
static constructors, run when the overlay loads. All 91 have a non-empty one.
That is a *given* entry point set rather than a guessed one, which matters: a
recursive-descent scan is only as trustworthy as its seeds, and a wrong seed
produces confident nonsense.

From there the walk is `lb_scan`'s: follow `BL`/`BLX` with the mode implied by
the instruction, stop at returns, and collect literal-pool words as tier-2
candidates. Two rules keep the result honest:

  * a call that leaves the overlay's address range is **recorded but not
    reported** as an overlay function -- it belongs to the ARM9 and is already
    in that corpus;
  * a tier-2 literal is only followed if it lands inside this overlay.

## The mission API

The set of ARM9 addresses that overlays call out to is more interesting than
the overlays themselves. It is the interface the campaign scripting is written
against -- every service a mission can ask the engine for. Ranking those targets
by how many distinct overlays call them separates the core mission API from
one-off helpers, and this tool reports that ranking.

Usage:
    python tools/lb_overlays.py
    python tools/lb_overlays.py --tsv docs/data/overlay_corpus.tsv --api-tsv docs/data/mission_api.tsv
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
import lb_scan  # noqa: E402
import capstone  # noqa: E402

OVERLAY_BASE = 0x0216A2E0

#: The 13 objective factories at 0x02047308..0x02047514. Each allocates its
#: object and calls one constructor, and that constructor's vtable literal
#: resolves through the RTTI to the class named here -- so this table is
#: *recovered from the ROM*, not assumed. `Battles::ObjectiveBase` has exactly
#: 13 concrete subclasses and every one appears here, which is what makes the
#: set closed. See docs/32.
OBJECTIVE_FACTORIES = {
    0x02047308: "UserUIPenObjective",
    0x0204732C: "DestroyMarkedUnitsObjective",
    0x02047358: "RepairMarkedUnitsObjective",
    0x02047384: "ProtectMarkedUnitsObjective",
    0x020473B0: "TimerObjective",
    0x020473D8: "DestroyUnitsObjective",
    0x0204740C: "CreateUnitsObjective",
    0x02047444: "CollectSomeItemsObjective",
    0x0204746C: "CollectMarkedItemsObjective",
    0x02047494: "CollectMarkedUnitsObjective",
    0x020474C0: "RetrieveMarkedUnitsObjective",
    0x020474EC: "TriggerAreaObjective",
    0x02047514: "ManualCompletionObjective",
}


def walk_seen(space, arm, thumb, addr, mode, limit=8192):
    """Like `lb_scan.walk`, but returns the set of addresses actually VISITED.

    This exists because of a mistake worth keeping. `lb_scan.walk` returns a
    `high` water mark -- the furthest address it touched -- and the first
    version of this tool treated `high - addr` as the function's covered
    bytes. **It is not.** A single branch near the end of a function raises
    `high` to the end while the walk skips everything in between, so the span
    can be reported as fully covered when most of it was never visited.

    On overlay 8 the walk reported a 0x2AC-byte extent and found 14 call
    targets; a linear read of the same span found 19, including four objective
    factories. The extent was right and the coverage claim was wrong.

    Returns (visited_addresses, calls).
    """
    cs = thumb if mode == "thumb" else arm
    pending = collections.deque([addr])
    seen = set()
    calls = []
    while pending:
        pc = pending.popleft()
        while True:
            if pc in seen or pc - addr > limit:
                break
            data = space.read(pc, 4)
            if not data or len(data) < (2 if mode == "thumb" else 4):
                break
            ins = list(cs.disasm(data, pc, count=1))
            if not ins:
                break
            i = ins[0]
            for off in range(i.size):
                seen.add(pc + off)
            m = i.mnemonic.split(".")[0]
            if m in ("bl", "blx") and i.operands and \
                    i.operands[0].type == capstone.arm.ARM_OP_IMM:
                calls.append(i.operands[0].imm & ~1)
            elif m.startswith("b") and m not in ("bl", "blx", "bx", "bic") \
                    and i.operands and \
                    i.operands[0].type == capstone.arm.ARM_OP_IMM:
                t = i.operands[0].imm
                if addr <= t < addr + limit:
                    pending.append(t)
                if m == "b":
                    break
            if lb_scan.is_return(i, mode == "thumb"):
                break
            pc += i.size
    return seen, calls


def sweep_calls(space, engines, lo, hi, mode, funcs9):
    """Count call sites by reading a span linearly, filtered to real targets.

    Control-flow walking under-reads a compiled mission: much of it is
    straight-line setup entered from branches the walk does not take. A linear
    sweep reads everything, at the risk of decoding a literal pool as code --
    so a `bl` only counts if its target is **a function already in the ARM9
    corpus**. Random data decoding to a branch that lands exactly on a known
    function entry is not a realistic accident.
    """
    arm, thumb = engines
    cs = thumb if mode == "thumb" else arm
    data = space.read(lo, hi - lo)
    out = collections.Counter()
    if not data:
        return out
    for ins in cs.disasm(data, lo):
        m = ins.mnemonic.split(".")[0]
        if m in ("bl", "blx") and ins.operands and \
                ins.operands[0].type == capstone.arm.ARM_OP_IMM:
            t = ins.operands[0].imm & ~1
            if t in funcs9:
                out[t] += 1
    return out


def scan_overlay(extract, ov, engines, funcs9):
    """Return (functions, calls_out) for one overlay.

    functions: {addr: (mode, size)} for addresses inside the overlay
    calls_out: {arm9_addr} the overlay calls
    """
    arm, thumb = engines
    space = lb_code.AddressSpace(extract, ov["id"], "arm9")
    lo = ov["ram_address"]
    hi = lo + ov["ram_size"]

    # Seeds: the static constructor pointer array. Its low bit is the mode,
    # exactly as for a vtable slot.
    seeds = []
    n = (ov["static_init_end"] - ov["static_init_start"]) // 4
    for i in range(n):
        w = space.u32(ov["static_init_start"] + i * 4)
        if w is None or w == 0:
            continue
        seeds.append((w & ~1, "thumb" if w & 1 else "arm"))

    found = {}
    calls_out = set()
    visited = set()
    pending = collections.deque(seeds)
    tried = set()

    while pending:
        addr, mode = pending.popleft()
        if (addr, mode) in tried:
            continue
        tried.add((addr, mode))
        if not (lo <= addr < hi):
            # An ARM9 target. Record the edge, do not walk it: it is already
            # in the ARM9 corpus and walking it would drag half the engine in.
            if addr in funcs9 or 0x02000000 <= addr < 0x02160000:
                calls_out.add(addr)
            continue
        try:
            end, calls, lits, bad = lb_scan.walk(space, arm, thumb, addr, mode)
        except Exception:
            continue
        if addr in found and found[addr][1] >= end - addr:
            continue
        found[addr] = (mode, max(0, min(end, hi) - addr))
        seen, _ = walk_seen(space, arm, thumb, addr, mode)
        visited.update(a for a in seen if lo <= a < hi)
        for t, tmode in calls:
            pending.append((t, tmode))
        for w in lits:
            # tier 2: a literal that points into THIS overlay
            if w and lo <= (w & ~1) < hi:
                pending.append((w & ~1, "thumb" if w & 1 else "arm"))

    # Call counting is done by a linear sweep over the MERGED function extents,
    # not from the walk. Merging first means overlapping entry points are swept
    # once, so nothing is double-counted; sweeping means the straight-line
    # setup code the walk never branches into is still read.
    spans = sorted((a, a + s) for a, (m, s) in found.items() if s)
    merged = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    # Sweep the WHOLE overlay range, not just the walk's function extents.
    # Restricting the sweep to extents reintroduced the same class of blind
    # spot the walk had: UserUIPenObjective vanished from the tally because its
    # single call site sits outside any extent the walk established. Every
    # overlay function found is Thumb, so the sweep is Thumb, and the
    # known-target filter is what keeps data out of the count.
    call_counts = sweep_calls(space, engines, lo, hi, "thumb", funcs9)
    # The out-edge set comes from the sweep too. Taking it from the walk
    # under-reports for the same reason the coverage did: SetTeamEnemy showed
    # in 37 of 91 missions from the walk and considerably more from the sweep.
    calls_out |= set(call_counts)
    return found, calls_out, call_counts, visited


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
    ap.add_argument("--api-tsv")
    ap.add_argument("--objectives-tsv")
    ap.add_argument("--coverage-tsv")
    ap.add_argument("--limit", type=int, help="scan only the first N overlays")
    args = ap.parse_args(argv)

    header = json.load(open(os.path.join(args.extract, "header.json"),
                           encoding="utf-8"))
    overlays = header["overlays"]
    if args.limit:
        overlays = overlays[:args.limit]
    funcs9 = lb_code.load_corpus(os.path.join(args.extract, "corpus.tsv"))
    engines = lb_scan.make_engines() if hasattr(lb_scan, "make_engines") \
        else lb_code.make_engines()

    rows = []
    coverage_rows = []
    objectives = {}
    api = collections.Counter()
    api_overlays = collections.defaultdict(set)
    tot_bytes = tot_cov = tot_span = 0

    print(f"{'ov':>3} {'ram_size':>9} {'funcs':>6} {'covered':>8} {'%':>6} {'calls out':>10}")
    for ov in overlays:
        found, calls_out, counts, visited = scan_overlay(
            args.extract, ov, engines, funcs9)
        objectives[ov["id"]] = {OBJECTIVE_FACTORIES[t]: n
                                for t, n in counts.items()
                                if t in OBJECTIVE_FACTORIES}
        # Coverage is VISITED bytes, not the function extent. See walk_seen.
        cov = len(visited)
        span, _ = _merged([(a, a + s) for a, (m, s) in found.items() if s])
        size = ov["ram_size"]
        tot_bytes += size
        tot_cov += cov
        pct = 100.0 * cov / size if size else 0.0
        tot_span += span
        coverage_rows.append((ov["id"], size, cov, span))
        print(f"{ov['id']:>3} {size:>9} {len(found):>6} {cov:>8} {pct:>5.1f}% "
              f"{len(calls_out):>10}")
        for a, (m, s) in sorted(found.items()):
            rows.append({"overlay": ov["id"], "address": "0x%08X" % a,
                         "mode": m, "size": "0x%x" % s})
        for t in calls_out:
            api[t] += 1
            api_overlays[t].add(ov["id"])

    print()
    print(f"TOTAL {tot_cov:,} of {tot_bytes:,} overlay bytes VISITED "
          f"-> {100.0*tot_cov/tot_bytes:.1f}%   ({len(rows):,} functions)")
    print(f"      (function extents span {tot_span:,} bytes = "
          f"{100.0*tot_span/tot_bytes:.1f}% -- span is NOT coverage, see walk_seen)")
    print(f"{len(api):,} distinct ARM9 targets called from overlays")

    print("\nThe mission API -- ARM9 functions called by the most overlays:")
    print(f"{'overlays':>9}  {'address':<12} name")
    for t, n in api.most_common(25):
        rec = funcs9.get(t)
        print(f"{n:>9}  0x{t:08X}  {lb_code.best_name(rec, t) if rec else '(not in corpus)'}")

    print()
    print("Objective types, by how many of the 91 missions use each:")
    use = collections.Counter()
    inst = collections.Counter()
    for objs in objectives.values():
        for name, n in objs.items():
            use[name] += 1
            inst[name] += n
    print(f"{'missions':>9}{'instances':>11}  class")
    for name, n in use.most_common():
        print(f"{n:>9}{inst[name]:>11}  Battles::{name}")
    missing = sorted(set(OBJECTIVE_FACTORIES.values()) - set(use))
    print(f"objective types used by no mission: {missing or 'none'}")

    if args.objectives_tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.objectives_tsv)),
                    exist_ok=True)
        names = sorted(OBJECTIVE_FACTORIES.values())
        tab = chr(9)
        nl = chr(10)
        with open(args.objectives_tsv, "w", encoding="utf-8") as fh:
            fh.write(tab.join(["overlay", "total"] + names) + nl)
            for ovid in sorted(objectives):
                o = objectives[ovid]
                cells = [str(ovid), str(sum(o.values()))] + \
                        [str(o.get(n, 0)) for n in names]
                fh.write(tab.join(cells) + nl)
        print(f"wrote {args.objectives_tsv}")

    if args.coverage_tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.coverage_tsv)),
                    exist_ok=True)
        with open(args.coverage_tsv, "w", encoding="utf-8") as fh:
            fh.write('overlay\tram_size\tvisited\textent_span\n')
            for oid, size, cov, span in coverage_rows:
                fh.write('\t'.join(str(x) for x in
                                   (oid, size, cov, span)) + '\n')
        print(f"wrote {args.coverage_tsv}")

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("overlay\taddress\tmode\tsize\n")
            for r in rows:
                fh.write(f"{r['overlay']}\t{r['address']}\t{r['mode']}\t{r['size']}\n")
        print(f"\nwrote {args.tsv} ({len(rows)} functions)")

    if args.api_tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.api_tsv)), exist_ok=True)
        with open(args.api_tsv, "w", encoding="utf-8") as fh:
            fh.write("address\tname\toverlay_count\toverlays\n")
            for t, n in api.most_common():
                rec = funcs9.get(t)
                name = lb_code.best_name(rec, t) if rec else ""
                ovs = ";".join(str(x) for x in sorted(api_overlays[t]))
                fh.write(f"0x{t:08X}\t{name}\t{n}\t{ovs}\n")
        print(f"wrote {args.api_tsv} ({len(api)} targets)")
    return 0


def _merged(intervals):
    if not intervals:
        return 0, 0
    ivs = sorted(intervals)
    total = sum(e - s for s, e in ivs)
    out = []
    cs, ce = ivs[0]
    for s, e in ivs[1:]:
        if s <= ce:
            ce = max(ce, e)
        else:
            out.append((cs, ce))
            cs, ce = s, e
    out.append((cs, ce))
    cov = sum(e - s for s, e in out)
    return cov, total - cov


if __name__ == "__main__":
    raise SystemExit(main())
