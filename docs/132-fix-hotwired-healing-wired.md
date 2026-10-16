# Fix Hotwired Healing, wired

## The mechanism, from docs/95

`Sim::HealSpell`'s tick filters candidates on **category**, **category-filter**
and **hp-not-full**. None of them is ownership. A hotwired unit changes owner
but does not move, so no exit callback fires, it stays in the candidate array at
`+0x3C`, and its former owner's aura keeps healing it.

The tick already computes what is needed. At `0x02078900` it resolves the source
from the two-byte handle at `this+0x18` and reads the source's owner byte at
`+0xE4` -- but only to gate a bookkeeping call. `func_0205E7F4`, the HP write,
runs on the next line unconditionally.

## The implementation

Two overrides, because reimplementing a 600-byte tick would be guessing at
the cartridge's rules:

1. **`func_02078770`, HealSpell's tick.** Resolves the source through
   `func_020872DC` -- the same resolver the tick itself uses on that field, so
   the handle interpretation cannot drift from the game's -- reads its owner,
   sets a flag, calls the **original** tick through `lb_dispatch_original`, and
   restores the flag afterwards.
2. **`func_0205E7F4`, the HP write.** While that flag is set and the setting is
   on, if the target's owner at `+0xE4` differs from the source's, it rewrites
   `r1` to the target's **current** hp (`entity+0x1A0`) before calling the
   original.

### Two choices worth stating

**Why a flag rather than a bare override.** The HP write has **19 callers**
across the image. Outside the heal tick this override does nothing at all, so
the other eighteen are untouched. `docs/125` is why that count was taken before
any code was written.

**Why hp = hp rather than a skip.** The runtime has no documented "return from
an override" helper, and inventing one would be guessing at a calling
convention. Writing the current hp keeps every path, side effect and return
value exactly as the cartridge would have them and changes only the amount.

## A wiring bug found on the way

The **"Fix Hotwired Healing" checkbox had no getter at all.** Nothing in the
port could read `g_set.fix_hotwired_healing`.

`lb_setting_patch_hotwire_glitch` looks like it would be the one, and this work
initially used it. It is a different control: it returns `hotwire_mode == 1`,
which is *"Patch the glitch (specials only)"* -- a filter on what hotwire may
**target**, saying nothing about healing. Wiring the heal fix to it would have
tied a heal behaviour to a targeting dropdown.

`lb_setting_fix_hotwired_healing()` is added and the override uses it.

## Status

Installed and non-destructive: the runner reports **4 gameplay overrides**, the
warp still reaches a Free Play match, and the field renders normally
(48684/49152 non-black pixels).

**The behaviour change itself is not yet verified.** Confirming it needs a live
hotwire of a unit standing in an enemy heal aura, then watching its hp with the
setting off and on. The setting defaults to off, so the current build behaves
exactly as before until it is enabled -- which is why "the game still runs" is
evidence that nothing broke, and not evidence that the fix works.
