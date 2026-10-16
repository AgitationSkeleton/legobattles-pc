# 322 — The framerate drop is ITCM being interpreted

The framerate defect is 39-45 fps in play, and docs/315 had it OPEN with the
note that the harness runs then sat at 53-60 and that was "a different route
and not an A/B". Two runs since then read **19.2 fps**, twice, to three significant
figures — which is not variance, and is measurable without anyone playing.

## The same number twice

```
audit1   19.20 fps   emu_ticks 49.589 ms/frame
band25   19.21 fps   emu_ticks 49.5   ms/frame
hud2     58.75 fps   emu_ticks 14.539 ms/frame
```

Every other stage is identical between them — `present 2.3`, `adaptive 1.3`,
`upload 0.12`, `draw 0.11`, `swap 0.43` ms — so the cost is entirely the
emulation thread, not rendering, not the compositor, not the harness.

## Tier 3, and where

| run | fps | tier3 hits |
|---|---|---|
| audit1 | 19.2 | **4,555,793** |
| band25 | 19.2 | **4,423,831** |
| hud2 | 58.8 | 712,016 |
| band33/34/35/28 | 57-58 | ~270,000 |

The slow runs interpret about fifteen times as many instructions. And the
coverage says where, with no ambiguity at all:

```
arm9  0x01FF80F8  kind 1  caller 0x02111DB4   1408596  30.92%
arm9  0x01FF8110  kind 1  caller 0x02111DB4    712976  15.65%
arm9  0x01FF80F0  kind 1  caller 0x02111DB4    635198  13.94%
arm9  0x01FF8100  kind 1  caller 0x02111DB4    438818   9.63%
arm9  0x01FF810C  kind 1  caller 0x02111DB4    392930   8.62%
arm9  0x01FF80E8  kind 1  caller 0x02111DB4    233587   5.13%
arm9  0x01FF8104  kind 1  caller 0x02111DB4    219402   4.82%
arm9  0x01FF80F4  kind 1  caller 0x02111DB4    116795   2.56%
```

**91.3% of the slow run's interpreter hits — 4.16 million — in forty bytes**,
all `kind 1`, which is ROOT: a genuine native-to-interpreter boundary rather
than a branch target inside the interpreter (the distinction this project got
wrong once already, and the caller column is `R[14]`, not a call site).

`0x01FF8000` is the ARM9 **ITCM** mirror. Those addresses are inside
`func_01FF8068` (0x17C bytes, ARM), and the caller `0x02111DB4` is inside
`func_02111D5C`. The fast run's coverage contains **none** of these.

ITCM is where a DS game puts its hottest inner loops precisely so they run from
fast memory. Interpreting one is the worst case available.

## What it is NOT

**Not a missing lift.** `port/config/lb_arm9_itcm.toml` exists, `port/lift.sh`
processes it, `port/generated/lb_arm9_itcm_dispatch.c` holds 199 entries
covering `0x01FF8000..0x01FF8318`, and `title_bank_registry.cpp:206` registers
it:

```cpp
nds_register_dispatch(NDS_ARM9, g_dispatch_lb_arm9_itcm, g_dispatch_lb_arm9_itcm_len, 0xFFFF0000u);
```

**Not byte validation.** The obvious theory is that ITCM is written at runtime
so the lifted bytes stop matching and the validation gate refuses them. The
counters say otherwise:

| | slow | fast |
|---|---|---|
| `cache_hit_absent` | **693,087,722** | 5,112,047 |
| `static_reject_bytes` | 42,440 | **72,190** |

The slow run has 135x the absent dispatches and **fewer** byte rejections than
the fast one. Whatever makes those addresses absent, it is not the gate that
counts byte mismatches.

## What is established

**ESTABLISHED:** two runs at 19.2 fps against six at 57-59, with the whole
difference in `emu_ticks`.

**ESTABLISHED:** the slow runs interpret 4.5M instructions against 0.27M, and
91.3% of that is forty bytes of ITCM inside `func_01FF8068`, entered from
`func_02111D5C`.

**ESTABLISHED:** `cache_hit_absent` is 693M against 5.1M — the dispatch cache
is answering "no lifted function here" hundreds of millions of times.

**ESTABLISHED:** the ITCM bank is lifted, emitted and registered, and byte
validation is not the discriminator.

**ESTABLISHED (control):** `cache_absent_itcm` now counts absent dispatches in
`0x01FF8000..0x01FFFFFF` at the point of the decision, so the claim no longer
rests on two instruments agreeing. A **fast** run reads

```
cache_hit_absent 2,025,409     cache_absent_itcm 0     57.3 fps
```

Zero. That is the control the argument needed and did not have: when the port
runs at full speed, ITCM is not being interpreted at all.

**ESTABLISHED (confirmation):** four runs of the same route, same binary:

| run | fps | `cache_hit_absent` | of which ITCM | share |
|---|---|---|---|---|
| hunt1 | 19.11 | 1,328,644,872 | 1,325,489,342 | **99.8%** |
| hunt2 | 18.86 | 423,714,154 | 420,308,538 | **99.2%** |
| hunt3 | **57.50** | 4,649,587 | **0** | 0% |
| hunt4 | 17.31 | 109,733,141 | 106,179,731 | **96.8%** |

`cache_absent_itcm_pc` reads **0x01FF80E8** in all three slow runs — the exact
address the tier-3 coverage named, arrived at independently. The fast run has
none. Counted at the point of the decision, not inferred.

## It is not the footprint band harvest

Three slow runs in four is a far higher rate than the 2-in-14 seen earlier, and
that jump overlaps `LB_FPBANDS` going on by default. Correlation is not cause
and this project has been wrong about exactly that before (docs/315 lists five
such), so it is worth stating what breaks it: the fps history on disk has

```
exp-fx1   19.60 fps   05:04
exp-fx4   19.43 fps   05:28
```

Both hours before any of the band work existed, and on the `--fxband` route
rather than `--build-flow`. **The slowdown predates the feature and spans
routes.** An A/B with the flag forced off is running for rigour, but the
timeline already rules out the regression reading.

**NOT ESTABLISHED:** why those addresses resolve to absent when they do, and
the counters cannot say. The slow run has **fewer** byte rejections than the
fast one — 47,875 against 66,918 — and its `static_reject_first_pc` is
`0x02041F38`, not ITCM at all. So the refusal is not showing up as a rejection,
and `cache_slow_lookup` is lower in the slow run (1.06M against 1.56M), which
fits a single missed lookup being cached as absent and then hit hundreds of
millions of times.

`candidate_count` is the discriminator, and it answered:

```
[lb_itcm] REJECTED pc=01FF8068 range 01FF8068+380 prov=1 first differing byte at +151 (expected EA)
[lb_itcm] MISS     pc=01FF8068 thumb=0 candidates=1 inactive=1        (19.3 fps run)
```

**`candidates=1 inactive=1`.** The entry was matched and the validation gate
**refused** it. Not a registration problem, not a range problem, not the bank
ordering — all three are now ruled out by measurement rather than by reading
the registry.

`prov=1`, so provenance passed and the refusal is byte inequality. The first
differing byte is at **+0x97**, which is address **0x01FF80FF** — the top byte
of the word at `0x01FF80FC`, and `0xEA` is the opcode byte of an ARM `b`. The
disassembly puts a branch exactly there:

```asm
01ff80f8  bne  #0x1ff8100
01ff80fc  b    #0x1ff8104     <- the expected 0xEA
01ff8100  cmp  r0, #1
```

So the live ITCM does not hold the instruction the lift captured, **inside the
hot loop itself**. The gate is doing its job: it is refusing to run native code
for bytes the guest no longer has. That also explains the intermittency
directly — runs where ITCM happens to match read `cache_absent_itcm = 0` and
57 fps, and runs where it does not read 17-19.

## What the live bytes are: a different register list

A second slow run, on the `--fxband` route, rejected a *different* ITCM
function and printed the words either side:

```
[lb_itcm] REJECTED pc=01FF8000 range 01FF8000+104 prov=1 first differing byte at +0
[lb_itcm]   01FF8000 live E92D401B expected E92D4000  <-- DIFFERS
[lb_itcm]   01FF8004 live E3A0C301 expected E3A0C301
[lb_itcm]   01FF8008 live E28CCE21 expected E28CCE21
[lb_itcm]   01FF800C live E51C1008 expected E51C1008
[lb_itcm]   01FF8010 live E3510000 expected E3510000
[lb_itcm]   01FF8014 live 0A000000 expected 0A000000
[lb_itcm]   01FF8018 live EA000000 expected EA000000
[lb_itcm]   01FF801C live E8BD8000 expected E8BD8000
```

`E92D4000` is `push {lr}`. `E92D401B` is `push {r0, r1, r3, r4, lr}`. **Only
the register-list halfword differs**; all seven other words are identical. And
the words after it — `mov ip, #0x4000000` / `add ip, ip, #0x210` /
`ldr r1, [ip, #-8]`, which is IME at `0x04000208` — identify this as the ITCM
**IRQ dispatcher**.

The earlier rejection was a different function (`0x01FF8068`, the scheduler) at
a different offset (+0x97, a branch opcode byte). So more than one ITCM
function diverges, and the divergences are small and instruction-shaped rather
than arbitrary.

## RETRACTED: it is not two byte images

The section below argued that the guest keeps a second ITCM image and the fix
was to lift it as another content-validated variant. **That is wrong.** A full
0x320-byte dump at the moment of rejection settles it:

```
live words captured: 200
differing words: 3 of 200
  01FF8000  live E92D4067  expected E92D4000
  01FF80FC  live FF000000  expected EA000000
  01FF8100  live E35000FF  expected E3500001
```

Three words. And the first of them read **E92D401B** in the previous slow run
and **E92D4067** in this one — *different values at the same address on
different runs*. A variant you cannot predict is not a variant, and nothing
can be lifted from it.

As bytes the changes are at offsets **0x00**, **0xFF** and **0x100** — the last
two adjacent, a two-byte write of `FF FF` straddling a 256-byte boundary, and
a single `00 -> 67` at the section base. `FF000000` is not a valid ARM
instruction. These are not alternative encodings of the same routine; they look
like a handful of bytes being corrupted.

So the correct reading is: **the validation gate is doing exactly its job.** It
refuses to run native code for bytes the guest no longer has, and the bytes it
is refusing over are damaged rather than different. The ~3x frame-rate loss is
the *consequence* of that refusal, and the bug is whatever writes those three
places.

Two candidates, and this page does not choose between them:

1. something in the guest writes into ITCM — a stray store, a DMA with a wrong
   destination, a bug in the port's ITCM write path;
2. the port's ITCM **read** path is wrong, so both the gate and the dump see
   bytes that are not really there. The gate and the probe share
   `bus_live_bytes_equal`/`bus_read_u32`, so they would agree while both being
   wrong — which is exactly the trap of two instruments agreeing.

(2) is testable first and cheaply: read the same addresses through the debug
server's `read_mem`, which is a different path, and see whether it agrees.
**Do that before chasing a writer.** 197 of 200 words matching argues the read
path is basically sound, but "basically sound" is what a partially wrong
mapping looks like.

## The reading that led there, kept for the record

Both states genuinely occur: a fast run reads `cache_absent_itcm = 0`, which
means the lifted ITCM validated and ran natively, so the live image matched the
capture. A slow run reads 1.3 billion. So the guest **writes its own ITCM at
some point during a run**, and whether it has done so before the first ITCM
dispatch decides whether the whole session runs the scheduler and IRQ
dispatcher natively or interprets them.

That makes the validation gate correct and the lift incomplete: only one of the
two byte images was captured. The mechanism for the other one already exists —
`lookup_in` walks every entry sharing an address and takes the one whose
validation matches, which is exactly how a second content-validated variant
would be selected.

**NOT ESTABLISHED:** that there are only two images, or that the second is
stable. A full 0x320-byte dump at the moment of rejection is being captured so
the live image can be diffed against
`out/eur/arm9/section_itcm_01ff8000.bin` offline; how many bytes differ in
total, and whether two slow runs produce the same image, is what decides
whether a second variant is the fix or only the first half of one.

**What this changes for a play session:** both instruments are cumulative,
cost two compares on a path only taken when a lookup has already failed, and
are in the deployed build. A session that feels slow will now say whether ITCM
went absent, and from which pc, without anyone having to reproduce it here.

**NOT ESTABLISHED:** that this is the 39-45 fps seen in play. That is a range
this port has since exceeded on most routes; what is shown here is a mechanism
that drops it to 19, which is worse than anything seen in play. It may be the
same cause at a milder severity, and that is a guess.
