#!/usr/bin/env python3
"""Patch a COPY of the ndsrecomp function finder so a declared exact_size is
the function's extent, not merely an upper bound.

The upstream tree at $NDSRECOMP is never modified. port/recomp/build.sh copies
recompiler/ and cmake/ into a build tree and runs this over the copy, exactly
as port/runner/build.sh + apply_hooks.py do for the runner. Anchors are matched
by exact text and MUST appear exactly once; a missing anchor is a hard error,
never a silent skip.

    python apply_finder_patch.py <build-tree-recompiler-dir>


WHY THIS PATCH EXISTS
=====================

lb_arm9.toml declares 9,156 entry points, every one carrying an exact `size`
read from the ds-decomp corpus (`authoritative_entry_points = true` makes the
size mandatory -- config.cpp refuses the config without it). Those sizes total
412,770 instructions.

The finder decodes 279,959 of them. **132,811 instructions -- 32% of the
game's ARM9 code -- are never lifted**, because FunctionFinder::discover_one()
stops its linear walk at the first unconditional terminator:

    if (ins.is_return || ins.is_indirect) break;
    if (unconditional_branch)            break;

and it does that whether or not the seed carried an exact_size. The declared
size is used only as the loop's upper bound (`pc < exact_end`), never as a
lower one. Everything after a function's first `bx lr` / `pop {pc}` /
unconditional `b` is dropped on the floor.

The dropped code is not dead. func_020267f8 is declared 0x7A8 bytes; the
emitted validation record for it is 428 bytes, and port/generated's dispatch
rows for it run 0x020267F8..0x020269A0 and then jump straight to 0x02026FA0.
The 0x600-byte hole in between contains 0x02026ACC, 0x02026ADC and 0x02026D30,
which between them take 2.7 million Tier-3 interpreter entries in one freeplay
run.

Across the whole run: of 21,882,484 Tier-3 ROOT entries, 21,869,173 (99.94%)
are at a PC with no dispatch row at all, spread over 22,760 distinct addresses
in 368 contiguous regions. That is what the interpreter is for -- code copied
into RAM at runtime -- but this is not copied code. It is the cartridge's own
static ARM9 image, which the decomp has already described, being interpreted
because the lift never translated it.

The comment at the break site explains the reasoning it was written under:

    //   3. An unconditional branch (B with cond=AL) -- we'll
    //      reach the target by adding it to the worklist.

That holds for the BRANCH TARGET. It does not hold for the instruction that
FOLLOWS the terminator, which is reached only from somewhere else in the
function -- a conditional branch backwards, a jump table, a landing pad. In
discovery mode, where the finder has no declared extent, stopping is the
correct conservative choice: it cannot tell code from the literal pool that
usually follows a return. With an exact_size in hand it does not have to
guess, and the conservative choice is simply wrong.

So: keep the terminator breaks exactly as they are for unsized seeds, and skip
them when the decomp has declared where the function ends. A terminator then
ends a basic block rather than the function, which is what it is.

WHAT STILL STOPS THE WALK, and why that is the safety net: `ins.is_undefined`
still breaks, and `addr_in_data_range(pc)` still breaks. Decoding past a
terminator does mean decoding literal pools as instructions. Those bytes are
never branched to, so emitting a translation of them is inert -- but if one
decodes to something the codegen cannot lower, the emission fails loudly.
`nds_recompile --audit` reports both ("codegen NOT-IMPLEMENTED ops" and
"UNDEFINED encodings"), so run the audit after building this and read those
two sections before lifting.
"""

from __future__ import annotations

import os
import sys


def replace(path, old, new):
    """Exact-text replacement; the anchor must occur exactly once."""
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            "apply_finder_patch: anchor occurs %d times (want 1) in %s:\n%s"
            % (count, path, old[:200]))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text.replace(old, new))


def apply(recompiler_dir):
    finder = os.path.join(recompiler_dir, "finder", "function_finder.cpp")
    if not os.path.isfile(finder):
        raise SystemExit("apply_finder_patch: missing %s" % finder)

    done = []

    # 1. A return-shaped or indirect transfer ends a basic block. It ends the
    #    FUNCTION only when nothing told us where the function ends.
    replace(
        finder,
        "        if (ins.is_return || ins.is_indirect) {\n"
        "            break;\n"
        "        }\n",
        "        //   ...unless the seed carried an exact_size. Then the\n"
        "        //   decomp has declared this function's extent and a\n"
        "        //   terminator ends a BASIC BLOCK, not the function: the\n"
        "        //   instruction after it is reached from elsewhere in the\n"
        "        //   same body (a backward conditional branch, a jump table,\n"
        "        //   a landing pad) and stopping here strands it. The loop\n"
        "        //   condition above already bounds the walk at exact_end,\n"
        "        //   and is_undefined / addr_in_data_range still break, so\n"
        "        //   continuing cannot run past what the corpus declared.\n"
        "        //   Measured on lb_arm9: stopping here left 132,811 of the\n"
        "        //   declared 412,770 instructions unlifted, and 99.94% of\n"
        "        //   the runtime's 21.9M Tier-3 entries land in that hole.\n"
        "        if ((ins.is_return || ins.is_indirect) && exact_end == 0u) {\n"
        "            break;\n"
        "        }\n")
    done.append("function_finder.cpp: return/indirect ends a block, not a "
                "sized function")

    # 2. Same for an unconditional direct branch. Its target is reached via
    #    the worklist; its fall-through is not reached by anything unless the
    #    walk keeps going.
    replace(
        finder,
        "        if (unconditional_branch) {\n"
        "            break;\n"
        "        }\n",
        "        if (unconditional_branch && exact_end == 0u) {\n"
        "            break;\n"
        "        }\n")
    done.append("function_finder.cpp: unconditional B ends a block, not a "
                "sized function")

    return done


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: apply_finder_patch.py <recompiler-dir>")
    for line in apply(sys.argv[1]):
        print("  " + line)


if __name__ == "__main__":
    main()
