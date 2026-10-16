# Labelling the ARM7's unreached code

**Status: the ARM7's unreached 28% is no longer unlabelled. 43% of it sits in a
run that names its own hardware, and the dominant subsystem is Wi-Fi.**
`docs/data/arm7_regions.tsv` carries every run with its subsystem fingerprint.

`docs/33` showed the ARM7 plateaus at ~70% under every scanning technique
available, and argued the remainder is linked-in but unused NitroSDK. That set
the next job: not *reaching* the code, but saying what it is.

## The obvious method does not work here

NitroSDK builds usually carry assertion strings — file names, function names,
`"%s:%d"` formats — and a library can be identified from them in minutes.

**This ARM7 has four printable strings in the entire image.** Assertions were
compiled out. There is no text to label anything with, and that is worth
recording as a negative result: the first idea was not merely unsuccessful, it
is unavailable, and no amount of effort on it would pay.

## What does work: a driver has to name its own hardware

An ARM7 subsystem is defined by the memory-mapped registers it touches, and
those are unambiguous and publicly documented — `0x04000400` is sound,
`0x040001C0` is the SPI bus, `0x04800000` is Wi-Fi. A Wi-Fi driver cannot avoid
mentioning `0x048xxxxx`.

So each unreached run is fingerprinted by the registers it uses:

```
subsystems LOADED by reached code  : wifi x59, SPI x18, IPC x13, IRQ x11,
                                     sound x10, timers x9, cartridge x5,
                                     video x4, power x4, keypad x2
subsystems LOADED by unreached code: wifi x44, SPI x10, cartridge x6,
                                     keypad x6, IRQ x4, timers x4, video x2
```

The reached side is exactly what an ARM7 should look like — the full driver set,
sound and IPC and touch — which is the sanity check that the method is sound
before its answers are trusted.

**8,220 of 19,232 unreached bytes (43%) are in a run that names its own
hardware.**

| range | bytes | subsystem |
| --- | ---: | --- |
| `0x037F8DDC..0x037F92DC` | 1,280 | **wifi** ×19 |
| `0x037FAEBC..0x037FB32C` | 1,136 | **wifi** ×22 |
| `0x038043A0..0x038048A4` | 1,284 | SPI ×10, keypad ×3 |
| `0x0380539C..0x0380593C` | 1,440 | timers ×4 |
| `0x03802970..0x03802F9C` | 1,580 | cartridge ×2 |
| `0x03802728..0x03802924` | 508 | cartridge ×4 |
| `0x03804F18..0x038050FC` | 484 | IRQ ×3, keypad ×3 |
| `0x037FB3AC..0x037FB488` | 220 | wifi ×3, IRQ ×1 |

**Wi-Fi is the single largest identifiable thing in the dead region**, 44 of the
76 register loads. The game has multiplayer, so the wireless stack is linked;
that a large part of it is never reached fits a title using local wireless while
the SDK's full library — infrastructure mode, the DWC/WFC paths — comes along
unused.

The remaining 57% loads **no registers at all**. That is not a failure of the
method: code that touches no hardware is not a driver. Runs of 1,248, 1,180 and
888 bytes with no register references are pure computation, and the obvious
candidates next to a wireless stack are its cryptography and checksums. That is
a hypothesis and is recorded as one.

## The bug that had to be fixed first

The first version of this analysis produced an absurd answer, and how it was
absurd is the useful part.

It scanned each unreached run for **words in the range** of a hardware register
and credited them to that run. The result:

```
across ALL unreached runs: wifi 80, SPI 18, IRQ 12, timers 10, sound 8, ...
across all REACHED code  : timers 1, IPC 1
```

**The reached code appeared to touch two hardware registers in total.** For an
ARM7 — the core that owns sound, touch, power and wireless — that is impossible,
which is what made it visible.

The cause is a pattern this project has now hit three times: **a reached
function keeps its literal pool in the bytes after it, and the walk reports
those bytes as a gap.** The register constants belonged to the reached drivers
and were being credited to the unreached regions purely because of where they
sit.

The fix is to attribute a register to the code that **loads** it — disassemble
the run, follow its own PC-relative loads, read what they point at. *A constant
sitting in a region proves nothing about that region; a constant the region
loads is that region's business.*

Both figures moved once corrected: unreached Wi-Fi from 80 to 44, and reached
hardware from 2 references to 145.

## What this does and does not change

It does **not** move the coverage number. The ARM7's wram is still **71.5% of
code** reached, because labelling code is not reaching it.

What it changes is the standing of the remaining 28%. It was "unreached, and
`docs/33` thinks it is dead SDK". It is now "unreached, 43% of it attributable
to a named subsystem, dominated by the wireless stack" — with a per-run table
anyone can check. For the port scope in the founding directive that is the
useful form: a port reimplements subsystems, and knowing the dead weight is
mostly Wi-Fi says the ARM7 side is smaller than 62 KB suggests.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| ARM7 wram | 71.5% | at its ceiling; remainder now labelled |
| overlays, code | 52.9% | |
| ARM9 annotated | 26.8% | |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Next by the ordering is the overlays at 52.9%. Unlike the ARM7's, that figure
has room: `docs/31` seeded the overlay scan only from each overlay's
`static_init` table, and the missing half is very likely reachable with better
seeds rather than genuinely dead.
