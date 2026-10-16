# 193 — The anchor was cutting the field, and the bars were an empty screen slot

Status: **fixed and verified on the real window.** Two of the eight reported
faults close here. Corrects the fog reading in docs/192, which is retracted in
place.

## Bug 8, the "vertical striping": adaptive_hud_anchor

`adaptive_hud_anchor` keeps a centre band together and moves whatever is
authored left of it to the left edge, and right of it to the right edge. In
Metroid Prime Hunters — where this port copied the setting from — engine 0 is
the TOP screen: a 3D world with a 2D HUD over it. Anchoring pins that HUD to the
corners of a wider view and leaves the world alone.

**In LEGO Battles engine 0 is the BOTTOM screen, and it is not a HUD overlay.
It is the battlefield.** So the anchor cut the field into three pieces and
pinned two of them to the edges. Measured on the presented surface, in
gameplay, at 448 wide:

    anchor on    lit 0..95 | BLACK 96..191 | lit 192..266 | BLACK 267..351 | lit 352..447
    anchor off   BLACK 0..95 | lit 96..351 | BLACK 352..447

96 + 75 + 85 = 256. The lit "extras" were the native view's own left and right
slices, relocated; the black was the hole they came out of. The artwork shows it
before the measurement does — the flowers on the ground texture repeat, so the
bars are the field's floor texture split into pieces — and the flowers in the
widened columns are indeed the same flowers.

Two controls confirm it is the anchor and not the widener:

* `adaptive_width = 256` bypasses the widener entirely and has **no bands**.
* The **software** 3D renderer at 448 reproduces the bands exactly, so it is
  not compute-renderer tile binning.

Fixed by `adaptive_hud_anchor = false` in `port/game.toml`. This does not place
the in-field HUD, and it never could have: **OBJ is disabled outright in
gameplay** (DISPCNT `00010F08`, bit 12 clear), so the only thing the key had to
act on was the BG bands, which are the field. Bug 4 stays open and needs a
different mechanism.

## Bug 3, the black bars: the window was sized for a screen it never draws

`create_presentation` opens the stacked layout at `kScreenHeight * 2 *
kWindowScale`, because stacked means top screen above bottom screen. Enhanced
draws **only** the bottom screen — the top one lives in the overlay panel — so
half the window was an empty slot for a screen nothing renders into.

Measured on the real window before the change: client **908×790**, game content
**908×390**. 790 is 2×390. That is the whole of *"black bars in gameplay,
gameplay view doesn't fill entire screen"*, and no amount of work in the
compositor could have reached it.

Two changes, hooks 0n and the cover fit:

1. Enhanced opens **one screen high**. For a 448-wide surface that is 896×384 —
   the surface's own aspect, so it fills completely on the first frame.
2. The fit is now **cover, not contain**. `min(sx, sy)` draws the whole surface
   and pads the remainder, which letterboxes every window that is not 2.33:1.
   `max(sx, sy)` fills the window and lets the excess fall outside it; SDL clips
   the destination rect to the target by itself, so the crop needs no source
   rect, and passing the full (oversized) rect to `lb_overlay_set_field_rect`
   keeps the touch mapping exactly as it was. Neither branch scales the axes
   differently, so nothing is ever stretched.

The cover is clamped so it can never crop **into** the DS's own view — showing
less than the cartridge did would be a regression however well it fills the
window. Windows narrower than 4:3, or wider than 21:9, bar slightly instead.
Everything between fills exactly.

Verified by capture at six sizes, measuring the content band against the client
area:

    640x480  800x600  1024x768  1280x720  1600x900  1920x1080
    100%     100%     100%      100%      100%      100%

## What is still black, and why it is honest

At 16:9 the window is full but about 25% of its width is black either side of
the field. That is not a bug left in: **the guest maintains tilemap entries only
for the 256 columns it draws**, so there is nothing truthful to put out there.
`LB_BG_EXTEND=1`, which lets the furthest-back text BG reach the widened
columns, was tried and changes nothing — the tiles out there are empty.

More field of view therefore requires the GUEST to fill more, which is the
`LB_VIEW_WIDTH` direction. That patch works and is not sufficient: it rewrites
`mov r1, #32` at `020333A4`, and the guest's own fog object at `02151F28`
proves it lands (stride 4→7, width 32→56, row table reallocated) — but the
rendered output is identical to the column. Some other constant governs the tile
fill and the 3D submission and has not been found.

## Bug 1, the lag: where the frame actually goes

The frontend's phase breakdown accounts for 0.25 ms of a 24 ms frame, so it
cannot answer this. `NDS_PROFILE_GPU` / `NDS_PROFILE_SCHED` can. In gameplay,
4100 frames:

    gpu2d  render_ns     5,567,396,600     1.358 ms/frame
           engine_a_ns   2,985,312,600     0.728
           engine_b_ns   2,582,084,000     0.630
    gpu3d  compute_sync  2,489,010,900     0.607   (CPU blocked on the GPU)
           compute_map   1,876,047,100     0.458
           compute_submit  612,452,300     0.149

The scheduler profile is SAMPLED — `sampled_rounds` 18,848 of `rounds`
19,016,991 — so its nanoseconds scale by about 1009×:

    ARM9 execution   ~41%
    devices          ~15%
    display           ~8%
    ARM7              ~7%
    GPU2D render      ~6%
    GPU3D compute     ~5%

`event_counts` agrees independently: `cyc9` 4,595,798,760 over a ~100 s run is
about 46 MHz against the ARM9's 67 MHz — **69% of real speed, and 60 × 0.69 =
41 fps**, which is what the window reports.

So the deficit is guest execution, not presentation, and it is not the widening:
native 256 measured 40.9 fps against 41.9 at 448, and the adaptive phase is
0.011 ms/frame. The recompiled banks already build at `-O2 -g0` under a Release
configuration, so there is no free build-flag win. Reaching 60 fps needs roughly
a 1.45× speedup in guest execution.

Not claimed here: any per-setting frame-rate cost. Single 6-second samples of
separate runs vary by several fps in this project (docs/191 drew a wrong
conclusion from exactly that), and every number above is one sample.

## Bug: the menus were stretched 1.75x

Found while checking that the cover fit had not cropped a menu button off the
screen. It had not — but `lb_menu_fill`, which runs for every scene the widener
refuses (menus are DISPCNT `00010D0B`, mode 3 with an extended BG3, and both
wideners stand down), was resampling the whole native image across the output:

    sx = (x * 255) / (output_width - 1)

That is a **448/256 = 1.75x horizontal scale of everything on screen** — every
glyph, button and portrait — and the exact thing the port's design rules out: the
vanilla UI keeps its own aspect and stays centred, and only the BACKDROP covers
the extra width.

It was measurable rather than a matter of taste. The Back button occupies native
columns 9..33. A capture found it at output columns 16..58, which is
`9 x 1.753` to `33 x 1.753` — and at 1:1 centred it belongs at 105..129.

`lb_edge_fill`, in the same file with a header comment arguing for exactly this
case, was already written and simply never called. `lb_menu_fill` now calls it:
native drawn 1:1 in the centre, each ROW's own edge pixel carried outward. For
the vertical gradients these menus use that continues the gradient; for a flat
panel it is one colour, which is the "single colour of the edge areas"
alternative. It never invents detail and never distorts
the layout.

Verified by capture at 1280x720: the book icon is square again rather than 340
pixels wide, and the Back button — which under the stretch was clipped to a
partial blue box at the window edge — is fully on screen.

## State of the eight reported faults

    1 laggy              measured and located, NOT fixed. 40-43 fps in
                         gameplay, 59-60 in menus. Guest execution, spread
                         across ARM9 41% / devices 15% / display 8% / ARM7 7%,
                         with no single lever. Needs ~1.45x.
    2 clicks             not re-tested this pass. The touch mapping is
                         unchanged by the cover fit by construction: the FULL
                         (oversized) destination rect is what goes to
                         lb_overlay_set_field_rect, so window pixels map to
                         guest columns exactly as before.
    3 black bars         FIXED. 100% of the client area filled at six sizes.
    4 HUD not at corners open, and the mechanism assumed for it is wrong: OBJ
                         is disabled in gameplay, so adaptive_hud_anchor was
                         never able to move those buttons.
    5 left-edge          located, NOT fixed. Columns 1 and 3 black on the top
                         tile-row of each metatile for the first 7.5 metatile
                         rows. Panning proves it is SCREEN-anchored and stale:
                         after scrolling out to sea the strip still shows the
                         old grass. So it is a rendering fault, not map data.
    6 click past aspect  largely moot now: the field fills the window and the
                         area outside the guest's 256 columns is black because
                         the guest has nothing there to click.
    7 static map         WORKS. D-pad panning moved the map (57.6% of sampled
                         pixels changed, terrain to ocean). The original report
                         was against the host wide-field renderer, which is off.
    8 striping           FIXED. It was adaptive_hud_anchor.

## Negative result: the left-edge sliver is not a missing window

`DISPCNT` read in gameplay is `2F08`, and bit 13 is set — WIN0 enabled. Since
`lb_wide2d.inc` states outright that this renderer *"implements no window logic
at all"*, that looked like the whole answer: the game masks a sliver at the
screen edge and the port draws it anyway.

It is not. Sampled twenty times across several frames, the state never varies:

    x20  DISPCNT=2F08  WIN0H=0000  WINOUT=0000

`WIN0H` = 0 means X1 = 0 and X2 = 0, and `WINOUT` = 0 means *nothing at all*
displays outside the window. Taken literally that is a zero-width window with a
blank exterior, which would blank the entire screen. The cartridge plainly does
not do that, so the hardware must be reading X2 = 0 as 256 — a window covering
the full width, with `WININ` = `003F` enabling every layer and the colour effect
inside it. That is indistinguishable from no windowing, which is what the port
already does.

So windows are a red herring here, and implementing them would not fix the
sliver. Recorded so the same lead is not followed twice.

What the sliver actually sits on, all read from the guest in gameplay:

    BG1CNT  4187   terrain: priority 3, 8bpp, 512x256, char base 1, screen base 1
    BG2CNT  4081   priority 1, 8bpp, 512x256
    BG3CNT  0284   priority 0, 8bpp, 256x256
    BG0HOFS..BG3HOFS  all 0000        -- no fine scroll at all
    MOSAIC            0000            -- not a mosaic artefact

All four BGs have a horizontal scroll of zero: the game does not scroll its
backgrounds, it **rewrites the tilemap**, which is what the scroll callback at
`020396E0` does — 0x680 bytes, 832 entries, 32 columns x 26 rows, per BG. With
HOFS = 0 the leftmost screen column is tilemap column 0, which that rectangle
covers, so the stale pixels are not a partial tile scrolled in from outside.

Remaining candidates, none yet tested: the DMA lands somewhere other than the
screen base the port computes from BGxCNT, or the guest's metatile expansion
leaves the top tile-row of the leftmost metatile column unwritten. The vertical
extent fits the second — the artefact stops below y=120, which is 7.5 metatiles
of the 16-pixel metatile grid.
