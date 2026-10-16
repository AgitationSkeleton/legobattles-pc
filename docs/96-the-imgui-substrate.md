# The ImGui substrate

**Status: ImGui is vendored and the three hook points in the runner are located.
No integration code is written yet.** This is the shared substrate for the
overlay features (the top-screen panel, the enhancement toggles, rebindable
inputs and the rest of the port's UI) — none of them can begin until it exists,
which is why it comes before any of them.

## The runner has no UI layer at all

```
find ndsrecomp -iname "*imgui*"      ->  nothing
grep -ril imgui runner/src           ->  nothing
```

There is no overlay, menu or HUD source in the runner. Every panel the port
needs has to be built from zero.

## What was retrieved

ImGui is cloned to `tools/imgui` on the **docking** branch, which the design
requires by implication: the top-screen overlay has to be movable and scalable
and start in the lower-right corner. Docking gives that; master does not.

## The presentation path is SDL_Renderer, not OpenGL

This is the decision that picks the backend, and it was nearly got wrong.

The runner initialises an **OpenGL 4.3 compute renderer** and says so at
startup, which suggests `imgui_impl_opengl3`. That renderer is the **3D GPU
emulation**, not the presentation path. Presentation is plain SDL:

```
frontend.cpp:718   SDL_Renderer* create_renderer(SDL_Window*)
frontend.cpp:874   SDL_RenderCopy(renderer, source, nullptr, nullptr);
frontend.cpp:922   SDL_RenderPresent(renderer);      /* and 950, 965 */
```

There is **no `SDL_GL_SwapWindow` in the file at all**. `gl_top` is a narrow
option that applies only to the top window of the *separate* layout.

So the backend pair is **`imgui_impl_sdl2` + `imgui_impl_sdlrenderer2`**, both
present in the clone. Choosing `imgui_impl_opengl3` from the startup banner
would have compiled and then drawn nothing.

## The three hook points

| # | where | what |
| --- | --- | --- |
| 1 | after `create_renderer`, `frontend.cpp:779` | `ImGui::CreateContext`, `ImGui_ImplSDL2_InitForSDLRenderer`, `ImGui_ImplSDLRenderer2_Init` |
| 2 | inside `while (SDL_PollEvent(&event))`, `frontend.cpp:1538` | `ImGui_ImplSDL2_ProcessEvent(&event)` **before** the runner's own handling, so panels swallow input that must not reach the guest |
| 3 | immediately before each `SDL_RenderPresent`, `frontend.cpp:922/950/965` | `NewFrame`, draw panels, `ImGui_ImplSDLRenderer2_RenderDrawData` |

**Hook 2's ordering is a correctness requirement, not a style choice.** The
main window is the touch screen and is clickable. If a click
that lands on a panel also reaches the guest as a stylus tap, every interaction
with the enhancement UI would poke the game. `io.WantCaptureMouse` /
`io.WantCaptureKeyboard` must gate the existing path.

Hook 3 appears three times because the runner presents from three places; all
three need it or panels vanish in whichever layout uses the missed one.

## What this does not settle

* **Multi-viewport is not available** with the SDL_Renderer backend. Docking
  works, so a movable, resizable, dockable overlay *inside* the window is fine,
  but the overlay cannot be dragged out into an OS window of its own. Nothing in
  the design needs that, but it is a real limit of this backend choice.
* **Where the config lives.** Rebindable inputs and the enhancement
  toggles need persistence, and `port/game.toml` is the title config
  rather than a user settings file. Which file holds user settings is undecided.
* **How host-decided options reach clients.** The host decides the enhancement
  toggles and joining clients must conform. The transport for that is the master server's
  `options` object (`services/master-server/README.md`), but nothing connects
  the panels to it yet.
