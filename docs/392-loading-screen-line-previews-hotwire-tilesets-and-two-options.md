# 392 — The loading screen, bridge and wall previews, Hotwire, remapped tilesets, two new options

Status: **all seven items implemented. Five are verified live; Hotwire
Anything and Transport Occupants are verified in code and memory but not by a
live cast** (see 4). Files: apply_hooks.py, lb_patches.cpp, lb_widemap.cpp,
lb_overlay.cpp/.h, lb_demo.cpp. Five defects and two feature requests:

1. The loading screen (the pre-game red brick spinning animation on a pure
   black backdrop) showed a glitched backdrop.
2. Build previews for bridges were entirely missing, possibly walls too.
3. The Hotwire Anything cheat did not work (it still required specials to be
   on the y-axis above the target).
4. The Hotwire Occupants setting worked, but Destroy Occupants was greyed out
   and did not work; another enhancement or cheat setting had also been
   unwired or greyed.
5. Palette-remapped worlds (e.g. Mars → Castle, Castle → Pirate) could have
   mismatched tiles: extra water, well caps and other incorrect squares.

Feature requests:

1. An Enhancements drop-down, 'Default Free Play Opponent' (the game's Free
   Play menu always defaults to Wizard), offering King, Wizard, Pirate,
   Imperial, Earth, Alien, Custom, Random.
2. An option after the SCL rename that renames the Pirate structure
   'Plantation' to 'Hide-Away' or 'Storehouse'.

| # | item | result | evidence |
|---|---|---|---|
| 1 | loading screen | fixed | t392c (before) vs t392m (after), second load |
| 2 | bridge previews | fixed, in the band too | t392e/f (none), t392h-j |
| 2 | wall previews | fixed, in the band too | t392k (clipped), t392l |
| 3 | Hotwire Anything | fixed in code | bytes read back live (t392o) |
| 3 | Patch the glitch | moved to the cast path | code only |
| 4 | Destroy occupants | enabled | code only |
| 5 | remapped tilesets | three fixes | replica over 122 maps; t392s-w on Exile |
| F1 | Default Free Play Opponent | new | t392n log |
| F2 | Pirate Plantation name | new | t392n log |

## 1. The loading screen

The brick loader is engine A in 2D: DISPCNT `00010408`, BG2 only, with a
512-wide map. `lb_wide2d_line` decoded that layer past the hardware's 256
columns for the widened view. Out there the map still held the previous
battle's tiles, so a SECOND load showed blue stripes either side of the brick.
A first load was clean, which is why it appeared only sometimes. The file
already said engine A's off-window tiles are not to be trusted:
`lb_engineA_widening_allowed` is false. The 2D widening now asks for it too
(apply_hooks.py, the docs/389 stanza).

Repro: pause → Restart → Yes, then window shots through the load (taps
737,393 and 833,567). In t392c (before), Y02/Y03 show the stale stripes. In
t392m (after), every frame is black around the brick.

## 2. Bridge and wall previews

### What the cartridge draws

`func_02037EC0` (from the view tick, with `view+0x6C`) picks the painter by
the placement state's mode at +0x10:

* **Not 1: `func_020370B4`.** It paints ONE rectangle per call: the origin is
  +0x20/+0x21 in map metatiles and the size is +4/+5 (`FUN_020f2a50` on each
  pair, then x runs from origin to origin+size). That covers buildings and
  bridges; a bridge is size 1x2.
* **1, set by the wall tick `func_020AB384` through `func_02036F30`:
  `func_02037B9C`.** It paints a LINE from the start +0x20/+0x21 to the end
  +0x18/+0x19 (both from the cursor function `func_020AA814`), in max(|dx|,
  |dy|) one-metatile steps. Each piece's tile (0x4F placeable, 0x3F not) is
  the cartridge's, and goes to the cell painter `func_0203CBB8` at 02037E2C.

**docs/286 named +4/+5 and +0x20/+0x21 the other way round.** That is
corrected in lb_widemap.cpp's probe and its log labels. The checkmark code and
`lb_fp_set_site_size` already read them correctly.

### What the port did

The band harvest (docs/320, 383) picks its camera shift from the
building-ghost record at 0x02157958. A wall or bridge drag never touches that
record: it reads origin 0,0, kind 0 the whole time (LB_FPLOG, t392e-g).

* **Bridges.** Every other frame was painted at a camera shifted by the whole
  map (-720,-512), so nothing showed. With LB_FPBANDS=0 the guest's own block
  appeared (t392g).
* **Walls.** The harvest hooks only `func_020370B4`, so pieces outside the
  DS's 32x24 were culled. A stale building harvest could also show beside
  them.

### The fix

* **A line's site is the painter's own record.** When the ghost record reads
  0,0/kind 0, `lb_fp_band_shift2` takes +0x20/+0x21. Measured at camera
  752,432: origin 34,33 painted cells 8..10 x 12..15, exactly
  (ox*24-camx)/8, (oy*16-camy)/8.
* **The wall line comes from an observer on `func_0203CBB8`** (ARM; r0 the
  view, r1 the cell at +4/+5, r2 the tile). It keeps only the line painter's
  call (lr 0x02037E30). From the ROM: p = (x*24, y*16) - camera, cells [p/8,
  (p+(24,16))/8) (FX_Div by 8.0 at 02007B68, exact for whole pixels, then a
  floor), culled and clamped to 32x24.
* **Each piece's whole rectangle is kept in the buffer's own cell frame.**
  `func_02036FBC`'s pre-hook commits it (mode 1 replaces the line; any other
  mode retires it). The BG3 band serves the culled part. In mode 1 no building
  harvest is ever served.
* **Recordings.** `fpline=1` in the header; an older recording replays the
  0,0 site, as it was paced.

Verified:

* Bridge inside the window (t392h/i).
* Bridge in the left band at the cursor (t392j: harvest shift -144,+64, 12
  cells).
* Wall from the window edge into the right band, 7 pieces, cells 29..49,
  unbroken across the seam (t392l). Nothing is left after the release.

## 3. Hotwire

### The vanilla rule is in the stylus tap

docs/236 found the validator's farm check and concluded "there is no
whitelist" (corrected in place there). The whitelist is in the armed-spell tap
handler `func_020A9350` (Thumb). With the hotwire bit (0x20) set it builds a
filter of tag 0x2B (enemy) and tag 0x3B (0x020A9452/0x020A945A). `func_020A9B7C`
gives 0x3B only to categories 5 and 6.

The glitch is in `func_020A9570`:

* it ORs together the tags of EVERY entity under the stylus;
* it keeps the LAST hit's handle (scan order: south and east last).

So a transport or special anywhere under the stylus satisfies the filter, and
the steal lands on whatever was scanned after it. That is why specials had to
be on the y-axis above the target.

### Anything

The second filter tag becomes 0x2B, so the halfword at 0x020A945A changes
`movs r1,#0x3B` → `#0x2B` (word 0x213BA802 → 0x212BA802). Any enemy under the
stylus now qualifies. The three farm-cap NOPs (0x0207B7F0..F8, docs/236) stay.

Read back live with LB_HOTWIRE_MODE=2 (t392o): the NOPs, 0x212BA802, and the
log line `[lb_hotwire] hotwire-anything ON`.

### Patch the glitch

A hotwire whose target is not category 5 or 6 is refused the way the
validator's 0 refuses it.

The first build overrode the validator `func_0207B73C` itself. **That could
not coexist with Anything**, for three reasons:

* the farm NOPs are inside that function;
* a byte-patched function runs in Tier 3 because it fails the recompiler's
  content check (docs/350, 351);
* `lb_dispatch_original` hands an override the lifted body, which predates the
  patch (docs/376).

So the override would have silently put the farm check back.

The check now sits on the validator's only caller, `func_0207B9C0` (its bl at
0207B9F8 is the only call in the image). The target handle is its 5th
argument, at [sp]. On refusal the override stores the null handle (literal
0x0208745C → 0x0215541C, what `func_02087580` returns) to *out and returns, as
the stock function does when the validator says 0. Every BL is generated as
`runtime_dispatch_literal_call`, so overrides and the content check both see
direct calls.

**Not verified by a live cast.** The Free Play route has no hotwire-capable
hero on the field. Verified: the bytes in a running battle, the generated call
path, the single caller, the null-handle literal.

## 4. Transport Occupants: Destroy

"Destroy them" (mode 1) is no longer greyed out. On the hotwire's SetOwner of
a transport (lr 0x02078DE4), the occupant machine in `set_owner_override` hops
through each occupant (the docs/383 hop: R14 = the override's address, the
return recognised by SP). There are two modes:

* **Destroy (mode 1).** Each occupant gets the game's own immediate kill,
  `func_0205E78C(entity, 0)`. That is the call `Sim::DieEntityCommand::v08`
  makes on every handle in a dying entity's list (+0x100, count +0x104).
* **Hotwire them (mode 2).** Each occupant gets SetOwner to the new owner.

`Sim::WaitingToDieEntityCommand::v08` (0x02075380), the same slot, is an
empty function. WaitingToDie never kills, which corrects docs/136-138 and 243
in place.

Not verified by a live hotwire of a loaded transport, for the same reason as
3.

**Other greyed or unwired settings.** No option is
greyed out any more. Four still carry UNSEEN badges (wired and traced, not yet
seen working in a run):

* Attack Boost Reaches Ranged Units;
* Use the save's profile name;
* Advertise when the game hosts;
* Game's Join screen uses the master server.

## 5. Remapped tilesets

Force Tileset renames the map's tileset, so table A's metatile numbers read
the forced world's pictures. A map's own detail compositions (table B) are
refilled from table A by a neighbour vote (docs/387). Three causes, three
fixes:

1. **Mars-only slots that draw another role.** Eight slots (Mars 98, 118,
   160, 165, 170, 205 to King and Pirate) take a stand-in of their role, keyed
   on a Mars source because 205 is a cloud piece in the other worlds. Role
   match Mars→King 98.3% → 99.2%, Mars→Pirate 97.0% → 99.2%.
2. **The vote counts only the detail index's own terrain class** (plane A at
   *(map+0x2E8): 0 grass, 2 ground, 3 water, 5 plateau). Before, a detail
   patch beside a shore took water: the "extra water" squares.
3. **Interior neighbours vote first.** A grass-to-ground transition piece is
   class 0 too, so a detail patch inside grass took it. Live on Exile (mp05,
   Mars) those were the stray strips and notches in both King and Pirate
   (t392t/u, where native Mars has a patch of bump details). The first tier
   now counts only table-A neighbours whose own 8 neighbours share their
   class. Then come the class tier and the plain vote.

A replica of `patch_forced_tables` (a scratch script, `remap/fill392.py`)
reproduces the live log exactly ("203 detail entries refilled for 730 detail
cells", 6 and 8 stand-ins). The table counts interior detail cells drawn with
an edge piece, over all 122 maps (the plain fills 116/96/64/89 count as
interior):

| pair | before | after |
|---|---:|---:|
| Mars → King | 6.8% | 2.2% |
| Mars → Pirate | 6.9% | 2.5% |
| King → Pirate | 5.7% | 1.7% |
| King → Mars | 6.3% | 2.5% |
| Pirate → King | 6.1% | 1.4% |
| Pirate → Mars | 7.1% | 2.8% |

Live on the new build (t392v/w), Exile's strips are gone in both worlds.
These are pictures only: the decoder is the tables' only reader, and the
gate's counts are unchanged.

## 6. Default Free Play Opponent (Enhancements)

`OpponentSetup::v08` (0x020C0764) builds the screen. The Wizard is the layout
file's: the portrait record [[screen+0x60]+4]+0x24 holds frame 0x39. Its last
call, `func_020C03B4` (lr 0x020C093F), is hooked:

* if the chosen default is not the Wizard and the frame still reads 0x39, the
  selector is set the way the cycler sets it (`enemy_set_choice`: rect,
  frame, redraw);
* Custom and Random also need the eight-way selector (docs/384).

A replay uses its recorded value (setting kind 5, `sets=2`), else the Wizard.
t392n with LB_DEFAULT_OPPONENT=0 logged `opponent screen opens on choice 0
(frame 0x38), not the Wizard`.

## 7. Pirate Plantation name (Enhancements)

The option sits after the Space Criminal Leader rename: Off, Hide-Away or
Storehouse. The LOC strings are rewritten in place, by docs/356's method, in
every language (id 445, and the "Build a Plantation" string ids 866-868
share). t392n with LB_PLANTATION_NAME=1 logged `"Hide-Away": 2 string(s)
rewritten (language 1)`. The build tooltip was not looked at.

## Recordings and multiplayer

* `sets=2` records setting kinds 5-7 (opening opponent, Transport Occupants,
  Hotwire targets). An older recording leaves them to the replaying machine.
* `fpline=1`: see 2.
* **Hotwire targets and Transport Occupants change the simulation.** In a
  multiplayer match both machines must have the same values, or the lockstep
  will desync. Nothing enforces that yet.

## Gate

On the final binary, with the test mods folder parked:

* **The t384f recording.** Twice (rep392a/b): counts identical to each other
  and to rep391a.
* **The counts gate.** gate392: counts identical to gate370b; 8 of 8 pictures
  identical by pixels to gate381; 59.81 fps.
* **Vanilla.** van392: 240 of 240 pictures identical by pixels to
  van-hudmask.
