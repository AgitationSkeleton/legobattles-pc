# 326 — The ITCM corruption is two bytes, and LB_WATCH could never have found it

Status: **mechanism narrowed, writer not yet named.** Continues docs/322 and
docs/323. Two theories are retired here, both by measurement.

> **2026-09-25, docs/369: the FF FF pair has a writer, and it does not
> recover.** The null-page idea below was right. `LB_NULLPAGE=1` named the
> store: `func_02087350` writes a null handle (FF FF) back through a handle
> pointer of `0x000000FF`. That pointer was handed over by the port's old
> projectile override, re-entered after a slice preemption. The two bytes are
> two byte stores, not one unaligned `STRH`. In the crashing play sessions
> the corruption was followed by wild accesses and a dead game, so "always
> recovers ... not a defect, it is a cost" (below) does not hold for FF FF.
> The later **00 00** episodes that this page merged into a "copy caught in
> progress" were not re-examined. They may still be what it says.

## Why this outranks the renderer phases

Every remaining phase of docs/325 ends in a performance claim, and no such claim
survives a run where the ITCM function is refused: the fallback is the
interpreter, and docs/322 measured `cache_absent_itcm = 523,530,086` on the run
that read 19.12 fps. The state fires in roughly one run in four, so a single
measurement cannot be trusted without reading that counter beside it.

Clean runs, all with `cache_absent_itcm = 0`:

| run | fps |
|---|---|
| itcmp1 | 51.16 |
| itcmp2 | 52.24 |
| p3a | 51.62 |

That is the honest baseline band: **51–52 fps**, not 57 and not 19.

## RETRACTED: the port's ITCM read path

docs/323 proposed that the validation gate and the dump agree because they share
`bus_read_u32`, and that the real bytes might be fine. The probe now prints both
paths side by side, `bus_read_u32` against the debug server's independent
`bus_debug_read8`:

    [lb_itcm] 01FF80FC bus_read_u32 FF000000 bus_debug_read8 FF000000
              agree (the bytes really differ)

**The read path is not the bug.** Something wrote those bytes, and a writer has
to be found. This retires the whole "no writer needs chasing" branch.

## RETRACTED: "3 of 200 words, different garbage per run"

docs/322 described the difference as scattered. Caught properly it is not. Two
words differ and each differs by exactly one byte, and the two bytes are
**adjacent**:

    01FF80FC  live FF000000  expected EA000000
    01FF8100  live E35000FF  expected E3500001

| address | expected | live |
|---|---|---|
| `0x01FF80FF` | `EA` | `FF` |
| `0x01FF8100` | `01` | `FF` |

The gate agrees: *"first differing byte at +151"*, and `0x01FF8068 + 0x97` is
`0x01FF80FF`.

So this is **one 16-bit write of `0xFFFF` to the unaligned address
`0x01FF80FF`**, not scattered corruption.

## What the bytes are

The dump shows the region is the IRQ/exception dispatcher: `MOV R3,#0xD2` /
`MSR CPSR_c,R3` (IRQ mode), `MOV R3,#0xD3` (SVC), `LDR PC,[SP],#4` (exception
return). The corrupt pair sits inside a linked-list walk:

    80E0  ADD  R2, R12, #8
    80E4  LDR  R1, [R2]
    80E8  CMP  R1, #0
    ...
    80F4  LDRH R0, [R1, #0x64]
    80FC  B +0          -> FF000000      (undefined instruction)
    8100  CMP  R0, #1   -> CMP R0, #0xFF
    810C  LDR  R1, [R1, #0x68]
    8110  BNE  80E8                      (loop)

`FF000000` is not a valid ARM encoding, so this is not the game patching its own
code on purpose.

## The strongest candidate: a near-null pointer, mirrored into ITCM

`nds_busf_classify` (runtime_arm.h) tests ITCM **first** for every address below
`0x02000000`, and ITCM is mirrored every 32 KB:

    if (addr < g_busf_itcm_limit) { *off = addr & g_busf_itcm.mask; ... }

`0x01FF80FF & 0x7FFF` is `0xFF`. So a store to **`0x000000FF`** — a null struct
pointer plus offset `0xFF` — lands on exactly these bytes. That is the shape of
the corruption: a 16-bit `0xFFFF` store through a pointer that should not have
been null.

Not yet established: whether the guest address really is near-null, or whether
the lifted `STRH` is passing an address the hardware would have aligned. Neither
path forces alignment — `bus_write_u16_slow` does `resolve(addr, 2)` and
memcpy's at `addr`, and the fast path does the same — so an unaligned `STRH`
writes across the boundary in this port where ARM9 hardware ignores bit 0.
**This is the next thing to settle, and it decides the fix.**

## Why LB_WATCH found nothing, which was never explained

docs/323 recorded one `LB_WATCH` run that produced no lines at all and flagged
the silence as unexplained. It is explained now, and it is structural.

`bus_read_*` and `bus_write_*` in `recompiler/armv4t/runtime_arm.h` are inline
fast paths:

    static inline void bus_write_u16(uint32_t addr, uint16_t val) {
        if (!g_runtime_deep_trace) {
            ... memcpy(w->data + off, &val, 2); nds_busf_note_write(...); return;
        }
        bus_write_u16_slow(addr, val);
    }

`watch_addr` is called only from the **slow** path in bus.cpp. Every write the
fast window serves — which is every ITCM write — is invisible to a watchpoint.

**No amount of `LB_WATCH` tuning can find this writer.** Runs spent on it are
wasted. `g_runtime_deep_trace` forces the slow path, so a watch can only see
these writes while deep trace is on.

## Next step

Probe inside the fast path, where the write actually happens, via
`apply_hooks.py` (runtime_arm.h is read-only ndsrecomp). In the ITCM branch of
`bus_write_u8/u16/u32`, when the offset lands in `0xF0..0x110`, print the guest
address, the value, the width and `R[15]`. Gate it on a global that defaults off
so the deployed build pays one predicted branch.

That names the writing PC in one run, which neither a watchpoint nor a byte
comparison can do.

## The probe is built, and it appears to suppress the bug

`LB_ITCMW=1` arms a check in `runtime_note_code_write`. It works, but it is
**expensive when armed** — two memory touches on every guest RAM write:

| run | fps | `cache_absent_itcm` | rejections |
|---|---|---|---|
| itcmw3 | 25.15 | 0 | 0 |
| itcmw4 | 37.79 | 0 | 0 |
| itcmw5 | 23.20 | 0 | 0 |

Against a 51–52 fps baseline. Three complete runs, **no rejection at all**,
where the unarmed builds hit one in itcmp2 — roughly one run in four.

Four complete armed runs, no rejection (itcmw6 also clean at 41.02 fps). Not
proof on its own (p ≈ 0.32 at a 1-in-4 rate), but together with the frame rate
it was a fair suspicion that the probe changes the timing that produces the
write.

**Confirmed by removing it.** The probe is now compile-time gated
(`#ifdef LB_ITCMW_PROBE`), and the very next pair of runs on the unprobed
binary produced:

    q1   fps=36.19   cache_absent_itcm = 0
    q2   fps=15.10   cache_absent_itcm = 266,145,829   <-- fired

So the bug is alive and the instrument was suppressing it. **Any future probe
for this must not sit on the per-write path.**

The gate is compile-time rather than runtime for a second measured reason: a
build carrying only the *disarmed* `if (g_lb_itcmw)` was suspected of costing
frame rate, and rather than argue about a branch, the shipped build now carries
nothing at all. Hunt with `-DLB_ITCMW_PROBE`.

## REFRAMED: this is not a stray write, it is a COPY caught in progress

The cheap watch (`LB_ITCMWATCH=1`, two byte reads a frame, dumping the bus
access ring on first change) caught it on the **first run**, at 40.19 fps with
`cache_absent_itcm = 21,337,946`:

    [lb_itcmwatch] 01FF80FF/0100 became 00 00 (expected EA 01)

**`00 00`, not `FF FF`.** docs/326 recorded `FF` at both bytes; this run has
`00` at both. The corrupt value is not constant, which a stray 16-bit store of a
fixed value cannot explain — and "different garbage per run" was already noted
in docs/322 and wrongly retracted as a variant theory.

What does explain it: the game **re-copies ITCM** (an overlay swap), and both
the watch and the validation gate can observe the region *part-way through the
copy*. Mid-copy you see whatever has landed so far — zeros ahead of the write
cursor in one run, a different partial state in another.

That fits every symptom:

* **the value varies** — it is copy progress, not a constant;
* **one run in four** — it depends on where the frame boundary falls in the
  copy, which is exactly the kind of timing the per-write probe destroyed;
* **the collapse is permanent afterwards** — the gate refuses the cached lift
  and nothing re-validates once the copy has finished and the bytes are correct
  again.

### Confirmed: it always recovers, and it happens constantly

With the watch reporting recovery as well as onset:

    r1  episode 1 RECOVERED after 26 frame(s)
    r2  episode 1 RECOVERED after 10 frame(s)
    r3  episode 1 RECOVERED after 20 frame(s)

and roughly **2,000 episodes per run** — ITCM is being rewritten continuously,
not swapped once. r2 finished at **50.72 fps with `cache_absent_itcm = 0`**
despite those episodes, so an episode by itself does not cost anything.

### So this is not a defect, it is a cost

The validation in `lookup_in` runs on **every lookup**, not once — it is not
sticky. So when the bytes come back, the very next dispatch validates and the
lifted body is used again. The interpreter fallback lasts the episode and no
longer.

That makes the behaviour **correct**: while the guest is part-way through
rewriting its own code, the cached lift genuinely does not match what is in
memory, and refusing it is the only right answer. The 21M-instruction figure is
a hot IRQ loop interpreted for the 10-26 frames of one copy.

What it costs is frame rate during those windows, and the reason it looks like
"one run in four" is that only some runs catch a long enough episode at a busy
enough moment.

**Reducing it is a recompiler-side job, not a runtime fix**: the ITCM page hosts
more than one code body over time and only one was captured at lift time
(`[lb_itcm] MISS ... candidates=1`). The validation machinery already supports
several candidates per page — `coverage_manifest.cpp` loops over them and picks
the one whose bytes match — so lifting the other ITCM bodies would let the
matching candidate be selected instead of falling through. That belongs in
`port/recomp/apply_finder_patch.py` and is a project of its own.

**The fix is therefore not to hunt a writer.** There is no bug to find in a
guest that is legitimately reloading its own code. The port's gate needs to
re-check after the region settles instead of refusing for the rest of the
session — a stale-cache invalidation, not a memory-corruption hunt.

Not yet established: which routine does the copy, and whether the ring can show
it (the 64 entries captured were all **ARM7 reads** of 0x0724..0x0920, so the
ARM9 write had already scrolled out — the ring needs to be sampled closer to
the change, or filtered to ARM9 writes).

## Priority note

This bug does not block play. It corrupts *measurement*, and there is already a
working defence: read `cache_absent_itcm` beside every frame-rate number and
discard the runs where it is non-zero. That is how the 51–52 fps baseline was
established. The renderer phases affect what the player sees; this does not.
It should be picked up again with a cheaper instrument — one that does not move
the timing it is trying to observe.
