# The Ghidra decompiler corpus — what it is, and what it is not

## Where it lives, and why it is not in this repository

    <RETools>\decomp_all\     6,838 files, ~19 MB, one per ARM9 function

(`<RETools>` is wherever you keep your Ghidra install, project and scripts.)

**It is deliberately NOT committed.** These files are the cartridge's own code
translated to C. Committing them would redistribute the game, for the same
reason `port/generated/` is gitignored. Only derived metadata belongs here —
`arm9_inventory.tsv`, `subsystem_map.tsv` and the like.

Regenerate it with:

    $env:JAVA_HOME="<RETools>\jdk21\jdk-21.0.12.1+1"
    $env:LB_TARGETS="<RETools>\targets_all.txt"
    $env:LB_OUTDIR="<RETools>\decomp_all"
    analyzeHeadless.bat <RETools>\ghidra_proj LB `
      -process section_static_02000000.bin -noanalysis `
      -scriptPath <RETools>\ghidra_scripts -postScript LBDecomp.java

About thirteen minutes for all 6,838. `targets_all.txt` is built from
`arm9_inventory.tsv`, largest function first, so a partial run covers the code
that matters most.

## WHAT THIS IS NOT

It is **not a decompilation of LEGO Battles**. It is what Ghidra's automatic
decompiler prints. Concretely, the largest function in the game begins:

    void FUN_0202e624(int param_1,int param_2,undefined4 param_3)
    {
      char cVar1;
      undefined1 auStack_830 [12];
      undefined1 auStack_824 [12];
      ...

* every one of the 6,838 functions is named `FUN_<address>`;
* types are `undefined1` / `undefined4` / bare `int` standing in for structs
  and pointers;
* an 8,780-byte function arrives as a wall of stack arrays;
* **it does not compile, and it is not known to be correct.**

That last point is not theoretical. The header on every file points at
`playbook/01-what-ghidra-silently-drops.md`, which records Ghidra dropping call
arguments at **169 of 170 call sites** for one function this project checked.
Treat the C as a strong hint about structure and never as ground truth; read
the disassembly before writing anything against it.

The measured decompilation figure remains **0.33% by code volume** (docs/328),
and this corpus does not change it. Extraction was the cheap part.

## The inventory

`arm9_inventory.tsv` — one row per function, from `tools/ghidra/LBInventory.java`:

| column | meaning |
|---|---|
| address | entry point |
| name | Ghidra's name (`FUN_…` unless someone has renamed it) |
| size | bytes in the function body |
| thunk / external | 1 if so; both are excluded from coverage denominators |
| callers / called | call-graph degree, for finding leaves and hubs |

Totals: 6,903 functions, 1,013,373 bytes; **6,838 real, 1,012,921 bytes** after
thunks and externals.

Shape of the work, which matters more than the count:

    top  5% of functions (  341) hold 32.8% of the code
    top 10% of functions (  683) hold 48.2% of the code
    top 25% of functions ( 1709) hold 71.6% of the code

    <= 16 bytes:  784 functions (11.5%) --  0.8% of the code
    <= 64 bytes: 3342 functions (48.9%) -- 10.6% of the code

Half the binary is in roughly 683 functions. Half the *functions* are under 64
bytes and are a tenth of the code — and those are the ones already done, which
is why 11.5% by count and 0.33% by volume are both true and only one is useful.

## The RTTI map

`rtti_classes.tsv` (517 rows) and `vtable_methods.tsv` (3,811 rows), from
`tools/lb_rtti.py`. The ARM9 shipped its Itanium-ABI RTTI, so these are the
original developers' own class names, not inference — 513 game classes, and
2,466 functions named `Class::virtual_NN` covering 15.54% of the 1,012,921
bytes. Unlike the decompiler corpus this *is* ground truth about names, though
only class names: RTTI carries no method names, so the slot index is positional.

`docs/data/rtti_summary.md` has the verified byte layout, the four layout traps
(Thumb bit-0 slots, null-encoded pure virtuals mid-vtable, a vtable that runs
into a string table, the bogus "1,023 vtables" figure), and the validation.

## Quoting progress from this

Use **bytes**, not function count. The size distribution (median 68, max 8,780)
is skewed hard enough that a count-based percentage flatters trivial work by a
factor of about 35.
