# 329 — A native PC port: what it would actually take

Status: **assessment, written after the pivot was reaffirmed.** docs/328
measured the starting point. This one is about the route.

## First, a word used carelessly

An earlier note said "all 6,838 functions decompiled" after running Ghidra
headless over the ARM9. That was wrong. What exists is **Ghidra's
automatic decompiler output**: 6,838 files, 19 MB, every function named
`FUN_<addr>`, every type `undefined1`/`undefined4`, an 8,780-byte function
rendered as a wall of `auStack_830[12]`. It does not compile, it has no names,
no structs, and the header on each file records that Ghidra dropped call
arguments at **169 of 170 call sites** for one function this project checked.

It is raw material, not a decompilation. **The number is still 0.33%.**

## The toolchain blocker, and who it blocks

`tools/lb_match.py` states it: LEGO Battles was built with **mwccarm**
(Metrowerks), the project has **devkitARM GCC**, and the two do not agree.
mwccarm is not installed anywhere on this machine.

That blocks a **matching decomp** — rebuilding byte-identical ROM code — and it
is exactly why all 789 functions done so far are <= 12 bytes: accessors are the
only place where one encoding is forced, so a match is achievable there and
nowhere else.

It does **not** block a native PC port. A native port compiles for x86 and
never needs to reproduce ARM bytes. But that cuts both ways:

> The moment byte equality stops being the goal, `MATCH` stops being a
> progress metric, and the project has no other one.

`decomp_status.tsv`, `lb_match.py` and every percentage derived from them
measure a goal the pivot abandons. A native port needs a different proof of
correctness, and that is the real design question — not which Ghidra frontend
to use.

## The verification problem, and the asset nobody counted

A reimplemented function is correct when it behaves like the original. Proving
that needs a reference implementation to compare against.

**The ndsrecomp port is that reference.** It already runs the cartridge's real
code, exposes memory over a debug server, and can be driven frame by frame. So
the tractable route is differential:

1. pick a function, reimplement it natively in C;
2. run the original (recomp) and the new one on the same inputs;
3. compare outputs and touched memory;
4. swap it in only when they agree, and keep the original available to fall
   back to.

That makes each step verifiable on its own, and it means the recomp path is
**not the thing being replaced — it is what makes the replacement checkable.**
Discarding it and starting a clean-room reimplementation throws away the only
oracle available and leaves correctness resting on reading Ghidra output, which
the 169-of-170 figure says is not safe.

## On GhydraMCP and GhidraMCP

Both were cloned and inspected. Both are Ghidra **tool** plugins:
`GhydraPlugin extends Plugin implements ApplicationLevelPlugin`, with a
`GhidraSwing.java` helper. A tool plugin needs a `PluginTool`, which exists only
in the GUI front-end; `analyzeHeadless` has none, so neither loads headless.

The plugin's source could be modified so it can be automated. Before doing
that it is worth saying what it would buy: `FunctionService.java` drives
`DecompInterface` — the same API `LBDecomp.java` already calls headless, and
which just produced all 6,838 files in about thirteen minutes. Renaming,
retyping and cross-references are likewise plain Ghidra script APIs.

So stripping the GUI layer out of GhydraMCP would add an HTTP hop to reach
capabilities already in hand. **It is not the bottleneck.** Writing Ghidra
scripts directly is the same access with fewer moving parts. GhydraMCP earns
its keep only for interactive, human-in-the-loop exploration, and it is cloned
and ready at `<tools>\GhydraMCP` for when the GUI is available.

## What the bottleneck actually is

Not tooling. Not Ghidra access. It is that 1.01 MB of ARM9 plus 268 KB of
overlays have to be **understood**, and understanding does not parallelise the
way extraction does. The decompiler output was the cheap part and it is done.
