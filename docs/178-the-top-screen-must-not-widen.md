# 178 — The top screen must not widen, and my captures were widening it

Status: **fixed and verified.** Retracts the "menus widen correctly" claim in
docs/174 and in commit `28c2397`.

## What was reported

A screenshot of the LEGO BATTLES title screen pulled across 448 columns: the
logo stretched, the word BATTLES broken by gaps, the three minifigs distorted.
The instruction was unambiguous — *"top-screen graphics were not meant to be
extended like this. I only meant to affect the bottom screen."*

That is correct, and it is what `port/game.toml` has said all along:

    adaptive_widescreen = "bottom"
    adaptive_capability = "bottom"

In this title the battlefield is the **touch screen** (POWCNT1 bit 15 clear puts
engine A on the bottom, docs/156). The top screen carries the minimap and the
title art. Widening it has no content behind it to reveal — there is nothing
there but a stretched bitmap.

## Why it happened anyway

`frontend.cpp` respects the config:

    if ((options.adaptive_screens & NDS_ADAPTIVE_TOP) && lb_setting_enhanced() ...)
        top_pixels = nds_gpu2d_adaptive_framebuffer(0, &top_width);

so **the interactive port never widened the top screen.** The stretched title
was never something the game drew.

`nds_gpu2d_adaptive_framebuffer()` itself, however, widens whichever screen it
is handed — the mask lived in the caller. The debug server's `framebuffer`
command is another caller, and it did not check:

    const uint32_t* fb = adaptive ? nds_gpu2d_adaptive_framebuffer(screen, &width)
                                  : nds_gpu2d_framebuffer(screen);

**So the defect was in my capture harness, and the screenshots I used to verify
menu widening were measuring something the port does not do.** Worse, I had
already been told about this once — the earlier report that "the scaling was
applied to the top window, not the game window" was the same bug. I diagnosed it
then, wrote it down, and did not fix it, and it went on to manufacture the exact
misleading screenshot that prompted the second report.

## The fix

The mask now lives in the widener rather than in each caller:

    static uint8_t g_lb_adaptive_screens = 0xFFu;
    void nds_gpu2d_set_adaptive_screens(uint8_t mask);

    const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width) {
        const uint32_t* native = nds_gpu2d_framebuffer(screen);
        if ((g_lb_adaptive_screens & (1u << (screen & 1))) == 0u) {
            if (width) *width = 256;
            return native;
        }
        ...

`main.cpp` sets it from `frontend_options.adaptive_screens` once the config and
the CLI have both had their say — outside the `interactive` branch, because
serve mode reaches the widener through the debug server rather than the
frontend, which is precisely how the hole was opened.

"Bottom only" is now a property of the renderer, not a convention every call
site has to remember.

## Verified

Forced widening active (`LB_FORCE_WIDTH=448`), so this is not just measuring a
disabled feature:

    top     adaptive=True -> 256x192      native, correct
    bottom  adaptive=True -> 448x192      widened

`screenshots/20260831-144255-topscreen-fix/title_top.png` is the title screen at
native width: logo undistorted, BATTLES unbroken, minifigs correctly
proportioned. `title_bottom.png` is Choose Profile at 448 with the background
filling edge to edge and the Create button anchored bottom-right.

## The lesson, which is not the same as docs/175's

docs/175's lesson was that a confident negative inference went unchallenged.
This one is different and worse: **the bug was correctly diagnosed and then not
fixed**, and the unfixed tooling went on to generate evidence that was used to
declare the feature working. A measurement harness that is known to be wrong is
not a lower priority than the thing it measures — it is a prerequisite for every
claim made with it.

Concretely, the "menus widen correctly" verification in docs/174 measured margin
fill on **both** screens and scored 100% top-screen fill as a pass. That was
scoring the artifact. The top-screen row of that table is withdrawn; the bottom
screen row stands, and is re-verified above.
