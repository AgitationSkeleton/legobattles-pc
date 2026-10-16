# 302 — RETRACTED: the validation gate is innocent

*The conclusion below was measured and is wrong — the ARM7 rejects nothing at
the gate. See the CORRECTION at the end, and docs/303 for the real cause. The
disassembly and the `--audit` finding here still stand.*

## Original title: the ARM7 is interpreted by one `continue`, not by missing entry points

docs/301 ended with a specific next step: declare `0x037FB684` and `0x037FB718`
in `lb_arm7_wram.toml`, because two addresses were 43.5% of every ARM7
native-to-interpreter boundary and neither was declared. **That plan was wrong**,
and following it would have fixed nothing. This records why, and where the
cause actually is.

## The plan failed its first check

`authoritative_entry_points` needs an exact `size`, and docs/301 said the lift's
`--audit` mode would supply the extents. It does not, on two counts.

`--audit` is *validate only, emit nothing* (`port/lift.sh`) — it checks declared
entry points, it does not enumerate undeclared ones. And it does not run at all:

```
nds_recompile --config port/config/lb_arm7_wram.toml \
              --bin out/eur/arm7/section_wram_037f8000.bin --audit
exit=139        (segmentation fault, no output)
```

Both the stock recompiler and this port's patched build segfault, on
`lb_arm7_wram`, `lb_arm7_static`, `lb_arm7_mainram` and `lb_arm9_itcm` alike —
every bank tried, including ones that lift fine in `emit` mode. **`--audit` is
broken**, which is worth knowing on its own, and the first run of it returned a
blank readout that could easily have been read as "nothing to report".

## Neither hot address is a function

Disassembling the region settles what the coverage counts could not:

```
0x037FB63C  push {r3,r4,r5,lr}        <- the real function
0x037FB670  beq  #0x37FB684
0x037FB67C  mov  lr, pc               <- pre-ARMv5 call-through-register
0x037FB680  bx   r1
0x037FB684  ldr  r2, [pc, #0x34]      <- "hot address" #1: the RETURN POINT
...
0x037FB6B0  bx   lr
0x037FB6B4  .. 0x037FB6C4             five literal-pool words
0x037FB6C8  ldr ip,[pc,#4]; mov r0,#0; bx ip; .word 0x037FB63C
0x037FB6D8  ... #1      0x037FB6E8 ... #2      0x037FB6F8 ... #3
0x037FB708  ... #4      0x037FB718 ... #5      <- "hot address" #2
0x037FB728  ... #6      0x037FB738 ... #7
0x037FB748  push {r3,lr}              a ninth handler, no channel index
```

`0x037FB684` is a **mid-function return point**: `mov lr, pc` two instructions
earlier sets `lr` to exactly `0x037FB684`, which is why every coverage row in
that cluster carries `caller = 0x037FB684`. `0x037FB718` is **arm #5 of an
eight-entry trampoline table**, each arm 0x10 bytes, each loading a channel
index and tail-jumping to the common body at `0x037FB63C`.

Declaring a return point as a function entry would have been wrong. The
"caller" column is `R[14]` sampled at the boundary, not a call site, and reading
it as one is what made the two addresses look like undeclared functions.

## The gap is real but small, and accounts to the byte

`0x037FB630 .. 0x037FB798` is undeclared — 0x168 bytes, and it decomposes
exactly:

```
0x037FB630   0x0C   literal pool / pad
0x037FB63C   0x8C   the common dispatcher
0x037FB6C8   0x80   eight trampolines, 0x10 each
0x037FB748   0x50   the ninth handler
                    0x0C + 0x8C + 0x80 + 0x50 = 0x168   exact
```

That it accounts to the byte is the check that the reading is right. But the
gap is not the problem, which the next measurement shows.

## The measurement that killed the plan

Classifying every ARM7 `ROOT` (kind 1 — a genuine native-to-Tier3 boundary)
against the declared entry map from all three ARM7 configs:

```
ARM7 ROOT: 2,331 entries, 2,568,236 hits
  declared ENTRY              0 entries          0 hits    0.0%
  mid-declared-function   1,596 entries  2,384,662 hits   92.9%
  UNDECLARED gap            735 entries    183,574 hits    7.1%
```

**92.9% of ARM7 interpreter entries are inside functions that are already
declared**, and not one lands on a declared entry point. Declaring every
undeclared byte in the ARM7 would address at most 7.1%.

## Where it actually goes wrong

Taking the six hottest mid-function addresses and looking them up in the
generated dispatch tables — every one is present:

```
0x037FF340  {0x037FF340u, 0u, lb_arm7_wram_afunc_037FF1C0, &g_validation_..._200}
0x037FF73C  {0x037FF73Cu, 0u, lb_arm7_wram_afunc_037FF3AC_chunk_1, ...}
0x03800088  {0x03800088u, 0u, lb_arm7_wram_afunc_037FFF54, ...}
0x037FC7A0  {0x037FC7A0u, 0u, lb_arm7_wram_afunc_037FC760, ...}
0x037FB4A8  {0x037FB4A8u, 0u, lb_arm7_wram_afunc_037FB488, ...}
0x037FD738  {0x037FD738u, 0u, lb_arm7_wram_afunc_037FD658, ...}
```

The lift emits **one dispatch entry per instruction address**, all pointing at
the containing function, which carries a re-entry `switch` with a label for
every instruction. So resuming mid-function is a designed-for case, not an
accident — the table can do it.

Three further things are confirmed rather than assumed:

* **the banks are registered on the right core** — `title_bank_registry.cpp`
  calls `nds_register_dispatch(NDS_ARM7, g_dispatch_lb_arm7_wram, …)`, and
  `NDS_ARM9 = 0, NDS_ARM7 = 1`, so `g_ctx[cpu & 1]` is the ARM7's context;
* **the mode matches** — the table says `thumb = 0` and every coverage row for
  these addresses says `thumb 0`;
* **the function pointer is non-null** — each entry names a generated function.

That leaves exactly one statement in `lookup_in()` that can drop them:

```c
if (!bus_range_has_write_provenance(validation->addr, validation->size) ||
    !bus_live_bytes_equal(validation->addr, validation->expected,
                          validation->size)) {
    if (inactive_candidate) *inactive_candidate = &table[i];
    continue;                 // -> lookup_static returns nullptr -> Tier 3
}
```

## Why one rejection costs millions of dispatches

From a real session's `dispatch_stats.json`:

```
arm7   cache_hit 6,588,079   cache_hit_absent 22,613,797   cache_slow_lookup 6,655
```

`cache_slow_lookup` is the only path that runs `lookup_in`. It is entered
**6,655 times in a whole match** — each ARM7 address is resolved once, comes
back absent, and `lookup_static_cached` then caches the absent result keyed on
the backing pages' generation counters. The ARM7 WRAM is written once at
autoload and then never again, so the generation never changes and **the absent
verdict is cached for the rest of the session**. One rejection at boot buys
22.6 million interpreted dispatches.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** the ARM7's interpreted share is not caused by missing entry
points. 92.9% of its interpreter entries are inside declared, lifted,
correctly-registered, correctly-moded functions whose addresses are in the
dispatch table, and the only statement that can refuse them is the validation
gate in `lookup_in`.

**ESTABLISHED:** `--audit` segfaults on every bank, so the extent-finding step
docs/301 planned does not exist.

**NOT ESTABLISHED:** which half of the gate fails. Provenance and byte-equality
want opposite fixes — no provenance means the autoload copy never marked the
bytes written, a byte difference means the expected image is not what the guest
put in RAM. Guessing between them is how docs/301 went wrong, so it is being
measured instead.

**NOT ESTABLISHED:** that fixing this makes the missing unit sounds audible.
That stays unclaimed until heard.

## The instrument

`apply_hooks.py` stanza 3z splits the short-circuited condition so each half is
attributable, and adds `static_reject_provenance`, `static_reject_bytes` and
`static_reject_first_pc` to `NdsDispatchStats` and to the JSON.

It cannot lag the game: `lookup_in` runs only on the cache slow path — 6,655
ARM7 entries per match against 22.6 million cached hits — and the fast path is
not touched. Evaluation order is unchanged, so `bus_live_bytes_equal` still
only runs when provenance passed.

## CORRECTION — the mid-function finding does not hold, and the gate is innocent

The counters this document specified were added and measured. On the ARM7:

```
static_reject_provenance  0
static_reject_bytes       0
```

**The validation gate never rejects anything on the ARM7.** The hypothesis this
document was written to test is dead, and it was dead for a methodological
reason worth naming: the "92.9% mid-declared-function" classification compared
an OLD reference capture against declared TOML spans, which is not the question.
The question is whether the pc is in a **dispatch table**, and a declared span
is not the same set.

Re-measured on a capture from the build being measured, and against the tables
the lift actually emits (docs/303):

```
ARM7 ROOT: 583 entries, 157,847 hits
   pc IS in a dispatch table :       0    0.0%
   pc is NOT in any table    : 157,847  100.0%
```

All of it is undeclared code. `0x037FB63C`, `0x037FB684`, `0x037FB718` and
`0x037FB748` were all absent from every ARM7 dispatch table, exactly as the
0x168-byte gap this document mapped implies.

**What survives:** the `--audit` segfault; the reading of `0x037FB684` as a
return point and `0x037FB718` as trampoline arm #5; the gap's byte-exact
decomposition; and the counters themselves, which turned out to explain the
ARM9 instead — 6,719 rejections, first at `0x0203DCB8`, beside this port's own
touch patches. That is by design: a patched function must not run from lifted
code that predates the patch.

**Also killed here:** the `page_count > 2` gate in `lookup_static_cached`,
which refuses any validation range spanning more than two 4 KiB pages even on a
hit. It is real, but it catches **1 function in 10,003** (`0x0202E624`, size
0x2398, 3 pages) — worth knowing that one address is permanently interpreted,
not worth anything else.

The cause and the fix are in docs/303.
