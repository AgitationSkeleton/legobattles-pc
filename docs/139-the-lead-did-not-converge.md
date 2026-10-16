# The take-then-kill lead did not converge, and what that reveals

## The lead, and where it stopped

`docs/138` found `func_02046530(x, entity)`: it calls `Entity::SetOwner`, tests
a byte at `[x+0x21]`, and only issues the kill if that byte is set. If `x` were
a settable object, hotwire-transport mode 1 would be a one-byte change instead
of six chained guest calls.

Three more hops:

* its caller `func_020464AC(x, entity)` reads a **byte at `[x+0x20]`**, passes it
  to `func_020A3E58` to get a per-player list, walks that list resolving 2-byte
  handles, and calls `02046530` on the match. So `[x+0x20]` is a **player
  index** and `[x+0x21]` sits directly beside it;
* `func_020464AC` is **not in any vtable** -- scanning the whole image for its
  address as a data word finds nothing -- so it is called directly, not
  virtually, and cannot be identified from a vtable the way `HotWireSpell` and
  `WaitingToDieCommand` were;
* its two callers are at `0204643E` and `020464A4`, but the code around them
  disassembles as repeated `movs r2, r5` / `movs r6, r4` runs, which is data or
  a jump table being read as Thumb, not instructions.

**`x` is not identified.** A pair of bytes at `+0x20`/`+0x21` where the first is
a player index is suggestive but is not a class, and `docs/105` and `docs/125`
are both what happens when suggestive is treated as identified.

The lead is abandoned, not disproved. Anyone resuming it should start by
resolving the alignment at `02046400`-`020464AC`, which is probably a switch
table whose entries name the real entry points.

## The pattern this exposes

Three settings are now in the same state, and it is worth naming:

| setting | code state | what blocks it |
| --- | --- | --- |
| Fix Hotwired Healing | **implemented** (`docs/132`) | no scenario exercises the heal path |
| Hotwire specials-only | **implemented** (`docs/135`) | no scenario casts a hotwire |
| Destroy the occupants | recipe **complete and verified** (`docs/137`, `docs/138`) | same, plus the recipe is unwritten |

None of the three is blocked on **evidence** any more. All three are blocked on
the same thing: **the harness can reach a match, but not a situation.**
`tools/lb_warp.py` walks to a Free Play start, where nothing fights, nothing
heals, and nothing casts.

## The better next move

Building that staging is worth more than any one of these settings, because it
unblocks all three and every future gameplay-affecting setting after them. The
debug server already has what it needs:

* `write_mem` with `NDS_DEBUG_ALLOW_WRITES=1`, used throughout this session;
* the entity pool at `0x02155028`, its array at `+0x08` and count at `+0x0C`,
  read repeatedly here;
* entity fields established across this project: owner `+0xE4`, handle
  `+0xDC`/`+0xDD`, hp `+0x1A0`, transport link `+0x114`, blueprint `+0x160`
  and category `+0x5C`.

A staging tool would place or retarget existing entities to build a situation --
two units of different owners adjacent, one inside a transport, one damaged
inside a heal aura -- and then let the existing overrides be observed.

That is the next thing to build.
