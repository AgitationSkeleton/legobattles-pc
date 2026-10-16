# The override mechanism fires; the heal path just is not exercised

An attempt to verify `docs/132`'s heal fix behaviourally, which first had to
establish whether the overrides run at all.

## The ambiguous result

With `LB_TRACE_HEAL=1`, `fix_hotwired_healing=1` written to `lb_settings.ini`,
and a warp into a Free Play match: **zero** trace lines from either the heal-tick
override or the HP-write override, over 150 million cycles.

Two explanations, and they need very different responses:

1. the dispatch overrides never fire, in which case `docs/132` is inert and so
   are the two overrides that predate it;
2. the heal path is simply not exercised by a quiet Free Play match -- no healer
   unit, no combat, so no HP changes at all.

An earlier note in this project's history says `run_to_pc` only catches
*dispatched* entries and that intra-bank calls are direct C-to-C, which made (1)
look likely.

## It is (2)

`runtime_dispatch_literal_call` is two lines:

```c
extern "C" void runtime_dispatch_literal_call(uint32_t target_pc) {
    runtime_dispatch(target_pc);
}
```

Every literal `BL` goes through `runtime_dispatch`, so an override on a `BL`
target **does** fire. Confirmed directly by temporarily registering a probe on
`func_0208727C`, the entity resolver with 368 callers:

```
[lb_heal] MECHANISM OK: resolver override fired (call 0)
[lb_heal] MECHANISM OK: resolver override fired (call 1)
[lb_heal] MECHANISM OK: resolver override fired (call 2)
```

The probe has been **removed** -- leaving an override on a 368-caller function
would cost a C call and a table lookup on every entity resolution in the game,
for a diagnostic.

## What this settles

* **The override mechanism works.** `docs/132`'s two overrides, and the SetOwner
  and projectile-damage overrides before them, are reached when their addresses
  are called.
* **The heal fix remains behaviourally unverified**, but for an ordinary reason:
  the test scenario contains no healer and no combat, so `func_0205E7F4` is
  never called. Silence from a probe on a function nothing calls is not evidence
  about the probe.

## What a real verification needs

A match containing a unit with a heal aura, an enemy unit inside that aura, and
a hotwire of that unit -- then its hp watched with the setting off and on. That
is gameplay staging rather than a harness question, and `tools/lb_warp.py` walks
to a match start, not to a specific tactical situation.

The cheaper alternative is to drive it from the debug server: place the entities
by writing the pool, or force the heal tick by calling it directly with a
constructed spell object. Both are more invasive than the fix itself, and
neither has been done.

## Method note

The first version of this check reported "39 matches" for `lb_heal` in the log
and looked like a success. Every one was the string `lb_healt.sav`, the save
file's own name, matched by a `grep` for `lb_heal`. Anchoring the pattern to
`^[lb_heal]` gave the true count: zero.

That is the fourth measurement error in this session, and the same shape as the
others: the tool answered a question adjacent to the one being asked.
