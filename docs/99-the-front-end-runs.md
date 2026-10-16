# The front end runs

**Status: the game reaches the title screen and name entry.** Splash -> Title ->
Name Entry, verified by measurement rather than by looking at the window.

## Measuring a screen without seeing it

The port cannot see its own output, so "it got to the title screen" had to
become something checkable. Every `Battles::User::*` screen is a class with a
vtable, and the only code that writes a vtable pointer into an object is that
class's constructor:

> **screen reached  <=>  its constructor executed**

The runner's `--coverage-manifest` records every 4 KB code page it ran.
`tools/lb_screens.py` locates each screen's constructor by finding the literal
references to its vtable, then asks whether that page is in the executed set.

**Resolution is a page, not a function.** `StartScreen` and `StatsScreen` share
page `0x020C9`, and `ProfileSelect` shares `0x020C1` with `ProfileCard`, so
those pairs cannot be told apart. The tool prints the sharing rather than
implying a precision it does not have. `CreateNewProfile` is alone on
`0x020B6`, so **name entry is unambiguous**.

## The result

```
YES  SplashScreen         publisher / title splash        ctor 020C7C10
YES  StartScreen          the title screen                ctor 020C9684  [shares a page with StatsScreen]
no   ProfileSelect        choose a save profile
YES  CreateNewProfile     NAME ENTRY                      ctor 020B6A04
no   ProfileCard          profile summary
no   SkirmishSetup        skirmish / multiplayer setup
YES  StatsScreen          stats                           ctor 020C9A9C  [shares a page with StartScreen]
no   CheatKeyboardScreen  the game's own cheat-code entry
```

`ProfileSelect` being absent is **correct**, not a failure: the save is empty, so
there is no profile to choose and the game goes straight to creating one.

## Getting there needed two fixes and one harness

**1. The save chip** (`docs` and `port/game.toml`). Until it was right the game
never left its error screen.

**2. `LB_AUTOPLAY`, a batch-mode driver.** A cycle-budget run presses nothing, so
the game sat at the title forever and everything past it was unverifiable. The
hook taps VBlank and holds Start, A, and a stylus tap on a slow cycle. It is a
test harness, off unless the environment variable is set, and it cannot play the
game — only walk menus.

Two mistakes in that harness are worth recording because both produced *silence*
rather than an error:

* **KEYINPUT is active low.** The first version started from `lb_keys = 0` and
  therefore held all twelve buttons down for the entire run. `frontend.cpp`
  releases everything with `nds_set_key_mask(0x0FFF)`, which is what settled it.
* **Buttons are not enough.** With the polarity fixed, coverage still sat at 121
  pages and the screen set did not move. LEGO Battles is a DS game and its front
  end is driven by the **stylus**. Adding a tap at the middle of the touch
  screen took coverage to 126 and reached name entry immediately.

## Coverage as a progress signal

| state | pages |
| --- | ---: |
| stuck on the save error | 107 |
| save chip fixed | 119 |
| autoplay, buttons only | 121 |
| autoplay with stylus | **126** |

Useful *between* configurations, useless *within* one — every wrong save chip
also gave exactly 107, because the game loops on the error screen either way.

## What is next

`SkirmishSetup` is the screen the planned multiplayer work needs, and it is
not reached yet: the autoplay driver only taps the centre of the screen, so it
cannot navigate a menu that needs a specific button. Driving further will need
either scripted coordinates or a human.
