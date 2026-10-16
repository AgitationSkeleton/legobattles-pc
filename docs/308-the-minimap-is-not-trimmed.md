# 308 — The minimap is not trimmed, and the collapsed overlay was the harness

Defect item 20 is recorded in docs/298 as *"Minimap trimmed; world truncated
inward"*, **PART FIXED** — the world truncation was the withdrawn
`apply_camera_bounds`, and the minimap half was left open.

This looks for the minimap half with instruments that would find a crop or a
scale error, and does not find one.

## 1. The port does not alter the top screen

The runs capture both the raw guest top screen and the one the port presents,
at 256x192. Across four shots:

```
shot 0: 256x192  differing px 0/49152 (0.00%)
shot 1: 256x192  differing px 0/49152 (0.00%)
shot 2: 256x192  differing px 0/49152 (0.00%)
shot 3: 256x192  differing px 0/49152 (0.00%)
```

Pixel-identical. Whatever the guest draws on the top screen — minimap included
— reaches the port unchanged.

## 2. The overlay renders it 1:1, with the aspect preserved

That is not the same question. The player sees the top screen as a scalable
ImGui overlay in the window, and a 256x192 image forced into a 272x231 panel
could easily be stretched or cropped. Measured by locating the same feature in
both — the unit-info panel's red frame, which is a hard colour boundary — in
the raw top screen and in a PrintWindow capture of the real window:

```
raw red-frame bbox:    x 0..255 (256 wide)   y 0..185 (186 tall)
window red-frame bbox: x 768..1023 (256)     y 210..395 (186)

scale x 1.0000   scale y 1.0000   ratio 1.0000
```

**Exactly 1:1 in both axes.** Nothing is scaled, so nothing can be trimmed by
scaling, and the frame is fully present in the window.

(A whole-region pixel diff comes out at 24.6%, which is *temporal*, not
geometric: the window capture is taken mid-flow and the raw captures at the
end, and between them the portrait, the health bar and the minimap contents all
change. The frame geometry is the part that answers the question, and it
matches exactly.)

## 3. What actually made the minimap missing from earlier captures

Every window capture before this one showed the Top Screen overlay as a bare
title bar — collapsed, with no minimap at all. That is a **harness artefact**:

```
port/runner/build/imgui.ini   Pos=336,149    Collapsed=1     <- the harness
build/imgui.ini               Pos=1348,632   Collapsed=0     <- DEPLOYED
```

`imgui.ini` sits beside the executable, is gitignored, and persists whatever
state the window was last left in. The deployed build has it
expanded in the lower-right corner, exactly as the port's top-screen overlay
is meant to be. Only the harness copy was collapsed.

This is the deployed-versus-harness trap in its purest form, and it was one
step away from being written up as "the minimap does not render". `lb_exp.py`
now asserts `Collapsed=0` for that one window before launch and says so when it
changes it, because a capture nobody can trust is worse than no capture.

## ESTABLISHED / NOT ESTABLISHED

**ESTABLISHED:** the presented top screen is pixel-identical to the guest's, in
four of four shots.

**ESTABLISHED:** the window overlay draws it at 1:1 in both axes, aspect
preserved, frame fully present.

**ESTABLISHED:** the collapsed overlay in earlier captures was the harness's own
`imgui.ini`, not the deployed build.

**NOT REPRODUCED:** minimap trimming. This is *not* the same as fixed — the
report describes something real — but two independent instruments that would catch a crop
or a scale error both come back clean.

**Worth separating out, because it is easy to mistake for a trim:** when a unit
is selected the top screen shows the **unit-info panel**, which covers the
minimap. That is the cartridge's own behaviour and is the subject of a
different item (docs/255: *"draws even while a unit is selected which hides the
minimap"*), not a trimming of the minimap itself.

## What would settle it

A capture showing the trim, or a description of when it happens — at a
particular window size, on a particular map, or after a resize. The overlay is
scalable, so a size a player uses and this harness does not is the
obvious untested axis: everything above was measured at 1600x900.
