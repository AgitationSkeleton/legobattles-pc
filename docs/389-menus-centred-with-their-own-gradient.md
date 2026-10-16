# 389 — Menus centred, each backdrop continued as its own gradient

Status: **implemented and verified** (lb_wide2d.inc, lb_widemap.cpp,
apply_hooks.py stanza 0zzzzzzzzzzzzzzzz). The goal: the menus, including the
boot startup logos and the save select, look consistent across window scales
and stretches and are properly centred in all layers, and the backdrop
gradient continues naturally toward the centre instead of the edges being
stretched.

## What a survey showed

`scratch/survey390.py` captures the window every 0.5 s through boot and then
takes a shot at each step of the Free Play route. It ran at 1473x830,
1900x700, 1200x1000 and 1920x1080 (pixel scale 2.25).

* **Not centred.** The Mobiclip, TT Games and legal screens and the title
  ("Touch the Screen to Start") sat in the window's top-left corner at every
  size. The menus were centred.
* **Stretched edges.** Every menu's backdrop is a radial gradient, bright
  blue in the middle, dark at the edges, with a fine dither. Outside the
  guest's 256x192 it became a cross:
  * horizontal streaks beside the picture;
  * vertical stripes above and below;
  * flat blocks in the four corners.

## Why

**The edges.** Two routes fill a menu's widened surface, and both carried
edge pixels:

* **An affine or 3D-plus-affine scene** (`lb_menu_fill` → `lb_edge_fill`)
  carried each row's edge pixel sideways, then the top and bottom rows up
  and down.
* **A text-BG scene** (`lb_wide2d_line` + `lb_fill_rows_outward`) is widened
  truthfully, then its top and bottom rows are carried out.

**The corner.** An engine-A scene the widener refuses, for example "a
non-text HUD BG" at DISPCNT 00010D0B, is shown as the native picture drawn at
the **band**: where the guest's 256 columns sit in the widened 3D raster.
That band is set only when a 3D geometry list is latched
(`lb_band_for_geometry` → `lb_band_promote`).

* **At boot it is 0,0.** The logos and title come before any list is latched.
  `LB_FILLTRACE=1` showed `engine 0 736x414 at 0,0 dispcnt 00010D0B` while
  the logo was on screen.
* **It lags a resize.** On each step of a window resize the band was the
  previous size's for a frame: 50,24 at 306x216, then 25,12.

## The fix

### A 2D or menu scene is centred

In the refused-scene branch, the native picture is placed at the band only
while a battle's camera places it (`lb_widemap_band_live`: the map and
camera are valid, the same condition `band_compute_at` clamps under). Only
then are the frames either side widened with that band. Everywhere else it
is centred:

* a 2D-only scene;
* the logos and the title;
* a menu.

The offsets the frontend maps touches with (`lb_shown_band_x/y`) are the
ones the picture was drawn at, so input follows.

### The backdrop continues as its own gradient (`lb_radial_fill`)

Every pixel outside the picture takes the colour where the ray from the
picture's centre leaves it:

* **The border is smoothed** along its own length (a 49-pixel box), so the
  dither does not streak.
* **It darkens with distance** past the border, at the rate the picture
  itself darkens over its outer 24 pixels. A picture that does not darken
  gets no darkening. The rate is capped at 3% per pixel.

The result is one gradient, centred on the window, that meets the picture
without a seam. It invents no detail and moves nothing inside the picture.

It applies to both routes. The text-BG route's rect is the full-width band
of 192 rows. The battle top screen's HUD anchor keeps its row carry, so no
battle picture changes.

**Cost:**

* **The ray geometry** depends only on the surface and the rect, so it is
  built once per size, in parallel rows.
* **Each frame** smooths the ~892-pixel border and recomputes the outside
  only when the border or the rate changed. A fade changes them; a still
  menu does not. Otherwise the outside is copied from the cache.
* **Each engine** keeps its own cache.

`LB_MENUFILL=edge` restores the edge carry; `LB_RADIALLOG=1` logs each new
geometry.

## Verified

The same survey after the fix (runs s390l, s390m, s390n, s390o):

* **Centred.** The logos, the legal screen and the title are centred at all
  four sizes, including pixel scale 2.25.
* **One gradient.** The title and every menu sit in a single radial gradient
  with no streaks, stripes or corner blocks.
* **Input follows.** Every route tap was verified by the harness, and every
  run reached the battle.

The gate numbers are in the section below.

## Gate

On the final binary (scratch `verify389.ps1`, pictures by decoded pixels):

* counts identical to gate370b;
* the t384f recording twice, identical to each other and to rep384c;
* pictures 8/8 identical to gate381, which are battle frames: the battle
  paths are unchanged;
* Vanilla 240/240 identical to van-hudmask;
* 59.85 fps.
