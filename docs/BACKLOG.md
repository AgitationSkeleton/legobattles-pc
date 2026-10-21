# Backlog

Planned work that is not the current task. Items are removed when they ship,
not when they are started.

## Save manager panel in the overlay -- LOAD/EXPORT SHIPPED 2026-09-02

The **load and export** half is implemented: a "Saves" tab in the overlay that
exports the live save image to a path and imports one back, retagging it to
whatever region prefix the running build uses.

That retagging is the whole point, and the reference save proves it: this port
builds as `AFE_BATTLE_SAVES`, while that cartridge save is
**`AFN_BATTLE_SAVES`**. Without a retag this build rejects it and shows empty
profile slots with no error at all.

Verified: builds, the tab appears in the overlay tab bar, the overlay renders
in a live battle with no crash, and the AFN->AFE transformation is the same one
that produced the working seeded save the route loads profile "Tester" from every
run. NOT yet verified: the tab's own contents by eye -- driving an ImGui click
from the harness did not land, so a human should open it once.

Still owed: the **unlock-everything cheat buttons**, below.

## Save manager: unlock-everything buttons -- ALREADY IMPLEMENTED, elsewhere

Not blocked, and not needed in the save panel. `apply_unlocks()` in
`lb_patches.cpp` sets the unlock bits in the **live profile record in RAM**, so
the game recomputes its own checksum when it next saves. Exposed as the
`unlock_units` / `unlock_extras` settings. That route is unaffected by the
checksum finding.

What the checksum finding (docs/207) does govern: the save manager must never
offer to edit save bytes in place. Clearing the unlock streams in a file makes
the game report "The LEGO Battles data is corrupted and will be deleted" and
delete it.

Still open, from reading the reference save: the port's unlock table's highest
index is 110, but the reference save's field B stops being set around index 94.
Either field B is shorter than assumed or the table reaches past it. See
docs/207.

Original note follows.

**Do not implement this by flipping unlock bits.** Measured 2026-09-02: the four
bytes at the start of each profile record are a checksum and the game verifies
it. Clearing the unlock streams and leaving those bytes alone makes the game
report "The LEGO Battles data is corrupted and will be deleted" -- and delete
it. A naive unlock button would destroy the player's save.

Blocked on locating the checksum function. `func_0204b1d4` is the writer that
emits the tag, so it is computed somewhere on that path. The acceptance test is
already written: clear the unlock streams, recompute, and get past the title
screen instead of the corruption message.

Original note follows.

Planned 2026-09-01, while the adaptive-screen work was running.

An ImGui panel that can **load and export a LEGO Battles save from any region**,
so the port can be tested against other units and levels instead of whatever a
fresh profile happens to unlock.

A real DS save is the reference: it has **everything unlocked except the
three-player game achievement**. That makes it the ground truth for the
"unlock everything" cheat buttons — the correct bit pattern can be read off it
rather than assembled from the per-item table in `lb_patches.cpp`, which is
built from 52 individually measured codes and is known to have two deliberate
gaps (Catapult, and stream index 108 / probable Reveal Map).

The source save is read-only and never committed.

Region matters because the save tag differs per region — `tools/lb_save.py`
already translates tags (`XXX_BATTLE_SAVES`), so the panel should reuse that
rather than re-deriving it.

Related: `docs/181` (the two parallel unlock bitfields), `tools/lb_save.py`.
