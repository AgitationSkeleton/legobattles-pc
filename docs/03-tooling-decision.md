# Ghidra, ndsrecomp, or both

**Both, plus the matching decompilation — as three tracks that feed each other,
not three alternatives.** They answer different questions and the output of each
is an input to the next.

## The question, stated properly

"Ghidra or ndsrecomp" is a category error. Ghidra is an *analysis database*: it
produces addresses, instruction modes, data ranges, types and decompiled C.
ndsrecomp is an *execution engine*: given an image, a load address, an entry PC
and a list of function entry points with their ARM/Thumb mode, it lifts guest
ARM to native C and runs it. **The expensive input ndsrecomp needs is exactly
what Ghidra produces.**

The real question is what produces a *running, correct* LEGO Battles soonest,
and what makes the result durable. Those have different answers, which is why
there are three tracks.

## Track A — Ghidra: the corpus

Ghidra headless over the ARM9 static module, ITCM, DTCM, the ARM7, and all 91
overlays. What matters out of it is not the decompiled C:

- **function addresses and sizes**, seeded from the third-party dsd config
  (9,158 + 364 already catalogued) and extended by Ghidra's own analysis;
- **ARM/Thumb mode per function**, from Ghidra's `TMode` context register. Never
  from prologue heuristics: measured on LSW2, a reasonable heuristic was wrong
  4 times in 5,860, and every error was a Thumb function lifted as ARM, which is
  not subtly wrong but entirely wrong;
- **data ranges**, computed as initialised memory minus instruction coverage.
  Undeclared data gets decoded as code, and arbitrary bytes decode into
  legal-looking ARM — on LSW2 that produced 82,946 `swi` instructions in a game
  that issues almost none;
- **types and cross-references**, for the human work of understanding.

The LSW2 project's Ghidra scripts transfer essentially unchanged:
`ExportFunctionModes.java`, `ExportDataRanges.java`, `FixVoidReturns.java`,
`DumpDataTypes.java`, `CreateEntryPoints.java`.

**Cost note.** Ghidra's C output is a lossy rendering of the instructions, and
the losses — dropped arguments, dropped return values, wrong symbol widths,
reordered side-effecting reads — are silent and land exactly where a
recompilation breaks. This is the reason track B exists.

## Track B — ndsrecomp: the behaviour

Static recompilation removes the step that failed repeatedly on the previous
project: *translation*. A lifted function is the cartridge's own instruction
semantics compiled natively. It is not a person's reading of what the function
meant.

The decisive property is the failure mode. In a tiered recompiler, a function
that is missing or mistrusted **falls back to the interpreter — it runs slow,
not wrong.** A hand-translated function that is subtly wrong produces a
plausible incorrect answer, and three of the six classes of mistranslation that
cost the LSW2 project real time do exactly that.

LEGO Battles is well suited to it:

- **91 overlays, all tiny.** Combined they are a small fraction of the code; the
  ARM9 static module holds 0x150880 bytes and essentially all the logic. There
  is no large dynamic-overlay problem to model.
- **Only three ARM9 programs** — static, ITCM (`0x320` bytes), DTCM (`0x60`) —
  all described by `MODULE_PARAMS` and all already extracted. LSW2 needed four
  programs, two of which were undocumented and cost it a day each. That risk is
  already retired here for the ARM9; the ARM7 still needs the same check.
- **The ARM7 is uncompressed** and sits at `0x02380000`, `0x26F28` bytes.

Caveat, stated plainly: ndsrecomp is self-described **very early pre-alpha**,
with no compatibility promise and no turnkey build from a clean clone. It has
demonstrated firmware boot, SM64DS, Mario Kart DS with Wiimmfi online, and early
Metroid Prime Hunters gameplay — and LSW2 stood up on it in one session. That is
a real but narrow track record, and the plan does not assume it will work
without effort.

## Track C — the matching decompilation: the durability

`ds-decomp` + `mwccarm`, continuing the third-party project's configuration. Its
output is C that a person can read and change, and that provably compiles to the
cartridge's own bytes.

The mistake to avoid is doing this **first**. At the rate implied by the existing
snapshot — 45 named functions out of 9,522 — a hand decompilation of LEGO
Battles is a multi-year community effort, and a port gated on it never ships.

Done **second**, against a lifted build that already runs, it is a different and
much easier problem: the lifted build is an **executable specification**. A
hand-written function can be diffed against the lifted original automatically,
which is a luxury a from-scratch decompilation does not have. LSW2 reached the
same conclusion and named the mechanism (`NDS_HLE=verify`).

So: write readable C where it earns its place — where behaviour must *change*,
or where the code is central enough to be worth owning — and verify each one.

## How the tracks connect

```
      ROM
       |
       |  tools/nds_extract.py      (done: sections, overlays, NitroFS)
       v
   images + entry points
       |
       +--> Ghidra headless ----> function list + modes + data ranges
       |                                   |
       |                                   v
       |                          ndsrecomp program configs
       |                                   |
       |                                   v
       |                            lifted banks -> a running port
       |                                   |
       |                          runtime coverage of interpreter fallbacks
       |                                   |
       +<----------------------------------+  fed back as Ghidra seeds
       |
       +--> dsd/mwccarm ------> readable C, verified against the lifted build
```

The loop in the middle is the important part: playing the game reports every
address that fell through to the interpreter, those addresses go back to Ghidra
as *seeds* (never as declared entry points — a coverage address is often
mid-function), and what Ghidra confirms as a function start becomes a new bank
entry. On LSW2 that took ARM9 interpreter dispatches from 79,360 to 808.

## Where LEGO Battles is easier than LEGO Star Wars II

Worth stating, because it changes the schedule:

| | LSW2 | LEGO Battles |
| --- | --- | --- |
| asset container | a proprietary `rom.bin` offset table, decoded under emulation | `COMP`, a documented block-LZ11 wrapper — **solved, 2,178/2,178 files decode** |
| graphics formats | custom | stock Nitro: NCGR, NCLR, NCER, NANR, NSCR, NFTR |
| 3D formats | custom | stock Nitro: NSBMD, NSBCA — apicula reads them |
| audio | a proprietary ten-section container, reverse engineered from scratch | a stock `SDAT` archive of 60 MB, plus one `SADL` |
| filesystem | flat archive | a real NitroFS with 3,915 named files in 28 directories |
| ARM9 programs | four, two undocumented | three, all in `MODULE_PARAMS` |
| existing corpus | none | 9,522 functions with modes, from a third-party config |

The asset track, which on LSW2 was a large reverse-engineering project of its
own, is on this game mostly a matter of pointing existing tools at unwrapped
files.

## Decision

1. **Now** — extraction and formats (done for the container; continuing for the
   game's own `MAP`, `BPNZ`, `FANZ` and `LANG` files).
2. **Next** — Ghidra headless corpus, seeded from the dsd config, exported and
   **committed before any bulk mutation**. The export is the only undo there is.
3. **Then** — ndsrecomp program configs and a first boot, with runtime coverage
   feeding back.
4. **In parallel, opportunistically** — names and structures into the dsd config
   as they are learned, so track C has somewhere to accumulate.
5. **Later** — readable C where it earns its place, verified against the lifted
   build.
