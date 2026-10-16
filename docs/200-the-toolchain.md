# 200 — The toolchain: Ghidra, a decompiled corpus, and an oracle

Status: **installed and working.** Everything here is reproducible from this
document.

## What is installed, and where

    <tools>\jdk21\jdk-21.0.12.1+1        Temurin JDK 21 (Ghidra 12 needs 21+)
    <tools>\ghidra_12.1.3_PUBLIC         Ghidra 12.1.3
    <tools>\ghidra_scripts               LBSeed.java, LBDecomp.java
    <tools>\ghidra_proj                  the analysed project (LB)
    <tools>\decomp                       decompiled C, one file per function
    <tools>\desmume                      DeSmuME 0.9.13 x64, for the oracle

The machine had only Java 8, which Ghidra 12 refuses; JDK 21 is installed
alongside rather than replacing it, and nothing on PATH changed.

## Importing the ARM9

    $env:JAVA_HOME="<tools>\jdk21\jdk-21.0.12.1+1"
    $env:LB_CORPUS="...\out\eur\corpus.tsv"
    analyzeHeadless.bat <tools>\ghidra_proj LB `
      -import ...\out\eur\arm9\section_static_02000000.bin `
      -processor ARM:LE:32:v5t -loader BinaryLoader -loader-baseAddr 0x02000000 `
      -scriptPath <tools>\ghidra_scripts -preScript LBSeed.java

163 seconds. `LBSeed` feeds Ghidra the 9,315 entry points this project's own
scanner already recovered, **with their ARM/Thumb mode**, before analysis. That
matters more on ARM than elsewhere: the instruction set is per-address state,
and a decompiler that guesses Thumb where the code is ARM produces confident,
plausible, wrong C. Half the functions this port cares about are Thumb
(`func_020A3234`, `func_020A3478`) and sit right next to ARM ones.

## Decompiling on demand

    $env:LB_TARGETS="<tools>\targets.txt"   # hex addresses, one per line
    $env:LB_OUTDIR="<tools>\decomp"
    analyzeHeadless.bat <tools>\ghidra_proj LB `
      -process section_static_02000000.bin -noanalysis `
      -scriptPath <tools>\ghidra_scripts -postScript LBDecomp.java

Twelve functions, zero failures, in well under a minute.

## What it bought immediately

The tilemap widening had been stuck for a long time on one question: where
does the guest put its tilemap? Deriving it from BGxCNT gave a relocation that
rendered a dither of wrong tiles, and the registers seemed self-contradictory.

`func_0210BC2C` — the function the scroll callback's DMA calls for its
destination — decompiles to three lines:

    return ((DISPCNT & 0x38000000) >> 27) * 0x10000 + 0x6000000 +
           ((*(u16*)0x0400000A & 0x1F00) >> 8) * 0x800;

`0x0400000A` is BG1CNT. So **the guest computes its DMA destination from the
register, at DMA time.** Two consequences, neither of which was going to be
guessed:

* The formula the port was using was already right. That doubt is closed.
* **Repointing BG1CNT moves the guest's own writes with it**, so block 0 fills
  itself. The host-side mirror was not merely unnecessary — it was the bug,
  copying the stale old block over the block the guest had just filled. That is
  exactly the dither.

The second staging buffer goes through `func_0210BC80`, a different base, so the
two layers are independent and repointing BG1 moves only BG1.

## The rule for using it

From the sibling project's `playbook/01-what-ghidra-silently-drops.md`: the
decompiler is excellent at producing C that *reads* like the function and is not
trying to produce C that *is* the function. It dropped call arguments at **169
of 170 call sites** for one function there, presenting as memory corruption
rather than as a decompiler fault.

So: **read it to understand, then write the change against the disassembly.**
`tools/lb_code.py` stays the authority on what the bytes are; Ghidra is the
authority on what they mean. Both headers written by `LBDecomp` say so.

## Why not a full decomp

Asked and answered by the sibling project's own history: LSW2 went from "a
corpus of Ghidra C that never booted" to the game running at 60fps, quickly,
by switching to ndsrecomp. A 10,019-entry-point title would multiply
every class of silent divergence in that playbook.

The right granularity is **targeted function-level overrides**: hand-write
correct C for the handful of view functions and register them through the
`lb_dispatch_original` trampoline the port already has. Decomp where it pays,
recompilation everywhere else. The list is about four functions, not ten
thousand.

## The oracle, not yet used

DeSmuME 0.9.13 is unpacked and ready to be piloted to a Free Play battle. A
savestate of that battle gives what static analysis cannot: what is really at an
address at runtime, on a reference emulator rather than on this port. The
sibling project calls savestates "the highest-value tool in the project" and
already has `tools/read_savestate.py` to mine them.

An earlier assessment dismissed savestates. That was too narrow — it
judged only whether the RUNNER could snapshot itself (it cannot cheaply; static
recompiler, guest CPU state in the recompiled register file). As an independent
oracle they are a different and much better proposition.

## Naming the virtuals, and the whole corpus (2026-09-24)

The cartridge's RTTI names 2,466 virtual functions (`tools/lb_rtti.py`,
`docs/data/vtable_methods.tsv`). The Ghidra project did not use them: its
exported corpus (`<tools>\decomp_all_v2`) knew 90 names, all from the
renderer audit. `tools/ghidra/LBApplyRtti.java` applies them, and
`tools/ghidra/LBDecompV2.java` (now in the repo too) re-exports every
function:

    $env:JAVA_HOME="<tools>\jdk21\jdk-21.0.12.1+1"
    analyzeHeadless.bat <tools>\ghidra_proj LB `
      -process section_static_02000000.bin -noanalysis `
      -scriptPath <repo>\tools\ghidra `
      -postScript LBApplyRtti.java <repo>\docs\data\vtable_methods.tsv `
      -postScript LBDecompV2.java <tools>\decomp_all_v3

Pass ONE directory to `-scriptPath`: the `.bat` forwards its arguments
through cmd.exe, which splits `a;b` at the semicolon ("Bad argument"). The
project was backed up first, to `<tools>\ghidra_proj_backup_2026-09-24`.

Result:

- 2,464 functions named. A name drops `Battles::` and joins the rest with
  `__`: `Battles::User::PauseMenu` slot 8 is `User__PauseMenu__v08`. Every
  class sharing the function is listed in its plate comment.
- **894 of them created**: virtual functions Ghidra's analysis had never
  found, because nothing calls them directly.
- 2 skipped, each inside an existing function that swallowed it:
  - `ArrowHighLightEffect` slot 5 (0200E6AC) inside `FUN_02007d80`;
  - `WorldViewScreen` slot 2 (020D9838) inside `FUN_020d9820`.
- The corpus now has 8,873 functions. Place any unnamed function by the
  nearest named ones: the `User__PauseMenu__v*` entries bracket the pause
  menu's own helpers.

It answered docs/361's open question in minutes: the battle view's exit
(`FUN_020dacb4`, between `User__WorldViewScreen` methods) goes through
`FUN_020a2334`, a fade to black on both screens.
