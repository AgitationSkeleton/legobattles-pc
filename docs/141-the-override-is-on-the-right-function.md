# The override is on the right function; docs/140 was wrong

## The correction

`docs/140` concluded that `docs/132`'s Fix Hotwired Healing override "hooks a
function the healing does not call", on the strength of the healing write being
at `0205EC98` while the override is registered on `0205E7F4`.

**They are the same function.**

* the only ARM prologue at or before the write is at **`0205E7F4`**;
* scanning every 4-byte slot between `0205E7F8` and `0205EC98` finds **no other
  `push`** -- it is one contiguous function, 1188 bytes from entry to that write;
* the epilogue right after the write, `addne sp, sp, #0x14` /
  `popne {r4, r5, r6, r7, pc}`, matches that prologue's
  `push {r4, r5, r6, r7, lr}` / `sub sp, sp, #0x14` exactly;
* BL targets landing anywhere in the range are `0205E7F4` (17 callers) and
  `0205E804` (1 caller, a mid-function continuation).

So `func_0205E7F4` **is** the hp setter, the write at `0205EC98` is its own, and
the override is registered on the correct address. `docs/140` is retracted on
that point.

What `docs/140` got right and keeps: `lb_stage.py` works, damaging a unit
produces healing, and the harness therefore *can* reach a healing situation --
which is what `docs/139` denied.

## The real open question, sharpened

The override is on the right function and **still does not fire**. Its
unconditional probe printed nothing across a full control run, checked both
anchored and unanchored, so this is not another grep artefact.

Meanwhile the same mechanism demonstrably works: `docs/133` registered a probe
on `func_0208727C` and it fired immediately.

So the question is no longer "is it the right function" but **why dispatch
reaches one function's callers and not another's**. The likeliest explanation is
that the recompiler emits a direct C call for some intra-bank call sites rather
than routing them through `runtime_dispatch`, in which case an override on the
callee is simply invisible to those callers -- and `func_0208727C`'s 368 callers
would include at least one that does dispatch, while `func_0205E7F4`'s 17 may
not.

That is testable by reading the generated C for one of the 17 call sites, which
is the next step. Until it is answered, **any** override in this port may be
silently inert depending on who calls it, which matters far more than this one
setting.

## A note on the trace format

Every `[lb_heal]` message ends in a **literal backslash-n** rather than a
newline, because the escape was mangled when the hook was written -- the same
`\n` mangling recorded in this project's notes, which has now bitten three
times. Messages therefore run together on one line. It did not affect this
result, since an unanchored search also found nothing, but it made the earlier
"count: 1 for three messages" reading confusing and should be fixed.
