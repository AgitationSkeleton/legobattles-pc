#!/usr/bin/env python3
"""Recursive-descent function finder, for images with no corpus.

The ARM9 arrives with 9,312 functions and their ARM/Thumb modes from a
third-party `ds-decomp` configuration. **The ARM7 arrives with nothing**, and it
is where the sound system, touch, SPI and Wi-Fi live (`docs/14`).

This walks the code instead of guessing at it. From a seed, it follows the
function's own branches to find its extent, records every `BL`/`BLX` target as a
new seed, and stops at returns. Nothing is inferred from byte patterns, so a
function is either reached by a real call or it is not reported.

**Instruction-set mode is tracked, not guessed.** It is exactly determined by
the call that reached the function: in ARM, `BL` stays ARM and `BLX` switches to
Thumb; in Thumb, `BL` stays Thumb and `BLX` switches to ARM. The LSW2 playbook
measured a prologue-matching heuristic at four errors in 5,860 — every one a
Thumb function lifted as ARM, which is not subtly wrong but entirely wrong — so
this tool refuses to guess and simply does not report what it cannot reach.

Two tiers are reported separately, because they are not equally trustworthy:

  tier 1  reached by a direct BL/BLX. Certain, mode certain.
  tier 2  a word in a literal pool that points into executable memory. These
          are function pointers — vtables, callbacks, jump tables — and the
          low bit gives the mode. Very likely code, but not proven to be a
          function *entry*, so they are listed apart and are not seeds.

Usage:
    python tools/lb_scan.py --cpu arm7 --seed 0x02380000 --seed 0x037F8000:arm
    python tools/lb_scan.py --cpu arm7 --auto [--tsv out/eur/arm7_corpus.tsv]
"""

import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lb_code import AddressSpace, make_engines          # noqa: E402
import capstone                                          # noqa: E402

MAX_FUNCTION = 0x2000          # a single function longer than this is a bug
RETURNS = ("bx", "pop", "ldm", "ldmia")


def is_return(insn, thumb):
    m = insn.mnemonic.split(".")[0]
    if m == "bx" and "lr" in insn.op_str:
        return True
    if m.startswith("pop") and "pc" in insn.op_str:
        return True
    if m.startswith(("ldm", "ldmia", "ldmfd")) and "pc" in insn.op_str:
        return True
    if m == "mov" and insn.op_str.replace(" ", "").startswith("pc,lr"):
        return True
    return False


def walk(space, arm, thumb, addr, mode):
    """Walk one function. Returns (end_address, calls, pointers, bad)."""
    cs = thumb if mode == "thumb" else arm
    step = 2 if mode == "thumb" else 4
    pending = collections.deque([addr])
    seen = set()
    calls = []
    lits = []
    high = addr
    bad = 0

    while pending:
        pc = pending.popleft()
        # Literal-pool loads tracked per basic block only. A value loaded in a
        # predecessor is deliberately forgotten: that can only lose a seed,
        # never invent one, which is the right way round for this tool.
        litreg = {}
        # Likewise the last `cmp <reg>, #imm`, which bounds a PC-relative
        # switch table below.
        cmpreg = {}
        while True:
            if pc in seen:
                break
            if pc - addr > MAX_FUNCTION:
                bad += 1
                break
            data = space.read(pc, 4 if mode == "arm" else 4)
            if not data or len(data) < step:
                break
            ins = list(cs.disasm(data, pc, count=1))
            if not ins:
                bad += 1
                break
            i = ins[0]
            seen.add(pc)
            high = max(high, pc + i.size)
            m = i.mnemonic.split(".")[0]

            if m in ("bl", "blx") and i.operands and \
                    i.operands[0].type == capstone.arm.ARM_OP_IMM:
                target = i.operands[0].imm
                # BL keeps the mode; BLX (immediate) switches it.
                tmode = mode if m == "bl" else ("arm" if mode == "thumb"
                                                else "thumb")
                calls.append((target & ~1, tmode))
            elif m in LOADS:
                t = literal_of(i, mode == "thumb")
                if t is not None:
                    w = space.u32(t)
                    if w is not None:
                        lits.append(w)
                        # Remember which register the literal landed in, so a
                        # later `bx <reg>` can be resolved exactly. This is
                        # reading the same word the CPU reads, not inferring
                        # from a byte pattern.
                        if m == "ldr":
                            litreg[i.op_str.split(",")[0].strip()] = w
                elif m == "ldr":
                    # A non-literal load clobbers whatever we knew.
                    litreg.pop(i.op_str.split(",")[0].strip(), None)
            elif m.startswith("b") and m not in ("bl", "blx", "bx", "bic") \
                    and i.operands and \
                    i.operands[0].type == capstone.arm.ARM_OP_IMM:
                t = i.operands[0].imm
                # Only follow branches that stay inside a plausible extent —
                # a long branch is a tail call and belongs to another function.
                if addr <= t < addr + MAX_FUNCTION:
                    pending.append(t)
                if len(m) == 1 or m == "b":     # unconditional: block ends
                    break

            # An ARM PC-relative switch: `cmp rN,#K` / `addls pc, pc, rN, lsl
            # #2` followed by a run of `b` instructions, one per case. The
            # walk cannot see those arms -- `add` is not a branch mnemonic, so
            # it reads the DEFAULT branch that follows, ends the block there,
            # and never decodes the table. The extent still covers the table,
            # because `high` is a high-water mark, so this is invisible: the
            # function looks fully walked while every `bl` in its arms went
            # unrecorded. docs/304 measured that as most of the ARM7's
            # remaining interpreted work.
            #
            # The idiom is exact, not a pattern guess. `add pc, pc, rN, lsl #2`
            # lands at (this instruction + 8) + rN*4, so the arms start at the
            # instruction AFTER the default branch, and the preceding `cmp`
            # gives the case count. Enqueueing the arm addresses is enough --
            # each is a `b` the walk already knows how to follow.
            if (m.startswith("add") and mode == "arm"
                    and i.op_str.replace(" ", "").startswith("pc,pc,")
                    and "lsl#2" in i.op_str.replace(" ", "")):
                body = i.op_str.replace(" ", "").split(",")
                idx = body[2] if len(body) > 2 else ""
                arms = cmpreg.get(idx)
                first = i.address + 8
                k = 0
                while arms is None or k <= arms:
                    at = first + k * 4
                    if not (addr <= at < addr + MAX_FUNCTION):
                        break
                    word = space.read(at, 4)
                    if not word:
                        break
                    ai = list(cs.disasm(word, at, count=1))
                    # With no `cmp` bound, stop at the first slot that is not
                    # an unconditional branch -- that is the end of the table.
                    if not ai or ai[0].mnemonic.split(".")[0] != "b":
                        if arms is None:
                            break
                        k += 1
                        continue
                    pending.append(at)
                    k += 1
                    if arms is None and k > 64:
                        bad += 1
                        break

            if m == "cmp" and mode == "arm" and "#" in i.op_str:
                lhs, _, rhs = i.op_str.partition(",")
                try:
                    cmpreg[lhs.strip()] = int(rhs.strip().lstrip("#"), 0)
                except ValueError:
                    cmpreg.pop(lhs.strip(), None)

            # An unconditional `bx <reg>` NEVER falls through, and treating it
            # as if it did is what hid the ARM7's interrupt trampolines: each
            # is `ldr ip,[pc,#4]; mov r0,#N; bx ip; .word <target>`, and the
            # walk ran straight through the literal into the next trampoline,
            # swallowing the whole table into one bogus function (docs/303).
            # `is_return` already catches `bx lr`; this is every other case.
            # A CONDITIONAL bx (`bxeq`) does fall through and is left alone.
            if m == "bx":
                reg = i.op_str.strip()
                w = litreg.get(reg)
                if w is not None:
                    # The target is known exactly, from the literal pool. It
                    # is a tail call, so record it as a seed like a BL would.
                    calls.append((w & ~1, "thumb" if w & 1 else "arm"))
                break

            if is_return(i, mode == "thumb"):
                break
            pc += i.size
    return high, calls, lits, bad


LOADS = ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh")


def literal_of(insn, thumb):
    for op in insn.operands:
        if op.type == capstone.arm.ARM_OP_MEM and \
                op.mem.base == capstone.arm.ARM_REG_PC:
            pc = insn.address + (4 if thumb else 8)
            return (pc & ~3) + op.mem.disp
    return None


PROLOGUES_ARM = (0xE92D0000, 0xE52D0000)     # stmfd sp!/push, str lr,[sp,#-4]!


def prologue(space, arm, thumb, addr, mode):
    """Does this address open like a function?

    ARM: a push (`stmfd sp!, {...}`) or a `sub sp` or a `mov ip, sp`.
    Thumb: a `push`.
    Deliberately narrow - a candidate that does not obviously start a function
    is dropped rather than walked, because a wrong entry point produces a wrong
    extent and a wrong extent poisons the coverage figure.
    """
    data = space.read(addr, 4)
    if not data or len(data) < 4:
        return False
    cs = thumb if mode == "thumb" else arm
    ins = list(cs.disasm(data, addr, count=1))
    if not ins:
        return False
    m = ins[0].mnemonic.split(".")[0]
    if m in ("push", "stmdb", "stmfd"):
        return True
    if mode == "arm" and m in ("sub", "mov") and "sp" in ins[0].op_str:
        return True
    return False


def executable(space, addr):
    p = space.find(addr)
    return p is not None and p.name in EXEC_SECTIONS


# "mainram" is the ARM7's autoload-1 block at 0x027E0000, 96,376 bytes. It was
# excluded here, which is why every scan reported the ARM7 as ~63 KB of
# executable bytes and found nothing in it -- the seeds fell outside the set and
# were silently dropped.
#
# It IS executed, and that is established rather than assumed: docs/83 found 40
# calls into it that survive a control-flow walk, and walking 0x027E0000
# directly yields a 396-byte function with 14 calls whose first instruction is
# an ordinary ARM prologue. docs/91 traced the boot stall to the ARM9 waiting on
# an IPCSYNC handshake the ARM7 never answers, which is exactly what an unlifted
# ARM7 would do.
EXEC_SECTIONS = {"static", "wram", "itcm", "mainram"}


def data_regions(extract, cpu):
    """Address ranges the header declares as data, which are never functions.

    The ARM7's MODULE_PARAMS block at `module_params_address` is the case that
    forced this. Two literal-pool words in the boot code point at it, so the
    tier-2 code-pointer scan offered it as a function entry, and its first word
    happens to decode as a conditional ARM instruction -- so it passed the
    prologue check and was reported as a 0x18-byte function. That is 24 bytes
    of pure data counted as code, and it inflated the ARM7 static section's
    coverage until `tools/lb_verify.py` cross-checked the block against
    header.json and found its words are the autoload fields exactly.

    A pointer into a region is not evidence the region is code. Where the
    header says otherwise, the header wins.
    """
    out = []
    path = os.path.join(extract, "header.json")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as fh:
        header = json.load(fh)
    cpu_hdr = header.get(cpu) or {}
    mp = cpu_hdr.get("module_params") or {}
    addr = mp.get("module_params_address")
    if addr:
        static = next((s for s in cpu_hdr.get("sections", [])
                       if s["name"] == "static"), None)
        end = (static["ram_address"] + static["size"]) if static else addr + 0x24
        if end > addr:
            out.append((addr, end))
    return out


def in_data(regions, addr):
    return any(lo <= addr < hi for lo, hi in regions)


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=os.path.join("out", "eur"))
    ap.add_argument("--cpu", choices=["arm9", "arm7"], default="arm7")
    ap.add_argument("--seed", action="append", default=[],
                    help="ADDR or ADDR:arm|thumb; repeatable")
    ap.add_argument("--auto", action="store_true",
                    help="use this CPU's known entry points")
    ap.add_argument("--scan-data", action="store_true",
                    help="tier 3: seed from every 4-aligned word ANYWHERE in "
                         "the image that points into executable memory and "
                         "lands on a plausible function prologue")
    ap.add_argument("--expand", action="store_true",
                    help="feed literal-pool code pointers back as seeds, "
                         "repeatedly, until nothing new is found")
    ap.add_argument("--tsv")
    ap.add_argument("--seed-from-coverage", action="append", default=[],
                    metavar="TIER3_COVERAGE.TXT",
                    help="seed from a run's tier3_coverage.txt: every ROOT pc "
                         "the interpreter was entered at on this CPU. This is "
                         "OBSERVATION, not a heuristic -- the cartridge's own "
                         "code ran at that address -- and it reaches what no "
                         "static rule can: leaf hardware drivers held only in "
                         "function-pointer tables, which open with `ldr "
                         "rN,[pc,#x]` and so are rejected by the prologue "
                         "check. A ROOT pc is not guaranteed to be a function "
                         "ENTRY (docs/302), so what this finds still has to "
                         "earn its place like any other candidate.")
    ap.add_argument("--merge",
                    help="an existing corpus TSV to start from. Every row it "
                         "has is kept EXACTLY as written -- address, mode, "
                         "size, tier, section -- and this scan only adds rows "
                         "for addresses it does not already carry. Use this "
                         "when widening coverage must not be able to regress "
                         "what already lifts: a full regenerate also shrinks "
                         "and drops entries, and that needs its own before/"
                         "after measurement (docs/303).")
    args = ap.parse_args()

    space = AddressSpace(args.extract, cpu=args.cpu)
    arm, thumb = make_engines()

    seeds = []
    for s in args.seed:
        a, _, m = s.partition(":")
        seeds.append((int(a, 0), m or "arm"))
    if args.auto and args.cpu == "arm9":
        # The cartridge header's ARM9 entry, and the `main` the third-party
        # config names just past it. Both ARM.
        seeds += [(0x02000800, "arm"), (0x02000C04, "arm")]
    if args.auto and args.cpu == "arm7":
        # From docs/14: the header entry, the block the boot stub branches to,
        # and the IRQ handler it installs. All three are ARM (bit 0 clear).
        seeds += [(0x02380000, "arm"), (0x037F8000, "arm"), (0x037FB488, "arm")]
        # ...and the ARM7 IRQ CALLBACK TABLE, which no call graph can reach.
        #
        # Its handlers are entered by the interrupt dispatcher indexing this
        # table, so nothing BLs them and the recursive descent above never
        # sees them. docs/303 found them the only way that is not a guess:
        # every word in the image that points at the trampolines lands in
        # 0x0380739C..0x038073FC, twenty-five consecutive slots, all of which
        # point into ARM7 code. The runtime agrees -- those handlers were
        # 62.6% of every ARM7 native-to-interpreter boundary in a real match.
        #
        # The targets are READ from the table, not listed here, so a different
        # build's table is followed rather than this one's contents assumed.
        for slot in range(0x0380739C, 0x03807400, 4):
            word = space.u32(slot)
            if word is None:
                break
            target = word & ~1
            # Bit 0 is the mode, so the alignment it implies is the check:
            # a Thumb entry is halfword-aligned, an ARM entry word-aligned.
            align = 2 if word & 1 else 4
            if not executable(space, target) or target % align:
                break
            seeds.append((target, "thumb" if word & 1 else "arm"))
    for path in args.seed_from_coverage:
        want = "arm7" if args.cpu == "arm7" else "arm9"
        taken = 0
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                col = line.split()
                # cpu pc thumb kind caller hits share; kind 1 is ROOT, the
                # only one that means "the interpreter was entered HERE".
                if len(col) < 6 or col[0] != want:
                    continue
                try:
                    pc, th, kind = int(col[1], 16), int(col[2]), int(col[3])
                except ValueError:
                    continue
                if kind != 1 or not executable(space, pc):
                    continue
                seeds.append((pc, "thumb" if th else "arm"))
                taken += 1
        print("coverage seeds from %s: %d" % (os.path.basename(path), taken))
    if not seeds:
        sys.exit("no seeds; pass --seed or --auto")

    funcs = {}
    pointers = collections.Counter()
    queue = collections.deque((a, m, 1) for a, m in seeds)
    badcount = 0
    rounds = 0
    datarg = data_regions(args.extract, args.cpu)
    skipped_data = 0
    while queue:
        addr, mode, tier = queue.popleft()
        if addr in funcs:
            if funcs[addr]["mode"] != mode:
                funcs[addr]["mode_conflict"] = True
            continue
        if not executable(space, addr):
            continue
        if in_data(datarg, addr):
            skipped_data += 1
            continue
        end, calls, lits, bad = walk(space, arm, thumb, addr, mode)
        badcount += bad
        funcs[addr] = {"mode": mode, "end": end, "size": end - addr,
                       "calls": len(calls), "tier": tier}
        # A call from a tier-N function is still tier N: the pointer that
        # started the chain is what the confidence rests on.
        for t, tm in calls:
            if t not in funcs:
                queue.append((t, tm, tier))
        for w in lits:
            if executable(space, w & ~1):
                pointers[w] += 1

        # With --expand, a code pointer sitting in a literal pool is fed back
        # as a seed. That is a widening of the rule and it is opt-in for that
        # reason: such a word is very likely a function pointer - a vtable
        # slot, a callback, a jump-table entry - and its low bit carries the
        # mode, but nothing here proves it is an *entry* rather than a label.
        # The report keeps the two tiers apart either way.
        if args.expand and not queue:
            for w in list(pointers):
                t = w & ~1
                if t not in funcs:
                    queue.append((t, "thumb" if w & 1 else "arm", 2))
            rounds += 1

    tier3 = 0
    if args.scan_data:
        # A far wider net: any word anywhere that points into executable memory.
        # Most of those are not function entries, so each candidate must also
        # open with a plausible prologue before it is walked. Both halves of
        # that rule are reported so the widening is visible.
        cands = []
        for prog in space.programs:
            for off in range(0, len(prog.data) - 3, 4):
                w = struct.unpack_from("<I", prog.data, off)[0]
                t = w & ~1
                if t in funcs or not executable(space, t):
                    continue
                if in_data(datarg, t):
                    continue
                cands.append((t, "thumb" if w & 1 else "arm"))
        checked = []
        for t, m in dict(cands).items():
            if prologue(space, arm, thumb, t, m):
                checked.append((t, m))
        print("tier 3 candidates: %d words point into code, %d open with a "
              "plausible prologue" % (len(dict(cands)), len(checked)))
        before = len(funcs)
        queue.extend((a, m, 3) for a, m in checked)
        while queue:
            addr, mode, tier = queue.popleft()
            if addr in funcs or not executable(space, addr):
                continue
            end, calls, lits, bad = walk(space, arm, thumb, addr, mode)
            badcount += bad
            funcs[addr] = {"mode": mode, "end": end, "size": end - addr,
                           "calls": len(calls), "tier": tier}
            for t2, tm in calls:
                if t2 not in funcs:
                    queue.append((t2, tm, tier))
        tier3 = len(funcs) - before

    print("cpu %s, seeds %d, expansion rounds %d" % (args.cpu, len(seeds), rounds))
    if datarg:
        print("   header-declared data regions excluded: %s (%d candidates rejected)"
              % (", ".join("0x%08X..0x%08X" % r for r in datarg), skipped_data))
    print("functions found: %d (tier 1+2 by call graph: %d, tier 3 added by "
          "the data-pointer scan: %d)" % (len(funcs), len(funcs) - tier3, tier3))
    byname = collections.Counter()
    for a, f in funcs.items():
        p = space.find(a)
        byname[(p.name if p else "?", f["mode"])] += 1
    for (sec, mode), n in sorted(byname.items()):
        print("   %-10s %-6s %5d" % (sec, mode, n))
    bytier = collections.Counter(f["tier"] for f in funcs.values())
    print("   by discovery tier: %s"
          % ", ".join("tier %d = %d" % (t, n) for t, n in sorted(bytier.items())))
    conflicts = [a for a, f in funcs.items() if f.get("mode_conflict")]
    print("   mode conflicts (a function called both ways): %d %s"
          % (len(conflicts), [hex(a) for a in conflicts[:6]]))
    print("   undecodable instructions hit: %d" % badcount)

    new = [w for w in pointers if (w & ~1) not in funcs]
    print()
    print("tier 2 - code pointers in literal pools: %d distinct, %d not already "
          "a tier-1 function" % (len(pointers), len(new)))
    tm = collections.Counter("thumb" if w & 1 else "arm" for w in new)
    print("   by mode bit: %s" % dict(tm))

    # Coverage MUST be computed from merged intervals. Summing the sizes
    # double-counts: a walk follows forward branches, so two functions that
    # share a tail - or one whose extent overrun swallows the next - both claim
    # the same bytes. Summing gave 188% on the ARM9, which is how the bug was
    # noticed; on the ARM7 it silently read 77.8% for a true 69.5%.
    spans = sorted((a, f["end"]) for a, f in funcs.items())
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    covered = sum(b - a for a, b in merged)
    total = sum(len(p.data) for p in space.programs if p.name in EXEC_SECTIONS)
    print()
    print("bytes inside a function: %d of %d executable (%.1f%%), over %d "
          "contiguous runs" % (covered, total,
                               100.0 * covered / total if total else 0,
                               len(merged)))
    overlap = sum(f["size"] for f in funcs.values()) - covered
    if overlap:
        print("   extents overlap by %d bytes - a walk that follows forward "
              "branches can overrun into the next function" % overlap)

    if args.tsv:
        # --merge: keep every line of the existing corpus byte-for-byte and
        # add only addresses it lacks. Additive by construction, so no entry
        # that lifts today can stop lifting because of this scan.
        kept = []
        held = set()
        grown = 0
        if args.merge and os.path.exists(args.merge):
            with open(args.merge, encoding="utf-8") as fh:
                for n, line in enumerate(fh):
                    if n == 0 or not line.strip():
                        continue
                    col = line.rstrip("\n").split("\t")
                    a = int(col[0], 16)
                    held.add(a)
                    # GROW ONLY. `high` is raised solely by instructions the
                    # walk actually decoded, so a larger extent from this scan
                    # means more real code was read and adopting it is strictly
                    # better. Shrinking is the direction that can un-lift
                    # working code, so an existing row is never reduced here.
                    #
                    # This is not pedantry. 0x03803C7C sat in the corpus at
                    # size 0x38, which ends at 0x03803CB4 -- exactly the first
                    # arm of the switch at 0x03803CAC. A merge that only ADDED
                    # rows kept that stale extent and left the arms undeclared,
                    # so the switch fix bought nothing for it (docs/305).
                    if len(col) > 2 and col[2]:
                        f = funcs.get(a)
                        if f and f["size"] > int(col[2], 16):
                            col[2] = "0x%x" % f["size"]
                            grown += 1
                    kept.append("\t".join(col))
            print("merge: kept %d existing rows from %s (%d extents grown)"
                  % (len(kept), args.merge, grown))
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        added = 0
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("address\tmode\tsize\tcalls\ttier\tsection\n")
            for line in kept:
                fh.write(line + "\n")
            for a in sorted(funcs):
                if a in held:
                    continue
                f = funcs[a]
                p = space.find(a)
                added += 1
                # The REAL discovery tier. This column used to be a literal 1
                # for every walked function, which made a corpus built with
                # --scan-data claim its tier-3 finds were call-graph certain.
                # Nothing consumes the column, so correcting it is free, and
                # a corpus must never misstate its own provenance.
                fh.write("0x%08x\t%s\t0x%x\t%d\t%d\t%s\n"
                         % (a, f["mode"], f["size"], f["calls"], f["tier"],
                            p.name if p else "?"))
            # Tier-2 rows are POINTER sightings, not walked functions, so they
            # carry no size. A consumer that reads the size column must cope
            # with that -- and one that filters on it must not then treat a
            # header-declared data address as code, which is why these are
            # filtered here as well as in the walk.
            for w in sorted(new):
                if in_data(datarg, w & ~1) or (w & ~1) in held:
                    continue
                p = space.find(w & ~1)
                fh.write("0x%08x\t%s\t\t\t2\t%s\n"
                         % (w & ~1, "thumb" if w & 1 else "arm",
                            p.name if p else "?"))
        if args.merge:
            print("merge: added %d new rows" % added)
        print("wrote %s" % args.tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
