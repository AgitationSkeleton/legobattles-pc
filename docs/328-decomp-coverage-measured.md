# 328 — What fraction of LEGO Battles is actually decompiled

Status: **measured, for the first time.** Every "percent decompiled" figure this
project has used came from counting rows in `docs/data/decomp_status.tsv`, which
is a list of what has been done with **no denominator attached**.

## The denominator

`tools/ghidra/LBInventory.java`, run headless against the ARM9 static section:

    6903 functions, 1013373 bytes of function body

Excluding thunks and externals: **6,838 real functions, 1,012,921 bytes**.

## The numerator

`decomp_status.tsv`, rows with `result == MATCH`: **789 functions, 3,380
bytes**.

## The ratio

| measure | value |
|---|---|
| **by code volume** | **0.33%** |
| by function count | 11.5% |

The two differ by 35x because of what was picked:

| | decompiled | whole ARM9 |
|---|---|---|
| mean function size | **4.3 bytes** | 148 bytes |
| median | **4 bytes** | 68 bytes |
| max | **12 bytes** | 8,780 bytes |

**The largest function decompiled so far is 12 bytes.** A 12-byte ARM function
is three instructions — a getter. Not one function of substance has been done,
and the 11.5% figure is entirely an artifact of counting getters as equal to a
simulation step.

## Where the work actually is

    top  5% of functions (  341) hold 32.8% of the code
    top 10% of functions (  683) hold 48.2% of the code
    top 25% of functions ( 1709) hold 71.6% of the code
    top 50% of functions ( 3419) hold 88.9% of the code

    <= 16 bytes:  784 functions (11.5%) --  0.8% of the code
    <= 64 bytes: 3342 functions (48.9%) -- 10.6% of the code

Half the binary lives in about 683 functions. Half the *functions* are under 64
bytes and together are a tenth of the code. So the remaining work is not 6,000
evenly sized units; it is a few hundred substantial ones and a long tail of
accessors, and the accessors are what has been done.

## What this means for a native port

A native PC port means reimplementing the ~1.01 MB of ARM9 function body plus
92 overlays (268 KB) — the simulation, pathfinding, AI, UI, audio and save
format — from a starting point of 0.33% by volume and **zero** functions above
12 bytes.

The pivot stands with that assessment in view. Recorded here so the scale is not rediscovered later: this is a rewrite of the
game, not a continuation of the port, and the recomp path that currently boots
and plays is what is being set aside to do it.

## Method note

Percentages by function count are close to meaningless on this binary and should
not be quoted. Size distribution is skewed hard enough (median 68, max 8,780)
that any progress figure has to be **by bytes**, and ideally weighted toward the
683 functions holding half the code.
