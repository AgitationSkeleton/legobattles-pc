# The ARM7 was never lifted, and one word was why

**Status: the boot stall in `docs/91` is diagnosed to a register and a cause. The
ARM9 spins on an `IPCSYNC` handshake the ARM7 never answers, and the ARM7 never
answers because 96 KB of its code was excluded from every scan this project has
ever run — by a set literal.**

## What the stall actually is

`Boot_WaitLoop` (`func_02111B64`) resolves cleanly once its literal pool is read:

```
r3 = 0x04000180   REG_IPCSYNC
r1 = 0x04000184   REG_IPCFIFOCNT   (written 0xC408: enable, clear, IRQ)
r0 = 0x027FFC00   the ARM7/ARM9 shared region
```

and the loop is the standard IPCSYNC handshake:

```asm
ldrh lr, [r3] & 0xF     ; the nibble the ARM7 put there
strh lr<<8, [r3]        ; echo it back in bits 8..11
wait: ldrh r0,[r3] & 0xF
      cmp r0, lr
      beq wait          ; spin until the ARM7 changes it
```

It is **bounded** — a 1000-iteration countdown and four attempts — not an
infinite loop. It gives up and returns. The game simply never gets its ARM7.

## The cause

`tools/lb_scan.py`:

```python
EXEC_SECTIONS = {"static", "wram", "itcm"}
```

The ARM7's autoload-1 block — **`mainram`, 96,376 bytes at `0x027E0000`** — is
not in that set. Every seed pointing into it was silently dropped, which is why
every ARM7 scan in this project has reported ~63 KB of executable bytes and found
nothing beyond WRAM.

The section is executed, and that was already established rather than assumed:

* `docs/83` found **40 calls into it that survive a control-flow walk**, and
  explicitly recorded it as "a region the corpus does not model at all".
* Reading `0x027E0000` gives `push {r3,r4,r5,lr}; sub sp,sp,#8` — an ordinary
  ARM prologue.
* Walking it directly produces sane functions: 396 bytes with 14 calls at the
  entry, 496 bytes at `0x027EA1AC`.

Three separate signals, across three documents, and the scanner still skipped it
because the section's **name** was not in a set.

## What changed

Adding `"mainram"` to `EXEC_SECTIONS` and rescanning with the 20 call targets as
seeds:

```
executable bytes    63,128  ->  159,504
ARM7 corpus            333  ->  469 functions
mainram                  0  ->  153 functions, 40,404 bytes (41.9% of the section)
```

The lift now covers **96 programs and 10,003 functions**, with
`lb_arm7_mainram` carrying 153 entry points.

## The lesson

**A filter keyed on a name fails silently when the name is unexpected.** The
scan did not warn, error, or report a skipped region — it reported 0 functions
and 0.0% coverage of a range that did not include the thing being asked about.

This is the same shape as `docs/91`'s CMake glob: *a filter that matches nothing
is not an error*. Twice in two passes, and both times the honest signal was a
count that looked plausible until compared against something independent —
159,504 executable bytes against 63,128, or 91 archive members that were all
dispatch objects.

**The general form: when a tool reports "nothing found", check what it was
willing to look at before believing the answer.**

## Tested: the ARM7 runs, and the budget was hiding it

The rebuilt runner (96 programs, 10,003 functions) was tested, and the first
result looked like a flat negative: **the same 13,733,678 instructions and the
same 6 code pages** as before. That is what was reported first, and it was the
wrong conclusion drawn from the wrong number — the ARM9's trace tail says
nothing about the ARM7.

Asking the runner about the ARM7 directly tells a different story:

```
                  ARM9 budget 5M          ARM9 budget 60M
ARM7 PC           0x02380158              0x00000008     (BIOS IRQ vector)
ARM7 SP           0x0380FB7C              0x0380FFC0
IE                0x00000000              0x00040000     (IPCSYNC IRQ enabled)
IPCSYNC.out       0x0000                  0x0800
exceptions        0                       1
dispatches        91,855                  129,218
```

**At 5M the ARM7 is still inside its autoload copy loop.** That loop is at
`0x0238013C`–`0x02380158` in the static section and moves four bytes per
iteration through the dispatcher, so copying ~159 KB is tens of thousands of
iterations. It is slow, not stuck.

**At 60M it is past the copy, has enabled the IPCSYNC interrupt (IE bit 18) and
has posted `0x0800` to IPCSYNC** — bits 8–11, which is exactly the field the
ARM9's `Boot_WaitLoop` reads as its low nibble.

So both halves of the handshake are now live. What has *not* been shown is the
two of them completing an exchange: the ARM9 still spins.

**What cannot be claimed:** that the mainram lift caused this. The 60-million
budget was never run against the previous build, so there is no controlled
comparison, and the honest statement is that the ARM7 reaches IPCSYNC under the
current build. Attributing it would need the old binary and the same budget.

41.9% of mainram is also still not all of it.
