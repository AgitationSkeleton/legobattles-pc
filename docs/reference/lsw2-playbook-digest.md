# The LEGO Star Wars II playbook, digested for this project

Source: the LEGO Star Wars II DS project's `playbook/` folder — thirteen documents written during
that project and addressed explicitly to this one. Its README's first paragraph
says "LEGO Battles is the intended next target."

This is a digest, not a replacement. **Read the originals before doing the thing
each one covers.** What follows is the shape of the advice plus the parts that
have already been acted on here, so that they are not re-derived.

## The one-sentence version

> Nearly every blocker on that project was Ghidra silently losing information
> that the instructions still contained.

Dropped arguments, dropped return values, wrong symbol widths, reordered reads
with side effects. Each produced a bug that looked like something else — a
timing problem, memory corruption, a regression in whatever was edited last. The
fix was always to read the ARM.

**The habit: when the C does not explain the behaviour, disassemble before
forming a theory. It is faster than the theory.**

## The thirteen, and what each saves you from

| file | saves you from |
| --- | --- |
| `01-what-ghidra-silently-drops.md` | the single largest source of bugs, by a wide margin |
| `02-automation-and-the-no-hand-edits-rule.md` | losing your work every time you re-export |
| `03-token-and-session-economy.md` | spending effort by hand on what a script should answer |
| `04-bring-up-order.md` | months of debugging in the wrong order |
| `05-the-oracle.md` | arguing with yourself about what the hardware does |
| `06-ds-specific-gotchas.md` | DS details that differ from GBA and from intuition |
| `07-starting-a-new-ds-project.md` | what to copy, and the order to do day one in |
| `08-standing-a-ds-game-up-on-a-recompiler.md` | reimplementing an engine you could recompile |
| `09-the-process-end-to-end.md` | not knowing what the road looks like before starting |
| `10-finding-live-state-in-ram.md` | searching 4 MB with a filter that cannot work |
| `11-verifying-a-port-and-its-tools.md` | trusting a tool never checked against reality |
| `12-the-literal-pool-cross-reference.md` | reading decompiled C when you could search for who uses a value |

## Day-one order, and where this project stands

From `07-starting-a-new-ds-project.md`:

1. **Extract every section and find every relocated block.** ARM9 static, ITCM,
   DTCM, ARM7, and anything the loader copies. Do not proceed until every block
   is decompiled at its runtime address — unresolved externals are code you do
   not have, and finding that out in month two is expensive.
   → **ARM9 side done** (`docs/04-rom-layout.md`): three programs, all from
   `MODULE_PARAMS`. **ARM7 WRAM relocation not yet checked** — on LSW2 this was
   an undocumented block worth 62 million interpreted instructions.
2. **Check Ghidra headless scripting works** before relying on it. → not yet
   done.
3. **Run the retyping passes** while re-exporting is cheap: void returns first,
   then `DAT_` symbols used in arithmetic that resolve into MMIO. → pending.
4. **Freeze the closure.** Write the needed function set to a file and report
   against it from week one, so progress is never a feeling. → pending.
5. **Set up the build**: 32-bit `i686-w64-mingw32`, `-mregparm=3`, `-O0`,
   `-fpermissive`. Put the msys mingw32 bin directory on PATH or gcc fails
   confusingly. → toolchain not yet present, see `docs/01-inventory.md`.
6. **Get a savestate of the real game running**, plus a helper that turns "read
   address X" into a number. → pending.

## Rules already adopted here

- **The ROM is the source of truth; the emulator is the oracle.**
- **Never hand-edit generated output.**
- **Record negative results and retract in place.** Already exercised: `docs/05-the-comp-container.md` records the wrong 4 KB
  block assumption at the point it was wrong.
- **Export and commit before any bulk mutation.** Applies the moment Ghidra
  work starts. Running 2,027 coverage addresses through `CreateEntryPoints
  -Analyze` on LSW2 destroyed 275 existing functions and 8% of disassembly
  coverage *while the function count rose 31%* — an operation that improved the
  metric being watched and degraded one that was not. The committed export was
  the only undo.
- **Never put a pipe between a command and its exit code.** `cmd | head` then
  `$?` reports head's status. This turned two failed builds into apparent
  successes on LSW2.

## The traps ranked by what they cost, from `09`

1. **Applying a requirement at the wrong layer.** "No save system" is a content
   statement; implemented as `save_type = "none"`, a hardware fact, the game
   refused to boot.
2. **Bulk-creating functions in Ghidra.** See above.
3. **An instrument whose failure looks like a result.** A verification harness
   printed 32 confident divergences while the code under test never executed.
   *Before building an instrument, decide what its failure would look like and
   whether you could tell it from a real finding.*
4. **Chasing initialisation.** If you need a running machine, run one.
5. **Pipes eating exit codes.**
6. **The wrong compiler.** A `g++` with no C++ standard library still passes
   CMake's compiler check, which compiles a bare `main()`.

## DS hardware notes worth having in front of you

From `06-ds-specific-gotchas.md`, condensed:

- **Character base is four bits on the DS, two on the GBA.** A wrong base
  renders *other real data*, convincingly. "I can explain what I am seeing" is
  not evidence that it is right.
- **`IF` is write-one-to-clear**, and a wrong-*width* write leaves the flag set,
  so the handler re-enters immediately.
- **DMA triggers on the 0→1 edge of the enable bit, never on level.**
- **Reading `IPCFIFORECV` pops a word** — the canonical side-effecting read, and
  the one a decompiler reordering above its guard silently breaks.
- **Publish the whole firmware system region** (`0x027FF800`, `0x027FFC00`,
  `0x027FFC40`, `0x027FFE00`) from a savestate readout, not from the parts you
  think the game needs. Shipping one of two chip-ID copies boots and then halts
  as if the cartridge were swapped mid-play.
- **The ARM9 binary is compressed and the header lies about its size.**
  → already handled, `docs/04-rom-layout.md`.
- **Ghidra creates functions on data and tells you so.** 1,040 of LSW2's 5,860
  carried "Control flow encountered bad instruction data"; 731 of those were
  `size=4` string tables and pointer arrays. Two cheap filters caught 417 of 417.
  The residue matters more than the junk: the same warning on a *large* function
  means a silently truncated body.

## Tools that transfer essentially unchanged

Named in `07`; to be imported into `tools/` when their track starts.

| tool | what it does |
| --- | --- |
| `ghidra_scripts/FixVoidReturns.java` | retypes `void` functions whose ARM returns a value, and re-decompiles |
| `ghidra_scripts/ExportFunctionModes.java` | the authoritative ARM/Thumb export |
| `ghidra_scripts/ExportDataRanges.java` | initialised memory minus instruction coverage |
| `ghidra_scripts/DumpDataTypes.java` | what Ghidra actually believes about types |
| `gen_dat_perfile.py` | shapes `DAT_` symbols, address-of-vs-bitwise-and rule included |
| `gen_call_patches.py`, `gen_irq_patches.py` | recover dropped call arguments by dataflow, refusing when they cannot prove a value |
| `host_pc_to_function.py` | host program counters in a stuck run back to DS function names |
| `report_closure_progress.py` | progress against a frozen denominator |

## Where this game differs

The playbook was written against a game with a proprietary archive, proprietary
audio and four ARM9 programs. LEGO Battles has a solved container, stock Nitro
formats, a stock SDAT, a real NitroFS and three ARM9 programs. The asset track
is therefore much shorter here. The *code* track is not — 9,158 ARM9 functions
against LSW2's 5,860 — so the Ghidra and recompiler advice applies in full and
then some.
