#!/usr/bin/env python3
"""Grade a reimplemented function against the cartridge's own code.

    python tools/lb_grade.py --demo demos/user-combat-2026-09-14.lbdemo

THE METHOD is docs/333. Replay a recorded route with LB_SHADOW=verify. The
cartridge runs normally and serves the game; the override records the arguments
it was called with, and a later call drains that batch, asks the CARTRIDGE the
same questions out of turn, and compares its answers against src/battles/.

WHAT THIS REPLACED, and why it is not still here. The first version ran three
arms -- control, control, native -- and compared FNV hashes of all 4 MB of main
RAM. The control-vs-control half of that was worth having and passed outright:
143 of 143 identical hashes over 17,040 frames, so the replay is
bit-deterministic. The native half was unusable. ndsrecomp accounts emulated
cycles per LIFTED INSTRUCTION, so a host replacement costs zero cycles where
the cartridge's costs sixty-odd; the emulated clock runs ahead, the scheduler
crosses timer boundaries at different instants, and a hash taken at a fixed
frame catches counters mid-update. It diverged at 4 of 136 samples and looked
like a failed reimplementation. It was not -- the divergences healed and the
call counts were identical. Verify mode replaces nothing, so no cycle changes.

THREE WAYS TO PASS FOR THE WRONG REASON, all checked here:

  * NOTHING WAS GRADED. Every probe refused, or the function was never called.
    Zero disagreements out of zero comparisons reads as a pass.
  * THE ORACLE WAS VACUOUS. 600 agreements mean nothing if every one was
    `0 == 0`; a stub returning 0 unconditionally would score the same. The run
    reports how many graded calls FOUND something and how many distinct keys
    and results the sample held.
  * SOMETHING WAS REPLACED. `native` must be 0: in verify mode the cartridge
    serves every call, and a non-zero count means the run was not measuring
    what it says.

Exit status is 0 only on a pass.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ROW = re.compile(
    r"^\[lb_shadow\] (\S+)\s+guest (\d+)\s+native (\d+)\s+graded (\d+)\s+"
    r"disagreed (\d+)\s+refused (\d+)\s+\|\s+found (\d+)\s+a0>=(\d+)\s+"
    r"a1>=(\d+)\s+results>=(\d+)\s*$")
FIRST = re.compile(r"^\[lb_shadow\]\s+first disagreement: (.+)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--window-size", default="1024x600")
    ap.add_argument("--stop-after", type=int, default=1800)
    ap.add_argument("--symbol", default="Battles_RegistryFind")
    ap.add_argument("--label", default="grade")
    ap.add_argument("--min-graded", type=int, default=100)
    ap.add_argument("--min-found", type=int, default=20,
                    help="graded calls that returned non-zero")
    ap.add_argument("--min-keys", type=int, default=5)
    ap.add_argument("--reuse", action="store_true")
    args = ap.parse_args()

    log = os.path.join(ROOT, "screenshots", "exp-" + args.label, "stderr.log")
    if not (args.reuse and os.path.exists(log)):
        cmd = [sys.executable, os.path.join(HERE, "lb_exp.py"), args.label,
               "--demo", args.demo, "--window-size", args.window_size,
               "--env", "LB_SHADOW=verify",
               "--stop-after", str(args.stop_after)]
        print("replaying %s under LB_SHADOW=verify" % args.demo, flush=True)
        subprocess.run(cmd, cwd=ROOT, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    row, first = None, None
    if os.path.exists(log):
        with open(log, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = ROW.match(line)
                # Counters are cumulative and reprinted; the LAST row wins.
                if m and m.group(1) == args.symbol:
                    row = [int(x) for x in m.groups()[1:]]
                    continue
                m = FIRST.match(line)
                if m:
                    first = m.group(1)

    if row is None:
        print("REFUSED: no [lb_shadow] row for %s in %s" % (args.symbol, log))
        return 2

    (guest, native, graded, disagreed, refused, found,
     keys, keys1, results) = row
    print()
    print("%s on %s" % (args.symbol, os.path.basename(args.demo)))
    print("  cartridge calls   %d" % guest)
    print("  graded            %d   (refused %d)" % (graded, refused))
    print("  disagreements     %d" % disagreed)
    print("  found a node      %d" % found)
    print("  distinct args    a0>=%d  a1>=%d   distinct results >=%d"
          % (keys, keys1, results))
    if first:
        print("  first disagreement: %s" % first)

    fail = []
    if native != 0:
        fail.append("native=%d -- verify mode must replace nothing" % native)
    if graded < args.min_graded:
        fail.append("only %d graded, below --min-graded %d"
                    % (graded, args.min_graded))
    if found < args.min_found:
        fail.append("only %d graded calls found a node, below --min-found %d "
                    "-- the comparison is close to vacuous"
                    % (found, args.min_found))
    # THE GATE IS ON a0, and a1 is reported without being gated: the
    # one-argument functions pass a literal zero as a1 and must not be failed
    # for it. a0 is the argument every graded function actually has.
    if keys < args.min_keys:
        fail.append("only >=%d distinct values of a0, below --min-keys %d"
                    % (keys, args.min_keys))

    print()
    if fail:
        for f in fail:
            print("REFUSED: %s" % f)
        return 2
    if disagreed:
        print("FAIL: %d of %d graded calls disagree" % (disagreed, graded))
        return 1
    print("PASS: %d graded, 0 disagreements, %d found a node, "
          ">=%d a0 / >=%d a1" % (graded, found, keys, keys1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
