# 332 — The native port: a metric that works, and a first tranche that is sized

docs/329 established that a *matching* decompilation is blocked by mwccarm not
being on this machine, and that the recomp port is the oracle a native port
would test against. docs/331 gave the denominators. This is the step after:
**what replaces `MATCH` as the progress number, and what gets written first.**

## The number is worse than 0.31%, and it matters which way

docs/331 reports 789 byte-matched functions, 3,380 bytes, 0.31% of the
1,095,891-byte binary. Splitting that by who wrote it:

| source | functions | bytes |
|---|---:|---:|
| `generated/accessors_arm.c` | 379 | — |
| `generated/accessors_thumb.c` | 406 | — |
| **both generated files** | **785** | **3,348** |
| `battles/stats.c` (hand-written) | 4 | **32** |
| `battles/collectable.c` (hand-written) | 2 | 0 — both **MISMATCH** |

**Hand-written, byte-matched decompilation of LEGO Battles is four functions
and 32 bytes: 0.003%.** The rest is a generator emitting `return
*(u32*)(this+0x18);` 785 times, and the only two hand-written functions that
attempt anything else both fail to match.

This is not a criticism of the generator — an accessor is genuinely done, and
785 of them is a real inventory of field offsets. It is a correction to what
"0.31% decompiled" sounds like.

## Why `MATCH` cannot be the native port's metric

`tools/lb_match.py` marks a function done when GCC reproduces the ROM's bytes.
The ROM was built with **mwccarm**. The ceiling that produces is visible in the
data: every matched function is ≤ 12 bytes, because three instructions have one
sensible encoding and four do not.

A native port does not need byte equality. But the moment byte equality stops
being the goal, `MATCH` stops meaning anything and **the project has no other
progress number at all** — which is how "11.5% decompiled" (a function count on
a distribution whose done functions average 4.3 bytes) came to be quoted for
months.

## The replacement: shadow execution against the running guest

The recomp port runs the cartridge's real code and already has the machinery:
`kOverrideTable` in `lb_patches.cpp` binds a C function to a guest address, and
`lb_dispatch_original(0, addr)` calls what was there. Thirteen overrides use it
today.

A **shadow** is the same hook used as a test rather than a patch:

1. capture the arguments from `g_cpu.R[0..3]` and any memory they point at;
2. call `lb_dispatch_original` — the real guest code — and capture its result
   and the memory it wrote;
3. call the C reimplementation on a copy of the same inputs;
4. compare, and count.

A function is **verified** when it has accumulated N live calls during real play
with zero disagreements. That is a property of the reimplementation, it needs no
compiler agreement, and it is produced by playing the game rather than by a
separate test corpus — real play sessions grade the port.

The status file becomes `docs/data/native_status.tsv`: symbol, address, size,
source file, calls observed, disagreements, first disagreeing input. **The
percentage is the byte total of functions with calls ≥ N and disagreements = 0,
over 1,095,891.** It is honest in a way `MATCH` never was about this binary,
because it measures behaviour, which is what a port has to preserve.

Three things it does not do, stated so they are not claimed later: it only
exercises paths the game actually takes; it cannot see a difference that both
implementations make in the same wrong way; and a function with side effects
needs its writes captured, not just its return value.

## The first tranche: 1,480 functions, 82,318 bytes, 7.51%

Reimplementation has a dependency order — a function that calls three others
cannot be finished before them. Except that a large part of this binary calls
nothing at all.

From `docs/data/arm9_inventory.tsv`, functions that are not thunks, not
externals, and have **`called = 0`** (no outgoing direct call in Ghidra's call
graph): **1,542 functions, 87,318 bytes.**

`called = 0` is not the same as self-contained. Ghidra's call graph counts
direct branches, and a virtual dispatch is an indirect one. Checking the
decompiler corpus for the `(**(code **)(...))()` pattern across those 1,542:

    no indirect call   1,480 functions   82,318 bytes   7.51% of the binary
    has indirect call     61 functions    5,000 bytes
    corpus missing         1

> ### CORRECTION, 2026-09-20: it is 1,150 functions and 6.35%, not 1,480 and 7.51%
>
> The `(**(code **)` test caught one spelling of an indirect call and missed
> the others. `func_02085d58` is a plain tail call —
>
>     02085d58  ldrb r1,[r1]
>     02085d5c  ldr  ip,[pc,#0x10]   = 0x020ed808
>     02085d70  bx   ip
>
> — which Ghidra renders `(*DAT_02085d74)(param_1 + 0x4c, *param_2)` and the
> call graph records as `called = 0`, because a `bx` through a register is not
> a direct branch. Re-filtering for **any** call spelling (`FUN_0`, `func_0`,
> `(*DAT_`, `(**(code`, `(*pcVar…`), excluding each function's own name so a
> file does not match its own header:
>
>     call-free leaves            1,150 functions   69,590 bytes   6.35%
>     call-free AND store-free      579 functions   26,530 bytes   2.42%
>
> **A SECOND CORRECTION to the second row, same day.** The store test was
> `^\s*(\*|\w+\[)[^=]*=[^=]` — it required a character after the `=`, so it
> missed every assignment Ghidra wrapped onto the next line, which is most of
> the wide ones. `func_02015DB8` ends in `strh r1,[r0,#0x1a]` and was listed as
> gradeable; its decompiler output is
>
>     *(ushort *)(param_1 + 0x1a) =
>          *(byte *)(param_2 + 4) & 0x1f | ...
>
> with the `=` last on its line. Anchoring the match to `=([^=]|$)` drops 7
> functions and 440 bytes. **Probing a storing function would apply its write a
> second time**, so this filter being right is a correctness requirement and
> not a bookkeeping one.
>
> **The 7.51% above was wrong and is withdrawn.** The second row matters
> because the grading probe in docs/333 **re-executes** the cartridge's code:
> a function that writes memory would apply its effect twice. Only the
> store-free subset can be graded that way, so **2.46% is the reach of the
> harness as built**, and the other 3.89 points need a method that does not
> re-run the function.

`FUN_0205a03c` and `FUN_0206ad74` — the two highest-traffic candidates in game
code — each make a virtual call through an object they were handed. They are
not first-tranche work.

The size distribution below is of the original 1,480 and is left as measured;
the shape it shows — half the tranche's bytes in the 52 largest — survives the
correction:

| size | functions | bytes |
|---|---:|---:|
| ≤ 16 | 603 | 5,748 |
| 17–64 | 593 | 20,898 |
| 65–128 | 150 | 13,362 |
| 129–256 | 82 | 14,670 |
| > 256 | 52 | 27,640 |

The 52 largest hold a third of the tranche's bytes and sit at `0x0210`–`0x0212`,
which is SDK and C-runtime territory — decompression, matrix maths, memory
moves. Those are the best first targets twice over: they are the largest single
wins, and several of them are standard algorithms a native port would replace
outright rather than transcribe.

## The order of work

1. **`lb_shadow.cpp` and the harness.** One registry, one comparison, one TSV.
   Until this exists no reimplementation can be graded and the percentage cannot
   move honestly.
2. **One function end to end**, to prove the harness rather than to make
   progress: a small leaf with heavy traffic, verified over a real session.
3. **The 52 large leaves**, largest first, which is 27,640 bytes — **2.52% of
   the binary** — and the cheapest bytes in it.
4. Only then the 65–256 byte band, where the game logic starts.

## Where the number stands today

| measure | bytes | % |
|---|---:|---:|
| hand-written, byte-matched | 32 | **0.003%** |
| byte-matched including generated accessors | 3,380 | 0.31% |
| behaviourally verified (this document's metric) | 0 | **0%** — the harness does not exist yet |
| reachable with no dependency ordering | 82,318 | 7.51% |

The last row is not progress. It is the size of the next step, which is the
first time this project has had one measured.

> **Both figures in the last two rows have moved since.** The harness exists
> and `Battles_RegistryFind` passed (docs/333), so the verified row is 68
> bytes, 0.0062%. The dependency-free row is **69,590 bytes, 6.35%** after the
> correction above, of which **26,970 bytes, 2.46%** is gradeable by the
> current probe. `docs/data/native_status.tsv` is the live count.
