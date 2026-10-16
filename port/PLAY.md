# Running the port

```
export PATH="/c/msys64/ucrt64/bin:$PATH"          # SDL2.dll lives here

port/runner/build/nds_runner.exe \
    $NDSRECOMP/bios  2000000000 \
    --rom "<your EU ROM>" \
    --config port/game.toml \
    --interactive --boot direct \
    --save-path lb.sav \
    --generated-firmware --network off --freebios
```

**`--interactive` is required for the overlay.** Without it the runner never
enters the SDL loop, so there is no window and no UI — that form is for boot and
coverage testing only.

**`--save-path` is required.** Without it the game cannot reach its save chip.
The chip itself is configured in `port/game.toml` as **`eeprom-tiny`, 512
bytes**, which was established from the game's own SPI traffic — see the comment
there.

Nothing here touches physical media. The runner writes exactly one file, the one
named by `--save-path`. When the game says it is formatting the DS Card it is
initialising its own save image inside that file.

## Controls

| key | action |
| --- | --- |
| **F1** or **Esc** | toggle the enhancement panel |
| mouse on the bottom screen | stylus |
| arrows / Z X A S / Q W / Enter / Backspace | D-pad, A B Y X, L R, Start, Select |
| hold Tab | turbo |
| close the window | quit |

**Esc no longer quits.** The runner binds `Esc=quit`, but Esc and F1 both
toggle the ImGui panel, so the overlay consumes it first. Close the window to
exit.

## What the panel does today

It opens on start and has tabs for all seven planned feature areas. **Almost nothing in
it is wired to the game yet**, and each control says so rather than pretending:

* **`[read, not wired]`** — the ROM location is known and cited in `docs/`
* **`[unread]`** — the ROM has not been read for this yet
* no badge — the control actually works

The Input tab is deliberately empty: rebindable input is the least-read item and
a list of bindings that cannot be rebound would be a lie.

Verified rendering, not just initialising:

```
[lb_overlay] ready (F1 or Esc toggles)
[lb_overlay] drawing: 1618 vertices, 2673 indices, 1 list(s) at 896x384
```
