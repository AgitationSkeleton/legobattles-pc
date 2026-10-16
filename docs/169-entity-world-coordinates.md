# 169 — Entity world coordinates, and how to recover the camera

Status: **fields identified, proven world-absolute, and registered to the map.**
Live units now draw on the full-battlefield render.

Follows docs/168, which draws the whole battlefield terrain. Units are the next
layer, and a unit can only be drawn on the full map if its position is known in
the same space as the terrain.

## The fields

    entity + 0x0EC   world X, 20.12 fixed point
    entity + 0x0F0   world Y, 20.12 fixed point

Read from the pool the usual way: `*(u32*)0x02155028` -> pool, `pool+0x08` the
array, `pool+0x0C` the count. Sixteen slots, six of which carry a resolvable
blueprint in this save:

    idx  owner  category            +0x0EC   +0x0F0
     13    0    7  structure          240.0    160.0
     14    0    1  builder            240.0    208.0
     15    0    0  hero               264.0    240.0
      9    1    0  hero              1248.0    768.0
     10    1    1  builder           1386.6    800.0
     11    1    7  structure         1224.0    816.0

Two tight clusters at opposite ends of a 1536x1008 map — exactly two players'
starting bases, which is the first thing that makes these look like positions.

## Why they are world-absolute, not view-relative

The camera was scrolled twice with the `keys` command (mask is ACTIVE-LOW; the
d-pad is bits 4-7) and the terrain origin re-measured each time:

    camera origin (-8,144) -> (382,141)   after 90 frames of RIGHT
    camera origin (382,141) -> (128,142)  after 60 frames of LEFT

Across both moves — 390 px and then 254 px of travel — **every one of the twelve
coordinate values above was byte-identical.** View-relative coordinates could
not survive that. This is the strongest single fact in this doc.

## Recovering the camera origin: use exact row matching, not correlation

To place units on the full map the camera's world origin is needed. Two methods
were tried and only one is trustworthy.

**Sampled correlation is unreliable here.** Scoring `emu[x,y] == full[ox+x,oy+y]`
over every 6th or 8th pixel picked `(-8,144)` with 87% agreement. On a map this
dominated by repeating grass, many offsets score nearly the same, and the winner
was a false peak — a later exact test put the true origin ~139 px away.

**Exact row matching is decisive.** Take a full 120-pixel run from several
screen rows and `find()` it in the corresponding full-map rows; correct offsets
get votes from every probe row, wrong ones from none. At the same moment the
correlation claimed `(-8,144)`, exact matching returned a single candidate,
`(131,144)`, with agreement on 88.1% of all pixels — the residual being the
units themselves.

This is the same class of error as the six already recorded: a measurement that
returns a confident number without the number being right.

## RETRACTED, then resolved: the "117 px offset"

An earlier revision of this doc reported a 117 px constant X offset between the
fields and the rendered sprites, measured once and flagged as unconfirmed. **That
figure is withdrawn.** It was an artefact of a bug in the level-grid base, and it
was never a property of the coordinates.

`grid_extent` walked upward from the camera's own row in whole-row strides. That
makes the base congruent to whatever row the camera is on, with two consequences:

1. the reported camera column is **always 0**, by construction, so it carried no
   information and could not be sanity-checked; and
2. the base can be wrong by whole cells. A base wrong by k cells does not shear
   the map -- row-major indexing makes it **roll** the whole map k cells
   horizontally with wraparound. The map still looks coherent, just shifted,
   which is exactly how a 4-cell error survived a visual check.

The tell was symmetry. This map is symmetric -- forest belts down *both* edges,
lake centred, cliff formations mirrored -- and the old base put the forest belt
only on the right. `0x0227F730` renders it symmetric; `0x0227F738`, four cells
later, does not.

The base is now anchored deterministically: the array is preceded by an object
header whose last two words are pointers into main RAM, so the walk's result is
snapped to the nearest address with that shape.

## The offset, calibrated and checked

With the base anchored and the camera at cell (0,8), the hero was located on
screen by its minifig colours -- warm pixels against green terrain, needing no
terrain reference and so immune to the correlation problem above -- at screen
(220,95), while its fields read (264,240):

    ENTITY_TO_MAP = (52, 17)        map_pixel = entity_world - offset

Two other entities were then checked **without refitting**: the structure lands
on the castle's top-left corner and the builder on the small figure beside it,
both within a few pixels. A third, independent check is the map itself -- with
this offset the two players' bases mark out mirror-symmetric positions on a
mirror-symmetric map, upper-left and lower-right.

It is still a calibration, not a decomp derivation, so `tools/lb_field.py` keeps
it in one named constant with `--entity-offset` to override, rather than smearing
it through the drawing code.

## Fog of war is a real layer, and the full-map view ignores it

Scrolling into unexplored ground fills the screen with a uniform grey shroud
pattern -- which is also why terrain matching failed there, and worth recording
against the requirement that the widened camera must not cull fog or shroud.
The host-side full-map render currently draws the *whole* level regardless of
what has been explored, so it reveals ground the player has not scouted. Applying
the explored mask is outstanding.

## Also fixed here: the colour expansion

The renderer's 5-bit-to-8-bit conversion did not match the runner and every
pixel differed, which made the first sprite-isolation attempt report 100%
non-terrain. The runner expands via a 6-bit intermediate, melonDS style:

    c6 = c5 * 2
    c8 = (c6 << 2) | (c6 >> 4)

Measured against the live framebuffer, grass reads `(0x28,0x9A,0x28)` and only
this formula reproduces all three channels — a plain `c5 << 3` gives `0x98` for
green, not `0x9A`. `tools/lb_field.py` now uses it, and terrain agreement with
the emulator went from 0% to 88.1%.
