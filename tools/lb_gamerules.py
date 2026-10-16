#!/usr/bin/env python3
"""Recover the game-rule set: the toggle dispatcher, its flags, and its readers.

LEGO Battles keeps its cheats and match modifiers as **one byte per rule** in a
settings object returned by ``func_020092A8``. Nothing in the ROM names them.
This tool recovers the set structurally, from three independent directions that
have to agree.

## 1. The dispatcher

``func_02007F3C`` is a Thumb jump-table switch on a rule id:

    ldrb r0, [r1]            ; the rule id
    cmp  r0, #0x9a           ; 154
    bls  +2
    b    <default>
    adds r0, r0, r0          ; id * 2
    add  r0, pc              ; PC here is (insn + 4), NOT word-aligned --
    ldrh r0, [r0, #6]        ;   this is ADD(register), not a literal load
    lsls r0, r0, #0x10
    asrs r0, r0, #0x10       ; sign-extend the halfword
    add  pc, r0              ; target = (insn + 4) + offset

so the table is 155 halfwords at ``0x02007F54`` and a target is
``0x02007F56 + sign_extend(entry)``. The table's own self-check is that it ends
exactly where the first arm begins.

**Pitfall.** The arms disassemble as a run of nonsense ``lsls`` instructions if
you read the function linearly -- that is the halfword table being decoded as
Thumb code. And getting the ``add r0, pc`` semantics wrong is the classic way to
land a few bytes off and produce plausible-looking garbage: for ADD (register)
PC reads as *instruction + 4*, while for ADR and LDR-literal it would be
``Align(PC, 4)``. Two different rules, one register.

## 2. The arms

Every handled arm has the same shape::

    r0 = GetSettings()
    if (r0[flag] != 0) { GetSettings(); toggle_flag(); }

The toggle functions form a contiguous block of 36-byte functions at
``0x020092D4``, one per rule, each of which simply inverts its byte:

    ldrb r1, [r0, #N] ; cmp r1, #0 ; moveq r1, #1 ; movne r1, #0 ; strb r1, [r0, #N]

So this is the **toggle** path -- the menu that turns a cheat on or off -- and
the id-to-flag mapping falls straight out of it.

## 3. The readers

Separately, every call to ``func_020092A8`` in the corpus is followed within a
few instructions by a load at some displacement. Collecting those gives, per
flag, the set of functions that *consume* it. This is the part that assigns
meaning: a flag read by ``ConstructProgressAction`` and ``RepairStructureAction``
is about building, whatever it is called.

## Why the red brick identification is safe

`docs/27` recovered 18 red bricks with display names. The dispatcher handles 30
ids, of which **0..17 are contiguous** -- suggestive, but on its own that is
just a coincidence of counts.

The independent check is `func_020D28EC`: it reads **exactly** the 18 flags that
ids 0..17 map to, and none of the other 12. A function that partitions the flag
space precisely along the red-brick boundary, found without reference to the id
ordering, is what turns the count coincidence into a conclusion.

Usage:
    python tools/lb_gamerules.py                       # the table
    python tools/lb_gamerules.py --tsv docs/data/game_rules.tsv
"""

from __future__ import annotations

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lb_code  # noqa: E402

DISPATCHER = 0x02007F3C
JUMP_TABLE = 0x02007F54
#: `add pc, r0` at 0x02007F52 -- PC reads as the instruction address + 4
JUMP_BASE = 0x02007F56
N_ARMS = 0x9A + 1

SETTINGS_GETTER = 0x020092A8

#: The main loop, called only from `main`. Its per-frame debug overlay guards
#: each readout behind a rule flag, and the format strings name the rules
#: outright -- five of the twelve non-red-brick rules were named this way.
MAIN_LOOP = 0x020F6730

#: reads every flag, so it carries no per-flag information
BULK_READERS = {0x02007F3C, 0x020088D4}

#: the red brick order from the table at 0x02125FF4 -- see docs/27
RED_BRICKS = [
    ("RedBrick_TurnOffFow", "Reveal Map"),
    ("RedBrick_OneHitKill", "One Hit Kill (Heroes)"),
    ("RedBrick_FastProduction", "Fast Building"),
    ("RedBrick_QuickMagic", "Fast Magic"),
    ("RedBrick_DoubleStuds", "Double LEGO Bricks"),
    ("RedBrick_RevealMinikit", "Show Minikit"),
    ("RedBrick_RevealCoins", "Show LEGO Studs"),
    ("RedBrick_Regeneration", "Regenerating Health"),
    ("RedBrick_IncreaseSpellRange", "Long Range Magic"),
    ("RedBrick_IncreaseSpellTime", "Super Magic"),
    ("RedBrick_RevealRedBrick", "Show Red Bricks"),
    ("RedBrick_AutoUpgrade", "Upgraded Towers"),
    ("RedBrick_SuperExplosions", "Super Explosions"),
    ("RedBrick_FastHarvest", "Fast Harvesting"),
    ("RedBrick_FullAllowance", "Full Unit Cap"),
    ("RedBrick_InvincibleHero", "Invincible Hero"),
    ("RedBrick_FastMining", "Fast Mining"),
    ("RedBrick_EnemiesOnMinimap", "Show Enemies"),
]


def read_jump_table(space):
    """Return [target_address] * 155, and the default arm."""
    raw = space.read(JUMP_TABLE, N_ARMS * 2)
    entries = struct.unpack("<%dH" % N_ARMS, raw)
    targets = [JUMP_BASE + (e if e < 0x8000 else e - 0x10000) for e in entries]
    default = collections.Counter(targets).most_common(1)[0][0]
    return targets, default


def arm_flag(space, engines, addr):
    """The settings displacement an arm tests, or None."""
    _, thumb_cs = engines
    for ins in thumb_cs.disasm(space.read(addr, 24) or b"", addr):
        if ins.mnemonic.startswith("ldr") and "[r0" in ins.op_str:
            op = ins.op_str.split(",", 1)[1].strip()
            if op == "[r0]":
                return 0
            if op.startswith("[r0, #"):
                return int(op[6:-1], 0)
        if ins.mnemonic in ("b", "pop", "bx"):
            break
    return None


def collect_readers(space, funcs, engines):
    """flag displacement -> set of function names that read it."""
    hits = collections.defaultdict(set)
    for addr in sorted(funcs):
        if addr in BULK_READERS:
            continue
        try:
            insns = [t[0] for t in lb_code.disassemble(space, funcs, addr,
                                                       engines=engines)]
        except Exception:
            continue
        for i, ins in enumerate(insns):
            if ins.mnemonic not in ("bl", "blx"):
                continue
            if "%x" % (SETTINGS_GETTER & 0xFFFFF) not in ins.op_str:
                continue
            for j in range(i + 1, min(i + 6, len(insns))):
                nxt = insns[j]
                tail = nxt.op_str.split(",", 1)[-1].strip()
                if nxt.mnemonic.startswith("ldr") and tail.startswith("[r0"):
                    if tail == "[r0]":
                        disp = 0
                    elif tail.startswith("[r0, #"):
                        disp = int(tail[6:-1], 0)
                    else:
                        break
                    hits[disp].add(lb_code.best_name(funcs.get(addr), addr))
                    break
                if nxt.mnemonic in ("bl", "blx"):
                    break
    return hits


def debug_strings(space, funcs, engines):
    """flag displacement -> [format strings the main loop guards behind it].

    Walks MAIN_LOOP linearly, tracking the most recent `ldrb r0, [r0, #N]`
    that follows a settings-getter call, and collecting every PC-relative
    literal in range that points at printable text. The game names its own
    debug rules this way: "FPS:%d", "Free Memory:%ld", "Build:%d".
    """
    out = collections.defaultdict(list)
    cur = None
    try:
        insns = list(lb_code.disassemble(space, funcs, MAIN_LOOP, engines=engines))
    except Exception:
        return out
    for ins, thumb, _ in insns:
        tail = ins.op_str.split(",", 1)[-1].strip()
        if ins.mnemonic == "ldrb" and tail.startswith("[r0"):
            cur = 0 if tail == "[r0]" else int(tail[6:-1], 0)
            out.setdefault(cur, [])
            continue
        target = lb_code.literal_target(ins, thumb)
        if target is None or cur is None:
            continue
        try:
            val = space.u32(target)
        except Exception:
            continue
        if val is None:
            continue
        text = space.cstr(val, 64) if 0x02000000 <= val < 0x02400000 else None
        if text and len(text) > 2 and text.isprintable():
            out[cur].append(text)
    return out


def main(argv=None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", default=lb_code.DEFAULT_EXTRACT)
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--tsv")
    args = ap.parse_args(argv)

    corpus = args.corpus or os.path.join(args.extract, "corpus.tsv")
    space = lb_code.AddressSpace(args.extract, None, "arm9")
    funcs = lb_code.load_corpus(corpus)
    engines = lb_code.make_engines()

    targets, default = read_jump_table(space)
    handled = [(i, t) for i, t in enumerate(targets) if t != default]

    # self-check: the table must end exactly where the first arm begins
    first_arm = min(t for _, t in handled)
    table_end = JUMP_TABLE + N_ARMS * 2
    print(f"dispatcher func_{DISPATCHER:08x}: {N_ARMS} ids, "
          f"{len(handled)} handled, {N_ARMS - len(handled)} default")
    print(f"jump table 0x{JUMP_TABLE:08X}..0x{table_end - 1:08X}, "
          f"first arm 0x{first_arm:08X} -- contiguous: {table_end == first_arm}")

    id2flag = {}
    for rid, t in handled:
        f = arm_flag(space, engines, t)
        if f is not None:
            id2flag[rid] = f
    print(f"{len(id2flag)} arms yielded a flag displacement")

    readers = collect_readers(space, funcs, engines)
    debug = debug_strings(space, funcs, engines)

    # Flags the simulation reads that the dispatcher does NOT expose. Their
    # existence is why "30 rules" is the size of the toggle menu, not the size
    # of the settings object.
    unreachable = sorted(set(readers) - set(id2flag.values()))
    if unreachable:
        print("flags read by code but NOT toggleable through the dispatcher: "
              + ", ".join("+0x%02X" % f for f in unreachable))

    brick_flags = {id2flag[i] for i in range(len(RED_BRICKS)) if i in id2flag}
    partitioners = [n for n in set().union(*readers.values())
                    if {f for f in readers if n in readers[f]} == brick_flags]
    print(f"functions reading EXACTLY the {len(brick_flags)} red brick flags "
          f"and no others: {', '.join(partitioners) or '(none)'}")
    print()

    rows = []
    for rid in sorted(id2flag):
        flag = id2flag[rid]
        brick = RED_BRICKS[rid] if rid < len(RED_BRICKS) else ("", "")
        rows.append({
            "rule_id": rid,
            "flag_offset": "0x%02X" % flag,
            "kind": "red_brick" if rid < len(RED_BRICKS) else "other",
            "blueprint": brick[0],
            "display_name": brick[1],
            "toggle_func": "0x%08X" % (0x020092D4 + 0x24 * flag),
            "debug_text": ";".join(debug.get(flag, ())),
            "readers": ";".join(sorted(readers.get(flag, ()))),
        })

    print("%3s %6s %-11s %-30s %s" % ("id", "flag", "kind", "display name", "readers"))
    for r in rows:
        rd = r["readers"].split(";")
        shown = ", ".join(rd[:3]) + (" (+%d)" % (len(rd) - 3) if len(rd) > 3 else "")
        label = r["display_name"] or (r["debug_text"].split(";")[0]
                                      if r["debug_text"] else "-")
        print("%3d %6s %-11s %-30s %s"
              % (r["rule_id"], r["flag_offset"], r["kind"],
                 label, shown if r["readers"] else "-"))

    if args.tsv:
        os.makedirs(os.path.dirname(os.path.abspath(args.tsv)), exist_ok=True)
        cols = ["rule_id", "flag_offset", "kind", "blueprint", "display_name",
                "toggle_func", "debug_text", "readers"]
        with open(args.tsv, "w", encoding="utf-8") as fh:
            fh.write("\t".join(cols) + "\n")
            for r in rows:
                fh.write("\t".join(str(r[c]) for c in cols) + "\n")
        print(f"\nwrote {args.tsv} ({len(rows)} rules)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
