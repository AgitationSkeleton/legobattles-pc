# The runner fork

**Upstream ndsrecomp is read-only. Every change this port makes to the runner
lives in this directory.**

That is a deliberate constraint. Upstream ndsrecomp (`$NDSRECOMP`) is a
third-party dependency sitting outside the repository, so edits made there would
not be captured by any commit here, would not be covered by the 125 ROM checks,
and would be silently lost the moment upstream is re-fetched. Keeping the fork
in-repo means the port's changes are reviewable in the same history as
everything else.

```
port/runner/
├── README.md          this file
├── src/
│   ├── lb_overlay.h   the ImGui layer's interface
│   └── lb_overlay.cpp the panels
├── apply_hooks.py     inserts the hooks into a COPY of upstream
├── build.sh           assemble tree -> apply hooks -> build
├── tree/              ASSEMBLED, gitignored - never edit by hand
└── build/             CMake output, gitignored
```

## How a build works

`build.sh` copies `cmake/`, `recompiler/`, `runner/`, `vendor/` and `tools/`
from upstream into `tree/`, drops this port's sources into `tree/runner/src/`,
runs `apply_hooks.py` over the copy, and builds. `generated/` is 6.5 MB of BIOS
banks and is pointed at in place with `NDS_GENERATED_DIR` rather than copied.

The runner's `CMakeLists.txt` reaches out to `../cmake`, `../generated`,
`../recompiler`, `../tools` and `../vendor`, so all of those have to sit beside
`runner/` in the assembled tree. Copying `runner/` alone fails at configure
time — that is how the list above was established, not by guessing.

## Why anchor insertion rather than a patch file

`apply_hooks.py` matches exact anchor text and **requires each anchor to appear
exactly once**, aborting loudly otherwise. Upstream is not a git repository, so
there is no commit to diff against, and a line-numbered patch would drift
silently as upstream changes.

The failure mode being defended against is specific and has already bitten this
project once. `docs/91` records a CMake glob that matched nothing: it was not an
error, just an empty list, and the result was a runner that built cleanly and
linked against functions whose bodies were never compiled — eleven minutes into
a link before anything went wrong. A hook that fails to apply would likewise
produce a runner that builds fine and shows no UI. So a missing anchor is a hard
error, and the fix is to re-locate the anchor rather than loosen the check.

## The hooks

| # | anchor | what it does |
| --- | --- | --- |
| 1 | `#include "relative_mouse_touch.h"` | include `lb_overlay.h` |
| 2 | `while (SDL_PollEvent(&event)) {` | forward events to ImGui first; `continue` when consumed |
| 3 | `NdsScreenLayout layout, int logical_width) {` | release the guest touch and return while a panel has the mouse |
| 4 | `render_screen(presentation, 1, screen_rect);` | draw the overlay (separate layout) |
| 5 | `render_screen(presentation, 1, bottom_rect);` | draw the overlay (stacked layout) |
| 6 | `render_screen(presentation, screen, screen_rect);` | draw the overlay when `screen == 1` |
| 7 | `add_executable(nds_runner` | add ImGui + `lb_overlay.cpp` |
| 8 | `target_include_directories(nds_runner PRIVATE` | add the ImGui include paths |

**Hooks 2 and 3 are correctness, not polish.** The port's main window is the
touch screen, and it is clickable. Without them a click on a panel would also
arrive at the guest as a stylus tap, so every interaction with the enhancement
UI would poke the game underneath it. Hook 3 releases the touch rather than
merely returning, which stops a stylus being left stuck down when the pointer
moves onto a panel mid-drag.

**Hooks 4-6 draw on the bottom screen only.** ImGui's SDL_Renderer backend binds
to a single renderer, so the overlay must be drawn for exactly one of them, and
the touch screen is the main game window.

## Which ImGui backend, and why not the obvious one

`imgui_impl_sdl2` + **`imgui_impl_sdlrenderer2`**.

The runner prints `[gpu3d] renderer: OpenGL 4.3 compute` at startup, which
points squarely at `imgui_impl_opengl3`. That renderer is the **3D GPU
emulation**, not the presentation path. Presentation is plain SDL: three
`SDL_RenderPresent` calls and no `SDL_GL_SwapWindow` anywhere in
`frontend.cpp`. Taking the banner at face value would have compiled and drawn
nothing.

ImGui is on the **docking** branch because the overlay is meant to be movable
and scalable. Multi-viewport is not available with the SDL_Renderer backend, so
the overlay can dock and move inside the window but cannot be dragged out into
an OS window of its own. Nothing requires that.

## On honesty in the UI

The port does not guess or pretend. Most controls in
the overlay do not reach the simulation yet, so each carries a badge:

* **`[read, not wired]`** — the ROM location is known and cited in `docs/`, but
  nothing is hooked up;
* **`[unread]`** — the ROM has not been read for this at all;
* no badge — the control actually works.

A switch that silently does nothing would be exactly that kind of pretended
change, so the state is on screen instead.

## The overlay only exists in interactive mode

The draw hooks sit at the three `SDL_RenderPresent` sites, so they are reached
only when the runner is actually presenting. **A plain cycle-budget run never
enters the SDL loop**, so it produces no window, no overlay and no
`[lb_overlay] ready` line — and that is not a fault:

```
nds_runner.exe <bios> 60000000 --rom ... --boot direct       -> batch, no UI
nds_runner.exe <bios> 60000000 --rom ... --interactive ...   -> window + overlay
```

The batch form is still the right one for boot and coverage testing, which is
what `docs/94` used. `--interactive` is the playable form.

## Esc no longer quits

The runner binds `Esc=quit` (it says so in its own startup banner). In the port
Esc and F1 toggle the ImGui panel, and the event hook consumes the key before
the runner sees it, so **Esc now toggles the panel and no longer exits**.

That is intended, not an accident, but it removes the documented quit key, so the panel footer says so and exiting is by closing the window.

## Incremental builds

`build.sh` copies with `cp -rp`. The `-p` is not cosmetic: without it every run
re-stamps 3.7 MB of unchanged upstream sources, `make` considers everything
dirty, and a one-line overlay edit costs a full twenty-minute rebuild of all 96
lifted banks. With it, only the touched translation units rebuild.
