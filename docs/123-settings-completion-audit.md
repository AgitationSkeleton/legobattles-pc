# Settings completion audit

The goal is for **every** overlay setting to be wired and verified. This is
the honest status of each one that is not, with its real blocker named. Several
are blocked on something other than missing evidence, and saying which is the
point of this document.

## What actually reaches the simulation today

Consumed in `lb_patches.cpp`:

```
lb_setting_cap                  x2
lb_setting_same_builders
lb_setting_hotwire_transport
lb_setting_balance_patch
lb_setting_attack_boost_ranged
lb_balance_apply / lb_balance_is_applied
```

`lb_overlay.h` declares nine `lb_setting_*` getters, so two are **defined but
never consumed**: `lb_setting_hotwire_anything` and
`lb_setting_patch_hotwire_glitch`. A getter with no caller is not a wired
setting, whatever the badge says.

## The audit

| control | badge | evidence | real blocker |
| --- | --- | --- | --- |
| **Zoom** | unread | none | **Ambiguous requirement.** Magnifying the existing image is a host-side scale and is easy. Showing *more* of the field is the widescreen problem, blocked by `docs/120`. These are different features and the control does not say which it is. |
| **Hotwire mode** ("anything") | read | -- | Getter exists, **no consumer**. Needs the hotwire target test located, as `docs/105` retracted the function previously believed to be it. |
| **Fix Hotwired Healing** | read | `docs/95` (heal path) | Getter exists, no consumer. Needs a seam inside a 600-byte tick; list filtering was ruled out. |
| **Balance patch** | read | `docs/95` | **Badge is stale.** `lb_setting_balance_patch` and `lb_balance_apply` are both consumed. It stays honest only because applying needs the player's own romhack file, so it does nothing until one is supplied. |
| **Unlock All / Everything** | unread | `docs/97`, `docs/121` | Progress store unknown. `docs/122` **ruled out** the runtime per-achievement buffers: their only payload write is a periodic clear. Next is diffing a 242-byte save record. |
| **Master server, Join by code** | read / unread | `docs/87-89` (wire format) | Nothing speaks the protocol yet. The cartridge lobby looks for nearby DS systems; announcing a lobby is implemented, joining is not. |
| **Music mode** | read | `docs/98` (all 98 tracks) | **No audio subsystem.** `audio_samples` returns *"audio start is no longer retained"*, and `docs/92`-`93` record that the ARM7 was never lifted and halts at the SWI vector. The data model is complete and the selection site is not found -- but even if wired, nothing could verify it, because no sound plays. |

## The shape of what is left

Three different kinds of blocker, and they need different work:

1. **Blocked on a subsystem, not on evidence** -- Music. `docs/98` calls its data
   model complete and it is; the gap is that this port has no audio at all.
   Wiring it would produce a control that cannot be verified, which is worse
   than leaving it unwired.
2. **Blocked on a reachable measurement** -- Unlock, Fix Hotwired Healing,
   Hotwire mode. Each needs one specific thing found in the game, and each has a
   named next step.
3. **Blocked on a decision, not on work** -- Zoom, and the multiplayer controls.
   Zoom needs its meaning settled. Multiplayer needs a client, which is a project
   rather than a setting.

## Recommendation

Wire in this order, because it is the order of decreasing certainty:

1. **Hotwire mode** and **Fix Hotwired Healing** -- both have a defined getter
   waiting for a consumer and a documented code path to attach it to.
2. **Unlock** -- one measurement away, with the wrong answer already eliminated.
3. **Zoom**, once its meaning is settled.
4. **Music** -- only after audio runs. Until then it should keep the `[read, not
   wired]` badge, which is accurate.
5. **Multiplayer** -- out of scope for a settings pass.
