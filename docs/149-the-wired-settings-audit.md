# Auditing the remaining WIRED badges

Two of the original six `WIRED` badges proved false (`docs/147`, `docs/148`), so
the other five were checked the same way rather than given the benefit of the
doubt. **All five hold**, and the reason they hold is the point of this note.

## The five, and what each depends on

| control | mechanism | dispatch override? |
| --- | --- | --- |
| Make All Builders The Same | `apply_same_builders()` rewrites blueprint records in RAM from the vblank hook | **no** |
| Build-limit caps (per class) | `apply_caps()` writes ARM9 static data from the vblank hook | **no** |
| Rebindable keyboard / pad | `lb_binding_key_bit` and `lb_binding_pad_bit` **replace** the frontend's `key_bit`/`controller_bit` via `apply_hooks.py` | **no** -- host-side |
| Browse lobbies | `lb_net_refresh_lobbies` against the master server | **no** -- host-side |
| Advertise a lobby | `lb_net_announce` / `lb_net_host_tick` | **no** -- host-side |

Verified in the tree rather than assumed: the two binding functions are inserted
as outright replacements at `apply_hooks.py:668` and `:675`; `apply_caps()` is
called unconditionally from `lb_patches_on_vblank()` and `apply_same_builders()`
behind its toggle; `lb_net.cpp` carries twenty `lb_net_*` definitions.

## The pattern, which is the useful part

**Every setting that genuinely works avoids dispatch overrides**, and every
setting that needed one is blocked:

* works: **data patches** (blueprints, caps), **host-side substitution**
  (input bindings), **host-side services** (lobby listing).
* blocked: **Attack Boost Affects Ranged**, **Hotwire targets**, **Hotwire
  Kills Transport Occupants** -- the three that must intercept a guest call.

That is not a coincidence and it is not about which settings were harder. It
follows directly from `docs/144`: the recompiler statically covered the library
layer and not the gameplay layer, so anything needing to intercept gameplay is
out of reach while anything expressible as data or as host behaviour is
untouched by the problem.

## The final settings picture

```
5 wired     data patches and host-side behaviour, all verified
5 read      ROM location known, not implemented
7 unread    not researched
3 blocked   understood, implemented or implementable, mechanically unreachable
```

The `read` and `unread` groups are ordinary remaining work. The `blocked` group
is not work at all in this port -- it is a dependency on recompiler coverage,
and no amount of further reading of the game will move it.

## What would unblock the three at once

Wider static coverage in ndsrecomp, so that gameplay call sites emit
`runtime_dispatch_literal_call` the way library call sites already do. That is a
single upstream change with a measurable acceptance test: the counts in
`docs/143` for `0206E408`, `02078D5C` and `02078770` become non-zero, and the
three overrides already written begin to fire without modification.
