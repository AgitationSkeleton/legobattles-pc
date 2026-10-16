# 171 — One flag byte per cheat, and eighteen settings actually wired

Status: **wired and verified live.** First settings in the overlay to move from
`[unread]` to `[wired]` on the cheats tab.

## First: docs/170's "correction 1" was wrong, and is withdrawn

docs/170 claimed the record table's base is `0x02126000` rather than docs/97's
`0x02125FF4`. **That is wrong. docs/97's base was right.**

The disambiguator is the code, not the data:

    literal-pool references to 0x02125ff4 : 6   (func_02000d18, func_02007d80,
                                                 func_02007f10, func_02009158,
                                                 func_020c2ec4, func_020d2500)
    literal-pool references to 0x02126000 : 0

Nothing in the image reaches `0x02126000`, and `func_020c2ec4` walks from
`0x02125FF4` with a 16-byte stride. The base was never in doubt once the xref
was run — which is the lesson: **a lattice argument settled by data coherence
alone is not settled.** Two readings 12 bytes apart both produced sensible-looking
records, because a partial-stride offset rotates the fields rather than
destroying them.

What docs/97 actually got wrong was the **field order**, not the base:

    docs/97 said:  +0x00 kind   +0x04 index   +0x08 code ptr  +0x0C sid
    actually:      +0x00 index  +0x04 code ptr +0x08 sid      +0x0C kind

docs/170 compensated for the rotation by moving the base instead of the fields,
which produced the same field *values* attributed to the wrong records.

The decisive self-check for the correct reading: on this layout, **`index`
equals the record number for all 155 records**, and exactly **57** carry a
seven-character code — matching docs/97's own count. Neither holds on the
shifted lattice.

docs/170's corrections 2 and 3 stand: `kind` takes seven values
{1,2,3,4,8,16,31}, and the table is a cheat registry rather than a registry of
everything unlockable.

## The finding: flags are per cheat, not per category

docs/97 concluded the flags block holds "one byte per category, not per item",
and on that basis kept the cheat options unwired. With the field order corrected
that conclusion inverts.

Records 0..17 are the eighteen distinct cheats, one each, and `index == record
number`. `func_020088D4` switches on `ldrb r1,[r5]` — which is now known to be
`index`, not `kind` — through a jump table bounded by `cmp r1, #0x11` (17).
Extracting the flags-block byte read in each case gives offsets **0..17 with no
gaps**.

So case `i` reads `flags[i]`, and record `i` *is* cheat `i`:

    flags[ 0]  Reveal Map              SKQMXPL      flags[ 9]  Super Magic          DWFTBNS
    flags[ 1]  One Hit Kill (Heroes)   AVMPWHK      flags[10]  Show Red Bricks      RTGYPKC
    flags[ 2]  Fast Building           QMSLPOE      flags[11]  Upgraded Towers      EDRFTGY
    flags[ 3]  Fast Magic              JRTPASX      flags[12]  Super Explosions     THNBGRE
    flags[ 4]  Double LEGO Bricks      BGQOYRT      flags[13]  Fast Harvesting      PQZLJOB
    flags[ 5]  Show Minikit            LJYQRAC      flags[14]  Full Unit Cap        UMSXIRQ
    flags[ 6]  Show LEGO Studs         CPLYREK      flags[15]  Invincible Hero      HJCRAWK
    flags[ 7]  Regenerating Health     ABABLRX      flags[16]  Fast Mining          KVBPQRJ
    flags[ 8]  Long Range Magic        ZPWJFUQ      flags[17]  Show Enemies         IBGOFWX

Names are the cartridge's own LOC strings; codes are from its record table. The
block is reached through `*(u32*)0x02150A70` (docs/97 found it), which resolves
to `0x0223BDE0` in a battle.

## Verified, not inferred

Writing `flags[0] = 1` into the live game raised the minimap's revealed area from
**925 to 1206** green pixels and the whole map appeared on it — lake, cliffs and
all. That is Reveal Map doing what its name says, through the cartridge's own
switch.

End-to-end through the port, with `LB_CHEATS=0,17`:

    [lb_cheat] flags block at 0x0223BDE0; holding cheat 0 on
    flags[0..17] = [1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1]
    minimap revealed pixels: 1142   (fog-of-war baseline 925)

## Implementation notes

`port/runner/src/lb_patches.cpp`, `apply_cheats()` on the vblank hook.

**Enabling is sticky; disabling is one-shot.** The first version wrote only when
the desired value changed. That looked correct and failed: the flags block is
re-initialised when a battle starts, so a cheat set at boot was zeroed and never
rewritten — the test reported all eighteen bytes clear. Comparing against the
guest byte each frame survives re-initialisation. Turning a cheat *off* stays a
single write so the cartridge's own cheat screen remains usable; the port only
forces a byte it is being asked to hold on.

`LB_CHEATS=0,17` forces cheats on without the UI so the wiring can be verified
headlessly — a test aid in the same spirit as `LB_BALANCE`.

**These are LOBBY-CARRIED.** Every one changes the simulation, so a host and a
joining client must agree or the lockstep desyncs. Nothing enforces that yet
because there is no client.

## Two of them matter for the widescreen work

* **Reveal Map (`flags[0]`)** is the cartridge's own fog-of-war control. docs/169
  records that the host-side full-map render draws unscouted ground, which is a
  fidelity problem; this is the game's own switch for it rather than a mask the
  port would have to reimplement.
* **Show Enemies (`flags[17]`)** is the matching enemy-visibility control.

## Still not wired, and correctly so

The four *Unlock* options (units, extras, Santa, maps) are a different thing:
per-item unlock state, which is profile data. docs/97's conclusion survives for
those — the flags block has eighteen bytes for eighteen cheats, and no per-item
unlockable state anywhere in it. They stay `[unread]`.
