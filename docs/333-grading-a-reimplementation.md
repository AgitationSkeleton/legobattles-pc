# 333 — How a reimplemented function is graded, and the first one

docs/332 said the native port needs a metric that survives the absence of
mwccarm, and named shadow execution. This is the mechanism as built, including
the design it had to abandon and why.

## The obvious design does not work here

Call the guest function and the C reimplementation with the same arguments and
compare the results. It is what every decomp project's differential test does,
and in this runner it cannot be written, for a reason already paid for twice:

> **A recompiled body does not return.** Its epilogue sets `R[15]` from LR and
> dispatches onward, so `original()` runs the target, then its caller, then
> *its* caller, and comes back only when the whole slice unwinds. Anything
> after it runs once, at slice end, with `R[15]` somewhere else. A second
> `original()` lands on the body's entry guard and falls into
> `runtime_dispatch_miss`.

So an override gets **one** call to the original, as a true tail call, and
nothing after it can read a return value. `runtime_call_push_return` can invoke
a leaf out of turn — and `func_02016A00` is a leaf — but doing so from inside
the dispatcher's own entry for that same address hits the guard.

## What is used instead: a trace hash over an A/B replay

The runner replays a recorded route deterministically. Run the same route
twice:

* **control arm** — `LB_SHADOW` unset. The cartridge's function runs. The
  override does nothing but increment a counter and tail-call.
* **native arm** — `LB_SHADOW=native`. `src/battles/registry.c` runs instead,
  and the cartridge's copy is never entered.

With `LB_RAMSUM=N`, both runs hash **all 4 MB of main RAM** with FNV-1a every
`N` frames, from `lb_patches_on_vblank` — on the emulation thread, with the
guest between frames. Two hash streams that agree at every sample mean the two
implementations left main RAM byte-identical at every sampled frame, for every
value written anywhere, not merely at the call site.

Stride is 4 bytes and not coarser on purpose: a stride that skipped words would
be blind to exactly the small scattered write a wrong lookup causes — one
pointer stored into one object — which is the failure this exists to catch.

### What it does not prove

Said here once so it is not claimed later:

* only the paths the replay took are covered; a hash that never diverges says
  nothing about an input the run never supplied;
* two implementations wrong in the same way agree;
* a difference that never reaches main RAM — a register the caller drops —
  is invisible.

It is evidence of behavioural equivalence **on the graded route**, which is a
real and checkable claim, and it is not a proof of correctness.

### The failure that would look like a pass

An A/B whose native arm never ran produces two identical hash streams. That is
why `lb_shadow_mode()` **refuses an unrecognised `LB_SHADOW` value and exits**
rather than falling back to the control arm, and why every registered function
carries a per-arm call count that is printed at the end of the run. A grading
run that does not show a non-zero `native` count is discarded.

## The first function: `func_02016A00`

`Battles::RegisteredCreateInfo` registry lookup. Read out of the image with
`lb_code.py dis`, not out of the decompiler:

    02016a00  mvn   r1, #0              ; r1 = -1
    02016a04  cmp   r0, r1
    02016a08  moveq r0, #0
    02016a0c  bxeq  lr                  ; key == -1 -> 0, without touching RAM
    02016a10  ldr   r1, [pc, #0x2c]     ; = 0x0215111C
    02016a14  ldr   r2, [r1, #0x40]     ; node = head
    02016a18  cmp   r2, #0
    02016a1c  beq   0x2016a3c
    02016a20  ldr   r1, [r2, #0x24]     ; node->key
    02016a24  cmp   r0, r1
    02016a28  moveq r0, r2
    02016a2c  bxeq  lr                  ; hit -> the node itself
    02016a30  ldr   r2, [r2, #4]        ; node = node->next
    02016a34  cmp   r2, #0
    02016a38  bne   0x2016a20
    02016a3c  mov   r0, #0
    02016a40  bx    lr
    02016a44  .word 0x0215111C

**Why this one.** It is a true leaf — no direct call and no virtual call, which
places it in the 1,480-function tranche docs/332 sized — it has 69 call sites
so a replay exercises it heavily, and it is pure, so a divergence can only come
from the lookup.

**Where the name comes from.** Seventeen functions load `0x0215111C` from their
literal pools, all in the `0x020163D0..0x02016F50` cluster, and the last is
named by the ARM9's own RTTI: `Battles::RegisteredCreateInfo::virtual_01`. The
class name is ground truth. RTTI carries no method names, so
`Battles_RegistryFind` is named for what it does.

**Two details a transcription of the decompiler output would have lost**, both
behavioural:

* **The `-1` guard runs before the head is loaded.** A key of `-1` returns 0
  even when the singleton is not yet constructed. Moving the guard after the
  load faults on a null holder during startup.
* **The sentinel is `-1`, not `0`.** Key 0 is a legitimate key and is looked up
  like any other. `if (!key) return 0` would be wrong in a way no test that
  never asks for key 0 would catch.

## The address space the decompilation reads through

`src/battles/mem.h` defines `B_R32`/`B_R16`/`B_R8` twice:

* under `BATTLES_ON_RECOMP`, as calls into ndsrecomp's bus, which is how the
  runner grades them;
* otherwise, as plain loads out of `battles_ram`, a 4 MB buffer the native port
  owns, with main RAM kept flat at `0x02000000`.

**The cartridge's layout is retained deliberately.** Every offset in this
decompilation was measured against the ROM's own structures; keeping main RAM
flat means a measured offset stays correct natively without being re-derived.
Rearranging the data is a later decision and a separate risk, and it is not
needed to run natively.

`BATTLES_ON_RECOMP` is load-bearing rather than cosmetic: without it the file
still compiles and then reads host addresses, which is a segfault at the first
lookup rather than a build error. It is set on that one source file by
`apply_hooks.py`.

## Running a grading pass

    LB_SHADOW=guest  LB_RAMSUM=120  ... --demo <route>    > control.log
    LB_SHADOW=native LB_RAMSUM=120  ... --demo <route>    > native.log
    diff <(grep lb_ramsum control.log) <(grep lb_ramsum native.log)

Empty diff, plus a non-zero `native` count in the native run's
`[lb_shadow] Battles_RegistryFind guest N native M` line, is a pass for that
route.

## The first grading pass, and what it found

Run on `demos/user-combat-2026-09-14.lbdemo` at 1024x600, `LB_RAMSUM=120`.

**The instrument check passed outright.** Two independent control runs produced
**143 of 143 identical hashes**, frame 0 to frame 17,040. The replay is
bit-deterministic over all 4 MB of main RAM, so trace hashing is a usable
oracle for this port. That is worth more than the function it was built for:
every future reimplementation can be graded against it.

**The native arm ran.** `[lb_shadow] Battles_RegistryFind guest 0 native 1831`
against the control's `guest 1362 native 0` — and at the matching report the
counts are **1362 in both arms**, so both arms called the function the same
number of times in the same places.

**And the streams diverge, at four samples out of 136:**

    f=5280  f=5520  f=6720  f=11880

**This does not show the reimplementation is wrong, and the method as built
cannot say whether it is.** Two things rule the obvious reading out:

* **The divergences heal.** A wrong registry lookup returns a wrong object
  pointer, the caller stores it, and state diverges permanently. Four isolated
  samples that re-converge are a transient value — one that differs at the
  sampling instant and is overwritten before the next.
* **The call counts are identical.** 1362 in both arms at the same report. A
  wrong lookup changes what the game does and therefore how often it asks.

### The confound: a host replacement consumes no emulated cycles

`runtime_tick`, `runtime_mem_cycles` and `runtime_code_cycles` account cycles
**per lifted instruction**. A replacement that returns through
`lb_override_return()` never enters the lifted body, so it costs **zero**
emulated cycles where the cartridge's version costs sixty-odd. The emulated
clock therefore runs ahead in the native arm, the scheduler crosses timer and
IRQ boundaries at different instants, and a hash taken at a fixed *frame* can
catch a counter mid-update. The native run also reached 136 samples against the
control's 143, which is the same effect on the run's length.

So the trace hash cannot separate "returned a different value" from "cost a
different number of cycles", and for a *replacement* arm the second is
guaranteed. **`Battles_RegistryFind` is UNGRADED, not FAILED.**

### What replaces it: observe, do not replace

The fix removes the replacement entirely:

1. the override on `0x02016A00` records the key from `R0` and **tail-calls the
   cartridge** — no behaviour change, no cycle change, both arms identical;
2. from `lb_patches_on_vblank`, call `func_02016A00` **out of turn** for each
   recorded key via `runtime_call_push_return` + `runtime_dispatch`, which is
   the mechanism that already works for leaf helpers and which this function
   qualifies for;
3. compare that answer against the C reimplementation on the same key.

The earlier objection — that calling the original a second time lands on the
body's entry guard — applies to a second `original()` from *inside* the
dispatcher's entry for that address. A call from the vblank hook is not inside
it. The grade becomes a direct per-call comparison and timing never enters it.
`runtime_call_cancel_return` must be called on every failure path or the frame
leaks one marker per call until `nds_halt("call-return overflow")`.

## Verify mode, as built — and the two things that had to be wrong first

**The vblank hook is not guest context.** Written as above, every probe reported
`[lb_call] UNWOUND at verify-registry` — 876 of them in one run.
`lb_patches_on_vblank()` is called from `io.cpp`'s `VCOUNT == 192` branch, which
is the **scheduler**, so `runtime_dispatch` from there has no slice to return
into. The probe runs from the override instead, which is guest context, the way
`issue_waiting_to_die` already makes its calls. It grades the **previous**
batch, at the top of a later call, before that call's own tail call — so no
probe ever runs inside the dispatch for the call it is sampling.

**An unwind is a preemption, not a failure, and cancelling its marker is
wrong.** `runtime_unwinding()` is armed by `runtime_slice_yield()`, whose
contract says the return-check "uses this to PRESERVE the pending return-push
instead of cancelling it — the call-return stack is saved/restored per-CPU
across the preemption". `func_02016A00`'s list walk ends in a backward branch
and `runtime_slice_yield()` is checked exactly at backward branches, so the
probe is preemptible by construction. The marker is now cancelled only when the
call failed for some other reason, and a preempted probe ends the batch rather
than pushing more markers into an unwind.

**And the report was the hash's passenger.** `lb_shadow_report()` printed only
from inside the `LB_RAMSUM` branch, so a verify run — which has no reason to
set `LB_RAMSUM` — produced the banner and then nothing: no graded count, no
refusals, no disagreement. A whole run's measurement was invisible because an
unrelated variable was unset.

## The result

`tools/lb_grade.py --demo demos/user-combat-2026-09-14.lbdemo`:

    Battles_RegistryFind on user-combat-2026-09-14.lbdemo
      cartridge calls   2663
      graded            603   (refused 121)
      disagreements     0
      found a node      523
      distinct keys    >=30   distinct results >=15

    PASS: 603 graded, 0 disagreements, 523 found a node, >=30 keys

**603 calls compared against the cartridge's own code, zero disagreements**, on
a recorded combat route, with the cartridge serving the game throughout
(`native 0`). 121 probes were preempted and are counted, not hidden.

### Why this is not a vacuous pass

Six hundred agreements would mean nothing if every one were `0 == 0` — a stub
returning 0 unconditionally would score identically. It is not that:

* **523 of 603 found a node**, so the list walk ran and returned real pointers;
* the remaining ~80 returned 0, so **both branches are exercised**;
* **at least 30 distinct keys** and **at least 15 distinct node addresses**
  appear in the sample (both counters cap at 32, so these are floors).

`lb_grade.py` refuses a grade rather than reporting a pass when any of those
floors is not met, when nothing was graded, or when `native != 0`.

### What it still does not prove

Only the paths this route took. 30-odd keys is a real sample and it is not the
whole key space; a key the run never asked for is untested. The claim is
behavioural equivalence **on the graded route**, and it is recorded that way.

## The second function: `func_0211FDB4`, the signed divide

The ARM7TDMI has no divide instruction, so every `/` and `%` on a signed int in
this binary is a call here. It is the hottest thing graded so far — **125,004
calls in one replay**.

    Battles_DivMod on user-combat-2026-09-14.lbdemo
      cartridge calls   125004
      graded            23542   (refused 5958)
      disagreements     0
      found a node      23539
      distinct keys    >=32   distinct results >=32

**Two edge cases are not what C does**, and both are why `src/battles/divmod.c`
does the magnitude work in `u32` instead of writing `a / b`:

* **Division by zero does not trap.** The `beq` at `0211FDD0` reads the flags
  from `cmp r1,#0` two instructions earlier (`rsblt` has no `S`), so `b == 0`
  jumps straight to the sign fixup with `r0` still holding `|a|` and `r1`
  holding 0. The result is **`q = a, r = 0`**. In C that is undefined
  behaviour and on x86 it faults, so a port that writes `a / b` turns a defined
  behaviour into a crash.
* **`INT_MIN` is not negated.** `rsblt r0, r0, #0` on `0x80000000` overflows
  back to `0x80000000`, and everything after it is unsigned.

### The harness produced a false positive, and its own output caught it

The first run with both functions reported `Battles_RegistryFind` **disagreeing
on 635 of 635 calls** — a function that had passed 603 of 603 the run before.
The report line said:

    first disagreement: Battles_RegistryFind(00000000) cartridge 00000000 native 00000000

which is nonsense: those are equal. The full `DISAGREE` line had the answer:

    cartridge 00000000,0215111C  native 00000000,00000000

R0 matched. **R1 held `0215111C` — the singleton address the cartridge loads
into `r1` as scratch** — against the zero the reimplementation never wrote.
Adding the divide's second result word had made the comparison two words wide
for every function, and a register a function does not return is not a result.

Fixed by giving each probe a **result-word count**: the registry returns one
word, the divide two. Two instrument defects came out of it:

* the "first disagreement" line printed only R0, so it could not show where the
  difference was — it now prints both words;
* `found` and `results>=` were counting the scratch word, which **inflated**
  them. The registry's honest figures are `found 521` and `results>=15`, not
  the 635 and 26 the two-word comparison reported.

## Three more leaves, and the method's real limit

`src/battles/leaf.c`, graded on the same route:

| function | bytes | cartridge calls | graded | disagreed | verdict |
|---|---:|---:|---:|---:|---|
| `Battles_SplitArrayElem` `020EF1E0` | 32 | 446,390 | 212,152 | **0** | PASS |
| `Battles_ListFindByKey` `0206AC2C` | 44 | 11,088 | 5,194 | **7** | see below |
| `Battles_IndirectU16IsSet` `0205E638` | 36 | 36 | 16 | 0 | **REFUSED** |

`Battles_SplitArrayElem` is the hottest thing graded yet — a u16 array with a
small-buffer optimisation, inline elements at `obj+4` and a spill block at
`obj+0x2C`. Its index comparison is **signed** (`addlt`, not `addlo`), so a
negative index returns an address *before* the object; that is the ROM's
behaviour and it is reproduced rather than corrected.

`Battles_IndirectU16IsSet` is **refused, not passed**. 16 graded calls with
zero disagreements looks like a pass and is not one: the sample holds only
**3 distinct arguments**, below `lb_grade.py --min-keys`. The route barely
calls it.

### THE STALENESS LIMIT: a delayed probe is not sound on mutable data

`Battles_ListFindByKey` disagreed on 19 of 3,742 calls, and the printed values
were nonsense on **both** sides:

    DISAGREE #3 Battles_ListFindByKey(022B6834,00000000):
                cartridge 111002E0   native E5921000

`E5921000` is `ldr r1,[r2]`; `E0021011` is the neighbouring instruction. Both
implementations were walking into **code**, one word apart — so the object's
`+0x0C` no longer pointed at a list by the time the probe ran, and the two
walks did not even read the same garbage.

The probe grades a batch of earlier calls, so there is a delay between the
game's call and the re-execution. **Shortening it moves the number, almost
exactly proportionally:**

| batch size | graded | disagreed | rate |
|---:|---:|---:|---:|
| 16 | 3,742 | 19 | **0.508%** |
| 4 | 5,194 | 7 | **0.135%** |

A 4x shorter delay gave a 3.8x lower rate. That is the signature of a
**staleness artifact, not a wrong reimplementation** — a logic error would not
care how long ago the arguments were recorded. It is recorded here rather than
tuned away, because the limit is real:

> **Delayed re-execution is only sound for arguments whose reachable data is
> stable between the call and the probe.** The registry, the divide and the
> split array are stable; a list the game mutates is not.

`Battles_ListFindByKey` is therefore **UNGRADED**, and grading it needs a probe
that runs in the same instant as the call — which the "a recompiled body does
not return" constraint at the top of this document is exactly what prevents.

The batch size is now 4 rather than 16 for a second reason: the ring only
drains when it fills, so a function the route calls rarely was never graded at
all. `Battles_IndirectU16IsSet` reported `graded 0` before the change.

## The soft-float trio, and why a big number was not a pass

`src/battles/softfloat.c` reimplements the three IEEE-754 binary32 routines the
game calls because neither DS CPU has an FPU — `func_0211F324` (add, 540 B),
`func_0211F934` (subtract, 608 B) and `func_0211F748` (multiply, 480 B). The
reimplementations are literally `a + b`, `a - b` and `a * b` on host floats:
the whole question a native port has is whether the host's binary32 agrees with
1,628 bytes of software mantissa alignment, and that is a measurement.

**They ran, they agreed, and they are still REFUSED:**

| function | graded | disagreed | distinct a0 | distinct a1 |
|---|---:|---:|---:|---:|
| `Battles_FMul` | 11,905 | 0 | **1** | 14 |
| `Battles_FAdd` | 11,358 | 0 | **1** | 5 |
| `Battles_FSub` | 39 | 0 | 9 | 1 |

Eleven thousand agreements over **one distinct first operand** is not eleven
thousand test cases. It is five, and fourteen. The route computes `k + x` and
`k * x` for a single fixed `k`, and a reimplementation that got the exponent
handling wrong for any other `k` would score exactly the same.

### The tally that hid it

The diversity counter hashed **`a0 ^ a1`** into one 32-entry set, so the same
runs reported `keys>=5` and `keys>=14` — numbers that read as thin but
adequate. Counting the two arguments separately is what showed `a0` to be a
single constant. The XOR collapses a pair and collides; it should never have
been one number.

This is the third instrument defect in this harness found by looking at its
output rather than by it failing (after the scratch register and the delayed
probe), and all three would have inflated a result.

### What a real float verdict needs: inputs of our own

For a **register-pure** function — no memory touched, arguments and result in
registers — the probe does not have to use the arguments the game supplied. It
can call the cartridge with **chosen** bit patterns: normals, denormals, signed
zeros, infinities, NaNs, and the exponent boundaries where a soft-float
implementation actually goes wrong. Nothing in main RAM is read or written, so
there is no staleness and no side effect.

That is the next step, and it is the only way these 1,628 bytes get an honest
verdict. Until then they are **written but unverified**, which the status file
records as a separate column rather than folding into the percentage.

### What is registered in a normal run: neither of them

`kRegistryFind` and `kDivMod` are graded, not patched, and `kDivMod` sits on
the signed divide. Leaving it registered would put a host dispatch and a table
lookup on one of the hottest paths in the game, in the build players run,
to serve a counter nobody is reading — and this project shipped a regression of
exactly that shape this week. The registration now **filters both entries out
unless `LB_SHADOW` is set**, keeping the source table sorted (which the binary
search requires) while the ARM9 never sees them.

## The number this moves

Nothing yet, and the status file says so. `docs/data/native_status.tsv` gains a
row when a function passes, carrying the route and the sample count. 68 bytes
is **0.006%** of the binary — twice the entire hand-written byte-matched
decompilation that preceded it, and meant to read as small.
