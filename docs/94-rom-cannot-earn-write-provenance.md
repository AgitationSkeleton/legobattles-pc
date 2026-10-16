# ROM cannot earn write provenance

**Status: the boot blocker is root-caused and fixed at the cause. The ARM7 was
not halted, starved, or missing code — every BIOS dispatch entry was being
rejected by a validation gate that ROM can never satisfy.**

## The chain

`docs/93` established the ARM7 stops at `PC=0x00000008` and never resumes. Six
hypotheses were tested and five were wrong, which is worth recording because
each wrong one was eliminated by a check rather than by argument.

| # | hypothesis | verdict |
| --- | --- | --- |
| 1 | waiting on an interrupt with `IME=0` | **wrong** — the SWI is `#3`, `WaitByLoop`, which returns |
| 2 | the BIOS bank was never linked | **wrong** — `${GEN}/arm7_bios.c` is linked unconditionally |
| 3 | the built bank was stale | **wrong** — binary 18:07 postdates bank 17:17 |
| 4 | no function exists at `0x08` | **wrong** — `arm7_bios_swi_vector_entry` exists and is registered |
| 5 | `BIOSPROT` hides the bytes | **wrong** — the exec path ignores `g_biosprot` |
| 6 | **the entry is found but marked inactive** | **correct** |

## What the SWI actually is

`LR = 0x038037BA` is Thumb, so the instruction is the halfword at `0x038037B8`:

```
038037B4  00df   svc #0
038037B6  7047   bx  lr
038037B8  03df   svc #3      <-- here
038037BA  7047   bx  lr
038037C2  04df   svc #4
038037CE  05df   svc #5
```

A BIOS thunk table. **`SWI 3` is `WaitByLoop`** and `R0 = 0xFA` (250) is its loop
count — which is why `IME=0` was a red herring: `WaitByLoop` returns on its own
and waits for nothing.

## The gate

`lookup_in()` in `runtime_arm.cpp` requires **two** conditions of a validated
entry, not one:

```c
if (!bus_range_has_write_provenance(validation->addr, validation->size) ||
    !bus_live_bytes_equal(validation->addr, validation->expected,
                          validation->size)) {
    if (inactive_candidate) *inactive_candidate = &table[i];
    continue;                       /* entry rejected */
}
```

The bytes matched. The **provenance** did not, and `written_for_ptr()` explains
why in five lines:

```c
if (match(g_main_ram,    g_main_ram_written))    return &g_main_ram_written;
if (match(g_itcm,        g_itcm_written))        return &g_itcm_written;
if (match(g_dtcm,        g_dtcm_written))        return &g_dtcm_written;
if (match(g_shared_wram, g_shared_wram_written)) return &g_shared_wram_written;
if (match(g_arm7_wram,   g_arm7_wram_written))   return &g_arm7_wram_written;
return nullptr;                     /* <- BIOS lands here, always */
```

**Five RAM regions. BIOS is not one of them.** `bus_load_arm7_bios()` is a bare
`std::memcpy` into `g_arm7_bios` that stamps no shadow, and no guest write ever
targets ROM. So `bus_range_has_write_provenance()` returns `false` for every
BIOS address, forever, and **every validated BIOS entry is permanently
inactive**.

The gate is not a bug in itself — its comment states a real concern: *"pristine
zero-filled RAM can otherwise select a late all-zero variant before firmware
boot has materialized it."* That reasoning is about **RAM the guest must fill
in**. Applied to ROM it is unsatisfiable by construction.

## Why this project hit it and upstream may not

The validation records exist **because this project put them there**.
`--validate-live-bytes` is not a default; `port/lift.sh` uses it deliberately
because the project assumes nothing about live code bytes, and the same flag was carried over to the four BIOS banks.

For cartridge code that is exactly right. For ROM it adds a check that can only
ever fail.

## The fix

Re-lift the four BIOS banks **without** `--validate-live-bytes`:

```
arm7_bios      637 functions (arm=111 thumb=526)
arm9_bios      167 functions (arm=63  thumb=104)
freebios_arm7   72 functions
freebios_arm9   70 functions
```

Identical counts to before — only the validation records are gone:

```c
/* before */ {0x00000008u, 0u, arm7_bios_swi_vector_entry, &g_validation_arm7_bios__3},
/* after  */ {0x00000008u, 0u, arm7_bios_swi_vector_entry, 0},
```

With a null validation pointer `lookup_in` skips the gate and returns the entry.

**This does not weaken that guarantee.** Every cartridge bank keeps
`--validate-live-bytes`. What the BIOS banks lose is a *runtime* re-check of
bytes that are already hash-verified at load — the runner prints `[load] arm7
bios: 16384 bytes, SHA-1 ok` against `24f67bdea115a2c847c8813a262502ee1607b7df`
before executing anything — and that cannot change afterwards, because nothing
in the machine can write to ROM. A SHA-1 over the whole image is a strictly
stronger check than four bytes at `0x08`.

## Tips and pitfalls

* **"Not found" and "found but refused" are different failures with the same
  symptom.** Five hypotheses died because the entry was visibly present in the
  table; the sixth explained how a present entry produces a miss.
* **Read the tool's own diagnosis first.** The runner printed
  `[dispatch-miss] ... INSIDE executable static ARM7 BIOS ROM at 0x00000008`
  on line 26 of a log whose *tail* had been read four times. The head of the log
  named the address, the CPU and the containing range immediately.
* **A safety check inherited from one context can be nonsense in another.**
  `--validate-live-bytes` is correct for RAM-resident cartridge code and
  unsatisfiable for ROM, and nothing warns about the difference.

## The result: LEGO Battles boots

Re-lifting the four BIOS banks without the validation records and relinking:

| | before | after |
| --- | ---: | ---: |
| dispatch misses | 1 (fatal) | **0** |
| ARM7 cycles | 3,426,692 *(frozen)* | **30,000,128** |
| ARM7 dispatches | 129,218 | **443,336** |
| ARM7 exceptions | 1 | **11,020** |
| ARM9 `IME` / ARM7 `IME` | 0 / 0 | **1 / 1** |
| ARM7 `IE` | `0x00040000` | **`0x01040099`** |
| executed code pages | 6 | **85** |

The ARM9 left `Boot_WaitLoop` (`0x02111C30` → `0x0210FD44`), both cores enabled
interrupts, `IPCSYNC.out` returned to 0 on both sides, and the ARM7 ran
**exactly half** the ARM9's cycles — the correct 67 MHz / 33 MHz ratio, so the
scheduler is pacing them properly rather than one starving.

The ARM7's steady state is textbook:

```
swi    pc=0x038037D4  addr=0x00000006      ; SWI 6 = Halt
dispatch pc=0x000011AC                     ; SVC_Halt, as named in biosnds7.toml
mem_w  pc=0x000011BC  addr=0x04000301 val=0x00000080   ; HALTCNT = halt
```

SWI 6 into `SVC_Halt` into a `HALTCNT` write is what a DS ARM7 does between
interrupts. No unimplemented ops, no dispatch misses, no errors.

**This is the first time LEGO Battles has run as native code.** The IPCSYNC
handshake that `docs/91` through `docs/93` chased is complete, and it was never
a handshake bug — it was one CPU unable to enter its BIOS at all.
