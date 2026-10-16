// lb_hud.cpp - move the in-field HUD out to the widened screen's own edges.
//
// This file belongs to legobattles-decomp, not to ndsrecomp.
//
// GOAL: HUD repositioning and scaling (with optional scaling in the ImGui
// settings). A corner button belongs in the corner. Widening the
// picture moves the corner; the button has to follow it.
//
// WHY NOT upstream's adaptive_hud_anchor. That rule anchors bands of a 2D HUD,
// and this game has no 2D HUD in the field: OBJ is disabled outright in
// gameplay (DISPCNT 00010F08, bit 12 clear, recorded in port/game.toml) and the
// BG layers are terrain and fog. The in-field HUD is 3D geometry, in the same
// polygon list as the units.
//
// WHICH POLYGONS. docs/212 classified them by measurement rather than by
// reading projection matrices: pan the camera, and whatever does not move is
// screen-locked. In one fully decomposed frame the HUD sat at z 4259328 and
// 4267520 while world geometry was at 6987264..7380480, and the world's z ramp
// runs with map depth, extrapolating to about 6.4M at the bottom of the screen.
// So a threshold halfway between separates them with a wide margin. Three other
// candidate discriminators were checked and rejected: `attr` is 2031808 for
// every polygon HUD and world alike, `submission_index` is dense with one
// polygon each, and the texture fields vary per texture rather than per role.
//
// WHERE THE MOVE HAPPENS. The polygon list is already in widened-surface
// coordinates, and the port composites the RASTERISED 3D line itself. Moving
// pixels there needs no depth interaction at all -- the line is flat RGBA by
// then. Translating vertices instead would mean patching melonDS's transform,
// which the build treats as read-only.
//
// ONE FLAT LINE CANNOT SAY WHAT WAS UNDER THE HUD (docs/375). Vacating a moved
// element's native rect by zeroing its opaque pixels also cut every world
// sprite that stood there -- a truncated Eagle Command -- and a
// world pixel showing through a transparent part of an element travelled with
// it. The soft renderer now draws the HUD's polygons (this file's z test) into
// a set of their own and merges them back, and lb_hud_shift_line vacates with
// the world-only row and moves the HUD-only one whenever the row it is handed
// is one the renderer owns.

#include <algorithm>
#include <atomic>
#include <chrono>    // docs/385: LB_HUDSPLIT0's half-second blocks
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>    // docs/385: LB_HUDTRACE
#include <utility>
#include <vector>

#include "gpu3d.h"

extern "C" int lb_band_left(int output_width);   /* src/lb_widemap.cpp */
extern "C" int lb_shown_band_x(void);            /* frontend.cpp (apply_hooks) */
extern "C" int lb_shown_band_y(void);

extern "C" int lb_setting_hud_anchor(void);
extern "C" float lb_setting_hud_scale(void);
extern "C" int lb_setting_enhanced(void);
/* docs/375: the world-only and HUD-only rows behind a 3D row; see
   apply_hooks.py stanza 0zzzzzz. */
extern "C" int nds_gpu3d_split_rows(const uint32_t* line,
                                    const uint32_t** world,
                                    const uint32_t** hud, int* x0, int* x1);

namespace {

// Between the HUD's 4267520 and the HEALTH BARS' 4718080.
//
// The first value here was 5,000,000, chosen as halfway to the world geometry's
// ~6.4M floor -- and it was too generous. With a unit selected the list showed
// quads at z 4718080, which docs/195 already named as the health-bar depth,
// being taken for HUD. They are world-anchored: they sit above whichever unit
// they belong to and move with it. Dragging one to the screen edge is exactly
// the kind of wrong that looks like a rendering bug.
//
// Most were caught by the on-screen guard below because their unit was out in a
// widened column, but a health bar over a unit standing NEAR the native edge
// would have passed both tests. The threshold has to do this job, not the
// guard. Overridable so a scene that disagrees can be investigated.
//
// 4,300,000 WAS ALSO TOO GENEROUS, by one layer (docs/348). The contextual
// command icons -- move, attack, harvest, patrol, assist, garrison, rally --
// are NOT OAM sprites (engine A's OBJ layer is off in the field): they are
// 3D quads drawn by FUN_0204C210 at z 502, depth 4,275,712, through the one
// object at 0x0215760C. Measured in a replay of a recorded play session, the
// depths near here are:
//
//     4,259,328     34 polygons   HUD
//     4,267,520 39,009 polygons   HUD
//     4,275,712    336 polygons   COMMAND ICONS  <- world-anchored
//     4,718,080 15,023 polygons   health bars
//
// Under 4,300,000 the icons were classified as screen-locked HUD and moved
// by a band width when near the native edge -- docs/324's "~267 guest px"
// error: the command cursor did not position right. The
// threshold now sits between the HUD's deepest layer and the icons.
uint32_t hud_z_threshold() {
    static uint32_t z = 0u;
    if (!z) {
        const char* v = std::getenv("LB_HUDZ");
        z = v ? (uint32_t)std::strtoul(v, nullptr, 0) : 4271616u;
    }
    return z;
}

// docs/375: THE HUD IS RASTERISED ON ITS OWN, so a moved element's native
// rect is vacated with the WORLD's pixels and only HUD pixels travel. The
// soft renderer draws every polygon under hud_z_threshold() into a second
// set and merges it back (apply_hooks.py stanza 0zzzzzz); lb_hud_shift_line
// asks it for the two rows behind the one it was handed. LB_HUDSPLIT=0 turns
// the split off and restores the old vacate-to-nothing path.
bool hud_split_env() {
    static const bool on = [] {
        const char* v = std::getenv("LB_HUDSPLIT");
        return !(v && v[0] == '0');
    }();
    return on;
}

// LB_HUDSPLIT=ab: split and unsplit frames in turn, 600 rendered frames each,
// so one run can time both under the same load (LB_3DWAIT=1 reports the two
// render times separately). Asked once per rendered frame, by the renderer.
bool hud_split_ab_now() {
    static const bool ab = [] {
        const char* v = std::getenv("LB_HUDSPLIT");
        return v && v[0] == 'a' && v[1] == 'b';
    }();
    if (!ab) return true;
    static std::atomic<unsigned> frames{0u};
    return ((frames.fetch_add(1u) / 600u) & 1u) == 0u;
}

// A HUD pixel over whatever is already at its destination: melonDS's own
// SoftRenderer::AlphaBlend with 3D alpha blending on (DISP3DCNT bit 3, set in
// every battle frame measured -- LB_3DCNT=1 reads 0019), which is how the
// pixel was merged into the line at its native position. Channels are 6 bit,
// alpha 5 bit at bit 24, as in the renderer's colour buffer.
uint32_t hud_blend(uint32_t src, uint32_t dst) {
    const uint32_t a = (src >> 24) & 0x1Fu;
    const uint32_t da = (dst >> 24) & 0x1Fu;
    if (da == 0u) return src;
    const uint32_t w = a + 1u;
    const uint32_t r = ((src & 0x3Fu) * w + (dst & 0x3Fu) * (32u - w)) >> 5;
    const uint32_t g =
        (((src >> 8) & 0x3Fu) * w + ((dst >> 8) & 0x3Fu) * (32u - w)) >> 5;
    const uint32_t b =
        (((src >> 16) & 0x3Fu) * w + ((dst >> 16) & 0x3Fu) * (32u - w)) >> 5;
    return r | (g << 8) | (b << 16) | ((a > da ? a : da) << 24);
}

// THE SIZE CAPS ARE GONE, replaced by the edge test in lb_hud_scan.
//
// They were kCornerMaxWidth = 64 and kCornerMaxHeight = 72, and each was
// a proxy for "this is a corner button, not a bar". The edge test states
// the same thing exactly: an element against ONE side is anchored to
// that side however big it is, and an element against BOTH spans the
// screen and wants stretching rather than moving. Keeping the caps as
// well only refused elements that the edges had already accepted -- the
// buildings list being the one that showed it.
const int kEdgeSlack = 16;

struct HudBox {
    int sx0, sx1;     // surface x, half-open
    int y0, y1;       // surface y, half-open
    int dx, dy;
    // THE RIGHT EDGE'S OWN DELTA, so an element can STRETCH and not only move.
    //
    // Equal to dx for everything that moves rigidly, which is everything that
    // moved before this existed. Where it differs, the element's left edge
    // follows the left anchor and its right edge the right anchor, so it spans
    // the gap between whatever sits either side of it as those separate.
    int dx1;
    // Which slot of LAST frame's capture holds this box's pixels, or -1. See
    // the note on HudCache.
    int prev;
    // A STRETCH THAT KEEPS ITS CONTENT (docs/365): caps move to the anchors,
    // the middle stays at its native place and width, and the gaps repeat
    // one column. 0 for every other box. See hud_stretch_multi().
    int slice5;
    // The rows the text quads over a five-slice box cover, in surface y, half
    // open (ty0 >= ty1: none). The gap fill needs to know which rows can hold
    // glyphs; see the fill note in lb_hud_shift_line.
    int ty0, ty1;
};

// A span is stretched by 3-SLICE, not by scaling, and the two are different.
//
// The message-box element has to position and stretch (the box, not the font)
// between the deselect and combat-happening buttons. Scaling the element would
// widen its border art with it. Measured
// on the bar itself (docs/272): the interior is 100% #141c4d on every one of
// its ten rows, with 1 px caps -- cap | flat fill | cap. Holding the caps at
// their native width and extending only the middle reproduces it EXACTLY at
// any width, and there is no font inside the box to distort.
//
// Wide enough to be a bar rather than a button. The buttons this sits between
// are 23 and 24 px; the bar is 199.
const int kSpanMinWidth = 96;
// Kept at their native width. The measured cap is 1 px; 4 takes three columns
// of flat fill with it, which is harmless on a flat interior and leaves room
// for an element whose border is thicker than this one's.
const int kSpanCap = 4;

// HOW MANY HUD QUADS ONE SCAN CAN HOLD. It was 64, and 64 is not enough
// (docs/365). A campaign tutorial box is drawn as 72..150 quads of 8x8 tiles,
// one 256-wide text quad in front of them and the buttons either side, and
// the scan stopped at the 64th quad. So part of a box was grouped and moved
// and the rest was not, and the deselect button, drawn after the tiles, was
// never seen at all. Measured in the tutorial at 1720x720: hudquads=64 and
// qncap rising on every frame, with 74, 106 and 152 HUD quads on screen.
// The per-frame cost is the pairwise grouping below, 256^2/2 compares at
// worst.
const int kMaxHudQuads = 256;

HudBox g_boxes[kMaxHudQuads];
int g_count = 0;
thread_local std::vector<uint32_t> g_scratch;   /* per-row; see g_cover */

// docs/371: WHAT THE CARTRIDGE'S WIDGET HIT TEST NEEDS FROM THE LAST SCAN.
//
// The scan runs where the frame is composited; the hit test (FUN_020EA1A4,
// through lb_patches.cpp's widget_rect_observer) runs inside the guest. So the
// boxes are published by copy under a lock at the end of each scan, never read
// live, with the band that turns their surface coordinates into guest ones.
struct HudHitSnap {
    int n = 0;
    int band = 0, band_y = 0;   // surface columns / rows left of and above guest 0
    int out_w = 0, out_h = 0;   // the surface the boxes were scanned on
    float scale = 1.0f;
    HudBox box[kMaxHudQuads];
};
std::mutex g_hit_mu;
HudHitSnap g_hit;
// The cartridge submits no HUD quads while the camera steps and for about three
// frames after (docs/349 s9c), so those frames scan no boxes. The game still
// hit-tests every enabled widget. So the last box seen for a widget rectangle
// is remembered, and used only on frames that scanned no boxes at all. It is
// keyed by the rectangle and the band, because a different band means a
// different move.
struct HudHitMemo {
    int x0, y0, x1, y1;
    int band, band_y, out_w, out_h;
    HudBox b;
    bool used;
};
HudHitMemo g_hit_memo[64];
unsigned g_hit_memo_next = 0;
// docs/383: THE LAST SCAN THAT SAW ANY HUD, for a widget no scan has shown
// this frame and no press has remembered. The memo above only exists once a
// press FOUND the widget's box, so the first press on a button that lands in
// the frames after a camera step (no HUD quads, docs/349 s9c) found nothing
// and was answered as stock: tested at its native rectangle, it missed, and
// the press went to the field as a move order under the button.
HudHitSnap g_hit_last;
extern "C" int lb_demo_play_hudmemo(void);   /* src/lb_demo.cpp */
// Composites scanned so far; lb_hud_scan counts, the row workers only read.
unsigned g_compose_frame = 0;

// WHERE A MOVED HUD ELEMENT ACTUALLY DREW, this line.
//
// MEASURED, and it is why this exists: engine A's field runs BG3 at priority 0,
// BG2 (the shroud) at 1, the 3D layer at 2 and BG1 (terrain) at 3. The in-field
// HUD is 3D geometry, so THE SHROUD IS DRAWN IN FRONT OF IT. On the DS that
// never shows, because the HUD's corners sit over the player's own ground. Move
// a corner into a widened band and it lands on cloud instead and vanishes
// completely -- measured, the bottom-left element at surface y 232..255 was
// 100% fog with not one pixel of the button in it. Anchoring a button into
// invisibility is worse than leaving it floating.
//
// The fog in those bands is not the game's. It is this port's conservative
// default for ground the guest's own window has never covered (see
// lb_widemap.cpp), so suppressing it under an element this port has MOVED
// reveals nothing: the element is opaque and covers those pixels anyway. The
// mask is set only where an opaque HUD pixel was actually written, not over the
// quad's whole bounding box, so a transparent margin still shows the band as it
// was.
// THREAD_LOCAL, because the compositor now runs its rows across workers.
//
// These four are lb_hud_shift_line's per-ROW scratch: the shifted line it
// returns, the mask of pixels a moved element drew, and which row that mask
// belongs to. lb_hud_cover_row reads the mask straight back for the same row,
// so the pair only makes sense within one thread's work on one row. g_boxes and
// g_count above are NOT thread_local and must not be: lb_hud_scan fills them
// once a frame, before the fan-out, and every row then only reads them.
thread_local std::vector<uint8_t> g_cover;
thread_local int g_cover_y = -1;
thread_local bool g_cover_any = false;

// SCALING NEEDS PIXELS FROM ROWS THIS SCANLINE HAS NOT REACHED.
//
// Enlarging a box anchored at its bottom corner makes the destination start
// ABOVE the source, so a single top-to-bottom pass would need rows it has not
// seen yet. Rather than reach into the frame buffer -- which is only there when
// the snapshot path is active -- each box's pixels are captured as its lines go
// past, and the scaled copy is drawn from the PREVIOUS frame's capture. The
// in-field HUD is screen-locked and changes rarely, so one frame of latency on
// it is not visible; and at scale 1.0 the cache is bypassed entirely, so the
// default path has no lag and no extra copy.
//
// KEYED BY THE SOURCE RECT, NOT THE SLOT (docs/365). A box used to draw from
// last frame's slot of the same index, guarded only by an equal size. That
// holds while the HUD is a few buttons of different sizes. A tutorial box is
// 150 tiles of 8x8, so a quad appearing or vanishing ahead of them (a button,
// on a selection change) moved every index along. For one frame each tile
// would then be drawn with its neighbour's pixels. Each box now names the
// previous slot whose source rect is its own (HudBox::prev).
struct HudCache {
    int w = 0, h = 0;
    int x0 = 0, y0 = 0;   // the box's source rect, top-left, in surface px
    std::vector<uint32_t> px;
};
HudCache g_cache_cur[kMaxHudQuads];
HudCache g_cache_prev[kMaxHudQuads];
int g_prev_count = 0;     // boxes last frame, i.e. filled slots of prev

// docs/348: THE BAND THE SCANNED POLYGONS WERE PLACED WITH. The polygon list
// read below is always the one latched at the latest VBlank (the live list),
// but the raster being composited can be the previous one when that is the
// raster whose camera matches the ground. Near a map edge the two lists were
// placed with different bands, so polygon coordinates are converted to the
// DS's own screen with the LIST's band and back to the surface with the
// RASTER's. INT32_MIN = not told; both are the raster's then.
int g_list_band = INT32_MIN;
int g_list_band_y = INT32_MIN;

// docs/380: the band of the raster being composited this frame, so a row can
// tell the cartridge's own 256x192 from the band. Set by lb_hud_scan before
// the row fan-out, and only read by the rows.
int g_raster_band = 0;
int g_raster_band_y = 0;

// docs/348: THE FOG HOLES A MOVED ELEMENT LEAVES BEHIND.
//
// Every in-field HUD element is a SpriteUIEngine quad, and the cartridge
// punches the fog under each one: SpriteUI_MarkCells (0203420c) marks the 8x8
// cells (x-4)>>3 .. (x-4+w)>>3 by y>>3 .. (y+h)>>3 (ends exclusive, clamped to
// 32x24) and Fog_ApplyUiCellMask (0203a4b4) writes the clear tile into BG2
// there. On a DS the element sits over its own hole. This port moves the
// element to the window's corner, and the hole stays where it was -- the
// "revealed rectangles" in the shroud: 16x48 and 16x24 patches of ground at
// native x 4..20 wherever the camera sat in cloud (docs/348 s6).
//
// Bit cx of g_refog[cy] marks a native cell whose hole belongs only to
// elements this port moved; lb_widemap draws the fog there as the cartridge's
// own rule gives it without the hole. A cell any UNMOVED element or command
// icon also covers keeps its hole.
uint32_t g_refog[24];
uint32_t g_keephole[24];

// ONE CELL OF SLACK ON EVERY SIDE. The cartridge marks a piece's cells from
// its LOGICAL rect (the sprite's size field); the render list only has the
// rasterised polygon, whose extent runs a pixel short of it -- measured on the
// markers panel: pieces at native x 3..12 and 12..19, which by the polygon
// alone mark column 0 only, while the cartridge punched columns 0 and 1. So
// this over-covers on purpose; lb_widemap refills only cells the cartridge
// ACTUALLY punched (its own snapshot of the grid, view+0x164), so the slack
// can never open or close a hole of its own.
void mark_ui_cells(uint32_t* grid, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    x -= 8; y -= 8; w += 16; h += 16;
    int cx0 = (x - 4) >> 3, cx1 = (x - 4 + w) >> 3;   // arithmetic shifts,
    int cy0 = y >> 3, cy1 = (y + h) >> 3;            // as the cartridge's
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > 32) cx1 = 32;
    if (cy1 > 24) cy1 = 24;
    for (int cy = cy0; cy < cy1; ++cy)
        for (int cx = cx0; cx < cx1; ++cx)
            grid[cy] |= 1u << cx;
}

// How many guest columns and rows the window is actually showing. 0 until the
// layout reports it, in which case the whole surface is assumed visible.
int g_visible_cols = 0;
int g_visible_rows = 0;

// HUD-depth quads the classifier DECLINED, so a run can say why something is
// still sitting where the 256-px screen put it. Reporting only what moved made
// the skipped ones invisible to the log while being the most obvious thing on
// screen -- the yellow and blue bricks, deselect and quest info icons
// floated.
struct HudReject { int nx0, nx1, y0, y1; uint32_t z; };
HudReject g_reject[32];
int g_reject_n = 0;

// QUADS THROWN OUT BEFORE THEY EVER REACH THE CLASSIFIER, with the arithmetic
// that threw them out.
//
// The count alone said "offscreen=9" and that was the whole story a run could
// tell: at a 1252x670 surface every one of the nine HUD quads that anchored
// correctly at 996x718 was rejected, so nothing moved at all and the HUD sat
// where the 256-px screen put it -- which is why the selected-unit HUD did
// not constrain to the edges. Which of the four bounds
// failed, and by how much, is not something to infer from a counter.
struct HudOffscreen { int sx0, sx1, sy0, sy1, nx0, nx1, ny0, ny1; };
HudOffscreen g_off[8];
int g_off_n = 0;

// On-screen HUD-depth quads this frame, counted whether or not they move.
int g_hud_seen = 0;

// IS SOMETHING SELECTED RIGHT NOW?
//
// A nonzero on-screen HUD-quad count means the in-field HUD exists, which only
// happens with something selected. ZERO is ambiguous -- it is also what a frame
// reports when every quad was rejected by the z test or the edge test (see
// g_reject above) -- so the honest reading is "true means selected, false means
// don't know".
//
// NOT A "IS THE MINIMAP GONE" SIGNAL, which is what it was added for and why
// this note exists. The top-screen camera box needed suppressing while the
// minimap is replaced, and "something is selected" looked like the same
// condition. It is not: a capture with the player's BASE selected shows 6 HUD
// quads WITH the minimap still on the top screen, so gating the box on this
// hid it in a state where it was valid. lb_overlay.cpp uses the presence of
// the cartridge's own camera box instead. Kept because it is the only
// selection signal the port has and the next caller should not have to
// rediscover its shape -- but check that "selected" is really the question
// before reaching for it.
extern "C" int lb_hud_selection_active(void) { return g_hud_seen > 0; }
// WHY A QUAD WAS NOT TAKEN AS HUD, which the log could not say before.
//
// The collection loop has two rejection points and neither recorded anything;
// only the LATER edge test filled g_reject. So a frame with "hudquads=0" was
// indistinguishable between "there is no in-field HUD right now" (true whenever
// nothing is selected), "every HUD quad is above the z threshold" and "the
// band put them outside the DS's own box". Those need different fixes, and
// HUD elements were being stranded in the middle of the field.
//
// g_rej_z_min is the LOWEST z that was rejected as world geometry: if the
// threshold is wrong, this is the number that says so and what to move it to.
int g_rej_z = 0;
int g_rej_offscreen = 0;
uint32_t g_rej_z_min = 0xFFFFFFFFu;
// Multi-quad groups declined for spanning both screen edges. CUMULATIVE and
// never reset -- the same rule g_bare_fill follows in lb_widemap.cpp, and for
// the same reason: the periodic summary cannot see a transient. See the note
// at the decline site.
unsigned g_span_declines = 0;
// Declines of an element that HUGS AN EDGE -- the shape of the
// open/collapse-animation defect (an element left at its native position).
// Cumulative and never reset, because the SKIPPED list beside it is
// printed on every 120th frame and cannot see a short transient.
unsigned g_edge_declines = 0;
// Every declined element, edge-hugging or not: the denominator for the above.
unsigned g_declines = 0;
// THE SAME BLIND SPOT, ON THE OTHER TWO EXITS.
//
// g_rej_z, g_rej_offscreen and g_rej_z_min are all reset at the top of every
// scan and only printed on the 120th frame, so they describe one sampled frame
// and say nothing about the 119 between. A quad rejected for three frames
// during an open/collapse animation -- which is precisely the
// open/collapse-animation defect -- cannot appear in them.
//
// These are the cumulative twins, on the g_bare_fill rule: never reset, so one
// occurrence anywhere in a session survives to the next summary line.
//
// g_rej_z_min_ever is the interesting one. The in-field HUD measures z
// 4259328..4267520, the command icons 4275712 and the health bars 4718080,
// with the threshold at 4271616 (docs/348), so a REJECTION at 4275712 is an
// icon -- expected -- and one elsewhere in the gap 4271616..4718079 is a quad
// that is neither: a HUD element being taken for world geometry and left at
// its native position. The per-frame minimum cannot show that unless it
// happens on a sampled frame; this can.
unsigned g_rej_z_total = 0;
unsigned g_rej_off_total = 0;
uint32_t g_rej_z_min_ever = 0xFFFFFFFFu;
// AND THE LAST BLIND SPOT IN THIS FILE: THE TWO SILENT CAPS.
//
// `Quad q[64]` is collected under `qn < 64` in the LOOP CONDITION, so hitting
// it stops the scan outright rather than skipping one quad -- every remaining
// polygon is abandoned, and g_hud_seen stops counting with it. `g_count < 64`
// drops boxes at emission the same way. A quad dropped by either is never
// moved, so it draws at its NATIVE position, which is the open/collapse
// defect's symptom exactly and would be intermittent in exactly the way it
// was seen to be.
//
// The sampled log cannot answer whether that happens: it prints every 120th
// frame, and the counts it showed (max 13 polys offered, max hudquads 10) are
// from sampled frames only. This is the third time in this file that a
// per-frame counter could not answer a question about a transient, so the cap
// gets the same cumulative treatment as the rest rather than another round of
// reasoning about whether 13 is close to 64.
//
// Two compares per frame, never reset.
unsigned g_poly_max_ever = 0;   // most polygons ever offered to one scan
unsigned g_qn_max_ever = 0;     // most HUD quads ever collected in one scan
unsigned g_qn_capped = 0;       // scans that actually hit the 64 ceiling
unsigned g_box_capped = 0;      // emissions that hit the g_boxes ceiling

// WHICH QUADS ARE ONE ELEMENT: TOUCHING IS NOT ENOUGH ACROSS TEXTURES
// (docs/365).
//
// Adjacency alone merged neighbouring WIDGETS. A tutorial box is 8x8 tiles
// of one texture (6DD01000), and the deselect button beside it is another
// (6ED00000) whose right edge is the box's left edge, x 27. Merged, box and
// button made one left-anchored element, and the box was dragged to the
// window's corner with it. At 1452x608 the combat button on the other side
// made the merged element right-anchored, and the whole box went to the
// bottom-right corner.
//
// Within one texture, touching still merges: the tiles of one box, the four
// quads of a sidebar brick. Across textures, two quads join only when their
// interiors overlap, like an icon or text drawn over a panel. A widget that
// only shares an edge with another stays its own element. LB_HUDGROUP=0
// restores adjacency alone, for an A/B.
bool hud_group_by_texture() {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_HUDGROUP");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on != 0;
}

// AN ELEMENT PLACED BY ITS INSET KEEPS ITS PLACE BESIDE THE EDGE ELEMENT IT
// TOUCHES (docs/383).
//
// Holding a building in the builder's red list makes the cartridge draw a
// label box, 24 tiles of 6DD01000 at native x 36..132 y 24..40, directly above
// the list (x 12..236 y 40..64). The two only share an edge and are different
// textures, so docs/365 keeps them apart, and each was placed by its own rule:
// the bricks-and-list element is against the top-left edges and goes flush
// (-4,-16 more than the band), while the label touches no edge and keeps its
// native inset from the corner. Measured at 1473x830 (run t383i): the list at
// -244,-127, the label at -240,-111. So the label landed on the list's top
// sixteen rows, was drawn under it and never seen, and a press held on a
// building was mapped through the label's face, slid off the button in the
// cartridge's held re-test, and selected nothing.
//
// The layout the cartridge drew is that the label sits on the list. So an
// element that is placed by the inset rule on an axis and touches an element
// held against the SAME side by its edge takes that element's delta on that
// axis: they stay together, as on the DS. Nothing else changes -- an element
// on no side (the centred tutorial box beside Deselect, docs/365) or against
// an edge itself is decided exactly as before. Like the press mapping, it
// applies live and to recordings that say `hudmemo=1`; LB_HUDATTACH=0 turns it
// off for an A/B.
bool hud_attach_live() {
    static int env = -1;
    if (env < 0) {
        const char* v = std::getenv("LB_HUDATTACH");
        env = (v && v[0] == '0') ? 0 : 1;
    }
    return env != 0 && lb_demo_play_hudmemo() != 0;
}

// A TEXT BOX STRETCHES BETWEEN THE BUTTONS WITHOUT STRETCHING ITS TEXT
// (docs/365).
//
// The message box must position and stretch (the box, not the font) between
// the deselect and combat-happening buttons. The 3-slice below does that for a single-quad bar, whose interior is flat.
// A tutorial box is different: 150 tiles of 8x8 (one texture), plus a
// separate 256-wide text quad in front of them. That text quad is never part
// of the element (it runs past x 256, so the on-screen test rejects it), but
// its pixels are inside the tiles' rectangles in the raster, so resampling
// the middle would resample the font.
//
// So a wide, centred, single-texture element of several quads that touches
// NEITHER edge is stretched as five slices:
//   * the caps (kSpanCap columns at each end) go to the anchors;
//   * the content between them stays at its native place and width;
//   * the two gaps take each row's majority colour over the content: the
//     border in the border rows, the interior in the text rows. A row with
//     no majority repeats the column just inside the left cap instead.
// It is declined unless that fallback column is clear of every other
// HUD-depth quad: the tutorial's text starts at x 32 and the column is 31.
//
// The buildings list touches both edges and keeps its SPAN-DECLINE; nothing
// else in the reference replay is multi-quad and 96 px wide.
//
// OFF BY DEFAULT until it is verified: the first two fills drew visible
// strips (see the fill note in lb_hud_shift_line). LB_HUDSTRETCH=1 turns it
// on.
bool hud_stretch_multi() {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_HUDSTRETCH");
        on = (v && v[0] == '1') ? 1 : 0;
    }
    return on != 0;
}

// WHAT EACH ELEMENT WAS AND WHAT WAS DONE WITH IT, for LB_HUDLOG. The moved
// list prints quads, and a 150-quad box printed as 150 identical lines said
// nothing about which widgets had been merged into it.
struct HudElemLog {
    int nx0, nx1, ny0, ny1, quads;
    uint32_t tex[3];
    int ntex;
    int dx, dy, dx1;
};
HudElemLog g_elem_log[32];
int g_elem_n = 0;

}  // namespace

// docs/375: the two questions the soft renderer asks once per frame. The
// renderer's HUD is exactly this file's HUD: the same depth threshold.
extern "C" unsigned lb_hud_z_threshold(void) { return hud_z_threshold(); }
// docs/380: and the UI engine's layer 0 with it -- the command icons and the
// placement checkmark, z 502, depth 4,275,712 -- which the renderer draws into
// the same set and flags (bit 31 of the colour). This file never moves them;
// it only keeps the fog and the footprint off them in the band.
extern "C" unsigned lb_hud_ui_threshold(void) { return 4275712u + 1u; }
extern "C" int lb_hud_split_wanted(void) {
    return (hud_split_env() && lb_setting_enhanced() &&
            lb_setting_hud_anchor() && hud_split_ab_now()) ? 1 : 0;
}

// See lb_overlay.h. Called once per presented frame by the enhanced layout.
extern "C" void lb_hud_set_visible_size(int columns, int rows) {
    g_visible_cols = columns;
    g_visible_rows = rows;
}

extern "C" void lb_hud_visible_size(int* columns, int* rows) {
    if (columns) *columns = g_visible_cols;
    if (rows) *rows = g_visible_rows;
}

// MOVING THE PIXELS IS ONLY HALF OF MOVING A BUTTON.
//
// The game's own hit test still believes the deselect icon is at native x 4..27
// -- this port only changed where it is DRAWN. So a click where the button now
// appears arrived as a touch at native -45, outside the panel, and was thrown
// away: anchoring the HUD had made it unclickable, so clicks outside the
// original aspect did not completely work.
//
// This undoes the move for input. A click inside an element's NEW rectangle is
// reported to the guest at the element's ORIGINAL position, so the cartridge's
// unmodified hit test finds it exactly where it thinks it is. No game code is
// patched and nothing has to be known about how it hit-tests.
//
// Returns 1 when the point was inside a moved element and gx was adjusted.
// `gx` is in SURFACE columns, the same space g_boxes uses.
// One box's inverse move: when surface point (gx, gy) is on the face the box
// is DRAWN at, replace it with the native point under it and return true;
// otherwise leave it and return false. Shared by the frontend's unmap below
// and the cartridge's widget hit test (lb_hud_hit_unmap, docs/371), so the two
// can never disagree about where an element is.
static bool hud_unmap_box(const HudBox& b, float scale, int* gx, int* gy) {
    const int bw = b.sx1 - b.sx0, bh = b.y1 - b.y0;
    const int dw = scale > 1.01f ? (int)(bw * scale) : bw;
    const int dh = scale > 1.01f ? (int)(bh * scale) : bh;
    // Where it is actually drawn -- the same geometry the shift uses, so a
    // scaled button is clickable over the whole of its enlarged face, and a
    // vertically anchored one over its face at the window's edge rather than
    // where the DS's 192 rows would have put it.
    // THE HIT REGION HAS TO STRETCH WITH THE FACE, for the reason the note
    // above gives: a click lands where the element is DRAWN. A stretched bar
    // whose hit test kept the native width would be clickable over a fraction
    // of itself, offset from where it appears.
    const bool stretching = (b.dx1 != b.dx);
    const int dwh = stretching ? (bw + b.dx1 - b.dx) : dw;
    const int dst_r = b.sx1 + b.dx;
    const int dx0 = stretching ? (b.sx0 + b.dx)
                               : ((b.dx > 0) ? (dst_r - dwh)
                                             : (b.sx0 + b.dx));
    const int dy1 = b.y1 + b.dy, dy0 = dy1 - dh;
    if (*gx < dx0 || *gx >= dx0 + dwh || *gy < dy0 || *gy >= dy1)
        return false;
    // Back to the element's own face, undoing the scale as well as the move,
    // so the guest gets the point the player actually aimed at. dwh, not dw:
    // for a stretched element those differ, and mapping back through the
    // unstretched width would hand the guest a point from the wrong part of
    // the face. The inverse is linear even though the forward map is a
    // 3-slice -- the caps are 4 px of a bar hundreds wide, so the error is
    // confined to them, and a flat interior has no feature for the difference
    // to land on.
    int within = dwh > 0 ? ((*gx - dx0) * bw) / dwh : 0;
    // A five-slice box maps back slice by slice, so a touch on its content
    // lands exactly on the native point under it; the two fills map to the
    // interior column beside them.
    if (b.slice5 && stretching && bw > 2 * kSpanCap) {
        const int i2 = *gx - dx0, lx = -b.dx;
        const int c_end = kSpanCap + lx + (bw - 2 * kSpanCap);
        if (lx >= 0 && c_end <= dwh - kSpanCap) {
            if (i2 < kSpanCap) within = i2;
            else if (i2 < kSpanCap + lx) within = kSpanCap;
            else if (i2 < c_end) within = i2 - lx;
            else if (i2 < dwh - kSpanCap) within = bw - kSpanCap - 1;
            else within = bw - (dwh - i2);
        }
    }
    const int withiny = dh > 0 ? ((*gy - dy0) * bh) / dh : 0;
    *gx = b.sx0 + within;
    *gy = b.y0 + withiny;
    return true;
}

extern "C" int lb_hud_unmap_touch(int* gx, int* gy) {
    if (!gx || !gy || g_count <= 0) return 0;
    const float scale = lb_setting_hud_scale();
    for (int i = 0; i < g_count; ++i)
        if (hud_unmap_box(g_boxes[i], scale, gx, gy)) return 1;
    return 0;
}

// docs/371. THE CARTRIDGE'S OWN HIT TEST, TOLD WHERE THE ELEMENT IS DRAWN.
//
// Clicking within the original aspect still hit moved HUD elements' original
// click points, and at some resolutions the menus were misaligned from their
// clickable positions.
//
// The port moves a HUD element's PICTURE; the widget behind it stays at its
// layout rectangle, and the game hit-tests every enabled widget whether or not
// it was drawn this frame (FUN_020E14BC; FUN_020EA1A4 is the rectangle test).
// The frontend used to fix the clicks from outside: it rewrote a click on a
// moved element to its native point (lb_hud_unmap_touch) and dropped a press on
// the spot it left (lb_hud_touch_phantom). Both read the polygons of the last
// composited frame, so both went blind on the frames the cartridge draws no HUD
// (docs/349 s9c): a press on the old spot then reached the invisible widget.
// And a dropped press is not a field click, which is what the player aimed.
//
// Now the widget test itself is corrected, per widget. widget_rect_observer
// hands the widget's rectangle and the tested point here:
//    1  the point is on the element's drawn face; (x, y) now holds the native
//       point under it, so the cartridge's test hits exactly as on a DS;
//   -1  the element was moved and the point is not on its face: the widget
//       must miss, and the manager then gives the press to the field;
//    0  the widget's element is not moved (or not found): answer as stock.
// Guest coordinates in and out; x0..x1 and y0..y1 are the test's inclusive
// bounds.
// The rule before docs/383, kept exactly, because a recording made under it
// must replay into the same presses (no `hudmemo=1` in its header). The
// caller holds g_hit_mu.
static int hit_unmap_legacy(int x0, int y0, int x1, int y1, int* x, int* y) {
    // The element is the box whose SOURCE (where the cartridge drew it) holds
    // the widget's centre. The smallest one, if several do: a merged group
    // and one of its members can both contain it.
    const int cx = (x0 + x1) / 2 + g_hit.band;
    const int cy = (y0 + y1) / 2 + g_hit.band_y;
    const HudBox* found = nullptr;
    long best = LONG_MAX;
    for (int i = 0; i < g_hit.n; ++i) {
        const HudBox& b = g_hit.box[i];
        if (cx < b.sx0 || cx >= b.sx1 || cy < b.y0 || cy >= b.y1) continue;
        const long area = (long)(b.sx1 - b.sx0) * (long)(b.y1 - b.y0);
        if (area < best) {
            best = area;
            found = &b;
        }
    }
    HudHitMemo* memo = nullptr;
    for (HudHitMemo& m : g_hit_memo)
        if (m.used && m.x0 == x0 && m.y0 == y0 && m.x1 == x1 && m.y1 == y1 &&
            m.band == g_hit.band && m.band_y == g_hit.band_y &&
            m.out_w == g_hit.out_w && m.out_h == g_hit.out_h) {
            memo = &m;
            break;
        }
    if (found) {
        if (!memo) {
            memo = &g_hit_memo[g_hit_memo_next++ % 64u];
            *memo = HudHitMemo{x0, y0, x1, y1, g_hit.band, g_hit.band_y,
                               g_hit.out_w, g_hit.out_h, *found, true};
        } else {
            memo->b = *found;
        }
    } else if (memo && g_hit.n == 0) {
        found = &memo->b;           // a frame with no HUD drawn: the last move
    }
    if (!found) return 0;
    const HudBox& b = *found;
    const bool moved = b.dx != 0 || b.dy != 0 || b.dx1 != 0 ||
                       g_hit.scale > 1.01f;
    if (!moved) return 0;
    int sx = *x + g_hit.band, sy = *y + g_hit.band_y;
    if (!hud_unmap_box(b, g_hit.scale, &sx, &sy)) return -1;
    *x = sx - g_hit.band;
    *y = sy - g_hit.band_y;
    return 1;
}

// docs/383: MAP THE PRESS, NOT THE WIDGET.
//
// A click on the HUD (red/blue bricks) sometimes under-clicked it and placed
// a movement cursor beneath it.
//
// The rule above finds each WIDGET's element by the widget's centre, and the
// boxes are single quads. Measured on a held battle (LB_HUDHITLOG, run
// t383a): the brick column is five quads over native x 4..20, the blue
// brick's widget 605 is x 4..12, the red one's 606 is x 4..12 lower down, and
// a third widget spans x 12..28 beside the blue one. A press on the right
// half of a drawn brick was then on no widget's own quad: 605 and 606
// answered "moved away", and the third widget's centre (x 20) lay just past
// every quad, so it answered as stock, at its old native place -- no widget
// took the press, and the manager gave it to the field as a move order under
// the button. On a DS the same point is native x 16 and the cartridge decides.
//
// So the PRESS is mapped first: if it is on any moved element's drawn face,
// it becomes the native point under it, for every widget, and the cartridge's
// own tests run there exactly as on the DS. Only a press on no moved face is
// kept away from a moved widget (-1), which is what stops a press on a
// button's old, now empty, spot from pressing it. Everything else is stock.
// The faces come from this frame's scan, or -- when the cartridge drew no HUD
// this frame (the frames after a camera step, docs/349 s9c) -- from the last
// scan that had any at the same band and surface.
static bool hit_moved(const HudBox& b, float scale) {
    return b.dx != 0 || b.dy != 0 || b.dx1 != 0 || scale > 1.01f;
}

static int hit_unmap_v2(int x0, int y0, int x1, int y1, int* x, int* y,
                        const char** why) {
    const HudHitSnap* s = &g_hit;
    if (g_hit.n == 0 && g_hit_last.n > 0 &&
        g_hit_last.band == g_hit.band && g_hit_last.band_y == g_hit.band_y &&
        g_hit_last.out_w == g_hit.out_w && g_hit_last.out_h == g_hit.out_h)
        s = &g_hit_last;
    if (s->n <= 0) {
        *why = "no HUD scanned yet: stock";
        return 0;
    }
    // 1. On a moved face: the native point under the press, through the face
    //    DRAWN ON TOP there. lb_hud_shift_line draws the boxes in index order
    //    and a later one overwrites an earlier, so that is the last box whose
    //    face holds the press: the element the player sees under the cursor.
    //    The first version took the smallest face instead, and a building's
    //    label box, drawn UNDER the list, won a press held on the list (run
    //    t383h; see hud_attach_live, which also stops the two overlapping).
    int fx = 0, fy = 0;
    bool on_face = false;
    for (int i = 0; i < s->n; ++i) {
        const HudBox& b = s->box[i];
        if (!hit_moved(b, s->scale)) continue;
        int tx = *x + s->band, ty = *y + s->band_y;
        if (!hud_unmap_box(b, s->scale, &tx, &ty)) continue;
        fx = tx;
        fy = ty;
        on_face = true;
    }
    if (on_face) {
        *x = fx - s->band;
        *y = fy - s->band_y;
        *why = (s == &g_hit) ? "on a moved face" : "on a moved face (last scan)";
        return 1;
    }
    // 2. On no moved face. A widget whose element moved -- its rect covers at
    //    least a quarter of its own area of a moved box's native source -- has
    //    left this spot, so it must miss.
    const long warea = (long)(x1 - x0 + 1) * (long)(y1 - y0 + 1);
    for (int i = 0; i < s->n; ++i) {
        const HudBox& b = s->box[i];
        if (!hit_moved(b, s->scale)) continue;
        const int bx0 = b.sx0 - s->band, bx1 = b.sx1 - s->band;
        const int by0 = b.y0 - s->band_y, by1 = b.y1 - s->band_y;
        const int ox = std::min(x1 + 1, bx1) - std::max(x0, bx0);
        const int oy = std::min(y1 + 1, by1) - std::max(y0, by0);
        if (ox <= 0 || oy <= 0) continue;
        if ((long)ox * (long)oy * 4 >= warea) {
            *why = "its element moved away: miss";
            return -1;
        }
    }
    *why = "not a moved element: stock";
    return 0;
}

extern "C" int lb_hud_hit_unmap(int x0, int y0, int x1, int y1, int* x,
                                int* y) {
    if (!x || !y) return 0;
    std::lock_guard<std::mutex> lk(g_hit_mu);
    // LB_HUDHITMEMO=0 is the old rule, for the A/B; so is a replay of a
    // recording made before docs/383 (no `hudmemo=1`), which must stay exact.
    static const bool v2_env = [] {
        const char* v = std::getenv("LB_HUDHITMEMO");
        return !(v && v[0] == '0');
    }();
    if (!v2_env || lb_demo_play_hudmemo() == 0)
        return hit_unmap_legacy(x0, y0, x1, y1, x, y);
    const int px = *x, py = *y;
    const char* why = "";
    const int r = hit_unmap_v2(x0, y0, x1, y1, x, y, &why);
    // LB_HUDHITLOG=1: every press on a moved face once (per point), and every
    // widget that misses because its element moved.
    static const bool log = std::getenv("LB_HUDHITLOG") != nullptr;
    static unsigned said = 0;
    static int last_px = INT_MIN, last_py = INT_MIN;
    if (log && said < 300u && r != 0 &&
        !(r > 0 && px == last_px && py == last_py)) {
        ++said;
        if (r > 0) { last_px = px; last_py = py; }
        // docs/385: the two bands -- the scan's, which placed the faces, and
        // the frame on screen's, which the frontend took off the press.
        std::fprintf(stderr, "[lb_hudhit] v2 rect %d..%d x %d..%d, press "
                     "%d,%d: %s -> %d,%d  [scan band %d,%d surface %dx%d "
                     "scale %.2f; shown band %d,%d]\n", x0, x1, y0, y1, px,
                     py, why, *x, *y, g_hit.band, g_hit.band_y, g_hit.out_w,
                     g_hit.out_h, g_hit.scale, lb_shown_band_x(),
                     lb_shown_band_y());
        // A miss: every moved face, as drawn, once per press point.
        static int faces_px = INT_MIN, faces_py = INT_MIN;
        if (r < 0 && (px != faces_px || py != faces_py)) {
            faces_px = px;
            faces_py = py;
            for (int i = 0; i < g_hit.n && i < 24; ++i) {
                const HudBox& b = g_hit.box[i];
                if (!hit_moved(b, g_hit.scale)) continue;
                std::fprintf(stderr, "[lb_hudhit]   face %d: source x %d..%d "
                             "y %d..%d, move %+d,%+d (x1 %+d); press at "
                             "surface %d,%d\n", i, b.sx0, b.sx1, b.y0, b.y1,
                             b.dx, b.dy, b.dx1, px + g_hit.band,
                             py + g_hit.band_y);
            }
        }
    }
    return r;
}

// AND THE OTHER HALF OF MOVING A BUTTON: THE PLACE IT LEFT.
//
// The defect: original-position HUD elements, even when invisible, could still
// be clicked within the original aspect (the red/blue bricks and the cancel
// button, for example).
//
// lb_hud_unmap_touch above moves a click from where an element is DRAWN to
// where the cartridge thinks it is. It does nothing about the reverse, and the
// reverse is a real click: the cartridge's hit test is still at the element's
// ORIGINAL rectangle, so a click on that now-empty patch of field finds the
// button and presses it. Nothing is drawn there, so the press has no visible
// cause -- which is exactly how the defect presented.
//
// This reports such a point so the caller can DROP the delivery entirely.
//
// DROPPING IS THE RIGHT ACTION, and the alternatives are worse. Displacing the
// point to somewhere no widget covers does not help: with no widget under it
// the click becomes a FIELD click, and the player who touched empty ground gets
// a move order instead of a phantom button press. A dropped touch is simply a
// touch that did not happen, which is what the player's eyes already believe.
//
// ORDER MATTERS, and it is the caller's job: this must only be consulted after
// lb_hud_unmap_touch has returned 0. A point can lie inside element A's NEW
// rectangle and element B's ORIGINAL one at the same time, and the real click
// on A must win over the phantom of B.
//
// Only elements that actually MOVED get a phantom zone. For one with dx, dy and
// dx1 all zero the original rectangle IS the drawn rectangle, so treating it as
// a phantom would make a stationary button unclickable -- the precise opposite
// of the bug.
extern "C" int lb_hud_touch_phantom(int gx, int gy) {
    if (g_count <= 0) return 0;
    for (int i = 0; i < g_count; ++i) {
        const HudBox& b = g_boxes[i];
        if (b.dx == 0 && b.dy == 0 && b.dx1 == 0) continue;
        if (gx >= b.sx0 && gx < b.sx1 && gy >= b.y0 && gy < b.y1) {
            // OBSERVABLE, BECAUSE A REPLAY CANNOT PRODUCE THIS CLICK.
            //
            // The harness aims at things it can see; this guard only fires on
            // a click at a place where there is deliberately nothing. So the
            // only way to know it works is for a real play session to say so,
            // and that needs a line in the log.
            //
            // It costs nothing when it never fires -- and it CANNOT fire often:
            // the caller consults it only on a press edge, so this is bounded
            // by how fast a person can click, not by frame rate. Capped anyway,
            // because a log that can grow without bound is how this project
            // once produced 74 MB in one run.
            static int said = 0;
            if (said < 16) {
                ++said;
                std::fprintf(stderr,
                             "[lb_hud] phantom press at %d,%d dropped -- "
                             "element %d left this rect (%d..%d, %d..%d) "
                             "moved by %+d,%+d%s\n",
                             gx, gy, i, b.sx0, b.sx1, b.y0, b.y1, b.dx, b.dy,
                             said == 16 ? "  (further ones silent)" : "");
                std::fflush(stderr);
            }
            return 1;
        }
    }
    return 0;
}

// docs/348: does native fog cell (cx, cy) -- the cartridge's 32x24 UI grid --
// hold a hole punched for an element this port has moved away? See g_refog.
// Read by the compositor's workers; written only by lb_hud_scan, before they
// start.
extern "C" int lb_hud_refog_cell(int cx, int cy) {
    if (cx < 0 || cx >= 32 || cy < 0 || cy >= 24) return 0;
    return (g_refog[cy] >> cx) & 1u;
}

// docs/348: the band the live polygon list was placed with. Set before each
// scan by the widened composite; see g_list_band.
extern "C" void lb_hud_set_list_band(int band, int band_y) {
    g_list_band = band;
    g_list_band_y = band_y;
}

// Rebuild the list of HUD quads that need moving. Once per frame.
static void lb_hud_scan_boxes(int output_width, int output_height, int band,
                              int band_y);

// Rebuild the boxes, then publish them for the widget hit test (docs/371). A
// wrapper, so every early return inside the scan publishes too.
extern "C" void lb_hud_scan(int output_width, int output_height, int band,
                            int band_y) {
    lb_hud_scan_boxes(output_width, output_height, band, band_y);
    g_raster_band = band;       // docs/380
    g_raster_band_y = band_y;
    std::lock_guard<std::mutex> lk(g_hit_mu);
    g_hit.n = g_count;
    g_hit.band = band;
    g_hit.band_y = band_y;
    g_hit.out_w = output_width;
    g_hit.out_h = output_height;
    g_hit.scale = lb_setting_hud_scale();
    for (int i = 0; i < g_count; ++i) g_hit.box[i] = g_boxes[i];
    if (g_count > 0) {                                  /* docs/383 */
        g_hit_last.n = g_count;
        g_hit_last.band = band;
        g_hit_last.band_y = band_y;
        g_hit_last.out_w = output_width;
        g_hit_last.out_h = output_height;
        g_hit_last.scale = g_hit.scale;
        for (int i = 0; i < g_count; ++i) g_hit_last.box[i] = g_boxes[i];
    }
}

static void lb_hud_scan_boxes(int output_width, int output_height, int band,
                              int band_y) {
    // Last frame's captures become the source for this frame's scaled draw.
    // SWAPPED, NOT COPIED: at 256 slots a copy is 256 vector allocations a
    // frame. cur is reassigned before it is written (px.assign below), so the
    // buffer it inherits is never read.
    for (int i = 0; i < kMaxHudQuads; ++i) {
        std::swap(g_cache_prev[i], g_cache_cur[i]);
        g_cache_cur[i].w = g_cache_cur[i].h = 0;
    }
    g_prev_count = g_count;
    g_count = 0;
    g_elem_n = 0;
    ++g_compose_frame;   // docs/385: LB_HUDTRACE's composite number
    // docs/348: before any early return, so no path leaves last frame's.
    std::memset(g_refog, 0, sizeof g_refog);
    std::memset(g_keephole, 0, sizeof g_keephole);
    g_reject_n = 0;
    g_off_n = 0;
    g_hud_seen = 0;
    g_rej_z = 0;
    g_rej_offscreen = 0;
    g_rej_z_min = 0xFFFFFFFFu;
    // NOTE the scan still runs with anchoring OFF; only the MOVE is skipped.
    //
    // The count of on-screen HUD quads is the only reliable "is a unit
    // selected?" signal this port has -- the in-field HUD does not exist until
    // something is selected. Returning early when anchoring was off made that
    // signal disappear exactly when a test needed anchoring off (to reach the
    // deselect button at its native position), which is a circular dependency
    // that cost two runs.
    const bool moving = lb_setting_hud_anchor() != 0;
    // TWO DIFFERENT WIDTHS, and conflating them was the bug.
    //
    // `extra` converts surface x back to the guest's own 256-wide coordinates,
    // so it is always half the surface's overhang. `shift` is how far a corner
    // element may actually MOVE, and that is governed by what the window is
    // showing, not by how wide the surface happens to be. The layout covers
    // rather than letterboxes, so on a 16:9 window roughly 341 of the 448
    // columns are on screen and the rest is cropped -- moving a button to
    // surface x 443 put it in the cropped region, where the player never sees
    // it. That is the "HUD buttons don't fully match the window" report.
    // PASSED IN, not fetched.
    //
    // This used to call lb_band_left itself, which is the band as of the last
    // vblank. The rows about to be composited use the band that came back with
    // the 3D frame they are compositing, and near a map edge those differ by a
    // frame -- which would put the anchor a pan step out from the geometry it
    // is anchoring, every frame, for as long as the camera is against the edge.
    // The caller knows which one it is using; it says so.
    //
    // RETRACTED: "X MUST NOT USE IT". It must, and this line using the centred
    // half-overhang is what stopped the HUD anchoring at play resolutions.
    //
    // The retracted text said horizontal 3D geometry is not band-biased --
    // that the viewport substitution centres it and only the Y path carries
    // `lb_band_y - lb_ycentre` -- and measured it on a 946x516 surface where
    // half-overhang reproduced the deselect icon's documented native span and
    // `band` did not. That measurement was correct WHEN IT WAS TAKEN and is
    // not correct now: SubmitVertex was later given the matching horizontal
    // term (docs/268), which adds `lb_band_left(RenderWidth) - centre` to
    // every vertex. Horizontal geometry has been band-biased ever since, and
    // this line kept undoing a shift the renderer had stopped applying.
    //
    // What it cost, measured with a unit selected while the window grew:
    //
    //     surface     real band   this line   quads anchored
    //     772x550       258         258         9
    //     996x718       370         370         9
    //     1252x670      498 (!)     498         0   <- all 9 "offscreen"
    //     1728x768      476         736         0   <- native x -256..-193
    //
    // The two that worked are the two where the camera was far enough from a
    // map edge for the clamped band to equal the centred split, which is
    // exactly the coincidence the touch mapping was also living on (docs/275).
    // At 1728x768 the nine quads that anchor correctly at 996 report native
    // x -256..-193, fail `nx0 >= 0`, and are dropped before the classifier
    // sees them -- so nothing moves and the whole HUD sits where the 256-px
    // screen put it, instead of the gameplay HUD constraining to the edges and
    // corners.
    //
    // The caller already passes the right number and says why in its own
    // comment: "Given the SAME band the rows are about to composite with ...
    // the anchor that undoes that has to use the same number or it mis-locates
    // every quad by the difference." Use it.
    //
    // The dx formulas below are unaffected. They are written against "where
    // the element is", which is `extra + native` under either rule, so the
    // native term still cancels; they reduce to -shift/+shift exactly when
    // extra == (W - 256) / 2, which is now a special case rather than the
    // definition.
    const int extra = band;
    // THE SAME TWO QUANTITIES, VERTICALLY.
    //
    // Once the window buys rows as well as columns (docs/215), a HUD element
    // the game put against the bottom of its 192-row screen is drawn
    // `extra_y` rows above the bottom of the surface -- melonDS's viewport
    // shift centres all 3D geometry, and the in-field HUD IS 3D geometry. So
    // it floats away from the edge exactly as the corner icons floated
    // sideways before the horizontal anchor existed. This is that fix, turned
    // ninety degrees.
    //
    // KEEPS THE BAND, unlike `extra` above. Not an oversight and not symmetry
    // worth tidying: the vertical viewport really is band-biased
    // (`lb_ybias = lb_band_y - lb_ycentre` in UpdateClipMatrix's pair), so
    // surface y minus band_y is the native row, measured. Changing this one to
    // half the overhang moves the deselect icon from its documented 167..191
    // to 149..173.
    const int extra_y = band_y;   // passed in, for the reason `extra` is
    // THE SURFACE'S SIZE, NOT THE BAND'S VALUE.
    //
    // This tested `extra` and `extra_y`, which was the same question while
    // `extra` was half the overhang: it is zero exactly when the surface is no
    // wider than the screen, and then there is nothing to anchor. Now that
    // `extra` is the real band it is ALSO zero when the camera is hard against
    // a map edge and the whole overhang has gone to the other side -- a
    // widened surface with plenty to anchor. Asking the surface directly says
    // what was always meant.
    if (output_width <= 256 && output_height <= 192) return;
    int vis = g_visible_cols > 0 ? g_visible_cols : output_width;
    if (vis > output_width) vis = output_width;
    if (vis < 256) vis = 256;
    const int shift = (vis - 256) / 2;
    int visr = g_visible_rows > 0 ? g_visible_rows : output_height;
    if (visr > output_height) visr = output_height;
    if (visr < 192) visr = 192;
    const int shift_y = (visr - 192) / 2;

    const uint32_t zmax = hud_z_threshold();
    // TWO PASSES, BECAUSE AN ELEMENT IS NOT ONE QUAD.
    //
    // Deciding the anchor per quad TEARS elements in half. Measured with the
    // base selected: the sidebar brick is a 2x2 block of quads spanning native
    // y 16..64, and the per-quad test moved its top half (ny0 = 16, inside the
    // edge slack) while leaving its bottom half (ny0 = 40) where it was --
    //
    //     x 71..79 y 26..50 -> -68,-10      top half moves up
    //     x 71..79 y 50..74 -> -68, +0      bottom half stays
    //
    // -- opening a ten-pixel gap down the middle of one brick. That is why
    // the two brick sidebar elements floated away from the window edge: half
    // of each was anchored and half was not.
    //
    // So the quads are collected first, merged into ELEMENTS by adjacency, and
    // the anchor is decided once per element from its whole bounding box. The
    // horizontal axis had the same latent bug and never showed it, because the
    // two columns of a brick share a y range and so always agreed.
    struct Quad { int sx0, sx1, sy0, sy1; int group; uint32_t tex; bool cmd; };
    Quad q[kMaxHudQuads];
    int qn = 0;
    // HUD-depth quads the on-screen test rejected, in raster coordinates:
    // the tutorial's 256-wide text quad is one. Not elements, but they draw
    // into the rows an element stretch repeats from (hud_stretch_multi).
    struct Rect { int sx0, sx1, sy0, sy1; };
    Rect offq[64];
    int offn = 0;
    // docs/348: see g_list_band.
    const int list_x = (g_list_band != INT32_MIN) ? g_list_band : extra;
    const int list_y = (g_list_band_y != INT32_MIN) ? g_list_band_y : extra_y;
    const uint32_t n = nds_gpu3d_render_polygon_count();
    for (uint32_t i = 0; i < n && qn < kMaxHudQuads; ++i) {
        NdsGpu3dPolygonSnapshot p{};
        if (!nds_gpu3d_render_polygon(i, &p)) continue;
        // A command icon (z 502, depth 4,275,712) punches its own hole and is
        // never moved; see g_refog.
        if (p.min_z == 4275712u)
            mark_ui_cells(g_keephole, (int)p.min_x - list_x,
                          (int)p.min_y - list_y,
                          (int)p.max_x - (int)p.min_x,
                          (int)p.max_y - (int)p.min_y);
        if (p.min_z >= zmax) {                      // world geometry
            ++g_rej_z;
            ++g_rej_z_total;                       /* cumulative twin */
            if (p.min_z < g_rej_z_min) g_rej_z_min = p.min_z;
            if (p.min_z < g_rej_z_min_ever) g_rej_z_min_ever = p.min_z;
            continue;
        }
        const int nx0 = (int)p.min_x - list_x;
        const int nx1 = (int)p.max_x - list_x;
        const int ny0 = (int)p.min_y - list_y;
        const int ny1 = (int)p.max_y - list_y;
        // IT HAS TO BE ON THE DS'S OWN SCREEN TO BE ITS HUD, in BOTH axes.
        //
        // Measured with a unit selected, the list contains quads at surface
        // x 0..10 -- native -96..-86, already out in the widened column. A
        // screen-locked HUD element cannot be there; those are world geometry
        // that happens to fall under the z threshold, and shifting them again
        // threw them further out. The z test alone was not enough.
        if (!(nx0 >= 0 && nx1 <= 256 && ny0 >= 0 && ny1 <= 192)) {
            if (offn < 64)
                offq[offn++] = {nx0 + extra, nx1 + extra, ny0 + extra_y,
                                ny1 + extra_y};
            ++g_rej_offscreen;
            ++g_rej_off_total;                     /* cumulative twin */
            if (g_off_n < 8) {
                g_off[g_off_n].sx0 = (int)p.min_x;
                g_off[g_off_n].sx1 = (int)p.max_x;
                g_off[g_off_n].sy0 = (int)p.min_y;
                g_off[g_off_n].sy1 = (int)p.max_y;
                g_off[g_off_n].nx0 = nx0;
                g_off[g_off_n].nx1 = nx1;
                g_off[g_off_n].ny0 = ny0;
                g_off[g_off_n].ny1 = ny1;
                ++g_off_n;
            }
            continue;
        }
        ++g_hud_seen;
        // Where this quad's pixels are in the RASTER being composited.
        q[qn].sx0 = nx0 + extra;
        q[qn].sx1 = nx1 + extra;
        q[qn].sy0 = ny0 + extra_y;
        q[qn].sy1 = ny1 + extra_y;
        q[qn].group = qn;
        q[qn].tex = p.tex_param;
        q[qn].cmd = p.min_z == 4275712u;   // a command icon (see above)
        ++qn;
    }

    // Union-find by ADJACENCY. Two quads belong to one element when their
    // rectangles touch or overlap; a 1-px slack covers quads that abut exactly.
    // Across TEXTURES only an overlap counts: see hud_group_by_texture().
    auto find = [&q](int a) {
        while (q[a].group != a) a = q[a].group = q[q[a].group].group;
        return a;
    };
    const bool by_texture = hud_group_by_texture();
    for (int a = 0; a < qn; ++a) {
        for (int b = a + 1; b < qn; ++b) {
            const bool touch =
                q[a].sx0 <= q[b].sx1 + 1 && q[b].sx0 <= q[a].sx1 + 1 &&
                q[a].sy0 <= q[b].sy1 + 1 && q[b].sy0 <= q[a].sy1 + 1;
            if (!touch) continue;
            if (by_texture && q[a].tex != q[b].tex) {
                const bool overlap =
                    q[a].sx0 < q[b].sx1 && q[b].sx0 < q[a].sx1 &&
                    q[a].sy0 < q[b].sy1 && q[b].sy0 < q[a].sy1;
                if (!overlap) continue;
            }
            const int ra = find(a), rb = find(b);
            if (ra != rb) q[rb].group = ra;
        }
    }

    // The cumulative maxima, recorded once the collection loop has finished.
    if ((unsigned)n > g_poly_max_ever) g_poly_max_ever = (unsigned)n;
    if ((unsigned)qn > g_qn_max_ever) g_qn_max_ever = (unsigned)qn;
    if (qn >= kMaxHudQuads) ++g_qn_capped;   /* the scan stopped early */

    // docs/383: each element's EDGE delta, where it has one, before any is
    // placed, so an inset element can take its neighbour's (hud_attach_live).
    // The same tests and formulas as the edge arm below: an element against
    // exactly one side of an axis goes flush to that side of the window.
    int edge_dx[kMaxHudQuads], edge_dy[kMaxHudQuads];
    signed char edge_sx[kMaxHudQuads], edge_sy[kMaxHudQuads];  // -1 L/T, +1 R/B
    const bool attach = moving && hud_attach_live();
    for (int r = 0; r < qn; ++r) {
        edge_dx[r] = edge_dy[r] = INT_MIN;
        edge_sx[r] = edge_sy[r] = 0;
        if (!attach || find(r) != r) continue;
        int x0 = 1 << 20, x1 = -(1 << 20), y0 = 1 << 20, y1 = -(1 << 20);
        for (int i = 0; i < qn; ++i) {
            if (find(i) != r) continue;
            x0 = std::min(x0, q[i].sx0);
            x1 = std::max(x1, q[i].sx1);
            y0 = std::min(y0, q[i].sy0);
            y1 = std::max(y1, q[i].sy1);
        }
        const int ex0 = x0 - extra, ex1 = x1 - extra;
        const int ey0 = y0 - extra_y, ey1 = y1 - extra_y;
        const int ecx = (ex0 + ex1) / 2, ecy = (ey0 + ey1) / 2;
        if (ecx < 0 || ecx > 256 || ecy < 0 || ecy > 192) continue;  // world
        const bool l = ex0 <= kEdgeSlack, rt = ex1 >= 256 - kEdgeSlack;
        const bool t = ey0 <= kEdgeSlack, bm = ey1 >= 192 - kEdgeSlack;
        if (output_width > 256 && ex1 > ex0 && l != rt) {
            edge_dx[r] = rt ? ((output_width + vis) / 2 - 256 - extra) +
                                  (256 - ex1)
                            : ((output_width - vis) / 2 - extra) - ex0;
            edge_sx[r] = rt ? 1 : -1;
        }
        if (output_height > 192 && ey1 > ey0 && t != bm) {
            edge_dy[r] = bm ? ((output_height + visr) / 2 - 192 - extra_y) +
                                  (192 - ey1)
                            : ((output_height - visr) / 2 - extra_y) - ey0;
            edge_sy[r] = bm ? 1 : -1;
        }
    }
    // docs/385: THE DRAWERS' PIECES FOLLOW THE BRICKS. The drawer open/close
    // animations still did not always occur at the adaptive position of the
    // HUD (the open/collapse-animation defect).
    //
    // LB_HUDTRACE on a list opening and closing (run t385d) shows why, frame
    // by frame. A list's items slide along their row between the brick
    // column and their slots, and every item in flight is an element of its
    // own -- a gap separates it from the column -- so it is placed by where it
    // happens to be:
    //
    //   [4..20 x 16..64 q4 -244,-127]     the brick column, to its corner
    //   [68..100 x 40..64 q2 -240,-111]   an item in the left third
    //   [100..132 x 40..64 q2 +0,-111]    one mid-screen: stays mid-field
    //   [212..244 x 40..64 q2 +251,-111]  one at the right: to the RIGHT edge
    //
    // and once the items reach their slots they touch the column and move
    // with it ([4..116 x 16..64 q8 -244,-127]). The shift in the middle frames
    // is the red list seen drawn mid-field. The
    // SPAN-DECLINE note below guessed at a spanning group; it was this.
    //
    // So the pieces take the anchor's shift. The anchor is the element
    // against the left edge, in the top half and at least a brick tall -- the
    // brick column, with whatever part of the list already touches it -- and
    // a piece is a small element (one item, at most 48x26) that is not
    // against the left edge itself, lies in the anchor's rows or the one below
    // them (the red list's second row), shares a texture with it and holds no
    // command icon (those are world-anchored). The anchor's shift is worked
    // out with the placement loop's own formulas.
    int dr_root = -1, dr_nx0 = 0, dr_ny0 = 0, dr_ny1 = 0;
    int dr_dx = 0, dr_dy = 0;
    if (moving && output_width > 256) {
        for (int r = 0; r < qn; ++r) {
            if (find(r) != r) continue;
            int x0 = 1 << 20, x1 = -(1 << 20), y0 = 1 << 20, y1 = -(1 << 20);
            for (int i = 0; i < qn; ++i) {
                if (find(i) != r) continue;
                x0 = std::min(x0, q[i].sx0);
                x1 = std::max(x1, q[i].sx1);
                y0 = std::min(y0, q[i].sy0);
                y1 = std::max(y1, q[i].sy1);
            }
            const int ex0 = x0 - extra, ex1 = x1 - extra;
            const int ey0 = y0 - extra_y, ey1 = y1 - extra_y;
            if (!(ex0 <= kEdgeSlack && ex1 < 256 - kEdgeSlack)) continue;
            if ((ey0 + ey1) / 2 >= 96 || ey1 - ey0 < 24) continue;
            if (dr_root >= 0 && ey1 - ey0 <= dr_ny1 - dr_ny0) continue;
            dr_root = r;
            dr_nx0 = ex0;
            dr_ny0 = ey0;
            dr_ny1 = ey1;
        }
        if (dr_root >= 0) {
            dr_dx = ((output_width - vis) / 2 - extra) - dr_nx0;
            if (output_height > 192) {
                const bool t = dr_ny0 <= kEdgeSlack;
                const bool bm = dr_ny1 >= 192 - kEdgeSlack;
                const int cy = (dr_ny0 + dr_ny1) / 2;
                const int dyt = (output_height - visr) / 2 - extra_y;
                const int dyb = (output_height + visr) / 2 - 192 - extra_y;
                if (t != bm)
                    dr_dy = bm ? dyb + (192 - dr_ny1) : dyt - dr_ny0;
                else if ((cy < 192 / 3) != (cy > (192 * 2) / 3))
                    dr_dy = cy > (192 * 2) / 3 ? dyb : dyt;
            }
        }
    }
    auto drawer_piece = [&](int root, int nx0, int nx1, int ny0,
                            int ny1) -> bool {
        if (dr_root < 0 || root == dr_root) return false;
        if (nx0 <= kEdgeSlack || nx1 - nx0 > 48 || ny1 - ny0 > 26) return false;
        if (ny0 < dr_ny0 - 1 || ny1 > dr_ny1 + 25) return false;
        bool shares = false;
        for (int i = 0; i < qn; ++i) {
            if (find(i) != root) continue;
            if (q[i].cmd) return false;
            for (int j = 0; j < qn && !shares; ++j)
                shares = find(j) == dr_root && q[j].tex == q[i].tex;
        }
        return shares;
    };

    // The edge delta on axis `ax` (0 x, 1 y) of an element that touches
    // element `root` and is held to `side` of that axis; INT_MIN if none.
    // "Touches" is the grouping's own test, 1 px of slack.
    auto attached_delta = [&](int root, int ax, int side) -> int {
        for (int i = 0; i < qn; ++i) {
            if (find(i) != root) continue;
            for (int j = 0; j < qn; ++j) {
                const int rj = find(j);
                if (rj == root) continue;
                if ((ax == 0 ? edge_sx[rj] : edge_sy[rj]) != side) continue;
                if (q[i].sx0 <= q[j].sx1 + 1 && q[j].sx0 <= q[i].sx1 + 1 &&
                    q[i].sy0 <= q[j].sy1 + 1 && q[j].sy0 <= q[i].sy1 + 1)
                    return ax == 0 ? edge_dx[rj] : edge_dy[rj];
            }
        }
        return INT_MIN;
    };

    for (int root = 0; root < qn && g_count < kMaxHudQuads; ++root) {
        if (find(root) != root) continue;           // not a group leader
        // The ELEMENT's bounding box, in the coordinates the game laid it out
        // in, which is where "is this in the corner" is a meaningful question.
        int bx0 = 1 << 20, bx1 = -(1 << 20), by0 = 1 << 20, by1 = -(1 << 20);
        int quads = 0;
        uint32_t texs[3] = {0u, 0u, 0u};
        int ntex = 0;
        for (int i = 0; i < qn; ++i) {
            if (find(i) != root) continue;
            if (q[i].sx0 < bx0) bx0 = q[i].sx0;
            if (q[i].sx1 > bx1) bx1 = q[i].sx1;
            if (q[i].sy0 < by0) by0 = q[i].sy0;
            if (q[i].sy1 > by1) by1 = q[i].sy1;
            ++quads;
            bool seen = false;
            for (int t = 0; t < ntex; ++t) seen = seen || texs[t] == q[i].tex;
            if (!seen && ntex < 3) texs[ntex++] = q[i].tex;
        }
        // For LB_HUDLOG: filled in here, the decision added below.
        HudElemLog* el = g_elem_n < 32 ? &g_elem_log[g_elem_n++] : nullptr;
        if (el) {
            el->nx0 = bx0 - extra;
            el->nx1 = bx1 - extra;
            el->ny0 = by0 - extra_y;
            el->ny1 = by1 - extra_y;
            el->quads = quads;
            el->ntex = ntex;
            for (int t = 0; t < 3; ++t) el->tex[t] = texs[t];
            el->dx = el->dy = el->dx1 = 0;
        }
        const int nx0 = bx0 - extra, nx1 = bx1 - extra;
        const int ny0 = by0 - extra_y, ny1 = by1 - extra_y;
        const int w = nx1 - nx0, h = ny1 - ny0;
        // WHICH EDGES IS THE ELEMENT AGAINST?
        //
        // This used to be a size test -- anchor only elements narrower than
        // kCornerMaxWidth -- guarding against "a 255-px-wide quad at HUD depth
        // whose right edge is well past the native screen", which an edge test
        // alone would have thrown sideways.
        //
        // The size was a proxy, and the thing it was proxying for is stated
        // exactly by the edges: an element touching BOTH sides SPANS the
        // screen, cannot be put against both window sides by moving it, and
        // wants stretching instead -- which is a separate job. An element
        // touching ONE side is against that side whatever its width, and
        // belongs against the corresponding window side.
        //
        // The proxy cost something real. Selecting the builder and tapping the
        // red brick opens the buildings list, and the list plus the two
        // sidebar bricks merge into one element 232 px wide -- so the whole
        // thing was declined and every quad reported `-> +0,-10`. The bricks
        // anchor left on their own, so opening the list made them JUMP 68 px
        // right, back into the middle of the field, with the bar floating
        // beside them. That bar's native span is 3..235: hard against the left
        // edge, and 21 px clear of the right, so it is left-anchored and
        // nothing about it is ambiguous.
        // A QUAD CENTRED OUTSIDE THE NATIVE SCREEN IS NOT A HUD ELEMENT.
        //
        // On the deployed build, command cursors still frequently misaligned,
        // moving to the far edge of the screen.
        //
        // That is this code. The contextual command icons -- the harvest icon
        // on a crystal, the move arrow, patrol, attack (docs/283) -- are
        // 16x16 quads drawn at HUD depth, so this scan sees them. But they are
        // WORLD-anchored: func_020A8DD0 positions them at `world - camera`,
        // which is the true offset from the camera and may legitimately exceed
        // 256 now that the native visibility cull is gone. The tests below then
        // read `nx1 >= 240` as "this element is against the right edge" and the
        // anchoring arm flings it to the window's edge.
        //
        // The distinction that holds: a real HUD element is SCREEN-anchored, so
        // its centre is always inside the native screen -- that is what makes
        // it HUD. A world-anchored marker out in the band has its centre
        // outside. Centre rather than edge, because a legitimate HUD element
        // may straddle a native edge by a few pixels and must still anchor.
        //
        // Declining here leaves the quad exactly where the guest put it, which
        // is already correct: the 3D viewport is SHIFTED, not scaled (docs/217),
        // so native x 300 lands at band_left + 300 -- on the world point the
        // player actually clicked.
        const int world_cx = (nx0 + nx1) / 2;
        const int world_cy = (ny0 + ny1) / 2;
        if (world_cx < 0 || world_cx > 256 || world_cy < 0 || world_cy > 192) {
            if (g_reject_n < 32) {
                g_reject[g_reject_n].nx0 = nx0;
                g_reject[g_reject_n].nx1 = nx1;
                g_reject[g_reject_n].y0 = ny0;
                g_reject[g_reject_n].y1 = ny1;
                g_reject[g_reject_n].z = 0u;
                ++g_reject_n;
            }
            continue;
        }
        const bool at_left = nx0 <= kEdgeSlack;
        const bool at_right = nx1 >= 256 - kEdgeSlack;
        const bool at_top = ny0 <= kEdgeSlack;
        const bool at_bottom = ny1 >= 192 - kEdgeSlack;
        // WHERE IT IS, NOT WHERE IT WOULD BE IF THE BAND WERE CENTRED.
        //
        // This moved by +/-shift, and shift is (vis - 256) / 2 -- half the
        // VISIBLE overhang. That is the right distance only while the element
        // sits at the centred position, because the two cancel: an element at
        // surface `extra + native` moved by `shift` lands on the edge exactly
        // when extra == (output_width - 256) / 2.
        //
        // lb_band_left no longer guarantees that. At a map edge the band goes
        // to 0 while shift stays at half the overhang, so a corner button was
        // being thrown a full band's width into the middle of the field -- the
        // HUD was visibly misaligned (the red+blue and cancel buttons). Caught
        // by logging `band` beside
        // `shift` and seeing 132 against 265 on the same line.
        //
        // Stated from first principles instead. The element is at
        // `extra + native`; the visible region is centred on the surface and
        // spans [(W - vis) / 2, (W + vis) / 2). So a right-anchored element
        // wants (W + vis) / 2 - 256 + native, and a left-anchored one wants
        // (W - vis) / 2 + native; subtract where it is and the `native` term
        // cancels out of both. These reduce to +/-shift exactly when
        // extra == (W - 256) / 2, which is why nothing changed before the
        // clamp existed.
        const int dx_right = (output_width + vis) / 2 - 256 - extra;
        const int dx_left = (output_width - vis) / 2 - extra;
        const int dy_bottom = (output_height + visr) / 2 - 192 - extra_y;
        const int dy_top = (output_height - visr) / 2 - extra_y;
        // INSET CORNER ELEMENTS COUNT AS CORNER ELEMENTS.
        //
        // kEdgeSlack is 16, so an element has to be within 16 px of a native
        // edge to be classified at all. Several of this game's are further in
        // than that -- measured, the resource icons sit about 31 px from their
        // edge -- so both tests came back false, `at_left != at_right` was
        // false, and dx stayed 0. The element then keeps its NATIVE position,
        // which on a 1600 px window is the middle of the screen. That is why
        // the UI did not conform to the window's left and right edges, and the
        // earlier reading that the HUD was correct was taken at 996x720 and did not generalise.
        //
        // The edge test is kept exactly as it was wherever it decides -- no
        // element that moved before moves differently now. This only supplies
        // an answer where it previously declined: an element whose CENTRE
        // falls in the outer third of the native screen is anchored to that
        // side.
        //
        // Thirds, not halves, because the middle must still be left alone. The
        // note above records a bar whose native span is 3..235 -- hard against
        // the left edge and 21 px clear of the right -- and a centred element
        // dragged to an edge is a worse fault than one left where it was.
        const int cx = (nx0 + nx1) / 2;
        const int cy = (ny0 + ny1) / 2;
        const bool inset_left = cx < 256 / 3;
        const bool inset_right = cx > (256 * 2) / 3;
        const bool inset_top = cy < 192 / 3;
        const bool inset_bottom = cy > (192 * 2) / 3;
        // AGAINST THE EDGE MEANS AGAINST IT, not "as far in as it was".
        //
        // The formulas above move an element by a fixed distance, so an
        // element the game placed 4 px inside its own screen edge ends up 4 px
        // inside the WINDOW edge. On the DS that margin is a margin; against a
        // window border it reads as a misalignment: a slight gap showed between
        // the window border and the red+blue and cancel buttons.
        //
        // Measured on the element in a capture of that gap: surface x 374..382 with
        // dx -370 lands it at surface x 4, and at pixel_scale 2 that is the
        // ~8 px gap in the picture.
        //
        // So for an element that IS against a native edge, take its own inset
        // off as well and put its edge on the window's. Only for that case:
        // the inset-thirds branch below decides for elements that touch no
        // edge at all, where the native offset is the whole of what positions
        // them and removing it would drag the middle of the HUD outwards.
        int dx = 0, dy = 0;
        if (moving && output_width > 256 && w > 0) {
            if (at_left != at_right)
                dx = at_right ? (dx_right + (256 - nx1)) : (dx_left - nx0);
            else if (at_left == at_right && inset_left != inset_right) {
                dx = inset_right ? dx_right : dx_left;
                if (attach) {   // docs/383: beside its edge neighbour
                    const int a = attached_delta(root, 0, inset_right ? 1 : -1);
                    if (a != INT_MIN) dx = a;
                }
            }
        }
        if (moving && output_height > 192 && h > 0) {
            if (at_top != at_bottom)
                dy = at_bottom ? (dy_bottom + (192 - ny1)) : (dy_top - ny0);
            else if (at_top == at_bottom && inset_top != inset_bottom) {
                dy = inset_bottom ? dy_bottom : dy_top;
                if (attach) {   // docs/383
                    const int a = attached_delta(root, 1, inset_bottom ? 1 : -1);
                    if (a != INT_MIN) dy = a;
                }
            }
        }
        // docs/385: a list item in flight goes where its drawer goes.
        if (drawer_piece(root, nx0, nx1, ny0, ny1)) {
            dx = dr_dx;
            dy = dr_dy;
        }
        // A WIDE CENTRED ELEMENT STRETCHES INSTEAD OF MOVING.
        //
        // The two tests above have both declined horizontally -- the element
        // touches neither edge and its centre is in the middle third -- and
        // that is CORRECT for an icon: the note above records why the middle
        // must be left alone, and dragging a centred element to an edge is a
        // worse fault than leaving it. But it is wrong for a bar. Measured
        // from LB_HUDLOG's own SKIPPED list (docs/272):
        //
        //   w=23   native x   4..27    y 168..192   left button, anchors left
        //   w=199  native x  28..227   y 160..184   THE BAR, declined
        //   w=24   native x 227..251   y 168..192   right button, anchors right
        //
        // The bar's centre is 127, in the middle third, so it stays 199 px wide
        // while the two buttons it sits between move apart. Giving its left
        // edge the left anchor and its right edge the right anchor preserves
        // both margins exactly -- 24 px clear of each button, which is what it
        // has natively -- and the middle takes up the slack.
        //
        // SINGLE-QUAD ONLY, deliberately. An element is a union of quads and
        // each one is emitted with the element's own delta; stretching a group
        // by a shared rule would pull its pieces apart, which is the exact
        // fault the "Deciding the anchor per quad TEARS elements in half" note
        // above records. A multi-quad group declines as it does today, so this
        // cannot regress anything that already worked.
        int dx1 = dx;
        bool slice5 = false;
        int text_y0 = 0, text_y1 = 0;   // see HudBox::ty0
        if (moving && output_width > 256 && w >= kSpanMinWidth &&
            at_left == at_right && inset_left == inset_right) {
            int quads_here = 0;
            for (int i = 0; i < qn; ++i)
                if (find(i) == root) ++quads_here;
            if (quads_here == 1) {
                dx = dx_left;
                dx1 = dx_right;
            } else if (hud_stretch_multi() && !at_left && !at_right &&
                       ntex == 1 && w > 2 * kSpanCap + 1) {
                // docs/365: the five-slice stretch, when the column it
                // repeats is clear of the quads the scan did not group.
                const int fx = bx0 + kSpanCap;
                bool clear = true;
                for (int k = 0; k < offn && clear; ++k)
                    clear = !(offq[k].sx0 <= fx && fx < offq[k].sx1 &&
                              offq[k].sy0 < by1 && by0 < offq[k].sy1);
                if (clear) {
                    dx = dx_left;
                    dx1 = dx_right;
                    slice5 = true;
                    // The rows any ungrouped (text) quad over the box covers.
                    for (int k = 0; k < offn; ++k) {
                        if (offq[k].sx1 <= bx0 || offq[k].sx0 >= bx1 ||
                            offq[k].sy1 <= by0 || offq[k].sy0 >= by1)
                            continue;
                        if (text_y0 >= text_y1) {
                            text_y0 = offq[k].sy0;
                            text_y1 = offq[k].sy1;
                        } else {
                            if (offq[k].sy0 < text_y0) text_y0 = offq[k].sy0;
                            if (offq[k].sy1 > text_y1) text_y1 = offq[k].sy1;
                        }
                    }
                }
            }
        }
        if (el) {
            el->dx = dx;
            el->dy = dy;
            el->dx1 = dx1;
        }
        if (!dx && !dy && dx1 == dx) {
            // SAY WHAT WAS SKIPPED. An element this declines keeps its native
            // position, which in a widened view is inset from the edge -- it
            // "floats". Reporting only what moved made those invisible to the
            // log while being the most obvious thing on screen.
            if (g_reject_n < 32) {
                g_reject[g_reject_n].nx0 = nx0;
                g_reject[g_reject_n].nx1 = nx1;
                g_reject[g_reject_n].y0 = by0;
                g_reject[g_reject_n].y1 = by1;
                g_reject[g_reject_n].z = 0u;
                ++g_reject_n;
            }
            // THE ONE DECLINE THAT IS NOT ORDINARY -- the open/collapse defect.
            //
            // Open/collapse HUD animations for the brick (red/blue/yellow)
            // actions sometimes still displayed at original-aspect positions.
            //
            // Most declines here are CORRECT and constant: a centred icon
            // touches no edge, its centre is in the middle third, and the notes
            // above record why the middle must be left alone. Counting all of
            // them would say nothing, because they happen every frame.
            //
            // This is the rare one. A group touching BOTH edges spans the
            // screen, so `at_left != at_right` is false and both inset tests
            // are false, and the stretch branch below takes single-quad
            // elements only -- so a MULTI-QUAD spanning group falls through
            // with dx and dy zero and is drawn at its native position. The
            // grouping note above already records the list and the two sidebar
            // bricks merging into one 232 px element; while that list is
            // opening, the merged group's width sweeps through the range where
            // it reaches both edges. Transiently spanning is exactly what an
            // intermittent fault would look like.
            //
            // WHY IT NEEDS ITS OWN COUNTER. g_reject is rebuilt every frame and
            // the summary below prints every 120th, so a decline lasting a few
            // frames is overwritten before it is ever reported. A replay of a
            // recorded combat session shows 29 logged HUD frames, 19 of them
            // with the build list open, and elements-moved equal to hudquads in
            // every one -- which does NOT rule this out, because 124 samples
            // across ~15,000 frames cannot see a three-frame event.
            //
            // Cumulative and never reset, so one occurrence anywhere in a
            // session survives to the next summary line. Costs an int compare
            // on a path that already runs once per declined group.
            // AN EDGE-HUGGING ELEMENT THAT DECLINES IS THE OPEN/COLLAPSE SHAPE,
            // and the SKIPPED list above cannot be trusted to show it.
            //
            // That list is rebuilt every frame and printed on every 120th, so
            // it samples about one frame in a hundred and twenty. Surveying
            // every run on disk found 119 declines of elements touching an
            // edge -- and their rects are exactly the buttons in the defect:
            // native x 4..12 and 12..19 at y 16..40 and y 40..64, the brick
            // actions, plus x 227..251 y 168..192 sixty-nine times. None of
            // those appear in 1,533 sampled frames from the newer builds,
            // which is encouraging and is NOT evidence: the note above already
            // records that sampling cannot see a three-frame event.
            //
            // So count it cumulatively, the way g_span_declines is counted,
            // and for the same stated reason. One occurrence anywhere in a
            // session then survives to the next summary line. The cost is an
            // int compare on a path that already runs once per declined group.
            // THE DENOMINATOR, without which the zero above means nothing.
            //
            // `edgedecl=0` across 36 runs and 1,642 summary lines reads like a
            // strong result and is unreadable on its own: it is equally
            // consistent with "declines happen constantly and none touch an
            // edge" and with "this block barely runs". A run's SKIPPED list
            // shows three entries, but that list is printed every 120th frame
            // while this path runs on every one, so the list cannot supply the
            // denominator either. Count the declines themselves.
            ++g_declines;
            if (nx0 <= 24 || nx1 >= 232) {
                ++g_edge_declines;
                if (g_edge_declines <= 8) {
                    std::fprintf(stderr,
                                 "[lb_hud] EDGE-DECLINE #%u: element native "
                                 "x %d..%d y %d..%d hugs an edge yet was left "
                                 "at its native position%s\n",
                                 g_edge_declines, nx0, nx1, ny0, ny1,
                                 g_edge_declines == 8u
                                     ? "  (further ones counted only)" : "");
                    std::fflush(stderr);
                }
            }
            if (at_left && at_right) {
                int quads_here = 0;
                for (int i = 0; i < qn; ++i)
                    if (find(i) == root) ++quads_here;
                if (quads_here > 1) {
                    ++g_span_declines;
                    if (g_span_declines <= 8) {
                        std::fprintf(stderr,
                                     "[lb_hud] SPAN-DECLINE #%u: %d-quad "
                                     "element native x %d..%d y %d..%d touches "
                                     "both edges -- left at its native "
                                     "position%s\n",
                                     g_span_declines, quads_here, nx0, nx1,
                                     ny0, ny1,
                                     g_span_declines == 8u
                                         ? "  (further ones counted only)"
                                         : "");
                        std::fflush(stderr);
                    }
                }
            }
            continue;
        }
        // Every quad of the element gets the element's OWN dx and dy, which is
        // the whole point: they move together or the element comes apart.
        for (int i = 0; i < qn; ++i) {
            if (find(i) != root) continue;
            // COUNTED, where it used to stop silently: g_box_capped was
            // declared and printed but never incremented.
            if (g_count >= kMaxHudQuads) { ++g_box_capped; break; }
            // A five-slice element is ONE box, its bounding rect, emitted
            // once: the slices are the element's, not any one tile's.
            if (slice5 && i != root) continue;
            struct Span { int min_x, max_x, min_y, max_y; };
            const Span p = slice5 ? Span{bx0, bx1, by0, by1}
                                  : Span{q[i].sx0, q[i].sx1, q[i].sy0,
                                         q[i].sy1};
        g_boxes[g_count].slice5 = slice5 ? 1 : 0;
        g_boxes[g_count].ty0 = slice5 ? text_y0 : 0;
        g_boxes[g_count].ty1 = slice5 ? text_y1 : 0;
        g_boxes[g_count].sx0 = (int)p.min_x;
        g_boxes[g_count].sx1 = (int)p.max_x;
        g_boxes[g_count].y0 = (int)p.min_y;
        g_boxes[g_count].y1 = (int)p.max_y;
        g_boxes[g_count].dx = dx;
        g_boxes[g_count].dy = dy;
        g_boxes[g_count].dx1 = dx1;
        // Allocate this frame's capture for the box.
        g_cache_cur[g_count].w = (int)p.max_x - (int)p.min_x;
        g_cache_cur[g_count].h = (int)p.max_y - (int)p.min_y;
        g_cache_cur[g_count].x0 = (int)p.min_x;
        g_cache_cur[g_count].y0 = (int)p.min_y;
        g_cache_cur[g_count].px.assign(
            (size_t)g_cache_cur[g_count].w * (size_t)g_cache_cur[g_count].h, 0u);
        // Last frame's capture OF THIS RECT, by the key in the HudCache note.
        // The same slot first: an unchanged HUD keeps its order.
        {
            int prev = -1;
            const HudCache& c = g_cache_cur[g_count];
            auto same = [&c](const HudCache& o) {
                return o.w == c.w && o.h == c.h && o.x0 == c.x0 && o.y0 == c.y0;
            };
            if (g_count < g_prev_count && same(g_cache_prev[g_count])) {
                prev = g_count;
            } else {
                for (int j = 0; j < g_prev_count; ++j)
                    if (same(g_cache_prev[j])) { prev = j; break; }
            }
            g_boxes[g_count].prev = prev;
        }
        ++g_count;
        }
    }

    // docs/348: the native cells whose fog hole now has nothing over it.
    for (int i = 0; i < g_count; ++i) {
        const HudBox& b = g_boxes[i];
        const bool moved = b.dx != 0 || b.dy != 0 || b.dx1 != b.dx;
        mark_ui_cells(moved ? g_refog : g_keephole, b.sx0 - extra,
                      b.y0 - extra_y, b.sx1 - b.sx0, b.y1 - b.y0);
    }
    for (int cy = 0; cy < 24; ++cy) g_refog[cy] &= ~g_keephole[cy];

    // LB_HUDTRACE=N (docs/385): every composite whose ELEMENTS differ from the
    // previous one's, one line each -- native box, move, quads -- plus what
    // was rejected, until N lines. LB_HUDLOG prints one frame in 120, which
    // cannot see a drawer's open/close animation (which did not always occur
    // at the adaptive position).
    {
        static const int trace_max = [] {
            const char* v = std::getenv("LB_HUDTRACE");
            return v ? std::atoi(v) : 0;
        }();
        static int traced = 0;
        static std::string last;
        if (trace_max > 0 && traced < trace_max) {
            std::string sig;
            char b[96];
            for (int i = 0; i < g_elem_n; ++i) {
                const HudElemLog& e = g_elem_log[i];
                std::snprintf(b, sizeof b, " [%d..%d x %d..%d q%d %+d,%+d]",
                              e.nx0, e.nx1, e.ny0, e.ny1, e.quads, e.dx, e.dy);
                sig += b;
            }
            for (int i = 0; i < g_reject_n; ++i) {
                std::snprintf(b, sizeof b, " REJ[%d..%d x %d..%d]",
                              g_reject[i].nx0, g_reject[i].nx1, g_reject[i].y0,
                              g_reject[i].y1);
                sig += b;
            }
            if (sig != last) {
                last = sig;
                ++traced;
                std::fprintf(stderr, "[lb_hudtrace] composite %u band %d,%d:%s\n",
                             g_compose_frame, extra, extra_y, sig.c_str());
            }
        }
    }
    static int said = -1;
    if (said < 0) said = std::getenv("LB_HUDLOG") ? 1 : 0;
    if (said == 1) {
        static unsigned t = 0;
        if ((++t % 120u) == 0u) {
            std::fprintf(stderr,
                         "[lb_hud] %d corner element(s) moved, "
                         "surface=%dx%d visible=%dx%d shift=%d,%d "
                         "band=%d,%d hudquads=%d  rejected: z=%d (lowest "
                         "%u, threshold %u) offscreen=%d  of %u polys "
                         "spandecl=%u edgedecl=%u/%u  [cumulative z=%u off=%u zminever=%u "
                         "polymax=%u qnmax=%u qncap=%u boxcap=%u]\n",
                         g_count, output_width, output_height, vis, visr,
                         shift, shift_y, extra, extra_y, g_hud_seen,
                         g_rej_z,
                         g_rej_z_min == 0xFFFFFFFFu ? 0u : g_rej_z_min,
                         unsigned(zmax), g_rej_offscreen,
                         nds_gpu3d_render_polygon_count(),
                         /* running total, so a transient that happened
                            between two of these lines is still visible */
                         g_span_declines, g_edge_declines, g_declines,
                         g_rej_z_total, g_rej_off_total,
                         g_rej_z_min_ever == 0xFFFFFFFFu
                             ? 0u : g_rej_z_min_ever,
                         g_poly_max_ever, g_qn_max_ever,
                         g_qn_capped, g_box_capped);
            for (int i = 0; i < g_count; ++i)
                std::fprintf(stderr,
                             "[lb_hud]   x %d..%d y %d..%d -> %+d,%+d\n",
                             g_boxes[i].sx0, g_boxes[i].sx1,
                             g_boxes[i].y0, g_boxes[i].y1,
                             g_boxes[i].dx, g_boxes[i].dy);
            // docs/365: one line per ELEMENT, with the textures merged into
            // it, moved or not.
            for (int i = 0; i < g_elem_n; ++i) {
                const HudElemLog& e = g_elem_log[i];
                char tex[40] = "";
                for (int t = 0, o = 0; t < e.ntex && o < 30; ++t)
                    o += std::snprintf(tex + o, sizeof tex - (size_t)o,
                                       t ? ",%08X" : "%08X", e.tex[t]);
                std::fprintf(stderr,
                             "[lb_hud]   ELEMENT native x %d..%d y %d..%d "
                             "quads=%d tex=%s -> %+d,%+d (right edge %+d)%s\n",
                             e.nx0, e.nx1, e.ny0, e.ny1, e.quads, tex,
                             e.dx, e.dy, e.dx1,
                             (!e.dx && !e.dy && e.dx1 == e.dx) ? "  declined"
                                                               : "");
            }
            for (int i = 0; i < g_off_n; ++i)
                std::fprintf(stderr,
                             "[lb_hud]   OFFSCREEN surface x %d..%d y %d..%d "
                             "-> native x %d..%d y %d..%d  (%s%s%s%s)\n",
                             g_off[i].sx0, g_off[i].sx1,
                             g_off[i].sy0, g_off[i].sy1,
                             g_off[i].nx0, g_off[i].nx1,
                             g_off[i].ny0, g_off[i].ny1,
                             g_off[i].nx0 < 0 ? "x<0 " : "",
                             g_off[i].nx1 > 256 ? "x>256 " : "",
                             g_off[i].ny0 < 0 ? "y<0 " : "",
                             g_off[i].ny1 > 192 ? "y>192" : "");
            for (int i = 0; i < g_reject_n; ++i)
                std::fprintf(stderr,
                             "[lb_hud]   SKIPPED native x %d..%d (w=%d) "
                             "y %d..%d z=%u\n",
                             g_reject[i].nx0, g_reject[i].nx1,
                             g_reject[i].nx1 - g_reject[i].nx0,
                             g_reject[i].y0, g_reject[i].y1, g_reject[i].z);
            std::fflush(stderr);
        }
    }
}

// Return this scanline with its corner HUD spans moved to the new edges.
//
// Returns the input pointer untouched when there is nothing to do, so a session
// with the setting off pays a single comparison per line and copies nothing.
// DOES THIS ROW CONTAIN A HUD BOX?
//
// For the re-present correction in docs/337. That shift slides the 3D row's
// sample index so world geometry tracks the ground on frames the guest did not
// re-submit -- and the in-field HUD is 3D geometry in the SAME raster, which is
// what made the earlier LB_LAGFIX attempt wrong. Rather than teach the HUD
// relocation below to compensate its source reads, the correction is simply
// not applied to rows the HUD occupies: those rows keep their exact present
// behaviour, and the HUD sits at the screen edges where there is little world
// geometry to correct anyway.
//
// Same span test as lb_hud_shift_line's, for the same reason it uses the union
// of source and destination: a scaled or vertically anchored element covers
// rows the game never drew it on.
extern "C" int lb_hud_row_has_box(int y) {
    if (g_count <= 0) return 0;
    const float scale = lb_setting_hud_scale();
    for (int i = 0; i < g_count; ++i) {
        const int bh = g_boxes[i].y1 - g_boxes[i].y0;
        const int dh = (int)(bh * (scale > 1.0f ? scale : 1.0f));
        const int dy1 = g_boxes[i].y1 + g_boxes[i].dy;
        int lo = g_boxes[i].y0, hi = g_boxes[i].y1;
        if (dy1 - dh < lo) lo = dy1 - dh;
        if (dy1 > hi) hi = dy1;
        if (y >= lo && y < hi) return 1;
    }
    return 0;
}

extern "C" const uint32_t* lb_hud_shift_line(const uint32_t* line, int y,
                                             int width) {
    // THE MASK IS THIS CALL'S OR NONE, set before either early return.
    //
    // It used to be reset only on the path that goes on to draw, so a row with
    // no box left the thread's mask exactly as the last covered row had set
    // it. That is harmless only while the thread never sees that row again,
    // and the fixed bands guarantee it does: the same thread takes the same
    // rows every frame, and the calling thread always takes band 0. (Since
    // docs/355 rows are handed out in 8-row chunks, so they move between
    // threads, and a stale mask would land on whatever row a thread happened
    // to draw next. The reset matters just as much.) So a row
    // kept its mask, and the shroud stayed cut out of it, for as long as no
    // box came back.
    //
    // Measured on the reference replay at 1280x720 (surface 640x360, ten
    // workers, 36-row bands): surface row 35, x 0..15, showed terrain through
    // unexplored cloud in frames 1285, 2570, 3855 and 6425 (LB_LAYERDUMP: BG1
    // wins there, BG2 above and below it). Row 35 is the last row of band 0.
    // x 0..15 is the width of the corner element moved to that edge, and
    // 7710, where the element is on screen, draws it there. What showed
    // through was real ground under the shroud, so it leaked the map as well
    // as marking the picture.
    g_cover_y = y;
    g_cover_any = false;
    if (!line) return line;

    // docs/375: THE TWO ROWS BEHIND THIS ONE, when the renderer drew the HUD
    // on its own. split 1: s_hud holds only HUD pixels and s_world the row
    // without them, both valid in [s_x0, s_x1). split 2: the frame was split
    // and no HUD polygon reached this row, so nothing here is the HUD's.
    // split 3 (docs/388): the renderer looked and the frame had no HUD
    // polygon at all. split 0: a row the renderer does not own (a copy), or a
    // frame it did not look at (the compute renderer, LB_HUDSPLIT=ab's other
    // half) -- the old path, vacating to nothing.
    const uint32_t* s_world = nullptr;
    const uint32_t* s_hud = nullptr;
    int s_x0 = 0, s_x1 = 0;
    const int split = hud_split_env()
        ? nds_gpu3d_split_rows(line, &s_world, &s_hud, &s_x0, &s_x1) : 0;
    bool cover_ready = false;
    const auto cover_begin = [&]() {
        if (cover_ready) return;
        cover_ready = true;
        if ((int)g_cover.size() != width) g_cover.assign((size_t)width, 0u);
        else std::fill(g_cover.begin(), g_cover.end(), uint8_t(0));
    };

    // docs/380: A HUD PIXEL THE CARTRIDGE DREW OUTSIDE ITS OWN 256x192 IS NOT
    // PUNCHED THROUGH THE LAYERS IN FRONT OF IT.
    //
    // The shroud (BG2, priority 1) and the placement footprint (BG3, priority
    // 0) are both in front of the 3D layer the HUD lives on. On the DS the
    // cartridge clears their cells under every UI element (SpriteUI_MarkCells,
    // docs/348), on a grid clamped to 32x24 cells: its own screen. Measured on
    // a held placement: in the native columns the footprint's cells under the
    // checkmark are cleared and the checkmark shows whole; with the site
    // panned into the band -- docs/372 keeps the checkmark beside it there --
    // the footprint's studs draw over the checkmark's lower half. Nothing in
    // the cartridge can clear a cell outside its grid, so the port covers
    // those HUD pixels the way it already covers the ones it moves.
    if (split == 1 && hud_split_env()) {
        const bool row_out = y < g_raster_band_y || y >= g_raster_band_y + 192;
        for (int x = s_x0; x < s_x1 && x < width; ++x) {
            if (x < 0 || !((s_hud[x] >> 24) & 0x1Fu)) continue;
            if (!row_out && x >= g_raster_band && x < g_raster_band + 256)
                continue;                  // the cartridge's own cells
            cover_begin();
            g_cover[(size_t)x] = 1u;
            g_cover_any = true;
        }
    }

    if (g_count <= 0) return line;
    const float early_scale = lb_setting_hud_scale();
    int hit = 0;
    for (int i = 0; i < g_count; ++i) {
        // The ENLARGED box AND ITS DESTINATION, not just the source. A scaled
        // element is anchored at its bottom edge and grows upward, so its top
        // rows are above the rows the game drew -- testing only the source
        // range skipped them and sheared the top off every scaled button. A
        // vertically anchored element moves off its source rows entirely, so
        // the span this line has to care about is the union of the two.
        const int bh = g_boxes[i].y1 - g_boxes[i].y0;
        const int dh = (int)(bh * (early_scale > 1.0f ? early_scale : 1.0f));
        const int dy1 = g_boxes[i].y1 + g_boxes[i].dy;
        int lo = g_boxes[i].y0, hi = g_boxes[i].y1;
        if (dy1 - dh < lo) lo = dy1 - dh;
        if (dy1 > hi) hi = dy1;
        if (y >= lo && y < hi) { hit = 1; break; }
    }
    if (!hit) return line;

    if ((int)g_scratch.size() != width) g_scratch.resize((size_t)width);
    std::memcpy(g_scratch.data(), line, (size_t)width * sizeof(uint32_t));
    cover_begin();

    const float scale = early_scale;
    const bool scaling = scale > 1.01f;

    // The HUD proper only: a pixel flagged as UI layer 0 (bit 31, docs/380) is
    // a command icon or the checkmark, which never move.
    const auto hud_px = [&](int x) -> uint32_t {
        if (split != 1 || x < s_x0 || x >= s_x1) return 0u;
        const uint32_t v = s_hud[x];
        return (v & 0x80000000u) ? 0u : v;
    };
    {   // Say once which of the three a run is getting (row workers race).
        static std::atomic<unsigned> seen{0u};
        const unsigned bit = 1u << (split & 3);
        if (!(seen.fetch_or(bit) & bit))
            std::fprintf(stderr,
                         "[lb_hudsplit] first moved-HUD row with split %d "
                         "(%s): row %d, HUD columns %d..%d\n", split,
                         split == 1 ? "world and HUD rows from the renderer"
                         : split == 2 ? "split frame, no HUD on this row"
                         : split == 3 ? "no HUD polygon in the frame: carried"
                                      : "not split: vacate to nothing",
                         y, s_x0, s_x1);
    }

    // EVERY SOURCE IS VACATED BEFORE ANY DESTINATION IS WRITTEN (docs/365).
    //
    // This was one loop, and each box vacated its own source right after
    // writing its destination. That is only safe while no box lands where a
    // later box still has to be vacated. The note it rested on said so:
    // "source and destination cannot overlap: the shift is the extension
    // width and the box is capped at 64 px". An element of many quads that
    // moves less than its own width breaks it. The tutorial box is 200 px of
    // 8x8 tiles moving 104 px left. Every tile processed after the one whose
    // destination it sat on wiped that destination, and holes opened through
    // the box ("Welcome to [ground]O Battles."). A box moving right by less
    // than its width does the same to itself. Capture and vacate everything,
    // then draw everything: the result no longer depends on box order.
    // A ROW WITH NO SPLIT IS NOT VACATED (the defect: little patches of
    // terrain overlapping things).
    //
    // Split 0 is a row whose raster the renderer did not split: a frame drawn
    // with no HUD polygon at all -- the frames after a camera step submit none
    // (docs/349 s9c) while lb_hud's boxes are still the last scan's -- or a
    // row held as a copy. The old path then vacated EVERY opaque pixel in
    // each box's source to nothing, and captured them as HUD: a unit standing
    // where a moved button's native rect is (Deselect, 24x24) lost that square
    // to the terrain under it, and the next frame drew the unit's pixels at
    // the button's corner. A real play session's log has "split 0 (not split:
    // vacate to nothing)" in a big fight; no harness run had one.
    //
    // Such a row has no HUD pixels the port can tell apart, so it is left as
    // the renderer drew it: nothing vacated, the previous capture carried
    // forward, and the moved HUD drawn from that capture (below). With
    // LB_HUDSPLIT=0 every row is split 0 by design and keeps the old path.
    // LB_HUDSPLIT0=1 (a test aid) treats every row as split 0 for half of
    // every second -- which no harness run meets on its own -- so the other
    // half keeps a trusted capture to carry forward. It checks the carry
    // (run t385p2: the moved Deselect stayed at its corner through the
    // forced blocks). It cannot reproduce the terrain patches: a forced row
    // still holds the HUD the renderer drew, which is left at the native
    // spot for those frames, where a real split-0 row has none to leave.
    //
    // docs/388: ONLY A FRAME WITH NO HUD POLYGON IS CARRIED -- split 3, which
    // the renderer now reports apart from split 0. The carry was taken by
    // every unsplit row, and the renderer did not split an edge-marked frame.
    // Building_Draw3D (0x0203B3AC), which draws Transport and Siege units,
    // turns edge marking on and anti-aliasing off, and only G3X_Init at boot
    // turns them back. So from the first ship, catapult or dragon on screen
    // to the end of the session, every row was split 0: nothing was vacated,
    // the carry lasted only until a box changed, and the HUD sat at its
    // native spot while the clicks went to the moved one. The renderer splits edge-marked frames now, and a split-0
    // row left (a copy, the compute renderer, LB_HUDSPLIT=0 or =ab) takes the
    // old path: the HUD moves with its own pixels, as it did before docs/385.
    static const bool force0 = std::getenv("LB_HUDSPLIT0") != nullptr;
    int split_used = split;
    if (force0) {
        static const auto t0 = std::chrono::steady_clock::now();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if ((ms / 500) & 1) split_used = 3;
    }
    const bool untrusted = split_used == 3 && hud_split_env();
    for (int i = 0; i < g_count; ++i) {
        const HudBox& b = g_boxes[i];
        if (y < b.y0 || y >= b.y1) continue;
        const int bw = b.sx1 - b.sx0;
        // Capture this row of the box for the NEXT frame's redraw, and vacate
        // the pixels the game drew at the old screen's corner.
        HudCache& c = g_cache_cur[i];
        const int row = y - b.y0;
        if (untrusted) {
            if (b.prev >= 0 && c.w == bw && row < c.h) {
                const HudCache& p = g_cache_prev[b.prev];
                if (p.w == c.w && p.h == c.h && !p.px.empty())
                    std::memcpy(c.px.data() + (size_t)row * (size_t)c.w,
                                p.px.data() + (size_t)row * (size_t)p.w,
                                (size_t)c.w * sizeof(uint32_t));
            }
            continue;
        }
        for (int x = b.sx0; x < b.sx1 && x < width; ++x) {
            if (x < 0) continue;
            const uint32_t v = split ? hud_px(x) : line[x];
            if (c.w == bw && row < c.h)
                c.px[(size_t)row * (size_t)c.w + (size_t)(x - b.sx0)] = v;
            if (!((v >> 24) & 0x1Fu)) continue;               // transparent
            // docs/375: split, what was under the HUD pixel is the world's
            // own; otherwise there is no telling, and it goes to nothing.
            g_scratch[(size_t)x] = split ? s_world[x] : 0u;
        }
    }

    for (int i = 0; i < g_count; ++i) {
        const HudBox& b = g_boxes[i];
        const int bw = b.sx1 - b.sx0, bh = b.y1 - b.y0;
        // WHEN THE ONE-PASS MOVE IS ENOUGH, AND WHEN IT IS NOT.
        //
        // A purely horizontal move can be done inside the line the pixels
        // arrive on: source and destination are the same row. A vertical move
        // cannot -- the destination row is not this one -- so it goes through
        // the same previous-frame capture that scaling already uses, for the
        // same reason and with the same one frame of latency, which is
        // invisible on a screen-locked HUD.
        // A STRETCH GOES THROUGH THE CACHE TOO. The one-pass move below writes
        // one destination per source pixel, which cannot widen anything; a
        // stretched box needs a destination-driven loop, and the cache path
        // already is one.
        const bool stretching = (b.dx1 != b.dx);
        // A row with no split draws the moved HUD from the capture carried
        // forward above, never from this row's own pixels (see `untrusted`).
        const bool via_cache = scaling || b.dy != 0 || stretching || untrusted;

        if (!via_cache) {
            // The same row, straight from the untouched input line.
            if (y < b.y0 || y >= b.y1) continue;
            for (int x = b.sx0; x < b.sx1 && x < width; ++x) {
                if (x < 0) continue;
                const uint32_t v = split ? hud_px(x) : line[x];
                if (!((v >> 24) & 0x1Fu)) continue;   // transparent
                const int d = x + b.dx;
                if (d >= 0 && d < width) {
                    g_scratch[(size_t)d] =
                        split ? hud_blend(v, g_scratch[(size_t)d]) : v;
                    g_cover[(size_t)d] = 1u;
                    g_cover_any = true;
                }
            }
            continue;
        }

        // Draw the copy from last frame's capture, anchored so the corner it
        // belongs in stays put: the moved edges are fixed and the box grows
        // inward from there.
        if (b.prev < 0) continue;
        const HudCache& src = g_cache_prev[b.prev];
        if (src.w != bw || src.h != bh || src.px.empty()) continue;
        const int dw = stretching ? (bw + b.dx1 - b.dx)
                                  : (scaling ? (int)(bw * scale) : bw);
        const int dh = scaling ? (int)(bh * scale) : bh;
        if (dw <= 0 || dh <= 0) continue;
        const int dst_r = b.sx1 + b.dx;
        // A STRETCH IS PINNED BY ITS LEFT EDGE, A MOVE BY THE EDGE IT IS GOING
        // TO. The right-anchored form exists so a box growing under hud_scale
        // keeps the corner it belongs in; applied to a stretch it would drag
        // the left edge instead of extending the middle. With dx0 at sx0+dx the
        // right edge lands at sx0+dx+dw = sx1+dx1, which is the pair of anchors
        // the classifier chose.
        const int dx0 = stretching ? (b.sx0 + b.dx)
                                   : ((b.dx > 0) ? (dst_r - dw)
                                                 : (b.sx0 + b.dx));
        const int dy1 = b.y1 + b.dy, dy0 = dy1 - dh;
        if (y < dy0 || y >= dy1) continue;
        const int srow = ((y - dy0) * bh) / dh;
        // THE FILL OF A FIVE-SLICE ROW: ITS OWN COLOUR WHERE IT HAS ONE.
        //
        // The first version repeated the column just inside the left cap. That
        // column is still inside the corner's highlight: in the top-border
        // rows it is lighter than the border's middle, so both gaps showed a
        // pale strip that stopped at the content.
        //
        // The second took every row's majority colour over the content, and
        // that drew white lines out of the text: in a row through the
        // letters' horizontal strokes, white is the majority.
        //
        // The third required 90% uniformity and still left a pale strip: the
        // top border carries highlight pixels, so it fell back to the column
        // inside the corner's highlight.
        //
        // So the rows are told apart by what can be in them, not by how they
        // look. Rows the box's text quad covers (HudBox::ty0..ty1, from the
        // quads the scan did not group) keep the cap-side column: the box's
        // interior there, clear of the text by the test in lb_hud_scan. Every
        // other row -- the borders and the margins -- has no glyph to win a
        // vote, so it takes its majority colour over the content. The
        // Boyer-Moore vote runs once per row per box.
        //
        // One exception: a row inside the text quad's span that is still at
        // least 90% one colour also takes its majority. The quad runs to the
        // box's bottom edge, so the bottom border's rows are inside its span.
        // They hold no glyph, and the cap-side column there is the corner's
        // lighter shading, which left a faint line along both gaps.
        uint32_t fillv = 0u;
        bool have_fill = false;
        const int src_y = b.y0 + srow;
        const bool text_row = src_y >= b.ty0 && src_y < b.ty1;
        if (b.slice5 && bw > 2 * kSpanCap) {
            const uint32_t* r = src.px.data() + (size_t)srow * (size_t)bw;
            uint32_t cand = 0u;
            int count = 0;
            for (int k = kSpanCap; k < bw - kSpanCap; ++k) {
                if (count == 0) { cand = r[k]; count = 1; }
                else if (r[k] == cand) ++count;
                else --count;
            }
            int n = 0;
            for (int k = kSpanCap; k < bw - kSpanCap; ++k) n += r[k] == cand;
            const int span = bw - 2 * kSpanCap;
            if (text_row ? (10 * n >= 9 * span) : (2 * n > span)) {
                fillv = cand;
                have_fill = true;
            }
        }
        for (int i2 = 0; i2 < dw; ++i2) {
            const int x = dx0 + i2;
            if (x < 0 || x >= width) continue;
            bool in_fill = false;
            // 3-SLICE FOR A STRETCH, LINEAR FOR A SCALE.
            //
            // The linear map below is right for scaling, where the whole
            // element grows. It is wrong for a stretch: it would widen the
            // 1 px caps in proportion, turning a border into a band. Holding
            // kSpanCap columns at each end and resampling only what is between
            // them keeps the border native and takes the slack out of the
            // middle -- which on a flat interior is exact.
            int scol;
            // FIVE SLICES (docs/365, hud_stretch_multi): caps at the anchors,
            // the content 1:1 where the game drew it, and the gaps between
            // repeating the column just inside the left cap. `lx` is how far
            // the left edge moved out, so the content starts at kSpanCap + lx.
            const int lx = -b.dx;
            const int c_end = kSpanCap + lx + (bw - 2 * kSpanCap);
            if (b.slice5 && stretching && lx >= 0 && bw > 2 * kSpanCap &&
                c_end <= dw - kSpanCap) {
                if (i2 < kSpanCap) scol = i2;
                else if (i2 < kSpanCap + lx) { scol = kSpanCap; in_fill = true; }
                else if (i2 < c_end) scol = i2 - lx;
                else if (i2 < dw - kSpanCap) { scol = kSpanCap; in_fill = true; }
                else scol = bw - (dw - i2);
                if (scol < 0) scol = 0;
                if (scol >= bw) scol = bw - 1;
            } else if (stretching && dw > 2 * kSpanCap && bw > 2 * kSpanCap) {
                if (i2 < kSpanCap) scol = i2;
                else if (i2 >= dw - kSpanCap) scol = bw - (dw - i2);
                else scol = kSpanCap + ((i2 - kSpanCap) * (bw - 2 * kSpanCap)) /
                                       (dw - 2 * kSpanCap);
                if (scol < 0) scol = 0;
                if (scol >= bw) scol = bw - 1;
            } else {
                scol = (i2 * bw) / dw;
            }
            const uint32_t v = (in_fill && have_fill)
                ? fillv
                : src.px[(size_t)srow * (size_t)bw + (size_t)scol];
            if (!((v >> 24) & 0x1Fu)) continue;
            g_scratch[(size_t)x] = split ? hud_blend(v, g_scratch[(size_t)x]) : v;
            g_cover[(size_t)x] = 1u;
            g_cover_any = true;
        }
    }
    return g_scratch.data();
}

// The mask for the line lb_hud_shift_line was last called with, or nullptr when
// nothing this port moved drew on it. See the note on g_cover: the compositor
// uses it to keep the shroud, which the field draws in FRONT of the 3D layer,
// off an element this port has relocated into a widened band.
extern "C" const uint8_t* lb_hud_cover_row(int y, int width) {
    if (!g_cover_any || g_cover_y != y || (int)g_cover.size() != width)
        return nullptr;
    return g_cover.data();
}
