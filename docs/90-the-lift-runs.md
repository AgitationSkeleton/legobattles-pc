# The lift runs

**Status: the lift runs and the output compiles. All 95 programs audit clean —
every SHA-1 verified, zero undefined functions, no codegen gaps — and all 205
translation units build to native objects with zero errors.** This is the first
time any part of LEGO Battles has existed as native machine code.

## Getting the recompiler to build

Four obstacles, none of them interesting except the last:

1. **cmake resolved to MSYS's**, which rewrites Windows paths, so
   a `X:/...` path became `/x/<cwd>/X:/...`.
2. **The path contains spaces**, which survives quoting in
   bash but not the round trip through cmake's path translation.
3. **Ninja resolved to Visual Studio's copy**, which cannot read MSYS paths and
   failed the compiler probe.
4. **`g++` on PATH is devkitPro's cross-compiler** — it has no host libstdc++, so
   `#include <cstdint>` failed. devkitPro is for building ARM binaries, which is
   the opposite of what is wanted here.

The fix: copy the recompiler to a space-free path
(`$NDSRECOMP`), use the Unix Makefiles generator to
avoid the wrong ninja, and point cmake at **MSYS2's UCRT64 g++ 16.2** explicitly.
`port/lift.sh` records the working invocation so none of this has to be
rediscovered.

## Two defects in my own config, both caught by tools rather than by me

**The recompiler rejected the first run outright:**

```
[nds_recompile] CONFIG ERROR SHA-1 mismatch:
  config declares: ad218acaff8af5e14c27c8032ae99cf94641dcac
  binary hashes:   dd6d06b7603d6e0f5dca51fbacfc7d710acf4f79
```

I had written the **unpacked image's** hash while `load_address` and `size`
described the **static section**. The identity has to match the thing the other
fields describe. Corrected in the generator.

**The second is the more consequential one.** The first successful audit
reported:

```
authoritative entries: no
[finder] functions=68489 (arm=33307 thumb=35182)
[audit] decoded 4124521 instructions
[audit] UNDEFINED encodings: ARM 0x6D657449 x4
```

`0x6D657449` is `"Item"` — a string being decoded as an ARM instruction. The
recompiler was running its **own speculative function finder** on top of my
seeds and inventing 59,333 functions that are not functions.

`[program].authoritative_entry_points = true` turns that off, and it requires an
**exact size for every entry point** — which this project has for all 9,314
functions. With it on:

| | speculative | authoritative |
| --- | ---: | ---: |
| functions | 68,489 | **9,156** |
| instructions decoded | 4,124,521 | **279,959** |
| finder `undefined` | 1 | **0** |
| distinct undefined encodings | 25 | 19 |
| codegen gaps | none | **none** |

**9,156 is exactly the config's entry count.** Nothing was guessed, which is the
project's rule and what sixty passes of corpus verification bought.

## The result

```
95 programs ok, 0 failed, 9867 functions

lb_arm9.toml        9156 functions  undefined=0   codegen gaps: none
lb_arm9_itcm.toml      6             undefined=0
lb_arm7_static.toml    3             undefined=0
lb_arm7_wram.toml    330             undefined=5
lb_ov000..090.toml     4 each        undefined=0
```

Emitting the ARM9 produces three files:

```
section_static_02000000.c            191.7 MB   9,156 function bodies
section_static_02000000.h              0.7 MB   declarations, mode-annotated
section_static_02000000_dispatch.c    10.9 MB   the dispatch table
```

The header carries the modes through, one per function:

```c
void section_static_02000000_tfunc_02000064(void);  /* 0x02000064 thumb */
```

**`port/generated/` is gitignored.** That C is the cartridge's own code
translated; committing it would be redistributing the game. The configs that
produce it are tracked, the output never is.

## It compiles

Sharding was the missing piece and it is not cosmetic. Unsharded, the ARM9 is a
single **191 MB `.c`**. `MetroidPrimeHuntersRecomp` splits its ARM9 sixteen ways,
so the same flags were used here:

```
--bank lb_arm9 --shards 16 --stable-address-shards
--max-function-bytes 512 --validate-live-bytes
```

`--validate-live-bytes` makes the recompiler check every function against the
live guest bytes before lowering it. Since nothing may be guessed, that is not
an option to leave off.

Sixteen shards, the largest 27 MB, the smallest 324 bytes. An 8.5 MB shard
compiles in **13 seconds**; the whole ARM9 bank — 16 shards plus a 17 MB
dispatch table — builds in **73 seconds** to 108 MB of objects with **zero
errors**.

Across every program:

```
95 programs ok, 0 failed, 9867 functions
205 of 205 objects built, 0 failed
```

`port/lift.sh compile` does the whole thing. `port/obj/` is gitignored alongside
`port/generated/`.

## What is still open

* **`lb_arm7_wram` has `undefined=5`** — five encodings the ARMv4T decoder does
  not recognise in the ARM7 WRAM section. Small, but real, and unexamined.
* **19 distinct undefined encodings in the ARM9**, inside real functions. These
  are almost certainly literal pools sitting between basic blocks — exactly the
  thing that cost `docs/83` a retraction. Declaring them as `data_range` entries
  is the correct fix and the config schema supports it.
* **Nothing has been LINKED or run.** Every translation unit compiles, but an
  executable needs the runner — the memory bus, the two-CPU scheduler, the
  hardware models and SDL presentation. That is the next question and a large
  one.
* The ARM7 `mainram` autoload still has no entry points (`docs/89`), so it is not
  lifted at all.

## Tips and pitfalls

* **A tool that refuses your input is doing you a favour.** The SHA-1 rejection
  and the `authoritative_entry_points` size requirement both caught real errors
  in configuration I had already validated my own way.
* **Check whether the tool is guessing on your behalf.** "authoritative entries:
  no" was one line of output and the difference between 9,156 real functions and
  68,489 invented ones.
* **A cross-compiler on PATH is not a host compiler.** devkitPro's `g++` builds
  ARM binaries; it cannot build a tool that runs on this machine.
* **Sizes are worth carrying.** The corpus recording a size per function is what
  made the authoritative mode available at all.
