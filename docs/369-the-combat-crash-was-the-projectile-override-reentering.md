# 369 — The combat crash was the projectile override re-entering itself

Status: **cause named and removed.** The replay that reproduced the crash
exactly is clean on the fixed build (verification below).

The defect (2026-09-25): the game invariably crashed on entering combat. Hero
healing or hotwire healing was suspected; music was ruled out by turning off
faction music.

It was neither healing nor music. It was the port's own override on the ranged
damage path, `func_0206E408`. It ran on every ranged hit whatever the setting.
When the scheduler ended a slice inside the guest call it made, it was entered
a second time with the wrong arguments.

## What every crash has in common

Both crashing sessions from the report (`build/logs/session-20260924-125930`
and `-130803`) and both harness reproductions start the same way.

First, the ITCM IRQ dispatcher has two bytes changed, found by the lookup's
validation:

    [lb_itcm] REJECTED pc=01FF8068 range 01FF8068+380 prov=1 first differing byte at +151 (expected EA)
    [lb_itcm]   01FF80FC live FF000000 expected EA000000  <-- DIFFERS
    [lb_itcm]   01FF8100 live E35000FF expected E3500001  <-- DIFFERS

Then wild accesses follow. They are the same addresses in every log:

    [bus] ARM9 unmapped read 0x7FDDB4BC w=4
    [bus] ARM9 unmapped write 0xFE33FF40 w=1
    [bus] ARM9 unmapped write 0xE92D4000 w=1
    [bus] ARM9 unmapped read 0x037FF768 .. 0x037FF788 w=4

`0x01FF80FF` and `0x01FF8100` are ITCM offsets 0xFF and 0x100. CP15 gives the
ITCM a 32 MB virtual span from address 0 (`[cp15] ITCM region: base=0
vsize=33554432`), so they are also addresses `0x000000FF` and `0x00000100`.
docs/326 got this far: "a null struct pointer plus offset 0xFF".

## The null page is protected on hardware

The new switch prints the protection regions the game itself programmed:

    [lb_null] MPU regions: 0=04000033 1=0200002B 2=023E0021 3=08000035
              4=027E001B 5=0100002F 6=FFFF001D 7=027FF017 control=0005707D

Region 5 is the ITCM: base `0x01000000`, size field 0x17, i.e. 16 MB. No region
covers `0x00000000..0x00FFFFFF`, and control bit 0 (the MPU) is on. So on a DS,
any CPU access below `0x01000000` is a data abort. The retail game cannot make
these stores and keep running. Whatever makes them here is a port divergence.

## The instrument: `LB_NULLPAGE=1`

apply_hooks.py stanza 0zzz. It reports every ARM9 data access below
`0x01000000`. Each report carries:

* the exact pc (the banks set R15 at every instruction);
* lr and r0-r7;
* for the first 12, the shadow return stack (`runtime_call_stack_data()`), i.e.
  the chain of callers.

ITCM is normally served by the inline fast window, which a hook cannot see
(docs/326's "LB_WATCH could never have found it"). Under this switch that
window's limit is 0, so ITCM goes through bus.cpp's slow path, where the check
lives. It is off by default, and then nothing changes.

Two reports appear in every run and are not the CPU. `R16/W16 00000000 at pc
021100E8` is `MI_StopDma(0)`'s deliberate one-halfword DMA from 0 to 0 (the
NitroSDK's DMA0 workaround: SAD=0, DAD=0, CNT=0x81400001; the pc is the `bl`
after the store). See "Left open" below.

## Reproducing it

A Free Play match with the reporting settings, left until the enemy attacks
(`lb_exp.py --route free --keep-settings --linger 540`), crashed in 2 of the first 4 runs. The other 2 were defeated before
the crash point. With identical build and settings the runs are close to
deterministic: crash5 and crash6 were both defeated at 12,881-12,882 widened
presents. So repeating a clean configuration only repeats the clean result.

What made it exact was the port's own session recording. Every run writes
`lb-session-<epoch>.lbdemo`. Replaying exp-crash3's recording
(`lb-session-1790360010.lbdemo`, 20 input events, the last at frame 3660)
ended in crash3's own final line:

    [dispatch-miss] cpu=arm9 pc=0xE3A0C300 thumb (lr=0x020F6BD4)

That is the same crash, and this time the null page was being watched.

## The trace

    [lb_null] #3 R8 000000FF = 000000EA at pc 0205C9B8 lr 02087368 | r0 02155024 r1 000000FF r2 000000FF ... r4 000000FF
    [lb_null]   callers, innermost first (depth 21): 02087368 020872E4 02087284 0206E408 0206E268 02050014 ...
    [lb_null] #4 W8 000000FF = 000000FF at pc 02087378 lr 02087374
    [lb_null] #5 W8 00000100 = 000000FF at pc 02087384 lr 02087374
    [lb_null] #8 R32 00000000 = E92D4000 at pc 021507EC lr 02039210
    [lb_null] #9 W8 00000000 = 00000000 at pc 021507F0 lr E92D4000 ... sp 00000000

Read from the ROM:

* **#3** is the entity-pool lookup `func_0205C9B4` reading a handle's slot
  byte at `0x000000FF`. Its callers are:
  * `func_02087350`;
  * `func_020872DC`;
  * `func_0208727C`, the target resolver;
  * **`0x0206E408`**, the return marker the port's projectile override
    pushes for its nested call;
  * `0x0206E268`, the cartridge's call site.

  So the handle pointer handed to the resolver was `0xFF`.
* **#4 and #5** are `func_02087350` failing the lookup and writing a null
  handle, FF FF, back through that pointer: to `0xFF` and `0x100`. Those are
  the dispatcher's bytes at `0x01FF80FF` and `0x01FF8100`, the change in every
  crash log.
* **#8 onwards** is the aftermath. A word read at 0 returns the ITCM's first
  instruction (`E92D4000`, `push {lr}`), sp becomes 0 and lr becomes
  `E92D4000`, and execution leaves the rails.

## The mechanism

`func_0206E408` is 72 bytes:

    0206E414  add r0, r6, #0x24 ; bl func_02087250   projectile = handle(cmd+0x24)
    0206E420  mov r0, r5        ; bl func_0208727C   target = resolve(r1)
    0206E428  cmp r0, #0        ; popeq {r4-r6,pc}
    0206E430  ldr r1, [r4,#0x160] / ldr r3, [r1,#8]
    0206E440  ldrh r2, [r3,#0x6C] / ldrh r3, [r3,#0x6E]
    0206E448  bl func_0206E970                       ApplyDamage(cmd, r1, min, max)

For "Attack Boost Affects Ranged Units" the port REPLACED it. The replacement
re-did the target check as a nested guest call, and it ran with the setting off
too:

    runtime_call_push_return(kProjectileDamage);   // marker 0x0206E408
    g_cpu.R[0] = target_handle;
    g_cpu.R[14] = kProjectileDamage;
    runtime_dispatch(kResolveTarget);
    if (runtime_unwinding()) return;   // re-entry redoes a pure lookup

The comment on the last line is where it breaks. `runtime_should_yield()` runs
at every compiled instruction and ends the slice as soon as the cycle cap is
reached, at any instruction. After that unwind the guest does not re-run the
override. It resumes INSIDE `func_0208727C`, finishes it, and returns to the
marker `0x0206E408`. The runtime dispatches that address, and the override is
entered a second time, with:

* r0 = the resolver's result, taken as "the command";
* r1 = whatever the resolver left, here 0xFF, taken as "the target handle";
* the real caller's return address gone, because it lived in the first
  entry's host frame.

The second entry resolves "handle" `0xFF`, which is #3-#5 above, and the
guest then returns through a garbage lr.

That is why it tracks combat so closely. Any ranged hit can be the one where a
slice boundary falls inside that nested call, and a fight has many.

## The fix

`func_0206E408` is no longer overridden: the cartridge's own code runs. The
boost moved to the one call that code makes:

* `apply_damage_observer` on `ApplyDamage` (`0x0206E970`, three callers).
* It acts only when lr is `0x0206E44C` (the return address of the bl at
  `0x0206E448`, which only the ranged path has) and the setting is on.
* It then adds the buff delta to r2/r3 and tail-calls the cartridge's applier.
* It makes no guest call and nothing runs after the tail call.
* With the setting off it changes nothing.

The shooter is found read-only in C (`resolve_handle_kind`, the same lookup),
and the cartridge still does its own lookups and write-backs.

`[lb_projectile] ranged damage applied N times` counts ranged hits (powers of
two) so "no ranged fight happened" can be told apart from "the path is broken".
The old counter could never print: the crash came on the first or second
ranged hit.

## Retracted or removed

* **"MI_CpuFill8 filling 2 bytes"** as the +0xFF writer: a guess in an
  uncommitted note. The writer is `func_02087350`'s handle write-back. The two
  observers built on the guess (on `MI_CpuFill8`/`MI_CpuCopy8`, dropping fills
  below 0x8000 by default) are removed. They never fired.
* **The ITCM byte repair.** It put up to 8 changed ITCM bytes back at lookup
  time. It was a stopgap "until the writer is named". It could not stop the
  crash (crash3 repaired and still died). It hid the evidence and is not what
  hardware does. Removed, never committed.
* **"re-entry redoes a pure lookup"** (the override's own comment) and
  **"projectile_damage_override ... does the same call correctly"** (a note in
  heal_tick_override's history, now marked RETRACTED in place).
* **docs/326's "always recovers ... not a defect, it is a cost"**, as far as
  the FF FF pair goes. docs/326 first caught FF FF at these two bytes, then a
  later watch run caught 00 00 that recovered, and merged the two into "a copy
  caught in progress". The FF FF pair is a stray store with a fixed value (the
  null handle), and in the reported sessions it did not recover. The 00 00
  episodes were not re-examined here. docs/326 carries a note.
* **Healing and hotwire.** With the reporting settings, `heal_tick_override` and
  `set_hp_override` are tail-call pass-throughs and were not on the path.

## Left open

* **The same unsafe nested call remains on two non-default paths.**
  * `set_owner_override`'s occupant sweep calls SetOwner nested, only with
    `hotwire_transport` 1 or 2 (off by default and in the reporting settings).
  * The kill self-test's `call_guest` chain runs only when a developer arms
    it.

  Both return on unwind exactly as the removed override did. They need the
  two-override pattern (the lifted body never returns; call original() once,
  as a tail call) before either path is offered as working.
* **DMA sees ITCM here, and cannot on hardware.** `resolve()` maps ITCM for
  every ARM9-side access, including DMA, but the TCMs are private to the CPU.
  `MI_StopDma(0)`'s dummy transfer shows it: harmless, since it copies a
  halfword onto itself, but a real divergence for any DMA whose address falls
  in the TCM windows.

## Verification

**The crash.** exp-fix369a replays the same recording that crashed
(`lb-session-1790360010.lbdemo`, reporting settings) on the fixed build, with
`LB_NULLPAGE=1` and a 420 s linger:

    [lb_projectile] ranged damage applied 1 times
    [lb_projectile] ranged damage applied 2 times
    [lb_projectile] ranged damage applied 4 times
    [lb_projectile] ranged damage applied 8 times

Results:
* The ranged path ran at least 8 times, where the old build died on the first
  or second hit.
* No CPU access to the null page (only the two MI_StopDma DMA reports), no
  ITCM rejection, no unmapped access, no dispatch miss.
* The match played on past the old crash point (about 15,500 presents) to its
  normal end, "Defeated!" (the harness player gives no orders), at about
  16,800.
* The game stayed live on the results screen for 12,000 more presents, at
  59.8 fps.

**Exactness gate** (exp-gate369, the reference replay `lb-session-1790122638` at
1473x830): instruction counts identical at all six checkpoints (frame 7500:
insn9 1769454450, insn7 104969363); 8 of 8 layer maps and composites
identical to gate-quiet; 59.83 fps. That replay has no ranged hit before frame
7500, so the swap cannot show there. It is shown above.

**Vanilla** (exp-van369, adaptive off): 240 of 240 presented and raw frames
identical to exp-van-hudmask.

25 gameplay overrides, down from 27: the two fill observers are removed, and
the projectile override became the ApplyDamage observer.
