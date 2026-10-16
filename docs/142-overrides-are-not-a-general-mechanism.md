# Dispatch overrides are not a general patching mechanism

## The puzzle

`docs/141` left this: the Fix Hotwired Healing override is on the **right**
function (`func_0205E7F4`, whose body contains the healing write 1188 bytes in),
and it still never fires -- while `docs/133`'s probe on `func_0208727C` fired
immediately.

## What the generated code shows

`func_0208727C`'s call sites emit, in `port/generated/lb_arm9_01.c`:

```c
runtime_dispatch_literal_call(0x0208727Cu);
```

That is what makes an override fire: `runtime_dispatch` consults the table, finds
the override registered ahead of the title banks, and calls it.

`func_0205E7F4` is different in two ways:

1. its generated function is **32 bytes long**:

   ```c
   /* 0x0205E7F4  mode=arm  end=0x0205E814  branches=1  indirect */
   ```

   The recompiler stopped at an indirect branch, so only the entry fragment of a
   **1188-byte** function was statically recompiled;

2. **`0205EC98` -- the healing write -- appears nowhere in the generated
   sources at all**, in any case, in any file.

## And most of the game runs dynamically

```
static_coverage:
    tier3_entries9  8,765,623
    tier3_insns9  146,173,233
    tier3_entries7  1,674,445
    tier3_insns7   13,822,916
```

**146 million ARM9 instructions executed through tier 3**, the dynamic tier. The
statically generated functions are not where most execution happens.

## The conclusion, stated as the leading explanation

An override fires when something calls `runtime_dispatch` for its address, and
what emits those calls is **statically generated code**. Code that is executed
dynamically -- because the recompiler stopped at an indirect branch, as it did 32
bytes into this very function -- does not necessarily route its calls through the
dispatch table.

So **whether an override fires depends on whether the code that calls it was
statically recompiled**, which is a property of the recompiler's coverage, not
of the port's intent. `func_0208727C` has statically generated callers.
`func_0205E7F4`'s live callers, on this path, apparently do not.

This is the leading explanation and it fits every observation, but it is not
proven: confirming it means showing that a tier-3 block's calls bypass the table,
which has not been read.

## Why this matters more than one setting

Four overrides ship in this port today -- SetOwner, projectile damage, the heal
bracket and the hotwire bracket. **Any of them may be silently inert**, and the
only reason SetOwner and projectile damage are not known to be is that neither
has been observed firing either. `docs/133` demonstrated the mechanism works
*somewhere*; it did not demonstrate it works for those addresses.

Anything claiming an override "is wired" needs a probe showing it fire on the
path that matters, not merely a successful registration. The registration line
`5 gameplay override(s) installed` says nothing about whether any of them run.

## What to do instead

Three options, in increasing order of reliability:

1. **Probe first, always.** Before building on an override, register it with a
   log and confirm it fires in the situation it targets. `lb_stage.py` now makes
   those situations reachable.
2. **Patch data, not code**, where the setting allows it. The balance-patch path
   already does this and is unaffected by any of the above.
3. **Patch at a statically covered address.** If a function's callers are
   generated, its override fires; the generated sources say which, and
   `port/generated` is greppable.
