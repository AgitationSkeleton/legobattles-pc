# 185 — The HUD anchor was unreachable, and only moves OBJ

Status: **framework gap found and fixed; the HUD-anchoring requirement is only
partly satisfied by it.** Found by reading the Metroid Prime Hunters port, which runs on
this same ndsrecomp runner.

## The framework already had it

MPH's `game.toml` sets two keys this port had never used:

    adaptive_hud_anchor = true
    adaptive_hud_center_width = 128

`gpu2d.cpp` implements them in the OBJ path. With the screen widened, instead of
translating every sprite by half the extra width it decides per sprite:

    centre < centre_left    ->  output_sx = sx                (pin left)
    centre >= centre_right  ->  output_sx = sx + extra * 2    (pin right)
    otherwise               ->  output_sx = sx + extra        (stay centred)

That is exactly what the unit-action, spell/buildable and deselect buttons
need.

## Why setting the key did nothing

`main.cpp`:

```c
nds_gpu2d_set_adaptive_hud_anchor(
    frontend_options.adaptive_hud_anchor &&
    (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,
    frontend_options.adaptive_hud_center_width);
```

**`NDS_ADAPTIVE_TOP`.** Both upstream titles using this — MPH and SM64DS — widen
the top screen, so there the condition reads as "is widening on at all". LEGO
Battles widens the **bottom** screen, so the flag could never reach the renderer
no matter what the config said.

Measured rather than assumed. Capturing the in-field HUD at 448 wide with the key
`true` and then `false` gave **identical** sprite columns:

    anchor ON   [(96,122), (183,183), (191,191), (200,202), (212,262), ...]
    anchor OFF  [(96,122), (183,183), (191,191), (200,202), (212,262), ...]

Byte for byte the same, and at `native + 96` — the unanchored branch, with
`extra = (448-256)/2 = 96`.

The **rendering** side needed no change: it is gated on `engine == 0`, and engine
A is the one carrying the widened surface whichever screen it drives — the touch
screen here, the top screen in MPH. Only the enable condition assumed a screen.
Hooked to follow whichever screen the title actually widens.

After the fix, the same capture:

    anchor OFF  [(96,122), ...]
    anchor ON   [(0,3), (100,122), ...]        <- a sprite pinned to the edge

**RETRACTED -- see docs/190.** The section below infers the in-field HUD is
a BG layer because it did not move when an OBJ did. Reading OAM shows engine A
has every sprite parked at (0,192) and DISPCNT bit 12 clear, so OBJ is disabled
on the battlefield engine entirely. The buttons could not have moved because
there were no sprites to move -- the inference reached the right conclusion for
the wrong reason, and the OBJ anchor cannot address that HUD at all.

## And that is where it stops

One element moved to x=0. **The button column did not move.** It sits at
output 96-122 in both, which is `native 0..26` taking the centred branch.

A sprite authored at native x=0 has its centre far left of the centre band and
must take the pin-left branch. It did not, so those buttons are **not OBJ
sprites** — the anchor path never sees them. On the evidence here they are a BG
layer; that is an inference from "they did not move while an OBJ did", not a
layer-toggle measurement, and is labelled as such.

So the framework's HUD anchoring is now reachable for this title and provably
works, but it satisfies the requirement only for whatever parts of the HUD
are OBJ. The buttons that actually need anchoring need either BG-layer anchoring,
which the framework does not have, or to be drawn by the port in wide-field mode
— where the guest HUD is not composited at all today.

## Worth noting about the reference port

MPH sets `adaptive_center_native = true` and its own comment records why: their
widened 3D path still has projection and culling cases where offscreen content
wraps into the side regions, so they present a centred native-width screen while
that is audited — and that fallback returns *before* the HUD anchor path runs. So
the anchor is configured but inactive there too, for a different reason.

Two ports, both with this feature switched on in config and neither actually
running it. It was worth measuring rather than trusting the setting.
