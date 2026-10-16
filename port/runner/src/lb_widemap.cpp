// lb_widemap.cpp - supply tile entries for the widened columns.
//
// Part of legobattles-decomp. See port/runner/README.md.
//
// WHY THIS EXISTS
//
// The port widens the field to 448 columns, and the black either side of it is
// missing TERRAIN, not missing units: a capture at 1280x720 shows the tilemap
// stopping dead at the native edge while the player's base and units are drawn
// well past it, standing on black (docs/197). 3D geometry already reaches into
// the widened columns; the 2D background does not, because the guest maintains
// tile entries only for the 32 columns it draws.
//
// WHERE THE ENTRIES LIVE, AND WHY NOT IN VRAM
//
// In a plain host array that only the decoder reads. Two VRAM designs were
// tried first and both failed for reasons worth keeping:
//
//   * Writing the layer's SECOND screen block in place destroys another
//     background. The screen bases are packed end to end -- BG2 at 0x0000, BG1
//     at 0x0800, BG3 at 0x1000 -- so a 512-wide layer's second block is the
//     next layer's visible tilemap (docs/199).
//
//   * Relocating BG1's screen base looked elegant, because func_0210BC2C
//     computes the guest's own DMA destination from BG1CNT, so the guest's
//     writes follow a relocation. But the destination is recomputed per call,
//     so it sees whichever value the register holds at that moment, and the
//     write histogram shows the tilemap splitting across both bases:
//
//         with the relocation on:  0800=18928   the OLD block, still written
//                                  2800=35712   the NEW one, also written
//
//     Nothing that mutates a register the GUEST also reads can be made safe
//     here. This is the same race the earlier IO-register write lost, inverted.
//
// A host array needs no free VRAM -- which was its own dead end, since every
// addressable screen block lives inside bank E's 64 KB (VRAMCNT: banks A-D are
// all 3D texture) and the guest occupies the low ones -- and it cannot race the
// guest at all. The guest's registers and memory are left exactly as they were.
//
// THE RECIPE, read out of the cartridge and confirmed in a live battle
// (docs/196, docs/199, docs/200):
//
//     view      = *(u32*)0x02152034        func_02039A5C allocates and stores it
//     camx/camy = *(s16*)(view + 0x5E/0x60)
//     map       = *(u32*)0x02156CB0
//     map_w/h   = *(u8*)(map + 0x14/0x15)  metatile grid; measured 64x64
//     row       = *(u32*)(map + 0x34 + y*4)          func_020A322C
//     mt        = *(u16*)(row + x*2)
//     split     = *(u16*)(0x0214B9D0 + 2)
//     base      = mt < split ? *(u32*)(map+0x338) : *(u32*)(map+0x33C)
//     src       = base + 2 + (mt - (mt < split ? 0 : split)) * 12
//     six u16 tile entries: 3 across, 2 down         func_020A3478

//
// THE SHROUD
//
// Widening the terrain alone would show the player ground the game is hiding,
// which is a gameplay change and not a port. The fog had to be found first, and
// finding it took correcting a wrong answer this file used to rest on.
//
// docs/202 recorded "nothing is shrouded in these matches" on the strength of
// two all-zero probes of the mask at 0x02151F28. That was wrong: a capture of
// the very same frame is wall-to-wall grey cloud. The mask reads zero because
// it is a per-frame screen-space scratch mask (func_0203420C fills it from
// entity extents, clamped to 0x20 x 0x18) and not the record that drives the
// picture.
//
// TWO LAYERS ARE INVOLVED, AND ONLY ONE OF THEM DRAWS.
//
// Histogramming engine A's tilemaps in a fogged view found BG3 holding
// 001F x768 -- 768 = 32 x 24, exactly the visible field -- and func_020396E0's
// third pass agrees, writing 0x1F wherever the visibility test is set:
//
//     if (*func_0203D8E8(tx, ty) != 0x1f && func_020EEBF4(fog, tx, ty))
//         *func_0203D8E8(tx, ty) = 0x1f;
//
// That looked conclusive and was half right. Filling the widened columns of BG3
// with 0x1F changed nothing on screen. Masking DISPCNT bit 11 (BG3) changed
// nothing either, while masking bit 10 removed the fog completely and left
// clean terrain across the whole widened window. So:
//
//     BG3 tile 0x1F is a TRANSPARENT marker -- an overlay cleared under fog.
//     BG2 is the layer that actually draws the grey cloud.
//
// Both are used here, for different jobs: BG3's 0x1F says WHERE the fog is,
// BG2 says WHAT it looks like.
//
// WHERE THE WIDENED COLUMNS GET THEIR ANSWER
//
// Not from the mask at 0x02151F28 -- it is screen space, so it has no bit for a
// column the guest never draws. From a map-space cache accumulated out of the
// guest's own output: every frame the 32x24 window it does draw is read back,
// converted to absolute tile coordinates (every BGxOFS is zero -- measured
// again here -- so hardware column h is absolute column cam_tx + h), and
// recorded.
//
// Tiles the window has never covered default to SHROUDED. That is the honest
// default and it errs in the safe direction: the widened view can lag behind
// the game by showing fog over ground that was revealed while off-screen, and
// it can never do the opposite and reveal ground the game is hiding. Panning
// there corrects it from the guest's own output.

#include <cstdint>
#include <thread>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <vector>

extern "C" {
#include "runtime_arm.h"
}

#include "bus.h"
#include "state.h"
#include "io.h"
#include "vram.h"
#include "gpu3d.h"   // docs/348: the UI sprites that punch the fog

void nds_gpu2d_set_adaptive_bg_extend(bool enabled);
extern "C" int lb_setting_enhanced(void);   /* src/lb_overlay.cpp */
extern "C" int lb_widemap_extends(int bg);
/* src/lb_patches.cpp -- puts the camera back if a harvest shift outlived the
   guest's frame. The shift state lives with the override that applies it. */
extern "C" void lb_fp_cam_rescue(void);
/* src/lb_overlay.cpp: is the host-side terrain renderer supplying the
   field this frame? docs/325 phase 2. */
extern "C" int lb_hostterrain_active(void);
// The map chain captured on the render path. This file runs in the vblank hook,
// where reading 0x02156CB0 yields 0; see lb_field_map_chain in lb_overlay.cpp.
extern "C" int lb_field_map_chain(uint32_t* map, int* w, int* h);
extern "C" int lb_hostterrain_fogged(void);

namespace {

// Floor division and remainder. C truncates toward zero, so -1 / 3 is 0 and the
// tile immediately left of the map origin would be read out of the metatile at
// column 0 instead of column -1. Every widened column on the LEFT is negative,
// so this is not an edge case here -- it is half the feature.
int floor_div(int a, int b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }
int floor_mod(int a, int b) { return a - floor_div(a, b) * b; }

const uint32_t kViewPtr = 0x02152034u;
const uint32_t kMapPtr = 0x02156CB0u;
const uint32_t kTileSplit = 0x0214B9D0u;   // +2 holds the u16 threshold

bool main_ram(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

int16_t read_s16(uint32_t addr) {
    return static_cast<int16_t>(bus_read_u16(addr));
}

// THE PERIODIC RENDER PROBES, OFF BY DEFAULT.
//
// `[lb_seen]`, `[lb_seam]`, `[lb_bg2set]`, `[lb_anchor]`, `[lb_rows]` and the
// fog-model candidate dump all fired on `g_scan_frames % 120`, with no flag at
// all, so the shipped game wrote them to stderr every two seconds for as long
// as it ran. Each one also does work to produce its string -- the seen-cache
// walk, the 768-entry histogram -- on the render path.
//
// They are worth keeping: each was written to answer a question that came back
// more than once. They are not worth paying for in a build nobody is
// debugging, and they once cost a measurement of their own by pushing
// `[lb_fp]` out of the harness's tail window. LB_SCANLOG=1 restores them.
bool scan_logging() {
    static const bool on = [] {
        const char* v = std::getenv("LB_SCANLOG");
        return v && v[0] == '1';
    }();
    return on;
}

// One metatile's six tile entries: three across, two down. Mirrors
// func_020A3478.
//
// This deliberately leaves out func_02034B44's building stamp, and a note here
// used to warn that "terrain is the half that is missing" -- i.e. that a
// building standing on widened ground would show as bare terrain. MEASURED, and
// it does not: recomputing the guest's own 32 columns every report frame and
// diffing against BG1 gives 0/768 different, on every frame of a run including
// the ones with the player's base on screen. Nothing is stamped into BG1 that
// this does not reproduce exactly, so buildings are 3D geometry on BG0 -- which
// already reaches the widened columns (docs/197). There is no gap to close.
//
// RETRACTED 2026-09-24 (docs/367): "no gap to close" is wrong for WALLS and
// BRIDGES. Those are BG1 tiles, stamped per cell by func_02034B44 (walls) and
// func_0200D8A8 (finished bridges, blueprint category 17) inside
// func_020A3478, and this fill leaves both out. Gates are billboards, like
// the other buildings. The 0/768 was true of the runs it was measured on: none
// had a wall or bridge on screen, and 3D buildings are never stamped. Seen in
// the campaign tutorial: a wall drawn in the native rows vanished the moment
// a D-pad scroll moved it into the band. The castle, which is 3D, stays put.
// docs/378: metatile_tiles now runs a read-only replica of both stamps
// (namespace lbstamp below); expand_metatile stays the plain decode, because
// the shroud calls it with a fixed index.
struct Metatile { uint16_t t[6]; bool ok; };

// Expand one metatile INDEX to its six tile entries. Split out from the lookup
// below because the shroud needs it for a fixed index rather than for whatever
// the map grid holds at a cell.
Metatile expand_metatile(uint32_t split, uint32_t table_a, uint32_t table_b,
                         uint16_t mt) {
    Metatile out{};
    const uint32_t base = (mt < split) ? table_a : table_b;
    if (!main_ram(base)) return out;
    const uint32_t index = (mt < split) ? mt : (mt - split);
    const uint32_t src = base + 2u + index * 12u;
    for (int i = 0; i < 6; ++i)
        out.t[i] = bus_read_u16(src + uint32_t(i) * 2u);
    out.ok = true;
    return out;
}

// ---- WALLS AND BRIDGES, STAMPED INTO THE TERRAIN (docs/367, docs/378) ------
//
// Terrain_DecodeMetatile (020A3478) runs two passes over every metatile it
// decodes, after the plain decode above: the wall stamp FUN_02034B44 and the
// finished-bridge deck stamp FUN_0200D8A8. The guest runs them only for its
// own 32 columns, so a wall or bridge in the widened band came out as bare
// terrain (a tutorial wall vanished as it scrolled into the band).
//
// This is docs/367 section 7's replica: READS ONLY. It calls no guest code
// and writes no guest memory. The guest's own passes write the wall's
// connection cache (entity+0x224) and clear stale occupancy handles; the
// replica computes the same answers without either. Every pointer is
// validated as main RAM, and any failure means no stamp: some of these globals
// read 0 at the vblank hook (docs/367 8.6, docs/378). LB_STAMPS=0 turns it off.
namespace lbstamp {

bool on() {
    static const bool enabled = [] {
        const char* v = std::getenv("LB_STAMPS");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// FUN_02087350 without its write: a 2-byte handle (pool index, serial) to
// the live entity, or 0.
uint32_t resolve(uint16_t h) {
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (!main_ram(pool)) return 0u;
    const uint32_t i = h & 0xFFu, serial = h >> 8;
    if (static_cast<int32_t>(i) >=
        static_cast<int32_t>(bus_read_u32(pool + 0xCu)))
        return 0u;
    const uint32_t arr = bus_read_u32(pool + 8u);
    if (!main_ram(arr)) return 0u;
    const uint32_t e = bus_read_u32(arr + 4u * i);
    return (main_ram(e) && bus_read_u8(e + 0xDDu) == serial) ? e : 0u;
}

// FUN_020872DC: a unit-class entity (blueprint type tag 0). Units, gates,
// walls and bridges are all unit-class.
bool unit_class(uint32_t e) {
    const uint32_t bp = bus_read_u32(e + 0xE0u);
    if (!main_ram(bp)) return false;
    const uint32_t rec = bus_read_u32(bp + 4u);
    return main_ram(rec) && bus_read_u8(rec + 8u) == 0u;
}

// The blueprint category (0x13 Wall, 0x11 Bridge); -1 if unreadable.
int category(uint32_t e) {
    const uint32_t bp = bus_read_u32(e + 0x160u);
    if (!main_ram(bp)) return -1;
    const uint32_t rec = bus_read_u32(bp + 8u);
    return main_ram(rec) ? static_cast<int>(bus_read_u8(rec + 0x5Cu)) : -1;
}

// FUN_0207D79C / FUN_0207D8A4: the live entity of category `want` that is
// the sole occupant of cell (x, y) in occupancy layer 0, 1 or 2, else 0.
uint32_t occupant(int x, int y, int want, int map_w, int map_h) {
    if (x < 0 || y < 0 || x >= map_w || y >= map_h) return 0u;
    const uint32_t grid = bus_read_u32(0x02155224u);
    if (!main_ram(grid)) return 0u;
    if (static_cast<int16_t>(
            bus_read_u16(grid + 0x84u + 2u * uint32_t(y))) == 0)
        return 0u;
    const uint32_t idx =
        uint32_t(x) + bus_read_u16(0x02155230u + 2u * uint32_t(y));
    const uint32_t states = bus_read_u32(grid + 0x70u);
    if (!main_ram(states)) return 0u;
    const uint8_t kinds = bus_read_u8(0x02128010u + bus_read_u8(0x0214AD08u));
    if (!(bus_read_u8(states + idx) & kinds)) return 0u;
    for (uint32_t L = 0; L < 3u; ++L) {
        const uint32_t layer = grid + 0x24u * L;
        const uint8_t mask = bus_read_u8(0x02128018u + bus_read_u8(layer));
        const uint32_t obj = bus_read_u32(layer + 4u);
        if (!main_ram(obj)) continue;
        const uint32_t data = bus_read_u32(obj + 4u);
        if (!main_ram(data)) continue;
        if (((bus_read_u8(data + idx) & mask) >> bus_read_u8(layer + 8u)) != 1u)
            continue;
        const uint32_t handles = bus_read_u32(layer + 0x10u);
        if (!main_ram(handles)) continue;
        const uint32_t e = resolve(bus_read_u16(handles + 2u * idx));
        if (e && unit_class(e) && category(e) == want) return e;
    }
    return 0u;
}

// FUN_02034B44's gate: drawn only while the local player SEES the cell now
// (the visible grid, fog holder +0x14; not the explored one at +0x10), or
// with Reveal Map, rules +0x1F, or FUN_020A223C's eliminated-observer case.
bool gate_open(int x, int y) {
    const uint32_t holder = bus_read_u32(0x021569E4u);
    if (main_ram(holder)) {
        const uint32_t vis = bus_read_u32(holder + 0x14u);
        if (main_ram(vis)) {
            const uint32_t rows = bus_read_u32(vis + 0x14u);
            if (main_ram(rows)) {
                const uint32_t row = bus_read_u32(rows + 4u * uint32_t(y));
                if (main_ram(row) &&
                    ((bus_read_u8(row + (uint32_t(x) >> 3)) >> (x & 7)) & 1u))
                    return true;
            }
        }
    }
    const uint32_t rules = bus_read_u32(0x02150A70u);
    if (main_ram(rules) &&
        (bus_read_u8(rules) || bus_read_u8(rules + 0x1Fu)))
        return true;
    constexpr uint32_t kSession = 0x02156A24u;
    if (!bus_read_u8(kSession + 0x70u)) return false;
    const uint32_t mgr = bus_read_u32(0x0215540Cu);
    const uint32_t id = bus_read_u32(kSession + 0x64u);
    const uint8_t null_team = bus_read_u8(0x02155420u);
    if (!main_ram(mgr)) return true;   // no player list: the null team
    const int32_t count = static_cast<int32_t>(bus_read_u32(mgr + 0xCu));
    const uint32_t arr = bus_read_u32(mgr + 8u);
    for (int32_t i = 0; i < count && i < 16 && main_ram(arr); ++i) {
        const uint32_t p = bus_read_u32(arr + 4u * uint32_t(i));
        if (!main_ram(p)) continue;
        const uint32_t state = bus_read_u32(p + 0x9Cu);
        if (bus_read_u32(p + 0x10u) == id && (state == 0u || state == 4u))
            return bus_read_u8(p + 0x14u) == null_team;
    }
    return true;                       // no active local player: null team
}

// FUN_0205E000 without its store: the connection mask (bit 0 E, 1 N, 2 W,
// 3 S) the guest would cache. Bits 0..2 are sticky and only bit 3 is
// recomputed, exactly as the cartridge does; a band wall is never recomputed
// by the guest, so its dirty bit is still set and this gives the value the
// guest will store once the wall enters its own columns.
uint16_t wall_shape(uint32_t e, int map_w, int map_h) {
    const uint16_t v = bus_read_u16(e + 0x224u);
    if (!(v & 0x10u)) return v;
    const uint16_t old = static_cast<uint16_t>(v & ~0x10u);
    const int px = bus_read_u8(e + 0x128u), py = bus_read_u8(e + 0x129u);
    const uint8_t team = bus_read_u8(e + 0xE4u);
    uint16_t n = 0;
    for (uint32_t d = 0; d < 4u; ++d) {
        const int nx = (px + static_cast<int32_t>(
                                 bus_read_u32(0x02127FF0u + 8u * d))) & 0xFF;
        const int ny = (py + static_cast<int32_t>(
                                 bus_read_u32(0x02127FF0u + 8u * d + 4u))) & 0xFF;
        const uint32_t o = occupant(nx, ny, 0x13, map_w, map_h);
        if (o && bus_read_u8(o + 0xE4u) == team)
            n = static_cast<uint16_t>(n | (1u << d));
    }
    uint16_t r = static_cast<uint16_t>(n | (old & ~0x08u));
    if (!r)
        for (uint32_t d = 0; d < 4u; ++d)
            if (old & (1u << d)) {
                r = static_cast<uint16_t>(d == 3u ? 2u : (1u << d));
                break;
            }
    return r;
}

// What Task_View_DrawField would store at +0x71: in one of the two
// selection handle lists (+4 handles, +8 count).
bool highlighted(uint32_t e) {
    for (const uint32_t list : {0x0215800Cu, 0x02158024u}) {
        const int32_t count = static_cast<int32_t>(bus_read_u32(list + 8u));
        const uint32_t hs = bus_read_u32(list + 4u);
        if (!main_ram(hs)) continue;
        for (int32_t i = 0; i < count && i < 256; ++i) {
            const uint32_t h = resolve(bus_read_u16(hs + 2u * uint32_t(i)));
            if (h == e && unit_class(h)) return true;
        }
    }
    return false;
}

// The bridge footprint, FUN_02001170: the lazily built table at 0x0213AB2C,
// or the immediates it is built from if its guard (bit 0 of 0x02150894) is
// still clear.
void footprint(uint32_t rec, int* w, int* h) {
    static const uint8_t kFp[11][2] = {{1, 1}, {2, 2}, {3, 3}, {2, 3}, {2, 6},
                                       {2, 9}, {3, 2}, {6, 2}, {9, 2}, {1, 4},
                                       {4, 1}};
    const int idx = bus_read_u8(rec + 0x1Du);
    if (bus_read_u8(0x02150894u) & 1u) {
        const uint32_t fp = 0x0213AB2Cu + 8u * uint32_t(idx - 1);
        *w = bus_read_u8(fp + 4u);
        *h = bus_read_u8(fp + 5u);
    } else if (idx >= 1 && idx <= 11) {
        *w = kFp[idx - 1][0];
        *h = kFp[idx - 1][1];
    } else {
        *w = *h = 0;
    }
}

// Both passes, in the cartridge's order, over one metatile's six tiles.
void stamp(int x, int y, uint16_t t[6], uint32_t table_a, int map_w,
           int map_h) {
    // The wall pass, FUN_02034B44.
    const uint32_t w = occupant(x, y, 0x13, map_w, map_h);
    if (w && gate_open(x, y)) {
        const uint32_t ent = ((x & 1) ? 0x02127DE0u : 0x02127E40u) +
                             6u * wall_shape(w, map_w, map_h);
        uint16_t off = highlighted(w) ? 4u : 0u;
        const uint32_t bp = bus_read_u32(w + 0x160u);
        const uint32_t rec = main_ram(bp) ? bus_read_u32(bp + 8u) : 0u;
        const uint16_t max_hp = main_ram(rec) ? bus_read_u16(rec + 0x62u) : 0u;
        const int16_t pct =
            max_hp ? static_cast<int16_t>(
                         static_cast<int32_t>(bus_read_u16(w + 0x1A0u)) * 100 /
                         static_cast<int32_t>(max_hp))
                   : 100;
        off = static_cast<uint16_t>(off + (pct < 33 ? 0x20u
                                           : pct < 66 ? 0x18u : 0u));
        const uint32_t team = bus_read_u8(w + 0xE4u);
        for (uint32_t i = 0; i < 6u; ++i) {
            const uint8_t b = bus_read_u8(ent + i);
            if (b == 0xFFu) continue;             // keep the terrain tile
            const uint16_t tb = bus_read_u16(0x02127DB4u + 2u * ((b & 1u) * team));
            t[i] = static_cast<uint16_t>(off + static_cast<uint16_t>(b + tb));
        }
    }
    // The bridge-deck pass, FUN_0200D8A8: a finished bridge (+0x231), no
    // visibility gate, always table A. It runs second, so it wins.
    const uint32_t br = occupant(x, y, 0x11, map_w, map_h);
    if (br && bus_read_u8(br + 0x231u) && main_ram(table_a)) {
        const uint32_t bp = bus_read_u32(br + 0xE0u);
        const uint32_t rec = main_ram(bp) ? bus_read_u32(bp + 4u) : 0u;
        if (!main_ram(rec)) return;
        int fw = 0, fh = 0;
        footprint(rec, &fw, &fh);
        const bool horiz = fw > fh;
        const int base = horiz
            ? (y == bus_read_u8(br + 0x129u) ? 0xD5 : 0xDA)
            : (x == bus_read_u8(br + 0x128u) ? 0xD0 : 0xD1);
        const uint32_t mt = static_cast<uint16_t>(
            base + 15 * static_cast<int>(bus_read_u8(br + 0xE4u)));
        const uint32_t src = table_a + 2u + 12u * mt;
        for (uint32_t i = 0; i < 6u; ++i) t[i] = bus_read_u16(src + 2u * i);
    }
}

}  // namespace lbstamp

Metatile metatile_tiles(uint32_t map, uint32_t split, uint32_t table_a,
                        uint32_t table_b, int mx, int my, int map_w,
                        int map_h) {
    Metatile out{};
    if (mx < 0 || my < 0 || mx >= map_w || my >= map_h) return out;
    const uint32_t row = bus_read_u32(map + 0x34u + uint32_t(my) * 4u);
    if (!main_ram(row)) return out;
    out = expand_metatile(split, table_a, table_b,
                          bus_read_u16(row + uint32_t(mx) * 2u));
    // docs/378: the cartridge's two stamps, after the plain decode, as
    // Terrain_DecodeMetatile runs them.
    if (out.ok && lbstamp::on())
        lbstamp::stamp(mx, my, out.t, table_a, map_w, map_h);
    return out;
}

// The widened columns AND ROWS. Indexed by HARDWARE TILE ROW and HARDWARE TILE
// COLUMN, both of which may be negative -- that is the whole point of the two
// origins below.
//
// GOAL THIS SERVES: an adjustable window size without the game zooming in. A
// taller window has to buy more field of view rather than a bigger picture, and
// the render path for that already works (472x256 fills a 1892x1031 window at a
// constant magnification, 1.79x the visible area). What kept it opt-in was this
// table: columns had an origin from the first day and rows started at zero, so
// every row above the guest's own window fell through to whatever the tilemap
// happened to hold there and came out as a band of stale sprite tiles across
// the top of the screen.
//
// WHICH CELLS BELONG TO THE GUEST, now that rows can fall outside its picture.
//
// For columns the answer was static: hardware tile columns 0..31 are the
// window the game maintains and everything else is this port's. Rows looked
// like they wanted the same treatment -- a count of maintained tile rows -- and
// measuring that count turned out to be both harder and unnecessary.
//
// WHAT THE MEASUREMENT SAID. func_020A3234 aligns the camera to the tilemap:
// column by whole tiles, but ROW forced even, because a metatile is two tile
// rows tall -- `cam_ty = (camy / 8) & ~1`. The 0..15 px of vertical scroll that
// alignment throws away comes back as BG*VOFS, so the visible 192 px can start
// part-way into the first tile row and needs ceil((192 + 15) / 8) = 26 rows.
//
// The first reading appeared to refute that: with the camera sitting still,
// this file's reconstruction of the terrain agreed with the guest's BG1 exactly
// through row 23 and disagreed wholesale from row 24 on, at every report of a
// five-second run. Real reading, wrong conclusion -- the camera had come to
// rest at an aligned position, where BG1VOFS is 0, rows 24..25 are never
// sampled, and they hold whatever was last written there. Panning settles it:
// during vertical motion `bgy` reads 8 and 13 on BG1 and BG2, and the same
// per-row diff then reads exact through row 25.
//
// So a maintained row is not a property of the layer at all. It is a property
// of the MOMENT: the guest keeps a row up to date exactly while its own 192-row
// picture reaches it, and lets it go stale as soon as it does not. A fixed
// count would have to be the maximum, 26, which then defers rows 24..25 to
// stale data whenever the camera is parked -- a strip of old fog and old
// terrain across the bottom of a taller window.
//
// THE RULE THIS USES INSTEAD is the same statement without the arithmetic: a
// cell is the guest's when the GUEST'S OWN SCANLINE is on screen. The decoder
// knows that directly -- it is handed the guest scanline `gy`, and gy outside
// 0..191 is a row the game never draws and therefore never maintains, whatever
// the scroll offset happens to be. Inside 0..191 nothing is substituted in the
// native columns, so the game's own picture stays bit-for-bit identical; that
// is the same guarantee the columns have always given, stated for rows.
const int kGuestScanlines = 192;
// HOW FAR THE TABLES REACH, AND WHY IT IS THIS FAR.
//
// These bounds are not a preference; they are the render ceiling divided by 8,
// and they have to be re-derived whenever that ceiling moves. A cell outside
// them is DECLINED by lb_widemap_tile, and a declined cell falls through to the
// hardware wrap and draws whatever the guest's tilemap happens to hold -- the
// garbage this whole file exists to replace.
//
// The old values, -16..47 and -8..33, were exactly right for the old 512x256
// ceiling: (512-256)/2 = 128 px of side band is 16 tiles, and (256-192)/2 = 32
// px of top band is 4 rows, both inside the table with room to spare. Raising
// the ceiling to 1024x512 without touching them would have left the tables
// covering 128 px of a band that is now 384 px, so the OUTER 256 px of each
// side -- two thirds of the widened field -- would have drawn the hardware
// wrap. This is the "fog or terrain might not appear right in extended
// screenspace" fault, and at the new ceiling it would have been the majority
// of the picture rather than an edge case.
//
// The arithmetic, so the next person moving the ceiling can redo it. Note the
// two axes are no longer derived the same way -- see the note below the
// constants on why the columns take the WHOLE overhang and the rows half of it:
//   side band  =  1728 - 256      = 1472 px = 184 tiles -> columns -184..216
//   top band   = ( 768 - 192) / 2 =  288 px =  36 tiles -> rows      -36..60
// plus a few tiles of slack, because a scroll offset puts a partial tile at
// each end and the fill is cheaper than an off-by-one at the screen edge.
//
// THESE FOLLOW THE SURFACE CEILING AND MUST BE REDONE WITH IT. They were sized
// for 1024x512 -- columns -48..79, rows -20..43, which the old -52..83 /
// -24..47 covered exactly -- and raising the ceiling to 1728x768 without
// raising them was measured, not guessed: lb_widemap_tile declines a cell
// outside these tables, and its own comment says a declined cell "falls
// through to the hardware wrap and draws garbage". At 1728x768 the counter
// read `declined_out_of_table=21957120 (MUST be 0)`, and the picture had
// strips of stale sprite tiles down the right edge and black rectangles
// punched into the terrain along the bottom.
const int kExtRowLo = -76;               // hardware tile rows kExtRowLo..kExtRowHi
const int kExtRowHi = 100;
const int kExtRows = kExtRowHi - kExtRowLo + 1;
const int kExtLo = -192;                 // hardware tile columns kExtLo..kExtHi
const int kExtHi = 224;
const int kExtCols = kExtHi - kExtLo + 1;

// The ceiling these are sized against, so a future raise cannot silently
// outgrow them.
//
// NEITHER AXIS IS HALF THE OVERHANG ANY MORE. The columns were
// 92 = (1728-256)/2/8 and the rows 36 = (768-192)/2/8,
// which is what a band split evenly either side can reach. lb_band_left and
// lb_band_top now SLIDE the guest's own window within the surface at a map
// edge, so the whole overhang can sit on one side:
//
//     columns  1728 - 256 = 1472 px = 184 tiles, all of it on the left at the
//              map's east edge and all on the right at its west edge
//     rows      768 - 192 =  576 px =  72 tiles, likewise north and south
//
// Leaving the tables at half that would put the outer half of the band outside
// them, and lb_widemap_tile declines a cell outside these tables -- the decline
// falls through to the hardware wrap and draws garbage, which is exactly the
// `declined_out_of_table` failure the 1728 raise already cost this port once.
// Sized for the worst case rather than the typical one, because the typical one
// is the map's MIDDLE and the failure only appears at its edge.
static_assert(-kExtLo >= 184 && kExtHi >= 32 + 184,
              "extension columns no longer cover a fully one-sided 1728 band");
static_assert(-kExtRowLo >= 72 && kExtRowHi >= 24 + 72,
              "extension rows no longer cover a fully one-sided 768 band");

// Is this cell the guest's own, i.e. one this file must decline so the game's
// picture is left bit-for-bit alone? `gy` is the GUEST scanline being decoded,
// which is negative above the game's own picture and >= 192 below it.
inline bool guest_cell(int htile, int gy) {
    return htile >= 0 && htile < 32 && gy >= 0 && gy < kGuestScanlines;
}

// LB_EXTCLAMP=N narrows the served band to N tiles either side of the guest's
// own window, WITHOUT rebuilding.
//
// It exists so the table-size fix is falsifiable rather than merely asserted.
// Enlarging the tables and then observing a clean run proves nothing on its
// own: the run would look identical if the tables had never been the limit.
// LB_EXTCLAMP=16 reproduces the old bounds exactly, so the same binary, the
// same window and the same route can be run both ways and the difference --
// in the decline counter and in the captured picture -- attributed to the
// table size and to nothing else.
int ext_clamp() {
    static int n = -2;
    if (n == -2) {
        const char* v = std::getenv("LB_EXTCLAMP");
        n = v ? std::atoi(v) : -1;
    }
    return n;
}

// Outside what this file can serve for the current settings. Shared by all
// three suppliers so terrain, fog and shroud can never disagree about where
// the band ends -- a disagreement there would show as fog without terrain
// under it, or the reverse, in a ring at the edge of the picture.
inline bool ext_out_of_range(int htile, int trow) {
    int lo = kExtLo, hi = kExtHi, rlo = kExtRowLo, rhi = kExtRowHi;
    const int c = ext_clamp();
    if (c >= 0) {
        if (-c > lo) lo = -c;
        if (32 + c < hi) hi = 32 + c;
        if (-c > rlo) rlo = -c;
        if (24 + c < rhi) rhi = 24 + c;
    }
    return htile < lo || htile > hi || trow < rlo || trow > rhi;
}

uint16_t g_ext[kExtRows][kExtCols];
bool g_ext_valid = false;

// THE FOG IS ON BG2, AND IT IS A METATILE.
//
// Two wrong answers preceded this one, both corrected by measurement rather
// than by reading further:
//
//   * "there is no fog in these matches" (docs/202) -- from an all-zero probe
//     of a screen-space scratch mask, contradicted by the capture.
//   * "the fog is BG3 tile 0x1F" -- from func_020396E0's third pass and a
//     histogram showing exactly 32x24 of them. Filling the widened columns of
//     BG3 with 0x1F changed nothing on screen, and clearing DISPCNT bit 11
//     changed nothing either, while clearing bit 10 removed the fog entirely.
//     BG3's 0x1F is a transparent CLEAR of an overlay under fog, not the fog.
//
// BG2's entries in a fully fogged view are period 3 across and period 2 down:
//
//     even rows   C2 C0 C1 C2 C0 C1 ...
//     odd rows    BD BB BC BD BB BC ...
//
// which is exactly func_020A3478's 3x2 metatile expansion. So the shroud is
// drawn through the same machinery as the terrain, from one fixed metatile --
// func_0203A4B4 writes index 100 for every tile the visibility test flags. The
// widened columns therefore expand metatile 100 at the SAME phase the terrain
// widener uses, which is why fog and ground stay in step across the seam.
// RETRACTED: metatile 100 is NOT the shroud, and the paragraph above is wrong
// where it says the shroud is expanded from one fixed metatile through the
// terrain's own 3x2 machinery.
//
// Measured. Expanding EVERY metatile in the map's tables and comparing against
// what the guest was observed drawing finds no match at all:
//
//   [lb_fogidx] shroud tiles 0C2 0C0 0C1 0BD 0BB 0BC == metatile -1 (split 440)
//
// on two different freeplay maps. The shroud is not in the metatile tables.
// "func_0203A4B4 writes index 100" is about kClearTile, which is also 100 --
// the tile BG2 holds where ground is REVEALED -- so that function writes the
// CLEAR tile where the visibility test passes. A tile index and a metatile
// index, conflated because they are the same number.
//
// Kept as a named constant only so the retraction has somewhere to live; the
// value is not used to look anything up.
const uint16_t kShroudMetatile = 100u;

// The six tiles of the fog metatile, MEASURED and constant across maps.
//
// Three independent observations agree: the histogram recorded in the comment
// above ("even rows C2 C0 C1 / odd rows BD BB BC"), and the learner's own
// committed values on map-index 0 and map-index 2, which are identical to each
// other and to it. They do not come from the map -- the search above proves
// that -- so they are a fixed shroud tileset, and a constant is the honest way
// to write them down.
//
// Phase order is the metatile's own: three across, then the second row.
//
// If a world ever uses different shroud graphics the seed will be wrong there,
// and the disagreement report at the learner's commit says so in one line and
// the learner's values replace these. That is why the check exists.
const uint16_t kShroudTiles[6] = {0x0C2u, 0x0C0u, 0x0C1u,
                                  0x0BDu, 0x0BBu, 0x0BCu};

// The shroud tileset's index range, measured with them.
//
// "487 interior tiles against 33 edge tiles in one real frame, all within
// 0x0B1..0x0C4" -- the histogram recorded at the edge table. Every interior
// tile above and every edge variant observed in the dump falls inside it.
//
// Used to reject a tile that is not shroud graphics at all before it can be
// learned as an edge variant. See the note at that test for the contaminated
// entry this exists to stop.
// THE UPPER BOUND IS FITTED TO A BIASED SAMPLE, AND IT DISCARDS THE ANSWER.
//
// "one real frame" is the problem. Every mask observable on this map has
// E1 S1 -- the explored region starts at the map's west edge -- so that frame
// could only contain north and west edge variants, and the range fitted to it
// can only cover those.
//
// Measured (docs/259): dumping 0x090..0x0E8 instead of 0x0B1..0x0C4 finds 62
// non-empty tiles, and the ones with a FADING BOTTOM BORDER -- which is what a
// south-facing cloud edge must have, and which NOT ONE tile in 0x0B1..0x0C4
// possesses -- sit just above the old bound:
//
//     0C8  23/64  top6 bot0 lft0 rgt5     open bottom and left  (SW corner)
//     0C9  39/64  top8 bot0 lft5 rgt7     open bottom           (S edge)
//     0CC  53/64  top8 bot0 lft6 rgt6     open bottom           (S edge)
//     0CF  42/64  top8 bot0 lft6 rgt3     open bottom and right (SE corner)
//     0D0  12/64  top5 bot0 lft3 rgt0     open bottom and right (SE corner)
//
// The consequence is worse than a gap: the learner REJECTS a whole metatile if
// any of its six tiles falls outside this range, so on a map that does draw a
// south edge the observation would be thrown away as "not fog" -- the learner
// could never escape the bias that created the bound.
//
// Raised to 0x0D0 to cover those. This is not a claim that 0C8..0D0 ARE the
// south variants: palette indices cannot tell them from other tiles in this
// tileset (controls 0E2/0E6 use the same ramp), so identification still needs
// one real sighting. It is the weaker and safer claim that the fog RECORD is
// the authority on whether a metatile is shrouded -- 100% agreement, docs/247
// -- and this range is only a secondary guard against a contaminated read. The
// record-based gate, mask_is_observed, and the uniform-metatile test all still
// apply, so widening the guard cannot admit a cell the record calls revealed.
//
// UNVERIFIABLE ON THIS MAP, and deliberately so: no south edge is ever drawn
// here, so this changes nothing that can be photographed. It makes the learner
// CAPABLE of the observation it is currently built to discard.
const uint16_t kShroudTileLo = 0x0B1u;
const uint16_t kShroudTileHi = 0x0D0u;
// The shroud layer's CLEAR tile: BG2 holds this wherever ground is
// revealed. Measured, not assumed -- see the histogram in scan_shroud.
const uint16_t kClearTile = 0x064u;
uint16_t g_ext2[kExtRows][kExtCols];
bool g_ext2_valid = false;

// BG3 carries 0x1F over the shrouded area -- transparent, but it is what the
// guest has there, and supplying it stops the widened columns from decoding
// BG3 through the hardware wrap and drawing whatever the 256-wide map holds.
const uint16_t kShroudTile = 0x1Fu;
uint16_t g_ext3[kExtRows][kExtCols];
bool g_ext3_valid = false;

// The map-space record, one byte per MAP TILE, 1 = shrouded. Sized from the
// metatile grid: 3 tiles across and 2 down per metatile.
std::vector<uint8_t> g_seen;
// HAS THIS TILE EVER BEEN LOOKED AT?
//
// g_seen defaults every tile to "shrouded", which is the conservative
// answer for ground the widened view has not watched. That default is
// indistinguishable from a real reading, and the fog-edge learner needs to
// tell them apart: a neighbour mask built partly from defaults is a
// fiction, and an edge table taught from a fiction is worse than one left
// empty.
//
// So record, separately, which tiles the scan has actually read. Same
// indexing, same lifetime.
std::vector<uint8_t> g_obs;
// WHAT KIND of fog tile each displayed tile showed (docs/350 section 4): 1 the
// cartridge's CLOUD -- a tile Fog_CodeToTiles uses only for an UNEXPLORED
// centre -- 2 clear or fog-of-war (only for an explored or visible centre), 0
// not read, not classifiable, or no tables. g_seen's "shrouded" is BG2 != clear,
// i.e. NOT CURRENTLY VISIBLE, which is the wrong thing to hold an EXPLORED
// record against: fog-of-war is explored ground. The record police below use
// this instead whenever the tables are there. Same indexing as g_seen.
std::vector<uint8_t> g_seen_kind;
int fx_tile_kind(uint16_t tile);      // below, beside the fog tables
bool fx_tile_kinds_ready(void);
int g_seen_w = 0;
int g_seen_h = 0;

// THE BAND'S OWN GROUND TRUTH, using TIME as the bridge.
//
// tools/lb_bandaudit.py says the RIGHT band matches the whole-map render on
// 12-23% of its detailed pixels while the guest window is 100.0%. But every
// other check says the band should be right: the substitution fires on 117 of
// 117 right-band tiles, it serves real varied terrain (101 102 103 106 107 183
// 184 185 1C7, not garbage), audit_terrain proves g_ext is correct for the
// guest's own 32 columns, and the SAME loop in fill_terrain fills the band
// columns with the same arithmetic. Something in that chain is false and
// reading it has not found which.
//
// The map render is the only independent reference the pixel test has, so if
// IT is wrong, the test cannot tell. This gets a reference that does not
// involve it at all: ground served into the band now moves into the guest's own
// window when the camera pans, and there the cartridge's own tilemap is the
// truth. Record what the band served for each absolute cell; when that cell
// later appears inside the guest's window, compare.
//
// Disagreement means the band really does serve the wrong tile. Agreement means
// the band is right and lb_bandaudit's map render is what is wrong -- which
// would also explain LEFT/TOP decaying with distance, since those are the parts
// of the render furthest from where it was calibrated.
//
// Defined here, beside g_seen_w, rather than next to the audit that grades it:
// the audit lives outside this anonymous namespace, so a definition there
// cannot satisfy a declaration in here.
std::vector<uint16_t> g_band_served;     // per absolute tile cell, 0 = none
unsigned g_band_checked = 0;
// WHICH BAND SERVED THE CELL, 1..4 -- set by lb_widemap_tile before recording.
//
// This was a left/right bool, which was enough while only horizontal pans were
// being graded. A VERTICAL pan brings rows above and below the guest's window
// into it, and every one of those cells has a hardware column inside 0..31, so
// a bool files them all as "LEFT" and the report reads as a left-band result
// when it is nothing of the kind. Four kinds, so the label cannot lie about
// which band a number came from.
enum BandKind { kBandNone = 0, kBandLeft, kBandRight, kBandAbove, kBandBelow };
int g_band_kind = kBandNone;
const char* band_name(int k) {
    switch (k) {
        case kBandLeft:  return "LEFT";
        case kBandRight: return "RIGHT";
        case kBandAbove: return "ABOVE";
        case kBandBelow: return "BELOW";
        default:         return "?";
    }
}
int g_fill_cam_tx = 0, g_fill_cam_ty = 0;   // the camera fill_terrain used
unsigned g_band_chk[5] = {0, 0, 0, 0, 0};
unsigned g_band_bad[5] = {0, 0, 0, 0, 0};
unsigned g_band_wrong = 0;
bool g_band_truth_on = false;            // set by audit_terrain when armed

// THE SAME BRIDGE FOR FOG -- but fog is not static, so the test is asymmetric.
//
// Terrain can be graded by time because a map's tile data never changes: what
// the band drew at an absolute cell must equal what the cartridge draws there
// whenever the camera arrives. Fog moves. A cell the band correctly drew as
// shrouded can be legitimately revealed before the camera pans to it, and
// grading that as a disagreement would manufacture failures.
//
// Exploration only ever runs one way: ground that has been revealed does not
// go back under the shroud. So the two directions mean different things.
//
//   band shrouded, cartridge later clear -- EXPLAINED. Someone explored it in
//       between. Counted, never faulted.
//   band clear, cartridge later shrouded -- A DEFECT, and one a player would
//       see directly: the widened band showing open ground the game says is
//       unexplored. Exploration cannot produce this.
//   both shrouded, different cloud tile -- the variant, counted apart. A cell's
//       cloud tile depends on its neighbours, so a neighbour revealed in
//       between changes it legitimately; this is a weaker signal than the
//       verdict and is reported as its own number rather than mixed in.
//
// This is the "Faithful fog" requirement -- the band shows fog where the game
// says unexplored -- expressed as something measurable.
unsigned g_scan_frames = 0;   // declared here: fog_record stamps with it
// THE DECISION THE FILL ACTUALLY MADE, for the guest's own 32x24 cells.
//
// [lb_visoff] grades the per-metatile record, which cannot see a per-TILE
// refinement layered on top of it. This records the final `s` the fill
// computed -- record, plus whatever dilation or edge rule is in effect -- for
// the cells the cartridge also draws, so the two can be compared in the SAME
// frame at per-tile resolution. No time bridge, so no variance.
uint8_t g_fill_s[24][32] = {};
bool g_fill_s_valid = false;
std::vector<uint16_t> g_fog_served;      // per absolute tile cell, 0 = none
// WHEN each band fog cell was served, so a stale one can be thrown out.
//
// The band can only be graded by time -- record what the band drew at an
// absolute cell, check it when the camera brings that ground into the guest's
// window -- and the GAP is the problem. Over a long gap the fog legitimately
// moves, so a disagreement means nothing, and the measurement swung between
// 1.1% and 7.6% on identical configurations. That is not a result, it is
// noise, and no fix can be judged against it.
//
// A cell graded a few frames after it was drawn cannot have changed: the
// cartridge's own fog was measured moving neither way over whole runs. So
// stamp each record with the scan frame and let the grader require freshness.
std::vector<uint16_t> g_fog_frame;
unsigned g_fog_chk = 0;         // band fog cells graded
unsigned g_fog_defect = 0;      // band said clear, cartridge says shrouded
unsigned g_fog_explained = 0;   // band said shrouded, cartridge clear
unsigned g_fog_variant = 0;     // both shrouded, different cloud tile
unsigned g_fog_defect_kind[5] = {0, 0, 0, 0, 0};
// THE POSITIVE CONTROL. "0 cells showed open ground where the cartridge says
// shrouded" is also what a run with no fog anywhere prints, and the two must
// not be confusable -- that is measurement bug 5. This counts the band cells
// that were genuinely SHROUDED and drawn with the cartridge's own cloud tile,
// so the report can say how much fog the test actually had to work with.
unsigned g_fog_agree_shrouded = 0;
// Where inside the 3x2 metatile each miss landed.
unsigned g_fog_defect_phase[6] = {0, 0, 0, 0, 0, 0};

// What lb_widemap_fog hands back, per band: how many cells were asked for and
// how many of those got a tile that actually clouds anything.
unsigned g_fogserve_n[5] = {0, 0, 0, 0, 0};
unsigned g_fogserve_fog[5] = {0, 0, 0, 0, 0};

// DOES FOG EVER COME BACK? The whole asymmetric rule above rests on
// exploration running one way, and if this game re-shrouds ground a unit has
// left, then "band clear, cartridge later shrouded" is ordinary behaviour and
// the defect count is measuring my own assumption instead of the port.
//
// So measure it, on the cartridge's own layer, in the guest's own window --
// the one region where this port is already known to be byte-identical to it.
// Per absolute cell: 1 = the cartridge drew open ground, 2 = it drew cloud.
// A 1 -> 2 transition is fog returning. If that count is zero over a whole
// run, the rule holds; if it is not, the defect number above is worthless and
// must be retracted rather than explained.
std::vector<uint8_t> g_cart_fog;
unsigned g_cart_reveal = 0;      // cartridge went shrouded -> clear
unsigned g_cart_reshroud = 0;    // cartridge went clear -> shrouded

void band_record(int abs_row, int abs_col, uint16_t tile) {
    if (!g_band_truth_on) return;
    if (g_seen_w <= 0 || g_seen_h <= 0) return;
    const int tw = g_seen_w * 3, th = g_seen_h * 2;   // tiles, not metatiles
    if (abs_col < 0 || abs_col >= tw || abs_row < 0 || abs_row >= th) return;
    if ((int)g_band_served.size() != tw * th) g_band_served.assign(tw * th, 0u);
    // Bits 11-13 remember WHICH BAND served it, because an aggregate cannot say
    // which band is lying when two measurements disagree -- that is how the
    // pixel test's "LEFT 100%, RIGHT 12-23%" sat beside a cross-check claiming
    // 88.6% of all band cells were wrong. Bit 15 marks the slot occupied, bits
    // 0-9 the tile; bit 14 is left clear.
    g_band_served[(size_t)abs_row * tw + abs_col] =
        uint16_t((tile & 0x3FFu) | 0x8000u |
                 (uint16_t(g_band_kind & 7) << 11));
}

// Same layout, same stamping rules, for the fog layer.
void fog_record(int abs_row, int abs_col, uint16_t tile) {
    if (!g_band_truth_on) return;
    if (g_seen_w <= 0 || g_seen_h <= 0) return;
    const int tw = g_seen_w * 3, th = g_seen_h * 2;
    if (abs_col < 0 || abs_col >= tw || abs_row < 0 || abs_row >= th) return;
    if ((int)g_fog_served.size() != tw * th) g_fog_served.assign(tw * th, 0u);
    if ((int)g_fog_frame.size() != tw * th) g_fog_frame.assign(tw * th, 0u);
    g_fog_served[(size_t)abs_row * tw + abs_col] =
        uint16_t((tile & 0x3FFu) | 0x8000u |
                 (uint16_t(g_band_kind & 7) << 11));
    g_fog_frame[(size_t)abs_row * tw + abs_col] = uint16_t(g_scan_frames);
}
uint32_t g_seen_map = 0;
unsigned g_scan_shrouded = 0;
unsigned g_ask3 = 0;
unsigned g_blanked = 0;   // band cells flattened by the cold-start branch
// BAND CELLS THIS FILE REFUSED BECAUSE THEY FELL OUTSIDE THE TABLES.
//
// This is the only direct measurement of "do the extension tables actually
// cover the band this surface needs". The native-cell audits cannot see it:
// they compare the guest's own 32x24 window, which is exactly the region a
// too-small table still serves correctly. So a table sized for 512x256 and a
// table sized for 1024x512 produce IDENTICAL audit output at 946x512, while
// one of them draws the hardware wrap over two thirds of the widened field.
//
// A declined cell is not blank -- it falls through to the hardware wrap and
// draws whatever the guest's tilemap holds out there, which is stale sprite
// and terrain data the game never maintains. That is fog or terrain not
// appearing right in the extended screen space, in its worst form.
//
// The invariant is simply: this counter is ZERO at every window size.
unsigned g_declined_bounds = 0;
// Band cells the record calls SHROUDED for which no fog tile was available, so
// the fill wrote kClearTile and the unexplored ground shows through. Counted at
// the assignment in fill_terrain; reported on the [lb_shroud] line so it is
// visible in any run without an env var. Zero is the only correct value.
unsigned g_bare_fill = 0;
unsigned g_served3 = 0;

// The six tiles the fog metatile is made of, learned from the guest.
uint16_t g_fog_tiles[6] = {0, 0, 0, 0, 0, 0};
bool g_fog_known[6] = {false, false, false, false, false, false};

// THE SAME SIX TILES, TAKEN FROM THIS FRAME ALONE.
//
// The confirmed table above accumulates across frames and needs 64
// observations per phase before it commits, and until it does, the band was
// drawn FLAT -- lb_widemap_tile served tile 0 for every cell the record
// called unexplored, rather than risk showing hidden ground. Measured, that
// is 740,735 blanked cell fetches in the first report window and 82,368 in
// the second, then zero.
//
// BE CAREFUL WITH THAT NUMBER, because reading it as "seconds" was wrong
// the first time. 740,735 fetches is about NINE frames' worth of a full
// band, spread over a 120-frame report window -- a fifth of a second, not
// the several seconds the visible defect lasted. So this branch is NOT the
// "sides break at times" defect; it is a real but brief blemish at the
// moment a match starts, and it is fixed here because it is cheap to fix
// and because knowing what it is NOT narrows what the other thing is.
//
// Waiting was never necessary. The tiles the cloud is made of are on screen
// RIGHT NOW, in the guest's own BG2, at every shrouded tile -- 520 of 768
// in a real match, about 86 per phase. One frame is ample evidence for
// "which tile does the cartridge draw at this phase"; what the 64-vote
// threshold actually protects against is a frame caught MID-UPDATE, before
// the expansion pass has run, and that is caught instead by the property
// that makes a metatile a metatile: its six tiles are DISTINCT.
//
// So this is the same reading with the waiting removed, used only while the
// confirmed table is still empty. It cannot reveal anything the confirmed
// table would not: it draws the SAME cloud, just sooner.
uint16_t g_fog_prov[6] = {0, 0, 0, 0, 0, 0};
bool g_fog_prov_ok = false;

// ---- fog EDGE variants, learned the same way the interior was --------------
//
// The six tiles above are the INTERIOR pattern: what the cartridge draws where
// a shrouded metatile is surrounded by shrouded metatiles. At a boundary it
// draws a transition variant instead, and the widened columns were drawing the
// interior pattern there -- a cosmetic defect rather than one wrong about
// visibility.
//
// The histogram in scan_shroud already showed the shape of the problem: 487
// interior tiles against 33 edge tiles in one real frame, all within
// 0x0B1..0x0C4.
//
// NOTHING HERE IS INVENTED. The table is learned from the guest's own 32
// columns, exactly as the interior pattern was: for a shrouded metatile whose
// eight neighbours are known, record which tile the cartridge actually drew at
// each of the six positions inside it, keyed by the neighbour mask. The
// widened columns then look up the same key. A mask never seen in the native
// columns falls back to the interior pattern, which is what was drawn before,
// so this can improve the picture and cannot make it worse.
//
// The phase used everywhere else IS the position within the metatile --
// (ar % 2) * 3 + (ac % 3) -- so the interior pattern is simply the entry for
// "all eight neighbours shrouded", and needs no special case.
uint16_t g_fog_edge[256][6];
uint8_t g_fog_edge_seen[256][6];      // observation count, saturating
unsigned g_fog_edge_masks = 0;        // distinct masks with a full metatile

// Bit per neighbour, in reading order with the centre skipped.
inline int neighbour_bit(int dy, int dx) {
    const int i = (dy + 1) * 3 + (dx + 1);
    return i < 4 ? i : i - 1;         // 0..7, centre (dy=0,dx=0) removed
}

// The four cardinal bits of a mask, by the numbering neighbour_bit gives:
// N is (-1,0) -> 1, W is (0,-1) -> 3, E is (0,+1) -> 4, S is (+1,0) -> 6.
inline uint8_t cardinal_of(uint8_t mask) {
    return uint8_t(((mask >> 1) & 1u) | (((mask >> 3) & 1u) << 1) |
                   (((mask >> 4) & 1u) << 2) | (((mask >> 6) & 1u) << 3));
}

// Which tier served each band cell, so the fallback can be measured rather
// than eyeballed: an exact mask, a cardinal match, or the interior pattern.
unsigned g_edge_exact = 0, g_edge_card = 0, g_edge_interior = 0;

// Any observed mask sharing this one's four cardinal neighbours, as a TABLE.
//
// THIS WAS A LINEAR SCAN OF 256 ENTRIES, PER CELL, AND IT IS WHY THE GAME WENT
// CHOPPY. The tier is reached whenever the exact 8-bit mask was never
// observed, and only six masks of 256 ever are -- so nearly every one of the
// ~73,000 band cells in a frame paid a 256-iteration search, up to ~18 million
// iterations per frame. The game ran very laggy and choppy, and logging was
// the first suspect; it was this.
//
// The answer is a 16-entry lookup, because the key is only the four cardinal
// bits. Rebuilt when the learner commits something new, which is a handful of
// times per battle, and then O(1) per cell forever after.
uint16_t g_card_tile[16][6];
bool g_card_ok[16][6];
unsigned g_card_built_for = 0xFFFFFFFFu;

// THE SHROUD TILESET'S BORDER COVERAGE, MEASURED FROM VRAM.
//
// docs/259: LB_FOGTILES dumps each tile as 8x8 palette indices; this is the
// count of non-transparent pixels on each of its four borders, 8 = solid cloud
// and 0 = fully faded. Full table in docs/reference/shroud-tile-borders.txt.
//
// Two independent checks say the reading is right. The six tiles at 64/64 with
// every border solid are exactly kShroudTiles -- which docs/248 established by
// an unrelated method -- and every one of the six learned masks is reproduced
// by the rule below.
struct ShroudTileBorders { uint16_t tile; uint8_t top, bot, lft, rgt; };
const ShroudTileBorders kTileBorders[] = {
    {0x0B1, 0, 6, 0, 4}, {0x0B2, 0, 8, 4, 7}, {0x0B3, 4, 8, 7, 8},
    {0x0B4, 2, 8, 8, 6}, {0x0B5, 0, 8, 6, 6}, {0x0B6, 2, 8, 5, 8},
    {0x0B7, 4, 8, 8, 5}, {0x0B8, 0, 8, 5, 5}, {0x0B9, 0, 6, 5, 0},
    {0x0BA, 6, 8, 3, 8}, {0x0BB, 8, 8, 8, 8}, {0x0BC, 8, 8, 8, 8},
    {0x0BD, 8, 8, 8, 8}, {0x0BE, 6, 8, 8, 5}, {0x0BF, 8, 5, 3, 8},
    {0x0C0, 8, 8, 8, 8}, {0x0C1, 8, 8, 8, 8}, {0x0C2, 8, 8, 8, 8},
    {0x0C3, 8, 6, 8, 1}, {0x0C4, 5, 8, 1, 8},
    // Beyond the range the old histogram saw, and the reason it could not:
    // that histogram came from one frame of a map where every mask has E1 S1,
    // so it could only contain north and west variants. These are the
    // south-facing ones. NOT confirmed as shroud graphics by palette -- the
    // controls share the ramp -- so they are used only where nothing observed
    // fits, and the result is measured rather than assumed.
    {0x0C8, 6, 0, 0, 5}, {0x0C9, 8, 0, 5, 7}, {0x0CC, 8, 0, 6, 6},
    {0x0CF, 8, 0, 6, 3}, {0x0D0, 5, 0, 3, 0},
};

// WHICH EDGES OF THE METATILE EACH POSITION TOUCHES.
//
// The metatile is 3 wide x 2 tall, so position 0 is its north-west corner,
// 1 the north edge, 2 the north-east corner, 3 the south-west, 4 the south,
// 5 the south-east. Bit 0 N, 1 W, 2 E, 3 S -- the order cardinal_of uses.
const uint8_t kPosEdges[6] = {
    /*0*/ 0x1 | 0x2, /*1*/ 0x1, /*2*/ 0x1 | 0x4,
    /*3*/ 0x2 | 0x8, /*4*/ 0x8, /*5*/ 0x4 | 0x8,
};

// DERIVE the tile for a cardinal mask and position, from the borders.
//
// The rule, validated against all six learned masks: a tile's OPEN borders are
// exactly the mask's ZERO bits for the edges its position touches. (The mask
// bit is set when the neighbour is SHROUDED, so a 0 means revealed ground next
// door and therefore a fading edge -- the opposite sense to the fog record's
// own bits, which is the trap docs/259 records.)
//
// Scored rather than matched exactly, because a corner tile fades diagonally
// and its two borders are not independent: 0B1 is the north-west corner and
// reads top 0, lft 0 but also rgt 4, which no exact rule would accept.
uint16_t fog_edge_derive(uint8_t card, int pos) {
    const uint8_t edges = kPosEdges[pos];
    const bool want_top_open = (edges & 0x1) && !(card & 0x1);
    const bool want_lft_open = (edges & 0x2) && !(card & 0x2);
    const bool want_rgt_open = (edges & 0x4) && !(card & 0x4);
    const bool want_bot_open = (edges & 0x8) && !(card & 0x8);
    int best = -1, best_score = 1 << 30;
    for (const ShroudTileBorders& t : kTileBorders) {
        // Distance from the openness this position needs. An edge the position
        // does not touch must stay SOLID, or a tile would fade into its own
        // neighbour inside the same metatile.
        int s = 0;
        s += std::abs((want_top_open ? 0 : 8) - int(t.top));
        s += std::abs((want_lft_open ? 0 : 8) - int(t.lft));
        s += std::abs((want_rgt_open ? 0 : 8) - int(t.rgt));
        s += std::abs((want_bot_open ? 0 : 8) - int(t.bot));
        if (s < best_score) { best_score = s; best = int(t.tile); }
    }
    return best < 0 ? kShroudTiles[pos] : uint16_t(best);
}

void fog_edge_build_cardinal(void) {
    if (g_card_built_for == g_fog_edge_masks) return;
    g_card_built_for = g_fog_edge_masks;
    std::memset(g_card_ok, 0, sizeof g_card_ok);
    for (int m = 0; m < 256; ++m) {
        const uint8_t c = cardinal_of(uint8_t(m));
        for (int p = 0; p < 6; ++p) {
            if (!g_fog_edge_seen[m][p] || g_card_ok[c][p]) continue;
            g_card_tile[c][p] = g_fog_edge[m][p];
            g_card_ok[c][p] = true;
        }
    }
    // ...and DERIVE the rest, because observation cannot reach them.
    //
    // The six learnable masks collapse to only THREE distinct cardinal
    // combinations -- (N0,E1,S1,W0), (N1,E1,S1,W0) and all-shrouded -- so 13 of
    // 16 have no entry and every cell needing one drew the square interior.
    // Measured, that is 1161 of 2365 band edge cells in one report, 49% of
    // them.
    //
    // Observation WINS: this only fills entries the loop above left empty, and
    // a mask the cartridge is later seen drawing replaces the derived tile on
    // the next rebuild. LB_FOGDERIVE=0 turns the tier off for an A/B.
    static const bool derive_on = [] {
        const char* v = std::getenv("LB_FOGDERIVE");
        return !(v && v[0] == '0');
    }();
    if (!derive_on) return;
    for (int c = 0; c < 16; ++c)
        for (int p = 0; p < 6; ++p)
            if (!g_card_ok[c][p]) {
                g_card_tile[c][p] = fog_edge_derive(uint8_t(c), p);
                g_card_ok[c][p] = true;
            }
}

inline const uint16_t* fog_edge_cardinal(uint8_t mask, int pos) {
    // LB_FOGCARD=0 disables this tier, for an A/B against the square edges.
    static const bool on = [] {
        const char* v = std::getenv("LB_FOGCARD");
        return !(v && v[0] == '0');
    }();
    if (!on) return nullptr;
    const uint8_t c = cardinal_of(mask);
    return g_card_ok[c][pos] ? &g_card_tile[c][pos] : nullptr;
}

// BG2's screen base, stashed by scan_shroud so the edge learner can read the
// same tilemap without recomputing it from DISPCNT and BG2CNT.
uint32_t g_bg2_base = 0u;

// Counters, so a zero result says WHICH guard rejected everything rather than
// leaving it to be guessed at. Declared here, above scan_shroud, because that
// is where they are PRINTED -- they were first put next to the learner, which
// is further down the file, and the print then did not compile.
unsigned g_le_cand = 0, g_le_notshroud = 0, g_le_margin = 0, g_le_notfog = 0,
         g_le_took = 0;
unsigned g_fog_agree = 0;
unsigned g_fog_frames_used = 0;
unsigned g_fog_frames_dropped = 0;
// The last verdict of the fog-tile learner, so a failure to learn can say why.
bool g_phase_have[6] = {false, false, false, false, false, false};
uint16_t g_phase_cand[6] = {0, 0, 0, 0, 0, 0};
unsigned g_phase_n[6] = {0, 0, 0, 0, 0, 0};
bool g_phase_clean = false;
// The raw per-phase modal tile THIS FRAME, before the distinctness gate.
bool g_frame_have[6] = {false, false, false, false, false, false};
uint16_t g_frame_modal[6] = {0, 0, 0, 0, 0, 0};
bool g_frame_expanded = false;

unsigned g_bg3_marked = 0;
// Which phase anchoring is true, counted both ways every frame.
unsigned g_match_screen = 0;
unsigned g_match_abs = 0;
unsigned g_match_nonzero = 0;

// Candidate map-space visibility buffers found by correlation, pending
// verification against further views.
// Each candidate carries its OWN stride. A single shared stride was wrong:
// it held whatever the last match used, so a verify pass re-checked every
// candidate at that stride and kept exactly the ones it happened to match.
// `last_rev` is the whole-map revealed count this candidate showed on the
// previous verification pass, and it is the discriminator the adoption side was
// missing. See the monotonicity test in find_fog_record.
struct Cand { uint32_t base; uint32_t stride; uint32_t last_rev; int miss; };
Cand g_cand[24] = {};
int g_cand_n = 0;

// The visibility grid, once discovered and verified. Zero means "not found";
// the widened columns then fall back to the conservative window cache.
uint32_t g_vis_base = 0;
uint32_t g_vis_stride = 0;

// ---- the game's own visibility record, LOCATED rather than correlated ------
//
// Every previous attempt at this searched RAM for a buffer whose contents
// correlate with the fog the guest draws, and graded the candidates against
// g_seen -- which is itself the approximation being replaced. A buffer that
// matches the proxy's own blind spots scores well, which is why four
// candidates in a row were adopted and then dropped for disagreeing.
//
// The address does not have to be guessed. Decompiling the one function that
// reads the mask (func_020EEBF4, 32 bytes) and its callers gives the chain
// outright. func_02036D1C tests a rectangle for visibility:
//
//     uVar5 = *(undefined4 *)(*DAT_02036eb8 + 0x10);
//     ... FUN_020eebf4(uVar5, x, y) on each of the four corners ...
//     if (!bypass && all four read 0) return 0;          // nothing visible
//
// and the accessor itself is
//
//     return (*(byte *)(*(int *)(*(int *)(param_1 + 0x14) + param_3 * 4) +
//                       (param_2 >> 3)) & 1 << (param_2 & 7)) != 0;
//
// so the descriptor carries a ROW TABLE at +0x14 and the record is ONE BIT
// PER CELL, not one byte -- which is why the old sampler, reading
// base + my*stride + mx as a byte, could never have read this record
// correctly even had the search landed on it.
//
// DAT_02036eb8 is 0x021569E4, read out of the static ARM9 image. Measured in
// a live battle, the descriptor it leads to reads:
//
//     +0x04 stride 8      = (64 + 7) / 8
//     +0x08 width  64     the MAP's width in metatiles, not the viewport's 32
//     +0x0C height 64     the MAP's height,             not the viewport's 24
//     +0x10 data          +0x14 row table
//
// 64x64 is the map (map+0x14/+0x15), so this is the map-space record the
// widened columns have needed all along. The two viewport-sized bitmasks that
// were mistaken for it -- the global at 0x02151F28 and the one at view+0x164 --
// both read 0 of 3072 bits over a 20 s linger, before and after. They are
// allocated, the view-width patch reallocates them correctly, and nothing
// populates them.
//
// POLARITY, from the decomp rather than assumed: the caller returns "not
// visible" when the corners read 0, so a SET bit means revealed.
//
// The holder and the descriptor both MOVE between battles -- measured at
// 0226991C in one run and 022698E0 in the next -- so this is re-read rather
// than cached across a session.
const uint32_t kFogHolder = 0x021569E4u;
uint32_t g_vis_rowtab = 0;          // descriptor +0x14
int g_vis_w = 0, g_vis_h = 0;
bool g_vis_is_bits = false;         // located record: one bit per metatile

// Read one metatile from whichever record was adopted.
//
// Both shapes live here so the five sample sites stay one line each, and so
// the bit layout is written down once. Out of range is "not revealed", which
// is the conservative answer: it can over-fog, never reveal hidden ground.
inline bool vis_revealed(int mx, int my) {
    if (g_vis_is_bits) {
        if (mx < 0 || my < 0 || mx >= g_vis_w || my >= g_vis_h) return false;
        const uint32_t row = bus_read_u32(g_vis_rowtab + uint32_t(my) * 4u);
        if (!main_ram(row)) return false;
        return ((bus_read_u8(row + uint32_t(mx >> 3)) >> (mx & 7)) & 1u) != 0u;
    }
    if (!g_vis_base) return false;
    return bus_read_u8(g_vis_base + uint32_t(my) * g_vis_stride +
                       uint32_t(mx)) != 0u;
}

// Follow the chain and validate it against the map this battle is on.
//
// Validated, not trusted: the holder is a live pointer and a frame caught
// between battles can have it pointing at a freed or half-built object. The
// dimensions must match the map's own, and the stride must be the one the
// setter computes from the width, or this is not the record it claims to be.
bool locate_fog_record(int map_w, int map_h) {
    const uint32_t holder = bus_read_u32(kFogHolder);
    if (!main_ram(holder)) return false;
    const uint32_t desc = bus_read_u32(holder + 0x10u);
    if (!main_ram(desc)) return false;
    const uint32_t stride = bus_read_u32(desc + 4u);
    const uint32_t w = bus_read_u32(desc + 8u);
    const uint32_t h = bus_read_u32(desc + 0xCu);
    const uint32_t rowtab = bus_read_u32(desc + 0x14u);
    if (!main_ram(rowtab)) return false;
    if (w != uint32_t(map_w) || h != uint32_t(map_h)) return false;
    if (stride != (w + 7u) / 8u) return false;
    if (bus_read_u32(rowtab) == 0u || !main_ram(bus_read_u32(rowtab)))
        return false;
    g_vis_rowtab = rowtab;
    g_vis_w = int(w);
    g_vis_h = int(h);
    g_vis_is_bits = true;
    g_vis_base = desc;      // non-zero so the "have a record" tests hold
    g_vis_stride = stride;
    return true;
}
// THE SEARCH IS NOT ONE-SHOT ANY MORE, and that it was is why a drop was fatal
// for the rest of the session.
//
// find_fog_record used to be gated on a function-local `static bool done` set
// the moment it first succeeded. Nothing ever cleared it, so when the grid was
// later dropped -- for any reason, including a single bad frame -- the widened
// columns fell back to the conservative cache permanently. Measured on one
// route: the band is 84.3% fogged while the grid is adopted and 99.1% after it
// is dropped, and it never came back. That is the spotty fog that crept onto
// the traversed area where the base and units are.
bool g_findfog_done = false;
// Views the search could actually correlate against -- balanced ones, with
// both revealed and shrouded ground in them. See the give-up test.
unsigned g_findfog_views = 0;
// Consecutive frames whose sample disagreed with the guest's own fog. A grid
// is retired on a STREAK, never on one frame; see the drop site.
unsigned g_vis_bad_streak = 0;
// Per-metatile "treat as shrouded" bitmap for this frame; see build_vis_bits.
std::vector<uint8_t> g_vis_bits;
unsigned g_interior_n = 0;   // interior holes reported so far
// Bare-cell reports emitted this frame; see the note at the [lb_bares]
// print. Reset at the top of each fill so the cap is per frame.
unsigned g_bare_n = 0;

unsigned g_vis_passes = 0;
unsigned g_vis_last_count = 0;
unsigned g_vis_shrinks = 0;
unsigned g_vis_checked = 0;
unsigned g_vis_wrong = 0;
// Addresses a shrink has disqualified. Never adopted again this session.
uint32_t g_vis_bad[16] = {};
int g_vis_bad_n = 0;

// WHERE WIDENED-COLUMN VISIBILITY COMES FROM. Now the game's own map-space
// grid by default; LB_FOGSRC=window restores the old window-derived cache.
//
// WHY THE DEFAULT CHANGED. The window-derived cache (g_seen) records which map
// tiles the guest's own 32x24 window has personally covered, and defaults
// everything else to SHROUDED. That is not the game's fog, it is a proxy for
// it, and the proxy is wrong in the direction that ruins the feature: an RTS
// camera pans sideways far more than vertically, so on a 1920x1080 surface the
// bands above and below the camera's track had never been "covered" and were
// drawn as solid cloud. Measured at 946x512: 84.5% of the surface was this
// port's own fog, with the top, bottom and right bands each containing exactly
// six distinct 8x8 tiles -- one fog metatile, repeated -- while the native area
// held 117-140. On screen that looked like the world not scaling with the
// window, and it was filed alongside real geometry bugs, which is why it
// survived.
//
// The map-space path reads the record the GAME maintains, so it agrees with the
// fog the player is actually being shown rather than approximating it. In the
// run that settled this the game's own record reported every sampled cell
// revealed AND the native view agreed -- "[lb_shroud] shrouded=0/768" -- so the
// cloud the old path drew over four-fifths of the screen was contradicting the
// cartridge, not protecting it.
//
// This is a fidelity fix, not a reveal-the-map cheat: find_fog_record locates
// the grid per battle and the every-frame agreement check below disqualifies a
// base whose readings disagree with the guest's own shrouded cells, falling
// back rather than showing ground the game is hiding.
// ON by default. Turning it off on the measurement below produced a visible
// regression: a straight horizontal fog edge along the top and bottom of the
// picture -- the fog view square around the original aspect -- with the base
// and units cut off at it.
//
// MEASURED AFTERWARDS, fog on, sitting at the base, winning layer per pixel:
//
//   306x216   TOP band 100.0% fog   BOTTOM 100.0% fog   LEFT 91.7% TERRAIN
//   386x276   TOP band 100.0% fog   BOTTOM 100.0% fog   LEFT 96.4% TERRAIN
//   guest window, the control, 26.3% terrain -- a quarter of it revealed
//
// The conservative record only knows ground the guest's own 32x24 window has
// drawn. The camera in that run had moved horizontally and never
// vertically, so the columns beside the window were known and every row above
// and below it was not -- and unknown means shrouded. That is not a tuning
// problem, it is the shape of the record: it cannot answer for ground the
// window has never occupied, and the vertical bands are exactly that ground.
//
// So the game's own visibility record stays the default, because it is the
// only source that can answer there. Its own fault is real and is recorded
// below; it is the smaller of the two by a wide margin.
//
// The band's fog is now graded the same way its terrain is -- record what the
// band served at an absolute cell, check it against the cartridge's own BG2
// when the camera brings that ground into the guest's window. The cartridge's
// fog was verified static over each run (0 cells revealed, 0 re-shrouded), so
// a disagreement in either direction is the band's.
//
//   The Pond,  this record   7592 graded   583 open-where-shrouded (7.7%)
//   The Pond,  conservative  8264 graded     0                     (0.0%)
//   Avast,     this record   9608 graded     0                     (0.0%)
//   Avast,     conservative  8872 graded     0                     (0.0%)
//
// with 6445, 7868, 9446 and 8670 cells respectively drawn shrouded AND
// matching the cartridge's own cloud tile, so no row of that table is the
// vacuous "nothing was fogged, nothing disagreed". The guest's own window
// stayed EXACT throughout, which is why the every-frame guard never fired:
// this record is right where the camera is and wrong further out, and the
// guard can only sample the guest's window.
//
// WHY THE "0 over-fogged cells" IN THOSE RUNS WAS NOT EVIDENCE. The band fog
// check scores a cell only once the camera brings that ground into the guest's
// window, and it files "band drew fog, cartridge says clear" as explained by
// exploration. Every run above drops into a fresh battle and pans away
// immediately, so there is almost no explored-but-off-window ground for it to
// catch -- the one case that matters here. A zero from a test that cannot
// produce a nonzero is measurement bug 5 again, and this time I wrote the
// caveat down and then read the zero as a pass anyway.
//
// The remaining fault in this record -- open ground where the cartridge says
// shrouded, 7.7% of graded band cells on The Pond and 0% on Avast -- is the
// thing to fix, rather than swapping it for a record that cannot see past the
// window at all. LB_FOGSRC=w selects that conservative source for comparison.
// OFF BY DEFAULT, WHICH IS WHAT docs/206 DECIDED AND THE CODE HAD STOPPED
// DOING. Opt in with LB_FOGSRC=map.
//
// That document ends "the switch stays off and is labelled experimental", and
// gives the reason:
//
//     "With LB_FOGSRC=map, after the camera has moved, the widened band breaks
//      into vertical stripes of alternating terrain and fog roughly a metatile
//      wide. The default window-derived path does not do this. Whatever the
//      grid is, the way it is being indexed is wrong at some camera positions."
//
// The default here had become 1 anyway. Exactly that artefact then showed up
// in captures twice -- nudging the camera caused visual corruption, and spotty
// fog crept onto the traversed area where the base and units are -- and the
// directional counter in the policing check says the grid that produces it is
// not merely mis-indexed but the wrong RECORD: 36 of 37 disagreements had it
// calling ground revealed that the cartridge was hiding.
//
// AND THE EVIDENCE WAS READ BACKWARDS EARLIER. I measured the band as
// 84.3% fogged with a grid adopted against 99.1% without, and reported the
// lower number as the better one. It is not automatically better: part of that
// 15-point difference is ground the cartridge is deliberately hiding. Less fog
// is only an improvement when it is CORRECT fog.
//
// The conservative window-derived cache is artefact-free and can never reveal
// hidden ground. Its cost is a straight fog wall at the native boundary, which
// is cosmetic conservatism -- a far better failure than striping, and a far
// better failure than showing the player what the game is keeping from them.
// ON by default again, and this time the record is LOCATED rather than
// searched for. It was turned off because the correlation search kept adopting
// wrong buffers and painting scattered cloud over explored ground -- spotty
// fog creeping onto the traversed area. That cause is gone: locate_fog_record follows the pointer chain
// out of the decomp, and the record it reaches agrees with the guest's own fog
// 100.0% across all three metatile phases over 400,000 samples, against
// 88.6 / 94.6 / 94.3% for the best correlated candidate.
//
// LB_FOGSRC=window restores the old window-derived cache, which draws the fog
// square around the original DS aspect.
int g_fogsrc_map = [] {
    const char* v = std::getenv("LB_FOGSRC");
    return (v && (v[0] == "w"[0] || v[0] == "0"[0])) ? 0 : 1;
}();
// Geometry captured at vblank for the render-time scan below.
int g_cam_tx = 0, g_cam_ty = 0, g_map_w = 0, g_map_h = 0;
// THE CAMERA IN PIXELS, not in tiles, and kept beside the tile version rather
// than derived from it.
//
// Everything else in this file works in whole 8 px tiles because that is the
// unit the tilemap is addressed in. The band clamp below is the one consumer
// that must not round: it decides how far the picture may show past the
// camera, and rounding that to a tile would make the map's edge jump 8 px at a
// time while the camera slides past it one pixel at a time. view+0x5E is the
// camera's x in map pixels -- lb_widemap_fill divides exactly this value by 8
// to get cam_tx, so the two cannot disagree about where the camera is.
// Cells the cartridge re-clouded after showing them, and the reverse. See
// the note at the g_seen write in scan_shroud: these settle whether this
// game's fog is "explored forever" or "currently visible", which decides
// whether a visibility grid that SHRINKS is the right one or the wrong one.
unsigned g_reshroud = 0, g_unshroud = 0;
// Agreement with the cartridge's own fog, split by the camera's metatile
// phase. See the accumulation site for why this is the deciding table.
unsigned g_phase_checked[3] = {0, 0, 0};
unsigned g_phase_wrong[3] = {0, 0, 0};
int g_cam_px_x = 0;
int g_cam_px_y = 0;
bool g_cam_px_valid = false;
// The value g_cam_px_valid had on entry, so a tolerated one-frame bail can put
// it back rather than assert a validity it has not established.
bool g_cam_px_was_valid = false;
// Consecutive frames the DISPCNT test has rejected. Reset on any accepted
// frame; see the note at the test itself.
unsigned g_dispcnt_miss = 0u;
// WHAT THE TEST ACTUALLY SAW, and for how long at a stretch.
//
// The last attempt tolerated a one-frame blip and did not move the numbers,
// because "3604 bails against 3122 runs-through" says nothing about whether
// those are alternating or in runs -- and the band log prints only on CHANGE,
// so its 13 transitions read equally as 13 alternations or 13 runs. The run
// length distinguishes them outright. Engine B's register is recorded beside
// engine A's because the test reads 0x04000000, and if the field is on the
// other engine for part of the time that is the whole answer.
uint32_t g_dispcnt_bail_a = 0u;   // engine A (0x04000000) when the test failed
uint32_t g_dispcnt_bail_b = 0u;   // engine B (0x04001000) at that same moment
unsigned g_dispcnt_run_max = 0u;  // longest consecutive rejection so far
unsigned g_dispcnt_ok_run = 0u;   // and the longest accepted stretch
// WHICH EARLY RETURN LEFT THE BAND FALLING BACK TO THE CENTRED SPLIT.
//
// lb_widemap_fill() clears g_cam_px_valid on entry and sets it only at the very
// end, so any early return costs the band its clamp for that frame -- and
// band_compute() then returns span/2, which is 4 px away from the real answer
// at the measured surface. Measured: left=721 on the 13 frames with geom=1 and
// left=725 on the 13 with geom=0, alternating. That is the residual shake left
// after the camera clamp, and the counter says which of the five returns does
// it rather than leaving it to be guessed at (the first guess, the visibility
// fingerprint skip, was in a different function entirely).
//
// [0] ran through  [1] stage/enhanced  [2] DISPCNT  [3] view/map ptr
// [4] map dims     [5] tile tables
unsigned g_fill_bail[6] = {0, 0, 0, 0, 0, 0};

// Is this metatile hidden according to the record? Off the map counts as
// hidden, the same convention the widened columns already use at a map edge.
bool hidden_metatile(int mx, int my) {
    if (g_vis_bits.empty()) return false;
    if (mx < 0 || my < 0 || mx >= g_map_w || my >= g_map_h) return true;
    return g_vis_bits[size_t(my) * size_t(g_map_w) + size_t(mx)] != 0u;
}
uint32_t g_map = 0, g_dispcnt = 0;
bool g_geom_valid = false;

// DOES THE MODEL DESCRIBE THE PICTURE AT ALL?
//
// choose_camera has to compute this to do its job, so the band may as well be
// gated on it. When no candidate camera agrees with the guest's own tilemap,
// this port's idea of the map is not the one on screen -- a scene changing, a
// battle loading, a map pointer swapped under it -- and anything drawn in the
// widened columns from that model would contradict the field beside it.
//
// Measured, this happens for the first 73 frames of a run and nowhere else in
// a 3480-frame run with 24 pan steps. That is about 1.2 seconds at the
// start, which is exactly the kind of window in which the fog fails to show
// or the sides, or even the original-aspect field, corrupt.
//
// While it holds, the band draws BLANK rather than wrong. Blank is still
// served rather than declined, because a declined column falls through to the
// hardware wrap and draws whatever the tilemap happens to hold there, which is
// the very garbage this is avoiding.
bool g_model_ok = false;
unsigned g_model_bad_frames = 0;
// Distribution of best_bad on a REJECTED frame: index 1..8 exact, 9 = "9 or
// more", 0 unused. With the max, this says whether rejections are near-misses
// or genuine disagreements -- see choose_camera.
unsigned g_model_bad_hist[10] = {0};
int g_model_bad_max = 0;
// Rejections that happen AFTER the model has once been good. Startup
// rejections -- the map not built yet -- are harmless and expected; a
// rejection after the model has worked is the one that could show as a stale
// band mid-play.
bool g_model_was_ok = false;
unsigned g_model_bad_after_ok = 0;
// The metatile tables, published by the vblank pass so the render-time camera
// check can expand a metatile without re-reading the map header.
uint32_t g_split = 0, g_table_a = 0, g_table_b = 0;
// Recent camera readings, newest first. See the note where they are pushed.
int g_cam_hist_tx[4] = {0, 0, 0, 0};
int g_cam_hist_ty[4] = {0, 0, 0, 0};
int g_cam_hist_n = 0;
bool fog_learned() {
    for (int i = 0; i < 6; ++i) if (!g_fog_known[i]) return false;
    return true;
}

// HOW LONG THE BAND SPENT BLANK, reported once.
//
// The area outside the original aspect ratio loading slowly on entering a new
// match is exactly the interval between the first scanned frame of a battle
// and the frame the fog tiles become known, because a widened cell the record
// calls shrouded is drawn blank until then. Counting scanned frames measures
// that directly, and does so whether the fill is fast or slow -- a pixel
// comparison only sees it on a map that actually has fog.
bool g_fog_wait_done = false;
// What the map-table seed supplied, kept so the observational learner can be
// used as an independent check on it. See the disagreement report.
uint16_t g_fog_seeded[6] = {0, 0, 0, 0, 0, 0};
bool g_fog_seeded_ok = false;
void fog_note_ready(const char* how) {
    if (g_fog_wait_done) return;
    g_fog_wait_done = true;
    // g_scan_frames is declared above and counts scanned frames for the whole
    // session, so this is frames-since-boot, not frames-since-this-battle.
    // For the cold start that is the number wanted: the first battle is the
    // one the player waits through.
    std::fprintf(stderr,
                 "[lb_fog] tiles known after %u scanned frame(s) (%s)\n",
                 g_scan_frames, how);
    std::fflush(stderr);
}

// Frames seen with nothing shrouded anywhere in the guest's window. Kept as a
// diagnostic; it is no longer allowed to decide whether anything is drawn.
unsigned g_clear_frames = 0;

// THE COLD START -- and a correction, because the first answer made it worse.
//
// The fog tiles can only be learned from a frame that HAS fog, so for the first
// frames of a battle they are unknown, and a widened column the map-space record
// calls "shrouded" must not be drawn as plain terrain.
//
// The first attempt refused to widen BG1 at all until the fog was known, on the
// reasoning that "empty columns are honest". THAT WAS WRONG, and a user capture
// showed why: a layer that is not extended is not blank. decode_text_line still
// runs over the widened columns using the HARDWARE WRAP, so those columns drew
// structured garbage from elsewhere in the tilemap. "Not extended" and "empty"
// are not the same thing, and the gate turned a small leak into visible
// corruption across the whole left band.
//
// It also never lifted. The escape hatch wanted 120 consecutive frames with
// nothing shrouded anywhere; measured near the player's base, `clear_frames`
// sat at 0 because every frame has some fog somewhere on screen. So the gate
// was permanently closed and the corruption was permanent with it.
//
// The rule now: ALWAYS widen BG1, so the columns are always served and can
// never fall through to the wrap. Where the record says shrouded and the fog
// tiles are not yet known, serve tile 0 instead of the terrain -- see
// lb_widemap_tile. Nothing hidden is revealed, and nothing garbage is drawn.

// A tiny per-phase vote. Four candidates is ample: a phase sees about 128
// observations in a fogged frame and a correct fill puts them all on one value,
// so this only has to survive a handful of strays.
const int kVotes = 4;
struct Tally { uint16_t v[kVotes]; int n[kVotes]; };

void vote(Tally& t, uint16_t value) {
    for (int i = 0; i < kVotes; ++i) {
        if (t.n[i] > 0 && t.v[i] == value) { ++t.n[i]; return; }
    }
    for (int i = 0; i < kVotes; ++i) {
        if (t.n[i] == 0) { t.v[i] = value; t.n[i] = 1; return; }
    }
    // Full and no match: weaken the candidates so a persistent newcomer can
    // still displace a stale one rather than being locked out.
    for (int i = 0; i < kVotes; ++i) --t.n[i];
}

// Read back the 32x24 window of fog the guest just drew, and record it in map
// space. The screen base is computed the way the guest computes its own DMA
// destination (func_0210BC2C): the DISPCNT map block plus BG2CNT's screen base
// field. Deriving it rather than hard-coding 0x0000 means a mode change cannot
// silently point this at another layer's tilemap.
//
// WHICH TILES THE FOG IS MADE OF IS LEARNED, NOT ASSUMED.
//
// func_0203A4B4 writes metatile index 100 for every flagged tile, so this first
// expanded metatile 100 -- and the self-check below reported `window match =
// 0/768` on a screen that is entirely fog. Metatile 100 expands to
// 11E 19F 1A0 / 121 1A1 1A2, while the fog on screen is C2 C0 C1 / BD BB BC.
// Whatever that 100 indexes, it is not this map's metatile table.
//
// So the six tiles are read out of the guest's own BG2 instead, at positions
// BG3 marks as shrouded, one per phase. That needs no index, survives a
// different tileset on a different map, and cannot drift from what is actually
// on screen. `g_fog_seen` is the cross-check: it counts shrouded tiles whose
// BG2 entry matches what was learned for their phase, and it should equal the
// shrouded count.
void scan_shroud(uint32_t dispcnt, uint32_t map, int map_w, int map_h,
                 int cam_tx, int cam_ty) {
    const int tw = map_w * 3;
    const int th = map_h * 2;
    if (g_seen_map != map || g_seen_w != tw || g_seen_h != th) {
        g_seen.assign(size_t(tw) * size_t(th), 1u);   // unseen == shrouded
        g_obs.assign(size_t(tw) * size_t(th), 0u);    // nothing read yet
        g_seen_kind.assign(size_t(tw) * size_t(th), 0u);
        g_seen_w = tw;
        g_seen_h = th;
        g_seen_map = map;
    }
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg2cnt = nds_io_debug_read(9, 0x0400000Cu, 16);
    const uint32_t bg3cnt = nds_io_debug_read(9, 0x0400000Eu, 16);
    const uint32_t base = block + ((bg2cnt >> 8) & 0x1Fu) * 0x800u;
    g_bg2_base = base;
    const uint32_t base3 = block + ((bg3cnt >> 8) & 0x1Fu) * 0x800u;

    // THE SHROUD TILESET'S GRAPHICS, once (LB_FOGTILES=1).
    //
    // docs/252: the east and south edge variants cannot be LEARNED on this
    // map -- the explored region starts at the map's own west edge, so every
    // fog boundary has revealed ground to its west or north and only those
    // variants are ever drawn. Observation is a dead end; the pixels are not.
    //
    // The shroud tileset is 0x0B1..0x0C4 and the observed variants use eleven
    // of those twenty. An east edge is the horizontal mirror of a west edge,
    // so the unobserved nine can be IDENTIFIED by comparing pixels rather than
    // guessed at -- the same standard kShroudTiles was held to.
    //
    // Dumps each tile as eight rows of eight palette indices. 4bpp, 32 bytes
    // per tile, low nibble first, from the BG character base: DISPCNT bits
    // 24..26 select a 64K block and BG2CNT bits 2..3 a 16K one inside it.
    // NOT ON THE FIRST SCAN. A one-shot here fired before the battle had
    // loaded its tileset and dumped twenty tiles of zeros twice. Wait until
    // the fog tiles are known, which is the port's own evidence that the
    // shroud graphics are resident.
    if (std::getenv("LB_FOGTILES") && fog_learned() && g_scan_frames > 300u) {
        static bool said = false;
        if (!said) {
            said = true;
            const uint32_t cblock = ((dispcnt >> 24) & 7u) * 0x10000u;
            const uint32_t cbase = cblock + ((bg2cnt >> 2) & 3u) * 0x4000u;
            std::fprintf(stderr,
                         "[lb_fogtiles] char base %06X (dispcnt %08X "
                         "bg2cnt %04X)\n",
                         cbase, unsigned(dispcnt), unsigned(bg2cnt));
            // 256-COLOUR, so 64 bytes per tile and one byte per pixel.
            // BG2CNT bit 7 says which, and it is SET here (bg2cnt 4082) --
            // a first version assumed 4bpp, used a 32-byte stride, and read
            // twenty tiles of zeros. Printed as two hex digits per pixel.
            const bool c256 = ((bg2cnt >> 7) & 1u) != 0u;
            const uint32_t tsize = c256 ? 64u : 32u;
            // THE RANGE IS A HYPOTHESIS, SO LET IT BE WIDENED.
            //
            // 0x0B1..0x0C4 came from a histogram of ONE frame ("487 interior
            // tiles against 33 edge tiles, all within 0x0B1..0x0C4"), and that
            // frame is from the same biased sample as everything else here:
            // every observable mask on this map has E1 S1, because the explored
            // region starts at the map's west edge. So the range can only
            // contain the variants that sample happens to use.
            //
            // It shows: of the twenty, only two are corner-sparse (0B1 at 17/64
            // and 0B9 at 23/64) though a metatile has four corners, and NOT ONE
            // fades at its bottom edge -- the lowest bottom coverage is 5 of 8,
            // and that one (0BF) was observed at a TOP-left position, so it is
            // internal shape rather than an edge. A south-facing cloud edge
            // cannot be drawn with these twenty tiles at all. Either the
            // cartridge draws south edges some other way, or the range is
            // simply short.
            //
            // LB_FOGTILES_LO / LB_FOGTILES_HI (hex) widen it so that can be
            // answered by looking instead of argued about.
            uint32_t lb_lo = 0x0B1u, lb_hi = 0x0C4u;
            if (const char* s = std::getenv("LB_FOGTILES_LO"))
                lb_lo = (uint32_t)std::strtoul(s, nullptr, 16);
            if (const char* s = std::getenv("LB_FOGTILES_HI"))
                lb_hi = (uint32_t)std::strtoul(s, nullptr, 16);
            for (uint32_t t = lb_lo; t <= lb_hi; ++t) {
                char rows[8][20];
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        uint32_t v;
                        if (c256) {
                            v = nds_vram_read_bg(
                                0, cbase + t * tsize + uint32_t(y) * 8u +
                                       uint32_t(x), 1) & 0xFFu;
                        } else {
                            const uint32_t b = nds_vram_read_bg(
                                0, cbase + t * tsize + uint32_t(y) * 4u +
                                       uint32_t(x) / 2u, 1);
                            v = (x & 1) ? ((b >> 4) & 0xFu) : (b & 0xFu);
                        }
                        rows[y][x * 2] = "0123456789ABCDEF"[(v >> 4) & 0xFu];
                        rows[y][x * 2 + 1] = "0123456789ABCDEF"[v & 0xFu];
                    }
                    rows[y][16] = 0;
                }
                std::fprintf(stderr,
                             "[lb_fogtiles] %03X  %s %s %s %s %s %s %s %s\n",
                             t, rows[0], rows[1], rows[2], rows[3],
                             rows[4], rows[5], rows[6], rows[7]);
            }
            std::fflush(stderr);
        }
    }

    static Tally tally[6] = {};
    Tally frame_tally[6] = {};
    static uint32_t tally_map = 0;
    if (tally_map != map) { for (int i = 0; i < 6; ++i) tally[i] = Tally{}; tally_map = map; }
    if (scan_logging() && g_scan_frames % 120u == 0u) {
        // The stripe is in the STORED cache, not the test -- absolute phase is
        // now proven correct (abs=624 against screen=0 at an unaligned camera),
        // yet the left band still stripes. So print the cache itself across the
        // widened columns rather than reasoning about it again.
        {
            char row[160];
            int n = 0;
            const int ar = cam_ty + 12;
            for (int h = kExtLo; h <= kExtHi && n < 150; ++h) {
                const int ac = cam_tx + h;
                char c = '?';
                if (ar >= 0 && ar < g_seen_h && ac >= 0 && ac < g_seen_w)
                    c = g_seen[size_t(ar) * size_t(g_seen_w) + size_t(ac)]
                            ? '#' : '.';
                if (h == 0 || h == 32) row[n++] = '|';
                row[n++] = c;
            }
            row[n] = 0;
            std::fprintf(stderr, "[lb_seen] row %d: %s\n", ar, row);
        }
        // ACROSS THE BOUNDARY, side by side.
        //
        // The cache is clean and the terrain is exact, so if the fog still
        // stripes it is the emitted TILE that is wrong, not the decision to
        // emit one. Print the tile values for a run of hardware columns either
        // side of the native boundary: the guest's own BG2 for columns it owns,
        // this file's g_ext2 for the ones it supplies. Both are the same 3-cycle
        // over absolute position, so a continuous sequence across the boundary
        // means the phases agree and a jump means they do not.
        {
            const int ty_probe = 12;
            const uint32_t bg2base =
                block + ((nds_io_debug_read(9, 0x0400000Cu, 16) >> 8) & 0x1Fu)
                            * 0x800u;
            char line[240];
            int n = 0;
            for (int h = -6; h <= 6 && n < 220; ++h) {
                uint16_t v = 0;
                char src = ' ';
                if (h >= 0 && h < 32) {
                    v = uint16_t(nds_vram_read_bg(
                        0, bg2base + uint32_t(ty_probe * 32 + h) * 2u, 2));
                    src = 'g';                       // guest's own
                } else if (h >= kExtLo && h <= kExtHi) {
                    v = g_ext2[ty_probe - kExtRowLo][h - kExtLo];
                    src = 'p';                       // this port's
                }
                n += std::snprintf(line + n, sizeof(line) - size_t(n),
                                   "%c%03X ", src, v & 0x3FFu);
            }
            std::fprintf(stderr, "[lb_seam] h=-6..6 row %d: %s\n",
                         ty_probe, line);

            // IS THE FOG TILESET CONTIGUOUS?
            //
            // Every fog tile seen so far -- 0B1 0B2 0B3, 0BB 0BC 0BD,
            // 0C0 0C1 0C2 -- sits inside 0x0B1..0x0C2. If the whole fog set
            // occupies one compact index range then "shrouded" is simply
            // "BG2 in [lo, hi]", which recognises EDGE tiles as well as
            // interior ones and needs no per-pattern table at all. Listing
            // every distinct value BG2 holds this frame settles whether that
            // is true or wishful.
            uint16_t seen[64];
            int seen_c[64] = {};
            int seen_n = 0;
            for (int py = 0; py < 24 && seen_n < 64; ++py) {
                for (int px = 0; px < 32 && seen_n < 64; ++px) {
                    const uint16_t v = uint16_t(nds_vram_read_bg(
                        0, bg2base + uint32_t(py * 32 + px) * 2u, 2)) & 0x3FFu;
                    bool have = false;
                    for (int i = 0; i < seen_n; ++i)
                        if (seen[i] == v) { have = true; break; }
                    if (!have) { seen[seen_n] = v; seen_c[seen_n] = 1; ++seen_n; }
                    else { for (int i = 0; i < seen_n; ++i) if (seen[i] == v) { ++seen_c[i]; break; } }
                }
            }
            for (int i = 1; i < seen_n; ++i)         // insertion sort
                for (int j = i; j > 0 && seen[j] < seen[j - 1]; --j) {
                    const uint16_t t = seen[j];
                    seen[j] = seen[j - 1];
                    seen[j - 1] = t;
                    const int tc = seen_c[j];
                    seen_c[j] = seen_c[j - 1];
                    seen_c[j - 1] = tc;
                }
            char dl[300];
            int dn = 0;
            for (int i = 0; i < seen_n && dn < 280; ++i)
                dn += std::snprintf(dl + dn, sizeof(dl) - size_t(dn),
                                    "%03X:%d ", seen[i], seen_c[i]);
            std::fprintf(stderr, "[lb_bg2set] %d distinct: %s\n", seen_n, dl);
        }
        std::fprintf(stderr,
                     "[lb_anchor] abs=%u screen=%u nonzero=%u of 768  "
                     "cam_tx %d (%%3=%d)\n",
                     unsigned(g_match_abs), unsigned(g_match_screen),
                     unsigned(g_match_nonzero),
                     cam_tx, floor_mod(cam_tx, 3));
        std::fflush(stderr);
    }
    g_match_abs = 0;
    g_match_screen = 0;
    g_match_nonzero = 0;
    g_fog_agree = 0;

    // 24 rows, not 26: func_0203A4B4's loop is `ty < 0x18`, so the last two
    // rows of the 32x26 buffer are whatever was there before and must not be
    // read as an answer.
    for (int ty = 0; ty < 24; ++ty) {
        const int ar = cam_ty + ty;
        if (ar < 0 || ar >= th) continue;
        const int sub = floor_mod(ar, 2);
        for (int tx = 0; tx < 32; ++tx) {
            const int ac = cam_tx + tx;
            if (ac < 0 || ac >= tw) continue;
            const uint32_t off = uint32_t(ty * 32 + tx) * 2u;
            // SHROUD IS DECIDED BY BG2, NOT BG3.
            //
            // BG3's 0x1F looked like the marker and is not: measured, it covers
            // the ENTIRE window, 768/768, in views where the middle of the
            // screen is plainly revealed terrain. Driving the cache from it
            // marked the whole map shrouded, which is why the widened bands came
            // out unconditionally grey with a dead-straight edge at the native
            // boundary -- they were never showing visibility, just a constant.
            //
            // BG2 is the layer masking proved actually draws the fog, so a tile
            // is shrouded exactly when BG2 has something opaque on it there.
            const uint16_t bg2 = uint16_t(nds_vram_read_bg(0, base + off, 2));
            // A NON-ZERO TILE INDEX IS NOT AN OPAQUE TILE.
            //
            // Both "BG3 == 0x1F" and "BG2 != 0" were tried as the shroud test
            // and both reported 768/768 -- the entire window shrouded -- in
            // frames whose captures show a large revealed region through the
            // middle. Neither is wrong about the tilemap: both layers really do
            // carry an entry on every visible tile. They are wrong about the
            // PICTURE, because most of those entries reference a transparent
            // graphic and paint nothing.
            //
            // What actually paints fog is the specific six-tile metatile the
            // tally learns, so the test is equality with the tile for this
            // phase. Before those are known nothing is claimed, and the cache
            // keeps its conservative all-shrouded default.
            // ABSOLUTE PHASE, because the VERDICT IS STORED ABSOLUTELY.
            //
            // This used screen phase (tx % 3), and a per-frame table indexed
            // that way is self-consistent -- which is why `agree` read 768/768
            // and it looked right. But g_seen is indexed by absolute tile, and
            // it accumulates across frames. Screen phase makes the phase for a
            // given map tile depend on where the camera happens to be, so the
            // same tile is tested against a different expected value on
            // different frames and the cache ends up disagreeing with itself.
            //
            // On screen that is a stripe every third column through the part of
            // the map the camera has already passed, while the never-scanned
            // side stays uniform. Both bands looked right in a single fogged
            // view, which is why this survived so long.
            const int ph = floor_mod(ar, 2) * 3 + floor_mod(ac, 3);
            // SHROUDED IS "NOT THE CLEAR TILE", and that is the whole test.
            //
            // A full histogram of BG2 in a real battle, once the harness could
            // no longer measure a menu by mistake:
            //
            //   064:248  0B1:3 0B2:4 0B3:4 0B4:1 0B5:1 0B6:1
            //   0BB:87 0BC:87 0BD:75  0BF:8  0C0:82 0C1:82 0C2:74  0C4:11
            //
            // 487 interior tiles, 33 edge tiles, and 248 of 0x064 -- and the
            // capture of that frame is about one third revealed. So 0x064 is
            // the layer's CLEAR tile, not the "raw pre-expansion index" it was
            // taken for; a menu reading 064 on all 768 tiles was a fully
            // REVEALED screen, which is why that reading looked so strange.
            //
            // Testing equality against the six INTERIOR tiles could only ever
            // classify an edge tile as revealed, which is exactly what made the
            // widened boundary wrong. Testing against the clear tile instead
            // gets all 25 edge variants right without needing to enumerate them.
            const bool shrouded = (bg2 & 0x3FFu) != kClearTile;
            // WHICH ANCHORING IS TRUE? Count both every frame. In a fogged view
            // the correct convention matches every visible tile; the wrong one
            // matches only where the two happen to coincide, and that share
            // moves with the camera -- which is precisely what stripes the
            // accumulated cache. Reasoning has now been wrong about this twice,
            // so it gets a counter instead of an argument.
            if (fog_learned()) {
                const int ph_screen = (ty & 1) * 3 + tx % 3;
                if ((bg2 & 0x3FFu) == (g_fog_tiles[ph_screen] & 0x3FFu))
                    ++g_match_screen;
                if (shrouded) ++g_match_abs;
                // "BG2 is non-empty" as a shroud test, counted for comparison.
                //
                // This was rejected once for reading 768/768 -- but that was
                // while the scan ran at VBLANK, on a staging buffer the guest
                // had not finished writing. The scan now runs at render time,
                // so the reading is worth taking again. It matters because the
                // equality test above can only ever recognise the six INTERIOR
                // tiles, and therefore calls every EDGE tile revealed; a
                // non-empty test would include them.
                if ((bg2 & 0x3FFu) != 0u) ++g_match_nonzero;
            }
            if ((nds_vram_read_bg(0, base3 + off, 2) & 0x3FFu) == kShroudTile)
                ++g_bg3_marked;
            // Only claim a tile once the fog tiles are known; until then the
            // cache keeps its conservative default rather than recording a
            // guess.
            // No longer gated on fog_learned(): the test is independent of
            // the interior tiles now, so visibility can be recorded from the
            // first frame instead of waiting for a fogged view to learn from.
            // DOES THE CARTRIDGE EVER RE-SHROUD GROUND IT HAS REVEALED?
            //
            // The codebase contradicts itself on this and everything about the
            // visibility grid follows from the answer:
            //
            //   docs/206  "Ground once explored is explored forever, so an
            //              explored record's revealed count can only rise. A
            //              single decrease rules it out."
            //   this file "the game clouds everything not CURRENTLY visible,
            //              so the grid that shrinks is the one whose shape
            //              matches the fog on screen."
            //
            // Both cannot hold, and the change that added a monotonicity
            // filter to the ADOPTION side -- rejecting any candidate whose
            // revealed count falls -- picked docs/206 without checking. If
            // this file's note is the true one, that filter rejects the right
            // buffer every time.
            //
            // It is decidable here and nowhere else: g_seen is the cartridge's
            // own BG2 verdict for a cell the guest window is drawing right
            // now, so a cell going REVEALED -> SHROUDED while still on screen
            // is the game re-clouding ground it had shown. Count those, and
            // count the reverse for scale.
            {
                const size_t idx = size_t(ar) * size_t(tw) + size_t(ac);
                const bool was_obs = !g_obs.empty() && g_obs[idx] != 0u;
                const bool was_shrouded = g_seen[idx] != 0u;
                if (was_obs && shrouded && !was_shrouded) ++g_reshroud;
                if (was_obs && !shrouded && was_shrouded) ++g_unshroud;
            }
            // A CAMERA THE MODEL CANNOT EXPLAIN MUST ABSTAIN, NOT RECORD.
            //
            // g_model_ok gates every SERVE of the band, but it did not gate
            // this WRITE -- so when choose_camera could not explain the guest's
            // tilemap (routinely, mid-pan, while the streamer is rewriting
            // columns) the port still stamped the guest's BG2 into g_seen at
            // whatever absolute cell the rejected camera implied. Those stamps
            // are permanent: g_seen is only cleared on a map change.
            //
            // That is shroud appearing over traversed or even starting
            // terrain depending on camera pans -- cloud written at
            // coordinates the player never saw it at. It is also
            // self-reinforcing, because the record police grade the visibility
            // record against this same cache and can drop a CORRECT record for
            // disagreeing with a poisoned one.
            //
            // Abstaining costs nothing: a cell not written this frame keeps
            // whatever it last had, and the next frame with an explained
            // camera writes it correctly.
            if (g_model_ok) {
                g_seen[size_t(ar) * size_t(tw) + size_t(ac)] =
                    shrouded ? 1u : 0u;
                g_obs[size_t(ar) * size_t(tw) + size_t(ac)] = 1u;
                if (!g_seen_kind.empty())
                    g_seen_kind[size_t(ar) * size_t(tw) + size_t(ac)] =
                        uint8_t(fx_tile_kind(uint16_t(bg2 & 0x3FFu)));
            }
            if (shrouded) ++g_scan_shrouded;

            // VOTE ON CLOUD TILES ONLY -- not on every tile, and not gated on
            // the shroud test either.
            //
            // Gating on the shroud test cannot bootstrap, because that test
            // depends on the learned tiles. Voting on EVERYTHING does not work
            // for the reason the old note assumed away: "fog dominates its
            // phase in any fogged view" only holds when fog covers most of the
            // phase. In a partly fogged view the clear tile wins the phases
            // that are mostly open ground, two of those phases then hold the
            // same value, and the pairwise-distinct filter throws the frame
            // away -- every frame, for the whole run. Measured on a fixed
            // route, same binary, three runs:
            //
            //   used 1574 dropped 226   saw 0C2 0C0 0C1 0BD 0BB 0BC   learned
            //   used    0 dropped 1920  saw 0C2 0C0 064 0BD 0BB 064   never
            //   used 1572 dropped 228   saw 0C2 0C0 0C1 0BD 0BB 0BC   learned
            //
            // 064 is kClearTile, in two phases at once. Nothing is ever
            // learned, so lb_widemap_fog declines every call and the widened
            // band draws NO FOG AT ALL -- open ground over terrain the game is
            // hiding, which is the one direction the band must never fail in.
            //
            // kClearTile is a constant this file already uses to decide the
            // shroud test, not something learned, so skipping it here costs no
            // bootstrap. A phase with no cloud in it this frame simply
            // abstains, which the filter below already handles.
            const uint16_t bg2_i = uint16_t(bg2 & 0x3FFu);
            if (bg2_i == kClearTile || bg2_i == 0u) continue;

            // SCREEN PHASE, NOT MAP PHASE.
            //
            // The terrain's metatile phase is absolute: the guest scrolls by
            // rewriting the tilemap, so screen column 0 shows map column
            // cam_tx. The fog is not. Indexing this tally by (cam_tx + tx) % 3
            // produced a phase -> tile table that ROTATED as the camera moved,
            // while screen column 0 read 0C2 at every camera position tried
            // (61, 111 and 126). The cloud fill is laid out from screen column
            // 0, so the widened columns have to be laid out from there too --
            // anchoring them to the map instead would break the pattern
            // exactly at the seam with the native columns.
            // Absolute, for the same reason as the shroud test above: a
            // learned table indexed by screen phase is only valid for the
            // frame it was learned in.
            const int phase = floor_mod(ar, 2) * 3 + floor_mod(ac, 3);
            vote(frame_tally[phase], bg2);
        }
    }

    // POLICE THE ADOPTED GRID AGAINST THE WINDOW.
    //
    // g_seen is exact for the 32 columns the guest actually draws, so it can
    // check the discovered grid for free on every frame. If the grid stops
    // agreeing -- a new battle reallocating it elsewhere, a map change, or the
    // correlation having latched onto a coincidence in the first place -- it is
    // dropped and rediscovered, and the widened columns fall back to the
    // conservative cache in the meantime. Nothing here can strand the picture
    // on a stale address.
    if (g_vis_base) {
        unsigned checked = 0, wrong = 0, wrong_revealed = 0;
        for (int ty = 0; ty < 24; ty += 2) {
            const int ar = cam_ty + ty;
            const int my = floor_div(ar, 2);
            if (my < 0 || my >= map_h) continue;
            for (int tx = 0; tx < 32; tx += 3) {
                const int ac = cam_tx + tx;
                const int mx = floor_div(ac, 3);
                if (mx < 0 || mx >= map_w) continue;
                if (ar < 0 || ar >= th || ac < 0 || ac >= tw) continue;
                // ONLY CELLS THE GUEST HAS ACTUALLY DISPLAYED.
                //
                // g_seen starts at 1 (shrouded) everywhere and is written only
                // where the guest's own columns have been scanned. g_obs is the
                // companion "has this been read yet" mask, and this comparison
                // ignored it -- so every cell the camera has never visited
                // counted as "the grid says revealed, the screen says
                // shrouded", a disagreement manufactured by the metric.
                //
                // That bites hardest at LARGE surfaces, because a wider view
                // clamps the camera further from the map edges and leaves more
                // of the map never displayed. Measured before this guard:
                // 97.4 / 99.2 / 18.5% by cam_tx%3 at a 928 px guest, against
                // 100 / 100 / 99.3% at 512 px. A rendering fault would not
                // care how much of the map the camera had toured.
                if (g_obs.empty() ||
                    !g_obs[size_t(ar) * size_t(tw) + size_t(ac)])
                    continue;
                const bool grid_shrouded = !vis_revealed(mx, my);
                // EXPLORED AGAINST EXPLORED (docs/350 section 4).
                //
                // The record is the EXPLORED grid (holder+0x10). g_seen says
                // "not the clear tile", which is NOT CURRENTLY VISIBLE -- and
                // the cartridge draws explored ground nobody can see as
                // fog-of-war, not clear. So with none of the player's units in
                // the window every explored cell there read as "the record
                // opens ground the screen hides": measured 126 of 132 on an
                // unattended game, and the CORRECT record was dropped as
                // cheating and re-located the next frame, three log lines a
                // frame for 20,872 frames.
                //
                // Held against the cartridge's own tile families instead --
                // cloud only for an unexplored centre -- the comparison means
                // what it says. A tile the tables cannot classify is skipped;
                // without the tables this is the old comparison.
                bool seen_shrouded =
                    g_seen[size_t(ar) * size_t(tw) + size_t(ac)] != 0u;
                if (fx_tile_kinds_ready() && !g_seen_kind.empty()) {
                    const uint8_t k =
                        g_seen_kind[size_t(ar) * size_t(tw) + size_t(ac)];
                    if (k == 0u) continue;
                    seen_shrouded = (k == 1u);          // cloud: unexplored
                }
                ++checked;
                if (grid_shrouded != seen_shrouded) {
                    ++wrong;
                    // The dangerous direction: the grid would open ground the
                    // cartridge is hiding.
                    if (!grid_shrouded) ++wrong_revealed;
                }
            }
        }
        // A quarter disagreeing is far beyond the blur expected from comparing
        // a per-metatile grid against per-tile truth at a fog edge.
        g_vis_checked = checked;
        g_vis_wrong = wrong;
        // AGREEMENT BY METATILE PHASE, accumulated rather than logged.
        //
        // The adopted grid reads 98.5% agreement one frame and 72.7% another.
        // Exactly one mechanism makes that camera-dependent without the buffer
        // being the wrong record: the grid is indexed floor_div(cam_tx + tx, 3)
        // and a metatile is 3 tiles across, so an origin misaligned by one or
        // two columns agrees on one residue of cam_tx % 3 and collapses on the
        // others. docs/206 guessed at "the indexing is wrong at some camera
        // positions" and never tested it.
        //
        // Per-frame logging cannot answer it -- the report fires rarely enough
        // that a whole run produced three samples, all at the same phase. So
        // the totals are kept here, one bucket per residue, and printed as a
        // table. A misalignment shows up as one bucket near 100% and the
        // others far below; a wrong record shows up as all three alike.
        {
            const int ph = floor_mod(cam_tx, 3);
            g_phase_checked[ph] += checked;
            g_phase_wrong[ph] += wrong;
            static unsigned n = 0;
            // EVERY 60, not 240. The harness never issues orders, so the AI
            // wins: a long linger ends in "Defeated!", the view object goes
            // away and the camera freezes. A table that only prints every 240
            // scans can miss the whole battle.
            if (std::getenv("LB_BANDLOG") && (++n % 60u) == 0u) {
                std::fprintf(stderr, "[lb_phase] agreement by cam_tx%%3:");
                for (int p = 0; p < 3; ++p)
                    std::fprintf(stderr, "  %d: %.1f%% of %u", p,
                                 g_phase_checked[p]
                                     ? 100.0 * double(g_phase_checked[p] -
                                                      g_phase_wrong[p]) /
                                           double(g_phase_checked[p])
                                     : 0.0,
                                 g_phase_checked[p]);
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
            }
        }
        // ON A STREAK, NOT ON ONE FRAME.
        //
        // This used to drop the grid the first time a single sample crossed
        // 25%, and that is far too eager for a per-frame check. Measured: the
        // frame before the drop agreed 127/132 (96.2%) and the drop frame read
        // 68/132 -- a step change, not a drift, and the grid never came back
        // because the search was one-shot. The band went from 84.3% fogged to
        // 99.1% and stayed there.
        //
        // A WRONG buffer disagrees every frame, so a streak costs it nothing;
        // a RIGHT one only has to survive the moments where this comparison is
        // unfair -- the camera crossing to ground the guest's window has not
        // drawn yet, or a frame caught while the game is rewriting its own fog.
        // Half a second of tolerance, the same shape of fix the comment beside
        // the `shrank` test already proposed.
        //
        // WHICH WAY the disagreement runs is logged rather than assumed,
        // because the two directions mean opposite things: grid-revealed over
        // guest-shrouded would be the port showing ground the game hides,
        // which is the one thing the widening may never do, while
        // grid-shrouded over guest-revealed is only over-fogging.
        // THE TWO DIRECTIONS ARE NOT EQUALLY URGENT, and treating them alike
        // was wrong in a way that showed on screen.
        //
        // grid-SHROUDED over guest-revealed only over-fogs: the picture is
        // uglier than it needs to be and nothing is given away. Waiting out a
        // streak there is free, and is what stops a good grid being retired
        // over one unfair frame.
        //
        // grid-REVEALED over guest-shrouded is the port opening ground the
        // cartridge is hiding, which is the single thing the widening may
        // never do. Waiting 30 frames on that is 30 frames of cheating, and
        // with the search now rearming it becomes a CYCLE: adopt a wrong
        // buffer, paint half a second of scattered cloud over revealed ground,
        // drop it, adopt the next one. Measured on this route -- 021B632E
        // disagreeing 37/132 with 36 of the 37 in this direction, then
        // 02314FCF, then 0227C3F9 -- and visible as rectangular fog blocks
        // strewn across ground the player has explored.
        //
        // So this direction drops on the spot, as it did before, and only the
        // harmless one gets the streak.
        const bool cheating = wrong_revealed * 2u > wrong;
        if (checked >= 32 && wrong * 4 > checked) {
            g_vis_bad_streak += cheating ? 30u : 1u;
            if (g_vis_bad_streak <= 30u)
                std::fprintf(stderr,
                             "[lb_findfog] %08X disagrees %u/%u (%u revealed "
                             "over shrouded, %u shrouded over revealed) -- "
                             "%s\n",
                             g_vis_base, wrong, checked, wrong_revealed,
                             wrong - wrong_revealed,
                             cheating ? "OPENS HIDDEN GROUND, dropping now"
                                      : "watching");
        } else {
            g_vis_bad_streak = 0;
        }
        if (g_vis_bad_streak >= 30u) {
            std::fprintf(stderr,
                         "[lb_findfog] DROPPED %08X: %u/%u disagree with the "
                         "guest's own fog, %u frames running\n",
                         g_vis_base, wrong, checked, g_vis_bad_streak);
            std::fflush(stderr);
            // REMEMBER IT, or rearming the search just finds it again.
            //
            // g_vis_bad is already consulted when candidates are pruned, but
            // only the `shrank` path ever wrote to it -- a buffer dropped for
            // DISAGREEING was free to be re-adopted on the next pass. With the
            // search rearming on a drop that closes a loop: adopt, paint wrong
            // fog, drop, adopt the same address again. Seen on this route as
            // three adoptions in one battle.
            if (g_vis_bad_n < 16) g_vis_bad[g_vis_bad_n++] = g_vis_base;
            g_vis_base = 0u;
            // OR THE FALLBACK READS A BYTE RECORD AS BITS. vis_revealed
            // branches on this, so a drop has to clear it with the base.
            g_vis_is_bits = false;
            g_vis_rowtab = 0u;
            g_cand_n = 0;
            g_vis_passes = 0;
            g_vis_bad_streak = 0;
            // Look again. The address is heap and discovered, so the usual
            // reason to lose it is that it moved, not that none exists.
            g_findfog_done = false;
        }
    }

    // Commit the modal value per phase, EVERY FRAME.
    //
    // The first version latched on first sight and was wrong for the whole
    // battle: it learned 079 07A 07B / 089 08A 08B from a frame where BG3
    // already said "shrouded" but BG2 still held the previous scene, and then
    // never revised it. The giveaway was `agree=0` on a screen that reads
    // 0C1 0C2 0C0 0C1 0C2 0C0 -- perfectly periodic, and nothing like what had
    // been learned. Taking the mode of this frame's ~128 observations per phase
    // cannot be fooled by one bad frame, and a phase with nothing shrouded in
    // it simply keeps the value it had.
    if (g_scan_shrouded == 0u) { if (g_clear_frames < 100000u) ++g_clear_frames; }
    else g_clear_frames = 0u;

    // THE FRAME FILTER: only an EXPANDED frame gets to vote.
    //
    // This runs early enough in the frame that BG2 usually still holds the raw
    // index func_0203A4B4 writes, before the expansion pass replaces it. Those
    // frames are not evidence about what the fog looks like, and letting them
    // vote is what kept the table from ever committing.
    //
    // Detecting them needs no constant, only the property that makes a 3x2
    // metatile a metatile: ITS SIX TILES ARE DISTINCT. A frame holding one
    // value across two different phases has not been expanded, so its votes are
    // dropped whole. Phases with no shrouded tile this frame simply abstain,
    // which is what lets sparse fog near the player's base still converge.
    {
        uint16_t fm[6];
        bool fh[6] = {false, false, false, false, false, false};
        for (int p = 0; p < 6; ++p) {
            int best = -1;
            for (int c = 0; c < kVotes; ++c)
                if (frame_tally[p].n[c] > 0 &&
                    (best < 0 || frame_tally[p].n[c] > frame_tally[p].n[best]))
                    best = c;
            if (best >= 0) { fm[p] = frame_tally[p].v[best]; fh[p] = true; }
        }
        bool expanded = true;
        for (int p = 0; p < 6 && expanded; ++p)
            for (int q = 0; q < p; ++q)
                if (fh[p] && fh[q] &&
                    (fm[p] & 0x3FFu) == (fm[q] & 0x3FFu)) { expanded = false; break; }
        // THE PROVISIONAL TABLE, from this frame alone. Same evidence the
        // accumulating tally uses and the same distinctness guard, without
        // the 64-vote wait that left the band flat for the first seconds of
        // every match. See the note on g_fog_prov.
        if (expanded) {
            bool all = true;
            for (int p = 0; p < 6; ++p) if (!fh[p]) all = false;
            if (all) {
                for (int p = 0; p < 6; ++p) g_fog_prov[p] = fm[p];
                g_fog_prov_ok = true;
            }
        }
        // WHAT THIS FRAME ACTUALLY SAW, kept for the report. "dropped 1920 of
        // 1920" says the distinctness filter refused every frame and not what
        // it was looking at -- and the tally-based report cannot say, because
        // an all-dropped run leaves the tally empty.
        for (int p = 0; p < 6; ++p) {
            g_frame_have[p] = fh[p];
            g_frame_modal[p] = uint16_t(fm[p] & 0x3FFu);
        }
        g_frame_expanded = expanded;
        if (expanded) {
            ++g_fog_frames_used;
            for (int p = 0; p < 6; ++p)
                for (int c = 0; c < kVotes; ++c)
                    if (frame_tally[p].n[c] > 0)
                        for (int k = 0; k < frame_tally[p].n[c]; ++k)
                            vote(tally[p], frame_tally[p].v[c]);
        } else {
            ++g_fog_frames_dropped;
        }
    }

    // THE TALLY PERSISTS ACROSS FRAMES.
    //
    // Per-frame voting only ever learned the fog in a view that was almost
    // entirely fogged. Near the player's base the shrouded area is small and
    // mostly EDGE tiles, so no phase's interior tile could take 80% of that
    // frame's votes and the table never committed -- measured, `fog_learned=0`
    // with `clear_frames=0` for a whole run. Accumulating instead lets a
    // handful of interior tiles per frame converge within a second or two,
    // while the majority and distinctness tests below still reject the
    // partially-expanded frames that per-frame voting was introduced to catch.
    // COMMIT ONLY A CLEAN FRAME.
    //
    // BG2 is not always the finished picture when this runs. One frame was
    // caught holding 064 -- decimal 100, the raw index func_0203A4B4 writes --
    // in every slot, mid-update and before expansion, and a plain mode vote
    // duly "learned" a fog made of six identical tiles. Two guards, both cheap:
    //
    //   * a phase commits only when its modal value takes 80% of that phase's
    //     observations, so a frame that is half raw and half expanded (the one
    //     above scored 389/768) commits nothing at all;
    //   * a round where all six phases agree on ONE tile is rejected outright.
    //     The fog is a 3x2 block of six DIFFERENT tiles -- C0 C1 C2 over
    //     BB BC BD -- so six identical values is never the expanded pattern.
    uint16_t cand[6];
    int candn[6];
    bool have[6] = {false, false, false, false, false, false};
    for (int p = 0; p < 6; ++p) {
        int best = -1, total = 0;
        for (int c = 0; c < kVotes; ++c) {
            total += tally[p].n[c];
            if (tally[p].n[c] > 0 &&
                (best < 0 || tally[p].n[c] > tally[p].n[best]))
                best = c;
        }
        if (best < 0 || tally[p].n[best] < 64) continue;
        // NO SHARE TEST HERE ANY MORE -- the frame filter above does that job.
        //
        // Two share tests were tried at this point and both failed against real
        // data. An 80% majority worked only for per-frame voting; once the tally
        // persisted, the raw index arriving every frame parked itself as a
        // permanent runner-up and the real tile's 62% was rejected forever. A
        // 1.5x plurality then failed too, because the raw index accumulates at
        // very nearly the same rate: the measured ratio was about 1.2.
        //
        // The mistake was filtering at the wrong place. Votes from a frame whose
        // BG2 is not yet expanded should never be counted at all, and the frame
        // filter rejects those on the general property that the fog's six tiles
        // are DISTINCT. What survives to here is already clean, so an absolute
        // count plus the six-distinct check below is enough.
        (void)total;
        cand[p] = tally[p].v[best];
        candn[p] = tally[p].n[best];
        have[p] = true;
    }
    // Replace the table only as a COMPLETE SET OF SIX DISTINCT TILES.
    //
    // Rejecting only "all six identical" was not enough: a frame committed
    // 064 064 064 / 064 0EA 064 -- five copies of the raw index and one stray
    // -- because one slot differed. The expanded fog is a 3x2 block of six
    // different tiles, so requiring all six present and pairwise distinct
    // rejects every partially-expanded frame, and committing them together
    // means the table is never a mix of two frames.
    // A VALUE THAT IS THE MODE OF ALL SIX PHASES IS NOT A FOG TILE -- PURGE IT.
    //
    // The fog metatile's six tiles are distinct by construction, so one value
    // winning every phase cannot be part of it. In practice it is always the
    // same thing: BG2 holds the raw index 100 (0x64) that func_0203A4B4 writes,
    // caught before the expansion pass replaces it, and this code runs early
    // enough in the frame to see it most of the time -- measured at 6036/6400
    // votes per phase on the first report, still 68% by the third.
    //
    // Merely refusing to commit was not enough once the tally became persistent:
    // 0x64 kept winning every phase forever, so the table never committed at all
    // and the widened columns stayed blank. Dropping the offending value from
    // every phase lets the real tile underneath it surface instead.
    {
        bool all_have = true, all_eq = true;
        for (int p = 0; p < 6; ++p) if (!have[p]) all_have = false;
        if (all_have)
            for (int p = 1; p < 6; ++p)
                if ((cand[p] & 0x3FFu) != (cand[0] & 0x3FFu)) all_eq = false;
        if (all_have && all_eq) {
            for (int p = 0; p < 6; ++p) {
                for (int c = 0; c < kVotes; ++c)
                    if (tally[p].n[c] > 0 &&
                        (tally[p].v[c] & 0x3FFu) == (cand[0] & 0x3FFu))
                        tally[p].n[c] = 0;
                have[p] = false;
            }
        }
    }

    bool clean = true;
    for (int p = 0; p < 6 && clean; ++p) {
        if (!have[p]) { clean = false; break; }
        for (int q = 0; q < p; ++q)
            if ((cand[p] & 0x3FFu) == (cand[q] & 0x3FFu)) { clean = false; break; }
    }
    if (clean) {
        // DOES THE MAP'S OWN METATILE AGREE WITH WHAT THE GUEST DREW?
        //
        // The seed reads metatile 100 out of the map's tables; the learner
        // watches the guest draw. They are independent, so a disagreement is
        // the seed being wrong on this tileset -- the one failure mode the
        // all-six-distinct test cannot catch, because a metatile that is not
        // the shroud can still have six distinct tiles.
        //
        // This is the check that generalises across maps without needing every
        // map to be played by hand: the port reports its own disagreement
        // wherever it happens. The learner's values win either way, as they
        // always did, so a wrong seed is corrected rather than kept.
        if (g_fog_seeded_ok) {
            int differ = 0;
            for (int p = 0; p < 6; ++p)
                if ((g_fog_seeded[p] & 0x3FFu) != (cand[p] & 0x3FFu)) ++differ;
            if (differ) {
                std::fprintf(stderr,
                             "[lb_fog] SEED DISAGREES with the guest on %d of "
                             "6 phases: seeded %03X %03X %03X %03X %03X %03X, "
                             "observed %03X %03X %03X %03X %03X %03X -- "
                             "metatile %u is not this tileset's shroud\n",
                             differ,
                             unsigned(g_fog_seeded[0] & 0x3FFu),
                             unsigned(g_fog_seeded[1] & 0x3FFu),
                             unsigned(g_fog_seeded[2] & 0x3FFu),
                             unsigned(g_fog_seeded[3] & 0x3FFu),
                             unsigned(g_fog_seeded[4] & 0x3FFu),
                             unsigned(g_fog_seeded[5] & 0x3FFu),
                             unsigned(cand[0] & 0x3FFu), unsigned(cand[1] & 0x3FFu),
                             unsigned(cand[2] & 0x3FFu), unsigned(cand[3] & 0x3FFu),
                             unsigned(cand[4] & 0x3FFu), unsigned(cand[5] & 0x3FFu),
                             unsigned(kShroudMetatile));
                std::fflush(stderr);
            }
            g_fog_seeded_ok = false;   // report once per battle
        }
        for (int p = 0; p < 6; ++p) {
            g_fog_tiles[p] = cand[p];
            g_fog_known[p] = true;
            g_fog_agree += unsigned(candn[p]);
        }
        fog_note_ready("learned by observation");

        // WHICH METATILE IS THE SHROUD, measured rather than assumed.
        //
        // The first seed attempt took index 100 from a comment and was wrong
        // on two maps out of three. Rather than guess a second index, ask the
        // map: expand every metatile and report the one whose six tiles are
        // what the guest was just observed drawing. If that index is the same
        // across maps and tilesets it is the constant the seed needs, and it
        // will have been derived from the game's own tables instead of prose.
        //
        // One shot per battle, only with LB_FOGIDX set: it walks the whole
        // table and is an investigation, not something every run should
        // pay for.
        if (std::getenv("LB_FOGIDX") && main_ram(g_table_a)) {
            int found = -1;
            for (uint32_t mt = 0; mt < 1024u && found < 0; ++mt) {
                const Metatile m =
                    expand_metatile(g_split, g_table_a, g_table_b,
                                    uint16_t(mt));
                if (!m.ok) continue;
                bool same = true;
                for (int p = 0; p < 6 && same; ++p)
                    if ((m.t[p] & 0x3FFu) != (cand[p] & 0x3FFu)) same = false;
                if (same) found = int(mt);
            }
            std::fprintf(stderr,
                         "[lb_fogidx] shroud tiles %03X %03X %03X %03X %03X "
                         "%03X == metatile %d (split %u)\n",
                         unsigned(cand[0] & 0x3FFu), unsigned(cand[1] & 0x3FFu),
                         unsigned(cand[2] & 0x3FFu), unsigned(cand[3] & 0x3FFu),
                         unsigned(cand[4] & 0x3FFu), unsigned(cand[5] & 0x3FFu),
                         found, unsigned(g_split));
            std::fflush(stderr);
        }
    }
    // WHICH GATE REFUSED, when learning never happens.
    //
    // Learning is all-or-nothing: every one of the six phases needs a
    // candidate AND the six must be distinct, or nothing is committed and the
    // band draws no fog at all -- which is a HOLE IN THE FOG over ground the
    // game is hiding. In one run of the same route this never fired despite
    // 402 shrouded cells being detected, and there was no way to see why:
    // g_fog_frames_used and g_fog_frames_dropped existed and were never
    // printed anywhere, so the failure looked like silence.
    for (int p = 0; p < 6; ++p) {
        g_phase_have[p] = have[p];
        g_phase_cand[p] = cand[p] & 0x3FFu;
        g_phase_n[p] = unsigned(candn[p]);
    }
    g_phase_clean = clean;

    // Report what the scan actually saw. Every previous wrong answer about the
    // fog came from believing a derivation instead of measuring its output, so
    // the base it reads and the count it finds are stated out loud.
    ++g_scan_frames;
    if (scan_logging() && g_scan_frames % 120u == 0u) {
        size_t hidden = 0;
        for (uint8_t v : g_seen) hidden += v;
        // FOG, not "nonzero". kClearTile is nonzero, so the old test counted
        // every entry in the table -- it printed 26329 out of 26329 entries
        // and read like "the table is full of fog" while the band was drawing
        // none. Count entries that actually cloud something, and count the
        // BAND separately from the guest's own cells, since the band is the
        // part under question.
        unsigned ext2_fog = 0, ext2_band_fog = 0, ext2_band = 0;
        for (int r = 0; r < kExtRows; ++r)
            for (int c = 0; c < kExtCols; ++c) {
                const uint16_t v = g_ext2[r][c] & 0x3FFu;
                const bool fogs = (v != 0u && v != kClearTile);
                if (fogs) ++ext2_fog;
                const int trow = r + kExtRowLo, htile = c + kExtLo;
                if (trow < 0 || trow >= 24 || htile < 0 || htile >= 32) {
                    ++ext2_band;
                    if (fogs) ++ext2_band_fog;
                }
            }
        std::fprintf(stderr,
                     "[lb_shroud] bg2 %04X bg3 %04X  fog tiles "
                     "%03X %03X %03X / %03X %03X %03X learned=%d  "
                     "shrouded=%u/768 (bg3marks %u) agree=%u  map %u/%u  cam tile %d,%d  "
                     "ext2 fog=%u (band %u/%u) bg3served=%u edgemasks=%u "
                     "bare=%u\n",
                     unsigned(base), unsigned(base3),
                     g_fog_tiles[0] & 0x3FFu, g_fog_tiles[1] & 0x3FFu,
                     g_fog_tiles[2] & 0x3FFu, g_fog_tiles[3] & 0x3FFu,
                     g_fog_tiles[4] & 0x3FFu, g_fog_tiles[5] & 0x3FFu,
                     fog_learned() ? 1 : 0,
                     unsigned(g_scan_shrouded), unsigned(g_bg3_marked),
                     unsigned(g_fog_agree),
                     unsigned(hidden), unsigned(g_seen.size()),
                     cam_tx, cam_ty, ext2_fog, ext2_band_fog, ext2_band,
                     unsigned(g_served3), g_fog_edge_masks, g_bare_fill);
        // HOW MANY BAND BOUNDARY CELLS GOT A SQUARE EDGE.
        //
        // "interior" is a cell with cloud on some sides only, drawn with the
        // all-sides pattern because no variant was available for it. That is
        // the square edge, and it is the number this fallback exists to move.
        std::fprintf(stderr,
                     "[lb_fogedge] band edge cells: exact %u, cardinal %u, "
                     "interior(square) %u\n",
                     g_edge_exact, g_edge_card, g_edge_interior);
        g_edge_exact = g_edge_card = g_edge_interior = 0;
        // THE DRAWN FOG'S OWN MAP-SPACE BOX.
        //
        // [lb_vis] reports the box of the RECORD and it is fixed while the
        // camera moves, so the record is map-anchored and correctly indexed.
        // That does not prove the same of what is DRAWN: g_ext2 is indexed
        // relative to the camera by construction, and if it is filled from a
        // camera other than the one it is then read with, the cloud slides
        // across the world as the view pans -- the starting uncovered area
        // moving with the view pan.
        //
        // So convert the fogged cells back to map coordinates using the camera
        // the table was FILLED from, and print that box next to the record's.
        // Two boxes that agree mean the fill is faithful; a drawn box that
        // tracks the camera while the record's does not is the fault, located.
        if (std::getenv("LB_FOGBOX")) {
            int fx0 = INT32_MAX, fy0 = INT32_MAX, fx1 = -1, fy1 = -1;
            for (int r = 0; r < kExtRows; ++r)
                for (int c = 0; c < kExtCols; ++c) {
                    const uint16_t v = g_ext2[r][c] & 0x3FFu;
                    if (v == 0u || v == kClearTile) continue;
                    const int abs_row = g_fill_cam_ty + (r + kExtRowLo);
                    const int abs_col = g_fill_cam_tx + (c + kExtLo);
                    const int mx = floor_div(abs_col, 3);
                    const int my = floor_div(abs_row, 2);
                    if (mx < fx0) fx0 = mx;
                    if (mx > fx1) fx1 = mx;
                    if (my < fy0) fy0 = my;
                    if (my > fy1) fy1 = my;
                }
            if (fx1 < 0) { fx0 = fy0 = 0; }
            std::fprintf(stderr,
                         "[lb_fogbox] drawn fog map box %d..%d,%d..%d  "
                         "fillcam %d,%d  livecam %d,%d\n",
                         fx0, fx1, fy0, fy1, g_fill_cam_tx, g_fill_cam_ty,
                         cam_tx, cam_ty);
        }
        // THE EDGE TABLE ITSELF (LB_FOGEDGE=1).
        //
        // "edgemasks=6" says six of 256 neighbour masks have ever been seen in
        // the guest's own 32 columns, and every other mask in the band falls
        // back to the interior pattern -- a cell with cloud on one side drawn
        // as though it had cloud on all of them. That is why band fog edges
        // came out perfectly square where normal fog tiles have rounded,
        // bumpy cloud edges: the square edge IS the fallback.
        //
        // The interior tiles turned out to be constants once they were looked
        // at rather than learned. Printing the mask -> six tiles mapping is the
        // same first step for the edges: if the tile is a function of the mask
        // over a fixed shroud tileset, all 256 can be written down instead of
        // waited for.
        if (std::getenv("LB_FOGEDGE")) {
            for (int mask = 0; mask < 256; ++mask) {
                bool full = true;
                for (int p = 0; p < 6 && full; ++p)
                    if (!g_fog_edge_seen[mask][p]) full = false;
                if (!full) continue;
                std::fprintf(stderr,
                             "[lb_fogedge] mask %02X (N%d NE%d E%d SE%d S%d "
                             "SW%d W%d NW%d) -> %03X %03X %03X %03X %03X %03X\n",
                             mask,
                             (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 4) & 1,
                             (mask >> 7) & 1, (mask >> 6) & 1, (mask >> 5) & 1,
                             (mask >> 3) & 1, (mask >> 0) & 1,
                             g_fog_edge[mask][0] & 0x3FFu,
                             g_fog_edge[mask][1] & 0x3FFu,
                             g_fog_edge[mask][2] & 0x3FFu,
                             g_fog_edge[mask][3] & 0x3FFu,
                             g_fog_edge[mask][4] & 0x3FFu,
                             g_fog_edge[mask][5] & 0x3FFu);
            }
        }
        {
            char ph[128];
            int n = 0;
            for (int p = 0; p < 6; ++p)
                n += g_phase_have[p]
                         ? std::snprintf(ph + n, sizeof ph - (size_t)n,
                                         " %03X(%u)", g_phase_cand[p],
                                         g_phase_n[p])
                         : std::snprintf(ph + n, sizeof ph - (size_t)n, " ---");
            char fr[128];
            int m = 0;
            for (int p = 0; p < 6; ++p)
                m += g_frame_have[p]
                         ? std::snprintf(fr + m, sizeof fr - (size_t)m, " %03X",
                                         g_frame_modal[p])
                         : std::snprintf(fr + m, sizeof fr - (size_t)m, " ---");
            std::fprintf(stderr,
                         "[lb_foglearn] frames used %u dropped %u  clean=%d "
                         " phases%s  | this frame expanded=%d saw%s\n",
                         g_fog_frames_used, g_fog_frames_dropped,
                         g_phase_clean ? 1 : 0, ph,
                         g_frame_expanded ? 1 : 0, fr);
        }
        // WHICH GUARD REJECTED EVERYTHING? edgemasks=0 on its own says the
        // learner found nothing and not why. These counters were added when it
        // first read zero and then never printed, so two runs reported the
        // same uninformative zero.
        std::fprintf(stderr,
                     "[lb_fogedge] cand=%u notshroud=%u margin=%u notfog=%u "
                     "took=%u masks=%u\n",
                     g_le_cand, g_le_notshroud, g_le_margin, g_le_notfog,
                     g_le_took, g_fog_edge_masks);

        // WHAT DOES THE GUEST DRAW THAT THIS DOES NOT?
        //
        // The widened columns are built from the metatile grid alone, leaving
        // out func_02034B44's building stamp -- so a building standing on
        // widened ground would show as bare terrain. Panning a building across
        // the boundary to look at it is awkward (one pan step moves 27 tiles)
        // and unnecessary: the guest's own 32 columns are ground where BOTH
        // answers exist. Recomputing them here and diffing against BG1 measures
        // exactly the gap, with no panning and no layer masking.
        const uint32_t bg1cnt = nds_io_debug_read(9, 0x0400000Au, 16);
        const uint32_t base1 = block + ((bg1cnt >> 8) & 0x1Fu) * 0x800u;
        const uint32_t ta = bus_read_u32(map + 0x338u);
        const uint32_t tb = bus_read_u32(map + 0x33Cu);
        const uint32_t sp = bus_read_u16(kTileSplit + 2u);
        unsigned diff = 0, checked = 0, walls = 0;
        char ex[160];
        int exn = 0;
        for (int ty = 0; ty < 24; ++ty) {
            const int ar = cam_ty + ty;
            const int my = floor_div(ar, 2), sb = floor_mod(ar, 2);
            for (int tx = 0; tx < 32; ++tx) {
                const int ac = cam_tx + tx;
                const Metatile m = metatile_tiles(map, sp, ta, tb,
                                                  floor_div(ac, 3), my,
                                                  map_w, map_h);
                if (!m.ok) continue;
                const uint16_t mine = m.t[sb * 3 + floor_mod(ac, 3)];
                const uint16_t theirs = uint16_t(nds_vram_read_bg(
                    0, base1 + uint32_t(ty * 32 + tx) * 2u, 2));
                ++checked;
                // docs/378: how many of the guest's tiles are the wall block
                // (docs/367), so one line says both whether walls were on
                // screen and whether the replica matched them.
                if ((theirs & 0x3FFu) >= 0x3C8u) ++walls;
                if ((mine & 0x3FFu) == (theirs & 0x3FFu)) continue;
                ++diff;
                if (exn < 110)
                    exn += std::snprintf(ex + exn, sizeof(ex) - size_t(exn),
                                         "(%d,%d %03X!=%03X)", tx, ty,
                                         mine & 0x3FFu, theirs & 0x3FFu);
            }
        }
        std::fprintf(stderr,
                     "[lb_terrain] mine vs guest BG1: %u/%u differ, %u wall "
                     "tiles on screen %s\n",
                     diff, checked, walls, exn ? ex : "");

        // HOW MANY TILE ROWS DOES THE GUEST ACTUALLY MAINTAIN?
        //
        // The serving rule no longer needs a number -- see the note on
        // kGuestScanlines -- but this is the measurement the rule was derived
        // from and it stays, because it is the only thing that would notice the
        // premise changing. A maintained row is exact against this file's own
        // reconstruction; a stale one is not.
        //
        // Reasoning has been wrong about this file before, so it is MEASURED
        // every report: the same recomputation as above, row by row over the
        // whole 32-row screen block. Rows the guest maintains read 0 different;
        // the first row that disagrees wholesale is where its window ends.
        // BG*VOFS is printed beside it because that is the quantity the 26
        // comes from.
        {
            char rl[220];
            int rn = 0;
            int last_clean = -1;
            for (int ty = 0; ty < 32; ++ty) {
                const int ar = cam_ty + ty;
                const int my = floor_div(ar, 2), sb = floor_mod(ar, 2);
                int d = 0, c = 0;
                for (int tx = 0; tx < 32; ++tx) {
                    const int ac = cam_tx + tx;
                    const Metatile m = metatile_tiles(map, sp, ta, tb,
                                                      floor_div(ac, 3), my,
                                                      map_w, map_h);
                    if (!m.ok) continue;
                    const uint16_t mine = m.t[sb * 3 + floor_mod(ac, 3)];
                    const uint16_t theirs = uint16_t(nds_vram_read_bg(
                        0, base1 + uint32_t(ty * 32 + tx) * 2u, 2));
                    ++c;
                    if ((mine & 0x3FFu) != (theirs & 0x3FFu)) ++d;
                }
                if (c > 0 && d == 0) last_clean = ty;
                if (rn < 200)
                    rn += std::snprintf(rl + rn, sizeof(rl) - size_t(rn),
                                        "%d", c == 0 ? 9 : (d == 0 ? 0
                                              : (d < 10 ? 1 : 2)));
            }
            std::fprintf(stderr,
                         "[lb_rows] guest BG1 row agreement (0=exact 1=few "
                         "2=many 9=offmap): %s  last exact row %d\n",
                         rl, last_clean);

            // THE OVERLAY LAYERS, measured the same way but with a different
            // yardstick, because there is no reconstruction to diff against.
            //
            // What a maintained row of BG2 or BG3 looks like is known exactly:
            // the layer's own CLEAR tile, or -- for BG2 -- one of the fog tiles
            // this file learns from the guest. A row the guest keeps up to date
            // is made entirely of those. A row it never writes is whatever was
            // last left in the screen block, which is sprite and font tiles
            // from another scene and lands outside that small set almost
            // everywhere. That is enough to see where each window ends, which
            // is the only thing the row origin needs to know.
            char pl2[64], pl3[64];
            int pn2 = 0, pn3 = 0;
            int last2 = -1, last3 = -1;
            for (int ty = 0; ty < 32; ++ty) {
                int odd2 = 0, odd3 = 0;
                for (int tx = 0; tx < 32; ++tx) {
                    const uint32_t off = uint32_t(ty * 32 + tx) * 2u;
                    const uint16_t v2 = uint16_t(
                        nds_vram_read_bg(0, base + off, 2)) & 0x3FFu;
                    const uint16_t v3 = uint16_t(
                        nds_vram_read_bg(0, base3 + off, 2)) & 0x3FFu;
                    bool ok2 = (v2 == kClearTile);
                    for (int p = 0; p < 6 && !ok2; ++p)
                        if (g_fog_known[p] && v2 == (g_fog_tiles[p] & 0x3FFu))
                            ok2 = true;
                    // The edge variants count too; they are as much the fog as
                    // the interior pattern is.
                    if (!ok2 && v2 >= 0x0B0u && v2 <= 0x0C8u) ok2 = true;
                    if (!ok2) ++odd2;
                    if (v3 != kShroudTile && v3 != 0u) ++odd3;
                }
                if (odd2 == 0) last2 = ty;
                if (odd3 == 0) last3 = ty;
                if (pn2 < 40)
                    pn2 += std::snprintf(pl2 + pn2, sizeof(pl2) - size_t(pn2),
                                         "%d", odd2 == 0 ? 0 : (odd2 < 10 ? 1 : 2));
                if (pn3 < 40)
                    pn3 += std::snprintf(pl3 + pn3, sizeof(pl3) - size_t(pn3),
                                         "%d", odd3 == 0 ? 0 : (odd3 < 10 ? 1 : 2));
            }
            std::fprintf(stderr,
                         "[lb_rows] BG2 %s last clean %d  "
                         "BG3 %s last clean %d\n",
                         pl2, last2, pl3, last3);
        }

        // DOES THE GRID ONLY EVER GROW?
        //
        // This separates "explored" from "currently visible": ground once
        // explored is explored forever, so an explored record's count is
        // monotonic, while current visibility rises and falls as units move.
        //
        // "THE PREFERENCE IT ENCODED WAS BACKWARDS: the game clouds everything
        // not CURRENTLY visible, so the grid that shrinks is the one whose
        // shape matches the fog on screen." THAT IS RETRACTED. It said the
        // opposite of docs/206 -- "ground once explored is explored forever,
        // so an explored record's revealed count can only rise" -- and the two
        // have sat in the tree contradicting each other, with everything about
        // which buffer to adopt following from whichever one is true.
        //
        // MEASURED, and it is not close. g_seen is the cartridge's own BG2
        // verdict for a cell the guest window is drawing right now, so a cell
        // going REVEALED -> SHROUDED while still on screen is the game
        // re-clouding ground it had shown. Counted over a full run with a pan
        // across a frontier:
        //
        //     reshroud 9      unshroud 777
        //
        // 777 cells revealed as the player explored, against 9 going back --
        // 1.2%, and consistent with tile-boundary noise at the window edge.
        // This game does not re-shroud. docs/206 is right.
        //
        // So monotonicity IS a real discriminator and is enforced on the
        // ADOPTION side, where a candidate whose whole-map revealed count
        // falls is rejected outright. It is measured here as well, and the
        // shrink branch below keeps its agreement test for the case where a
        // buffer drifts rather than jumps.
        if (g_vis_base) {
            unsigned revealed = 0;
            // THE REVEALED REGION'S MAP-SPACE BOUNDING BOX.
            //
            // The count alone cannot tell a map-space record from a
            // camera-relative one: a window of revealed ground sliding across
            // the map keeps the same area and reports "same" every frame. The
            // BOX is what separates them. Explored ground is fixed in the
            // world, so its box may only grow; a box whose edges TRACK THE
            // CAMERA is the starting uncovered area moving with the view
            // pan.
            //
            // Printed beside cam so the two can be read off one line, which is
            // the mistake the agreement figure made: every 100% reading was
            // taken with a STATIC camera, and a static camera cannot tell the
            // two cases apart at all.
            int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = -1, by1 = -1;
            for (int my = 0; my < map_h; ++my)
                for (int mx = 0; mx < map_w; ++mx)
                    if (vis_revealed(mx, my)) {
                        ++revealed;
                        if (mx < bx0) bx0 = mx;
                        if (mx > bx1) bx1 = mx;
                        if (my < by0) by0 = my;
                        if (my > by1) by1 = my;
                    }
            if (bx1 < 0) { bx0 = by0 = 0; }
            // ISOLATED REVEALED METATILES IN THE RECORD ITSELF.
            //
            // The band showing terrain where the guest's own window shows only
            // fog looks like a fault and need not be one: the explored region
            // on this map is the top-left corner, so the LEFT and ABOVE bands
            // point back at explored ground while RIGHT and BELOW point into
            // the dark. The service rates match that geography exactly.
            //
            // What would NOT be explained by geography is an isolated square
            // of ground in a sea of cloud -- unless the record holds isolated
            // revealed metatiles, in which case a DS would draw the same thing
            // and the port is being faithful. A single scout's path leaves
            // exactly that. So count them: revealed cells whose four cardinal
            // neighbours are all hidden.
            unsigned lone = 0;
            if (std::getenv("LB_FOGLONE"))
            for (int my = 0; my < map_h; ++my)
                for (int mx = 0; mx < map_w; ++mx) {
                    if (!vis_revealed(mx, my)) continue;
                    if (vis_revealed(mx - 1, my) || vis_revealed(mx + 1, my) ||
                        vis_revealed(mx, my - 1) || vis_revealed(mx, my + 1))
                        continue;
                    ++lone;
                }
            if (lone)
                std::fprintf(stderr,
                             "[lb_lone] %u revealed metatile(s) in the record "
                             "have NO revealed cardinal neighbour -- isolated "
                             "patches are the cartridge's own record, not the "
                             "band's doing\n", lone);
            const char* trend = "first";
            if (g_vis_last_count) {
                if (revealed > g_vis_last_count) trend = "grew";
                else if (revealed < g_vis_last_count) {
                    // A SHRINK NO LONGER DISQUALIFIES ON ITS OWN.
                    //
                    // The original rule was: ground once explored is explored
                    // forever, so an EXPLORED record's count can only rise; a
                    // shrink therefore proves the candidate is "currently
                    // visible" instead, and monotonicity separates the two
                    // where agreement cannot.
                    //
                    // Both halves of that are true and the conclusion still
                    // does not follow, because CURRENTLY VISIBLE IS THE RECORD
                    // THIS FILE WANTS. The game clouds everything that is not
                    // currently visible, not merely the never-explored, so the
                    // grid that shrinks as units move is precisely the one
                    // whose shape matches the fog on screen.
                    //
                    // WHAT THE OLD RULE COST, measured in the run that
                    // reproduced the defect: it disqualified a buffer
                    // that agreed with the guest's OWN DRAWN FOG on 127 of 132
                    // sampled cells (96.2%). With nothing adopted the scan
                    // falls back to g_seen, the window-derived cache, which
                    // shrouds every cell the camera has not personally
                    // covered -- and on a 2022x1142 window that painted a
                    // PERFECT RECTANGLE of clear ground exactly the guest's own
                    // 24 rows tall with solid cloud around it: fog and units
                    // misbehaving at different window sizes, with perfectly
                    // square fog.
                    //
                    // So the guest's own pixels decide, and a shrink is fatal
                    // only when agreement with them is poor. THE THRESHOLD IS
                    // FITTED TO TWO POINTS and that is worth stating plainly:
                    // the buffer this must keep agrees 96.2%, and the wrong
                    // buffer the old comment describes agreed 92.4%. 95% sits
                    // between them. If a third candidate ever lands in that
                    // gap this needs a real discriminator -- most likely
                    // agreement sustained over several samples, since a wrong
                    // buffer should drift out of agreement while a right one
                    // does not.
                    const bool agrees_well =
                        g_vis_checked >= 32u &&
                        (g_vis_checked - g_vis_wrong) * 100u >=
                            g_vis_checked * 95u;
                    if (agrees_well) {
                        trend = "shrank, KEPT (agrees with the guest's fog)";
                    } else {
                    trend = "SHRANK -- disqualified";
                    ++g_vis_shrinks;
                    if (g_vis_bad_n < 16) g_vis_bad[g_vis_bad_n++] = g_vis_base;
                    std::fprintf(stderr,
                                 "[lb_vis] DISQUALIFIED %08X: revealed fell "
                                 "%u -> %u (agreement %u/%u below 95%%)\n",
                                 g_vis_base, g_vis_last_count, revealed,
                                 g_vis_checked - g_vis_wrong, g_vis_checked);
                    g_vis_base = 0u;
                    g_vis_is_bits = false;      // see the note at the other drop
                    g_vis_rowtab = 0u;
                    g_cand_n = 0;
                    g_vis_passes = 0;
                    g_vis_last_count = 0;
                    // Rearm the search here too, for the reason given at the
                    // other drop site: losing the grid used to be permanent.
                    g_findfog_done = false;
                    std::fflush(stderr);
                    }
                } else {
                    trend = "same";
                }
            }
            if (g_vis_base) g_vis_last_count = revealed;
            std::fprintf(stderr,
                         "[lb_vis] %08X revealed %u/%d  %s  box %d..%d,%d..%d  "
                         "shrinks %u  "
                         "agrees with guest fog %u/%u (%.1f%%)  "
                         "cam %d,%d phase %d,%d  reshroud %u unshroud %u\n",
                         g_vis_base, revealed, map_w * map_h, trend,
                         bx0, bx1, by0, by1,
                         g_vis_shrinks,
                         g_vis_checked - g_vis_wrong, g_vis_checked,
                         g_vis_checked
                             ? 100.0 * double(g_vis_checked - g_vis_wrong) /
                                   double(g_vis_checked)
                             : 0.0,
                         // THE METATILE PHASE OF THE CAMERA, which is the one
                         // thing that would make agreement CAMERA-DEPENDENT
                         // without the buffer being wrong.
                         //
                         // A metatile is 3 tiles across and 2 down, and the
                         // grid is indexed floor_div(cam_tx + tx, 3). If the
                         // grid's own origin is not aligned to absolute tile
                         // column 0, agreement collapses whenever cam_tx % 3
                         // lands on the offending residue and is fine
                         // otherwise -- which is exactly the shape of the
                         // swing seen here, 98.5% one frame and 72.7% another.
                         // Logging the residue is what separates "the indexing
                         // is wrong" from "this is the wrong record", and
                         // docs/206 guessed the former without ever checking.
                         cam_tx, cam_ty,
                         floor_mod(cam_tx, 3), floor_mod(cam_ty, 2),
                         g_reshroud, g_unshroud);
        }

        // THE WIDENED COLUMNS THEMSELVES.
        //
        // The diff above only covers the guest's own 32 columns, so it says
        // nothing about the ones this file invents -- and a user capture shows
        // the LEFT widened band drawing structured but wrong terrain while the
        // right band looks correct. Report the state that decides what those
        // columns draw: whether the tables are valid and being offered, how
        // many entries fell off the map (which currently yields tile 0, not
        // black), and the absolute column range the bands cover.
        unsigned offmap_l = 0, offmap_r = 0, zero_l = 0, zero_r = 0;
        for (int trow = 0; trow < 24; ++trow) {
            for (int h = kExtLo; h <= kExtHi; ++h) {
                if (h >= 0 && h < 32) continue;
                const int ac = cam_tx + h;
                const uint16_t v = g_ext[trow - kExtRowLo][h - kExtLo];
                const bool off = (ac < 0 || ac >= map_w * 3);
                if (h < 0) { offmap_l += off; zero_l += (v == 0); }
                else       { offmap_r += off; zero_r += (v == 0); }
            }
        }
        {   // What the fog tally is actually holding, per phase.
            char tb[220]; int tn = 0;
            for (int p = 0; p < 6 && tn < 190; ++p) {
                int best = -1, tot = 0;
                for (int c = 0; c < kVotes; ++c) {
                    tot += tally[p].n[c];
                    if (tally[p].n[c] > 0 &&
                        (best < 0 || tally[p].n[c] > tally[p].n[best])) best = c;
                }
                tn += std::snprintf(tb + tn, sizeof(tb) - size_t(tn),
                                    "%03X:%d/%d ",
                                    best >= 0 ? (tally[p].v[best] & 0x3FFu) : 0xFFFu,
                                    best >= 0 ? tally[p].n[best] : 0, tot);
            }
            std::fprintf(stderr, "[lb_tally] %s\n", tb);
        }
        // THE FIELD'S LAYER ORDER, stated rather than assumed.
        //
        // It decides whether a HUD element this port MOVES into a fogged band
        // can still be seen: the in-field HUD is 3D geometry on BG0, so if the
        // shroud on BG2 has the lower priority number it is drawn in front and
        // an anchored button disappears behind the cloud. Two readings of the
        // same capture were consistent with either answer, which is exactly
        // when a number beats an inference.
        std::fprintf(stderr,
                     "[lb_prio] BG0 %u  BG1 %u  BG2 %u  BG3 %u  "
                     "(0 = frontmost)\n",
                     unsigned(nds_io_debug_read(9, 0x04000008u, 16) & 3u),
                     unsigned(bg1cnt & 3u),
                     unsigned(nds_io_debug_read(9, 0x0400000Cu, 16) & 3u),
                     unsigned(nds_io_debug_read(9, 0x0400000Eu, 16) & 3u));
        std::fprintf(stderr,
                     "[lb_wide] blanked=%u (cold-start flat band)  "
                     "declined_out_of_table=%u (MUST be 0; tables cover "
                     "cols %d..%d rows %d..%d)\n",
                     g_blanked, g_declined_bounds, kExtLo, kExtHi,
                     kExtRowLo, kExtRowHi);
        g_blanked = 0;
        g_declined_bounds = 0;
        std::fprintf(stderr,
                     "[lb_wide] ext_valid=%d extends1=%d fog_learned=%d "
                     "clear_frames=%u  cam_tx=%d cols %d..%d (map %d tiles)  "
                     "left: offmap=%u zero=%u  right: offmap=%u zero=%u\n",
                     g_ext_valid ? 1 : 0, lb_widemap_extends(1),
                     fog_learned() ? 1 : 0, g_clear_frames,
                     cam_tx, cam_tx + kExtLo, cam_tx + kExtHi, map_w * 3,
                     offmap_l, zero_l, offmap_r, zero_r);

        // WHERE the zeros are, and whether the model was rejected.
        //
        // "zero=161" cannot be acted on: 161 cells is about seven zero COLUMNS
        // or about one and a half zero ROWS, and those are completely different
        // pictures -- a green strip beside the field, or a green stripe across
        // it. The capture shows a strip, so print the distribution and
        // stop inferring it.
        //
        // g_model_bad_frames has been counted since the model check was written
        // and never once printed. choose_camera demands best_bad == 0 over 48
        // sampled cells, and when it fails g_model_ok goes false and
        // lb_widemap_tile blanks EVERY cell it serves to tile 0 -- which is
        // flat green. A counter that decides the whole band and is invisible is
        // no better than not having it.
        {
            char zb[260]; int zn = 0;
            for (int h = 32; h <= 47 && zn < 230; ++h) {
                unsigned z = 0;
                for (int trow = 0; trow < 24; ++trow)
                    if (g_ext[trow - kExtRowLo][h - kExtLo] == 0u) ++z;
                zn += std::snprintf(zb + zn, sizeof(zb) - size_t(zn), "%u ", z);
            }
            char rb[260]; int rn = 0;
            for (int trow = 0; trow < 24 && rn < 230; trow += 2) {
                unsigned z = 0;
                for (int h = 32; h <= 47; ++h)
                    if (g_ext[trow - kExtRowLo][h - kExtLo] == 0u) ++z;
                rn += std::snprintf(rb + rn, sizeof(rb) - size_t(rn), "%u ", z);
            }
            char hb[160]; int hn = 0;
            for (int i = 1; i <= 8 && hn < 130; ++i)
                hn += std::snprintf(hb + hn, sizeof(hb) - size_t(hn),
                                    "%d:%u ", i, g_model_bad_hist[i]);
            std::snprintf(hb + hn, sizeof(hb) - size_t(hn),
                          "9+:%u", g_model_bad_hist[9]);
            std::fprintf(stderr,
                         "[lb_wideR] model_bad_frames=%u (after-first-ok %u) "
                         "model_ok=%d badmax=%d bad-of-48 hist %s  "
                         "right zeros per col h32..47: %s | per row (every 2nd, "
                         "of 16): %s\n",
                         g_model_bad_frames, g_model_bad_after_ok,
                         g_model_ok ? 1 : 0, g_model_bad_max, hb, zb, rb);

            // THE THREE TABLES ACROSS ONE ROW, left band to right band.
            //
            // The layer dump says BG2 -- the fog layer -- wins ~100% of the
            // RIGHT band while BG1 wins the LEFT, with the guest's own window a
            // healthy mix. Both bands are served kClearTile from g_ext2 when
            // the fog tiles are unlearned, so the same value cannot explain
            // both outcomes and one of my assumptions about these tables is
            // wrong. Print them instead of reasoning about them.
            //
            // h is the hardware tile column: negative is the left band, 0..31
            // is the guest's own window, 32+ is the right band.
            {
                const int r = 12;                     // mid screen
                char e1[400]; int n1 = 0;
                char e2[400]; int n2 = 0;
                char e3[400]; int n3 = 0;
                for (int h = -10; h <= 42 && n1 < 360; ++h) {
                    const char* sep = (h == 0 || h == 32) ? "| " : "";
                    n1 += std::snprintf(e1 + n1, sizeof(e1) - size_t(n1),
                                        "%s%03X ", sep,
                                        g_ext[r - kExtRowLo][h - kExtLo] & 0x3FFu);
                    // FULL 16-BIT ENTRY, not the tile index alone. Masking with
                    // 0x3FF hid the palette and flip bits, and the guest draws
                    // the SAME indices as detailed rock where the band draws
                    // flat green -- same tile, different pixels, which is what
                    // an attribute mismatch looks like.
                    n2 += std::snprintf(e2 + n2, sizeof(e2) - size_t(n2),
                                        "%s%04X ", sep,
                                        g_ext2[r - kExtRowLo][h - kExtLo]);
                    n3 += std::snprintf(e3 + n3, sizeof(e3) - size_t(n3),
                                        "%s%03X ", sep,
                                        g_ext3[r - kExtRowLo][h - kExtLo] & 0x3FFu);
                }
                std::fprintf(stderr, "[lb_row%d] ext  (terrain): %s\n", r, e1);
                std::fprintf(stderr, "[lb_row%d] ext2 (fog)    : %s\n", r, e2);
                std::fprintf(stderr, "[lb_row%d] ext3 (shroud) : %s\n", r, e3);
                std::fprintf(stderr,
                             "[lb_row%d] clear=%03X shroudtile=%03X "
                             "ext2_valid=%d ext3_valid=%d extends2=%d\n",
                             r, unsigned(kClearTile) & 0x3FFu,
                             unsigned(kShroudTile) & 0x3FFu,
                             g_ext2_valid ? 1 : 0, g_ext3_valid ? 1 : 0,
                             lb_widemap_extends(2));

                // GROUND TRUTH: what the GUEST'S OWN BG2/BG3 hold for the same
                // row and its own 32 columns.
                //
                // Our tables say columns ~23..31 are shrouded, but those
                // columns are drawn by the cartridge, and a capture of them
                // shows detailed rock there rather than fog. Both cannot be
                // true. This reads the guest's tilemaps directly so the model
                // can be compared against the picture instead of against
                // itself -- the [lb_vis] "agrees with guest fog 127/132" test
                // samples only 132 cells, nearly all of them near the camera
                // and nearly all in one state, so it cannot speak for the band.
                {
                    const uint32_t blk = ((g_dispcnt >> 27) & 7u) * 0x10000u;
                    const uint32_t c2 = nds_io_debug_read(9, 0x0400000Cu, 16);
                    const uint32_t c3 = nds_io_debug_read(9, 0x0400000Eu, 16);
                    const uint32_t b2 = blk + ((c2 >> 8) & 0x1Fu) * 0x800u;
                    const uint32_t b3 = blk + ((c3 >> 8) & 0x1Fu) * 0x800u;
                    char g2[300]; int m2 = 0;
                    char g3[300]; int m3 = 0;
                    for (int h = 0; h < 32 && m2 < 270; ++h) {
                        const uint32_t off = uint32_t(r * 32 + h) * 2u;
                        m2 += std::snprintf(g2 + m2, sizeof(g2) - size_t(m2),
                                            "%04X ",
                                            unsigned(nds_vram_read_bg(0, b2 + off, 2)) & 0xFFFFu);
                        // FULL 16 BITS. The band substitutes a bare 0x1F for
                        // BG3 and it decodes to 0x9665 -- (5,19,5) in BGR555,
                        // the same green as the terrain -- while the cartridge
                        // writes 0x1F in these very columns and draws nothing.
                        // Same index, opposite result, so whatever separates
                        // them is in the bits I was masking off.
                        m3 += std::snprintf(g3 + m3, sizeof(g3) - size_t(m3),
                                            "%04X ",
                                            unsigned(nds_vram_read_bg(0, b3 + off, 2)) & 0xFFFFu);
                    }
                    std::fprintf(stderr, "[lb_row%d] GUEST bg2 h0..31 : %s\n", r, g2);
                    std::fprintf(stderr, "[lb_row%d] GUEST bg3 h0..31 : %s\n", r, g3);
                }
            }
        }

        std::fflush(stderr);
    }
    g_scan_shrouded = 0;
    g_bg3_marked = 0;
}

// ON BY DEFAULT, because it is not a diagnostic -- it is the feature.
//
// This defaulted to OFF, and everything in this file was written and measured
// with LB_WIDEMAP=1 set by hand, so the DEFAULT experience was never the one
// being looked at. Measured without it, at 1920x1080: the field is pillarboxed
// to roughly the native width with black bars either side, and a band of stale
// tiles runs across the top -- exactly the two things this file exists to
// prevent.
//
// Adaptive window size is ONE toggle whose off position is pure-vanilla
// rendering. A second, hidden, default-off switch in front of half the
// feature would break that. Enhanced is the toggle; this follows it.
//
// LB_WIDEMAP=0 still turns it off, for an A/B against vanilla without leaving
// Enhanced.
int stage() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("LB_WIDEMAP");
        cached = v ? std::atoi(v) : 1;
    }
    return cached;
}

}  // namespace

// One widened tile entry, or 0 if this cell is not one we supply.
//
// `htile` is the HARDWARE tile column and `trow` the HARDWARE tile row, both
// signed. The guest's own cell -- column 0..31 AND row 0..25 -- is never served
// from here, so the game's picture is bit-for-bit untouched. Everything outside
// that rectangle is ours, which now includes the NATIVE columns of a row above
// or below the guest's window: a widened row crosses straight through the
// middle of the native picture, unlike a widened column.
extern "C" int lb_widemap_tile(int htile, int trow, int gy, uint16_t* out) {
    if (!g_ext_valid) return 0;
    if (guest_cell(htile, gy)) return 0;
    if (ext_out_of_range(htile, trow)) { ++g_declined_bounds; return 0; }
    // The model must describe the picture -- see g_model_ok. Blank, not
    // declined: a declined cell falls through to the hardware wrap.
    // HOLD THE LAST GOOD TABLE, DO NOT BLANK.
    //
    // Symptom: the screen flickered semi-frequently -- temporarily
    // transparent shroud, a yellow flash, other graphical flickering.
    //
    // choose_camera demands 0 of 48 sampled cells disagree, with no
    // tolerance. During a pan the guest's tilemap genuinely IS a mix of
    // two cameras -- the streamer rewrites columns as the camera crosses
    // a tile -- so no candidate matches all 48 and the model is rejected
    // for that frame. Blanking then paints the whole band tile 0 for one
    // frame: transparent on the fog layer (the terrain shows straight
    // through) and a flat solid tile on terrain (the flash).
    //
    // Declining is NOT the alternative -- the note above is right that a
    // declined cell falls through to the hardware wrap and draws garbage.
    // The third option is to serve what is already in the table, which is
    // the last fill made with a camera the model COULD explain. Slightly
    // stale for one frame beats a full-band flash, and `g_ext_valid`
    // above already guarantees the table has been filled at least once,
    // so there is no uninitialised case to fall into.
    //
    // LB_MODELBLANK=1 restores the blank for A/B.
    if (!g_model_ok) {
        static const bool blank = [] {
            const char* v = std::getenv("LB_MODELBLANK");
            return v && v[0] == '1';
        }();
        if (blank) { *out = 0u; return 1; }
    }
    // Serving SOMETHING for every widened column is the point: a column this
    // declines falls through to the hardware wrap and draws garbage. Where the
    // record says shrouded and the fog tiles are not known yet, that something
    // is tile 0 -- blank -- rather than the terrain underneath.
    // ...unless there is demonstrably no shroud in play at all.
    //
    // This branch blanks a column the map-space record calls unexplored, and
    // that record is the one docs/206 parked as unreliable. With the
    // reveal-map cheat on it got the RIGHT extension wrong in exactly the way
    // a parked record would: `[lb_seen]` read
    // `........|................|################` -- native columns clear,
    // right sixteen shrouded -- so the whole right band was painted with tile
    // 0 and came out as flat green next to fully detailed terrain, while the
    // left band was correct. Entities still drew over it, which is what made
    // it read as "the extension works but the ground is missing".
    //
    // g_clear_frames counts consecutive frames with nothing shrouded anywhere
    // in the guest's own window. It was demoted to a diagnostic because near
    // the player's base it sits at 0 -- every frame has fog somewhere -- so it
    // could never open a gate. That is an argument against trusting it when it
    // is SMALL, not when it is large: a full second with zero shrouded tiles
    // on screen means the shroud is not running, and a record that then claims
    // an off-screen column is hidden is contradicting a direct measurement.
    // One second, so a single clear frame mid-battle cannot trip it.
    if (!fog_learned() && !g_fog_prov_ok && g_ext3_valid && g_clear_frames < 60u &&
        g_ext3[trow - kExtRowLo][htile - kExtLo] == kShroudTile) {
        // COUNT IT. The sides were seen breaking at times and becoming
        // inconsistent, and the fog sometimes failed to show, with
        // captures showing the band FLAT while the native columns beside
        // it show detailed terrain, and a hard edge exactly at the native
        // boundary.
        //
        // This branch is the only thing in this file that puts a flat tile
        // in the band, and it is live precisely while the fog tiles are
        // not yet learned -- which is the first seconds of every match.
        // "Sometimes" and "at the start" look the same from the outside,
        // so the way to tell them apart is to count how many cells it
        // blanks and watch that number fall to zero.
        ++g_blanked;
        *out = 0u;
        return 1;
    }
    *out = g_ext[trow - kExtRowLo][htile - kExtLo];
    // Remember what the BAND was served, so the cross-check in audit_terrain
    // can grade it against the cartridge's own tilemap once the camera brings
    // that ground inside the guest's window. Only band cells: the guest's own
    // columns never reach here (guest_cell declines them above).
    // Vertical first: a row above or below the guest's window crosses straight
    // through the native columns, so those cells have an ordinary column and a
    // left/right test would mislabel every one of them.
    // AUDIT BOOKKEEPING ONLY, and behind the audit's own flag. This runs on
    // every compositor worker for every 8-px tile of the widened terrain, and
    // the unconditional write below made every worker store into one global --
    // a cache line bounced between cores tens of thousands of times a frame to
    // feed a record that is off unless LB_MAPAUDIT is set (lb_widemap_fog
    // was already gated this way).
    if (g_band_truth_on) {
        g_band_kind = (trow < 0)    ? kBandAbove
                    : (trow >= 24)  ? kBandBelow
                    : (htile >= 32) ? kBandRight
                                    : kBandLeft;
        band_record(g_fill_cam_ty + trow, g_fill_cam_tx + htile, *out);
    }
    return 1;
}

// Is this METATILE shrouded? Off the map counts as shrouded, which is what the
// widened columns already assume and what the cartridge draws at a map edge.
//
// The shroud is a per-metatile property -- the whole 3x2 is fogged or none of
// it is -- so the top-left tile's verdict speaks for the cell.
// Has the scan actually read this metatile, as opposed to defaulting it?
// Same convention as metatile_shrouded: the top-left tile speaks for the cell.
bool metatile_observed(int mx, int my) {
    const int ar = my * 2, ac = mx * 3;
    if (ar < 0 || ac < 0 || ar >= g_seen_h || ac >= g_seen_w) return false;
    if (g_obs.empty()) return false;
    return g_obs[size_t(ar) * size_t(g_seen_w) + size_t(ac)] != 0u;
}

// Are all eight neighbours of this metatile real readings? The mask is only
// meaningful if they are -- see g_obs.
bool mask_is_observed(int mx, int my) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dy || dx) && !metatile_observed(mx + dx, my + dy))
                return false;
    return true;
}

bool metatile_shrouded(int mx, int my) {
    const int ar = my * 2, ac = mx * 3;
    if (ar < 0 || ac < 0 || ar >= g_seen_h || ac >= g_seen_w) return true;
    return g_seen[size_t(ar) * size_t(g_seen_w) + size_t(ac)] != 0u;
}

// THE MASK MUST ASK THE SAME AUTHORITY THE FILL ASKS.
//
// Symptom: square-line edges on the horizontal fog, flat where the vanilla
// renderer's edge is not.
//
// Measured, one composited frame, the cartridge's own 256 columns against the
// band, walking the BG1/BG2 layer-win boundary row by row:
//
//     CARTRIDGE   280 rows  mean vertical run  2.04  steps 0:143 1:93 2:27 4:7
//     PORT band   368 rows  mean vertical run 28.31  steps 0:355 24:8 48:4
//
// The cartridge's outline moves 1, 2 or 4 px every couple of rows -- sub-TILE
// detail, which can only come from a partially transparent edge tile's art.
// The band's moves only in whole metatiles (24 px) and holds the same column
// for up to 112 rows. Whole metatiles flip between solid fog and clear with no
// transition between them, which is the flat edge.
//
// The cause is that this port answers "is it fogged" and "which edge variant"
// from DIFFERENT sources:
//
//     fill_terrain's `s`   hidden_metatile()   -> g_vis_bits, the GAME'S OWN
//                                                 fog record (docs/247), which
//                                                 covers the whole map
//     this mask            metatile_shrouded() -> g_seen, this port's cache of
//                                                 what the guest's own window
//                                                 has actually been shown
//
// In the band those disagree by construction. g_seen defaults to SHROUDED for
// ground the guest's window has never covered, so at the revealed region's
// edge the record correctly clears a metatile while every neighbour still
// reads shrouded -- mask 0xFF, interior tile, no fade. A clear tile hard
// against a solid one is exactly a 24 px step with no raggedness.
//
// So ask the record, which is the authority the fill already trusts and the
// one the cartridge itself is drawing from. Fall back to the cache when the
// record has not been located yet: hidden_metatile answers `false` for an
// empty g_vis_bits, and taking that at face value would report every neighbour
// REVEALED and make every cell an edge cell.
//
// LB_FOGMASKREC=0 restores the cache, which is the A/B.
//
// NOT CHANGED: learn_fog_edges' own metatile_shrouded test. That runs over the
// guest's own 32 columns, where the cache is accurate by construction and is
// what the learned variants are graded against.
uint8_t metatile_mask(int mx, int my) {
    static const bool use_record = [] {
        const char* v = std::getenv("LB_FOGMASKREC");
        return !(v && v[0] == '0');
    }();
    const bool have_record = use_record && !g_vis_bits.empty();
    uint8_t m = 0u;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dy && !dx) continue;
            const bool shrouded = have_record
                                      ? hidden_metatile(mx + dx, my + dy)
                                      : metatile_shrouded(mx + dx, my + dy);
            if (shrouded)
                m = uint8_t(m | (1u << neighbour_bit(dy, dx)));
        }
    return m;
}

// Learn which tile the cartridge draws at each position of a shrouded metatile,
// for each neighbour mask. Only from the guest's own 32 columns -- the widened
// ones are this port's own output and learning from them would be circular.
void learn_fog_edges(int cam_tx, int cam_ty) {
    // NOT `if (!g_bg2_base)`. BG2's screen base in this game is genuinely
    // ZERO -- docs/203 records the fog layer at base 0 -> 0x0000 -- so using 0
    // as "not set yet" rejected every frame, and the learner reported
    // edgemasks=0 through two runs without ever reaching its own counters.
    // g_seen_w is the honest liveness test: it is only non-zero once
    // scan_shroud has sized the record.
    if (g_seen_w <= 0) return;
    g_le_cand = g_le_notshroud = g_le_margin = g_le_notfog = g_le_took = 0;
    // Metatiles wholly inside the guest's window. The window is 32x24 tiles
    // and a metatile is 3x2, so the last partial column is skipped rather than
    // learned from half a cell.
    for (int ty = 0; ty + 1 < 24; ty += 1) {
        const int ar = cam_ty + ty;
        if (floor_mod(ar, 2) != 0) continue;         // top row of the metatile
        const int my = floor_div(ar, 2);
        for (int tx = 0; tx + 2 < 32; ++tx) {
            const int ac = cam_tx + tx;
            if (floor_mod(ac, 3) != 0) continue;     // left column of it
            const int mx = floor_div(ac, 3);
            ++g_le_cand;
            if (!metatile_shrouded(mx, my)) { ++g_le_notshroud; continue; }
            // Every neighbour's verdict must be a real reading rather than
            // the conservative unseen default, or the mask is a fiction.
            //
            // This used to be a GEOMETRIC test -- the metatile had to sit a
            // whole metatile inside the guest's window, so that all eight
            // neighbours were on screen this frame. That was far stricter than
            // the rule it was standing in for, and it refused exactly the
            // metatiles the band needs most: the ones at the window's edge,
            // which is where the widened columns join the native picture.
            //
            // Measured, it left nine cells per frame drawing the interior
            // cloud where the cartridge drew an edge variant, every one of
            // them with `seen 0` for its mask -- masks D7, F0 and FC at screen
            // rows 0, 1, 22 and 23, all inside the old margin.
            //
            // g_seen is an ABSOLUTE, PERSISTENT record, so a neighbour off the
            // current screen is not therefore unknown; it is unknown only if it
            // has never been read. g_obs answers that directly, so ask it.
            if (!mask_is_observed(mx, my)) { ++g_le_margin; continue; }
            const uint8_t mask = metatile_mask(mx, my);
            uint16_t t[6];
            bool ok = true;
            for (int sy = 0; sy < 2 && ok; ++sy)
                for (int sx = 0; sx < 3 && ok; ++sx) {
                    const uint32_t off =
                        uint32_t((ty + sy) * 32 + (tx + sx)) * 2u;
                    const uint16_t v =
                        uint16_t(nds_vram_read_bg(0, g_bg2_base + off, 2));
                    // IN THE SHROUD'S OWN TILE RANGE, not merely "not clear".
                    //
                    // The only test here was `!= kClearTile`, so every BG2
                    // tile that was not the clear marker counted as fog --
                    // including whatever a menu, a transition or a
                    // mid-update frame happens to hold. Combined with the
                    // unconditional overwrite below, which is last-write-
                    // wins, one bad frame replaces a good entry for the rest
                    // of the session.
                    //
                    // It happened and was caught in the dump: a second entry
                    // for mask FF reading 0EA 0EA 0EA 0EA 0EA 0EA, against
                    // the correct 0C2 0C0 0C1 0BD 0BB 0BC.
                    //
                    // The range is measured, not invented -- the histogram
                    // recorded at the edge table's own comment is "487
                    // interior tiles against 33 edge tiles in one real frame,
                    // all within 0x0B1..0x0C4". 0x0EA is outside it, and so
                    // is kClearTile (0x064), so this subsumes the old test
                    // and is kept alongside it for what it says.
                    const uint16_t idx = v & 0x3FFu;
                    if (idx == kClearTile) ok = false;
                    if (idx < kShroudTileLo || idx > kShroudTileHi) ok = false;
                    t[sy * 3 + sx] = v;
                }
            // A UNIFORM METATILE IS A FRAME CAUGHT MID-UPDATE.
            //
            // The range test above stopped 0EA 0EA 0EA 0EA 0EA 0EA and the
            // next run produced 0BA 0BA 0BA 0BA 0BA 0BA instead -- inside the
            // shroud range, so indistinguishable by index, and still not a
            // metatile. Every valid entry in the dump has six DISTINCT tiles,
            // which is the property the interior learner already relies on:
            // "what the 64-vote threshold actually protects against is a frame
            // caught MID-UPDATE, and that is caught instead by the property
            // that makes a metatile a metatile: its six tiles are DISTINCT".
            //
            // Only the all-identical case is rejected, not partial repeats: an
            // edge variant legitimately reuses a tile across positions (0C4
            // appears at position 3 of both D4 and D6), so requiring full
            // distinctness here would throw away real observations.
            if (ok) {
                bool uniform = true;
                for (int i = 1; i < 6 && uniform; ++i)
                    if ((t[i] & 0x3FFu) != (t[0] & 0x3FFu)) uniform = false;
                if (uniform) ok = false;
            }
            if (!ok) { ++g_le_notfog; continue; }   // not drawn as fog
            ++g_le_took;
            if (!g_fog_edge_seen[mask][0]) ++g_fog_edge_masks;
            for (int i = 0; i < 6; ++i) {
                g_fog_edge[mask][i] = t[i];
                if (g_fog_edge_seen[mask][i] < 255u) ++g_fog_edge_seen[mask][i];
            }
        }
    }
}

// Build the widened terrain table for a GIVEN camera.
//
// Work in ABSOLUTE tile coordinates. The guest's visible tile column 0 is
// absolute tile column cam_tx (every BGxHOFS is zero -- the game scrolls by
// rewriting the tilemap, docs/195), so a hardware tile column h is absolute
// column cam_tx + h. Deriving these as offsets from the camera's METATILE
// instead loses its phase within that metatile and shears every column.
//
// Cached on the camera it was last built for, so a still camera costs nothing
// and a moving one costs one rebuild per tile crossed -- exactly as before,
// only now the camera is the one the render pass picked rather than the one
// vblank happened to read.
// WHICH CELLS A FILL HAS TO COVER (docs/348).
//
// The tables are sized for the largest surface the renderer allows (1728x768,
// band wholly to one side), and both fills walked all of it -- 73,809 cells,
// eight bus reads each for the terrain -- whatever the window, on every frame
// the camera moved, an estimated 2.5-4 ms of the emulation thread per moving
// frame.
//
// A surface W x H can only ever ask for the cells below, WHATEVER the band:
// decode_text_line keys a pixel at surface column x on hardware tile column
// floor((x - extra + bias) / 8) with extra in [0, W-256] and the scroll bias
// in [-4, +3] (Bg_SetScroll's 4 - camx%8), and on hardware tile row
// floor((gy + vbias) / 8) with gy in [-(H-192), H) and vbias in [0, 15]
// (camy%16). Two tiles of slack each way.
struct ExtBounds { int rlo, rhi, hlo, hhi; };
bool operator==(const ExtBounds& a, const ExtBounds& b) {
    return a.rlo == b.rlo && a.rhi == b.rhi && a.hlo == b.hlo && a.hhi == b.hhi;
}
const ExtBounds kFullBounds = {kExtRowLo, kExtRowHi, kExtLo, kExtHi};
ExtBounds bounds_for_surface(int w, int h) {
    const int sx = w > 256 ? w - 256 : 0;
    const int sy = h > 192 ? h - 192 : 0;
    ExtBounds b;
    b.hlo = -((sx + 4 + 7) / 8) - 2;
    b.hhi = (256 + sx + 2) / 8 + 2;
    b.rlo = -((sy + 7) / 8) - 2;
    b.rhi = (192 + sy + 14) / 8 + 2;
    if (b.hlo < kExtLo) b.hlo = kExtLo;
    if (b.hhi > kExtHi) b.hhi = kExtHi;
    if (b.rlo < kExtRowLo) b.rlo = kExtRowLo;
    if (b.rhi > kExtRowHi) b.rhi = kExtRowHi;
    return b;
}
ExtBounds g_fill_bounds = kFullBounds;   // what the NEXT fill must cover
// docs/387: bumped when the metatile tables change under an unmoved camera
// (Force Tileset's detail patch), so the next fill rebuilds instead of
// keeping the band it built from the old entries. docs/388: and on every
// Terrain_FillWindow, the guest's own refill, which a wall retile asks for
// after rewriting plane E.
unsigned g_terrain_gen = 0u;

void fill_terrain(int cam_tx, int cam_ty) {
    static int last_tx = INT32_MIN, last_ty = INT32_MIN;
    static uint32_t last_map = 0;
    static ExtBounds last_b = {0, -1, 0, -1};
    static unsigned last_gen = 0u;
    const ExtBounds bd = g_fill_bounds;
    if (g_ext_valid && cam_tx == last_tx && cam_ty == last_ty &&
        g_map == last_map && bd == last_b && g_terrain_gen == last_gen)
        return;
    last_b = bd;
    last_gen = g_terrain_gen;
    // THE CAMERA THIS TABLE WAS ACTUALLY BUILT FROM.
    //
    // band_record stamped its cells with the LIVE g_cam_tx, but the table it is
    // recording from was built here, and the two can differ by a frame while
    // the camera is panning. A one-column mislabel is exactly what the
    // cross-check saw: ours 028 against theirs 029, 042/043, 045/046, 1E2/1E3 --
    // consistently one tile out, alternating on the metatile's 2-row period.
    // Stamping with the table's own camera removes that as an explanation, so
    // whatever disagreement survives is the band's and not the clock's.
    g_fill_cam_tx = cam_tx;
    g_fill_cam_ty = cam_ty;
    last_tx = cam_tx;
    last_ty = cam_ty;
    last_map = g_map;

    std::memset(g_ext, 0, sizeof g_ext);
    for (int trow = bd.rlo; trow <= bd.rhi; ++trow) {
        const int abs_row = cam_ty + trow;
        const int my = floor_div(abs_row, 2);
        const int sub = floor_mod(abs_row, 2);
        // ONE METATILE LOOKUP PER THREE COLUMNS, not one per column: a
        // metatile is three tiles across, and every column of it reads the
        // same eight bytes.
        int cached_mx = INT32_MIN;
        Metatile m{};
        for (int h = bd.hlo; h <= bd.hhi; ++h) {
            // Every cell, native columns included: the native ones are never
            // served, but they are what the camera check compares against.
            const int abs_col = cam_tx + h;
            const int mx = floor_div(abs_col, 3);
            const int within = floor_mod(abs_col, 3);
            if (mx != cached_mx) {
                m = metatile_tiles(g_map, g_split, g_table_a, g_table_b, mx,
                                   my, g_map_w, g_map_h);
                cached_mx = mx;
            }
            g_ext[trow - kExtRowLo][h - kExtLo] =
                m.ok ? m.t[sub * 3 + within] : 0u;
        }
    }
    g_ext_valid = true;
}

// How badly would a candidate camera disagree with the picture on screen?
//
// Samples native cells -- ones this port never draws -- and compares what the
// candidate camera says should be there against what the guest's own BG1
// tilemap actually holds. A sparse grid rather than all 768, because this runs
// per candidate per frame and the answer is either 0 or very large: a camera
// that is off by a pan step disagrees on nearly every cell, so eight columns by
// six rows separates the cases with no ambiguity at all.
int sample_mismatch(int cam_tx, int cam_ty, uint32_t bg1_base) {
    int bad = 0;
    for (int trow = 1; trow < 24; trow += 4) {
        const int abs_row = cam_ty + trow;
        const int my = floor_div(abs_row, 2);
        const int sub = floor_mod(abs_row, 2);
        for (int h = 1; h < 32; h += 4) {
            const int abs_col = cam_tx + h;
            const Metatile m = metatile_tiles(g_map, g_split, g_table_a,
                                              g_table_b, floor_div(abs_col, 3),
                                              my, g_map_w, g_map_h);
            const uint16_t ours =
                (m.ok ? m.t[sub * 3 + floor_mod(abs_col, 3)] : 0u) & 0x3FFu;
            const uint16_t theirs = uint16_t(nds_vram_read_bg(
                0, bg1_base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
            // A WALL TILE SAYS NOTHING ABOUT THE CAMERA (docs/367). Walls are
            // stamped over the metatile by func_02034B44 from the block
            // 0x3C8-0x3FF, the same in all three tilesets, and `ours` never
            // has them. Counting such a cell as a mismatch failed the check on
            // every frame a wall sat on a sampled row, and choose_camera
            // (which wants 0) then held the band on a stale table: the bottom
            // band drew one metatile row off, measured against the
            // cartridge's own drawing at camera y 528. The
            // cell is skipped; the rest of the grid still decides.
            if (theirs >= 0x3C8u) continue;
            if (ours != theirs) ++bad;
        }
    }
    return bad;
}

// WHICH CAMERA IS THE GUEST ACTUALLY DRAWING FROM?
//
// Not necessarily the one its view struct held at vblank. The struct advances
// with the game's logic; the tilemap is rewritten separately and lags it, so
// for one to two frames after every pan step the picture on screen belongs to
// the PREVIOUS camera. A band built from the struct's value is a whole pan step
// away from the field beside it for those frames -- measured at 750 of 768
// native cells disagreeing, i.e. the entire screen, not an edge artefact.
//
// So ask the picture. The newest reading is tried first because it is right
// whenever the camera is still, which is most of the time; only when it
// disagrees is the history walked. Ties go to the newest.
//
// This cannot drift: it is re-decided every frame against VRAM, so a wrong pick
// lasts exactly one frame and only if the map genuinely offers two alignments
// that both match 48 sampled cells.
int g_cam_pick_age = 0;        // how far back the chosen camera came from
unsigned g_cam_pick_late = 0;  // frames where the newest reading was rejected


void choose_camera(uint32_t dispcnt) {
    if (g_cam_hist_n <= 0) { g_model_ok = false; return; }
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg1cnt = nds_io_debug_read(9, 0x0400000Au, 16);
    const uint32_t base = block + ((bg1cnt >> 8) & 0x1Fu) * 0x800u;

    int best = 0;
    int best_bad = sample_mismatch(g_cam_hist_tx[0], g_cam_hist_ty[0], base);
    for (int i = 1; i < g_cam_hist_n && best_bad > 0; ++i) {
        const int bad = sample_mismatch(g_cam_hist_tx[i], g_cam_hist_ty[i],
                                        base);
        if (bad < best_bad) {
            best_bad = bad;
            best = i;
        }
    }
    g_cam_pick_age = best;
    if (best != 0) ++g_cam_pick_late;
    g_cam_tx = g_cam_hist_tx[best];
    g_cam_ty = g_cam_hist_ty[best];

    // Zero of the 48 sampled cells may disagree. The sample is spread across
    // the whole native window, and every failure mode seen so far -- a camera
    // off by a pan step, a map not yet built, a stale pointer -- disagrees on
    // hundreds of cells out of 768, so there is no need for a tolerance and no
    // benefit to one: a tolerance would only let a genuinely wrong model
    // through on a map uniform enough to hide it, and on such a map the band
    // it drew would be indistinguishable anyway.
    g_model_ok = (best_bad == 0);
    if (g_model_ok) g_model_was_ok = true;
    if (!g_model_ok) {
        ++g_model_bad_frames;
        if (g_model_was_ok) ++g_model_bad_after_ok;
        // HOW BADLY does a rejected frame disagree? The comment above argues
        // against a tolerance because "every failure mode seen so far ...
        // disagrees on hundreds of cells out of 768". That is a claim about
        // the data, so record the data rather than re-reading the claim.
        //
        // It matters for the stale-band defect. A rejection makes the band serve
        // the PREVIOUS frame's table, so ~74 rejections in a run is ~74
        // frames of stale band. If those rejections disagree on 1-3 of 48,
        // they are the mid-pan case this file already describes -- the
        // streamer rewriting columns, so the tilemap genuinely IS a mix of
        // two cameras and no candidate can match all 48 -- and that is a
        // different thing from a wrong model, wanting a different remedy.
        // If they disagree on dozens, the comment is right and holding the
        // last good table is exactly the correct behaviour.
        //
        // One increment on a path already taken ~74 times in a whole run.
        if (best_bad > g_model_bad_max) g_model_bad_max = best_bad;
        const int b = best_bad < 0 ? 0 : (best_bad > 8 ? 9 : best_bad);
        ++g_model_bad_hist[b];
    }
}

// AUDIT: does this port's terrain model agree with the cartridge's own?
//
// The widened band was seen to break at times and become inconsistent, and
// sometimes the fog failed to show or the sides, or even the original-aspect
// field, corrupted. "At times" is the hard part -- a defect
// that is not on screen when the probe looks is a defect the probe reports as
// absent, and this port has already published two wrong conclusions that way
// (docs/214, docs/217).
//
// There is a way to measure it continuously rather than hunt for it, and it
// falls out of how lb_widemap_fill is written. The fill computes g_ext for
// EVERY cell in its 42x64 window, native columns included, and then serves only
// the cells outside the guest's own 32x24 rectangle. So for the cells it does
// NOT serve there is an independent answer sitting in VRAM: the guest's own BG1
// tilemap, which is the cartridge's rendering of the same map through the same
// camera.
//
// Where the two agree, the band is being built from a correct model and cannot
// be wrong for the reason being looked for. Where they DISAGREE, this port's
// model of the map has drifted from the game's -- and since the band comes from
// that same model, the band is wrong at the same instant, in the same way, for
// the columns nobody can check by eye.
//
// So the audit turns "sometimes" into a number per frame. Zero for a whole
// match means the terrain model is not the cause and the search moves on; any
// non-zero frame is the defect caught in the act, with its camera position and
// its row and column recorded.
//
// LB_MAPAUDIT=1. Off by default: it is 768 VRAM reads a frame.
unsigned g_audit_frames = 0;      // frames audited
unsigned g_audit_bad_frames = 0;  // frames with at least one mismatch
unsigned g_audit_worst = 0;       // worst single-frame mismatch count
bool audit_on() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("LB_MAPAUDIT");
        cached = (v && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

// WHAT THE CARTRIDGE ITSELF READS TO DECIDE A CELL IS SHROUDED.
//
// The band's fog comes from a per-METATILE byte grid found by correlation, and
// it cannot represent a fog boundary that falls inside a metatile: measured
// same-frame against the cartridge inside the guest's own window it leaves 3.9%
// of tiles open where the cartridge fogs them, and that is its best alignment.
// The cartridge has no such problem, so find what IT reads.
//
// func_0203A4B4 is the shroud pass. Its first instructions are
//
//     0203A4BC  ldr   r6,[r15,#0x120]     ; literal at 0203A5E4 -> the object
//     0203A4C4  ldr   r3,[r6,#0x4]        ; bytes per row
//     0203A4C8  ldr   r2,[r6,#0xc]        ; rows
//     0203A4D4  ldr   r7,[r6,#0x10]       ; the mask buffer
//     0203A4E8  ldr   r5,[r10,#0x174]     ; last frame's copy, on the view
//     0203A4DC  mul   r4,r3,r2            ; ... compared byte for byte
//
// so the object's fields are stride at +4, rows at +0xC, the mask at +0x10 and
// the row-pointer table at +0x14, and func_020EEBF4 indexes the row table by
// ty and tests bit (tx & 7) of byte (tx >> 3).
//
// docs/202 read this object as living at 0x02151F28 and found it empty, twice,
// on a screen that was wall to wall cloud -- which cannot be true of the buffer
// this pass tests. So take the address from the literal instead of assuming it,
// and print what is actually there. LB_FOGOBJ=1.
void probe_fog_object(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_FOGOBJ") ? 1 : 0;
    if (!on) return;
    static unsigned n = 0;
    if ((++n % 120u) != 0u) return;
    const uint32_t obj = bus_read_u32(0x0203A5E4u);
    const uint32_t stride = bus_read_u32(obj + 0x04u);
    const uint32_t width = bus_read_u32(obj + 0x08u);
    const uint32_t rows = bus_read_u32(obj + 0x0Cu);
    const uint32_t buf = bus_read_u32(obj + 0x10u);
    const uint32_t rowtab = bus_read_u32(obj + 0x14u);
    unsigned set = 0;
    if (buf >= 0x02000000u && buf < 0x02400000u && stride && stride < 64u &&
        rows && rows <= 64u) {
        for (uint32_t i = 0; i < stride * rows; ++i) {
            uint8_t b = bus_read_u8(buf + i);
            while (b) { set += b & 1u; b = uint8_t(b >> 1); }
        }
    }
    std::fprintf(stderr,
                 "[lb_fogobj] literal 0203A5E4 -> %08X: stride %u width %u "
                 "rows %u buf %08X rowtab %08X  bits set %u of %u\n",
                 obj, stride, width, rows, buf, rowtab, set, stride * rows * 8u);
    std::fflush(stderr);
}

void audit_terrain(uint32_t dispcnt) {
    probe_fog_object();
    if (!audit_on() || !g_ext_valid) return;
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg1cnt = nds_io_debug_read(9, 0x0400000Au, 16);
    const uint32_t base = block + ((bg1cnt >> 8) & 0x1Fu) * 0x800u;

    // Arm the band recorder now that the audit is known to be on.
    g_band_truth_on = true;

    // The band cross-check, before the native audit below.
    if (!g_band_served.empty() && g_seen_w > 0) {
        const int tw = g_seen_w * 3;
        for (int trow = 0; trow < 24; ++trow) {
            const int ar = g_cam_ty + trow;
            for (int h = 0; h < 32; ++h) {
                const int ac = g_cam_tx + h;
                if (ac < 0 || ac >= tw || ar < 0) continue;
                const size_t i = (size_t)ar * tw + ac;
                if (i >= g_band_served.size()) continue;
                const uint16_t rec = g_band_served[i];
                if (!(rec & 0x8000u)) continue;
                const uint16_t theirs = uint16_t(nds_vram_read_bg(
                    0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
                ++g_band_checked;
                const int kind = int((rec >> 11) & 7u);
                const bool bad = (rec & 0x3FFu) != theirs;
                if (bad) ++g_band_wrong;
                if (kind >= 1 && kind <= 4) {
                    ++g_band_chk[kind];
                    g_band_bad[kind] += bad ? 1u : 0u;
                }
                // The first few disagreements, with coordinates: if ours is
                // consistently a neighbour's tile the fault is an off-by-N in
                // the absolute column, which a rate alone cannot show.
                static unsigned shown = 0;
                if (bad && shown < 12u) {
                    ++shown;
                    std::fprintf(stderr,
                                 "[lb_bandbad] abs (%d,%d) %s ours %03X "
                                 "theirs %03X  (cam %d,%d  h %d trow %d)\n",
                                 ac, ar, band_name(kind),
                                 unsigned(rec & 0x3FFu), unsigned(theirs),
                                 g_cam_tx, g_cam_ty, h, trow);
                }
                g_band_served[i] = 0u;      // score each cell once
            }
        }
        // 200, not 2000. A pan of 42 tile columns brings only ~1000 band cells
        // into the guest's window (24 rows each), so the first threshold never
        // fired and the diagnostic printed nothing at all -- which reads
        // exactly like "the cross-check found nothing to check".
        static unsigned last = 0;
        if (g_band_checked - last >= 200u) {
            last = g_band_checked;
            // A band NOBODY SAMPLED prints "not sampled", never "0/0 (0.0%)".
            // A downward pan never brings a cell from above the window into it,
            // so ABOVE legitimately has no data in that run -- and a zero
            // percentage there reads exactly like a pass. That is measurement
            // bug 5: a check that never fires must not look like a check that
            // passed.
            char per[256];
            int n = 0;
            for (int k = kBandLeft; k <= kBandBelow; ++k)
                n += g_band_chk[k]
                         ? std::snprintf(per + n, sizeof per - (size_t)n,
                                         "  %s %u/%u (%.1f%%)", band_name(k),
                                         g_band_bad[k], g_band_chk[k],
                                         100.0 * double(g_band_bad[k]) /
                                             double(g_band_chk[k]))
                         : std::snprintf(per + n, sizeof per - (size_t)n,
                                         "  %s not sampled", band_name(k));
            std::fprintf(stderr,
                         "[lb_bandtruth] %u band cells later seen in the "
                         "guest's window: %u disagreed (%.1f%%) %s\n",
                         g_band_checked, g_band_wrong,
                         100.0 * double(g_band_wrong) /
                             double(g_band_checked ? g_band_checked : 1),
                         per);
            std::fflush(stderr);
        }
    }

    unsigned bad = 0;
    int first_r = -1, first_c = -1;
    uint16_t first_ours = 0, first_theirs = 0;
    for (int trow = 0; trow < 24; ++trow) {
        for (int h = 0; h < 32; ++h) {
            // Tile index only. Palette and flip bits come from the metatile
            // tables the same way for both, but a mismatch confined to those
            // would be a different bug from the one being hunted and would
            // drown the count that matters.
            const uint16_t ours =
                g_ext[trow - kExtRowLo][h - kExtLo] & 0x3FFu;
            const uint16_t theirs = uint16_t(nds_vram_read_bg(
                0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
            if (ours == theirs) continue;
            // OFF THE MAP IS NOT A MODELLING ERROR.
            //
            // Panned hard against the right edge, hardware column 31 is
            // absolute column 192 of a 192-column map -- past the last one.
            // The audit reported 24 cells there, one per row, ours 000 against
            // the guest's 023, and neither way of reading past the edge
            // explains 023: clamping mx to the last column and reading the row
            // entry unclamped BOTH give 021.
            //
            // What does explain it is that the guest writes its tilemap a
            // whole METATILE at a time, three columns per metatile. With the
            // window starting at absolute column 161 the last metatile it
            // writes covers columns 189..191, hardware 28..30, and hardware
            // column 31 is simply not written that pass -- it keeps whatever
            // an earlier camera left there. The cartridge is showing a stale
            // column at the map edge.
            //
            // Trying to reproduce that made things worse, and measurably: a
            // camera clamped to (columns - 32) shifted the whole picture by a
            // tile and took the disagreement from 24 cells to 221. So the
            // camera reading is right, the guest's edge column is stale, and
            // the honest thing to do with a cell that has no map under it is
            // to leave it blank -- which is what this port already does.
            //
            // It is also invisible either way: column 31 is the GUEST'S, never
            // served from here. Excluded from the count so the audit measures
            // the model rather than the cartridge's edge behaviour.
            if (ours == 0u) {
                const int abs_col = g_cam_tx + h;
                const int abs_row = g_cam_ty + trow;
                if (floor_div(abs_col, 3) >= g_map_w ||
                    floor_div(abs_row, 2) >= g_map_h ||
                    abs_col < 0 || abs_row < 0)
                    continue;
            }
            if (!bad) {
                first_r = trow;
                first_c = h;
                first_ours = ours;
                first_theirs = theirs;
            }
            ++bad;
        }
    }
    ++g_audit_frames;
    if (bad) {
        ++g_audit_bad_frames;
        if (bad > g_audit_worst) g_audit_worst = bad;
    }
    // Every bad frame is printed, because the question is when they happen and
    // how they cluster, not how many there are in total. Clean frames are
    // summarised once a second so the log shows the audit is alive.
    if (bad) {
        // WHAT DOES THE CARTRIDGE DO AT THE MAP'S EDGE?
        //
        // The remaining mismatches sit in one column at the far right of a
        // fully panned map, where this port's bounds test calls the metatile
        // off-map and blanks it while the guest draws a real tile. The map is
        // a table of ROW POINTERS -- row = *(map + 0x34 + my*4), then
        // *(row + mx*2) -- so an mx one past the last column is a read one
        // entry past that row's data, not a wrap to the next row.
        //
        // Two behaviours could produce a real tile there: the guest clamps mx
        // to the last column, or it does not bounds-check and reads the entry
        // anyway. Rather than pick one, print what each would give beside what
        // the guest actually drew. Whichever matches is the answer.
        const int abs_row = g_cam_ty + first_r;
        const int abs_col = g_cam_tx + first_c;
        const int my = floor_div(abs_row, 2);
        const int sub = floor_mod(abs_row, 2);
        const int within = floor_mod(abs_col, 3);
        const int mx = floor_div(abs_col, 3);
        uint16_t clamped = 0xFFFFu, raw = 0xFFFFu;
        if (my >= 0 && my < g_map_h) {
            const int cx = mx < 0 ? 0 : (mx >= g_map_w ? g_map_w - 1 : mx);
            const Metatile mc = metatile_tiles(g_map, g_split, g_table_a,
                                               g_table_b, cx, my, g_map_w,
                                               g_map_h);
            if (mc.ok) clamped = mc.t[sub * 3 + within] & 0x3FFu;
            const uint32_t rowp =
                bus_read_u32(g_map + 0x34u + uint32_t(my) * 4u);
            if (main_ram(rowp) && mx >= 0) {
                const Metatile mr = expand_metatile(
                    g_split, g_table_a, g_table_b,
                    bus_read_u16(rowp + uint32_t(mx) * 2u));
                if (mr.ok) raw = mr.t[sub * 3 + within] & 0x3FFu;
            }
        }
        std::fprintf(stderr,
                     "[lb_audit] frame %u: %u/768 native cells DISAGREE, "
                     "first r%d c%d ours %03X theirs %03X (mx %d of %d: "
                     "clamped %03X raw %03X), cam %d,%d\n",
                     g_scan_frames, bad, first_r, first_c, first_ours,
                     first_theirs, mx, g_map_w, clamped, raw, g_cam_tx,
                     g_cam_ty);
    }
    else if ((g_audit_frames % 60u) == 0u)
        std::fprintf(stderr,
                     "[lb_audit] %u frames, %u bad (worst %u cells), cam %d,%d\n",
                     g_audit_frames, g_audit_bad_frames, g_audit_worst,
                     g_cam_tx, g_cam_ty);
}

// THE SAME AUDIT, FOR THE FOG.
//
// The terrain audit above answers "is the band's ground the right ground". This
// answers the remaining cosmetic question: does the band draw the same fog
// TILE the cartridge would draw there?
//
// It is a fair question rather than a tautology. g_seen records which absolute
// tiles are shrouded, and that record is learned from the guest's own columns,
// so the shrouded/clear VERDICT for a native cell is close to given. The tile
// is not: this port re-emits fog from a six-entry interior pattern plus a table
// of edge variants keyed on the shrouded-neighbour mask, and whether that
// reproduces the cartridge's choice at a cloud's edge is exactly the open
// question. Comparing native cells measures it directly.
//
// Split three ways, because the three mean different things:
//   verdict  - we call a cell fogged and the guest does not, or the reverse.
//              A modelling error in the shroud record.
//   tile     - both agree it is fogged, different tile. The edge table.
//   total    - the sum, for a single number to watch.
unsigned g_fa_verdict = 0, g_fa_tile = 0, g_fa_cells = 0;

void audit_fog(uint32_t dispcnt) {
    // THE PROBES COME BEFORE THE g_ext2_valid GATE.
    //
    // That gate is about whether this port can DRAW fog -- it is false until
    // the six cloud tiles have been learned from the guest's own BG2. The
    // probes below ask a different question: what is the cartridge drawing,
    // and what does the visibility record say. Neither needs anything learned.
    //
    // Gating them together cost a measurement. Under reveal-map there is no
    // cloud on screen, so nothing is ever learned, so g_ext2_valid stays false
    // and [lb_recbyte] and [lb_visoff] could not run AT ALL -- in exactly the
    // configuration where "is anything drawn on this layer here" is easiest to
    // ask, because with the map revealed the answer should be nothing.
    if (!audit_on()) return;
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg2cnt = nds_io_debug_read(9, 0x0400000Cu, 16);
    const uint32_t base = block + ((bg2cnt >> 8) & 0x1Fu) * 0x800u;
    const bool can_draw = g_ext2_valid && g_model_ok;

    // IS THE RECORD MERELY MIS-ALIGNED? Measure, do not assume -- the same
    // question for the map viewer's grid was settled by searching offsets and
    // the answer there was a flat (+2,+0) nobody had guessed.
    //
    // The record is one byte per metatile and the cartridge's BG2 is one entry
    // per tile, so mx = ac/3, my = ar/2. If that division has the wrong origin
    // the band reads a neighbouring metatile, which shows up as misses on some
    // column residues and not others -- and the miss histogram by metatile
    // position is 69 128 128 / 57 104 104, with residue 0 about half the rest.
    //
    // This compares the record against the cartridge's OWN BG2 over the
    // guest's window, where the cartridge is ground truth and available in the
    // same frame, so there is no time bridge and no lag. Per-tile truth versus
    // per-metatile record cannot reach zero at a fog boundary; the point is
    // which offset is LOWEST.
    if (std::getenv("LB_VISOFF") && g_vis_base && g_map_w > 0) {
        // SPREAD ACROSS THE RUN, not the first six frames it sees.
        //
        // This was `shown < 6`, so every sample came from six CONSECUTIVE
        // frames -- a tenth of a second, at whatever camera position the run
        // happened to be in. I then compared "early" and "late" samples to ask
        // whether the interior holes follow the screen or the ground, and of
        // course both coordinates were identical: the camera had not moved
        // between them. The conclusion drawn from that comparison was
        // unfounded and is retracted.
        //
        // Once every 120 scans instead, so samples land at genuinely different
        // camera positions and the question can actually be asked.
        static int shown = 0;
        if (scan_logging() && shown < 12 && (g_scan_frames % 120u) == 0u) {
            ++shown;
            // EVERY CANDIDATE, not just the adopted one, and split by
            // DIRECTION. The search adopts the first survivor because "any
            // survivor will do -- they all agree with the guest's own fog on
            // every view sampled". They agree on the SAMPLED views; the band
            // draws cells those samples never covered, and the errors run
            // about 3:1 in the direction that shows open ground the cartridge
            // is hiding. If these buffers are per-player, the one the screen is
            // drawn from should stand out here -- and the cheating direction is
            // the column that matters, since over-fogging is the error this
            // file is allowed to make.
            for (int ci = 0; ci < g_cand_n && ci < 12; ++ci) {
                unsigned chk = 0, hole = 0, over = 0;
                for (int trow = 0; trow < 24; ++trow) {
                    const int ar = g_cam_ty + trow;
                    for (int h = 0; h < 32; ++h) {
                        const int ac = g_cam_tx + h;
                        const int mx = floor_div(ac, 3);
                        const int my = floor_div(ar, 2);
                        if (mx < 0 || my < 0 ||
                            mx >= g_map_w || my >= g_map_h) continue;
                        const bool rec_shrouded = bus_read_u8(
                            g_cand[ci].base + uint32_t(my) * g_cand[ci].stride +
                            uint32_t(mx)) == 0u;
                        const uint16_t t = uint16_t(nds_vram_read_bg(
                            0, base + uint32_t(trow * 32 + h) * 2u, 2))
                            & 0x3FFu;
                        const bool cart_shrouded =
                            (t != 0u && t != kClearTile);
                        ++chk;
                        if (!rec_shrouded && cart_shrouded) ++hole;
                        if (rec_shrouded && !cart_shrouded) ++over;
                    }
                }
                std::fprintf(stderr,
                             "[lb_visoff] %08X%s: %.1f%% HOLES (open where the "
                             "cartridge fogs), %.1f%% over-fogged, of %u\n",
                             g_cand[ci].base,
                             g_cand[ci].base == g_vis_base ? " (adopted)" : "",
                             chk ? 100.0 * hole / chk : 0.0,
                             chk ? 100.0 * over / chk : 0.0, chk);
            }
            // AND THE BITMAP THE BAND ACTUALLY DRAWS FROM -- the adopted
            // record after dilation. Same frame, no time bridge, so this has
            // none of the variance the band grader has: it says what a player
            // looking at this frame would see.
            if (!g_vis_bits.empty()) {
                unsigned chk = 0, hole = 0, over = 0;
                for (int trow = 0; trow < 24; ++trow) {
                    const int ar = g_cam_ty + trow;
                    for (int h = 0; h < 32; ++h) {
                        const int ac = g_cam_tx + h;
                        const int mx = floor_div(ac, 3);
                        const int my = floor_div(ar, 2);
                        if (mx < 0 || my < 0 ||
                            mx >= g_map_w || my >= g_map_h) continue;
                        const bool ours_shrouded =
                            g_vis_bits[size_t(my) * size_t(g_map_w) +
                                       size_t(mx)] != 0u;
                        const uint16_t t = uint16_t(nds_vram_read_bg(
                            0, base + uint32_t(trow * 32 + h) * 2u, 2))
                            & 0x3FFu;
                        const bool cart_shrouded =
                            (t != 0u && t != kClearTile);
                        ++chk;
                        if (!ours_shrouded && cart_shrouded) ++hole;
                        if (ours_shrouded && !cart_shrouded) ++over;
                    }
                }
                std::fprintf(stderr,
                             "[lb_visoff] DRAWN (adopted, dilated): %.1f%% "
                             "HOLES, %.1f%% over-fogged, of %u\n",
                             chk ? 100.0 * hole / chk : 0.0,
                             chk ? 100.0 * over / chk : 0.0, chk);
            }
            // IS THE RECORD'S BYTE A BOOLEAN AT ALL?
            //
            // This file reads it as one -- `bus_read_u8(...) ? revealed :
            // hidden` -- and every interior hole found so far reports the same
            // byte, 01, on ground the cartridge is fogging. If the byte is a
            // LEVEL rather than a flag (explored versus currently visible, say)
            // then treating every nonzero alike would produce exactly those
            // holes, and the fix is a threshold rather than a dilation.
            //
            // So cross-tabulate, over the guest's own window where the
            // cartridge is ground truth in the same frame: for each byte value
            // the record holds, how many of those tiles does the cartridge fog
            // and how many does it leave clear. A boolean shows two clean
            // columns; a level shows a value that sits on both sides.
            if (g_vis_base && g_map_w > 0) {
                unsigned fogged[8] = {}, clear_[8] = {};
                for (int trow = 0; trow < 24; ++trow) {
                    const int ar = g_cam_ty + trow;
                    for (int h = 0; h < 32; ++h) {
                        const int ac = g_cam_tx + h;
                        const int mx = floor_div(ac, 3), my = floor_div(ar, 2);
                        if (mx < 0 || my < 0 ||
                            mx >= g_map_w || my >= g_map_h) continue;
                        // 0 or 1: the located record is one BIT per
                        // metatile, so the histogram below has two buckets
                        // rather than the byte record's range.
                        const uint8_t v = vis_revealed(mx, my) ? 1u : 0u;
                        const unsigned slot = v < 7u ? v : 7u;
                        const uint16_t t = uint16_t(nds_vram_read_bg(
                            0, base + uint32_t(trow * 32 + h) * 2u, 2))
                            & 0x3FFu;
                        if (t != 0u && t != kClearTile) ++fogged[slot];
                        else ++clear_[slot];
                    }
                }
                std::fprintf(stderr, "[lb_recbyte] value: fogged/clear ");
                for (unsigned v = 0; v < 8; ++v)
                    if (fogged[v] || clear_[v])
                        std::fprintf(stderr, " %s%u=%u/%u", v == 7 ? ">=" : "",
                                     v, fogged[v], clear_[v]);
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
            }

            // THE DECISION THE FILL ACTUALLY MADE, per tile, same frame.
            if (g_fill_s_valid) {
                unsigned chk = 0, hole = 0, over = 0;
                // IS THIS REALLY A RESOLUTION LIMIT? The story has been that
                // the record is one byte per METATILE against per-TILE fog, so
                // a boundary inside a metatile cannot be represented. If that
                // is the whole story then every hole sits in a metatile with at
                // least one hidden neighbour. Fogging just the edge tiles only
                // recovered one point of the 3.9%, which does not fit -- so
                // count each hole by how many of its metatile's eight
                // neighbours the record calls hidden. Holes at 0 are in the
                // INTERIOR of a region the record says is entirely revealed,
                // and no resolution argument explains those.
                unsigned by_nb[10] = {};
                uint16_t hole_val[8] = {};
                unsigned hole_val_n[8] = {};
                // Fresh samples every report, so early and late frames of a pan
                // can be compared: map-anchored fog keeps its ABSOLUTE cell as
                // the camera moves, a HUD element keeps its SCREEN cell.
                g_interior_n = 0;
                for (int trow = 0; trow < 24; ++trow) {
                    for (int h = 0; h < 32; ++h) {
                        const uint16_t t = uint16_t(nds_vram_read_bg(
                            0, base + uint32_t(trow * 32 + h) * 2u, 2))
                            & 0x3FFu;
                        const bool cart = (t != 0u && t != kClearTile);
                        const bool ours = g_fill_s[trow][h] != 0u;
                        ++chk;
                        if (!ours && cart) {
                            ++hole;
                            const int mx = floor_div(g_cam_tx + h, 3);
                            const int my = floor_div(g_cam_ty + trow, 2);
                            int nb = 0;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx)
                                    if ((dx || dy) &&
                                        hidden_metatile(mx + dx, my + dy)) ++nb;
                            by_nb[nb] += 1u;
                            for (int i = 0; i < 8; ++i) {
                                if (hole_val_n[i] && hole_val[i] != t) continue;
                                hole_val[i] = t;
                                ++hole_val_n[i];
                                break;
                            }
                            // WHERE the interior ones are. These are the holes
                            // no resolution argument explains -- the record
                            // calls the metatile and all eight neighbours
                            // revealed, and the cartridge draws interior cloud
                            // on them anyway. They are stable across samples,
                            // so they are a fixed feature with a findable
                            // cause; print the absolute cell, the metatile,
                            // the tile's position inside it, and the record's
                            // own byte, so the cause can be looked at rather
                            // than guessed.
                            if (nb == 0 && g_interior_n < 3) {
                                const uint8_t rec =
                                    vis_revealed(mx, my) ? 1u : 0u;
                                // SCREEN position as well as absolute. Those
                                // consecutive indices (0B2 0B3 0B4 0B5 0B6)
                                // look more like a graphic than cloud, and if
                                // BG2 carries a HUD element on some rows then
                                // this test counts it as fog and the hole rate
                                // is overstated. Map-anchored fog keeps its
                                // ABSOLUTE cell as the camera pans; a HUD
                                // element keeps its SCREEN cell. Printing both
                                // across frames says which.
                                std::fprintf(stderr,
                                             "[lb_hole] frame %u abs tile "
                                             "(%d,%d) screen (%d,%d) "
                                             "metatile (%d,%d) "
                                             "pos (%d,%d) cartridge %03X "
                                             "record byte %02X\n",
                                             g_scan_frames,
                                             g_cam_tx + h, g_cam_ty + trow,
                                             h, trow, mx, my,
                                             floor_mod(g_cam_tx + h, 3),
                                             floor_mod(g_cam_ty + trow, 2),
                                             unsigned(t), unsigned(rec));
                                ++g_interior_n;
                            }
                        }
                        if (ours && !cart) ++over;
                    }
                }
                if (hole) {
                    // AND WHICH TILE the cartridge actually drew there. This
                    // test calls any BG2 entry that is neither 0 nor
                    // kClearTile "fog", which assumes every other value on
                    // that layer is cloud. If it is not, some of these holes
                    // are the test's fault and not the record's -- so print
                    // the values rather than trusting the assumption.
                    char hv[160];
                    int n = 0;
                    for (int i = 0; i < 8 && hole_val_n[i]; ++i)
                        n += std::snprintf(hv + n, sizeof hv - (size_t)n,
                                           " %03X x%u", hole_val[i],
                                           hole_val_n[i]);
                    std::fprintf(stderr,
                                 "[lb_holeshape] holes by hidden neighbours "
                                 "0..8: %u %u %u %u %u %u %u %u %u   "
                                 "cartridge drew:%s\n",
                                 by_nb[0], by_nb[1], by_nb[2], by_nb[3],
                                 by_nb[4], by_nb[5], by_nb[6], by_nb[7],
                                 by_nb[8], hv);
                }
                std::fprintf(stderr,
                             "[lb_filltruth] the fill's own verdict vs the "
                             "cartridge, per tile: %.1f%% HOLES, %.1f%% "
                             "over-fogged, of %u\n",
                             chk ? 100.0 * hole / chk : 0.0,
                             chk ? 100.0 * over / chk : 0.0, chk);
            }
            std::fflush(stderr);
        }
    }

    // FIRST, the assumption the band check depends on: watch the cartridge's
    // own BG2 per absolute cell and count which way it moves.
    if (g_band_truth_on && g_seen_w > 0 && g_seen_h > 0) {
        const int tw = g_seen_w * 3, th = g_seen_h * 2;
        if ((int)g_cart_fog.size() != tw * th) g_cart_fog.assign(tw * th, 0u);
        for (int trow = 0; trow < 24; ++trow) {
            const int ar = g_cam_ty + trow;
            if (ar < 0 || ar >= th) continue;
            for (int h = 0; h < 32; ++h) {
                const int ac = g_cam_tx + h;
                if (ac < 0 || ac >= tw) continue;
                const uint16_t t = uint16_t(nds_vram_read_bg(
                    0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
                const uint8_t now = (t != 0u && t != kClearTile) ? 2u : 1u;
                uint8_t& was = g_cart_fog[(size_t)ar * tw + ac];
                if (was == 1u && now == 2u) ++g_cart_reshroud;
                else if (was == 2u && now == 1u) ++g_cart_reveal;
                was = now;
            }
        }
    }

    // EVERYTHING BELOW compares what this port DREW against the cartridge, so
    // it needs the fog table to be valid. The probes above do not.
    if (!can_draw) return;

    // THE BAND'S FOG, graded against the cartridge's own BG2 once the camera
    // brings that ground into the guest's window. The audit below this one
    // compares the guest's OWN 32x24 cells, which is the one region the band
    // never serves -- so until now nothing had measured fog in the band at all.
    if (!g_fog_served.empty() && g_seen_w > 0) {
        const int tw = g_seen_w * 3;
        for (int trow = 0; trow < 24; ++trow) {
            const int ar = g_cam_ty + trow;
            for (int h = 0; h < 32; ++h) {
                const int ac = g_cam_tx + h;
                if (ac < 0 || ac >= tw || ar < 0) continue;
                const size_t i = (size_t)ar * tw + ac;
                if (i >= g_fog_served.size()) continue;
                const uint16_t rec = g_fog_served[i];
                if (!(rec & 0x8000u)) continue;
                // FRESH ONLY. LB_FOGAGE frames (default 180, about three
                // seconds) between the band drawing a cell and the camera
                // arriving at it. Beyond that a disagreement says nothing --
                // the fog is allowed to move -- and mixing those in is what
                // made this number swing from 1.1% to 7.6% on identical runs.
                static const unsigned max_age = [] {
                    const char* v = std::getenv("LB_FOGAGE");
                    const int n = v ? std::atoi(v) : 180;
                    return n > 0 ? (unsigned)n : 180u;
                }();
                if (i < g_fog_frame.size()) {
                    const unsigned age =
                        (unsigned)(uint16_t(g_scan_frames) - g_fog_frame[i]);
                    if (age > max_age) { g_fog_served[i] = 0u; continue; }
                }
                const uint16_t ours = uint16_t(rec & 0x3FFu);
                const uint16_t theirs = uint16_t(nds_vram_read_bg(
                    0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
                // kClearTile is "not shrouded" on this layer, and so is 0
                // (nothing drawn). Both mean open ground.
                const bool ours_fog = (ours != 0u && ours != kClearTile);
                const bool theirs_fog = (theirs != 0u && theirs != kClearTile);
                ++g_fog_chk;
                if (ours_fog && !theirs_fog) {
                    ++g_fog_explained;         // revealed in between
                } else if (!ours_fog && theirs_fog) {
                    ++g_fog_defect;            // band showed open ground
                    const int k = int((rec >> 11) & 7u);
                    if (k >= 1 && k <= 4) ++g_fog_defect_kind[k];
                    // WHERE INSIDE THE METATILE. The visibility record is one
                    // byte per METATILE -- 3 tiles across, 2 down -- while the
                    // cartridge's BG2 is per tile. If a fog boundary can fall
                    // inside a metatile then the record cannot represent it and
                    // these misses must cluster on one row parity or column
                    // residue. If they are spread evenly the record is simply
                    // wrong and the resolution is not the story.
                    ++g_fog_defect_phase[((ar % 2) + 2) % 2 * 3 +
                                         (((ac % 3) + 3) % 3)];
                    static unsigned shown = 0;
                    if (shown < 8u) {
                        ++shown;
                        std::fprintf(stderr,
                                     "[lb_fogbad] abs (%d,%d) %s ours %03X "
                                     "theirs %03X\n", ac, ar,
                                     band_name(int((rec >> 11) & 7u)),
                                     unsigned(ours), unsigned(theirs));
                    }
                } else if (ours_fog && theirs_fog) {
                    if (ours != theirs) ++g_fog_variant;
                    else ++g_fog_agree_shrouded;
                }
                g_fog_served[i] = 0u;          // score each cell once
            }
        }
        static unsigned last_fog = 0;
        if (g_fog_chk - last_fog >= 200u) {
            last_fog = g_fog_chk;
            std::fprintf(stderr,
                         "[lb_fogtruth] %u band fog cells later seen in the "
                         "guest's window: %u showed open ground the cartridge "
                         "calls shrouded (%.1f%%)   LEFT %u RIGHT %u ABOVE %u "
                         "BELOW %u   [%u drawn shrouded and MATCHING, %u "
                         "explained by exploration, %u cloud variant]"
                         "   by metatile phase %u %u %u / %u %u %u"
                         "   cartridge itself: %u revealed, %u RE-SHROUDED"
                         " (nonzero re-shroud invalidates the defect count)\n",
                         g_fog_chk, g_fog_defect,
                         100.0 * double(g_fog_defect) / double(g_fog_chk),
                         g_fog_defect_kind[kBandLeft],
                         g_fog_defect_kind[kBandRight],
                         g_fog_defect_kind[kBandAbove],
                         g_fog_defect_kind[kBandBelow],
                         g_fog_agree_shrouded, g_fog_explained, g_fog_variant,
                         g_fog_defect_phase[0], g_fog_defect_phase[1],
                         g_fog_defect_phase[2], g_fog_defect_phase[3],
                         g_fog_defect_phase[4], g_fog_defect_phase[5],
                         g_cart_reveal, g_cart_reshroud);
            std::fflush(stderr);
        }
    }

    unsigned verdict = 0, tile = 0, guest_fogged = 0;
    int first_r = -1, first_c = -1;
    uint16_t first_ours = 0, first_theirs = 0;
    // WHICH TILES ARE WE FAILING TO PRODUCE? A count of mismatches alone
    // cannot tell "the edge variant is slightly off" from "the guest is
    // drawing a tile this port has never heard of", and those want opposite
    // fixes. Eight slots is plenty: the fog set is six interior tiles plus
    // whatever else turns up, and "whatever else" is the interesting part.
    uint16_t miss_val[8] = {};
    unsigned miss_cnt[8] = {};
    for (int trow = 0; trow < 24; ++trow) {
        for (int h = 0; h < 32; ++h) {
            const uint16_t ours =
                g_ext2[trow - kExtRowLo][h - kExtLo] & 0x3FFu;
            const uint16_t theirs = uint16_t(nds_vram_read_bg(
                0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
            if (theirs != 0u) ++guest_fogged;
            if (ours == theirs) continue;
            for (int i = 0; i < 8; ++i) {
                if (miss_cnt[i] && miss_val[i] != theirs) continue;
                miss_val[i] = theirs;
                ++miss_cnt[i];
                break;
            }
            // Tile 0 is "nothing drawn here" on this layer for both sides, so
            // one being 0 and the other not is a disagreement about WHETHER
            // the cell is fogged, not about which cloud tile to use.
            if ((ours == 0u) != (theirs == 0u)) ++verdict;
            else ++tile;
            if (first_r < 0) {
                first_r = trow;
                first_c = h;
                first_ours = ours;
                first_theirs = theirs;
            }
        }
    }
    g_fa_verdict += verdict;
    g_fa_tile += tile;
    g_fa_cells += 768u;
    if ((verdict || tile) && (g_scan_frames % 60u) == 0u) {
        char m[96];
        int n = 0;
        for (int i = 0; i < 8 && miss_cnt[i]; ++i)
            n += std::snprintf(m + n, sizeof(m) - size_t(n), "%03X:%u ",
                               miss_val[i], miss_cnt[i]);
        m[n] = 0;
        std::fprintf(stderr,
                     "[lb_fogaudit] frame %u: %u verdict + %u tile of 768 "
                     "(guest fogs %u), first r%d c%d ours %03X theirs %03X, "
                     "missing %s cam %d,%d\n",
                     g_scan_frames, verdict, tile, guest_fogged, first_r,
                     first_c, first_ours, first_theirs, m, g_cam_tx, g_cam_ty);
        // WHEN THE COUNT IS SMALL, NAME EVERY CELL.
        //
        // A handful of wrong cloud tiles is either "the edge table has not seen
        // this neighbourhood yet", which fixes itself as the player pans, or
        // "the mask this port computes is not the one the cartridge keys on",
        // which never fixes itself. The two are told apart by whether the table
        // has an entry for the mask -- so print the mask, the position inside
        // the metatile, and the observation count for that slot.
        if (tile && tile <= 16u)
            for (int trow = 0; trow < 24; ++trow)
                for (int h = 0; h < 32; ++h) {
                    const uint16_t o =
                        g_ext2[trow - kExtRowLo][h - kExtLo] & 0x3FFu;
                    const uint16_t t = uint16_t(nds_vram_read_bg(
                        0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
                    if (o == t || (o == 0u) != (t == 0u)) continue;
                    const int ar = g_cam_ty + trow, ac = g_cam_tx + h;
                    const int pos =
                        floor_mod(ar, 2) * 3 + floor_mod(ac, 3);
                    const uint8_t mk = metatile_mask(floor_div(ac, 3),
                                                     floor_div(ar, 2));
                    std::fprintf(stderr,
                                 "[lb_fogcell] r%2d c%2d mt %d,%d pos %d "
                                 "mask %02X seen %u  ours %03X theirs %03X\n",
                                 trow, h, floor_div(ac, 3), floor_div(ar, 2),
                                 pos, mk, g_fog_edge_seen[mk][pos], o, t);
                }
    } else if (!verdict && !tile && (g_scan_frames % 300u) == 0u)
        std::fprintf(stderr,
                     "[lb_fogaudit] frame %u: EXACT on %u fogged cells, "
                     "running %u verdict + %u tile over %u cells\n",
                     g_scan_frames, guest_fogged, g_fa_verdict, g_fa_tile,
                     g_fa_cells);
}

// Read the guest's own fog, at the moment it is about to be drawn.
//
// Called from the top of engine A's frame rather than from vblank. That
// distinction is the whole point: on vblank the staging buffers are still being
// written, and every reading taken there said "everything is shrouded" even for
// frames showing revealed ground. Here VRAM holds exactly the picture.
// THE RECORD FOR THIS FRAME, as a per-metatile bitmap, optionally DILATED.
//
// 1 = treat as shrouded. Built once per frame from g_map_w * g_map_h byte
// reads (4096 on a 64x64 map) so the fill can consult a neighbourhood without
// nine bus reads per cell -- the fill visits 26329 cells, which would be a
// quarter of a million reads a frame.
//
// WHY DILATE. The record is one byte per METATILE, the cartridge's BG2 one
// entry per TILE, so a fog boundary that falls inside a metatile cannot be
// represented. Measured against the cartridge over the guest's own window,
// where the cartridge is ground truth in the same frame, the adopted record
// leaves 3.9% of tiles open where the cartridge fogs them, and that is its
// BEST alignment -- every neighbouring origin scores worse, so it is a
// resolution limit, not a misalignment. No candidate the search finds does
// better than 1.0% holes, and that one over-fogs 3.9%.
//
// The two errors are not equal. Showing open ground the game is hiding is
// reconnaissance the player has not earned; drawing fog over ground that is
// actually clear costs a little visibility at a boundary. The fog must have
// no holes over untraversed terrain (that would be cheating), so the
// uncertainty goes one way: a metatile touching hidden ground is drawn
// hidden.

// Fingerprint of g_vis_bits, and what the band tables were last built
// from. See the skip in lb_widemap_scan.
uint32_t g_vis_sig = 0;
uint32_t g_ext_fill_sig = 0xFFFFFFFFu;
int g_ext_fill_tx = INT32_MIN, g_ext_fill_ty = INT32_MIN;
bool g_ext_fill_fog = false;

void build_vis_bits(void) {
    if (!g_vis_base || g_map_w <= 0 || g_map_h <= 0) { g_vis_bits.clear(); return; }
    const size_t n = size_t(g_map_w) * size_t(g_map_h);
    static std::vector<uint8_t> raw;
    if (raw.size() != n) raw.assign(n, 1u);
    if (g_vis_bits.size() != n) g_vis_bits.assign(n, 1u);
    for (int my = 0; my < g_map_h; ++my)
        for (int mx = 0; mx < g_map_w; ++mx)
            raw[size_t(my) * size_t(g_map_w) + size_t(mx)] =
                vis_revealed(mx, my) ? 0u : 1u;
    // OFF BY DEFAULT. Dilation trades holes for over-fog -- 3.9% holes and
    // 0.0% over-fog becomes 1.3% and 8.6% -- and fog draws IN FRONT of the 3D
    // layer the units are on, so that extra metatile of cloud eats into unit
    // and building sprites at a fog edge. The build that had this on showed
    // exactly that: unit and building sprites truncating as the camera
    // panned. Cutting the player's own base in half is
    // not a good trade for 2.6 points of holes.
    //
    // The real fix is a visibility record that is right, not a thicker cloud
    // over a wrong one. LB_FOGDILATE=1 to try it.
    static const int dilate = [] {
        const char* v = std::getenv("LB_FOGDILATE");
        return (v && v[0] != "0"[0]) ? 1 : 0;
    }();
    for (int my = 0; my < g_map_h; ++my) {
        for (int mx = 0; mx < g_map_w; ++mx) {
            uint8_t s = raw[size_t(my) * size_t(g_map_w) + size_t(mx)];
            if (!s && dilate) {
                for (int dy = -1; dy <= 1 && !s; ++dy)
                    for (int dx = -1; dx <= 1 && !s; ++dx) {
                        const int qx = mx + dx, qy = my + dy;
                        // Off the map counts as hidden, which is what the
                        // widened columns already assume at a map edge.
                        if (qx < 0 || qy < 0 || qx >= g_map_w || qy >= g_map_h)
                            s = 1u;
                        else if (raw[size_t(qy) * size_t(g_map_w) + size_t(qx)])
                            s = 1u;
                    }
            }
            g_vis_bits[size_t(my) * size_t(g_map_w) + size_t(mx)] = s;
        }
    }
    // A FINGERPRINT OF THE VISIBILITY, so the band fill can skip a frame that
    // would produce exactly the table it already has. See g_ext_fill_sig.
    uint32_t h = 2166136261u;
    for (uint8_t v : g_vis_bits) { h ^= v; h *= 16777619u; }
    g_vis_sig = h;
}


int fogedge(void) {
    static const int on = [] {
        const char* v = std::getenv("LB_FOGEDGE");
        return (v && v[0] != "0"[0]) ? 1 : 0;
    }();
    return on;
}

// ── The building-placement footprint ─────────────────────────────────────────
//
// BG3 engine A carries NOTHING BUT this footprint. docs/306 traced the upload:
// func_020396E0 -> func_0210CBE0 -> func_0210BD78, which reads BG3CNT at
// 0x0400000E. And func_020370B4 clears the buffer to 0x001F -- the same
// kShroudTile this file already hands BG3's band columns -- then writes 0x4F
// (placeable) or 0x3F (blocked) for each footprint cell.
//
// So the port's standing measurement that "BG3 is uniformly inert" is true
// only while nothing is being placed, and that is the SECOND cause of the
// missing band footprint, stacked on the guest's own cull of any cell outside 32x24 (docs/286).
//
// THIS IS A READ ONLY. It changes no guest state and makes no guest call; it
// exists so the next step works from what the footprint actually IS rather
// than from what the disassembly says it should be.
const uint32_t kFpRowTable = 0x021520F8u;  // row base per y (func_02038018)
const int kFpRows = 26;                    // 26 rows of
const int kFpCols = 32;                    //   32 halfword cells

// OFF BY DEFAULT AND FREE WHEN OFF. Instrumentation must not cost the game
// anything, so the whole scan sits
// behind one cached bool. With it on, the cost is 832 halfword reads on the
// frame the summary CHANGES -- the compare is the same walk, and a summary
// that matches the last one prints nothing, so a static footprint is silent.
bool fp_logging() {
    static const bool on = [] {
        const char* v = std::getenv("LB_FPLOG");
        return v && v[0] == '1';
    }();
    return on;
}

void footprint_probe(void) {
    if (!fp_logging()) return;
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return;

    // IS PLACEMENT EVEN ACTIVE? Ask the game, instead of inferring it from
    // whether anything painted.
    //
    // func_020398B8 is the view tick, and it reaches the painter like this:
    //
    //     ldrb r0, [r4, #0x78]     ; r4 = the view
    //     cmp  r0, #0
    //     beq  <skip>
    //     add  r0, r4, #0x6C       ; the placement state is the view's own
    //     bl   func_02037EC0       ;   sub-struct, not a separate object
    //
    // So `view+0x78` is the placement flag and `view+0x6C` is the state
    // func_020370B4 paints from. docs/286 read that state's layout as +0x04/
    // +0x05 origin tile and +0x20/+0x21 footprint size; that is BACKWARDS
    // (docs/392): the painter loops x from +0x20 to +0x20 + +0x04, so +0x20/
    // +0x21 is the origin in map metatiles and +0x04/+0x05 the size, as the
    // checkmark code (func_02036FBC) and lb_fp_set_site_size already read it.
    // +0x10 is the mode and +0x24 the whole-site verdict func_02037EC0 stores
    // back.
    //
    // This distinction is the point. An empty buffer means "nothing painted",
    // which is equally consistent with "placement never started" and "the
    // taps landed but the footprint was culled" -- and docs/307 spent three
    // passes unable to tell those apart from the buffer alone.
    const uint32_t ps = view + 0x6Cu;
    const int active = static_cast<int>(bus_read_u8(view + 0x78u));
    const int mode = static_cast<int>(bus_read_u8(ps + 0x10u));
    const int org_x = static_cast<int>(bus_read_u8(ps + 0x20u));
    const int org_y = static_cast<int>(bus_read_u8(ps + 0x21u));
    const int fw = static_cast<int>(bus_read_u8(ps + 0x04u));
    const int fh = static_cast<int>(bus_read_u8(ps + 0x05u));
    // docs/392: a wall line's END (func_02036F1C), its start being +0x20/+0x21.
    const int end_x = static_cast<int>(bus_read_u8(ps + 0x18u));
    const int end_y = static_cast<int>(bus_read_u8(ps + 0x19u));
    const int verdict = static_cast<int>(bus_read_u8(ps + 0x24u));

    // THE PLACEMENT UI RECORD, and it is NOT view+0x6C.
    //
    // Everything above reads `view+0x6C`, and a whole run of it is constant --
    // `origin=2,2 size=16x15 verdict=1` on every sample, active or not (under
    // the swapped labels above: a 2x2 site at 16,15). The
    // record the placement UI actually drives lives at 0x02157958, and the two
    // functions that own it say what each byte is:
    //
    //   func_020AA910 (ENTRY, thumb):
    //     func_02039f5c(&pt, view, 0x80, 0x60)   ; screen CENTRE, 128,96
    //     rec[0x0C] = pt.x ; rec[0x0D] = pt.y    ; the ghost's origin cell
    //     rec[0x03] = kind[0] ; rec[0x04] = kind[1]
    //     rec[0x01] = 0 ; rec[0x02] = 0
    //     *(0x0214BB84+4) = the building definition
    //
    //   func_020AA9F8 (TICK, thumb):
    //     rec[0x00] == 0 on the first tick, then set to 1
    //     rec[0x01] != 0  -> the whole tick early-outs: placement is finished
    //     rec[0x14]/rec[0x15] = the cursor cell, from func_020AA814
    //     first tick: if the cursor is OUTSIDE [origin, origin+size) the
    //       origin is recomputed from the cursor and *(0x0214BB84) = 0;
    //       if it is INSIDE, *(0x0214BB84) = 1 and the origin is left alone
    //     later ticks: the cursor delta is accumulated, so the ghost DRAGS
    //
    // So the ghost is cursor-driven, it starts at the screen centre, and
    // `0x0214BB84[0]` is "the cursor is on the ghost". Printing these is what
    // separates "the ghost did not move" from "the tap was inside the ghost".
    const uint32_t kPlaceRec = 0x02157958u;
    const uint32_t kPlaceDef = 0x0214BB84u;
    const int ph_phase = static_cast<int>(bus_read_u8(kPlaceRec + 0x00u));
    const int ph_done = static_cast<int>(bus_read_u8(kPlaceRec + 0x01u));
    const int ph_kind = static_cast<int>(bus_read_u8(kPlaceRec + 0x03u));
    const int ph_ox = static_cast<int>(bus_read_u8(kPlaceRec + 0x0Cu));
    const int ph_oy = static_cast<int>(bus_read_u8(kPlaceRec + 0x0Du));
    const int ph_cx = static_cast<int>(bus_read_u8(kPlaceRec + 0x14u));
    const int ph_cy = static_cast<int>(bus_read_u8(kPlaceRec + 0x15u));
    const int ph_hover = static_cast<int>(bus_read_u8(kPlaceDef));

    int cells = 0, lo_x = kFpCols, hi_x = -1, lo_y = kFpRows, hi_y = -1;
    // The footprint's OWN bounding box, over placeable|blocked cells only.
    // The box above spans every non-inert cell, and rows 24..25 are
    // permanently 0x0000 -- 64 cells across the full width -- so it reads
    // 0,..31,25 whatever the footprint does. That is what made five different
    // cursor positions look identical in docs/307.
    int fp_lo_x = kFpCols, fp_hi_x = -1, fp_lo_y = kFpRows, fp_hi_y = -1;
    int placeable = 0, blocked = 0, other = 0;
    uint16_t sample = 0u;
    for (int y = 0; y < kFpRows; ++y) {
        // Through the ROW TABLE, not a computed stride. docs/286 found the
        // geometry lives entirely in this table -- func_0203D8E8 hardcodes no
        // stride at all -- so reading it the guest's way is what keeps this
        // honest if the table ever moves.
        const uint32_t row = bus_read_u32(kFpRowTable + uint32_t(y) * 4u);
        if (!main_ram(row)) return;
        for (int x = 0; x < kFpCols; ++x) {
            const uint16_t t = bus_read_u16(row + uint32_t(x) * 2u);
            if (t == kShroudTile) continue;
            ++cells;
            if (x < lo_x) lo_x = x;
            if (x > hi_x) hi_x = x;
            if (y < lo_y) lo_y = y;
            if (y > hi_y) hi_y = y;
            // THE FOOTPRINT'S OWN BOX, separate from the box above.
            //
            // The box above spans every non-inert cell, and rows 24..25 are
            // permanently 0x0000 because func_020370B4 clears only 0x600
            // bytes (24 rows) while the row table builds 26. That junk is 64
            // cells across the full width, so it pins the reported box at
            // 0,..31,25 whatever the footprint does -- which is exactly what
            // made five different cursor positions look identical in
            // docs/307. The placeable/blocked box is the one that moves.
            if ((t & 0x3FFu) == 0x4Fu || (t & 0x3FFu) == 0x3Fu) {
                if (x < fp_lo_x) fp_lo_x = x;
                if (x > fp_hi_x) fp_hi_x = x;
                if (y < fp_lo_y) fp_lo_y = y;
                if (y > fp_hi_y) fp_hi_y = y;
            }
            if ((t & 0x3FFu) == 0x4Fu)      ++placeable;
            else if ((t & 0x3FFu) == 0x3Fu) ++blocked;
            else { ++other; sample = t; }
        }
    }

    // The flag is part of the key, so entering or leaving placement reports
    // even when nothing paints -- which is the case docs/307 could not see.
    static int last_cells = -1, last_lo_x = -2, last_lo_y = -2;
    static int last_hi_x = -2, last_hi_y = -2, last_active = -1;
    // The FOOTPRINT box is part of the key too, so a ghost that moves without
    // changing its cell count still reports.
    static int last_fp = -1;
    // The UI record is part of the key as well, so a cursor that moves without
    // moving the ghost -- the case that tells a failed delivery apart from a
    // tap that landed on the ghost -- still reports.
    static int last_rec = -1;
    const int fp_key = ((fp_lo_x + 2) * 100 + (fp_lo_y + 2)) * 10000 +
                       (fp_hi_x + 2) * 100 + (fp_hi_y + 2);
    const int rec_key = (((ph_phase * 2 + ph_done) * 2 + ph_hover) * 256 +
                         ph_ox) * 256 + ph_oy;
    static int last_end = -1;
    const int end_key = end_x * 256 + end_y;
    if (cells == last_cells && lo_x == last_lo_x && lo_y == last_lo_y &&
        hi_x == last_hi_x && hi_y == last_hi_y && active == last_active &&
        fp_key == last_fp && rec_key == last_rec && end_key == last_end)
        return;
    last_cells = cells; last_lo_x = lo_x; last_lo_y = lo_y;
    last_hi_x = hi_x; last_hi_y = hi_y; last_active = active;
    last_fp = fp_key; last_rec = rec_key; last_end = end_key;

    if (cells == 0) {
        std::fprintf(stderr,
                     "[lb_fp] no cells painted -- active=%d mode=%d "
                     "origin=%d,%d size=%dx%d end=%d,%d verdict=%d  "
                     "rec phase=%d done=%d kind=%d org=%d,%d cur=%d,%d "
                     "hover=%d\n",
                     active, mode, org_x, org_y, fw, fh, end_x, end_y, verdict,
                     ph_phase, ph_done, ph_kind, ph_ox, ph_oy, ph_cx, ph_cy,
                     ph_hover);
        std::fflush(stderr);
        return;
    }
    // The camera is what decides whether a site was culled, so it is printed
    // beside the box. +0x5E/+0x60 are s16 and func_0203CBD8 shifts them left
    // by 12 to make the 12.12 the conversion wants, so these are the raw
    // integers the guest stores.
    std::fprintf(stderr,
                 "[lb_fp] cells=%d box=%d,%d..%d,%d  placeable=%d blocked=%d "
                 "other=%d  cam=%d,%d  touching-edge=%c  active=%d mode=%d "
                 "origin=%d,%d size=%dx%d end=%d,%d verdict=%d  rec "
                 "phase=%d done=%d kind=%d org=%d,%d cur=%d,%d hover=%d  "
                 "FOOTPRINT %d,%d..%d,%d%s\n",
                 cells, lo_x, lo_y, hi_x, hi_y, placeable, blocked, other,
                 read_s16(view + 0x5Eu), read_s16(view + 0x60u),
                 (lo_x == 0 || hi_x == kFpCols - 1 || lo_y == 0 ||
                  hi_y == 23) ? 'y' : 'n',
                 active, mode, org_x, org_y, fw, fh, end_x, end_y, verdict,
                 ph_phase, ph_done, ph_kind, ph_ox, ph_oy, ph_cx, ph_cy,
                 ph_hover,
                 fp_lo_x, fp_lo_y, fp_hi_x, fp_hi_y,
                 (fp_hi_x >= kFpCols - 1 || fp_hi_y >= 23 || fp_lo_x <= 0 ||
                  fp_lo_y <= 0) ? "  CLIPPED AT THE CULL EDGE" : "");
    if (other)
        std::fprintf(stderr, "[lb_fp]   unexpected tile 0x%04X\n", sample);
    std::fflush(stderr);
}

// ── The placement footprint in the BANDS (the fix) ─────────────────────────
//
// docs/318 measured the defect end to end, with the ghost dragged across a
// fixed camera:
//
//   native x  12   FOOTPRINT 0,12..1,15    8 cells of 24   CLIPPED
//   native x -60   nothing painted         verdict STILL 1
//   native x 243   FOOTPRINT 26,12..31,15                  CLIPPED
//   native x 315   nothing painted
//
// The guest evaluates the site out there and decides the building CAN go
// there, and draws nothing saying so. Two independent causes, both measured:
// func_020370B4 writes only into a 32x26 buffer covering the native 256 px,
// and BG3 -- which carries nothing but this footprint -- is not extended, so
// the bands have no BG3 source at all.
//
// This is docs/306 design (3). Shift the guest's own camera by whole
// metatiles so a band lands inside that buffer, call the guest's own painter,
// harvest what it wrote, restore. EVERY PER-CELL VERDICT IS STILL THE
// CARTRIDGE'S -- nothing about the placement predicate is replicated here,
// which is the constraint that rejected designs (1) and (2).
//
// A metatile is 24 px and a buffer cell is 8, so a shift in multiples of 24
// preserves both the cell phase and the `4 - (camx % 8)` BGxOFS bias the
// guest applies.
// The band width in guest pixels, latched by lb_band_latch further down this
// file. Declared here because this block sits above it.
extern int g_band_left;

const int kFpMeta = 24;     // a metatile across, in guest px
const int kFpMetaY = 16;    // and down
const int kFpPaintRows = 24;   // func_020370B4 clears and paints rows 0..23

// ONE HARVEST, SHIFTED IN BOTH AXES (docs/383).
//
// The red/blue building placement and its cursor were still not fully
// consistent in the bands. docs/320's harvest shifted the camera
// horizontally only, by the band capped at ten metatiles (240 px). Two places
// were therefore never painted while the checkmark (docs/372) was still shown
// beside the site:
//   * ABOVE and BELOW the DS's own 192 rows -- no vertical shift existed, and
//     the compositor cleared every BG3 band row outright;
//   * the OUTER part of a side band wider than 240 px -- a 940-wide
//     surface has 342-px bands, so a site more than 240 px out was outside
//     every harvest window.
// Now the shift is chosen from the SITE: whole metatiles in each axis, so that
// the site lands two metatiles in from the painter window's top-left, wherever
// it is in the widened view. Every cell value is still the cartridge's own
// painter's; only which window it paints is arranged.
static uint16_t g_fp_h[kFpRows * kFpCols];
static int g_fp_hx = 0, g_fp_hy = 0;       // the camera shift it was painted at
static bool g_fp_h_valid = false;
static int g_fp_h_cells = 0;
// Latched once a frame, because lb_widemap_extends is asked per scanline and a
// value that flips mid-render would draw one frame with two geometries -- the
// same fault lb_band_for_geometry documents.
static bool g_fp_bands_live = false;
static bool g_fp_bands_latched = false;
static unsigned g_fp_harvests = 0;
// Cells this file has actually handed the compositor for a band cell.
static unsigned g_fp_served = 0;
// The site's size in metatiles, from func_02036FBC's state (+4/+5) at the last
// post hook. The painter runs before the post hook in the same call, so this
// is the size of the building being placed from its second frame on; 2x2
// until then.
static int g_fp_site_w = 2, g_fp_site_h = 2;

// ON BY DEFAULT, as docs/320 decided, with the same off switch: LB_FPBANDS=0
// makes lb_fp_placement_active() return 0, the painter override a pure tail
// call, and lb_widemap_extends(3) 0.
static bool fp_bands_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LB_FPBANDS");
        return !(v && v[0] == '0');
    }();
    return on;
}

extern "C" int lb_fp_log(void) { return fp_logging() ? 1 : 0; }

// Is the guest placing a building right now? Its own flag, not an inference
// from whether anything painted -- docs/307 spent three passes unable to tell
// "placement never started" from "the footprint was culled".
extern "C" int lb_fp_placement_active(void) {
    if (!fp_bands_enabled()) return 0;
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return 0;
    return bus_read_u8(view + 0x78u) ? 1 : 0;
}

extern "C" void lb_fp_set_site_size(int w, int h) {
    if (w >= 1 && w <= 8) g_fp_site_w = w;
    if (h >= 1 && h <= 8) g_fp_site_h = h;
}

static int fp_floor_div(int a, int b) {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

// DOES THE SITE NEED A SHIFTED PAINT, AND BY HOW MUCH.
//
// Returns 1 with the shift in guest px when any part of the site is outside
// the painter's own window (32 cells x 24 rows, the DS's 256x192), 0 when the
// guest's own paint already holds all of it -- the common case, which then
// costs nothing. `rec[0x0C]/[0x0D]` is the site's origin in MAP metatiles
// (docs/318), and the camera is view+0x5E/+0x60 in guest px.
// docs/392: IS THIS A WALL OR BRIDGE LINE, NOT A BUILDING'S GHOST? The site
// below is the building-placement UI record at 0x02157958 (FUN_020AA910 sets
// its origin and kind at entry, FUN_020AA9F8 drags it). A wall or bridge is
// laid by dragging a line, and that placement never touches the record: it
// reads origin 0,0 and kind 0 the whole time (runs t392e-i, LB_FPLOG), while
// view+0x78 says a placement is active. Taking 0,0 for the site made every
// other frame paint at a camera shifted by the whole map (-720,-512): no
// bridge preview showed at all, and only half a wall's.
//
// The painter's own record is what a line's site comes from instead.
// func_020370B4 paints ONE rectangle per call, from its argument view+0x6C:
// origin +0x20/+0x21 in map metatiles, size +4/+5 (FUN_020f2a50 on each pair,
// then x from origin to origin+size; footprint_probe prints the two pairs
// under each other's names). During the bridge drag of run t392i the origin
// walked 34..41,33 with the stylus at size 1x2, and the cells painted were
// exactly (ox*24 - camx)/8 and (oy*16 - camy)/8: 8..10 x 12..15 at 34,33
// under camera 752,432. So the guest's bridge preview IS one 1x2 block at the
// cursor, and the band is harvested for it like a building's ghost.
extern "C" int lb_fp_line_placement(void) {
    const uint32_t rec = 0x02157958u;
    return (bus_read_u8(rec + 0x0Cu) == 0u && bus_read_u8(rec + 0x0Du) == 0u &&
            bus_read_u8(rec + 0x03u) == 0u) ? 1 : 0;
}

extern "C" int lb_demo_play_fp2d(void);     /* src/lb_demo.cpp */
extern "C" int lb_demo_play_fpline(void);   /* docs/392 */
extern "C" int lb_fp_band_shift2(int* sx, int* sy) {
    if (!g_geom_valid || !fp_bands_enabled()) return 0;
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return 0;
    if (!bus_read_u8(view + 0x78u)) return 0;
    const int camx = read_s16(view + 0x5Eu);
    const int camy = read_s16(view + 0x60u);
    int ox = static_cast<int>(bus_read_u8(0x02157958u + 0x0Cu));
    int oy = static_cast<int>(bus_read_u8(0x02157958u + 0x0Du));
    // A replay of a recording made before docs/392 keeps the ghost's 0,0.
    if (lb_demo_play_fpline() != 0 && lb_fp_line_placement()) {
        ox = static_cast<int>(bus_read_u8(view + 0x6Cu + 0x20u));
        oy = static_cast<int>(bus_read_u8(view + 0x6Cu + 0x21u));
    }
    // A RECORDING MADE BEFORE docs/383 replays docs/320's rule exactly: the
    // camera moves in X only, by the band capped at ten metatiles, when the
    // site's origin is under one metatile from the left or past nine. The
    // painter's work under that camera is what the recording was paced by.
    // LB_FP2D=0 asks for the same rule live, for the A/B.
    static const bool fp2d_env = [] {
        const char* v = std::getenv("LB_FP2D");
        return !(v && v[0] == '0');
    }();
    if (lb_demo_play_fp2d() == 0 || !fp2d_env) {
        const int screen_mt = ox - camx / kFpMeta;
        const int need = (screen_mt < 1) ? 0 : (screen_mt > 9 ? 1 : -1);
        if (need < 0) return 0;
        const int band = g_band_left;
        if (band <= 0) return 0;
        int steps = (band + kFpMeta - 1) / kFpMeta;
        if (steps > 10) steps = 10;
        if (sx) *sx = (need == 0 ? -steps : steps) * kFpMeta;
        if (sy) *sy = 0;
        return 1;
    }
    const int px0 = ox * kFpMeta - camx, py0 = oy * kFpMetaY - camy;
    const int px1 = px0 + g_fp_site_w * kFpMeta;
    const int py1 = py0 + g_fp_site_h * kFpMetaY;
    if (px0 >= 0 && px1 <= 256 && py0 >= 0 && py1 <= kFpPaintRows * 8)
        return 0;
    // Two metatiles in from the window's left and top: room for the widest
    // building (and the checkmark beside it) on every side.
    const int wx = fp_floor_div(px0 - 2 * kFpMeta, kFpMeta) * kFpMeta;
    const int wy = fp_floor_div(py0 - 2 * kFpMetaY, kFpMetaY) * kFpMetaY;
    if (sx) *sx = wx;
    if (sy) *sy = wy;
    return (wx != 0 || wy != 0) ? 1 : 0;
}

extern "C" int lb_fp_cam_x(void) {
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return INT_MIN;
    return read_s16(view + 0x5Eu);
}

extern "C" int lb_fp_cam_y(void) {
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return INT_MIN;
    return read_s16(view + 0x60u);
}

// +0x5E and +0x60 are HALFWORDS: func_0203CBD8 reads them with ldrsh before
// shifting them left by 12. Writing any other width is the trap this project
// has already lost three answers to.
extern "C" void lb_fp_cam_set_x(int px) {
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return;
    bus_write_u16(view + 0x5Eu, static_cast<uint16_t>(px & 0xFFFF));
}

extern "C" void lb_fp_cam_set_y(int px) {
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return;
    bus_write_u16(view + 0x60u, static_cast<uint16_t>(px & 0xFFFF));
}

// Read the buffer the guest has just painted under a shifted camera. Through
// the row table, for the same reason footprint_probe does: func_0203D8E8
// hardcodes no stride at all.
extern "C" void lb_fp_harvest2(int sx, int sy) {
    int n = 0;
    for (int y = 0; y < kFpRows; ++y) {
        const uint32_t row = bus_read_u32(kFpRowTable + uint32_t(y) * 4u);
        if (!main_ram(row)) { g_fp_h_valid = false; return; }
        for (int x = 0; x < kFpCols; ++x) {
            const uint16_t t = bus_read_u16(row + uint32_t(x) * 2u);
            g_fp_h[y * kFpCols + x] = t;
            if ((t & 0x3FFu) == 0x4Fu || (t & 0x3FFu) == 0x3Fu) ++n;
        }
    }
    g_fp_hx = sx;
    g_fp_hy = sy;
    g_fp_h_cells = n;
    g_fp_h_valid = true;
    g_fp_bands_live = true;
    ++g_fp_harvests;
    if (fp_logging()) {
        static int last[3] = {-1, INT_MIN, INT_MIN};
        if (n != last[0] || sx != last[1] || sy != last[2]) {
            last[0] = n; last[1] = sx; last[2] = sy;
            std::fprintf(stderr,
                         "[lb_fp] harvest: %d footprint cell(s) at camera "
                         "shift %+d,%+d px  harvests=%u extends3=%d "
                         "served=%u\n", n, sx, sy, g_fp_harvests,
                         g_fp_bands_latched ? 1 : 0, g_fp_served);
            std::fflush(stderr);
        }
    }
}

extern "C" int lb_fp_native_ready(void) { return g_fp_h_valid ? 1 : 0; }

// REBUILD THE NATIVE WINDOW FROM THE HARVEST, EXACTLY (docs/320's argument,
// in both axes). The shift is whole cells -- sx/8 across, sy/8 down -- so a
// harvested cell (x, y) is native cell (x + sx/8, y + sy/8), and the ones that
// land inside 0..31 x 0..23 are precisely what the guest would have painted
// itself. Everything else in the native window is inert. Rows 0..23 only:
// func_020370B4 clears 0x600 bytes, 24 rows, and leaves 24 and 25 as they are.
extern "C" int lb_fp_native_rebuild2(int sx, int sy) {
    if (!g_fp_h_valid) return 0;
    const int dcol = sx / 8, drow = sy / 8;
    for (int y = 0; y < kFpPaintRows; ++y) {
        const uint32_t row = bus_read_u32(kFpRowTable + uint32_t(y) * 4u);
        if (!main_ram(row)) return 0;
        for (int x = 0; x < kFpCols; ++x)
            bus_write_u16(row + uint32_t(x) * 2u, kShroudTile);
    }
    for (int y = 0; y < kFpPaintRows; ++y) {
        const int ny = y + drow;
        if (ny < 0 || ny >= kFpPaintRows) continue;
        const uint32_t row = bus_read_u32(kFpRowTable + uint32_t(ny) * 4u);
        if (!main_ram(row)) return 0;
        for (int x = 0; x < kFpCols; ++x) {
            const int nx = x + dcol;
            if (nx < 0 || nx >= kFpCols) continue;
            bus_write_u16(row + uint32_t(nx) * 2u, g_fp_h[y * kFpCols + x]);
        }
    }
    return 1;
}

extern "C" void lb_fp_band_clear(void) {
    g_fp_h_valid = false;
    g_fp_h_cells = 0;
    g_fp_bands_live = false;
}

// ── A WALL LINE'S PIECES, FROM THE CARTRIDGE'S OWN CELL PAINTER (docs/392) ──
//
// A wall is not painted by func_020370B4 at all. func_02037EC0 sends mode 1
// (view+0x6C+0x10, set by the wall tick func_020AB384 through func_02036F30)
// to func_02037B9C instead, which paints a LINE: from the start +0x20/+0x21 to
// the end +0x18/+0x19 (both from the cursor function func_020AA814), max(|dx|,
// |dy|) steps of one metatile, each piece's tile (0x4F placeable, 0x3F not)
// decided by the cartridge and handed to func_0203CBB8 at 02037E2C. A line in
// the widened view is longer than any one painter window, so the camera-shift
// harvest above cannot hold it; and it needs none, because func_0203CBB8 is
// told every piece -- including the ones it then culls.
//
// func_0203CBB8 (ARM; view in r0, the cell in r1 at +4/+5, the tile in r2):
//   p = (cell.x * 24, cell.y * 16) - (view+0x5E, view+0x60)   (020147A0..)
//   cells [p/8, (p + (24,16))/8)  -- FX_Div by 8.0 (02007B68 -> 0210A6F4),
//   exact for whole pixels, then >>12, so a floor
//   gone if the end is < 0 or the start past 31,23; else clamped to 32x24
// So each piece's whole rectangle is known in the buffer's own cell frame --
// the frame lb_fp_band_tile is asked in -- and the band serves the part the
// clamp dropped. Every value is the cartridge's; only the cull is undone.
struct FpLinePiece { int16_t x0, y0, x1, y1; uint16_t tile; };
const int kFpLineMax = 128;
static FpLinePiece g_fp_line_pend[kFpLineMax], g_fp_line[kFpLineMax];
static int g_fp_line_pend_n = 0, g_fp_line_n = 0;
static bool g_fp_line_out = false;       // a committed piece is outside 32x24
static bool g_fp_line_latched = false;   // served this render pass
static int16_t g_fp_line_lo_x = 0, g_fp_line_lo_y = 0;
static int16_t g_fp_line_hi_x = 0, g_fp_line_hi_y = 0;

extern "C" void lb_fp_line_note(uint32_t view, int mx, int my, int tile) {
    if (!main_ram(view) || g_fp_line_pend_n >= kFpLineMax) return;
    const int px = mx * kFpMeta - read_s16(view + 0x5Eu);
    const int py = my * kFpMetaY - read_s16(view + 0x60u);
    FpLinePiece& p = g_fp_line_pend[g_fp_line_pend_n++];
    p.x0 = static_cast<int16_t>(fp_floor_div(px, 8));
    p.y0 = static_cast<int16_t>(fp_floor_div(py, 8));
    p.x1 = static_cast<int16_t>(fp_floor_div(px + kFpMeta, 8));
    p.y1 = static_cast<int16_t>(fp_floor_div(py + kFpMetaY, 8));
    p.tile = static_cast<uint16_t>(tile);
}

// From func_02036FBC, which func_02037EC0 calls right after its painter.
// `mode` is the placement state's +0x10. A painter call that noted pieces
// replaces the line; a re-entry of the post hook (a slice yield) finds none
// and keeps it; any other mode retires it.
extern "C" void lb_fp_line_commit(int mode) {
    if (mode != 1) {
        g_fp_line_n = 0;
        g_fp_line_out = false;
        g_fp_line_pend_n = 0;
        return;
    }
    if (g_fp_line_pend_n == 0) return;
    g_fp_line_n = g_fp_line_pend_n;
    g_fp_line_pend_n = 0;
    g_fp_line_out = false;
    for (int i = 0; i < g_fp_line_n; ++i) {
        g_fp_line[i] = g_fp_line_pend[i];
        const FpLinePiece& p = g_fp_line[i];
        if (i == 0 || p.x0 < g_fp_line_lo_x) g_fp_line_lo_x = p.x0;
        if (i == 0 || p.y0 < g_fp_line_lo_y) g_fp_line_lo_y = p.y0;
        if (i == 0 || p.x1 > g_fp_line_hi_x) g_fp_line_hi_x = p.x1;
        if (i == 0 || p.y1 > g_fp_line_hi_y) g_fp_line_hi_y = p.y1;
        if (p.x0 < 0 || p.y0 < 0 || p.x1 > kFpCols || p.y1 > kFpPaintRows)
            g_fp_line_out = true;
    }
    if (fp_logging()) {
        static int last[5] = {-1, 0, 0, 0, 0};
        if (g_fp_line_n != last[0] || g_fp_line_lo_x != last[1] ||
            g_fp_line_lo_y != last[2] || g_fp_line_hi_x != last[3] ||
            g_fp_line_hi_y != last[4]) {
            last[0] = g_fp_line_n; last[1] = g_fp_line_lo_x;
            last[2] = g_fp_line_lo_y; last[3] = g_fp_line_hi_x;
            last[4] = g_fp_line_hi_y;
            std::fprintf(stderr,
                         "[lb_fp] line: %d piece(s), cells %d,%d..%d,%d%s  "
                         "first tile 0x%04X\n", g_fp_line_n, g_fp_line_lo_x,
                         g_fp_line_lo_y, g_fp_line_hi_x - 1,
                         g_fp_line_hi_y - 1,
                         g_fp_line_out ? "  PAST THE WINDOW" : "",
                         g_fp_line[0].tile);
            std::fflush(stderr);
        }
    }
}

static int fp_line_tile(int htile, int trow, uint16_t* out) {
    if (htile < g_fp_line_lo_x || htile >= g_fp_line_hi_x ||
        trow < g_fp_line_lo_y || trow >= g_fp_line_hi_y)
        return 0;
    // Inside the window the guest's own buffer is drawn, never this.
    if (htile >= 0 && htile < kFpCols && trow >= 0 && trow < kFpPaintRows)
        return 0;
    for (int i = g_fp_line_n - 1; i >= 0; --i) {   // later pieces paint over
        const FpLinePiece& p = g_fp_line[i];
        if (htile < p.x0 || htile >= p.x1 || trow < p.y0 || trow >= p.y1)
            continue;
        if ((p.tile & 0x3FFu) != 0x4Fu && (p.tile & 0x3FFu) != 0x3Fu) return 0;
        *out = p.tile;
        ++g_fp_served;
        return 1;
    }
    return 0;
}

// One BG3 cell outside the guest's own 32x24, in the tilemap frame the
// compositor samples (column and row signed, scroll bias applied). Only a
// footprint cell is served; everything else declines, and a declined BG3 band
// cell draws nothing at all rather than the inert tile -- 0x001F is OPAQUE,
// which is the regression docs/205 recorded when this layer was extended.
extern "C" int lb_fp_band_tile(int htile, int trow, uint16_t* out) {
    if (!out) return 0;
    if (g_fp_line_latched) return fp_line_tile(htile, trow, out);   // docs/392
    if (!g_fp_bands_latched || !g_fp_h_valid) return 0;
    const int col = htile - g_fp_hx / 8;
    const int row = trow - g_fp_hy / 8;
    if (col < 0 || col >= kFpCols || row < 0 || row >= kFpPaintRows) return 0;
    const uint16_t t = g_fp_h[row * kFpCols + col];
    if ((t & 0x3FFu) != 0x4Fu && (t & 0x3FFu) != 0x3Fu) return 0;
    *out = t;
    ++g_fp_served;
    return 1;
}

extern "C" int lb_fp_bands_active(void) {
    return (g_fp_bands_latched || g_fp_line_latched) ? 1 : 0;
}

// ---- THE CARTRIDGE'S OWN FOG RULE, FOR THE WIDENED COLUMNS (docs/348) -----
//
// Fog_FillWindow (02039fb4) builds BG2 one metatile at a time.
// Fog_NeighbourCode (0203a318) gives each of five cells -- the metatile and
// its N, W, E and S neighbours, in that bit order, two bits each -- a state:
//
//     2  visible   (the record's +0x14 bit grid, or every cell while the
//                   reveal override is on)
//     1  explored  (the record's +0x10 bit grid)
//     0  neither, or outside the map (FUN_020eec6c: u8 x < w && u8 y < h)
//
// Fog_CodeToTiles (0203a460) maps the 10-bit code through the byte table at
// 0x0213C8FC to a set index, and the set's six tile bytes at 0x0213C7DC
// (index * 6: three across, then the three below) are the BG2 entries. The
// record is *(0x021569E4); each grid is +8 w, +0xC h, +0x14 row pointers, bit
// x&7 of byte x>>3 (FUN_020eec2c).
//
// The tables, read out: 48 sets over the 243 codes. A VISIBLE centre is six
// 0x64 -- the clear tile -- whatever its neighbours. An EXPLORED-BUT-UNSEEN
// centre is the fog-of-war set (0xF5, edge variants EB..FF / AC..B0). An
// UNEXPLORED centre is the cloud (C2 C0 C1 / BD BB BC) with edge variants
// chosen by whether each cardinal neighbour is explored or visible.
//
// The band's older fog knew one bit, explored, and learned its edge tiles by
// watching the guest's own columns. So it drew explored-but-unseen ground
// fully CLEAR where the cartridge beside it draws fog-of-war, and could only
// ever draw edge variants it had happened to see. This is the rule itself,
// from the cartridge's own record and tables, read live -- nothing is copied
// out of the ROM, and the native window grades it every frame (fx_grade).
//
// LB_FOGEXACT=0 falls back to the older path.
namespace {
const uint32_t kFxCodeTable = 0x0213C8FCu;   // Fog_CodeToTiles, DAT_0203a4ac
const uint32_t kFxTileSets  = 0x0213C7DCu;   // Fog_CodeToTiles, DAT_0203a4b0
const int kFxMaxCode = 2 + (2 << 2) + (2 << 4) + (2 << 6) + (2 << 8);   // 682
uint8_t g_fx_code[kFxMaxCode + 1];
uint8_t g_fx_sets[64 * 6];
int g_fx_tables = -1;            // -1 not read yet, 0 refused, 1 good
std::vector<uint8_t> g_fx_state; // per metatile: 0 / 1 / 2, as last read
int g_fx_w = 0, g_fx_h = 0;
uint64_t g_fx_sig = 0;           // fingerprint of both grids as last read
bool g_fx_ok = false;            // the record was readable at the last read
int g_fx_reveal = 0;             // the override, as the cartridge's BG2 shows it
int g_fx_reveal_run = 0;         // signed run of evidence for/against it
unsigned g_fx_graded = 0, g_fx_wrong = 0, g_fx_uiclear = 0, g_fx_frames = 0;
unsigned g_fx_uicells = 0;       // cells skipped: marked in the UI grid

int fx_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_FOGEXACT");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on;
}

// The two tables, read once from the running game's static data. REFUSED --
// and the older path kept -- unless the sets the port has measured on screen
// are where the decomp says they are: code 0 (all five unexplored) is the
// cloud's interior, and an all-visible neighbourhood is six clear tiles.
bool fx_tables(void) {
    if (g_fx_tables >= 0) return g_fx_tables == 1;
    int maxset = 0;
    for (int c = 0; c <= kFxMaxCode; ++c) {
        g_fx_code[c] = bus_read_u8(kFxCodeTable + uint32_t(c));
        if (g_fx_code[c] > maxset) maxset = g_fx_code[c];
    }
    bool ok = maxset < 64;
    if (ok)
        for (int i = 0; i < (maxset + 1) * 6; ++i)
            g_fx_sets[i] = bus_read_u8(kFxTileSets + uint32_t(i));
    static const uint8_t kInterior[6] = {0xC2, 0xC0, 0xC1, 0xBD, 0xBB, 0xBC};
    const int all_visible = kFxMaxCode;   // every field 2
    for (int i = 0; ok && i < 6; ++i) {
        if (g_fx_sets[g_fx_code[0] * 6 + i] != kInterior[i]) ok = false;
        if (g_fx_sets[g_fx_code[all_visible] * 6 + i] != 0x64u) ok = false;
    }
    g_fx_tables = ok ? 1 : 0;
    std::fprintf(stderr, "[lb_fogexact] tables %s: %d sets\n",
                 ok ? "read" : "REFUSED (not where the decomp says)",
                 maxset + 1);
    std::fflush(stderr);
    return ok;
}

// WHICH FAMILY A BG2 TILE BELONGS TO, from those same two tables (docs/350
// section 4). Every neighbourhood code N | W<<2 | C<<4 | E<<6 | S<<8 (each 0
// unexplored, 1 explored, 2 visible) names one six-tile set, so a tile is CLOUD
// (1) if only sets with an unexplored centre use it, EXPLORED (2) if only sets
// with an explored or visible centre do -- clear and fog-of-war both -- and 0
// if both or neither. Codes with a field of 3 are never formed by
// Fog_NeighbourCode and are skipped: their table bytes are not a family.
uint8_t g_fx_kind[1024];
int g_fx_kinds = -1;             // -1 not built, 0 unavailable, 1 built

bool fx_tile_kinds_ready(void) {
    if (g_fx_kinds < 0) {
        if (!fx_tables()) {
            g_fx_kinds = 0;
            return false;
        }
        uint8_t used[1024] = {};  // bit 0: unexplored centre, bit 1: explored
        for (int c = 0; c <= kFxMaxCode; ++c) {
            bool valid = true;
            for (int f = 0; f < 5; ++f)
                if (((c >> (2 * f)) & 3) == 3) valid = false;
            if (!valid) continue;
            const int centre = (c >> 4) & 3;
            for (int i = 0; i < 6; ++i) {
                const uint16_t t = g_fx_sets[g_fx_code[c] * 6 + i];
                used[t & 0x3FFu] |= centre == 0 ? 1u : 2u;
            }
        }
        int cloud = 0, explored = 0, both = 0;
        for (int t = 0; t < 1024; ++t) {
            g_fx_kind[t] = used[t] == 1u ? 1u : used[t] == 2u ? 2u : 0u;
            cloud += used[t] == 1u;
            explored += used[t] == 2u;
            both += used[t] == 3u;
        }
        g_fx_kinds = 1;
        std::fprintf(stderr, "[lb_fogkind] %d cloud tiles, %d clear/fog-of-war "
                     "tiles, %d used by both\n", cloud, explored, both);
        std::fflush(stderr);
    }
    return g_fx_kinds == 1;
}

int fx_tile_kind(uint16_t tile) {
    if (!fx_tile_kinds_ready() || tile >= 1024u) return 0;
    return g_fx_kind[tile];
}

// Both grids for the whole map, once per call. 2 x 64 rows x 8 bytes on the
// maps this game ships, so reading everything is cheaper than being clever.
bool fx_read_record(void) {
    g_fx_ok = false;
    if (g_map_w <= 0 || g_map_h <= 0) return false;
    const uint32_t holder = bus_read_u32(kFogHolder);
    if (!main_ram(holder)) return false;
    const uint32_t ex = bus_read_u32(holder + 0x10u);
    const uint32_t vi = bus_read_u32(holder + 0x14u);
    if (!main_ram(ex) || !main_ram(vi)) return false;
    const uint32_t w = bus_read_u32(ex + 8u), h = bus_read_u32(ex + 0xCu);
    if (w != uint32_t(g_map_w) || h != uint32_t(g_map_h)) return false;
    if (bus_read_u32(vi + 8u) != w || bus_read_u32(vi + 0xCu) != h)
        return false;
    const uint32_t erows = bus_read_u32(ex + 0x14u);
    const uint32_t vrows = bus_read_u32(vi + 0x14u);
    if (!main_ram(erows) || !main_ram(vrows)) return false;
    if (g_fx_state.size() != size_t(w) * size_t(h))
        g_fx_state.assign(size_t(w) * size_t(h), 0u);
    uint64_t sig = 1469598103934665603ull;
    const uint32_t stride = (w + 7u) / 8u;
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t er = bus_read_u32(erows + y * 4u);
        const uint32_t vr = bus_read_u32(vrows + y * 4u);
        if (!main_ram(er) || !main_ram(vr)) return false;
        for (uint32_t bx = 0; bx < stride; ++bx) {
            const uint8_t eb = bus_read_u8(er + bx);
            const uint8_t vb = bus_read_u8(vr + bx);
            sig = (sig ^ eb) * 1099511628211ull;
            sig = (sig ^ vb) * 1099511628211ull;
            for (uint32_t k = 0; k < 8u; ++k) {
                const uint32_t x = bx * 8u + k;
                if (x >= w) break;
                g_fx_state[size_t(y) * w + x] =
                    ((vb >> k) & 1u) ? 2u : ((eb >> k) & 1u) ? 1u : 0u;
            }
        }
    }
    g_fx_w = int(w);
    g_fx_h = int(h);
    g_fx_sig = sig;
    g_fx_ok = true;
    return true;
}

// THE RECORD AS THE GROUND ON SCREEN WAS BUILT FROM IT (docs/348).
//
// Graded on frames whose record had been stable for three frames, the rule
// matches the cartridge's window on 100% of visible cells and 99.7% of
// fog-of-war cells; the disagreements sit on frames just after units moved,
// with the rule showing the NEW state (fog-of-war) and the window still the
// old one (clear). The window lags the record by an update, by construction:
// Task_View_DrawField (task 0x1D) builds the BG2 staging from the record right
// after that update's simulation, and Task_View_CommitBGs (task 0x01 of the
// NEXT update, before its simulation) writes BG2's scroll and DMAs the
// staging. Reading the record live put the widened fog an update ahead of the
// native fog beside it -- a fog boundary a metatile out of step at the seam
// for a frame, every time a unit moved.
//
// So the record is read at the moment its ground is committed: the BG2 scroll
// write (lb_tag_on_bg2), when the record still holds exactly the state the
// staging was built from. Everything downstream uses that copy.
bool g_fx_committed = false;

// Fog_NeighbourCode's state for one cell. Negative coordinates are what the
// cartridge's u8 arithmetic wraps past the map's width, so both are "outside".
inline int fx_state(int mx, int my, int reveal) {
    if (mx < 0 || my < 0 || mx >= g_fx_w || my >= g_fx_h) return 0;
    if (reveal) return 2;
    return g_fx_state[size_t(my) * size_t(g_fx_w) + size_t(mx)];
}

// The six BG2 entries the cartridge would write for this metatile.
inline const uint8_t* fx_tiles(int mx, int my, int reveal) {
    const int code = fx_state(mx, my - 1, reveal) |
                     (fx_state(mx - 1, my, reveal) << 2) |
                     (fx_state(mx, my, reveal) << 4) |
                     (fx_state(mx + 1, my, reveal) << 6) |
                     (fx_state(mx, my + 1, reveal) << 8);
    return &g_fx_sets[g_fx_code[code] * 6];
}

int fx_log_on(void) {
    static int on = -1;
    if (on < 0) on = std::getenv("LB_FOGEXACTLOG") ? 1 : 0;
    return on;
}
}  // namespace

// GRADE THE RULE AGAINST THE CARTRIDGE, every frame, where the cartridge's own
// answer is sitting in VRAM: the guest's 32x24 window of BG2, at the camera
// choose_camera has just matched to BG1. Run at scan time, from
// lb_widemap_scan, with the learners.
//
// It also decides the reveal override, which comes from two global flags the
// fog code consults (a cheat/debug pair at *(0x02150A70) +0 and +0x1F, and an
// ownership test through FUN_020a223c) rather than from the record. Instead of
// reimplementing that ownership chain, it asks the picture which branch the
// cartridge took: with the override on, the guest's window is all clear tiles
// whatever the record says. A few cells always disagree -- the window was
// built one update ago, and UI panels punch clear tiles into it
// (Fog_ApplyUiCellMask 0203a4b4) -- so the decision needs a run of frames.
extern "C" int lb_shown_cam(int* x, int* y);   // below, with the tags
extern "C" int lb_hud_refog_cell(int cx, int cy);   // lb_hud.cpp

// Is UI cell (cx, cy) one the cartridge PUNCHED into the BG2 now in VRAM?
//
// Not the live grid at 0x02151F28: task 0x1C clears and re-marks that every
// update, so at any given moment it can be half-built. Fog_ApplyUiCellMask
// (0203a4b4) keeps a copy of exactly the grid it punched with -- BitGrid_Alloc
// at view+0x164, the data memcpy'd to view+0x174 -- and Task_View_DrawField
// (02039148) compares against that copy to decide whether to rebuild. So the
// copy IS the set of holes in the current BG2. BitGrid layout per
// FUN_020eec2c / BitGrid_Test: row pointers at +0x14, bit cx&7 of byte cx>>3.
// -1 when it cannot be read.
int fx_ui_cell(int cx, int cy) {
    if (cx < 0 || cx >= 32 || cy < 0 || cy >= 24) return 0;
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return -1;
    const uint32_t rows = bus_read_u32(view + 0x164u + 0x14u);
    if (!main_ram(rows)) return -1;
    const uint32_t row = bus_read_u32(rows + uint32_t(cy) * 4u);
    if (!main_ram(row)) return -1;
    return (bus_read_u8(row + uint32_t(cx >> 3)) >> (cx & 7)) & 1;
}
// The punched cells, read once per composite (lb_widemap_compose_prep) so the
// compositor's workers test a bit instead of walking guest memory per tile.
uint32_t g_ui_snap[24];

// The record as committed (see g_fx_committed), or a live read when no BG2
// commit has been seen yet -- the first frames of a battle.
bool fx_record_for_ground(void) {
    // A copy from another map (the battle before this one) is not this one's.
    if (g_fx_committed && g_fx_ok && g_fx_w == g_map_w && g_fx_h == g_map_h)
        return true;
    return fx_read_record();
}

void fx_grade(uint32_t dispcnt, int cam_tx, int cam_ty) {
    if (!fx_on() || !fx_tables() || !fx_record_for_ground()) return;
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg2cnt = nds_io_debug_read(9, 0x0400000Cu, 16);
    const uint32_t base = block + ((bg2cnt >> 8) & 0x1Fu) * 0x800u;
    // The row parity of the ground in VRAM -- the camera of the last BG
    // commit -- which is what places a UI cell on a BG2 row.
    int ptag_x = 0, ptag_y = 0;
    const bool ptag = lb_shown_cam(&ptag_x, &ptag_y) != 0;
    const int parity = ptag ? (floor_div(ptag_y, 8) & 1) : -1;
    unsigned cells = 0, wrong = 0, uiclear = 0, pred_fog = 0, vram_fog = 0;
    // What KIND of cell each agreement was, so a clean grade on ground that
    // happens to be all clear cannot pass for one that exercised the rule:
    // [0] clear (64), [1] fog-of-war set (explored, unseen centre),
    // [2] cloud (unexplored centre).
    static unsigned kind_ok[3] = {0, 0, 0}, kind_n[3] = {0, 0, 0};
    // ...and the same split for frames on which the record has not changed
    // for three frames, i.e. the cartridge has had time to rebuild its window
    // from it (Task_View_DrawField rebuilds BG2 once a unit has moved). A rule
    // error shows on these too; a window that merely lags the record does not.
    static unsigned skind_ok[3] = {0, 0, 0}, skind_n[3] = {0, 0, 0};
    static uint64_t last_sig = 0;
    static int stable = 0;
    stable = (g_fx_sig == last_sig) ? stable + 1 : 0;
    last_sig = g_fx_sig;
    const bool settled = stable >= 3;
    for (int trow = 0; trow < 24; ++trow) {
        const int ar = cam_ty + trow;
        const int my = floor_div(ar, 2), py = floor_mod(ar, 2);
        for (int h = 0; h < 32; ++h) {
            const int ac = cam_tx + h;
            const int mx = floor_div(ac, 3), px = floor_mod(ac, 3);
            if (mx < 0 || my < 0 || mx >= g_fx_w || my >= g_fx_h) continue;
            const uint16_t pred = fx_tiles(mx, my, g_fx_reveal)[py * 3 + px];
            const uint16_t pred0 = fx_tiles(mx, my, 0)[py * 3 + px];
            const uint16_t vram = uint16_t(nds_vram_read_bg(
                0, base + uint32_t(trow * 32 + h) * 2u, 2)) & 0x3FFu;
            // A cell the cartridge punched for a UI sprite is not the fog
            // rule's to answer: excluded from the grade, counted apart.
            if (parity >= 0 && fx_ui_cell(h, trow - parity) == 1) {
                ++g_fx_uicells;
                continue;
            }
            ++cells;
            if (pred0 != 0x64u) ++pred_fog;
            if (vram != 0x64u) ++vram_fog;
            {
                const int c = fx_state(mx, my, g_fx_reveal);
                const int k = (c == 2) ? 0 : (c == 1) ? 1 : 2;
                ++kind_n[k];
                if (pred == vram) ++kind_ok[k];
                if (settled) {
                    ++skind_n[k];
                    if (pred == vram) ++skind_ok[k];
                    // The first few settled fog-of-war disagreements, whole,
                    // so a rule error could be read off rather than argued.
                    static unsigned shown = 0;
                    if (k == 1 && pred != vram && shown < 12 && fx_log_on()) {
                        ++shown;
                        std::fprintf(stderr,
                            "[lb_fogfow] metatile %d,%d pos %d: rule %03X "
                            "cartridge %03X  N%d W%d C%d E%d S%d\n",
                            mx, my, py * 3 + px, pred, vram,
                            fx_state(mx, my - 1, g_fx_reveal),
                            fx_state(mx - 1, my, g_fx_reveal),
                            fx_state(mx, my, g_fx_reveal),
                            fx_state(mx + 1, my, g_fx_reveal),
                            fx_state(mx, my + 1, g_fx_reveal));
                    }
                }
            }
            if (pred == vram) continue;
            ++wrong;
            if (vram == 0x64u) ++uiclear;
        }
    }
    // The override's evidence: plenty of fog predicted from the record and
    // none at all drawn means the cartridge took the reveal branch; any fog
    // drawn means it did not. Neither when the record predicts none.
    if (pred_fog >= 64u && vram_fog == 0u) {
        if (g_fx_reveal_run < 0) g_fx_reveal_run = 0;
        if (++g_fx_reveal_run >= 3) g_fx_reveal = 1;
    } else if (vram_fog > 0u) {
        if (g_fx_reveal_run > 0) g_fx_reveal_run = 0;
        if (--g_fx_reveal_run <= -3) g_fx_reveal = 0;
    }
    g_fx_graded += cells;
    g_fx_wrong += wrong;
    g_fx_uiclear += uiclear;
    ++g_fx_frames;
    // THE CARTRIDGE'S UI GRID BESIDE THE PORT'S REFILL MASK (docs/348 s4b),
    // printed when they differ: which punched cells the port fills back and
    // which it leaves. A cell punched for an element the port did NOT move
    // must stay open, so a difference is only a defect where the element was
    // moved -- the rows say which elements those are.
    if (fx_log_on() && (g_fx_frames % 120u) == 0u) {
        static unsigned printed = 0;
        bool differ = false;
        uint32_t cart[24] = {}, port[24] = {};
        for (int cy = 0; cy < 24; ++cy)
            for (int cx = 0; cx < 32; ++cx) {
                if (fx_ui_cell(cx, cy) == 1) {
                    cart[cy] |= 1u << cx;
                    // What the compositor will actually refill: a punched
                    // cell near a moved element and near no unmoved one.
                    if (lb_hud_refog_cell(cx, cy)) port[cy] |= 1u << cx;
                }
            }
        for (int cy = 0; cy < 24; ++cy)
            if ((cart[cy] & 0xFu) != (port[cy] & 0xFu)) differ = true;
        if (differ && printed < 6) {
            ++printed;
            std::fprintf(stderr, "[lb_uigrid] frame %u: cartridge UI grid "
                         "vs port refill, low 8 columns per row (row: cart | "
                         "port)\n", g_fx_frames);
            for (int cy = 0; cy < 24; ++cy) {
                if (!cart[cy] && !port[cy]) continue;
                char a[9], b[9];
                for (int cx = 0; cx < 8; ++cx) {
                    a[cx] = ((cart[cy] >> cx) & 1u) ? '#' : '.';
                    b[cx] = ((port[cy] >> cx) & 1u) ? '#' : '.';
                }
                a[8] = b[8] = 0;
                std::fprintf(stderr, "[lb_uigrid]   row %2d: %s | %s   "
                             "(full rows %08X | %08X)\n", cy, a, b,
                             cart[cy], port[cy]);
            }
        }
    }
    if (fx_log_on() && (g_fx_frames % 60u) == 0u)
        std::fprintf(stderr,
                     "[lb_fogexact] frame %u: native window %u cells, %u "
                     "disagree (%u of them clear in VRAM, i.e. UI holes or a "
                     "stale window); totals %u/%u = %.2f%%; reveal %d; "
                     "agree by centre: visible %u/%u fog-of-war %u/%u "
                     "cloud %u/%u; UI-grid cells excluded %u; SETTLED "
                     "frames: visible %u/%u fog-of-war %u/%u cloud %u/%u\n",
                     g_fx_frames, cells, wrong, uiclear, g_fx_wrong,
                     g_fx_graded,
                     g_fx_graded ? 100.0 * g_fx_wrong / g_fx_graded : 0.0,
                     g_fx_reveal, kind_ok[0], kind_n[0], kind_ok[1],
                     kind_n[1], kind_ok[2], kind_n[2], g_fx_uicells,
                     skind_ok[0], skind_n[0], skind_ok[1], skind_n[1],
                     skind_ok[2], skind_n[2]);
}

// ---- UI SPRITES PUNCH THE FOG (docs/348) ------------------------------------
//
// In adaptive view, command sprites drew beneath the fog, unlike vanilla LEGO
// Battles. On a DS the command icon (the SpriteUIEngine element at
// 0x0215760C, drawn every frame from task 0x1C) stays visible over fog because
// SpriteUI_MarkCells (0203420c) marks the 8x8 screen cells its quad covers --
//
//     cx0 = (x-4) >> 3,  cx1 = (x-4+w) >> 3  (exclusive, arithmetic shifts)
//     cy0 =  y    >> 3,  cy1 = (y+h)   >> 3  (exclusive)
//
// clamped to the 32x24 screen -- and Fog_ApplyUiCellMask (0203a4b4) writes the
// clear tile, 100, into BG2 at column cx, row cy + ((camy>>3)&1) of each.
// The icons are 3D quads under the fog layer (BG2 priority 1, 3D 2), so that
// hole is the only reason they show. The clamp drops every cell outside the
// DS's own screen, so in the widened columns nothing punched the fog and the
// icon disappeared under it.
//
// SpriteUI_MarkCells has direct callers, so it cannot be overridden here; the
// cells are rebuilt instead from the icon's quads in the render list (depth
// 4,275,712, measured -- sprite+0x56 = 502 via FUN_02034820(elem, 0)), with
// the same formula WITHOUT the clamp, and only the cells outside the native
// 32x24 are kept: the cartridge already punches those.
//
// The list is the one latched at the last VBlank, built with camera
// g_tag_render; the tables may be for the ground's camera g_tag_bg (a frame
// that pairs the previous raster). A world-anchored icon's screen x moves by
// the camera delta, so the cells are taken at x + (render - bg), which is
// exact whenever the two agree and follows the icon when they do not.
std::vector<std::pair<int, int>> g_ui_punch;   // (trow, htile), next fill
uint64_t g_ui_punch_sig = 0;
unsigned g_ui_punch_cells = 0, g_ui_punch_frames = 0;
// docs/348: the current g_ext2 came from the cartridge's own rule (so its
// NATIVE cells hold the fog as the cartridge would draw it with no UI hole),
// and the row parity -- (camy >> 3) & 1 -- of the ground camera it was built
// for, which Fog_ApplyUiCellMask adds to a UI cell's row. -1 = unknown.
bool g_ext2_exact = false;
int g_fill_parity = -1;
unsigned g_refog_served = 0;
extern "C" int lb_hud_refog_cell(int cx, int cy);   // lb_hud.cpp

// docs/348: the fog and shroud tables for a given camera (below), and whether
// the widened composite builds both tables itself (lb_widemap_compose_prep).
void fill_fog_for(int cam_tx, int cam_ty);
static int compose_fill_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_COMPOSEFILL");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on;
}
// ...AND ONLY WHILE THE WIDENED COMPOSITE IS ACTUALLY RUNNING. With the
// adaptive layout off (vanilla, or any frame nothing composites) the prep
// never runs, and the tables still have a consumer: the NATIVE decode's edge
// columns, where the scroll bias samples a tilemap column the cartridge never
// writes (htile -1 / 32). Measured: with the tables built only by the prep,
// vanilla's leftmost 4 columns went black; the previous build's vanilla had
// terrain there. So the scan builds them whenever the prep has not run within the
// last two scans.
unsigned g_scan_seq = 0, g_prep_seq = 0;
extern "C" int lb_compose_fill_on(void) {
    return compose_fill_enabled() && g_prep_seq != 0 &&
           g_scan_seq - g_prep_seq <= 2u;
}

extern "C" void lb_widemap_scan(void) {
    if (!g_geom_valid) return;
    ++g_scan_seq;
    // When the scan builds the tables itself (no composite running), it only
    // has to cover what the native decode can ask for.
    if (!lb_compose_fill_on()) g_fill_bounds = bounds_for_surface(256, 192);
    // At render time, so the guest has already painted this frame's footprint.
    footprint_probe();
    // THE CAMERA MUST NOT SURVIVE THE FRAME SHIFTED.
    //
    // The harvest shifts the guest's own camera and restores it inside one
    // painter call. If the slice ends mid-call and the guest never re-enters,
    // the shift would stand for the rest of the frame and the WHOLE PICTURE
    // would jump. Render time is after the guest's frame, so a shift still
    // applied here is a fault; put it back and count it rather than let it
    // show.
    lb_fp_cam_rescue();
    // Latch the band supply for the whole of this render pass.
    // docs/383: served while the site is still (partly) outside the guest's
    // own window and a harvest of it exists. The moment it comes back
    // inside, the guest's own paint has every cell and the harvest retires.
    {
        int fp_sx = 0, fp_sy = 0;
        const int fp_need = lb_fp_band_shift2(&fp_sx, &fp_sy);
        g_fp_bands_latched = fp_need && g_fp_bands_live && g_fp_h_valid;
        // docs/392: a wall line (mode 1) is served from its pieces instead,
        // and never from a harvest, which only func_020370B4's modes make.
        g_fp_line_latched = false;
        const uint32_t view = bus_read_u32(kViewPtr);
        if (main_ram(view) && bus_read_u8(view + 0x78u) &&
            bus_read_u8(view + 0x6Cu + 0x10u) == 1u) {
            g_fp_bands_latched = false;
            g_fp_line_latched = g_geom_valid && fp_bands_enabled() &&
                                g_fp_line_n > 0 && g_fp_line_out;
        }
    }
    // Pick the camera the picture is drawn from, then build the terrain for
    // THAT camera, before anything downstream reads g_cam_tx/g_cam_ty. The fog
    // scan below uses the same pair, so terrain and fog cannot shear apart.
    //
    // docs/348: the LEARNERS below (scan_shroud, learn_fog_edges) still run
    // here, against the VRAM of this moment and the camera that matches it.
    // The TABLES the band is drawn from are built by the widened composite
    // instead, for the camera the ground it is drawing was committed with --
    // this scan runs a whole VBlank before that composite, while the guest's
    // commit for it has not happened yet, which is the band-versus-native seam
    // a code audit found.
    choose_camera(g_dispcnt);
    if (!lb_compose_fill_on()) fill_terrain(g_cam_tx, g_cam_ty);
    audit_terrain(g_dispcnt);
    // docs/348: grade the reproduced fog rule against the guest's own BG2
    // window, at the camera that window was just matched to.
    if (g_model_ok) fx_grade(g_dispcnt, g_cam_tx, g_cam_ty);
    scan_shroud(g_dispcnt, g_map, g_map_w, g_map_h, g_cam_tx, g_cam_ty);
    // After the scan, so g_seen is complete for this frame: a neighbour mask
    // built mid-scan would be reading verdicts the loop has not written yet.
    learn_fog_edges(g_cam_tx, g_cam_ty);

    // After scan_shroud, which is what adopts or drops g_vis_base, and before
    // the fill that reads it.
    build_vis_bits();

    // O(1) per cell afterwards; see fog_edge_build_cardinal.
    fog_edge_build_cardinal();
    if (lb_compose_fill_on()) {
        audit_fog(g_dispcnt);
        return;
    }
    fill_fog_for(g_cam_tx, g_cam_ty);
}

// THE WIDENED FOG AND SHROUD TABLES FOR A GIVEN CAMERA.
//
// Split out of lb_widemap_scan (docs/348) so the widened composite can build
// them for the camera its ground was committed with. The body is unchanged
// except that it reads the camera it is given rather than g_cam_tx/g_cam_ty,
// and walks only the cells the surface can ask for (g_fill_bounds).
void fill_fog_for(int cam_tx, int cam_ty) {
    g_bare_n = 0;
    const ExtBounds bd = g_fill_bounds;
    // docs/348: THE CARTRIDGE'S RULE, when its record and tables are there,
    // on the record as it was when the ground now in VRAM was committed (see
    // g_fx_committed) -- not as it is now, which can be an update ahead.
    if (fx_on() && fx_tables() && fx_record_for_ground()) {
        static int ex_tx = INT32_MIN, ex_ty = INT32_MIN, ex_rev = -1;
        static uint64_t ex_sig = 0, ex_punch = 0;
        static ExtBounds ex_b = {0, -1, 0, -1};
        if (g_ext2_valid && g_ext3_valid && ex_tx == cam_tx &&
            ex_ty == cam_ty && ex_sig == g_fx_sig && ex_rev == g_fx_reveal &&
            ex_punch == g_ui_punch_sig && bd == ex_b)
            return;
        ex_tx = cam_tx;
        ex_ty = cam_ty;
        ex_sig = g_fx_sig;
        ex_rev = g_fx_reveal;
        ex_punch = g_ui_punch_sig;
        ex_b = bd;
        // So the older path's cache cannot mistake its stale tables for
        // current ones if the record later becomes unreadable.
        g_ext_fill_tx = INT32_MIN;
        for (int trow = bd.rlo; trow <= bd.rhi; ++trow) {
            const int ar = cam_ty + trow;
            const int my = floor_div(ar, 2), py = floor_mod(ar, 2);
            int cached_mx = INT32_MIN;
            const uint8_t* t = nullptr;
            int centre = 0;
            for (int h = bd.hlo; h <= bd.hhi; ++h) {
                const int ac = cam_tx + h;
                const int mx = floor_div(ac, 3);
                if (mx != cached_mx) {
                    t = fx_tiles(mx, my, g_fx_reveal);
                    centre = fx_state(mx, my, g_fx_reveal);
                    cached_mx = mx;
                }
                g_ext2[trow - kExtRowLo][h - kExtLo] =
                    uint16_t(t[py * 3 + floor_mod(ac, 3)]);
                // BG3's widened entry: the port's own transparent marker for
                // ground the game is hiding (see kShroudTile); only read while
                // a building is being placed.
                g_ext3[trow - kExtRowLo][h - kExtLo] =
                    centre == 0 ? kShroudTile : 0u;
            }
        }
        // The UI sprites' holes, exactly as Fog_ApplyUiCellMask writes them
        // into the native window: the clear tile, whatever the fog was.
        for (const auto& c : g_ui_punch) {
            if (c.first < bd.rlo || c.first > bd.rhi ||
                c.second < bd.hlo || c.second > bd.hhi)
                continue;
            g_ext2[c.first - kExtRowLo][c.second - kExtLo] = kClearTile;
        }
        g_ext2_valid = true;
        g_ext3_valid = true;
        g_ext2_exact = true;
        return;
    }
    g_ext2_exact = false;
    const bool fog_ok = fog_learned();
    static ExtBounds last_b = {0, -1, 0, -1};
    // SKIP A FILL THAT WOULD REPRODUCE THE TABLE WE ALREADY HAVE.
    //
    // This loop walks kExtRows x kExtCols -- about 73,000 cells, each
    // with metatile lookups and neighbour tests -- and it ran EVERY
    // FRAME. fill_terrain next door has been cached on the camera since
    // it was written; this one never was, and it is the larger of the
    // two. The game ran unacceptably choppy because of it.
    //
    // Its inputs are the camera, the visibility record and whether the
    // fog tiles are known. g_vis_sig fingerprints the record, so when
    // all three are unchanged the output is identical by construction
    // and the frame can keep the tables it has. A camera that moves or
    // ground that gets explored rebuilds immediately, so nothing is
    // ever shown stale.
    if (g_ext2_valid && g_ext3_valid &&
        g_ext_fill_tx == cam_tx && g_ext_fill_ty == cam_ty &&
        g_ext_fill_sig == g_vis_sig && g_ext_fill_fog == fog_ok &&
        bd == last_b) {
        audit_fog(g_dispcnt);
        return;
    }
    g_ext_fill_tx = cam_tx;
    g_ext_fill_ty = cam_ty;
    g_ext_fill_sig = g_vis_sig;
    g_ext_fill_fog = fog_ok;
    last_b = bd;
    for (int trow = bd.rlo; trow <= bd.rhi; ++trow) {
        const int ar = cam_ty + trow;
        for (int h = bd.hlo; h <= bd.hhi; ++h) {
            // NO CELL IS SKIPPED. A native column can be asked for almost
            // any row once the scroll offset is non-zero -- guest scanline -1
            // with bgy 15 lands on tile row 1, which is squarely inside the
            // guest's own window -- so there is no row range the native
            // columns can be left unfilled for. The tables are 42x64 entries
            // and the whole fill is a few thousand array writes.
            const int ac = cam_tx + h;
            // Unknown ground is shrouded, which is the conservative default
            // this whole file rests on: the widened view may lag behind the
            // game by hiding ground revealed off-screen, and can never do the
            // opposite.
            //
            // The row test used to be `trow < 24`, i.e. "the two rows past the
            // guest's own 24 are always shrouded". That was over-cautious once
            // the record became absolute: g_seen is indexed by MAP row, so a
            // row below the window is simply a row the camera has or has not
            // visited, and the unseen default already answers for it. Keeping
            // the special case forced a permanent strip of fog along the
            // bottom 16 px of every widened frame even after the player had
            // panned across that ground.
            uint8_t s = 1u;
            // HAS THE SCAN ACTUALLY READ THIS TILE, or is s just the default?
            // The record below is one byte per METATILE while the cartridge's
            // BG2 is one entry per TILE, so it cannot reproduce a fog boundary
            // that falls inside a metatile. Measured against the cartridge over
            // the guest's own window, where the cartridge is ground truth in
            // the same frame, the record disagrees on 3.9% of tiles -- and that
            // is its BEST alignment: every neighbouring origin scores worse
            // (4.7, 5.5, 5.7, 6.2, 7.0, 7.3, 7.8%), so this is a resolution
            // limit and not a misalignment.
            //
            // g_seen, where the scan has actually read a tile, is exact: it is
            // the cartridge's own BG2 for a cell the guest's window drew. So
            // prefer it there and let the record answer only for ground the
            // window has never covered, which is the one place nothing better
            // exists. LB_FOGOBS=1 to try it; off until it is proven.
            bool observed = false;
            if (ar >= 0 && ar < g_seen_h && ac >= 0 && ac < g_seen_w) {
                s = g_seen[size_t(ar) * size_t(g_seen_w) + size_t(ac)];
                observed = !g_obs.empty() &&
                           g_obs[size_t(ar) * size_t(g_seen_w) +
                                 size_t(ac)] != 0u;
            }
            static const int fogobs = [] {
                const char* v = std::getenv("LB_FOGOBS");
                return (v && v[0] != "0"[0]) ? 1 : 0;
            }();

            // THE GAME'S OWN MAP-SPACE VISIBILITY, when LB_FOGSRC=map.
            //
            // g_seen only knows ground the guest's 32-column window has drawn,
            // so everything else defaults to shrouded and the fog forms a
            // straight wall at the native boundary instead of following the
            // terrain. The correlation search (LB_FINDFOG) found a byte-per-
            // metatile grid, stride 64, non-zero = revealed, that agrees with
            // the guest's own fog on every sampled view.
            //
            // The address is DISCOVERED, never hard-coded. It is heap -- well
            // above the game's static data, whose map pointer sits at
            // 02156CB0 -- and only looked stable because this route allocates
            // identically every run. find_fog_record locates it per battle and
            // the check below drops it the moment it stops agreeing with the
            // guest's own fog.
            if (g_fogsrc_map && g_vis_base && !(fogobs && observed) &&
                !g_vis_bits.empty()) {
                const int mx = floor_div(ac, 3), my = floor_div(ar, 2);
                if (mx >= 0 && my >= 0 && mx < g_map_w && my < g_map_h) {
                    s = g_vis_bits[size_t(my) * size_t(g_map_w) + size_t(mx)];
                    // EDGE TILES ONLY, when the metatile next door is hidden.
                    //
                    // The record is one byte per METATILE and the cartridge's
                    // fog is per TILE, so a boundary running through a metatile
                    // cannot be represented and the band draws open ground the
                    // cartridge fogs. Dilating the whole metatile fixes that
                    // and costs far too much: it fogged 8.6% of the picture
                    // and, because fog draws in front of the 3D layer, cut
                    // into the player's own base.
                    //
                    // A metatile is 3 tiles across and 2 down, so only the
                    // tiles ON the side facing a hidden neighbour can be part
                    // of that boundary. Fog those and leave the metatile's
                    // interior alone -- one tile of caution instead of six.
                    if (!s && fogedge()) {
                        const int px = floor_mod(ac, 3);
                        const int py = floor_mod(ar, 2);
                        const int dx = (px == 0) ? -1 : (px == 2) ? 1 : 0;
                        const int dy = (py == 0) ? -1 : 1;
                        // THE DIAGONAL TOO. Measured: of the holes left in a
                        // frame, 18 sat in metatiles with exactly THREE hidden
                        // neighbours -- a corner -- and 6 in metatiles with
                        // none. Checking only the facing row or column misses
                        // every corner, which is why the first version of this
                        // rule recovered barely a point. A tile in the corner
                        // of its metatile touches three neighbours, so ask all
                        // three.
                        if (dx && hidden_metatile(mx + dx, my)) s = 1u;
                        else if (hidden_metatile(mx, my + dy)) s = 1u;
                        else if (dx && hidden_metatile(mx + dx, my + dy)) s = 1u;
                    }
                }
            }
            // Screen phase, matching the scan -- see the note there. The phase
            // IS the position inside the metatile, so it indexes the edge
            // table directly.
            // Keep the verdict for the guest's own cells, so audit_fog can
            // grade this decision against the cartridge in the same frame.
            if (trow >= 0 && trow < 24 && h >= 0 && h < 32) {
                g_fill_s[trow][h] = s;
                g_fill_s_valid = true;
            }
            const int pos = floor_mod(ar, 2) * 3 + floor_mod(ac, 3);
            // The confirmed table when there is one, this frame's reading
            // until then. Both draw the same cloud.
            const bool have_fog = fog_ok || g_fog_prov_ok;
            uint16_t fog = fog_ok ? g_fog_tiles[pos] : g_fog_prov[pos];
            if (s && fog_ok) {
                // THE EDGE VARIANT, when one has been observed for this
                // neighbourhood. A mask never seen in the guest's own columns
                // falls back to the interior pattern -- which is exactly what
                // was drawn before this table existed, so an unlearned mask
                // leaves the picture no worse than it was.
                const uint8_t mask = metatile_mask(floor_div(ac, 3),
                                                   floor_div(ar, 2));
                // A FULLY SHROUDED METATILE IS INTERIOR. NOTHING MAY SUBSTITUTE
                // FOR IT.
                //
                // This is why the fog came out transparent where it should be
                // opaque, and why that was inconsistent, varying for no
                // apparent reason: it is the first twenty-odd seconds of
                // every battle, and then it heals.
                //
                // Measured with LB_LAYERDUMP, which records which layer won
                // each pixel. Over the fogged ground, BG1 -- the terrain --
                // wins 12.46% of the pixels on composited frames 240 through
                // 1440, and 0.00% from 1680 on. The counters say why:
                //
                //   [lb_fogedge] band edge cells: exact 0, cardinal 73809 ... masks=1
                //   [lb_fogedge] band edge cells: exact 0, cardinal 0     ... masks=6
                //
                // EVERY cell in the table -- all 73,809, interior ones
                // included -- took the cardinal tier while only one mask had
                // been learned, and what it served was not the interior set:
                //
                //   early  ext2 (fog): 00BA 00BB 00BA 00BA 00BB 00BA ...
                //   late   ext2 (fog): 00C2 00C0 00C1 00C2 00C0 00C1 ...
                //
                // 0BA is 54/64 opaque (docs/reference/shroud-tile-borders.txt)
                // -- it fades on its left border, because it is a WEST EDGE
                // tile. Two of every three columns get it, so about 12% of the
                // cloud is holes, which is the 12.46% the layer dump counts,
                // and the terrain under it reads straight through: the lake and
                // the paths are legible in lb_comp-1712x730-240.png and gone in
                // -1920.png.
                //
                // The third tier below already refuses to count mask 0xFF as a
                // square edge, because a metatile with cloud on all eight sides
                // is not an edge at all. The two tiers in front of it never got
                // the same guard, so the substitution machinery was free to
                // replace the correct interior pattern with an edge variant
                // fitted to a DIFFERENT neighbourhood -- and with one mask
                // learned, g_card_tile carries whatever that single observation
                // implied for all sixteen cardinal classes.
                //
                // An unlearned mask must leave the picture no worse than it was,
                // which is what the comment above promises. For 0xFF "no worse"
                // means untouched.
                if (mask == 0xFFu) {
                    /* interior: keep g_fog_tiles[pos] */
                } else if (g_fog_edge_seen[mask][pos]) {
                    fog = g_fog_edge[mask][pos];
                    if (mask != 0xFFu) ++g_edge_exact;
                } else if (const uint16_t* c = fog_edge_cardinal(mask, pos)) {
                    // SAME CARDINAL NEIGHBOURS, ANY DIAGONALS.
                    //
                    // Only six of 256 masks are ever observed in the guest's
                    // own 32 columns -- measured, on three maps -- so almost
                    // every boundary cell in the band took the interior
                    // pattern instead: cloud with cloud on all sides, drawn
                    // where there is cloud on one side. That is the square
                    // fog edge, against the rounded bumpy edge the cartridge
                    // draws natively.
                    //
                    // The dump says the variant is chosen by the CARDINAL
                    // neighbours. Masks differing only in a diagonal give
                    // identical tiles -- FF and FE (NW), D6 and F6 (SW), D4
                    // and F4 (SW) -- while N and W each change a specific
                    // edge of the metatile: W swaps positions 0 and 3, the
                    // left column, and N swaps 0,1,2, the top row.
                    //
                    // So a mask never seen directly can still be served from
                    // one that shares its four cardinal bits. This tier is
                    // only reached when the exact mask is absent, where the
                    // alternative is the interior pattern, so it can improve
                    // the picture and cannot make it worse -- the same
                    // argument the exact table was added under.
                    fog = *c;
                    ++g_edge_card;
                } else if (mask != 0xFFu) {
                    // A boundary cell drawn with the all-sides-shrouded
                    // pattern. This is the square edge, counted.
                    ++g_edge_interior;
                }
            }
            // REVEALED GROUND GETS kClearTile, NOT TILE 0.
            //
            // 0x064 is the cartridge's own marker for "not shrouded" on BG2 --
            // this file has always read it that way, three lines of scan_shroud
            // decide `shrouded` by testing against it -- but the band wrote a
            // plain 0 there instead, on the assumption that one blank tile is
            // as good as another.
            //
            // The fog audit says otherwise, and says it in numbers: with the
            // whole screen fogged at camera 53,18 the guest drew a non-zero
            // BG2 tile in all 768 native cells while this port left 294 of them
            // at 0, every one of them a cell the guest filled with 064. Writing
            // a different tile index than the cartridge does is writing
            // different pixels, whatever tile 0 happens to look like -- and the
            // visible result was exactly fog that would not show and broken
            // fog over the side band.
            //
            // lb_widemap_clear_tile already returned 0x064 for this layer, so
            // the right value was known here all along; it was simply not the
            // one being stored.
            // WHY DID THIS BAND CELL COME OUT CLEAR?
            //
            // [lb_bare] counts cells drawn clear where the record says hidden
            // but cannot say which branch left s at 0 -- it inspects the
            // finished table. This catches it at the assignment, with every
            // input that could have decided it, so the answer is read rather
            // than deduced. Four per frame is enough to name a branch.
            // THE CONDITION HAS TO MATCH THE ONE [lb_bare] COUNTS, which is
            // "the cell ends up kClearTile", not "s is 0". The assignment is
            // `(s && have_fog) ? fog : kClearTile`, so a cell the record calls
            // hidden also comes out clear when have_fog is false -- and a
            // first version of this probe tested only !s, fired zero times,
            // and left 46 counted cells unexplained. That gap is the evidence
            // that the other term is the one doing it.
            if ((!s || !have_fog) && g_bare_n < 4u &&
                (trow < 0 || trow >= 24 || h < 0 || h >= 32)) {
                const int dmx = floor_div(ac, 3), dmy = floor_div(ar, 2);
                if (dmx >= 0 && dmy >= 0 && dmx < g_map_w && dmy < g_map_h &&
                    !vis_revealed(dmx, dmy)) {
                    const bool in_seen = ar >= 0 && ar < g_seen_h &&
                                         ac >= 0 && ac < g_seen_w;
                    ++g_bare_n;
                    std::fprintf(stderr,
                                 "[lb_bares] map %d,%d abs tile %d,%d "
                                 "trow %d h %d  s=0 yet record hidden: "
                                 "seen=%d obs=%d fogsrc=%d visbase=%08X "
                                 "bits=%d bitsn=%u havefog=%d\n",
                                 dmx, dmy, ac, ar, trow, h,
                                 in_seen ? (int)g_seen[size_t(ar) *
                                                       size_t(g_seen_w) +
                                                       size_t(ac)] : -1,
                                 observed ? 1 : 0, g_fogsrc_map, g_vis_base,
                                 g_vis_bits.empty()
                                     ? -1
                                     : (int)g_vis_bits[size_t(dmy) *
                                                       size_t(g_map_w) +
                                                       size_t(dmx)],
                                 (unsigned)g_vis_bits.size(),
                                 have_fog ? 1 : 0);
                }
            }
            // A SHROUDED CELL WITH NO FOG TILE IS DRAWN CLEAR, AND CLEAR IS
            // TRANSPARENT -- the terrain the player has not explored shows
            // through. Count it HERE, where the branch is already evaluated,
            // so it costs nothing and is always on.
            //
            // [lb_bare] already reports this, but it walks all 73,000 cells
            // with a record lookup each and is therefore behind LB_FOGBARE,
            // which means nobody playing ever has it on -- and the defect
            // seen in play is exactly unexplored terrain rendering
            // see-through, varying for no apparent reason. A defect that only a
            // diagnostic nobody runs can see is a defect that gets argued
            // about instead of measured. This one rides on the assignment.
            if (s && !have_fog) ++g_bare_fill;
            g_ext2[trow - kExtRowLo][h - kExtLo] =
                (s && have_fog) ? fog : kClearTile;
            g_ext3[trow - kExtRowLo][h - kExtLo] = s ? kShroudTile : 0u;
        }
    }
    // BAND CELLS DRAWN CLEAR WHERE THE RECORD SAYS HIDDEN.
    //
    // AFTER the fill, and that placement is the whole point. The first version
    // of this sat in scan_shroud's periodic report, which runs BEFORE the fill
    // in this same function -- so it inspected the PREVIOUS frame's g_ext2
    // against the CURRENT camera and record. During a pan that mislabels cells
    // wholesale, and the count duly grew with the length of the pan: 22, then
    // 46, then 92 as pan steps were added. It was measuring its own staleness.
    //
    // The giveaway was that an assignment-time probe covering both terms of
    // `(s && have_fog) ? fog : kClearTile` fired ZERO times in the same runs.
    // A cell that is never written that way cannot be written that way; the
    // disagreement was in the reader.
    //
    // Here the table, the camera and the record are all this frame's, so a
    // non-zero count is a real fault in the fill.
    // LB_FOGBARE=1. Diagnostic only, and it walks all kExtRows x kExtCols --
    // about 73,000 cells, each with a record lookup. Even at one frame in 120
    // that is a visible hitch every few seconds, which is not something to
    // leave on in a build someone is playing.
    if (std::getenv("LB_FOGBARE") && (g_scan_frames % 120u) == 0u) {
        unsigned bare = 0;
        int b1x = -1, b1y = -1;
        for (int r = 0; r < kExtRows; ++r)
            for (int c = 0; c < kExtCols; ++c) {
                const int trow = r + kExtRowLo, htile = c + kExtLo;
                if (trow >= 0 && trow < 24 && htile >= 0 && htile < 32)
                    continue;                      // the cartridge's own cells
                if ((g_ext2[r][c] & 0x3FFu) != kClearTile) continue;
                const int mx = floor_div(cam_tx + htile, 3);
                const int my = floor_div(cam_ty + trow, 2);
                if (mx < 0 || my < 0 || mx >= g_map_w || my >= g_map_h)
                    continue;
                if (vis_revealed(mx, my)) continue;   // legitimately clear
                ++bare;
                if (b1x < 0) { b1x = mx; b1y = my; }
            }
        if (bare)
            std::fprintf(stderr,
                         "[lb_bare] %u band cell(s) drawn CLEAR where the "
                         "record says hidden; first at map %d,%d\n",
                         bare, b1x, b1y);
    }

    // Valid as soon as EITHER table has the six tiles -- the band draws the
    // cloud from the first frame that shows one instead of going flat.
    g_ext2_valid = fog_ok || g_fog_prov_ok;
    g_ext3_valid = true;
    // After the tables are built, so it grades this frame's output.
    audit_fog(g_dispcnt);
}

// Which engine-A layers this port can supply widened entries for.
//
// The compositor widens one layer on its own (the furthest back, which is the
// terrain here). The shroud sits at priority 0 and would never be chosen, so it
// has to be named. Answering only while the tables are actually valid means
// that outside the field scene the compositor's own rule is the only one in
// play, exactly as before.
// Does the HOST field own this layer's band columns outright?
//
// Distinct from lb_widemap_extends(), and stronger. `extends` answers "can this
// port supply widened entries for that layer"; a false there still lets
// upstream's own `wide_bg_slot` rule extend the furthest-back layer, which is
// BG1 -- the terrain. So declining in `extends` did not stop BG1 reaching the
// bands, and the band columns kept painting over the host field.
//
// When this returns 1 the layer must not be extended by ANY rule, because the
// host field is already drawing those columns AND is the only thing carrying
// the shroud. See the phase 3 note in apply_hooks beside decode_bg_line.
extern "C" int lb_widemap_hostowns(int bg) {
    // BG1 only, deliberately. BG1 is the terrain and host terrain replaces it
    // exactly. The fog layers are left alone: this repo's own comments
    // disagree about whether the shroud is BG2 or BG3 (lb_hud.cpp says BG2 at
    // priority 1; the 0t stanza says BG3 at priority 0), and extending a fog
    // layer over an already-fogged base can only add fog, never reveal
    // ground. Settle which layer it is before widening this.
    return (bg == 1 && lb_hostterrain_active()) ? 1 : 0;
}

// docs/387: the metatile tables changed (Force Tileset's detail patch).
extern "C" void lb_widemap_terrain_changed(void) { ++g_terrain_gen; }

extern "C" int lb_widemap_extends(int bg) {
    // docs/325 PHASE 2: WHEN THE HOST DRAWS THE TERRAIN, THIS MUST NOT.
    //
    // Measured with host terrain supplying the compositor's base pixel:
    // `band 80/256 kept` -- only 31% of band pixels reached the screen as
    // host terrain, because the band synthesis below was drawing OVER it.
    // The host map was underneath the very thing it exists to replace.
    //
    // Declining BG1's extension leaves the band columns with no BG1 source,
    // so they fall through to the base -- which is the level grid, drawn at
    // full map resolution with no 32-column limit and no camera model to get
    // wrong. The guest's own 256 columns are untouched: BG1 still decodes
    // natively there, and `native 138/256 kept` says the two agree wherever
    // nothing else is drawn on top.
    if (bg == 1 && lb_hostterrain_active()) return 0;
    if (bg == 1) return g_ext_valid ? 1 : 0;
    // BG2 IS THE FOG, AND IT IS KEPT. Retiring it was tried and reverted, for
    // a reason a counter could not have produced -- the two fogs do not look
    // alike. Sampled from one wide frame, band against native:
    //
    //     band   (host shroud)   (24, 20, 32)          near-black, flat
    //     native (guest cloud)   (154,170,178), (138,146,154),
    //                            (178,195,203), (146,162,178)
    //
    // The guest's fog is a light blue-grey CLOUD TEXTURE of at least four
    // tones; the host's is one dark colour. A flat fill cannot be made to
    // match a textured tile by choosing a better constant, so the band read as
    // a dark stripe beside a light one.
    //
    // Letting the guest draw its own fog over the widened columns keeps the
    // art authentic everywhere and needs no cloud art reproduced host-side.
    // The host field stays fogged underneath as the SAFETY NET: if this
    // extension ever fails to reach a column, that column falls through to a
    // base that is already dark rather than revealing unexplored ground.
    if (bg == 2) return g_ext2_valid ? 1 : 0;
    // BG3 IS NOT EXTENDED. Tile 0x1F is not transparent.
    //
    // It was extended on the reasoning that supplying 0x1F beat letting those
    // columns decode BG3 through the hardware wrap. That rested on "0x1F is a
    // transparent marker", which came from filling BG3 with it and seeing no
    // change on screen -- observed at a time when the widened columns were
    // showing garbage anyway, so the test could not have detected a change.
    //
    // It is opaque, and BG3 is priority 0, so it painted over everything. That
    // is the left-edge band: with BG3 extended the band runs surface columns
    // 84..99, and BG3's own decoded line is opaque across exactly 84..98 while
    // transparent either side. The pre-existing artefact is only the native
    // part; extending BG3 stretched a four-column blemish into sixteen.
    //
    // Not extending it means the widened columns get no BG3 source at all
    // (hud_x is negative out there, and the compositor skips a layer with no
    // source column), which is what those columns should have.
    //
    // EXCEPT WHILE A BUILDING IS BEING PLACED. BG3 carries nothing but the
    // placement footprint, and docs/318 measured what "not extended" costs:
    // the ghost drags into a band, the guest still says the site is legal,
    // and no indicator is drawn because there is no BG3 out there to draw it
    // on. While a harvest is live every widened BG3 column is supplied
    // explicitly -- a footprint cell, or nothing at all -- so the opaque band
    // this comment records cannot come back: it came from serving 0x001F,
    // which lb_fp_band_tile never does.
    if (bg == 3) return lb_fp_bands_active();
    return 0;
}

// The fog entry for a widened column: the shroud metatile's tile for this
// phase where the map-space record says the player has not seen this tile, and
// 0 (transparent) where it has, so the widened terrain shows through.
extern "C" int lb_widemap_fog(int htile, int trow, int gy, uint16_t* out) {
    if (!g_ext2_valid) return 0;
    if (guest_cell(htile, gy)) {
        // docs/348: THE ONE PLACE A NATIVE CELL IS SUBSTITUTED. The cartridge
        // punched a clear tile here for a HUD element, and this port moved
        // that element to the window's corner (lb_hud_refog_cell), so the hole
        // would show ground nobody is standing over. The exact rule's table
        // holds this cell's fog as the cartridge would draw it without the
        // hole -- it matches the guest's own window on every non-UI cell
        // (fx_grade) -- so that is what is drawn instead.
        const int cy = trow - g_fill_parity;
        if (g_ext2_exact && g_fill_parity >= 0 && cy >= 0 && cy < 24 &&
            ((g_ui_snap[cy] >> htile) & 1u) &&
            lb_hud_refog_cell(htile, cy) &&
            !ext_out_of_range(htile, trow)) {
            *out = g_ext2[trow - kExtRowLo][htile - kExtLo];
            return 1;
        }
        return 0;
    }
    if (ext_out_of_range(htile, trow)) { ++g_declined_bounds; return 0; }
    // The model must describe the picture -- see g_model_ok. Blank, not
    // declined: a declined cell falls through to the hardware wrap.
    // HOLD THE LAST GOOD TABLE, DO NOT BLANK.
    //
    // Symptom: the screen flickered semi-frequently -- temporarily
    // transparent shroud, a yellow flash, other graphical flickering.
    //
    // choose_camera demands 0 of 48 sampled cells disagree, with no
    // tolerance. During a pan the guest's tilemap genuinely IS a mix of
    // two cameras -- the streamer rewrites columns as the camera crosses
    // a tile -- so no candidate matches all 48 and the model is rejected
    // for that frame. Blanking then paints the whole band tile 0 for one
    // frame: transparent on the fog layer (the terrain shows straight
    // through) and a flat solid tile on terrain (the flash).
    //
    // Declining is NOT the alternative -- the note above is right that a
    // declined cell falls through to the hardware wrap and draws garbage.
    // The third option is to serve what is already in the table, which is
    // the last fill made with a camera the model COULD explain. Slightly
    // stale for one frame beats a full-band flash, and `g_ext_valid`
    // above already guarantees the table has been filled at least once,
    // so there is no uninitialised case to fall into.
    //
    // LB_MODELBLANK=1 restores the blank for A/B.
    if (!g_model_ok) {
        static const bool blank = [] {
            const char* v = std::getenv("LB_MODELBLANK");
            return v && v[0] == '1';
        }();
        if (blank) { *out = 0u; return 1; }
    }
    *out = g_ext2[trow - kExtRowLo][htile - kExtLo];
    // WHAT THIS ACTUALLY SERVES, per band. The table holds fog for 16829 of
    // its 25561 band entries and the picture shows none in the band, so the
    // question is which cells the surface asks for and what they get. Counted
    // at the point of service, which is the only place that knows both.
    //
    // GATED, BECAUSE THIS FUNCTION IS CALLED PER PIXEL.
    //
    // None of this was ever behind a flag, so every player's build ran it. One
    // Free Play run counted 540 MILLION calls (LEFT 50M, RIGHT 96M, ABOVE
    // 131M, BELOW 262M), and each one paid for two counter increments plus a
    // four-iteration loop that re-summed g_fogserve_n from scratch purely to
    // decide whether it was time to print -- then a further ~5000 fprintf +
    // fflush pairs, 856 KB of stderr, on a handle the harness redirects to a
    // file. The game was unacceptably laggy, and excessive logging was one
    // suspect; here that suspicion was correct.
    static const bool lb_fogserve_on = [] {
        const char* v = std::getenv("LB_FOGSERVE");
        return v && v[0] != '0';
    }();
    if (lb_fogserve_on) {
        const uint16_t v = uint16_t(*out & 0x3FFu);
        const bool fogs = (v != 0u && v != kClearTile);
        const int k = (trow < 0)    ? kBandAbove
                    : (trow >= 24)  ? kBandBelow
                    : (htile >= 32) ? kBandRight
                                    : kBandLeft;
        ++g_fogserve_n[k];
        if (fogs) ++g_fogserve_fog[k];
        static unsigned last = 0;
        unsigned all = 0;
        for (int i = 1; i <= 4; ++i) all += g_fogserve_n[i];
        if (all - last >= 200000u) {
            last = all;
            std::fprintf(stderr,
                         "[lb_fogserve] served LEFT %u/%u  RIGHT %u/%u  "
                         "ABOVE %u/%u  BELOW %u/%u  (fog / total asked)\n",
                         g_fogserve_fog[kBandLeft], g_fogserve_n[kBandLeft],
                         g_fogserve_fog[kBandRight], g_fogserve_n[kBandRight],
                         g_fogserve_fog[kBandAbove], g_fogserve_n[kBandAbove],
                         g_fogserve_fog[kBandBelow], g_fogserve_n[kBandBelow]);
            std::fflush(stderr);
        }
    }
    // Grade the band's FOG the same way its terrain is graded: remember what
    // was served at this absolute cell and check it when the camera brings that
    // ground into the guest's window. Stamped with the camera the FILL used
    // (g_fill_cam_*), not the live one -- serving happens later in the frame
    // than filling, and using the live camera is what produced a convincing
    // 88.6% failure that was pure clock skew.
    //
    // fog_record already returns immediately when the audit is off, but the
    // band-kind ladder in front of it did not, and per pixel even three
    // compares are worth not doing. Both are audit bookkeeping, so both move
    // behind the same flag.
    if (g_band_truth_on) {
        g_band_kind = (trow < 0)    ? kBandAbove
                    : (trow >= 24)  ? kBandBelow
                    : (htile >= 32) ? kBandRight
                                    : kBandLeft;
        fog_record(g_fill_cam_ty + trow, g_fill_cam_tx + htile, *out);
    }
    return 1;
}

// The entry that draws NOTHING on an overlay layer.
//
// This is the backstop for the 4-px sliver at the native viewport's left edge.
// The game biases its BGxOFS by 4 - (camx % 8) -- measured live as bgx = 508,
// i.e. -4, on BG1/BG2/BG3 while BG0 stays 0 -- so the first four screen columns
// sample the tilemap column just LEFT of the 32 the game maintains. That column
// is stale: whatever was last written there. On a 256-px screen the result is a
// four-pixel sliver in the extreme left edge, which is easy to never notice;
// widened, the native viewport starts in the middle of the picture and the same
// sliver becomes a bright column straight down the screen.
//
// The fog and shroud suppliers above can only answer while their reconstruction
// is valid, and with the map revealed g_ext2_valid is false -- there is no fog
// to rebuild -- so they decline and the stale tile is drawn anyway. Declining
// has to mean "draw nothing", not "fall through to whatever is in memory".
//
// Both values are MEASURED, not chosen: with the map revealed the port reads
// the guest's own BG2 window as 0x0064 in all 768 entries and its BG3 window as
// 0x001F in all 768, and both decode fully transparent. They are the entries
// these two layers use to mean "clear", which is exactly what an unmaintained
// column outside the guest's window should show.
extern "C" int lb_widemap_clear_tile(int bg, uint16_t* out) {
    if (bg == 2) { *out = kClearTile; return 1; }
    if (bg == 3) { *out = kShroudTile; return 1; }
    return 0;
}

// IS THIS CELL OUTSIDE THE WORLD ALTOGETHER?
//
// Distinct from "shrouded" and from "I cannot answer". A widened column past
// the map's own edge has no terrain to show at ANY visibility. The goal is a
// wide view of the whole game field with the only black space being that
// outside the actual world boundary.
//
// Until now such a column fell through to the hardware tilemap, which is 32
// entries wide and WRAPS, so the columns west of the map redrew the map's own
// first 32 columns over and over. Before this function existed, a capture
// showed exactly that: not gameplay filling the screen, but one small snippet
// of the terrain texture tiling outside the game's own rendering.
//
// Supplying tile 0 instead is not the answer either, and this file already
// recorded why: with the map revealed the right band "was painted with tile 0
// and came out as flat green next to fully detailed terrain". Entry 0 selects
// tile index 0, whose pixels in this tileset are grass, not transparency.
//
// So the caller is told OFF-MAP and emits transparent pixels, letting the
// backdrop through. Black, exactly where the world stops.
extern "C" int lb_widemap_offmap(int htile, int trow) {
    if (!g_geom_valid || g_map_w <= 0 || g_map_h <= 0) return 0;
    // THE CAMERA THE TERRAIN TABLE WAS BUILT FOR, not g_cam_tx: that has two
    // writers (choose_camera and lb_widemap_fill), and at composite time it
    // holds the raw vblank camera, so at a map edge the blanked strip and the
    // terrain beside it disagreed by a pan step while panning.
    const int abs_col = g_fill_cam_tx + htile;
    const int abs_row = g_fill_cam_ty + trow;
    const int mx = floor_div(abs_col, 3);
    const int my = floor_div(abs_row, 2);
    return (mx < 0 || mx >= g_map_w || my < 0 || my >= g_map_h) ? 1 : 0;
}

// THE LEVEL'S SIZE, from the game's own record.
//
// lb_field derives its grid by scanning main RAM for a plausible extent, and
// that heuristic loses the last row: on a map this record calls 64x64 the
// battlefield panel reported 64x63 and drew 1536x1008 instead of 1536x1024.
// The record is not a guess and does not need to be one, so anything that
// wants the level's dimensions should ask here first.
//
// Returns 0 until the geometry has been located, which is also the honest
// answer outside a battle.
// WHERE THE GUEST'S OWN 256 COLUMNS SIT INSIDE THE WIDENED SURFACE.
//
// Six places used to work this out for themselves as (output_width - 256) / 2:
// the HUD anchor, the touch mapping, the entity-in-band counters, the gather
// rect, the 3D viewport. That was one decision written six times, and it is no
// longer "half each side".
//
// At a map edge a centred band hangs off the world, and off the map there is
// nothing to draw -- the port can only paint cloud there once it has SEEN
// cloud, because the six tiles are learned from the guest's own BG2 and no
// table for them exists in the ROM. A run with no fog on screen therefore
// gets a black slab, which is what the captures showed.
//
// A PC RTS does not have this problem because it never shows past the edge.
// C&C confines the WHOLE tactical rect, using the view's own width and height:
//
//     Confine_Rect(&xx, &yy, TacLeptonWidth, TacLeptonHeight,
//                  Cell_To_Lepton(MapCellWidth), Cell_To_Lepton(MapCellHeight));
//         -- REDALERT/DISPLAY.CPP:4243, DisplayClass::Set_Tactical_Position
//
// The same effect costs no gameplay change here: leave the cartridge's camera
// and its pixels exactly as they are, and slide the guest's window within the
// surface so the extra space goes to the side that still has map.
//
// ---- the clamp ------------------------------------------------------------
//
// LB_BANDCLAMP=0 turns it off and restores the exact centred split, so the
// before and after are the same binary, the same window and the same route.
int band_clamp_on() {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_BANDCLAMP");
        on = (v && v[0] == "0"[0]) ? 0 : 1;
    }
    return on;
}

// ONE RULE, BOTH AXES. The horizontal and vertical cases differ only in the
// guest's own extent (256 px against 192), the camera, and the map's size, so
// writing it twice would only create somewhere for the two to disagree -- which
// is exactly what the gather rect's two axes did before they were read
// properly, and it cost this port a wrong patch.
//
//   span      how much surface there is beyond the guest's own extent
//   before    how much map lies between the map's edge and the guest window
//   after     how much lies beyond the far side of it
int band_split(int span, int before, int after) {
    if (span <= 0) return 0;
    const int centred = span / 2;
    // The camera itself outside the map is not a case this can improve on, and
    // a negative "available" would clamp the wrong way. Leave it centred.
    if (before < 0 || after < 0) return centred;

    int lead;
    if (before + after < span) {
        // The whole map is smaller than the surface on this axis. Nothing can
        // fill the edges, so centre the MAP instead and let the overhang fall
        // evenly either side of it -- which is what C&C does when a map is
        // smaller than the tactical view.
        lead = (before - after + span) / 2;
    } else {
        lead = centred;
        if (lead > before) lead = before;              // near edge: slide back
        if (span - lead > after) lead = span - after;  // far edge
    }
    if (lead < 0) lead = 0;
    if (lead > span) lead = span;
    return lead;
}

// Where the guest's own 256 columns should sit, for a surface this wide, given
// where the camera is on the map RIGHT NOW. Pure; the latch below is what makes
// it stable for a frame.
//
// docs/348: THE CAMERA IS A PARAMETER. band_compute() asks about the camera
// read at the last vblank; the geometry path asks about the camera the list it
// is transforming was actually built with (see list_relatch_x). Same rule,
// different moment, and the moment is the caller's to name.
int band_compute_at(int output_width, int camx);
int band_compute(int output_width) {
    return band_compute_at(output_width, g_cam_px_x);
}
int band_compute_at(int output_width, int camx) {
    const int span = output_width - 256;
    if (span <= 0) return 0;
    if (!band_clamp_on()) return span / 2;
    // No map, no edge to clamp to -- menus, title cards, the front end.
    //
    // SAY WHEN THIS FIRES IN A BATTLE. span/2 is 192 on a 640 surface and the
    // clamped answer is 188, so every frame that lands here moves the whole
    // band 4 px -- and it does not land here consistently. Measured with
    // LB_HUDLOG on one run: 14 frames reported band=188 and 6 reported
    // band=192, which is a 4 px alternation and is visible as shake.
    //
    // g_geom_valid is set once and never cleared, so in a battle this is
    // g_cam_px_valid, which lb_widemap_fill() clears on entry and sets only at
    // the very end. Naming the condition is what tells a menu (correct) from a
    // mid-battle bail (not).
    if (!g_geom_valid || !g_cam_px_valid || g_map_w <= 0) {
        static int on = -1;
        if (on < 0) {
            const char* v = std::getenv("LB_BANDWHY");
            on = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (on) {
            static unsigned n = 0;
            if ((n++ % 30u) == 0u) {
                // The bail histogram too: [0] is the SUCCESS counter, bumped at
                // the very end of lb_widemap_fill. If it is 0 while the others
                // are 0 as well, the function is not being CALLED -- which is a
                // different problem from bailing, and no amount of relaxing the
                // gates inside it would ever show up.
                std::fprintf(stderr,
                             "[lb_bandwhy] span/2 fallback: geom=%d cam_px=%d "
                             "map_w=%d  (span %d -> %d)  fill ok=%u "
                             "bail1=%u bail2=%u bail3=%u bail4=%u bail5=%u\n",
                             g_geom_valid ? 1 : 0, g_cam_px_valid ? 1 : 0,
                             g_map_w, span, span / 2,
                             g_fill_bail[0], g_fill_bail[1], g_fill_bail[2],
                             g_fill_bail[3], g_fill_bail[4], g_fill_bail[5]);
                // And the state the gate saw, so "it should have passed" is a
                // reading rather than a belief.
                const uint32_t m = bus_read_u32(kMapPtr);
                std::fprintf(stderr,
                             "[lb_bandwhy]   gate saw dispcnt=%08X map=%08X "
                             "w=%d h=%d t338=%08X row0=%08X\n",
                             g_dispcnt_bail_a, m,
                             main_ram(m) ? (int)bus_read_u8(m + 0x14u) : -1,
                             main_ram(m) ? (int)bus_read_u8(m + 0x15u) : -1,
                             main_ram(m) ? bus_read_u32(m + 0x338u) : 0u,
                             main_ram(m) ? bus_read_u32(m + 0x34u) : 0u);
                std::fflush(stderr);
            }
        }
        return span / 2;
    }
    // A metatile is 3 tiles across at 8 px each.
    const int map_px = g_map_w * 24;
    // FOUR PIXELS OF SCROLL BIAS, and they are not a rounding error -- they
    // were the entire undrawn residue once the clamp was in. Measured at
    // 786x426 parked against the west edge: columns 0..3 and NO others drew
    // nothing, on every row, 0.51% of the picture.
    //
    // The camera is not where the picture starts. func_0202d1b4 writes BGxOFS
    // as (-(4 - camx % 8)) & 0x1FF, so the hardware samples the tilemap from
    // `camx + bias` with bias in -4..+3 -- and the decoder applies exactly that
    // (lb_bias in decode_text_line). Clamping the band to camx therefore aligns
    // the surface to the camera and leaves the first `-bias` pixels sampling
    // map column -1, which is off the world and draws nothing.
    //
    // -4 is the worst case of that range rather than the live value, because
    // the live one belongs to the renderer and asking for it here would couple
    // the clamp to a register read this file does not otherwise need. Being up
    // to 7 px tighter than strictly necessary costs at most 7 px of the far
    // band and can never reopen the sliver.
    //
    // FLOORED AT ZERO, NOT HANDED TO band_split NEGATIVE. The first version of
    // this passed `camx - 4` straight through, and at the west edge camx IS 0,
    // so `before` went negative and tripped band_split's "the camera is outside
    // the map" guard -- which returns the CENTRED band and disengages the clamp
    // altogether. Measured: undrawn went from 0.51% back up to 34%, the whole
    // black slab restored by a correction meant to shave four pixels off it.
    // At camx 0..3 there is genuinely no map left of the bias to show and the
    // sliver is inherent; everywhere else this removes it.
    const int kBias = 4;
    int before = camx - kBias;
    int after = map_px - (camx + 256) + kBias;
    if (before < 0) before = 0;
    if (after < 0) after = 0;
    return band_split(span, before, after);
}

// THE SAME THING VERTICALLY, and it is not decorative: with the horizontal
// clamp in and the map revealed, the only undrawn pixels left in a 1266x696
// capture were 108 rows across the WHOLE width at the top -- the north edge of
// the world hanging off the top of the surface, which is the black slab above
// rotated ninety degrees. A metatile is 2 tile rows at 8 px, so 16 px tall
// against the 24 px the column axis uses.
int band_compute_y_at(int output_height, int camy);
int band_compute_y(int output_height) {
    return band_compute_y_at(output_height, g_cam_px_y);
}
int band_compute_y_at(int output_height, int camy) {
    const int span = output_height - 192;
    if (span <= 0) return 0;
    if (!band_clamp_on()) return span / 2;
    if (!g_geom_valid || !g_cam_px_valid || g_map_h <= 0) return span / 2;
    const int map_px = g_map_h * 16;
    // NO BIAS TERM HERE, deliberately. The horizontal one above is there
    // because a 4 px undrawn sliver was MEASURED at the west edge and traced to
    // BGxOFS; nothing equivalent has been measured vertically -- the same
    // capture reported no undrawn row at all -- and the run that would show it
    // parks against the map's north edge, which this route does not reach.
    // Guessing a vertical bias by symmetry would be inventing a correction for
    // a defect no measurement has seen.
    int before = camy;
    int after = map_px - (camy + 192);
    if (before < 0) before = 0;
    if (after < 0) after = 0;
    return band_split(span, before, after);
}

// ---- ONE VALUE PER FRAME, and the frame it belongs to ---------------------
//
// The band is a translation of the WHOLE picture, so 2D and 3D disagreeing
// about it by even a frame would slide the units off the terrain while the
// camera approaches a map edge -- a pan speed's worth of offset, several
// pixels, exactly the kind of "units drift" this port has already chased once.
//
// So it is latched once per vblank and then handed forward along the same path
// the geometry itself takes:
//
//   lb_band_latch()        at vblank, from the camera read that vblank
//   lb_band_for_geometry() GPU3D's viewport transform, which RECORDS what it
//                          used rather than being told when it ran -- so no
//                          assumption is made about whether the guest's vblank
//                          IRQ runs before or after GPU3D's own VBlank()
//   lb_band_promote()      GPU3D::VBlank, when the geometry banks swap: the
//                          value the geometry now queued for rendering used
//   lb_band_render()       what the compositor stores beside the 3D snapshot
//
// The compositor then uses the value that came back with the 3D lines it is
// actually compositing, so the two cannot shear apart however the frames
// interleave. The 2D layers may therefore lag the live camera by a frame at a
// map edge; that is a frame of the picture stopping slightly late, which is
// not visible, and it is the safe direction to be wrong in.
int g_band_w = 0;              // the surface width the latch was computed for
int g_band_left = 0;           // and the value, for that width
// The clip-space shift the 3D geometry is placed by: band_left minus the
// centred split, latched once per frame. Separate from g_band_left because the
// geometry path must NEVER take lb_band_left's centred fallback -- see the note
// in lb_band_latch. One value, one frame, no width test.
int g_geom_shift = 0;
int g_band_used = 0;           // what the geometry pipeline last transformed with
int g_band_render = 0;         // ...promoted when its banks swapped
int g_cam_used_x = 0, g_cam_used_y = 0;      // camera the geometry used
int g_cam_render_x = 0, g_cam_render_y = 0;  // ...promoted with its banks
int g_band_h = 0;              // the same four, vertically
int g_band_top = 0;
int g_band_used_y = 0;
int g_band_render_y = 0;
// COUNTED, because a hand-off that silently does not happen looks exactly like
// one that happens and agrees. The first run of this chain produced a composite
// byte-identical to the unclamped arm, which is what a band of 0 everywhere
// would produce and also what "the clamp does nothing" would produce.
unsigned g_band_geom_calls = 0;
// Times lb_band_left answered with the centred split because the width it
// was asked about was not the latched one. Cumulative, never reset -- the
// g_bare_fill rule -- so an occurrence between two band-log lines survives.
unsigned g_band_fallbacks = 0;
unsigned g_band_promotes = 0;

extern "C" int lb_band_left(int output_width) {
    const int total = output_width - 256;
    if (total <= 0) return 0;
    // A caller asking about a width this frame was not latched for gets the
    // honest centred answer for THAT width rather than a value belonging to a
    // different surface.
    if (output_width == g_band_w) return g_band_left;
    // COUNT THE FALLBACK. It is honest but it is also a cliff: at a wide
    // surface the centred and clamped splits differ by hundreds of pixels, so
    // any caller that takes it disagrees wholesale with any caller that does
    // not. That has now produced two visible defects -- the pan smear, where
    // two vertices of one polygon took different answers, and the resize nudge,
    // where the viewport took it and the vertex shift did not.
    //
    // Both were found by reasoning backwards from a picture. A count makes the
    // next one visible forwards instead, and it is free: one increment on a
    // branch that is already being taken, reported on the band line that
    // already prints.
    ++g_band_fallbacks;
    return total / 2;
}

// THE GEOMETRY PATH'S OWN ACCESSOR. Deliberately ignores its argument.
//
// It takes a width only so the call site in the vendored 3D code reads the same
// as the one it replaces. It must not compare that width against anything: the
// comparison is exactly what made lb_band_left drop to a centred split hundreds
// of pixels away, and doing it per vertex is what let a single polygon be
// submitted half-shifted and get stretched to the frustum edge by the clipper.
// ---- THE CAMERA EACH LAYER WAS DRAWN WITH (docs/348) ----------------------
//
// The cartridge draws a frame from ONE camera, committed once per game update
// (Task_View_Update 0203942c at task priority 0x15 -> View_CommitCamera
// 020398b8), and every layer reads that one value:
//
//   * the 3D list -- units and buildings at (camx-4, camy), effects at
//     (camx, camy) -- is built by tasks 0x1D/0x1F/0x20 AFTER the commit and
//     handed over by SwapBuffers (task 0x25, 02024d08), the last task;
//   * the ground -- BG1 terrain and BG2 fog, staged at 0x23/0x1D from the same
//     camera -- is committed by Task_View_CommitBGs (020396e0), which is
//     priority 0x01 of the NEXT update: it reads view+0x5E/+0x60 at that
//     moment, writes BGxOFS through Bg_SetScroll (0202d1b4) and DMAs the
//     staging.
//
// The one-run deferral pairs the two only when the next update starts at the
// very next VBlank. NitroMain (020f6730) waits a SECOND VBlank whenever an
// update finished in under 20 ms (020f6904..020f6928: t1 - t0 < 0x14 ->
// OS_WaitVBlankIntr again), and then the 3D list latches one VBlank BEFORE the
// ground that goes with it is committed. That is the sprite-against-ground
// shudder of docs/342, phase-locked to the game's two-updates-in-three cadence
// exactly as its lagscan found -- and why no fixed offset could remove it.
//
// So each layer is TAGGED with the camera it was drawn with, read from the
// game's own view object at the moment the game hands that layer over, and
// the widened composite pairs the 3D raster whose tag matches the ground's.
struct LbCamTag {
    int x = 0, y = 0;
    bool ok = false;
};
LbCamTag g_tag_swap;     // read when the guest's SwapBuffers executes
LbCamTag g_tag_render;   // ...promoted with its list at the VBlank latch
LbCamTag g_tag_bg;       // read when the guest commits BG1's scroll
unsigned g_tag_bg_n = 0;

static LbCamTag read_view_cam_tag(void) {
    LbCamTag t;
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return t;
    t.x = read_s16(view + 0x5Eu);
    t.y = read_s16(view + 0x60u);
    // Same plausibility window lb_widemap_fill uses for "a view with a
    // camera in it" -- a menu's stale pointer can read anything.
    t.ok = t.x > -256 && t.x < 8192 && t.y > -256 && t.y < 8192;
    return t;
}

// ---- THE BAND A LIST IS PLACED BY, FROM THAT LIST'S OWN CAMERA -------------
//
// The geometry used to take the band latched at the PREVIOUS vblank, from the
// camera read there -- the camera of the update before the one building the
// list. Mid-map the two give the same (centred) band; near a map edge the
// band slides with the camera, and a list placed by the previous camera's
// band shows one pan step past the world's edge on the far side, or stops a
// step short of it, for exactly the frames the camera is moving. The first
// vertex a list submits comes after its update's camera commit (every GX
// writer is a task of priority 0x18 or later), so the view object holds that
// camera there.
//
// Latched ONCE per list, at its first vertex, and held for every later vertex
// of it: the band must not move between two vertices of one polygon (the
// stretch that lb_band_latch's own comment records).
static bool g_list_relatch_x = true, g_list_relatch_y = true;
static int g_list_left = 0, g_list_shift = 0;
static int g_list_top = 0, g_list_shift_y = 0;
unsigned g_list_relatches = 0, g_list_from_view = 0;

static int list_band_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_LISTBAND");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on;
}

static void list_relatch_x(int render_width) {
    g_list_relatch_x = false;
    ++g_list_relatches;
    const int total = render_width - 256;
    int left = (render_width == g_band_w) ? g_band_left
                                          : (total > 0 ? total / 2 : 0);
    if (list_band_on() && total > 0) {
        const LbCamTag c = read_view_cam_tag();
        if (c.ok && g_geom_valid && g_cam_px_valid) {
            left = band_compute_at(render_width, c.x);
            ++g_list_from_view;
        }
    }
    g_list_left = (total > 0) ? left : 0;
    g_list_shift = (total > 0) ? (left - total / 2) : 0;
}

static void list_relatch_y(int render_height) {
    g_list_relatch_y = false;
    const int total = render_height - 192;
    int top = (render_height == g_band_h) ? g_band_top
                                          : (total > 0 ? total / 2 : 0);
    if (list_band_on() && total > 0) {
        const LbCamTag c = read_view_cam_tag();
        if (c.ok && g_geom_valid && g_cam_px_valid)
            top = band_compute_y_at(render_height, c.y);
    }
    g_list_top = (total > 0) ? top : 0;
    g_list_shift_y = (total > 0) ? (top - total / 2) : 0;
}

// The guest's SwapBuffers executed (GPU3D command 0x50): the list it closes
// was built with the camera the view holds now, and the NEXT vertex belongs
// to the next list, which must take its own band.
extern "C" void lb_tag_on_swap(void) {
    g_tag_swap = read_view_cam_tag();
    g_list_relatch_x = true;
    g_list_relatch_y = true;
}

// Task_View_CommitBGs wrote BG1's scroll: the ground now in VRAM is this
// camera's (it reads view+0x5E/+0x60 for that very write).
extern "C" void lb_tag_on_bg1(void) {
    g_tag_bg = read_view_cam_tag();
    ++g_tag_bg_n;
}

// Task_View_CommitBGs wrote BG2's scroll: the fog staging it is about to DMA
// was built from the fog record as it stands NOW (before this update's
// simulation runs), so this is the moment to take the record the widened fog
// must be drawn from. See g_fx_committed.
extern "C" void lb_tag_on_bg2(void) {
    if (!fx_on() || !g_geom_valid || !g_cam_px_valid) return;
    g_fx_committed = fx_read_record();
}

extern "C" int lb_band_geom_shift(int render_width) {
    if (g_list_relatch_x) list_relatch_x(render_width);
    return g_list_shift;
}

// The clip-space VERTICAL shift, the twin of lb_band_geom_shift (docs/348).
extern "C" int lb_band_geom_shift_y(int render_height) {
    if (g_list_relatch_y) list_relatch_y(render_height);
    return g_list_shift_y;
}

// Is the vertical band applied in clip space (SubmitVertex) rather than after
// clipping (the viewport transform)? See apply_hooks.py, docs/348. Read once;
// both call sites must agree or the picture moves twice.
extern "C" int lb_clip_y_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_CLIPY");
        on = (v && v[0] == '0') ? 0 : 1;
    }
    return on;
}

extern "C" int lb_band_right(int output_width) {
    const int rest = output_width - 256 - lb_band_left(output_width);
    return rest > 0 ? rest : 0;
}

extern "C" int lb_band_top(int output_height) {
    const int span = output_height - 192;
    if (span <= 0) return 0;
    if (output_height == g_band_h) return g_band_top;
    return span / 2;
}

extern "C" int lb_band_bottom(int output_height) {
    const int rest = output_height - 192 - lb_band_top(output_height);
    return rest > 0 ? rest : 0;
}

// The full overhang, independent of how it is split. What a consumer that must
// cover the band WHEREVER it lands needs -- the entity gather rect, which is
// patched into guest code and must not be rewritten every time the camera
// moves a pixel.
extern "C" int lb_band_span(int output_width) {
    const int total = output_width - 256;
    return total > 0 ? total : 0;
}

extern "C" int lb_band_span_y(int output_height) {
    const int total = output_height - 192;
    return total > 0 ? total : 0;
}

extern "C" int lb_band_latch(int output_width, int output_height) {
    // VALUE BEFORE WIDTH. Publication order, not decoration.
    //
    // This wrote the width first. Between the two stores the pair was
    // INCONSISTENT -- the new width alongside the previous frame's value -- and
    // lb_band_left's `output_width == g_band_w` test would accept it and hand
    // back a band belonging to the frame before. Writing the value first means
    // a reader either sees the old width (and takes the honest centred answer)
    // or the new width with the value that belongs to it. It never sees a new
    // width wearing a stale band.
    const int fresh_left = band_compute(output_width);
    g_band_left = fresh_left;
    g_band_w = output_width;

    // THE SHIFT THE GEOMETRY IS PLACED BY, latched once per frame.
    //
    // Symptom: sometimes, when panning the camera, sprites or even special
    // models stretched horizontally very wide.
    //
    // The 3D path applies its band shift in CLIP space, per vertex, by calling
    // lb_band_left(RenderWidth) for every vertex of every polygon. That has two
    // failure modes and the second is the stretch:
    //
    //   1. lb_band_left falls back to the CENTRED split whenever the width it
    //      is asked about is not the width that was latched. That is not a
    //      small error -- at a wide surface the centred and clamped
    //      splits differ by hundreds of pixels -- so a width disagreement moves
    //      every vertex at once.
    //   2. Worse, if the answer changes BETWEEN two vertices of one polygon,
    //      the polygon is submitted half-shifted. The shift is applied before
    //      clipping, so one vertex can land inside the frustum and its
    //      neighbour outside; the clipper then synthesises a boundary vertex
    //      and the quad is stretched to the edge of the screen. That is the
    //      smear, and it only appears while panning because the split only
    //      moves when the clamp engages near a world edge.
    //
    // A single value, latched here and read without any width test, cannot do
    // either. It is at worst one frame stale -- which is invisible, because the
    // whole frame is drawn with one consistent band -- instead of inconsistent
    // within a single polygon, which is not.
    {
        const int total = output_width - 256;
        g_geom_shift = (total > 0) ? (fresh_left - total / 2) : 0;
    }
    g_band_h = output_height;
    g_band_top = band_compute_y(output_height);
    /* LB_BANDLOG=1 prints the band whenever the split actually moves. */
    static int log_on = -1;
    if (log_on < 0) log_on = std::getenv("LB_BANDLOG") ? 1 : 0;
    if (log_on) {
        // ON CHANGE **AND** PERIODICALLY.
        //
        // On change alone prints only TRANSITION frames, and a transition frame
        // is exactly where the geometry pipeline is legitimately still carrying
        // the previous width's value -- so every line said "used" disagreed
        // with "left" and none of them said whether the steady state was right.
        // That read as a broken hand-off and was not one.
        // ...AND AN UNBIASED MODE, because the trigger above makes the log
        // useless for the one question it looks like it answers.
        //
        // Printing "on change" means the sample is almost entirely transition
        // frames, where `used` legitimately still carries the previous value.
        // Counting `used != left` over those lines yields 99% and means
        // nothing -- it is the selection rule, not a defect. That statistic was
        // computed and briefly believed (docs/258) before this comment was
        // read, which is exactly what it warns about.
        //
        // LB_BANDLOG_ALL=N prints every Nth frame regardless of whether
        // anything moved, so "does `used` disagree with `left` in the steady
        // state" becomes answerable instead of merely askable.
        static int every = -1;
        if (every < 0) {
            const char* v = std::getenv("LB_BANDLOG_ALL");
            every = v ? std::atoi(v) : 0;
            if (every < 0) every = 0;
        }
        static unsigned all_n = 0;
        const bool force_all = every > 0 && ((all_n++ % (unsigned)every) == 0u);
        static int last_left = -1, last_w = -1, last_top = -1;
        static unsigned n = 0;
        if (force_all || g_band_left != last_left || output_width != last_w ||
            g_band_top != last_top || (++n % 240u) == 0u) {
            last_left = g_band_left;
            last_w = output_width;
            last_top = g_band_top;
            std::fprintf(stderr,
                         "[lb_band] %dx%d  left=%d right=%d top=%d bot=%d  "
                         "cam=%d,%d map=%dx%dpx geom=%d  usedx=%d renderx=%d "
                         "usedy=%d rendery=%d (geomcalls=%u promotes=%u) "
                         "campick age=%d late=%u fillcam=%d rendercam=%d "
                         "fill ok=%u bail 1=%u 2=%u 3=%u 4=%u 5=%u "
                         "bailA=%08X bailB=%08X runmax=%u okrun=%u "
                         "fallbacks=%u\n",
                         output_width, output_height, g_band_left,
                         output_width - 256 - g_band_left, g_band_top,
                         output_height - 192 - g_band_top,
                         g_cam_px_x, g_cam_px_y,
                         g_map_w * 24, g_map_h * 16,
                         g_cam_px_valid ? 1 : 0,
                         g_band_used, g_band_render,
                         g_band_used_y, g_band_render_y,
                         g_band_geom_calls, g_band_promotes,
                         // HOW FAR BACK THE CAMERA THE PICTURE IS DRAWN FROM
                         // CAME. choose_camera computes both of these every
                         // frame and, until now, NOTHING PRINTED EITHER -- so
                         // the one quantity that would say whether the 2D
                         // layers are lagging the 3D geometry during a pan was
                         // being measured and thrown away. Sprites were seen
                         // going jittery and dishevelled while in motion; a
                         // non-zero age here is what that would look like
                         // from the inside.
                         g_cam_pick_age, g_cam_pick_late,
                         // THE TWO CAMERAS, on ONE line so the pair can never
                         // be read from different moments.
                         //
                         // fillcam is the camera the TERRAIN matches:
                         // choose_camera picks it by comparing candidates
                         // against the guest's own BG1 tilemap, so the
                         // extension table is built for it by construction.
                         // rendercam is the camera the geometry was placed
                         // against, promoted beside the 3D snapshot.
                         //
                         // A DIFFERENCE HERE IS NOT THE MOTION JITTER, and this comment
                         // said it was until the consumers were checked.
                         // g_cam_render_x has exactly one reader --
                         // dump_polygon_list_if_asked() under LB_POLYLIST --
                         // so it is DIAGNOSTIC ONLY and drives no pixel. The
                         // compositor pairs on the BAND (g_wide_3d_band /
                         // lb_band_render), not on this. Measured anyway,
                         // since it is free: settled frames differ 28.4% of
                         // the time, 105 of them by -658 px, and none of that
                         // reaches the screen.
                         //
                         // Kept because it is the right instrument for the
                         // question "is the polygon list being compared
                         // against the wrong camera", which is what it was
                         // added for.
                         g_cam_tx * 8, g_cam_render_x,
                         g_fill_bail[0], g_fill_bail[1], g_fill_bail[2],
                         g_fill_bail[3], g_fill_bail[4], g_fill_bail[5],
                         g_dispcnt_bail_a, g_dispcnt_bail_b,
                         g_dispcnt_run_max, g_dispcnt_ok_run,
                         g_band_fallbacks);
            std::fflush(stderr);
        }
    }
    return g_band_left;
}

extern "C" int lb_band_for_geometry(int render_width) {
    // THE LATCHED BAND, NOT lb_band_left's WIDTH-TESTED ONE.
    //
    // Symptom: while resizing the window, sprites nudged as the camera moved.
    //
    // The same geometry is placed by TWO things that have to agree: this
    // viewport offset, and the clip-space vertex shift that reads
    // lb_band_geom_shift(). Before the smear fix both derived from
    // lb_band_left(RenderWidth), so when that took its centred fallback they
    // took it together and stayed consistent. The smear fix deliberately gave
    // the vertex path a latched value with no width test -- correctly, because
    // a per-vertex width disagreement is what stretched polygons -- and left
    // this one on the width test. So during a RESIZE, when RenderWidth has
    // changed but the latch has not caught up, the viewport moves to the
    // centred split while the vertices stay on the latched band, and the
    // geometry is displaced relative to the viewport it is drawn into. That is
    // the nudge, and the smear fix introduced it.
    //
    // Both now come from the same latch. g_band_left and g_geom_shift are
    // assigned from one `fresh_left` in lb_band_latch, so viewport and vertices
    // cannot disagree by construction rather than by luck.
    //
    // The cost is the same one the vertex path already accepted and documented:
    // at worst one frame stale during a resize. A whole frame drawn with one
    // consistent band is invisible; a frame drawn with two is not.
    //
    // docs/348: both now come from the LIST's latch (list_relatch_x), taken
    // from the camera the list was built with, still once per list.
    if (g_list_relatch_x) list_relatch_x(render_width);
    const int v = (render_width > 256) ? g_list_left : 0;
    g_band_used = v;
    // THE CAMERA THIS GEOMETRY IS BEING PLACED AGAINST, carried alongside the
    // band for the same reason and by the same route.
    //
    // Without it a dump of the render polygon list cannot be compared with a
    // camera at all: the list is the PREVIOUS frame's geometry and any camera
    // read at dump time is this frame's, so a stationary object's computed
    // world position swings by one pan step every time the camera moves. That
    // is a measurement artefact shaped exactly like the shear it would be used
    // to detect, and the first run of this produced +/-17 px of it.
    g_cam_used_x = g_cam_px_x;
    g_cam_used_y = g_cam_px_y;
    ++g_band_geom_calls;
    return v;
}

extern "C" int lb_band_for_geometry_y(int render_height) {
    if (g_list_relatch_y) list_relatch_y(render_height);
    const int v = (render_height > 192) ? g_list_top : 0;
    g_band_used_y = v;
    return v;
}

extern "C" void lb_evt(int kind);   /* lb_patches.cpp, docs/342 */
extern "C" void lb_pair_on_latch(void);   /* lb_patches.cpp, docs/343 */
extern "C" void lb_band_promote(void) {
    lb_evt(1);   // the VBlank latch of a new geometry list
    lb_pair_on_latch();
    g_band_render = g_band_used;
    g_band_render_y = g_band_used_y;
    g_cam_render_x = g_cam_used_x;
    g_cam_render_y = g_cam_used_y;
    g_tag_render = g_tag_swap;   // docs/348: the camera this list was built with
    ++g_band_promotes;
}

// ---- WHICH RASTER GOES WITH THIS GROUND (docs/348) -------------------------
//
// Two 3D rasters are available when the widened composite runs at the 263->0
// wrap: the LIVE one (the list latched at the VBlank just past, rasterised
// from VCount 215) and the SNAPSHOT (the one on screen during the frame that
// just ended, copied during its scanlines). The ground in VRAM is whatever
// Task_View_CommitBGs last committed. Returns 1 to composite the snapshot and
// 0 for the live raster:
//
//   live tag == ground tag       live      (the DS's own pairing held)
//   snapshot tag == ground tag   snapshot  (the list latched a VBlank before
//                                           the ground that goes with it)
//   neither                      live      (a camera jump; nothing matches)
//
// Outside a battle, or with no tags yet, the old rule stands: the snapshot
// whenever it exists at this size. LB_PAIRCAM=0 restores the old rule
// everywhere; LB_LIVE3D=1 still forces the live raster.
extern "C" int lb_live3d(void);   // lb_patches.cpp, docs/342
extern "C" unsigned lb_dump_frame_get(void);
unsigned g_pick_live = 0, g_pick_snap = 0, g_pick_none = 0, g_pick_old = 0;
extern "C" int lb_pair_pick(int snap_ok, int snap_x, int snap_y,
                            int snap_tag_ok) {
    static int mode = -1;
    if (mode < 0) {
        const char* v = std::getenv("LB_PAIRCAM");
        mode = (v && v[0] == '0') ? 0 : 1;
    }
    static int log_on = -1;
    if (log_on < 0) log_on = std::getenv("LB_PAIRLOG") ? 1 : 0;
    if (lb_live3d()) return 0;
    const bool in_field = g_geom_valid && g_cam_px_valid;
    if (mode == 0 || !in_field || !g_tag_bg.ok || !g_tag_render.ok) {
        ++g_pick_old;
        return snap_ok ? 1 : 0;
    }
    const bool live_match = g_tag_render.x == g_tag_bg.x &&
                            g_tag_render.y == g_tag_bg.y;
    const bool snap_match = snap_ok && snap_tag_ok &&
                            snap_x == g_tag_bg.x && snap_y == g_tag_bg.y;
    const int pick = live_match ? 0 : snap_match ? 1 : 0;
    if (live_match) ++g_pick_live;
    else if (snap_match) ++g_pick_snap;
    else ++g_pick_none;
    if (log_on)
        std::fprintf(stderr,
                     "[lb_pairpick] frame %u bg %d,%d live %d,%d snap %d,%d%s "
                     "-> %s  (live %u snap %u none %u old %u)\n",
                     lb_dump_frame_get(), g_tag_bg.x, g_tag_bg.y,
                     g_tag_render.x, g_tag_render.y, snap_x, snap_y,
                     (snap_ok && snap_tag_ok) ? "" : " (no snapshot)",
                     live_match ? "LIVE" : snap_match ? "SNAP" : "none",
                     g_pick_live, g_pick_snap, g_pick_none, g_pick_old);
    return pick;
}

// The camera of the ground on screen: the one Task_View_CommitBGs last
// committed, which the composite pairs everything else with (docs/348). For
// consumers that must follow the picture the player is looking at -- the drag
// box's world-anchored press corner.
extern "C" int lb_shown_cam(int* x, int* y) {
    if (x) *x = g_tag_bg.x;
    if (y) *y = g_tag_bg.y;
    return g_tag_bg.ok ? 1 : 0;
}

// The camera tag the snapshot copy stores beside its rows (gpu2d.cpp).
extern "C" int lb_tag_render_get(int* x, int* y) {
    if (x) *x = g_tag_render.x;
    if (y) *y = g_tag_render.y;
    return g_tag_render.ok ? 1 : 0;
}

// ---- THE WIDENED TABLES, BUILT FOR THE GROUND BEING DRAWN (docs/348) -------
//
// Called by the widened composite, once, before any row is decoded. The
// native columns are about to be decoded from VRAM as the guest's last
// Task_View_CommitBGs left it, and that commit's camera is g_tag_bg -- so the
// widened columns are built for exactly that camera, and the two meet without
// a seam. Checked against the tilemap first (the same 48-cell test
// choose_camera uses, zero tolerance): a tag that does not describe the
// picture is never drawn from. If it fails, choose_camera is asked instead,
// against this same VRAM; if that fails too, the last tables stand.
// The cells the command icons would have marked had the cartridge's grid been
// as wide as the surface -- see g_ui_punch. `bg_x/bg_y` is the camera the fog
// table is being built for.
unsigned g_ui_icon_polys = 0, g_ui_icon_inside = 0;   // diagnostics
static void collect_ui_punches(int bg_x, int bg_y) {
    static const uint32_t kIconDepth = 4275712u;   // z 502, the command icon
    g_ui_punch.clear();
    uint64_t sig = 1469598103934665603ull;
    const int dx = g_tag_render.ok ? (g_tag_render.x - bg_x) : 0;
    const int dy = g_tag_render.ok ? (g_tag_render.y - bg_y) : 0;
    const int parity = (bg_y >> 3) & 1;            // Fog_ApplyUiCellMask's row
    const uint32_t n = nds_gpu3d_render_polygon_count();
    for (uint32_t i = 0; i < n; ++i) {
        NdsGpu3dPolygonSnapshot p{};
        if (!nds_gpu3d_render_polygon(i, &p)) continue;
        if (p.min_z != kIconDepth) continue;
        ++g_ui_icon_polys;
        // The quad in the DS's own screen coordinates, then moved to where it
        // stands under the camera the fog table is for.
        const int x = (int)p.min_x - g_band_render + dx;
        const int y = (int)p.min_y - g_band_render_y + dy;
        const int w = (int)p.max_x - (int)p.min_x;
        const int h = (int)p.max_y - (int)p.min_y;
        if (w <= 0 || h <= 0) continue;
        const int cx0 = floor_div(x - 4, 8), cx1 = floor_div(x - 4 + w, 8);
        const int cy0 = floor_div(y, 8), cy1 = floor_div(y + h, 8);
        for (int cy = cy0; cy < cy1; ++cy)
            for (int cx = cx0; cx < cx1; ++cx) {
                // The cartridge's own grid covers these; it punched them.
                if (cx >= 0 && cx < 32 && cy >= 0 && cy < 24) {
                    ++g_ui_icon_inside;
                    continue;
                }
                g_ui_punch.emplace_back(cy + parity, cx);
                sig = (sig ^ uint64_t(uint32_t(cy + parity))) *
                      1099511628211ull;
                sig = (sig ^ uint64_t(uint32_t(cx))) * 1099511628211ull;
            }
    }
    g_ui_punch_sig = g_ui_punch.empty() ? 0u : sig;
    if (!g_ui_punch.empty()) {
        g_ui_punch_cells += (unsigned)g_ui_punch.size();
        ++g_ui_punch_frames;
    }
}

unsigned g_prep_tag = 0, g_prep_pick = 0, g_prep_hold = 0;
extern "C" void lb_widemap_compose_prep(int output_width, int output_height) {
    if (!compose_fill_enabled()) return;
    if (!g_geom_valid || !g_cam_px_valid || g_map_w <= 0) return;
    g_prep_seq = g_scan_seq;   // the composite is building the tables now
    g_fill_bounds = bounds_for_surface(output_width, output_height);
    const uint32_t dispcnt = nds_io_debug_read(9, 0x04000000u, 32);
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg1cnt = nds_io_debug_read(9, 0x0400000Au, 16);
    const uint32_t base = block + ((bg1cnt >> 8) & 0x1Fu) * 0x800u;
    int cam_tx = 0, cam_ty = 0;
    bool have = false;
    if (g_tag_bg.ok) {
        cam_tx = floor_div(g_tag_bg.x, 8);
        cam_ty = floor_div(g_tag_bg.y, 8) & ~1;   // func_020A3234's alignment
        have = sample_mismatch(cam_tx, cam_ty, base) == 0;
        if (have) ++g_prep_tag;
    }
    // The UI holes need the ground's camera in PIXELS (its row parity), which
    // only the tag has; without it this frame draws no holes rather than
    // holes in the wrong place.
    if (have) {
        collect_ui_punches(g_tag_bg.x, g_tag_bg.y);
        g_fill_parity = floor_div(g_tag_bg.y, 8) & 1;
        // The holes in the BG2 now in VRAM (see fx_ui_cell), for the
        // refill of the ones a moved HUD element left behind. A row at a time:
        // 32 bits are four bytes of one row.
        std::memset(g_ui_snap, 0, sizeof g_ui_snap);
        const uint32_t view = bus_read_u32(kViewPtr);
        const uint32_t rows = main_ram(view)
            ? bus_read_u32(view + 0x164u + 0x14u) : 0u;
        if (main_ram(rows))
            for (int cy = 0; cy < 24; ++cy) {
                const uint32_t row = bus_read_u32(rows + uint32_t(cy) * 4u);
                if (!main_ram(row)) break;
                g_ui_snap[cy] = uint32_t(bus_read_u8(row)) |
                                (uint32_t(bus_read_u8(row + 1u)) << 8) |
                                (uint32_t(bus_read_u8(row + 2u)) << 16) |
                                (uint32_t(bus_read_u8(row + 3u)) << 24);
            }
    } else {
        std::memset(g_ui_snap, 0, sizeof g_ui_snap);
        g_ui_punch.clear();
        g_ui_punch_sig = 0;
        g_fill_parity = -1;
    }
    if (!have) {
        choose_camera(dispcnt);
        if (g_model_ok) {
            cam_tx = g_cam_tx;
            cam_ty = g_cam_ty;
            have = true;
            ++g_prep_pick;
        }
    }
    if (!have) {
        ++g_prep_hold;
        return;
    }
    fill_terrain(cam_tx, cam_ty);
    fill_fog_for(cam_tx, cam_ty);
    static int log_on = -1;
    if (log_on < 0) log_on = std::getenv("LB_PAIRLOG") ? 1 : 0;
    if (log_on)
        std::fprintf(stderr,
                     "[lb_prep] frame %u fill %d,%d (tag %u pick %u hold %u) "
                     "bounds r %d..%d h %d..%d  ui holes %u (frames with "
                     "any %u, cells %u; icon quads seen %u, their native "
                     "cells %u)\n",
                     lb_dump_frame_get(), cam_tx, cam_ty, g_prep_tag,
                     g_prep_pick, g_prep_hold, g_fill_bounds.rlo,
                     g_fill_bounds.rhi, g_fill_bounds.hlo, g_fill_bounds.hhi,
                     (unsigned)g_ui_punch.size(), g_ui_punch_frames,
                     g_ui_punch_cells, g_ui_icon_polys, g_ui_icon_inside);
}

// HOW OFTEN THE GUEST ACTUALLY SWAPS BUFFERS.
//
// lb_band_promote() is called from the FlushRequest branch alone -- the branch
// where the guest hands new geometry to the renderer. The else branch is
// RenderFrameIdentical, which re-renders the previous list unchanged. So this
// counter IS the guest's SwapBuffers rate, and differencing it per compositor
// frame is what separates the two explanations of docs/340:
//
//   * if the guest swaps on nearly every frame, then rcam freezing while the
//     ground moves means the port is LOSING those swaps, and the defect is
//     upstream of the band code entirely;
//   * if the guest swaps at roughly half rate, the cartridge updates its world
//     at 30 Hz and the port's error is that it moves the GROUND at 60 Hz --
//     which is the coupling func_020396E0 maintains and this port broke.
//
// The distinction decides which of the two fixes is right, and guessing it
// wrong would either hide a dropped-submission bug or lock the picture to a
// rate the cartridge never used.
extern "C" unsigned lb_band_promote_count(void) { return g_band_promotes; }

// The camera the geometry now queued for rendering was transformed against.
// Pair this with the render polygon list, never the live camera.
extern "C" int lb_band_render_cam(int* x, int* y) {
    if (x) *x = g_cam_render_x;
    if (y) *y = g_cam_render_y;
    return 1;
}

extern "C" int lb_band_render(void) { return g_band_render; }
extern "C" int lb_band_render_y(void) { return g_band_render_y; }
// docs/389: is a battle's camera placing the band this frame? The condition
// band_compute_at clamps under; without it (menus, title cards, the boot
// logos) there is no raster the native picture has to line up with.
extern "C" int lb_widemap_band_live(void) {
    return (g_geom_valid && g_cam_px_valid && g_map_w > 0) ? 1 : 0;
}

extern "C" int lb_widemap_map_size(int* w, int* h) {
    if (!g_geom_valid || g_map_w <= 0 || g_map_h <= 0) return 0;
    if (w) *w = g_map_w;
    if (h) *h = g_map_h;
    return 1;
}

// ONE FRAME NUMBER FOR EVERY DUMP.
//
// The composited frame and the whole-map render are written from different
// places -- the compositor in gpu2d.cpp and the battlefield panel in
// lb_overlay.cpp -- and each was numbering its files from its own counter.
// Those tick in different places, so "pair by nearest frame number" silently
// compared a composite against a camera rect it was never drawn with. That
// produced the retracted "RIGHT band 12-23%" and then, once a guard rejected
// the mismatched pairs, rejected EVERY row and left the audit with nothing to
// say. The counter cannot live in gpu2d.cpp's anonymous namespace because the
// panel could not link to it, so it lives here, where both already call in.
unsigned g_dump_frame = 0;
extern "C" unsigned lb_dump_frame_tick(void) { return ++g_dump_frame; }
extern "C" unsigned lb_dump_frame_get(void) { return g_dump_frame; }

// The camera in MAP METATILE coordinates, so a caller working in some other
// grid's cells can line the two spaces up rather than assuming they share an
// origin -- which is the mistake that made the first attempt at the map-viewer
// fix drop the control from 100.0% to 53.5%.
// The two cameras the drift probe has to compare, read TOGETHER.
//
// g_fill_cam_tx is the camera fill_terrain built the extension tables from --
// what actually indexes the fog and terrain in the widened columns. g_cam_tx is
// the live one. If they differ at the moment the compositor uses the tables,
// the band columns are showing a different instant from the guest's own 256,
// which is what "the fog drifts" describes.
//
// One call returning both, rather than two accessors, because this port has
// paired values from different moments three times and called the result
// evidence. Deliberately NOT gated on g_geom_valid: a probe that silently
// reports nothing when the geometry is stale would hide exactly the frames
// worth looking at.
extern "C" void lb_widemap_drift_cams(int* fill_tx, int* cam_tx) {
    if (fill_tx) *fill_tx = g_fill_cam_tx;
    if (cam_tx) *cam_tx = g_cam_tx;
}

extern "C" int lb_widemap_cam_metatile(int* mx, int* my) {
    if (!g_geom_valid) return 0;
    if (mx) *mx = floor_div(g_cam_tx, 3);
    if (my) *my = floor_div(g_cam_ty, 2);
    return 1;
}

// The camera in MAP TILE coordinates. The panel needs this to put its camera
// box in the same space as a map-space render; changing the render without
// changing the box is what broke the two earlier attempts.
extern "C" int lb_widemap_cam_tile(int* tx, int* ty) {
    if (!g_geom_valid) return 0;
    if (tx) *tx = g_cam_tx;
    if (ty) *ty = g_cam_ty;
    return 1;
}

// HOW FAR BACK THE CAMERA THE GROUND MATCHES CAME FROM.
//
// choose_camera tests each of the last four camera readings against the
// guest's OWN BG1 tilemap and takes the first with ZERO mismatches across 48
// sampled cells, so this index is not a proxy: it is the age, in frames, of
// the camera the terrain on screen was actually drawn with.
//
// That makes it the one number that can separate the two readings of docs/335.
// If it is 0, the ground is current and the geometry's two-frame lag is a real
// relative slide -- the jitter. If it is 2, the GROUND is lagging by the same
// amount and the two move together, so there is nothing to see and the lag I
// measured is an artifact of comparing geometry against a camera reading the
// picture never used.
//
// It was already computed every frame and printed only from a band-log line
// that does not fire in the current configuration -- measured and thrown away,
// which is the failure this project keeps rediscovering.
extern "C" int lb_widemap_cam_pick_age(void) { return g_cam_pick_age; }

// THE CAMERA IN PIXELS, for the one measurement that can settle whether the
// picture shears while it moves.
//
// A stationary unit's WORLD position is `surf_x + camx - band`: the band puts
// the guest's own columns at surf `band`, and the guest draws a thing at
// world x as screen x - camx. So for a unit that is not walking, that
// expression is a constant, frame after frame, however fast the camera pans.
// Any variation in it is the 2D and the 3D disagreeing about where the world
// is -- which is exactly what "while in motion sprites become jittery and
// disseheveled" looks like when it is written down as a number.
//
// Tile granularity cannot answer this: the whole effect is smaller than the
// 8 px lb_widemap_cam_tile rounds to.
extern "C" int lb_widemap_cam_px(int* x, int* y) {
    if (!g_geom_valid || !g_cam_px_valid) return 0;
    if (x) *x = g_cam_px_x;
    if (y) *y = g_cam_px_y;
    return 1;
}

// THE WORLD'S EXTENT IN PIXELS, for the camera clamp.
//
// Same conversions band_compute() uses and for the same reason: a metatile is
// 3 tiles across at 8 px and 2 down at 8 px, so the map is g_map_w*24 by
// g_map_h*16. Exported rather than recomputed at the call site, because a
// second copy of those two constants is a second place for them to be wrong.
extern "C" int lb_widemap_map_px(int* w, int* h) {
    if (!g_geom_valid || g_map_w <= 0 || g_map_h <= 0) return 0;
    if (w) *w = g_map_w * 24;
    if (h) *h = g_map_h * 16;
    return 1;
}

// The raw metatile INDEX the map's row-pointer table holds at (mx, my).
//
// Exported so the battlefield panel's grid can be lined up against the map by
// MEASUREMENT rather than by assumption. Two assumed relationships have already
// been falsified by the control, and comparing indices is the direct question:
// which offset, if any, makes the located grid agree with the map's own table.
extern "C" int lb_widemap_map_index(int mx, int my, uint16_t* out) {
    if (!g_geom_valid || !out) return 0;
    if (mx < 0 || my < 0 || mx >= g_map_w || my >= g_map_h) return 0;
    const uint32_t row = bus_read_u32(g_map + 0x34u + uint32_t(my) * 4u);
    if (!main_ram(row)) return 0;
    *out = bus_read_u16(row + uint32_t(mx) * 2u);
    return 1;
}

// ONE METATILE, THROUGH THE PATH THAT IS PROVEN.
//
// The battlefield panel's whole-map render had its own copy of this and got it
// wrong twice over. It read the grid as a FLAT array, grid_base + (cy*grid_w +
// cx)*2, while the real grid is a table of PER-ROW POINTERS at map + 0x34 +
// my*4 -- so its rows drift as soon as those are not contiguous, which is
// exactly the "right near the camera, wrong further out" the band audit saw.
// And it chose between the two metatile tables with a blank-tile heuristic
// instead of the split threshold that expand_metatile uses.
//
// This path is the one graded against the cartridge's own tilemap: 4700 band
// cells over two pans, zero disagreements. Anything that wants map terrain
// should come through here rather than re-deriving it.
extern "C" int lb_widemap_metatile(int mx, int my, uint16_t* out6) {
    if (!g_geom_valid || !out6) return 0;
    const Metatile m = metatile_tiles(g_map, g_split, g_table_a, g_table_b,
                                      mx, my, g_map_w, g_map_h);
    if (!m.ok) return 0;
    for (int i = 0; i < 6; ++i) out6[i] = m.t[i];
    return 1;
}

// The shroud entry for a widened column: 0x1F where the map-space record says
// the player has not seen this tile, 0 (transparent) where it has. Same column
// contract as above -- the guest's own 32 columns are never served from here,
// so the native picture is untouched.
extern "C" int lb_widemap_shroud(int htile, int trow, int gy, uint16_t* out) {
    ++g_ask3;
    if (!g_ext3_valid) return 0;
    if (guest_cell(htile, gy)) return 0;
    if (ext_out_of_range(htile, trow)) { ++g_declined_bounds; return 0; }
    // The model must describe the picture -- see g_model_ok. Blank, not
    // declined: a declined cell falls through to the hardware wrap.
    // HOLD THE LAST GOOD TABLE, DO NOT BLANK.
    //
    // Symptom: the screen flickered semi-frequently -- temporarily
    // transparent shroud, a yellow flash, other graphical flickering.
    //
    // choose_camera demands 0 of 48 sampled cells disagree, with no
    // tolerance. During a pan the guest's tilemap genuinely IS a mix of
    // two cameras -- the streamer rewrites columns as the camera crosses
    // a tile -- so no candidate matches all 48 and the model is rejected
    // for that frame. Blanking then paints the whole band tile 0 for one
    // frame: transparent on the fog layer (the terrain shows straight
    // through) and a flat solid tile on terrain (the flash).
    //
    // Declining is NOT the alternative -- the note above is right that a
    // declined cell falls through to the hardware wrap and draws garbage.
    // The third option is to serve what is already in the table, which is
    // the last fill made with a camera the model COULD explain. Slightly
    // stale for one frame beats a full-band flash, and `g_ext_valid`
    // above already guarantees the table has been filled at least once,
    // so there is no uninitialised case to fall into.
    //
    // LB_MODELBLANK=1 restores the blank for A/B.
    if (!g_model_ok) {
        static const bool blank = [] {
            const char* v = std::getenv("LB_MODELBLANK");
            return v && v[0] == '1';
        }();
        if (blank) { *out = 0u; return 1; }
    }
    *out = g_ext3[trow - kExtRowLo][htile - kExtLo];
    ++g_served3;
    return 1;
}

// FIND THE MAP-SPACE EXPLORED RECORD BY CORRELATION (LB_FINDFOG=1).
//
// The widened columns currently get their visibility from a cache accumulated
// out of the guest's own BG3, which is exact for ground the window has covered
// and conservatively SHROUDED for everything else. That is safe but visibly
// wrong: at the start of a battle the revealed area around the player's base
// extends past the 32-column window, so the fog forms a dead-straight vertical
// wall exactly at the native boundary while the fog inside the window follows
// the terrain organically.
//
// Fixing it needs the record the game itself keeps -- the one the minimap draws
// from, which must be map space because fog persists as ground is explored.
// Reading more disassembly has twice produced a confident wrong answer here
// (docs/202, and the BG3-is-the-fog detour), so this finds it by MEASUREMENT
// instead: take the visibility this file already knows to be true over the
// guest's own window, then scan all of main RAM for a buffer that agrees with
// it in map-space layout.
//
// A candidate must agree on BOTH classes. Matching only "revealed" is trivially
// satisfied by any zero-filled region, which is why the counts are kept apart
// and both are required to be substantial.
bool find_fog_record(uint32_t dispcnt, int map_w, int map_h,
                     int cam_tx, int cam_ty) {
    // THE POINTER CHAIN FIRST. The record's address is known from the decomp
    // (see locate_fog_record), so the correlation search below is only a
    // fallback for a build where the chain does not validate -- a different
    // region, or a frame caught between battles.
    //
    // This is not a shortcut past the agreement check. The located record is
    // graded every frame by exactly the same comparison against the guest's
    // own fog, and dropped on the same streak, so adopting it here claims
    // nothing that is not then tested. What it removes is the SEARCH, which
    // graded candidates against g_seen -- the approximation being replaced --
    // and so kept adopting buffers that shared the proxy's blind spots.
    //
    // LB_FOGLOC=0 forces the old search, so the two can be A/B'd against each
    // other on one route.
    if (!g_vis_base) {
        const char* off = std::getenv("LB_FOGLOC");
        if (!(off && off[0] == '0') && locate_fog_record(map_w, map_h)) {
            std::fprintf(stderr,
                         "[lb_findfog] LOCATED %ux%u bits, rows at %08X, "
                         "stride %u -- from the pointer chain, not a search\n",
                         unsigned(g_vis_w), unsigned(g_vis_h), g_vis_rowtab,
                         unsigned(g_vis_stride));
            std::fflush(stderr);
            return true;
        }
    }

    BusRegion ram{};
    if (!bus_get_region("mainram", &ram) || !ram.ptr) {
        std::fprintf(stderr, "[lb_findfog] no mainram region\n");
        return true;
    }

    // Ground truth, at METATILE granularity: a metatile counts as revealed
    // only if every one of its tiles that we can see is revealed, so a partly
    // covered metatile is never claimed either way.
    const uint32_t block = ((dispcnt >> 27) & 7u) * 0x10000u;
    const uint32_t bg3cnt = nds_io_debug_read(9, 0x0400000Eu, 16);
    const uint32_t base3 = block + ((bg3cnt >> 8) & 0x1Fu) * 0x800u;

    struct Sample { int mx, my; bool revealed; };
    std::vector<Sample> samples;
    for (int my = (cam_ty + 1) / 2; my <= (cam_ty + 23) / 2; ++my) {
        for (int mx = (cam_tx + 2) / 3; mx <= (cam_tx + 31) / 3; ++mx) {
            if (mx < 0 || my < 0 || mx >= map_w || my >= map_h) continue;
            int seen = 0, shroud = 0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 3; ++sx) {
                    const int ty = my * 2 + sy - cam_ty;
                    const int tx = mx * 3 + sx - cam_tx;
                    if (tx < 0 || tx >= 32 || ty < 0 || ty >= 24) continue;
                    // GROUND TRUTH COMES FROM g_seen, NOT FROM BG3.
                    //
                    // This used to re-derive visibility from BG3 == 0x1F, the
                    // indicator already proved saturated: it reports every
                    // visible tile shrouded, so every attempt collected
                    // "120 samples (0 revealed, 120 shrouded)" and the search
                    // could never run for want of a second class. g_seen is the
                    // corrected reading -- BG2 equality with the learned fog
                    // tile for the phase -- and is exactly the truth to
                    // correlate against.
                    const int ar = cam_ty + ty, ac = cam_tx + tx;
                    if (ar < 0 || ar >= g_seen_h || ac < 0 || ac >= g_seen_w)
                        continue;
                    ++seen;
                    if (g_seen[size_t(ar) * size_t(g_seen_w) + size_t(ac)])
                        ++shroud;
                }
            }
            // MAJORITY, NOT UNANIMITY.
            //
            // This used to accept only metatiles whose six tiles agreed, and
            // that filter is what let the wrong buffer through. Near the
            // player's base a unanimous metatile is either fully explored AND
            // currently occupied, or wholly unexplored -- precisely the cells
            // where "explored" and "currently visible" coincide. The search
            // therefore could not tell those two records apart, adopted a
            // current-visibility grid, and was rejected by the frame-by-frame
            // check moments later (docs/206).
            //
            // Taking the majority instead includes the cells that separate
            // them: ground the player has explored but has no unit near. A
            // metatile straddling a fog edge now contributes a slightly noisy
            // label, which is why the match below tolerates a few misses
            // rather than demanding perfection.
            if (seen < 6) continue;                 // partly off-window
            samples.push_back({mx, my, shroud * 2 <= seen});
        }
    }
    int n_rev = 0, n_shr = 0;
    for (const Sample& s : samples) (s.revealed ? n_rev : n_shr)++;
    std::fprintf(stderr,
                 "[lb_findfog] %u samples (%d revealed, %d shrouded) "
                 "over map %dx%d\n",
                 unsigned(samples.size()), n_rev, n_shr, map_w, map_h);
    // A BALANCED VIEW, not merely a mixed one.
    //
    // "At least 8 of each" is far too weak. A panned view came back 107
    // revealed against 9 shrouded, and on that evidence the search happily
    // matched a completely different family of addresses -- nine samples cannot
    // distinguish a real visibility record from any buffer that is mostly
    // non-zero. Balanced views are what found the same address run after run.
    //
    // So wait for one. This costs nothing: the caller retries, and a view with
    // a real fog edge across it comes along quickly.
    // A FIFTH OF EACH CLASS WAS REJECTING THE ONLY USEFUL VIEWS.
    //
    // The rule exists for a good reason -- docs/206 records a 107-revealed
    // against 9-shrouded view matching "a completely different family of
    // addresses", because nine samples of a class cannot distinguish a
    // visibility record from any buffer that is mostly non-zero -- and a fifth
    // was the number picked to stop it.
    //
    // It stops too much. Measured over a pan that crosses a fog frontier, the
    // views this route actually produces are:
    //
    //     120 rev /   0 shr  x470     no fog on screen, correctly refused
    //      18 rev / 102 shr  x21      REFUSED: 18 < 24
    //      16 rev / 104 shr  x2       REFUSED: 16 < 24
    //      95 rev /  25 shr  x6       accepted
    //      93 rev /  27 shr  x1       accepted
    //
    // So the search only ever ran on revealed-heavy views -- the ones docs/206
    // says match an unrelated family -- and refused every shroud-heavy one by
    // as little as six samples. The buffer it then adopted, run after run, was
    // that family: 023153F7 and its neighbours at +0x10, +0x20, +0x30, all
    // disagreeing 37/132 with every single disagreement in the direction that
    // opens hidden ground. docs/206's SUCCESSFUL case was 38/82 -- shroud
    // heavy, which is what a view straddling a real frontier looks like.
    //
    // TRIED AT AN EIGHTH, AND REVERTED. Recorded rather than deleted, because
    // the result is the most informative thing this search has produced.
    //
    // An eighth admits those 18-of-120 views, and with them the search finds a
    // COMPLETELY DIFFERENT family -- 0213C843 and its neighbours at +1, +2, +3,
    // low and near the static globals, exactly where docs/206 says a persistent
    // per-map record belongs, rather than the 0227/0231 heap addresses every
    // previous run landed on. It is also not dropped, where the heap ones were
    // dropped within seconds. Band fog fell from 99.1% to 76.8%.
    //
    // And it is still the wrong buffer, by its own numbers:
    //
    //     agrees with the guest's own fog   81.8%   (the good one: 96.2%)
    //     shrinks                           1       (an explored record cannot)
    //     fog audit                         56 tile disagreements of 768,
    //                                       against 2-3 on the conservative
    //                                       path, and in the direction
    //                                       "ours 064 clear, theirs 0C0 fog"
    //
    // That last line is the decider. 56 of 768 is the port's model calling
    // ground clear that the cartridge is hiding -- the one thing the widening
    // may never do -- and the policing check does not drop it, because 24 of
    // 132 wrong is under the one-in-four bar. So an eighth trades a bad buffer
    // that gets DROPPED for a bad buffer that gets KEPT, which is worse.
    //
    // A fifth stands until there is a discriminator that does not need a
    // threshold fitted to single runs. What the experiment established, and
    // what is worth keeping: the right record is findable from shroud-heavy
    // views and lives low, and the sampling gate is what decides which family
    // the search can even see.
    const int need = int(samples.size()) / 5;
    if (n_rev < 8 || n_shr < 8 || n_rev < need || n_shr < need) return false;
    // A VIEW THIS SEARCH COULD ACTUALLY USE. Counted so the caller can give up
    // on failed SEARCHES rather than on elapsed frames -- see the note there.
    ++g_findfog_views;

    // One byte per metatile is the layout to try first: the minimap is about
    // one pixel per metatile, and a bit-packed record would need the stride to
    // be guessed as well. Both row strides worth trying are covered by letting
    // the stride vary independently of map_w.
    // ONE SAMPLE SET IS NOT PROOF.
    //
    // 4 MB of RAM against ~100 yes/no samples will throw up coincidences, and
    // the first search did return four candidates at once. So once candidates
    // exist, later frames RE-VERIFY them against a fresh sample set from a
    // different camera position and prune whatever no longer agrees. A buffer
    // that tracks visibility across several independent views is the record;
    // a buffer that happened to match one view is not.
    if (g_cand_n > 0) {
        int kept = 0;
        for (int i = 0; i < g_cand_n; ++i) {
            // Same tolerance as the search, for the same reason: metatile
            // majorities over per-tile truth are slightly noisy at fog edges.
            bool ok = true;
            // COUNT EVERY MISS, do not stop at the budget.
            //
            // The survivors of a search are not independent buffers: they come
            // out as CONSECUTIVE BYTES -- 0213C843, 0213C844, 0213C845,
            // 0213C846 in one run -- because a byte-per-metatile grid with a
            // 64-wide stride still matches tolerably when the base is off by a
            // byte or three. They are one buffer at four alignments, and only
            // one of those alignments is right.
            //
            // Picking the wrong one shifts the whole grid by one to three
            // METATILE COLUMNS, and that is very likely the artefact docs/206
            // parked this feature for: "the widened band breaks into vertical
            // stripes of alternating terrain and fog roughly a metatile wide".
            // A stripe a metatile wide is what a column-shifted grid looks
            // like.
            //
            // So the miss count has to be comparable between candidates, which
            // means counting all of them rather than bailing out the moment
            // the budget is passed. The budget still decides survival; the
            // total decides which survivor is the real alignment.
            int vmiss = 0;
            const int vbudget = int(samples.size()) / 20;
            for (const Sample& s : samples) {
                const uint32_t off = g_cand[i].base - 0x02000000u +
                                     uint32_t(s.my) * g_cand[i].stride +
                                     uint32_t(s.mx);
                if (off >= ram.len) { ok = false; vmiss = 1 << 20; break; }
                const bool nonzero = ram.ptr[off] != 0u;
                if (nonzero != s.revealed) ++vmiss;
            }
            if (vmiss > vbudget) ok = false;
            g_cand[i].miss = vmiss;
            // MONOTONICITY, which is what tells the two kinds of record apart.
            //
            // Agreeing with the guest's own fog on a view is necessary and is
            // NOT sufficient, and the comment at the adoption site below used
            // to claim it was ("Any survivor will do ... the only property this
            // needs"). It is not: near the player's base, ground that is
            // explored and ground that is currently in view are the same cells,
            // so a CURRENTLY-VISIBLE buffer matches every sample just as well
            // as an EXPLORED one. The port then adopted whichever came first
            // and paid for it at the next drop.
            //
            // The two differ in how they change. This game never re-shrouds:
            // ground once explored stays drawn, so its record only ever grows.
            // A currently-visible record shrinks the moment a unit walks away.
            // So a candidate whose whole-map revealed count FALLS between
            // passes is the wrong kind, whatever it agreed with.
            //
            // Cheap: one pass over the map per surviving candidate, only on
            // frames that produced a balanced view, and only until adoption.
            if (ok) {
                uint32_t rev = 0;
                for (int my = 0; my < map_h; ++my) {
                    for (int mx = 0; mx < map_w; ++mx) {
                        const uint32_t off = g_cand[i].base - 0x02000000u +
                                             uint32_t(my) * g_cand[i].stride +
                                             uint32_t(mx);
                        if (off < ram.len && ram.ptr[off]) ++rev;
                    }
                }
                if (rev + 1u < g_cand[i].last_rev) {
                    std::fprintf(stderr,
                                 "[lb_findfog] %08X revealed FELL %u -> %u: "
                                 "a currently-visible record, not an explored "
                                 "one\n",
                                 g_cand[i].base, g_cand[i].last_rev, rev);
                    ok = false;
                } else {
                    g_cand[i].last_rev = rev;
                }
            }
            if (ok) g_cand[kept++] = g_cand[i];
        }
        const int dropped = g_cand_n - kept;
        g_cand_n = kept;
        std::fprintf(stderr,
                     "[lb_findfog] verify pass: %d survive, %d dropped "
                     "(%d rev / %d shr this view)\n",
                     kept, dropped, n_rev, n_shr);
        for (int i = 0; i < kept; ++i)
            // .base, NOT the struct. Passing a Cand to a varargs %08X is
            // undefined and printed nonsense: the last run reported
            // "survivor 72BBC950", which is not a main-RAM address at all --
            // and this is the line anyone reads to find out WHICH buffer the
            // search is about to adopt.
            std::fprintf(stderr,
                         "[lb_findfog]   survivor %08X stride %u miss %d/%u\n",
                         g_cand[i].base, unsigned(g_cand[i].stride),
                         g_cand[i].miss, unsigned(samples.size()));
        // WHAT EACH SURVIVOR SAYS OUTSIDE THE GUEST'S WINDOW.
        //
        // Every survivor agrees with the guest's own fog on the sampled views,
        // and the code adopted the first one on the grounds that this is "the
        // only property this needs". It is not. The band draws cells the guest
        // window does NOT cover, and agreement inside the window says nothing
        // about what a buffer holds outside it -- a buffer the game only fills
        // for the visible area reads as whatever was left there.
        //
        // Measured symptom that sent me here: fog coverage falls from 32.7% on
        // the last guest row to 0.0% on the first band row, in the same frame,
        // top and bottom alike. The band is drawing no fog at all, because the
        // adopted record calls every off-window cell revealed.
        //
        // So print, per survivor, the revealed fraction INSIDE the window and
        // OUTSIDE it. A real map-wide record early in a match is mostly
        // unexplored outside; a stale or view-only buffer reads revealed
        // almost everywhere.
        for (int i = 0; i < kept; ++i) {
            unsigned in_rev = 0, in_tot = 0, out_rev = 0, out_tot = 0;
            const int wx0 = floor_div(g_cam_tx, 3), wx1 = floor_div(g_cam_tx + 31, 3);
            const int wy0 = floor_div(g_cam_ty, 2), wy1 = floor_div(g_cam_ty + 23, 2);
            for (int my = 0; my < g_map_h; ++my) {
                for (int mx = 0; mx < g_map_w; ++mx) {
                    const bool rev = bus_read_u8(
                        g_cand[i].base + uint32_t(my) * g_cand[i].stride +
                        uint32_t(mx)) != 0u;
                    const bool inside = (mx >= wx0 && mx <= wx1 &&
                                         my >= wy0 && my <= wy1);
                    if (inside) { ++in_tot; in_rev += rev ? 1u : 0u; }
                    else        { ++out_tot; out_rev += rev ? 1u : 0u; }
                }
            }
            std::fprintf(stderr,
                         "[lb_visprobe] %08X stride %u: revealed %u/%u inside "
                         "the window (%.0f%%), %u/%u outside (%.0f%%)\n",
                         g_cand[i].base, unsigned(g_cand[i].stride),
                         in_rev, in_tot,
                         in_tot ? 100.0 * in_rev / in_tot : 0.0,
                         out_rev, out_tot,
                         out_tot ? 100.0 * out_rev / out_tot : 0.0);
        }
        // FIND THE POINTER, not just the buffer.
        //
        // The surviving address is heap-allocated and different every run
        // (0230C033 one run, 0230BFFB the next), so the address itself is
        // useless. What is reusable is whatever holds it: a fixed global, or a
        // field in the map or view struct. Scan for a word pointing at the
        // buffer -- and at the few bytes before it, since the byte that
        // correlates may be part-way into a wider entry rather than its start.
        if (kept >= 1 && kept <= 24) {
            std::fprintf(stderr,
                         "[lb_findfog] converged on %08X stride %u; "
                         "looking for what points at it\n",
                         g_cand[0].base, unsigned(g_cand[0].stride));
            int found = 0;
            for (uint32_t off = 0; off + 4 <= ram.len && found < 16; off += 4) {
                uint32_t v;
                std::memcpy(&v, ram.ptr + off, 4);
                const int32_t d = int32_t(v) - int32_t(g_cand[0].base);
                if (d > -16 && d <= 0) {
                    std::fprintf(stderr,
                                 "[lb_findfog]   %08X holds %08X (buffer%+d)\n",
                                 0x02000000u + off, v, d);
                    ++found;
                }
            }
            if (!found)
                std::fprintf(stderr, "[lb_findfog]   nothing points at it\n");
            std::fflush(stderr);
        }
        // ADOPT, once a candidate has held up across several distinct views.
        //
        // "ANY SURVIVOR WILL DO ... THE ONLY PROPERTY THIS NEEDS" IS RETRACTED.
        // That is what this comment used to say, and the runs disprove it: the
        // adopted grid was dropped again and again, and the directional counter
        // in the policing check named the reason -- 021B632E disagreeing 37/132
        // with 36 of the 37 calling ground revealed that the cartridge was
        // hiding. A buffer can agree with every sampled view and still be the
        // wrong KIND of record, because near the player's base "explored" and
        // "currently visible" are the same cells.
        //
        // Agreement is now necessary rather than sufficient: a survivor must
        // also never have shrunk (see the monotonicity test above) and must
        // have survived more views than the three that were not enough.
        //
        // Adopting a discovered address rather than a hard-coded one is what
        // makes this safe to leave on. 0227DEB0 came back at the same address
        // in every run, but it sits well above the game's static data (the map
        // pointer is at 02156CB0), so it is heap, and it is only stable because
        // this route allocates identically every time. A different map or a
        // different sequence of play would move it.
        // THE LOWEST SURVIVING ADDRESS, not the first one found.
        //
        // docs/206 records which buffer is the right one and where it lives:
        // 0215E6ED, "just above the game's static globals -- the map pointer is
        // at 02156CB0 -- where a persistent per-map record would be expected,
        // unlike 0227DEB0 which was far up in the heap". The wrong one there
        // was current-visibility rather than explored.
        //
        // Every candidate this port has adopted and then dropped has been high
        // heap: 0227C66B, 021B632E, 02314FCF, 0227C3F9, 023153F7 -- the last of
        // which disagreed 37/132 with ALL 37 calling ground revealed that the
        // cartridge was hiding. Not one was in the 0215 region.
        //
        // So order by address. A record the game keeps for the whole battle is
        // allocated once, early, near its static data; a view-scoped or
        // per-frame buffer is handed out wherever the heap happens to be. That
        // is a property of what the buffer IS, which is exactly the kind of
        // discriminator agreement alone cannot supply -- and it costs a sort of
        // at most 24 entries.
        // FEWEST MISSES FIRST, then the lowest address.
        //
        // Address order alone is arbitrary between alignments of the SAME
        // buffer, which is what consecutive survivors are, and being one byte
        // out shifts the grid by a metatile column. Agreement is what
        // distinguishes them, so rank on it and keep the address as the
        // tie-break -- that is where the docs/206 argument still applies,
        // between genuinely different buffers.
        int pick = -1;
        for (int i = 0; i < kept; ++i) {
            bool bad = false;
            for (int j = 0; j < g_vis_bad_n; ++j)
                if (g_vis_bad[j] == g_cand[i].base) bad = true;
            if (bad) continue;
            if (pick < 0 || g_cand[i].miss < g_cand[pick].miss ||
                (g_cand[i].miss == g_cand[pick].miss &&
                 g_cand[i].base < g_cand[pick].base))
                pick = i;
        }
        // SIX, not three. Three was the number in use while the monotonicity
        // test did not exist, and every candidate it adopted was later dropped.
        // The test needs passes to work with -- a record cannot be shown not to
        // shrink until it has been watched over several views -- so the two
        // changes go together.
        if (pick >= 0 && ++g_vis_passes >= 6 && !g_vis_base) {
            g_vis_base = g_cand[pick].base;
            g_vis_stride = g_cand[pick].stride;
            std::fprintf(stderr,
                         "[lb_findfog] ADOPTED %08X stride %u after %u views\n",
                         g_vis_base, unsigned(g_vis_stride),
                         unsigned(g_vis_passes));
        }
        std::fflush(stderr);
        return g_vis_base != 0u;
    }

    int hits = 0;
    for (uint32_t stride : {uint32_t(map_w), uint32_t(64), uint32_t(128)}) {
        const uint32_t span = stride * uint32_t(map_h);
        if (span == 0 || span > ram.len) continue;
        for (uint32_t b = 0; b + span <= ram.len && hits < 12; ++b) {
            // Tolerate a few misses. The labels are now metatile majorities
            // over per-tile truth, so a metatile straddling a fog edge can
            // legitimately disagree with any per-metatile record. Demanding
            // unanimity here would reject the very record being looked for.
            // The budget stays tight: one in twenty, and both classes must
            // still be well represented, so it cannot be satisfied by a
            // zero-filled region or a constant one.
            int ok_rev = 0, ok_shr = 0, miss = 0;
            const int miss_budget = int(samples.size()) / 20;
            bool fail = false;
            for (const Sample& s : samples) {
                const uint8_t v =
                    ram.ptr[b + uint32_t(s.my) * stride + uint32_t(s.mx)];
                const bool says_revealed = v != 0u;
                if (says_revealed == s.revealed) {
                    if (s.revealed) ++ok_rev; else ++ok_shr;
                } else if (++miss > miss_budget) {
                    fail = true;
                    break;
                }
            }
            if (fail || ok_rev < 8 || ok_shr < 8) continue;
            std::fprintf(stderr,
                         "[lb_findfog] MATCH base %08X stride %u "
                         "(nonzero=revealed) %d/%d\n",
                         0x02000000u + b, unsigned(stride), ok_rev, ok_shr);
            if (g_cand_n < 24)
                g_cand[g_cand_n++] = Cand{0x02000000u + b, stride, 0u, 0};
            ++hits;
        }
    }
    if (!hits)
        std::fprintf(stderr, "[lb_findfog] no byte-per-metatile buffer "
                             "matches; try a packed or inverted layout\n");
    // Finding candidates is not finishing. Keep running so later views can
    // prune them; only "nothing matched at all" ends the search here.
    return hits == 0;
}

extern "C" void lb_widemap_fill(void) {
    // THE COUNTERS, PRINTED UNCONDITIONALLY, because the previous probe printed
    // them only when band_compute() took its fallback -- so it went silent at
    // exactly the moment the battle started, and the frozen boot-phase numbers
    // were read as if they described gameplay. `fill ok=0 bail2=1711` was three
    // times reported as "the fill never completes" when all 1711 bails happened
    // BEFORE the map was located, where bailing is correct. The [lb_locate]
    // line proving it sat NINE LINES BELOW the last counter line in the same
    // log.
    {
        static int on = -1;
        if (on < 0) {
            const char* v = std::getenv("LB_BANDWHY");
            on = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (on) {
            static unsigned n = 0;
            if ((n++ % 120u) == 0u) {
                std::fprintf(stderr,
                             "[lb_fill] entries=%u ok=%u bail1=%u bail2=%u "
                             "bail3=%u bail4=%u bail5=%u geom=%d map_w=%d\n",
                             n, g_fill_bail[0], g_fill_bail[1], g_fill_bail[2],
                             g_fill_bail[3], g_fill_bail[4], g_fill_bail[5],
                             g_geom_valid ? 1 : 0, g_map_w);
                std::fflush(stderr);
            }
        }
    }
    // CLEARED FIRST, SET LAST, so it means "the field geometry was readable at
    // THIS vblank" and nothing weaker.
    //
    // g_geom_valid is not that: none of the early returns below clear it, so it
    // stays true from the last battle all the way through the menus. That is
    // harmless for the tile suppliers, which are only consulted while the field
    // is on screen, and would NOT be harmless for the band clamp -- a menu is
    // not a map, and clamping its background to a stale battle's camera would
    // slide the title card sideways.
    g_cam_px_was_valid = g_cam_px_valid;
    g_cam_px_valid = false;
    // ENHANCED IS THE TOGGLE. Vanilla means the cartridge untouched, and
    // that includes not supplying tiles for columns it never draws.
    if (stage() <= 0 || !lb_setting_enhanced()) { ++g_fill_bail[1]; return; }

    // ONLY IN THE FIELD SCENE.
    //
    // Without this the code also ran through the menus, where the view and map
    // pointers are stale but still look like main-RAM addresses -- and the
    // emulator died partway through the route rather than misdrawing anything.
    // The field is display mode 1 with BG0 as the 3D layer and BG1 enabled;
    // measured DISPCNT 00010F08 (docs/193).
    // THE BG ENABLE BITS ARE NOT A BATTLE TEST, and requiring them stopped
    // this function completing at all on the level under test.
    //
    // Measured there: band_compute's span/2 fallback reported
    // `geom=0 cam_px=0 map_w=0` for the whole run -- g_geom_valid and g_map_w
    // are only set at the END of this function, so it had never once run
    // through. The band therefore always took the fallback, which is span/2
    // (192 on a 640 surface) against a clamped answer of 188: a 4 px step,
    // alternating with the frames that reached the clamp by another path.
    //
    // It also made the earlier "hold the camera across a short absence" fix
    // dead on arrival: it restores g_cam_px_was_valid, and that can never have
    // been true if the full path never ran. A bootstrap that cannot bootstrap.
    //
    // The bail values say why: `bailA=00010D0B`, whose BG-enable nibble is 0x0D
    // -- BG1 OFF -- and engine B's `bailB=00211C13` has no BG1 either. The game
    // toggles those bits; they do not mean "not in a battle".
    //
    // What does mean it is the MAP. A menu has no map object with a plausible
    // grid in it, and this is the same chain func_020A3478 follows to expand a
    // metatile, so it is the game's own definition of "a level is loaded"
    // rather than a proxy for it.
    const uint32_t dispcnt = nds_io_debug_read(9, 0x04000000u, 32);
    // RESTORED TO THE ORIGINAL TEST, and the detour is worth recording.
    //
    // This was rewritten to consult a cached map chain instead, on the strength
    // of `fill ok=0 bail2=1711` -- read three times as "the fill never
    // completes". It was measured wrong: the counters were only printed when
    // band_compute() took its fallback, so the probe went SILENT the moment the
    // battle started and the frozen boot-phase numbers were read as if they
    // described gameplay. Printed unconditionally, the truth is:
    //
    //     entries=4201 ok=2478 bail1=0 bail2=1720 bail3=0 bail4=0 bail5=2
    //     geom=1 map_w=64
    //
    // The fill completes 2478 times and bail2 stops at 1720, all of them before
    // the map is located, where bailing is correct. Nothing was broken here.
    //
    // The replacement was also a latent REGRESSION: a cached chain persists
    // after a battle ends, so the gate would pass on menu screens and let a
    // stale battle's bands draw over them -- which is the exact case the
    // original test exists to prevent.
    if (((dispcnt >> 16) & 3u) != 1u ||
        !(dispcnt & 0x8u) || !(dispcnt & 0x100u) || !(dispcnt & 0x200u)) {
        // WHAT THE GATE ITSELF SAW, on the gate's own thread.
        //
        // The previous attempt printed this from band_compute(), which runs in
        // the fanned-out row body, and it reported w=0 / row0=31111111 for the
        // very map object lb_field reads correctly as 64x64. Same address, same
        // frame, different values -- because it was a different thread. Those
        // numbers said nothing about this gate, and nothing was changed on
        // their strength.
        {
            static int on = -1;
            if (on < 0) {
                const char* v = std::getenv("LB_BANDWHY");
                on = (v && v[0] && v[0] != '0') ? 1 : 0;
            }
            if (on) {
                static unsigned n = 0;
                if ((n++ % 60u) == 0u) {
                    const uint32_t m = bus_read_u32(kMapPtr);
                    uint32_t cm = 0; int cw = 0, ch = 0;
                    const int chain = lb_field_map_chain(&cm, &cw, &ch);
                    std::fprintf(stderr,
                                 "[lb_gate] chain=%d cm=%08X cw=%d ch=%d\n",
                                 chain, cm, cw, ch);
                    std::fprintf(stderr,
                                 "[lb_gate] tid=%u dispcnt=%08X mode=%u map=%08X "
                                 "w=%d h=%d t338=%08X row0=%08X\n",
                                 (unsigned)(size_t)std::hash<std::thread::id>{}(
                                     std::this_thread::get_id()),
                                 dispcnt, (dispcnt >> 16) & 3u, m,
                                 main_ram(m) ? (int)bus_read_u8(m + 0x14u) : -1,
                                 main_ram(m) ? (int)bus_read_u8(m + 0x15u) : -1,
                                 main_ram(m) ? bus_read_u32(m + 0x338u) : 0u,
                                 main_ram(m) ? bus_read_u32(m + 0x34u) : 0u);
                    std::fflush(stderr);
                }
            }
        }
        g_ext_valid = false;
        g_ext3_valid = false;
        ++g_fill_bail[2];
        // A ONE-FRAME LAYER BLIP IS NOT A SCENE CHANGE.
        //
        // This test decides whether to build the extension tables, and for that
        // it is exactly right. It was ALSO deciding, by omission, whether the
        // camera is known -- g_cam_px_valid is cleared at the top of this
        // function and set only at the end -- and that is wrong, because the
        // camera is no less known on a frame where a BG layer happens to be off.
        //
        // Measured: of 6726 fills at one wide surface, 3604 left through
        // here and 3122 ran through, roughly alternating, and DISPCNT was
        // alternating between 00010F08 and 00010408. 0x408 has neither BG0 nor
        // BG1 enabled, so every one of those frames failed the test. The band
        // then fell back to the centred split and the picture moved 4 px:
        // left=721 on the geom=1 frames, left=725 on the geom=0 ones. That is
        // the shake left over after the camera clamp.
        //
        // So hold the camera across a short absence and drop it on a sustained
        // one. The menu case the guard exists for is a SUSTAINED absence -- a
        // title card does not flicker back into display mode 1 every other
        // frame -- so it still invalidates, which is what keeps a stale
        // battle's camera from sliding a menu background sideways.
        //
        // Half a second at 60 Hz. Long enough for any per-frame blip, far
        // shorter than the briefest trip through a menu.
        //
        // AND IT DOES NOT FIX THE 4 PX STEP, measured after adding it: still
        // 14 frames at left=721 geom=1 and 13 at left=725 geom=0, unchanged.
        // The tolerance is written for a blip -- one bad frame between good
        // ones -- and the counts say that is not the shape of it: 3604 bails
        // against 3122 runs-through is consistent with long RUNS of each, and
        // a run past 30 drops validity exactly as before. The band log only
        // prints on CHANGE, so 13 transitions were read as 13 alternations
        // when they are equally 13 runs; that reading was the mistake.
        //
        // Kept because tolerating a genuine blip is right on its own terms and
        // costs nothing. What it is NOT is the fix, and the next measurement is
        // the DISPCNT value itself on this path plus the run lengths -- the
        // suspicion being that the field is on the other engine for part of the
        // time and 0x04000000 is engine A's register.
        g_dispcnt_bail_a = dispcnt;
        g_dispcnt_bail_b = nds_io_debug_read(9, 0x04001000u, 32);
        if (++g_dispcnt_miss > g_dispcnt_run_max)
            g_dispcnt_run_max = g_dispcnt_miss;
        // THE CAMERA IS KNOWN EVEN WHEN A BG LAYER IS OFF, and the 30-frame
        // tolerance above was the right idea aimed at the wrong shape.
        //
        // The measurement this note asked for came back: on the bail path
        // `bailA=00010D0B`, whose BG-enable bits are 0x0D -- BG1 is OFF -- and
        // engine B's `bailB=00211C13` has no BG1 either. So BG1 really is
        // disabled on those frames; the game toggles it, and the runs are long,
        // which is why tolerating 30 frames changed nothing.
        //
        // Using BG-enable bits to answer "are we still in a battle" cannot
        // work, because that is not what they mean. The VIEW OBJECT answers it:
        // it is what holds the camera (camx/camy at +0x5E/+0x60), it is
        // allocated per battle, and a menu does not have one with a plausible
        // camera in it. So hold the camera while the view still reads as a
        // view, and drop it when it does not.
        //
        // This matters far beyond a 4 px step. lb_field_camera_rect() falls
        // back to TILE granularity when the pixel camera is unavailable, so
        // every bail frame moved the host field in 8 px jumps while the guest's
        // own columns slid smoothly on their sub-tile fine scroll. Measured
        // before this change, over 200 consecutive frames, 14 of the 16 frames
        // in which anything moved had the two regions disagreeing.
        {
            const uint32_t vw = bus_read_u32(kViewPtr);
            bool view_ok = false;
            if (main_ram(vw)) {
                const int16_t vx = read_s16(vw + 0x5Eu);
                const int16_t vy = read_s16(vw + 0x60u);
                view_ok = vx > -256 && vx < 8192 && vy > -256 && vy < 8192;
            }
            if (view_ok) {
                // REFRESH THE VALUE, not just the validity flag. Restoring the
                // flag alone left g_cam_px_x holding the position from the last
                // frame that ran all the way through, so the host field stood
                // still on every bail frame while the guest's own columns
                // advanced.
                //
                // Measured before this, over the panning stretch of a 200-frame
                // recording, consecutive-frame change per region:
                //
                //     native  16.4   4.1  11.3   2.7  17.9   4.7  16.4
                //     band     0.0  20.6   2.0  14.3   1.5  22.6   0.0
                //
                // Exactly ANTIPHASE, and a band delta of 0.0 means those pixels
                // were byte-identical: the bands were updating every other
                // frame, so half the picture was always one frame stale. That
                // is the reported shake, and it is why the seam looked clean --
                // the stale half is self-similar cloud, which hides an offset.
                //
                // This is the same read the full path does 120 lines below
                // (read_s16(view + 0x5E)), so there is no second source of
                // truth to drift.
                g_cam_px_x = read_s16(vw + 0x5Eu);
                g_cam_px_y = read_s16(vw + 0x60u);
                g_cam_px_valid = g_cam_px_was_valid;
            }
        }
        return;
    }
    if (g_dispcnt_miss) g_dispcnt_ok_run = 0u;
    ++g_dispcnt_ok_run;
    g_dispcnt_miss = 0u;

    const uint32_t view = bus_read_u32(kViewPtr);
    const uint32_t map = bus_read_u32(kMapPtr);
    if (!main_ram(view) || !main_ram(map)) { g_ext_valid = false; g_ext3_valid = false; ++g_fill_bail[3]; return; }

    // A battle map is 64x64 metatiles here; anything wildly outside that is a
    // stale pointer being read as a map.
    const int map_w = bus_read_u8(map + 0x14u);
    const int map_h = bus_read_u8(map + 0x15u);
    if (map_w < 8 || map_h < 8 || map_w > 200 || map_h > 200) {
        g_ext_valid = false;
        g_ext3_valid = false;
        ++g_fill_bail[4];
        return;
    }

    const uint32_t table_a = bus_read_u32(map + 0x338u);
    const uint32_t table_b = bus_read_u32(map + 0x33Cu);
    const uint32_t split = bus_read_u16(kTileSplit + 2u);
    if (!main_ram(table_a) || !main_ram(table_b)) {
        g_ext_valid = false;
        g_ext3_valid = false;
        ++g_fill_bail[5];
        return;
    }

    // The guest's own alignment (func_020A3234): camera tile x, and camera tile
    // y forced even because a metatile is two tile rows tall.
    const int cam_px_x = read_s16(view + 0x5Eu);
    const int cam_px_y = read_s16(view + 0x60u);
    const int cam_tx = floor_div(cam_px_x, 8);
    const int cam_ty = floor_div(cam_px_y, 8) & ~1;


    // The tables the render-time camera check needs, published for it.
    g_split = split;
    g_table_a = table_a;
    g_table_b = table_b;

    // SEED THE FOG TILES FROM THE MAP, instead of waiting to learn them.
    //
    // THE COLD START IS THE "slow load time for the area outside the original
    // aspect ratio upon loading into a new match".
    //
    // The six tiles of the fog metatile were learned by watching the guest
    // draw them: a per-phase vote over many frames, committed only once all
    // six phases have a candidate and the six are distinct. Until that
    // commits, a widened cell the record calls shrouded is drawn BLANK, so on
    // entering a battle the band outside the native aspect fills in slowly as
    // the votes accumulate.
    //
    // None of it was necessary. The note at kShroudMetatile already says where
    // the tiles come from: func_0203A4B4 writes metatile index 100 for every
    // tile the visibility test flags, and the shroud is expanded through the
    // same 3x2 machinery as the terrain. expand_metatile was even split out of
    // metatile_tiles for this -- "the shroud needs it for a fixed index rather
    // than for whatever the map grid holds at a cell" -- and then nothing ever
    // called it that way. kShroudMetatile was declared and never used.
    //
    // So the moment the map's own tables are known, so are the fog tiles.
    // Exactly, from the same source the terrain widener reads, with no frames
    // of observation and no vote to go wrong.
    //
    // AND THE VOTE COULD GO WRONG, in a way this explains. The learner's own
    // note records "0x64 kept winning every phase forever, so the table never
    // committed at all and the widened columns stayed blank". 0x64 is 100 --
    // kClearTile, the value BG2 holds where ground is REVEALED. The learner
    // was voting for the clear tile and rejecting it, on a map whose shroud
    // metatile index happens to be the same number.
    //
    // The observational learner is kept as the fallback: it still runs, and it
    // still corrects these if the guest disagrees, so a map that does not use
    // metatile 100 is no worse off than before.
    // OFF BY DEFAULT, because metatile 100 is NOT the shroud and this seeded
    // the wrong tiles.
    //
    // RETRACTION. The note at kShroudMetatile reads "func_0203A4B4 writes
    // index 100 for every tile the visibility test flags", and this took that
    // as the shroud's metatile index. It is not. kClearTile is 0x064 -- also
    // 100 -- and it is the tile BG2 holds where ground is REVEALED, so what
    // that function writes is the CLEAR tile where the visibility test passes.
    // The comment conflated a tile index with a metatile index and the seed
    // inherited the error.
    //
    // Measured across three freeplay maps by the disagreement check below:
    //
    //   map 0  seeded 11E 19F 1A0 121 1A1 1A2   observed 0C2 0C0 0C1 0BD 0BB 0BC
    //   map 2  seeded 196 197 198 199 19A 19B   observed 0C2 0C0 0C1 0BD 0BB 0BC
    //
    // Six of six phases wrong on both, and the seed's own values differ per
    // map while the observed ones do not -- which is the shape of reading the
    // wrong index out of a per-map table.
    //
    // The cold start it targets is real and measured: without a seed the band
    // sits at 0 of 73041 fog tiles for the first 16 [lb_shroud] reports, and
    // with one it is 73041 of 73041 from the first.
    //
    // The seed now uses kShroudTiles -- the measured constants -- rather than
    // a metatile lookup, so it no longer depends on the retracted claim. On by
    // default; LB_FOGSEED=0 forces the observational learner for an A/B.
    static const bool seed_on = [] {
        const char* v = std::getenv("LB_FOGSEED");
        return !(v && v[0] == '0');
    }();
    if (seed_on && !fog_learned()) {
        for (int p = 0; p < 6; ++p) {
            g_fog_tiles[p] = kShroudTiles[p];
            g_fog_known[p] = true;
            g_fog_seeded[p] = kShroudTiles[p];
        }
        g_fog_seeded_ok = true;   // the learner checks these; see the report
        fog_note_ready("seeded from the measured constants");
        std::fprintf(stderr,
                     "[lb_fog] seeded %03X %03X %03X %03X %03X %03X "
                     "(no learning needed)\n",
                     unsigned(kShroudTiles[0]), unsigned(kShroudTiles[1]),
                     unsigned(kShroudTiles[2]), unsigned(kShroudTiles[3]),
                     unsigned(kShroudTiles[4]), unsigned(kShroudTiles[5]));
        std::fflush(stderr);
    }

    // A SHORT HISTORY OF CAMERA READINGS, newest first.
    //
    // The view struct's camera is read here, on vblank, after the guest's
    // frame logic has already advanced it. The guest's TILEMAP does not
    // advance at the same moment -- measured, it lags by one pan step for one
    // to two frames -- so the value read here names a camera the picture on
    // screen is not yet drawn from. Building the band from it puts the widened
    // columns a whole pan step away from the native ones, which is the
    // side-views breaking at times and becoming inconsistent.
    //
    // The history exists so the render-time check has something to choose
    // BETWEEN. Four is enough for a lag of one or two frames with room over.
    if (g_cam_hist_n == 0 || g_cam_hist_tx[0] != cam_tx ||
        g_cam_hist_ty[0] != cam_ty) {
        for (int i = 3; i > 0; --i) {
            g_cam_hist_tx[i] = g_cam_hist_tx[i - 1];
            g_cam_hist_ty[i] = g_cam_hist_ty[i - 1];
        }
        g_cam_hist_tx[0] = cam_tx;
        g_cam_hist_ty[0] = cam_ty;
        if (g_cam_hist_n < 4) ++g_cam_hist_n;
    }

    g_cam_tx = cam_tx;
    g_cam_ty = cam_ty;
    g_cam_px_x = cam_px_x;
    g_cam_px_y = cam_px_y;
    g_cam_px_valid = true;
    ++g_fill_bail[0];
    g_map_w = map_w;
    g_map_h = map_h;
    g_map = map;
    g_dispcnt = dispcnt;
    g_geom_valid = true;

    // THE SHROUD RUNS EVERY FRAME. The terrain below does not, and must not.
    //
    // Terrain only changes when the camera crosses a tile, so it is gated on
    // that. Fog is different: it lifts as units walk, with the camera perfectly
    // still, so gating it the same way would freeze the widened fog while the
    // native columns cleared beside it.
    {   // One-shot correlation search, opt-in via LB_FINDFOG=1.
        static int findfog = -1;
        if (findfog < 0) {
            // LB_FOGSRC=map implies discovery: using the grid requires
            // finding it, and making the caller pass two switches to get one
            // feature is a trap.
            const char* v = std::getenv("LB_FINDFOG");
            findfog = ((v && v[0] != '0') || g_fogsrc_map) ? 1 : 0;
        }
        static unsigned wait = 0;
        // GIVING UP IS COUNTED IN SEARCHES, NOT IN FRAMES.
        //
        // It used to be `wait > 3000`, i.e. about fifty seconds of wall clock
        // whatever happened in them -- and find_fog_record returns early,
        // having done nothing, whenever the view is not BALANCED, because a
        // view with no fog in it cannot distinguish a visibility record from
        // any buffer that is mostly non-zero. A camera parked on explored
        // ground therefore burned the whole budget on views the search could
        // not use: measured on one route, 194 attempts, every one of them
        // "120 samples (120 revealed, 0 shrouded)", and the search then retired
        // permanently having never once run. The band stayed 99% fogged for the
        // rest of the battle.
        //
        // g_findfog_views counts the views that got past that gate, so the
        // budget is spent on real attempts. Waiting longer costs one sampling
        // pass per 15 frames and nothing else.
        //
        // A DROP rearms this, because the commonest reason to lose the grid is
        // the heap moving it, and the address is discovered rather than known.
        // THE LOCATED PATH NEEDS NO WAIT, AND WAITING FOR IT COSTS TWO SECONDS
        // OF WRONG PICTURE.
        //
        // Symptom: on loading in, the level rendered wrong for about 2 seconds
        // before fixing itself. That is this counter, literally --
        // lb_widemap_fill runs once per guest vblank, so `wait >= 120` is 2.0 s
        // at 60 Hz. Until it fires g_vis_base is 0, the visibility record is
        // not consulted, and the band falls back to g_seen, which starts
        // SHROUDED everywhere and is only ever written from the guest's own 32
        // columns -- a sheet of cloud that ends in a straight wall at the
        // native boundary.
        //
        // The wait was written for the CORRELATION search below it, which
        // grades candidate buffers against g_seen and therefore needs g_seen to
        // be populated first. locate_fog_record needs none of that: it is a
        // pointer walk that validates itself (w == map_w, h == map_h,
        // stride == (w+7)/8, row pointer in main RAM), so it is correct on the
        // first frame it is tried or not at all. It was behind the counter only
        // by position.
        //
        // The correlation keeps its 120 frames untouched.
        if (findfog && !g_findfog_done) {
            const char* off = std::getenv("LB_FOGLOC");
            if (!(off && off[0] == '0') && locate_fog_record(map_w, map_h)) {
                g_findfog_done = true;
                std::fprintf(stderr,
                             "[lb_findfog] LOCATED on frame %u (no wait): "
                             "%ux%u bits, rows at %08X, stride %u\n",
                             wait, unsigned(g_vis_w), unsigned(g_vis_h),
                             g_vis_rowtab, unsigned(g_vis_stride));
                std::fflush(stderr);
            }
        }
        if (findfog && !g_findfog_done && ++wait >= 120u &&
            (wait % 15u) == 0u) {
            if (find_fog_record(dispcnt, map_w, map_h, cam_tx, cam_ty) ||
                g_findfog_views > 200u)
                g_findfog_done = true;
        }
    }
    // THE SHROUD SCAN MOVED TO RENDER TIME -- see lb_widemap_scan below.
    //
    // It used to run here, on vblank, and read VRAM that the guest had not
    // finished writing: BOTH candidate indicators came back saturated, BG2
    // non-zero on all 768 tiles and BG3 holding 0x1F on all 768, in frames whose
    // capture plainly shows revealed terrain across the middle of the screen.
    // Reading a staging buffer mid-update cannot say what is on screen. The
    // scan now happens at the top of engine A's frame, where VRAM is by
    // definition exactly what is about to be drawn.

    // THE TERRAIN TABLE IS NOT BUILT HERE ANY MORE.
    //
    // It used to be, from the camera read three lines above, and that is what
    // made the band shear during a pan: on the frame the camera crosses a
    // tile, this reading is already the NEW camera while the guest's tilemap
    // -- the picture the native columns are drawn from -- still holds the old
    // one. The band jumped a whole pan step ahead of the field beside it for
    // one frame in three, which is what showed on screen.
    //
    // Building it at render time instead lets the choice of camera be MEASURED
    // against the guest's own tilemap rather than assumed. See choose_camera.
    static int announced = 0;
    if (!announced) {
        announced = 1;
        // Asking for the widened columns is part of THIS feature, not a second
        // switch. LB_BG_EXTEND is wired in main.cpp inside an `if (serve)`
        // block the interactive path never reaches -- measured, the widener
        // reported `bg_extend=0 slot=-1` with it set, so no layer was decoded
        // with the extra columns and everything written for them was read by
        // nobody.
        nds_gpu2d_set_adaptive_bg_extend(true);
        std::fprintf(stderr,
                     "[lb_widemap] on: map %08X %dx%d metatiles, split %u, "
                     "tables %08X/%08X\n",
                     map, map_w, map_h, unsigned(split), table_a, table_b);
        std::fflush(stderr);
    }

}
