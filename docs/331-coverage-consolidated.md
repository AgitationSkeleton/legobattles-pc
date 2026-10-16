# 331 — The coverage picture, consolidated

Status: **the first complete accounting.** docs/328 measured the ARM9
denominator, the RTTI extraction named a sixth of it, and the overlay survey
closed the last unmeasured region. This supersedes every earlier percentage.

## The whole game, in bytes

| region | bytes | note |
|---|---:|---|
| ARM9 function bodies | **1,012,921** | 6,838 real functions, thunks and externals excluded |
| overlay code | **82,970** | 91 overlays, 88.2% of their 94,048 bytes |
| **total code** | **1,095,891** | |

Everything below is a fraction of that 1,095,891.

## What we have

| measure | bytes | % of total code | what it means |
|---|---:|---:|---|
| **byte-matched decompilation** | 3,380 | **0.31%** | compiles to the ROM's own bytes |
| RTTI-named functions | 157,378 | 14.36% | ground-truth *class* identity, not source |
| subsystem-classified (top 200) | 240,816 | 21.97% | knows which system it belongs to |
| Ghidra decompiler output | 1,012,921 | 92.4% | raw automatic C, does not compile |

**These four are not stages of one pipeline and must not be added.** They
overlap and they measure different things:

* *Decompiled* means a human wrote C that compiles to the original bytes. 789
  functions, none larger than 12 bytes. That is the only column that means the
  code is understood well enough to reimplement.
* *Named* means the ROM told us the class. It is free, it is ground truth, and
  it says nothing about what the function does. 2,466 functions.
* *Classified* means somebody read it and decided which subsystem it is in.
* *Decompiler output exists* means Ghidra printed something. It is raw
  material.

## The honest headline

**0.31% of LEGO Battles is decompiled.** The other figures describe how well
the remaining 99.69% is *mapped*, which is real progress and is not the same
thing.

A useful way to hold it: the map is now good — a sixth of the binary has the
developers' own class names, a fifth has a subsystem, and every mission script
is identified down to its overlay id. The territory is almost entirely
unwalked.

## What the day's measurements corrected

Five figures in circulation were wrong, and all five were quoted confidently:

| claim | actual | how it went wrong |
|---|---|---|
| "11.5% decompiled" | 0.31% | function COUNT on a distribution where the done ones average 4.3 bytes and the binary averages 148 |
| "1,023 vtables" | 515 | a line count of `512 typeinfo_ + 511 vtable_` symbol lines |
| "~268 KB of overlays" | 94,048 bytes | `du -sh` disk usage, 4 KB block rounding over 91 small files |
| "mission logic is not in the ARM9" | it is | 700 ARM9 functions / 25,944 bytes of mission classes; 0 of 90 campaign vtable slots point into overlay space |
| "the overlays are unexamined" | they were examined | lb_overlays.py, docs/06, docs/82 and a recompiler lift already covered them |

Four of the five inflated progress or scope. The pattern is the project's own
recorded lesson applied to itself: **a number is measured or it is unknown**,
and a count is not a measurement when the units differ by 35x.

## What a native port would still require

The 0.31% is the number that matters for a reimplementation, and the ceiling on
it is not effort — it is the toolchain. `tools/lb_match.py` only counts a
function as done when GCC reproduces the ROM's bytes, and the ROM was built
with **mwccarm**, which is not on this machine. That is why every decompiled
function is an accessor: three instructions have one sensible encoding, and
anything larger does not.

For a native PC port byte-matching is not required — but then `MATCH` stops
being a progress metric and the project has no other one. Replacing it means
differential testing against a reference implementation, and the recomp port is
that reference. See docs/329.
