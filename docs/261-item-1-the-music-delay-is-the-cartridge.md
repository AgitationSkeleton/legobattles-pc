# 261 — Item #1: the music setting applies instantly; the delay is the cartridge's

The observation: after changing the music setting to faction-aligned, the music followed the Earth builder, but for the first few seconds the King theme still played.

## The setting is not slow

`lb_music_tick()` runs **once per frame** from `lb_patches_on_vblank`. It
notices a mode change on the frame it happens, resets its cached state, asks
`lb_music_wanted_side()`, and writes the answer immediately.

Measured, Free Play with `LB_MUSIC_MODE=2`:

```
[lb_music] owner 0's builder is blueprint 82 -> side 4
[lb_music] Faction-aligned -> side 4 (Earth), was 6
```

Both lines land **before the first `[lb_shroud]` line of the run** — the side is
changed as soon as a battle is up and the builder is readable. There is no
latency on the port's side of this.

(Note `was 6`: side 6 is the front end's own music path, outside
`func_02089c38`'s 0..5. The player reported hearing *King*, side 0, which is a
real in-battle side — so their case was a mode change during a match already
playing side 0, not the menu-to-battle transition measured here.)

## What the port writes is exactly what the cartridge writes

`lb_music_tick` mirrors `func_020898e8`, and the mirror is faithful:

```
020898e8  ldr  r1, [pc, #0x14]
020898ea  ldr  r2, [r1, #0xc]
020898ec  cmp  r0, r2            ; already this side? do nothing
020898ee  beq  #0x20898fc
020898f0  str  r0, [r1, #0xc]    ; the side
020898f2  ldr  r0, [pc, #0x10]
020898f4  movs r1, #0
020898f6  str  r1, [r0, #0xc]    ; clear the three runtime words, so the
020898f8  str  r1, [r0, #0x10]   ; next update re-picks instead of letting
020898fa  str  r1, [r0, #0x18]   ; the current track play out
020898fc  bx   lr
```

It has exactly **one** caller, `func_02088980`, and that is a thunk —
`adds r0, r1, #0; bx r3` — which adds nothing. So there is no stream stop, no
fade, no restart anywhere on the cartridge's own side-change path.

## So the delay is the cartridge's own behaviour, reproduced faithfully

Changing the side does not stop what is playing. It clears the three runtime
words so that **the next time `func_02089974` runs** it re-picks rather than
continuing. Until that update comes round, the previous track keeps playing —
which is precisely "for a few seconds at first it was King still".

The port is not adding the delay. It is matching the cartridge to the
instruction.

## This is a decision, not a bug

Making the change audible immediately means going **beyond** what the cartridge
does: finding the audio module's stream-stop entry (`func_02089B64` loads
`Sound/sound_data.sdat`, and the module runs from about `0x02089AAC` to
`0x0208A4CC`, docs/98) and cutting the current stream so the re-pick happens
at once. That is an enhancement with a cost — it would chop the playing track
off mid-phrase rather than letting it finish, which is audibly abrupt and is
presumably why the game does it this way.

**Not built, because it is the project owner's call**, and either answer is
defensible:

- *Leave it* — the port behaves exactly as the cartridge does, and the delay is
  a deliberate musical crossfade-at-boundary.
- *Force it* — add a stream stop on a mode change only (never on the game's own
  side changes), so the setting feels responsive at the cost of an abrupt cut.

Asked. Until answered, nothing is changed here: the setting works, it applies
on the frame it is toggled, and what lags is the cartridge's track swap.
