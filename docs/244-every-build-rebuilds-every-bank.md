# 244 — Every build rebuilds all 302 generated banks, and why

An incremental build that touched only `lb_overlay.cpp` and `lb_patches.cpp` was
still compiling **generated bank objects** twenty-five minutes in. That is not a
misconfiguration; it follows from how the port applies its hooks, and it is worth
writing down because it sets the floor on every edit-build-run cycle.

## The chain

1. `port/runner/build.sh` assembles a build tree under `port/runner/tree/`,
   copying upstream with `cp -rp` and this port's own sources with `cp -p`. The
   `-p` matters and is already correct: timestamps are preserved, so a plain
   re-copy does not by itself invalidate anything.
2. `port/runner/apply_hooks.py` then patches files in that tree. Among them is
   **`runtime_arm.h`** — `apply_hooks.py:1252`, recorded in its own log line as
   "runtime_arm.h: inline per-instruction predicates in banks".
3. All **302** files in `port/generated/*.c` include that header.
4. Its rewrite gives it a fresh mtime, newer than every bank `.obj`, so make
   correctly rebuilds all of them.

Make is behaving properly here. The header really did change relative to the
pristine upstream copy, and the banks really do depend on it.

## The cost

Measured, not estimated: **23 bank objects in ~25 minutes** at `LB_JOBS=1`,
against 302 generated `.c` files. That is roughly 65 seconds per object and would
put a full serial rebuild near five hours.

`LB_JOBS=1` is not the villain but it is the multiplier. It exists for a good
reason — three earlier builds died with the compiler killed for memory while a
game run and the master server competed for RAM — but that reason only applies
*while something else is running*. On an otherwise idle machine the same build at
`LB_JOBS=8` is bounded by cores rather than by patience.

**Rule of thumb: `LB_JOBS=1` only when a run or the server is up. Otherwise 8.**

## A fix that is deliberately NOT applied yet

`apply_hooks.py` writes unconditionally — `insert()` and `replace()` both end in
`open(path, "w")` — so the patched header gets a new mtime every build even
though the patch is deterministic and the resulting bytes are identical each
time. The obvious fix is to make the write conditional, or to restore a stable
mtime afterwards, so that a rebuild with no source change leaves the header
untouched and make skips all 302 banks.

That would turn a ~40-minute cycle into seconds, which is the single largest
downtime in this project. It is not applied here because the naive version is
**unsafe in a way this project has already been burned by**: if the mtime is
pinned to the upstream file's, then editing `apply_hooks.py` itself changes the
patched output while leaving the timestamp old, make skips the rebuild, and the
binary silently does not contain the change. The existing note that
`bash port/build.sh` from PowerShell "silently runs the old binary" is the same
class of fault, and it cost real debugging time.

A correct version has to make the header's mtime depend on **both** inputs — the
upstream source and `apply_hooks.py` — for example by stamping it with
`max(mtime(upstream), mtime(apply_hooks.py))`, or by comparing content against
the previous tree before overwriting. Either is a small change, but it is a
change to the mechanism that guarantees the built binary matches the sources, so
it wants doing deliberately and verifying with a test that proves an
`apply_hooks.py` edit still forces a rebuild — not slipping in beside unrelated
work.

Recorded so the next person hitting a forty-minute "incremental" build knows it
is expected, knows the job-count rule, and knows the fix and its trap.
