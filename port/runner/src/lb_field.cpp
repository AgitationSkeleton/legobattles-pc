// lb_field.cpp - draw the WHOLE battlefield, host-side, from guest memory.
//
// This file belongs to legobattles-decomp, not to ndsrecomp. See lb_field.h for
// why it exists. tools/lb_field.py is the reference implementation; every
// algorithm here was developed and verified there first, against the running
// game, and the comments below record the ways the obvious approach was wrong.

#include "lb_field.h"
#include "lb_fog.h"

extern "C" {
#include "runtime_arm.h"
}

// VRAM and palette RAM are NOT readable through the CPU-bus helpers from here:
// bus_read_u16/u32 return zero for 0x05000000 and 0x06000000, which rendered the
// whole map black while the debug server could read the same addresses fine. The
// video module owns those regions and maps them per VRAMCNT, so its own accessor
// is the correct one.
#include "vram.h"
// BG1HOFS/BG1VOFS, for the sub-tile scroll phase in effect this frame. See
// lb_field_camera_rect.
#include "io.h"
// bus_dump_access_ring, for the ITCM byte watch.
#include "state.h"
#include "gpu2d.h"
#include "gpu3d.h"

#include <thread>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kMainRam = 0x02000000u;
constexpr uint32_t kMainRamSize = 0x00400000u;
constexpr uint32_t kMainRamEnd = kMainRam + kMainRamSize;
constexpr int kMtEntryBytes = 12;   // six u16 tile indices
constexpr int kMtW = 3, kMtH = 2;   // tiles per metatile (docs/168)
constexpr int kTile = 8;
constexpr int kBlankTile = 0x016;   // the fully transparent tile

inline bool in_ram(uint32_t a) { return a >= kMainRam && a < kMainRamEnd; }

struct Window { uint16_t tile[24][32]; };

// One copy of main RAM. The locators scan it several times over and going
// through bus_read_* for each halfword would be far slower.
struct RamCopy {
    std::vector<uint8_t> b;
    void load() {
        b.resize(kMainRamSize);
        for (uint32_t i = 0; i < kMainRamSize; i += 4) {
            const uint32_t v = bus_read_u32(kMainRam + i);
            b[i] = (uint8_t)v;
            b[i + 1] = (uint8_t)(v >> 8);
            b[i + 2] = (uint8_t)(v >> 16);
            b[i + 3] = (uint8_t)(v >> 24);
        }
    }
    uint16_t u16(uint32_t a) const {
        const uint32_t o = a - kMainRam;
        if (o + 1 >= kMainRamSize) return 0;
        return (uint16_t)(b[o] | (b[o + 1] << 8));
    }
    uint32_t u32(uint32_t a) const {
        const uint32_t o = a - kMainRam;
        if (o + 3 >= kMainRamSize) return 0;
        return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
               ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    }
};

struct Key6 {
    uint16_t t[6];
    bool operator==(const Key6& o) const {
        return std::memcmp(t, o.t, sizeof(t)) == 0;
    }
};
struct Key6Hash {
    size_t operator()(const Key6& k) const {
        size_t h = 1469598103934665603ull;
        for (int i = 0; i < 6; ++i) { h ^= k.t[i]; h *= 1099511628211ull; }
        return h;
    }
};

uint32_t bg1_layout(uint32_t* char_base_out) {
    const uint32_t dispcnt = bus_read_u32(0x04000000u);
    const uint32_t cnt = bus_read_u16(0x0400000Au);
    if (char_base_out) {
        *char_base_out = 0x06000000u + ((dispcnt >> 24) & 7u) * 0x10000u +
                         ((cnt >> 2) & 3u) * 0x4000u;
    }
    return 0x06000000u + ((dispcnt >> 27) & 7u) * 0x10000u +
           ((cnt >> 8) & 0x1Fu) * 0x800u;
}

void read_window(uint32_t map_base, Window* w) {
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x)
            w->tile[y][x] = (uint16_t)(
                nds_video_read(9, map_base + (uint32_t)(y * 32 + x) * 2u, 2) & 0x3FFu);
}

Key6 block_at(const Window& w, int px, int py, int bx, int by) {
    Key6 k{};
    for (int dy = 0; dy < kMtH; ++dy)
        for (int dx = 0; dx < kMtW; ++dx)
            k.t[dy * kMtW + dx] = w.tile[py + by * kMtH + dy][px + bx * kMtW + dx];
    return k;
}

int table_extent(const RamCopy& ram, uint32_t base) {
    int n = 0;
    while (n < 8192) {
        const uint32_t a = base + (uint32_t)n * kMtEntryBytes;
        if (!in_ram(a + 12u)) break;
        bool ok = true;
        for (int k = 0; k < 6 && ok; ++k) ok = ram.u16(a + (uint32_t)k * 2u) < 0x400u;
        if (!ok) break;
        ++n;
    }
    return n;
}

// The primary metatile table, and the window's alignment within a metatile.
//
// Anchored on a signature the level grid cannot imitate: the table is generated,
// so its first two entries reference tiles 0..5 then 6..11. Two weaker methods
// were tried first and BOTH found the level grid instead -- taking the first RAM
// occurrence of an on-screen block (a wrong-phase block occurs somewhere by
// chance), and accepting any run where "all six values look like tile indices"
// (grid cells are small numbers too). docs/168.
bool find_primary(const RamCopy& ram, const Window& w, uint32_t* base_out,
                  int* count_out, int* px_out, int* py_out) {
    std::vector<uint32_t> cands;
    for (uint32_t a = kMainRam; a + 24u < kMainRamEnd; a += 2u) {
        bool ok = true;
        for (int k = 0; k < 12 && ok; ++k) ok = ram.u16(a + (uint32_t)k * 2u) == k;
        if (ok) cands.push_back(a);
    }
    // Which of the two failures this is decides the whole fix, and they look
    // identical from outside: "no metatile table in RAM" is a search problem,
    // "a table but nothing on screen matches it" is a VIEW problem -- the
    // window is all fog, and no amount of searching will help.
    static unsigned lb_fp = 0;
    const bool lb_say = (lb_fp++ % 8u) == 0u;
    if (cands.empty()) {
        if (lb_say) {
            std::fprintf(stderr, "[lb_locate]   find_primary: NO candidate "
                                 "tables in RAM (0..11 signature)\n");
            std::fflush(stderr);
        }
        return false;
    }

    int best = -1;
    for (uint32_t cand : cands) {
        const int n = table_extent(ram, cand);
        if (n < 64) continue;
        std::unordered_map<Key6, int, Key6Hash> lut;
        lut.reserve((size_t)n * 2u);
        for (int i = 0; i < n; ++i) {
            Key6 k{};
            for (int j = 0; j < 6; ++j)
                k.t[j] = ram.u16(cand + (uint32_t)i * kMtEntryBytes + (uint32_t)j * 2u);
            lut.emplace(k, i);
        }
        for (int py = 0; py < kMtH; ++py) {
            for (int px = 0; px < kMtW; ++px) {
                const int bw = (32 - px) / kMtW, bh = (24 - py) / kMtH;
                int score = 0;
                for (int by = 0; by < bh; ++by)
                    for (int bx = 0; bx < bw; ++bx)
                        if (lut.count(block_at(w, px, py, bx, by))) ++score;
                if (score > best) {
                    best = score;
                    *base_out = cand; *count_out = n;
                    *px_out = px; *py_out = py;
                }
            }
        }
    }
    if (best <= 0 && lb_say) {
        // Count how many DISTINCT tiles the visible window holds. A window
        // that is all fog is a handful of tiles repeated; a window of terrain
        // is many. That number tells the two cases apart without guessing.
        std::unordered_map<uint16_t, int> seen;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x) seen[w.tile[y][x]]++;
        std::fprintf(stderr,
                     "[lb_locate]   find_primary: %d candidate table(s), "
                     "best score 0 -- NOTHING ON SCREEN MATCHES. "
                     "window holds %d distinct tiles\n",
                     (int)cands.size(), (int)seen.size());
        // Print raw table entries beside raw window entries. read_window masks
        // to 0x3FF (tile number only) while the table keys are raw u16s, so if
        // the table carries palette or flip bits in the top 6 the two can
        // never compare equal no matter how good the search is. That would be
        // a per-tileset failure -- fine on a map whose entries are palette 0,
        // dead on one whose are not -- which is exactly the reported symptom.
        // Does ANY candidate's tile range even overlap the screen's? The table
        // entries carry no palette bits (measured: 0047 0049 004B ...), so the
        // masking theory is dead; what differs is the NUMBERS. If every
        // candidate tops out far below the tiles actually on screen, the real
        // table is not among them and the "0,1,2,..,11" signature -- derived
        // from one map -- does not hold for this one.
        uint16_t wlo = 0xFFFF, whi = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x) {
                const uint16_t t = w.tile[y][x];
                if (t < wlo) wlo = t;
                if (t > whi) whi = t;
            }
        std::fprintf(stderr, "[lb_locate]   window tiles %04X..%04X\n",
                     (unsigned)wlo, (unsigned)whi);
        for (size_t ci = 0; ci < cands.size() && ci < 8u; ++ci) {
            const uint32_t c = cands[ci];
            const int n = table_extent(ram, c);
            uint16_t tlo = 0xFFFF, thi = 0;
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < 6; ++j) {
                    const uint16_t t =
                        ram.u16(c + (uint32_t)i * kMtEntryBytes +
                                (uint32_t)j * 2u);
                    if (t < tlo) tlo = t;
                    if (t > thi) thi = t;
                }
            std::fprintf(stderr,
                         "[lb_locate]   cand %08X extent %d tiles %04X..%04X"
                         "%s\n", c, n, (unsigned)tlo, (unsigned)thi,
                         (n >= 64 && tlo <= whi && thi >= wlo)
                             ? "  <- overlaps the screen" : "");
        }
        std::fflush(stderr);
    }
    return best > 0;
}

// The level grid.
//
// Found by extracting the visible metatile window and searching RAM for it in
// two dimensions. The probe is the most VARIED row, because a row of nothing but
// grass matches almost everywhere; every other row must then agree at the
// candidate stride. Across the whole of main RAM this yields exactly one
// surviving candidate.
bool find_grid(const RamCopy& ram, const std::vector<std::vector<int>>& rows,
               uint32_t* base_out, int* w_out, int* probe_out, uint32_t* anchor_out) {
    static const int kWidths[] = {32, 40, 48, 56, 64, 72, 80, 96, 100, 112, 128,
                                  160, 192, 256};
    int probe = 0;
    size_t best_variety = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<int> seen;
        for (int v : rows[i]) {
            if (v < 0) continue;
            bool dup = false;
            for (int s : seen) if (s == v) { dup = true; break; }
            if (!dup) seen.push_back(v);
        }
        if (seen.size() > best_variety) { best_variety = seen.size(); probe = (int)i; }
    }
    if (best_variety < 3) return false;

    const std::vector<int>& pr = rows[probe];
    for (uint32_t a = kMainRam; a + (uint32_t)pr.size() * 2u < kMainRamEnd; a += 2u) {
        bool hit = true;
        for (size_t i = 0; i < pr.size() && hit; ++i)
            if (pr[i] >= 0) hit = ram.u16(a + (uint32_t)i * 2u) == (uint16_t)pr[i];
        if (!hit) continue;
        for (int wdt : kWidths) {
            bool all = true;
            for (size_t dy = 0; dy < rows.size() && all; ++dy) {
                const long long start =
                    (long long)a + ((long long)dy - probe) * wdt * 2;
                if (start < (long long)kMainRam) { all = false; break; }
                for (size_t dx = 0; dx < rows[dy].size() && all; ++dx) {
                    if (rows[dy][dx] < 0) continue;
                    const uint32_t cell = (uint32_t)start + (uint32_t)dx * 2u;
                    if (!in_ram(cell + 1u)) { all = false; break; }
                    all = ram.u16(cell) == (uint16_t)rows[dy][dx];
                }
            }
            if (all) {
                *anchor_out = a; *w_out = wdt; *probe_out = probe;
                *base_out = 0;
                return true;
            }
        }
    }
    return false;
}

// The grid's true base.
//
// Walking up in row strides from the camera's row is NOT enough: it makes the
// base congruent to whatever row the camera is on, which forces the reported
// camera column to 0 and can leave the base wrong by whole cells. A base wrong
// by k cells does not shear the map -- row-major indexing ROLLS it k cells
// horizontally with wraparound, so it still looks coherent. That is how a
// 4-cell error survived a visual check (docs/169). The array is preceded by an
// object header whose last two words are RAM pointers, so the walk's result is
// snapped to the nearest address with that shape.
void grid_extent(const RamCopy& ram, uint32_t row0, int width, int count,
                 uint32_t* base_out, int* rows_out) {
    const uint32_t stride = (uint32_t)width * 2u;
    uint32_t top = row0;
    while (top >= kMainRam + stride) {
        bool ok = true;
        for (int i = 0; i < width && ok; ++i)
            ok = ram.u16(top - stride + (uint32_t)i * 2u) < (uint16_t)count;
        if (!ok) break;
        top -= stride;
    }
    uint32_t bottom = row0;
    while (bottom + 2u * stride <= kMainRamEnd) {
        bool ok = true;
        for (int i = 0; i < width && ok; ++i)
            ok = ram.u16(bottom + stride + (uint32_t)i * 2u) < (uint16_t)count;
        if (!ok) break;
        bottom += stride;
    }
    *rows_out = (int)((bottom - top) / stride) + 1;

    for (int d = 0; d <= width; ++d) {
        for (int s = 0; s < 2; ++s) {
            const int delta = s ? -d : d;
            const long long cand = (long long)top + (long long)delta * 2;
            if (cand < (long long)kMainRam + 12) continue;
            const uint32_t c = (uint32_t)cand;
            if (in_ram(ram.u32(c - 12)) && in_ram(ram.u32(c - 8))) {
                *base_out = c;
                return;
            }
            if (d == 0) break;
        }
    }
    *base_out = top;
}

}  // namespace

extern "C" int lb_widemap_map_size(int* w, int* h);   /* src/lb_widemap.cpp */
extern "C" int lb_widemap_metatile(int mx, int my, uint16_t* out6);
extern "C" int lb_widemap_cam_metatile(int* mx, int* my);
extern "C" int lb_widemap_map_index(int mx, int my, uint16_t* out);
extern "C" int lb_widemap_cam_tile(int* tx, int* ty);
// The same camera at PIXEL precision. The guest's fine scroll is sub-tile, so
// positioning the host field on tile boundaries makes the bands step while the
// native columns slide. See lb_field_camera_rect.
extern "C" int lb_widemap_cam_px(int* x, int* y);

// ---- the map, by pointer chain instead of by search -------------------------
//
// THE RECIPE, out of the cartridge. func_020A3478 is the metatile expansion and
// is 106 bytes; the whole of it is the chain below:
//
//     uVar2 = *param_4;                                  // grid cell
//     if (uVar2 < *(u16 *)(DAT_020a34e4 + 2))            // the split
//         puVar1 = *(int *)(param_1 + 0x338) + 2 + uVar2 * 0xc;
//     else
//         puVar1 = *(int *)(param_1 + 0x33c) + 2 + (uVar2 - split) * 0xc;
//     ... six u16 copied out: 3 across, 2 down
//
// param_1 is the MAP OBJECT -- func_020A3234, its caller, uses
// *(u8 *)(param_1 + 0x14) as the grid row stride and as the column bound, which
// is map+0x14, the width this port already reads. So:
//
//     map    = *(u32*)0x02156CB0
//     w / h  = *(u8*)(map + 0x14) / *(u8*)(map + 0x15)
//     row    = *(u32*)(map + 0x34 + y*4)       func_020A322C
//     mt     = *(u16*)(row + x*2)
//     split  = *(u16*)(0x0214B9D0 + 2)
//     base   = mt < split ? *(u32*)(map+0x338) : *(u32*)(map+0x33C)
//     src    = base + 2 + (mt - (mt < split ? 0 : split)) * 12
//
// WHY THIS REPLACED A SEARCH. find_primary() looked for the table by scanning
// main RAM for twelve consecutive u16 equal to 0,1,...,11 -- an identity run
// assumed to be the first metatile. Measured on a 96x96 battle level, that
// signature finds four arrays, every one of them a false positive:
//
//     window tiles 0005..01F2
//     cand 021505A6 extent 16 tiles 0000..005E
//     cand 022E7A6E extent  3 tiles 0000..0011
//     cand 022F6F98 extent  3 tiles 0000..0011
//     cand 02301806 extent 49 tiles 0000..0090
//
// All four are below the 64-entry minimum, so every one is skipped and locate
// fails -- silently, disabling the entire host field. It worked on the one
// level the harness plays and nowhere else. A pointer the game itself follows
// is correct on every level by construction; a signature derived from one level
// is a correlation, and docs/247 already had to undo exactly this mistake for
// the fog record.
const uint32_t kMapPtr = 0x02156CB0u;
const uint32_t kTileSplitPtr = 0x0214B9D0u;   // +2 holds the u16 threshold

// One grid cell, through the row table.
inline uint16_t grid_cell(const LbFieldInfo* in, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= in->grid_w || cy >= in->grid_h) return 0xFFFFu;
    if (in->row_table) {
        const uint32_t row = bus_read_u32(in->row_table + (uint32_t)cy * 4u);
        if (!in_ram(row)) return 0xFFFFu;
        return bus_read_u16(row + (uint32_t)cx * 2u);
    }
    return bus_read_u16(in->grid_base +
                        (uint32_t)(cy * in->grid_w + cx) * 2u);
}

// Where a metatile index's six u16 entries live. Returns 0 when the index has
// no table -- which is a real state, not an error: the grid can hold indices
// past the secondary table on a level that only uses one.
inline uint32_t metatile_src(const LbFieldInfo* in, uint16_t mt) {
    if (mt == 0xFFFFu) return 0u;
    if (in->table_count > 0 && mt >= (uint16_t)in->table_count) {
        if (!in->secondary_table) return 0u;
        return in->secondary_table +
               (uint32_t)(mt - (uint16_t)in->table_count) * kMtEntryBytes;
    }
    if (!in->primary_table) return 0u;
    return in->primary_table + (uint32_t)mt * kMtEntryBytes;
}

extern "C" bool lb_field_locate(LbFieldInfo* out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));

    // Follow the chain first. Only if it does not validate does the old search
    // run, so a level the chain cannot read still behaves as it did before.
    {
        const uint32_t map = bus_read_u32(kMapPtr);
        if (in_ram(map)) {
            const int w = (int)bus_read_u8(map + 0x14u);
            const int h = (int)bus_read_u8(map + 0x15u);
            const uint32_t ta = bus_read_u32(map + 0x338u);
            const uint32_t tb = bus_read_u32(map + 0x33Cu);
            const uint32_t split = bus_read_u16(kTileSplitPtr + 2u);
            const uint32_t row0 = bus_read_u32(map + 0x34u);
            // VALIDATED, NOT TRUSTED -- the same discipline docs/247 applies to
            // the fog record. Between battles this pointer can be freed or
            // half-built, and adopting it then draws garbage over the field.
            if (w > 0 && h > 0 && w <= 255 && h <= 255 &&
                in_ram(ta) && in_ram(row0) && split > 0u) {
                out->map_obj = map;
                out->row_table = map + 0x34u;
                out->grid_w = w;
                out->grid_h = h;
                out->primary_table = ta + 2u;          // the +2 is the header
                out->secondary_table = in_ram(tb) ? tb + 2u : 0u;
                out->table_count = (int)split;
                uint32_t char_base = 0;
                out->map_base = bg1_layout(&char_base);
                out->char_base = char_base;
                static bool said = false;
                if (!said) {
                    said = true;
                    std::fprintf(stderr,
                                 "[lb_locate] chain: map %08X %dx%d rows %08X "
                                 "tables %08X/%08X split %u\n",
                                 map, w, h, out->row_table, ta, tb,
                                 (unsigned)split);
                    std::fflush(stderr);
                }
                return true;
            }
        }
    }

    uint32_t char_base = 0;
    const uint32_t map_base = bg1_layout(&char_base);
    Window w;
    read_window(map_base, &w);

    RamCopy ram;
    ram.load();

    // SAY WHY LOCATE FAILED, rate-limited.
    //
    // Both failure exits below were silent, and that silence cost a release:
    // the whole host-field path -- terrain, shroud, the phase 2/3 work -- is
    // gated on this function, and on a 96x96 battle map
    // it never succeeded once in 4,840 frames. The runtime log showed no
    // [lb_hostterrain] line at all, which is indistinguishable from "the
    // feature is off" and was read as exactly that for too long. A failure
    // that disables a subsystem must name itself.
    // Every 8th call, not every 120th. locate is itself rate-limited to one
    // call per 90 frames, so %120 was one line per 10,800 frames -- a single
    // sample across a whole session, which cannot distinguish "fails during
    // the menu, where BG1 is legitimately unset" from "fails in battle".
    static unsigned lb_fail = 0;
    const bool lb_say = (lb_fail++ % 8u) == 0u;
    const uint32_t lb_dispcnt = bus_read_u32(0x04000000u);
    const uint32_t lb_bg1cnt = bus_read_u16(0x0400000Au);

    uint32_t primary = 0;
    int count = 0, px = 0, py = 0;
    if (!find_primary(ram, w, &primary, &count, &px, &py)) {
        if (lb_say) {
            std::fprintf(stderr,
                         "[lb_locate] FAILED at find_primary "
                         "(map_base %08X char_base %08X "
                         "DISPCNT %08X BG1CNT %04X)\n",
                         map_base, char_base, lb_dispcnt,
                         (unsigned)lb_bg1cnt);
            std::fflush(stderr);
        }
        return false;
    }

    std::unordered_map<Key6, int, Key6Hash> lut;
    lut.reserve((size_t)count * 2u);
    for (int i = 0; i < count; ++i) {
        Key6 k{};
        for (int j = 0; j < 6; ++j)
            k.t[j] = ram.u16(primary + (uint32_t)i * kMtEntryBytes + (uint32_t)j * 2u);
        lut.emplace(k, i);
    }

    const int bw = (32 - px) / kMtW, bh = (24 - py) / kMtH;
    std::vector<std::vector<int>> rows((size_t)bh);
    for (int by = 0; by < bh; ++by) {
        rows[(size_t)by].resize((size_t)bw);
        for (int bx = 0; bx < bw; ++bx) {
            auto it = lut.find(block_at(w, px, py, bx, by));
            rows[(size_t)by][(size_t)bx] = (it == lut.end()) ? -1 : it->second;
        }
    }

    uint32_t dummy = 0, anchor = 0;
    int width = 0, probe = 0;
    if (!find_grid(ram, rows, &dummy, &width, &probe, &anchor)) {
        if (lb_say) {
            // How many of the visible blocks matched a metatile at all. If this
            // is low the window is mostly fog or UI and there is nothing to
            // match against, which is a different problem from a grid that is
            // present but not found.
            int matched = 0, total = 0;
            for (const auto& r : rows)
                for (int v : r) { ++total; if (v >= 0) ++matched; }
            std::fprintf(stderr,
                         "[lb_locate] FAILED at find_grid: primary %08X "
                         "count %d, window blocks matched %d/%d "
                         "(align %d,%d)\n",
                         primary, count, matched, total, px, py);
            std::fflush(stderr);
        }
        return false;
    }

    const uint32_t row0 = anchor - (uint32_t)probe * (uint32_t)width * 2u;
    uint32_t base = 0;
    int nrows = 0;
    grid_extent(ram, row0, width, count, &base, &nrows);

    const uint32_t cell0 = (row0 - base) / 2u;

    // The second metatile table. About a quarter of on-screen cells display a
    // block that is nowhere in the primary table; those blocks all sit on a
    // clean 12-byte lattice of their own. Its base follows from any one such
    // cell -- base = (address of the block) - (cell value) * 12 -- and every
    // other mismatching cell must agree. docs/168.
    std::unordered_map<uint32_t, int> votes;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const Key6 blk = block_at(w, px, py, bx, by);
            const uint32_t cell =
                base + (uint32_t)(((int)cell0 / width + by) * width +
                                  ((int)cell0 % width) + bx) * 2u;
            const uint16_t mi = ram.u16(cell);
            if (mi >= (uint16_t)count) continue;
            auto it = lut.find(blk);
            if (it != lut.end() && it->second == (int)mi) continue;  // explained
            for (uint32_t a = kMainRam; a + 12u < kMainRamEnd; a += 2u) {
                bool eq = true;
                for (int k = 0; k < 6 && eq; ++k)
                    eq = ram.u16(a + (uint32_t)k * 2u) == blk.t[k];
                if (!eq) continue;
                const long long b2 = (long long)a - (long long)mi * kMtEntryBytes;
                if (b2 >= (long long)kMainRam) votes[(uint32_t)b2]++;
            }
        }
    }
    uint32_t second = 0; int best_votes = 0;
    for (const auto& kv : votes)
        if (kv.second > best_votes) { best_votes = kv.second; second = kv.first; }

    out->map_base = map_base;
    out->char_base = char_base;
    out->primary_table = primary;
    out->secondary_table = (best_votes >= 3) ? second : 0;
    out->table_count = count;
    out->grid_base = base;
    out->grid_w = width;
    out->grid_h = nrows;

    // THE LAST ROW OF THE LEVEL, from the game's own record.
    //
    // grid_extent() walks main RAM looking for where the grid stops, and on the
    // 64x64 map in the Free Play route it stopped one row early: the panel
    // announced "64 x 63 metatiles" and drew 1536x1008, with the bottom row of
    // the level simply absent. lb_widemap reads the dimensions out of the level
    // record the game itself indexes with, so it is not a heuristic.
    //
    // Only the HEIGHT is taken, and only when the record's width AGREES with
    // the scanned one. The width is what the cell addressing below is built on
    // -- grid_base plus (row * width + col) * 2 -- so a disagreement there means
    // the two are not describing the same grid and neither should be trusted
    // over the other. Agreement on the width is what makes the height credible.
    {
        int rw = 0, rh = 0;
        if (lb_widemap_map_size(&rw, &rh) && rw == width && rh > 0 &&
            rh != nrows) {
            std::fprintf(stderr,
                         "[lb_field] grid height %d -> %d (the level record's, "
                         "width %d agrees)\n", nrows, rh, width);
            out->grid_h = rh;
        }
    }
    out->phase_x = px;
    out->phase_y = py;
    out->cam_cell_x = (int)(cell0 % (uint32_t)width);
    out->cam_cell_y = (int)(cell0 / (uint32_t)width);
    return true;
}

extern "C" void lb_field_size(const LbFieldInfo* info, int* w, int* h) {
    if (!info) return;
    if (w) *w = info->grid_w * kMtW * kTile;
    if (h) *h = info->grid_h * kMtH * kTile;
}

extern "C" void lb_field_camera_rect(const LbFieldInfo* info, int* x, int* y) {
    // Prefer the game's own camera (docs/174). It is exact, needs no terrain to
    // match against, and keeps working over unexplored ground -- which is where
    // terrain matching gives up (docs/173).
    // MAP SPACE, to match the render.
    //
    // lb_field_render now draws the map directly through lb_widemap, so the box
    // must be in the map's own pixels too. The camera's tile column times 8 IS
    // that position. Moving one without the other is what broke both earlier
    // attempts at this: the picture and the box described different coordinate
    // systems and the guest-window control fell to 53.5%, then 31.0%.
    // PIXELS, NOT TILES -- and this is the whole of the reported shake.
    //
    // This used to return `cam_tile * 8`, quantising the host field's position
    // to 8 px. The guest's own columns are NOT quantised: func_020396E0 sets a
    // sub-tile fine scroll, `4 - (cam_x mod 8)` horizontally and
    // `-(cam_y mod 16)` vertically, so BG1 slides smoothly between tile steps
    // while the tilemap itself only moves in whole cells.
    //
    // So the native strip moved every frame and the bands moved every eighth,
    // and the two were never in the same place while the camera was moving.
    // Measured, 200 consecutive frames, per-frame motion of each region:
    //
    //     frame  94  native +16  band  +0
    //     frame 101  native  +0  band  -8
    //     frame 108  native +16  band  +0
    //     frame 118  native  +0  band +16
    //
    // 14 of the 16 frames in which anything moved had the two disagreeing,
    // sometimes in opposite directions. That is "the middle jitters" and
    // "sprites shake when moving", and no amount of seam work could have fixed
    // it, because the seam is only where the disagreement shows.
    //
    // lb_widemap_cam_px is the SAME coordinate system as cam_tile -- both come
    // from widemap's geometry -- so this is strictly more precision, not a
    // different space. That distinction is what the note above is warning
    // about, and it is why the pixel accessor is preferred over
    // lb_field_camera() here: mixing spaces is what cost the two earlier
    // attempts.
    // THE SUB-TILE PHASE COMES FROM THE SCROLL REGISTERS, not from a sampled
    // camera -- which is what removes the one-frame offset rather than guessing
    // at its sign.
    //
    // func_0202D1B4 (the BGxOFS setter) writes `-param_2 & mask`, and
    // func_020396E0 calls it with `param_2 = 4 - (camx mod 8)` horizontally and
    // `-(camy mod 16)` vertically. Inverting that:
    //
    //     camx & 7 == (BG1HOFS + 4) & 7
    //     camy & 7 == BG1VOFS & 7
    //
    // Those registers hold the values the guest's own columns are being drawn
    // with FOR THIS FRAME, so a position built from them cannot be a frame
    // ahead or behind the native strip -- the two are the same number by
    // construction. Sampling `view+0x5E` at composite time cannot promise that,
    // because the guest may already have written the next frame's camera.
    //
    // Measured before this, over the panning stretch of a 200-frame recording:
    //
    //     native  16.4   4.1  11.3   2.7  17.9   4.7
    //     band     0.0  20.6   2.0  14.3   1.5  22.6
    //
    // Antiphase, with band deltas of exactly 0.0 -- half the picture one frame
    // stale, every other frame. That is the reported shake.
    // THE GUEST'S OWN CAMERA, every frame, in map pixels.
    //
    // lb_hostterrain_row() maps surface x to `g_ht_cx + (x - extra)`, so
    // g_ht_cx IS the map pixel drawn at the native view's left edge -- which is
    // the definition of the guest's camera. Anything else here is a different
    // quantity wearing the same name.
    //
    // It used to prefer lb_widemap's cached tile camera. Traced per frame, that
    // did not track the guest at all: of 927 frames on which the camera moved,
    // 877 -- 94.6% -- had the host stepping by a different amount, repeatedly
    // `guest dx=+16, host dx=+0`. Half the picture held still while the other
    // half moved, which is the reported shake.
    //
    // WHY THE TRACE AND NOT THE PIXELS. Consecutive-frame pixel deltas cannot
    // settle this: terrain and fog are both 8 px tiled patterns, so a shift by
    // a whole tile is BYTE-IDENTICAL. The band region measured 6 to 48 distinct
    // colours with the top four covering 81-90% of it, and a 0.0 delta there
    // means "moved by a tile" exactly as much as "did not move". Two separate
    // false conclusions came out of that before the camera numbers were
    // compared directly.
    // RETRACTED: "g_ht_cx is the guest's camera by definition."
    //
    // The row accessor does map surface x to `g_ht_cx + (x - extra)`, so that
    // reasoning looked airtight -- but it is contradicted by measurement.
    // Positioning from lb_field_camera() made the camera TRACK perfectly (0 of
    // 925 moving frames disagreed, against 877 before) and made the picture
    // WORSE: the left seam went from mean 1.04 / max 2.42 / 0 frames above 3x
    // to mean 1.42 / max 7.32 / 5 frames. The two cameras are offset by ~52 px,
    // so they are not the same origin, and the note further up this function --
    // "the picture and the box described different coordinate systems" -- was
    // describing exactly this.
    //
    // lb_widemap_cam_px is the one in the render's own space, so alignment
    // comes from it. Whether it also tracks per frame is a separate question,
    // measured separately; precision here was the part that was wrong, not the
    // source.
    // THE VIEW OBJECT, READ FRESH, EVERY FRAME.
    //
    //     view = *(u32*)0x02152034                func_02039A5C stores it
    //     camx = *(s16*)(view + 0x5E)
    //     camy = *(s16*)(view + 0x60)
    //
    // This is the same source lb_widemap derives cam_px from, so it is in the
    // render's own coordinate space -- and reading it here rather than taking
    // widemap's cached copy means it cannot be stale.
    //
    // WHY NOT lb_field_camera(). That LOCATES a camera object by scanning, and
    // it finds a DIFFERENT one: traced per frame against widemap's, the two
    // differ by dy = 0 (mode, 697 of 921 frames) and dx = 48 (mode, 635 of
    // 921). Y agrees exactly and X is two metatiles out, which is the signature
    // of a different object, not of a lag. Positioning from it tracked the
    // motion perfectly and moved the picture: the left seam went from mean
    // 1.04 / 0 frames above 3x to mean 1.42 / max 7.32 / 5 frames.
    //
    // So: correct space from the view pointer, correct freshness from reading
    // it here. Neither a magic 48 nor a cached value.
    {
        const uint32_t view = bus_read_u32(0x02152034u);
        if (in_ram(view)) {
            const int cx = (int)(int16_t)bus_read_u16(view + 0x5Eu);
            const int cy = (int)(int16_t)bus_read_u16(view + 0x60u);
            if (cx > -256 && cx < 8192 && cy > -256 && cy < 8192) {
                if (x) *x = cx;
                if (y) *y = cy;
                return;
            }
        }
        int px = 0, py = 0;
        if (lb_widemap_cam_px(&px, &py)) {
            if (x) *x = px;
            if (y) *y = py;
            return;
        }
        int tx = 0, ty = 0;
        if (lb_widemap_cam_tile(&tx, &ty)) {
            if (x) *x = tx * kTile;
            if (y) *y = ty * kTile;
            return;
        }
    }
    int cx = 0, cy = 0;
    if (lb_field_camera(&cx, &cy)) {
        if (x) *x = cx;
        if (y) *y = cy;
        return;
    }
    if (!info) return;
    // Fallback: the located cell. phase_x counts tiles into the metatile, so the
    // window's left edge sits phase_x * 8 pixels before the camera cell.
    if (x) *x = info->cam_cell_x * kMtW * kTile - info->phase_x * kTile;
    if (y) *y = info->cam_cell_y * kMtH * kTile - info->phase_y * kTile;
}

// THE CLIP WINDOW, in map pixels. Zero width or height means "the whole map",
// which is what the overlay panel wants.
//
// WHY THIS EXISTS. docs/325 phase 1 measured lb_field_render at 1.256 ms for
// the whole 1536x1024 map and concluded a visible-rect design was
// "unnecessary". That measurement was taken at a 512x192 guest, on its own,
// before the shroud pass and the 4,096 fog-cell reads were layered on top of
// it. It does not hold at a wide-monitor surface: making locate succeed
// turned this path on for a 96x96 level for the first time and it measured
// 26.9 fps, borderline unplayable.
//
// The compositor only ever samples the rows and columns under the camera, so
// everything outside that is work thrown away -- including the full-map clear
// below, which is 1.57M writes a frame by itself.
namespace {
int g_win_x0 = 0, g_win_y0 = 0, g_win_x1 = 0, g_win_y1 = 0;
}

extern "C" void lb_field_set_window(int x0, int y0, int x1, int y1) {
    g_win_x0 = x0; g_win_y0 = y0; g_win_x1 = x1; g_win_y1 = y1;
}

// Read it back, so a caller can save and restore it -- the bench times the
// whole-map and clipped renders in the SAME frame and must not disturb the
// window lb_hostterrain_prepare set.
extern "C" void lb_field_get_window(int* x0, int* y0, int* x1, int* y1) {
    if (x0) *x0 = g_win_x0;
    if (y0) *y0 = g_win_y0;
    if (x1) *x1 = g_win_x1;
    if (y1) *y1 = g_win_y1;
}

extern "C" bool lb_field_render(const LbFieldInfo* info, uint32_t* dst) {
    if (!info || !dst || info->grid_w <= 0 || info->grid_h <= 0) return false;
    const int W = info->grid_w * kMtW * kTile;
    const int H = info->grid_h * kMtH * kTile;
    // Clamped to the map, and to the whole map when the window is unset, so a
    // caller that never sets one behaves exactly as before.
    const int wx0 = (g_win_x1 > g_win_x0) ? std::max(0, g_win_x0) : 0;
    const int wy0 = (g_win_y1 > g_win_y0) ? std::max(0, g_win_y0) : 0;
    const int wx1 = (g_win_x1 > g_win_x0) ? std::min(W, g_win_x1) : W;
    const int wy1 = (g_win_y1 > g_win_y0) ? std::min(H, g_win_y1) : H;
    if (wx1 <= wx0 || wy1 <= wy0) return false;
    // Cell range covering that pixel window.
    const int cx_lo = wx0 / (kMtW * kTile);
    const int cy_lo = wy0 / (kMtH * kTile);
    const int cx_hi = std::min(info->grid_w, (wx1 + kMtW * kTile - 1) / (kMtW * kTile));
    const int cy_hi = std::min(info->grid_h, (wy1 + kMtH * kTile - 1) / (kMtH * kTile));

    // DS colour is 5 bits per channel, but the runner expands it the way melonDS
    // does, through a 6-bit intermediate. A naive 5->8 shift does NOT match:
    // measured against the live framebuffer, grass reads (0x28,0x9A,0x28), and
    // only this reproduces all three channels (docs/169).
    uint32_t pal[256];
    for (int i = 0; i < 256; ++i) {
        const uint32_t v = nds_video_read(9, 0x05000000u + (uint32_t)i * 2u, 2);
        const uint32_t r6 = (v & 31u) * 2u, g6 = ((v >> 5) & 31u) * 2u,
                       b6 = ((v >> 10) & 31u) * 2u;
        pal[i] = 0xFF000000u | (((r6 << 2) | (r6 >> 4)) << 16) |
                 (((g6 << 2) | (g6 >> 4)) << 8) | ((b6 << 2) | (b6 >> 4));
    }

    std::vector<uint8_t> tiles((size_t)1024 * 64);
    for (size_t i = 0; i < tiles.size(); i += 4) {
        const uint32_t v = nds_video_read(9, info->char_base + (uint32_t)i, 4);
        tiles[i] = (uint8_t)v; tiles[i + 1] = (uint8_t)(v >> 8);
        tiles[i + 2] = (uint8_t)(v >> 16); tiles[i + 3] = (uint8_t)(v >> 24);
    }
    bool blank[1024];
    for (int i = 0; i < 1024; ++i) {
        blank[i] = true;
        for (int k = 0; k < 64 && blank[i]; ++k)
            if (tiles[(size_t)i * 64 + k]) blank[i] = false;
    }

    {
        static int shown = 0;
        if (shown < 3) {
            ++shown;
            int nz = 0;
            for (int i = 0; i < 1024; ++i) if (!blank[i]) ++nz;
            std::fprintf(stderr,
                         "[lb_field] render: char_base=%08X pal[1]=%08X "
                         "non-blank tiles=%d\n",
                         info->char_base, pal[1], nz);
        }
    }

    // Clear only the window, row by row -- the full-map clear was 1.57M
    // writes a frame on its own.
    for (int y = wy0; y < wy1; ++y) {
        uint32_t* row = dst + (size_t)y * (size_t)W;
        for (int x = wx0; x < wx1; ++x) row[x] = pal[0];
    }

    // LB_GRIDORIGIN=1: MEASURE the located grid's origin against the map's own
    // row-pointer table, instead of assuming a relationship for a third time.
    //
    // Two assumptions have been falsified by the guest-window control -- that
    // the grid's cells ARE the map's metatiles (control 100.0% -> 53.5%), and
    // that they differ by the camera's offset (-> 31.0%). So ask the data:
    // which (dx, dy), if any, makes the grid's stored metatile INDEX agree with
    // the index the map's table holds. Indices, not pixels, so nothing here
    // depends on tilesets, palettes or the render at all.
    {
        static int probe = -1;
        if (probe < 0) probe = std::getenv("LB_GRIDORIGIN") ? 1 : 0;
        if (probe == 1) {
            probe = 2;                       // once per process
            int best_dx = 0, best_dy = 0, best_hits = -1, best_tot = 0;
            for (int dy = -8; dy <= 8; ++dy) {
                for (int dx = -8; dx <= 8; ++dx) {
                    int hits = 0, tot = 0;
                    for (int cy = 0; cy < info->grid_h; cy += 3) {
                        for (int cx = 0; cx < info->grid_w; cx += 3) {
                            const uint16_t mine = bus_read_u16(
                                info->grid_base +
                                (uint32_t)(cy * info->grid_w + cx) * 2u);
                            uint16_t theirs = 0;
                            if (!lb_widemap_map_index(cx + dx, cy + dy, &theirs))
                                continue;
                            ++tot;
                            if (mine == theirs) ++hits;
                        }
                    }
                    if (tot >= 50 && hits > best_hits) {
                        best_hits = hits; best_tot = tot;
                        best_dx = dx; best_dy = dy;
                    }
                }
            }
            std::fprintf(stderr,
                         "[lb_gridorigin] grid %dx%d at %08X, cam cell (%d,%d):"
                         " best offset (%+d,%+d) matches %d of %d sampled"
                         " metatile indices (%.1f%%)\n",
                         info->grid_w, info->grid_h, info->grid_base,
                         info->cam_cell_x, info->cam_cell_y,
                         best_dx, best_dy, best_hits, best_tot,
                         best_tot ? 100.0 * best_hits / best_tot : 0.0);
            std::fflush(stderr);
        }
    }

    for (int cy = cy_lo; cy < cy_hi; ++cy) {
        for (int cx = cx_lo; cx < cx_hi; ++cx) {
            // THE PROVEN LOOKUP, IN MAP SPACE -- picture and camera box both.
            //
            // lb_widemap's lookup is graded against the cartridge's own tilemap
            // (4700 band cells over two opposing pans, zero disagreements),
            // while the fallback below reads the grid as a flat array when it
            // is a table of per-row pointers and picks its table by a blank-tile
            // heuristic instead of the split threshold. That is why the old
            // render was right at the camera and drifted away from it.
            //
            // Two attempts to swap in this lookup were rejected by the guest
            // window control (100.0% -> 53.5%, then -> 31.0%) because they
            // changed the picture while lb_field_camera_rect still reported the
            // grid's coordinates: box and picture in different spaces. Both move
            // together now -- this loop indexes the MAP at (cx, cy) and draws it
            // at the map's own pixels, and lb_field_camera_rect returns the
            // camera's map tile times 8. The grid's origin no longer enters into
            // it; LB_GRIDORIGIN=1 measured that displacement at (+2,+0) exactly,
            // 462 of 462 sampled indices, which also proves the flat read and
            // row stride below were never the drift's cause.
            //
            // Measured after the change, detailed pixels only, guest control
            // 99.9-100.0% throughout: LEFT 100.0%, RIGHT 20.3% -> 100.0%,
            // TOP 100.0%, BOTTOM 49.9% -> 87.3-100.0%.
            {
                uint16_t proven[6];
                if (lb_widemap_metatile(cx, cy, proven)) {
                    for (int sub = 0; sub < 6; ++sub) {
                        const uint16_t idx = proven[sub];
                        if (idx >= 1024) continue;
                        const int ox = (cx * kMtW + sub % kMtW) * kTile;
                        const int oy = (cy * kMtH + sub / kMtW) * kTile;
                        const uint8_t* td = &tiles[(size_t)idx * 64];
                        for (int py = 0; py < kTile; ++py) {
                            uint32_t* row = dst + (size_t)(oy + py) * W + ox;
                            for (int pxi = 0; pxi < kTile; ++pxi)
                                row[pxi] = pal[td[(size_t)py * kTile + pxi]];
                        }
                    }
                    continue;
                }
            }

            // FALLBACK, for when lb_widemap has not located the map's own
            // structures (the panel still has to draw something). It reads the
            // grid as a flat array and picks its table by blankness, which is
            // why it drifts with distance from the camera -- accurate enough to
            // orient by, not accurate enough to grade against.
            const uint16_t mi = grid_cell(info, cx, cy);
            // WHICH TABLE IS NOT A GUESS ANY MORE. This used to pick the
            // primary and fall back to the secondary when an entry referenced a
            // tile that was blank in VRAM -- "accurate enough to orient by",
            // and it drifts with distance from the camera. func_020A3478 picks
            // by the SPLIT and nothing else, so metatile_src() does too.
            const uint32_t src = metatile_src(info, mi);
            if (!src) continue;
            uint16_t six[6];
            for (int k = 0; k < 6; ++k)
                six[k] = bus_read_u16(src + (uint32_t)k * 2u);

            for (int sub = 0; sub < 6; ++sub) {
                const uint16_t idx = six[sub];
                if (idx >= 1024) continue;
                const int ox = (cx * kMtW + sub % kMtW) * kTile;
                const int oy = (cy * kMtH + sub / kMtW) * kTile;
                const uint8_t* td = &tiles[(size_t)idx * 64];
                for (int py = 0; py < kTile; ++py) {
                    uint32_t* row = dst + (size_t)(oy + py) * W + ox;
                    for (int pxi = 0; pxi < kTile; ++pxi) {
                        const uint8_t ci = td[py * kTile + pxi];
                        if (ci) row[pxi] = pal[ci];
                    }
                }
            }
        }
    }
    (void)kBlankTile;
    return true;
}

namespace {

// Score how well the visible window sits at (cx, cy) with alignment (px, py).
//
// Only DISTINCTIVE cells count -- those whose displayed block is not the most
// repeated block on screen. Two earlier versions failed on exactly this:
//
//   * scoring all ~120 cells at every candidate came to ~1.5M bus reads a frame
//     and stalled the emulator so the camera never moved at all;
//   * scoring a sample of cells, then ACCEPTING the previous position when it
//     still scored well, made the tracker report the same cell forever -- on a
//     map that is mostly grass, a stale position still matches most cells.
//
// Grass matches grass everywhere, so it carries no positional information. The
// modal block is skipped and the rest decide, which is the same reason
// find_grid() probes with the most varied row rather than any row.
//
// A cell counts when the displayed block equals EITHER table's entry for the
// grid value there; checking both avoids needing tile-blankness.
struct Score { int hit, miss; };

Score camera_score(const LbFieldInfo* in, const Window& w, const Key6& modal,
                   int px, int py, int cx, int cy) {
    Score sc{0, 0};
    const int bw = (32 - px) / kMtW, bh = (24 - py) / kMtH;
    if (cx < 0 || cy < 0 || cx + bw > in->grid_w || cy + bh > in->grid_h)
        return Score{-1, 0};
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const Key6 blk = block_at(w, px, py, bx, by);
            if (blk == modal) continue;                 // carries no information
            const uint16_t mi = bus_read_u16(
                in->grid_base + (uint32_t)((cy + by) * in->grid_w + cx + bx) * 2u);
            if (mi >= (uint16_t)in->table_count) continue;
            bool ok = true;
            for (int k = 0; k < 6 && ok; ++k)
                ok = bus_read_u16(in->primary_table + (uint32_t)mi * kMtEntryBytes +
                                  (uint32_t)k * 2u) == blk.t[k];
            if (!ok && in->secondary_table) {
                ok = true;
                for (int k = 0; k < 6 && ok; ++k)
                    ok = bus_read_u16(in->secondary_table +
                                      (uint32_t)mi * kMtEntryBytes +
                                      (uint32_t)k * 2u) == blk.t[k];
            }
            if (ok) ++sc.hit; else ++sc.miss;
        }
    }
    return sc;
}

Key6 modal_block(const Window& w, int px, int py) {
    const int bw = (32 - px) / kMtW, bh = (24 - py) / kMtH;
    Key6 best{}; int bestn = -1;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const Key6 k = block_at(w, px, py, bx, by);
            int n = 0;
            for (int qy = 0; qy < bh; ++qy)
                for (int qx = 0; qx < bw; ++qx)
                    if (block_at(w, px, py, qx, qy) == k) ++n;
            if (n > bestn) { bestn = n; best = k; }
        }
    }
    return best;
}

}  // namespace

extern "C" bool lb_field_track_camera(LbFieldInfo* info) {
    if (!info || info->grid_w <= 0 || info->grid_h <= 0) return false;

    // COST IS THE WHOLE PROBLEM HERE, and it took three tries to see it.
    //
    // Scoring every candidate position over every visible cell came to roughly
    // 1.5M bus reads a frame. That did not merely run slowly: it starved the
    // guest so the camera never moved, and the tracker then looked like it was
    // failing to follow a camera that was in fact standing still. Cutting the
    // candidate set helped and still left ~350k reads a frame, with the same
    // symptom.
    //
    // So the common case gets a cheap path: the camera almost never changes
    // half or alignment between frames, and moves at most a cell or two, so try
    // the current half and phase over a tiny neighbourhood first and take it on
    // a CLEAN match (no mismatching distinctive cell). Everything else is the
    // rare path.
    uint32_t char_base = 0;
    const uint32_t live = bg1_layout(&char_base);
    info->char_base = char_base;
    const uint32_t cur_half_off = (info->map_base >= live + 0x800u) ? 0x800u : 0u;

    Window w_cur;
    read_window(live + cur_half_off, &w_cur);
    {
        // A CLEAN match is required: no distinctive cell may disagree. Scoring
        // hit - 3*miss and accepting anything positive is what let the tracker
        // sit on a stale cell for the whole run -- a wrong position still hits
        // plenty of cells, it just also misses some, and "plenty" outvoted
        // "some". A wrong position is not merely worse, it is inconsistent.
        const Key6 modal = modal_block(w_cur, info->phase_x, info->phase_y);
        int besthit = 0, bx = 0, by = 0;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                const Score s = camera_score(info, w_cur, modal, info->phase_x,
                                             info->phase_y,
                                             info->cam_cell_x + dx,
                                             info->cam_cell_y + dy);
                if (s.hit < 0 || s.miss != 0) continue;
                if (s.hit > besthit) { besthit = s.hit;
                                       bx = info->cam_cell_x + dx;
                                       by = info->cam_cell_y + dy; }
            }
        }
        if (besthit >= 6) {
            info->cam_cell_x = bx;
            info->cam_cell_y = by;
            info->map_base = live + cur_half_off;
            return true;
        }
    }

    // Rare path: the alignment or the buffer half changed, or the view jumped.
    // Wider, all phases, both halves -- and still clean-match only.
    Window win[2];
    read_window(live, &win[0]);
    read_window(live + 0x800u, &win[1]);
    int best = 0, bx = 0, by = 0, bpx = 0, bpy = 0, bhalf = 0;
    for (int half = 0; half < 2; ++half) {
        for (int py = 0; py < kMtH; ++py) {
            for (int px = 0; px < kMtW; ++px) {
                const Key6 modal = modal_block(win[half], px, py);
                for (int dy = -10; dy <= 10; ++dy) {
                    for (int dx = -10; dx <= 10; ++dx) {
                        const int cx = info->cam_cell_x + dx;
                        const int cy = info->cam_cell_y + dy;
                        const Score s = camera_score(info, win[half], modal,
                                                     px, py, cx, cy);
                        if (s.hit < 0 || s.miss != 0) continue;
                        if (s.hit > best) { best = s.hit; bx = cx; by = cy;
                                            bpx = px; bpy = py; bhalf = half; }
                    }
                }
            }
        }
    }
    if (best < 6) return false;              // lost; caller re-locates
    info->cam_cell_x = bx;
    info->cam_cell_y = by;
    info->phase_x = bpx;
    info->phase_y = bpy;
    info->map_base = live + (bhalf ? 0x800u : 0u);
    return true;
}

// Entity world space is offset from the level array's frame by a constant.
//
// CALIBRATED, not derived from the decomp, so it lives in one named place
// rather than being smeared through the drawing code. Measured with the grid
// base header-anchored (so the camera column is a real value, see docs/169):
// the hero's sprite was located on screen by its minifig colours at (143,92)
// with the camera at map (88,144) and its field reading (264,240), giving
// (33,4); the castle's corner independently gives (32,6). Two anchors at the
// same camera position, agreeing within two pixels.
//
// It is NOT confirmed across camera positions, so treat it as approximate:
// good enough to put a marker on a 1536-pixel map, not good enough to build
// anything positional on.
// TWO offsets, and conflating them is what made this wrong four times.
//
//   kCameraToMap   the camera field -> map pixels
//   kEntityToMap   an entity's world position -> map pixels
//
// They are different because the camera's anchor is not a unit's anchor, and
// lb_field_camera() was subtracting the ENTITY offset from the CAMERA, so fixing
// one broke the other.
//
// kCameraToMap is measured: on a fresh level the located map camera reads
// (88,144) and the game's camera field reads (136,144). Y agrees to the pixel,
// x differs by 48 = 2 * 24, exactly two metatiles.
//
// kEntityToMap is measured by CONSTELLATION MATCHING, which is what finally made
// it trustworthy. Locating one unit by colour cannot say WHICH entity it is when
// three sit within 80 px, and three earlier values came from matching the wrong
// one. Matching the SHAPE of all three at once cannot mis-attribute: with the
// camera at (79,146) the three owner-0 entities land on three blobs with a
// spread of 11 px and a mean screen offset of (-60,-147), i.e.
//
//     screen = world - camera + (19,-1)
//     map    = camera_map + screen = world - (29, 1)
//
// Checked: the hero at world (264,240) predicts screen (204,93); its blob is at
// (202,89).
static const int kCameraToMapX = 48;
static const int kCameraToMapY = 0;

// Map pixels -> minimap pixels on the top screen.
//
// MEASURED, not derived. The camera was swept across the level with the d-pad
// and the game's own white camera indicator located in each frame, giving 18
// (camera, box) pairs fitted by least squares (docs/180):
//
//     minimap_x = 0.059882 * camera_x + 135.269
//     minimap_y = 0.091210 * camera_y +  39.683
//
// Two things fall out that could not have been guessed. The scales DIFFER, so
// the minimap squashes the battlefield into a square: 1536 * ax = 92.0 and
// 1008 * ay = 91.9, a 92x92 panel for a 1536x1008 map. And the fit predicts a
// 15.3 x 17.5 box for the 256x192 view against 16.0 x 17.7 measured, which is
// the independent check that the mapping is right.
//
// The first attempt at this measurement took the bounding box of every white
// pixel on the top screen and picked up the HUD numerals, which produced two
// mutually inconsistent scales -- worth stating because the inconsistency is
// what exposed the contamination.
static const float kMmAx = 0.059882f, kMmBx = 135.269f;
static const float kMmAy = 0.091210f, kMmBy = 39.683f;

extern "C" void lb_field_map_to_minimap(int map_x, int map_y,
                                        float* mm_x, float* mm_y) {
    // The fit is in CAMERA coordinates, which lead map coordinates by the
    // offset lb_field_camera() subtracts.
    if (mm_x) *mm_x = kMmAx * (float)(map_x + kCameraToMapX) + kMmBx;
    if (mm_y) *mm_y = kMmAy * (float)(map_y + kCameraToMapY) + kMmBy;
}

static const int kEntityToMapX = 29;
static const int kEntityToMapY = 1;

extern "C" bool lb_field_entity_at(int index, int* owner, int* category,
                                   int* map_x, int* map_y) {
    if (index < 0) return false;
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (!in_ram(pool)) return false;
    const uint32_t arr = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if (!in_ram(arr) || count <= 0 || count > 4096) return false;

    int seen = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t e = bus_read_u32(arr + (uint32_t)i * 4u);
        if (!in_ram(e)) continue;
        const uint32_t bp = bus_read_u32(e + 0x160u);
        if (!in_ram(bp)) continue;
        const uint32_t info = bus_read_u32(bp + 8u);
        if (!in_ram(info)) continue;
        if (seen++ != index) continue;
        if (owner) *owner = (int)bus_read_u8(e + 0xE4u);
        if (category) *category = (int)bus_read_u8(info + 0x5Cu);
        if (map_x) *map_x = (int)((int32_t)bus_read_u32(e + 0xECu) >> 12) - kEntityToMapX;
        if (map_y) *map_y = (int)((int32_t)bus_read_u32(e + 0xF0u) >> 12) - kEntityToMapY;
        return true;
    }
    return false;
}

// The same walk again, exposing the ENTITY's own address.
//
// WHY: the remaining question for widened-field input is why a click in the widened
// band does not select. docs/226 ruled out the entity being missing from the
// gathered list, and docs/224-225 ruled out the coordinate being lost or
// mis-signed on the way in, so what is left is downstream of both. The way to
// find it is to watch the entity itself: an in-band click that SELECTS must
// write something into this struct, and whichever instruction does that names
// the routine that decides. Then the same watch on an out-of-band click says
// whether that routine ran at all.
//
// A harness cannot watch an address it has not been told, and the entity is
// reached through a pool pointer and an array, so nothing static names it.
extern "C" bool lb_field_entity_ptr(int index, uint32_t* addr) {
    if (index < 0) return false;
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (!in_ram(pool)) return false;
    const uint32_t arr = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if (!in_ram(arr) || count <= 0 || count > 4096) return false;
    int seen = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t e = bus_read_u32(arr + (uint32_t)i * 4u);
        if (!in_ram(e)) continue;
        const uint32_t bp = bus_read_u32(e + 0x160u);
        if (!in_ram(bp)) continue;
        const uint32_t info = bus_read_u32(bp + 8u);
        if (!in_ram(info)) continue;
        if (seen++ != index) continue;
        if (addr) *addr = e;
        return true;
    }
    return false;
}

// The same walk as lb_field_entity_at, exposing the blueprint RECORD address.
//
// That address is what NAMES an entity. Records are laid out at
// base + 4 + index * 0x7C -- the constants lb_patches.cpp already uses to patch
// them -- so (record - base - 4) / 0x7C is an index into out/eur/entities.json,
// which carries the name and the category. Category 0 there is "Hero", which is
// the game's own word for the modelled special units.
//
// This exists because "a grey shape at surface x 360 is a unit" is not an answer
// to "do modelled special units render in the widened columns". WHICH unit has
// to come from the data, not from looking at it.
extern "C" bool lb_field_entity_blueprint(int index, uint32_t* record) {
    if (index < 0) return false;
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (!in_ram(pool)) return false;
    const uint32_t arr = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if (!in_ram(arr) || count <= 0 || count > 4096) return false;

    int seen = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t e = bus_read_u32(arr + (uint32_t)i * 4u);
        if (!in_ram(e)) continue;
        const uint32_t bp = bus_read_u32(e + 0x160u);
        if (!in_ram(bp)) continue;
        const uint32_t info = bus_read_u32(bp + 8u);
        if (!in_ram(info)) continue;
        if (seen++ != index) continue;
        if (record) *record = info;
        return true;
    }
    return false;
}

// The scroll callback whose view object carries the camera (docs/174).
static const uint32_t kScrollCallback = 0x020396E0u;

// THE LOCATED CAMERA OBJECT, hoisted out of lb_field_camera() so the clamp can
// write through the same pointer the reader validated.
//
// Finding it is a full main-RAM sweep, so there must be exactly one of these
// and it must be shared. A second copy in the writer would either repeat the
// sweep or, worse, drift out of step with this one after a rescan and write a
// camera into whatever object used to be live.
uint32_t g_cam_obj = 0;
static int g_cam_cooldown = 0;

// The guest was reset (BR-0007): its RAM is re-initialised, so the cached
// pointer now addresses zeroes that still pass plausible(). Forget it.
extern "C" void lb_field_reset(void) {
    g_cam_obj = 0;
    g_cam_cooldown = 0;
}

extern "C" bool lb_field_camera(int* map_x, int* map_y) {
    // The view object moves between runs, so it is located rather than
    // hardcoded: find the callback record holding the scroll function's address
    // and take the userdata in the next word. The code image also contains that
    // address in a literal pool, so candidates whose next word is not a RAM
    // pointer into a plausible object are rejected.
    uint32_t& cached = g_cam_obj;
    auto plausible = [](uint32_t obj) {
        if (!in_ram(obj)) return false;
        const int16_t x = (int16_t)bus_read_u16(obj + 0x5Eu);
        const int16_t y = (int16_t)bus_read_u16(obj + 0x60u);
        return x > -256 && x < 8192 && y > -256 && y < 8192;
    };
    int& cooldown = g_cam_cooldown;
    if (!cached || !plausible(cached)) {
        // BACK OFF AFTER A MISS. Outside a battle there is no camera object,
        // and this rescan is a FULL main-RAM sweep -- about a million
        // iterations. The effect sampler calls this every frame, so one run
        // did 19508 sweeps, each with a log line, and 2750 of its 3951 samples
        // reported "no camera". That in turn made every effect in those frames
        // read as `no-position`, which looked like a class the sampler could
        // not place rather than a lookup that was not running at all.
        if (cooldown > 0) { --cooldown; return false; }
        static int scans = 0;
        ++scans;
        if (scans <= 8 || (scans % 64) == 0)
            std::fprintf(stderr, "[lb_field] camera object rescan #%d\n", scans);
        cached = 0;
        RamCopy ram;
        ram.load();
        for (uint32_t a = kMainRam; a + 8u < kMainRamEnd; a += 4u) {
            if (ram.u32(a) != kScrollCallback) continue;
            const uint32_t obj = ram.u32(a + 4u);
            if (plausible(obj)) { cached = obj; break; }
        }
        if (!cached) {
            // Quiet, and do not sweep again next frame. Sixty calls is about a
            // second, far shorter than any menu and far longer than one sweep.
            cooldown = 60;
            static int misses = 0;
            if (++misses <= 4)
                std::fprintf(stderr, "[lb_field] camera object NOT FOUND "
                             "(quiet for 60 calls; normal outside a battle)\n");
            return false;
        }
        cooldown = 0;
        std::fprintf(stderr, "[lb_field] camera object at %08X\n", cached);
    }
    if (map_x) *map_x = (int16_t)bus_read_u16(cached + 0x5Eu) - kCameraToMapX;
    if (map_y) *map_y = (int16_t)bus_read_u16(cached + 0x60u) - kCameraToMapY;
    return true;
}

// WRITE THE CAMERA, in the same map coordinates lb_field_camera() reports.
//
// docs/112 established both halves of this: the pair at +0x5E/+0x60 is the
// camera in pixels 1:1, writing it takes and persists, and doing so is safe for
// lockstep because docs/85 found no Sim->User calls -- the camera is
// presentation. It also set the one rule a port must not break: "it bypasses
// the guest's clamp, so a port doing this must not exceed the bounds the guest
// would have enforced." The clamp in lb_patches.cpp only ever moves the camera
// INWARD, which is strictly inside those bounds.
//
// Returns 0 when there is no located camera -- outside a battle there is none,
// and the caller must not treat that as a failure worth reporting.
extern "C" int lb_field_camera_write(int map_x, int map_y) {
    if (!g_cam_obj || !in_ram(g_cam_obj)) return 0;
    bus_write_u16(g_cam_obj + 0x5Eu,
                  (uint16_t)(int16_t)(map_x + kCameraToMapX));
    bus_write_u16(g_cam_obj + 0x60u,
                  (uint16_t)(int16_t)(map_y + kCameraToMapY));
    return 1;
}

// THE LOCATED CAMERA OBJECT ITSELF, for the one caller that needs to reach a
// field other than the camera pair.
//
// docs/281: the object carries the world bound rect the guest's own clamp
// (func_0203906C) bounds the camera against -- a MIN tile point at +0x150 and a
// MAX tile point at +0x158, each with its tile x at +4 and tile y at +5.
// Widening those is how the port extends the guest's clamp outward instead of
// correcting the camera after the guest has already set it.
//
// Returns 0 before the level is up, exactly as lb_field_camera() does. The
// caller must call lb_field_camera() first: this does NOT run the locate sweep,
// because doing so per frame outside a battle is the cost that note warns
// about.
extern "C" uint32_t lb_field_camera_obj(void) {
    if (!g_cam_obj || !in_ram(g_cam_obj)) return 0u;
    return g_cam_obj;
}

// ---- the learned-sprite cache -------------------------------------------
//
// Shared by the learning pass and whatever draws the map. Sized for the entity
// pool; a slot holds one 40x40 ARGB patch with 0 meaning transparent.
namespace {
// +/-28 rather than +/-20. docs/179 measured structure sprites landing tens
// of pixels from their entity record, which put them outside the old window
// entirely -- so the centre-seeded fill found either nothing or a NEIGHBOUR,
// and stamping a neighbour at this entity's position is what drew ghost
// buildings on ground where none stood.
constexpr int kSprR = 28;
constexpr int kSprSide = kSprR * 2;
// The cache is keyed by (owner, category), NOT by entity slot.
//
// Keyed by slot, every unit had to be seen individually before it could be
// drawn off-camera, so a swordsman built after the camera moved away was never
// drawable however many identical swordsmen had already been learned. Two of a
// level's six entities were being drawn for that reason alone.
//
// Owner is part of the key because it selects the team palette: the same
// blueprint in red and in blue are different pictures.
constexpr int kMaxEntities = 64;
constexpr int kSprKeys = 4 * 64;          // owner 0..3, category 0..63

inline int spr_key(int owner, int cat) {
    return (owner & 3) * 64 + (cat & 63);
}
// One frame of the 3D layer, unpacked once per learning pass. nds_gpu3d_line()
// hands back a buffer that is only valid until the next call, so it cannot be
// sampled per entity -- every entity would read whatever line the last one asked
// for.
uint32_t g_layer3d[256 * 192];
uint8_t  g_layer3d_a[256 * 192];
std::vector<uint32_t> g_spr;
std::vector<uint8_t> g_spr_valid;
// Blob centroid offset from the patch centre, per slot: the entity record's
// anchor error for that entity, measured rather than assumed.
std::vector<int8_t> g_spr_ox, g_spr_oy;
// Running mean of that offset per entity category, for LB_DIAG.
double g_cat_ox[256] = {0}, g_cat_oy[256] = {0};
int g_cat_n[256] = {0};

// Tile graphics and palette, refreshed occasionally rather than per frame: the
// tileset is 64 KB and does not change between levels.
std::vector<uint8_t> g_tiles;
uint32_t g_pal[256];
bool g_blank[1024];
int g_gfx_age = 0;

void refresh_graphics(const LbFieldInfo* info) {
    if (!g_tiles.empty() && --g_gfx_age > 0) return;
    g_gfx_age = 120;
    g_tiles.resize((size_t)1024 * 64);
    for (size_t i = 0; i < g_tiles.size(); i += 4) {
        const uint32_t v = nds_video_read(9, info->char_base + (uint32_t)i, 4);
        g_tiles[i] = (uint8_t)v; g_tiles[i + 1] = (uint8_t)(v >> 8);
        g_tiles[i + 2] = (uint8_t)(v >> 16); g_tiles[i + 3] = (uint8_t)(v >> 24);
    }
    for (int i = 0; i < 256; ++i) {
        const uint32_t v = nds_video_read(9, 0x05000000u + (uint32_t)i * 2u, 2);
        const uint32_t r6 = (v & 31u) * 2u, g6 = ((v >> 5) & 31u) * 2u,
                       b6 = ((v >> 10) & 31u) * 2u;
        g_pal[i] = 0xFF000000u | (((r6 << 2) | (r6 >> 4)) << 16) |
                   (((g6 << 2) | (g6 >> 4)) << 8) | ((b6 << 2) | (b6 >> 4));
    }
    for (int i = 0; i < 1024; ++i) {
        g_blank[i] = true;
        for (int k = 0; k < 64 && g_blank[i]; ++k)
            if (g_tiles[(size_t)i * 64 + k]) g_blank[i] = false;
    }
}

// One map pixel of terrain, straight from the grid, by the cartridge's own
// selection rule (func_020A3478: the split, and nothing else).
uint32_t terrain_pixel(const LbFieldInfo* info, int mx, int my) {
    if (mx < 0 || my < 0) return g_pal[0];
    const int cx = mx / (kMtW * kTile), cy = my / (kMtH * kTile);
    if (cx >= info->grid_w || cy >= info->grid_h) return g_pal[0];
    const uint16_t mi = grid_cell(info, cx, cy);
    const uint32_t src = metatile_src(info, mi);
    if (!src) return g_pal[0];
    uint16_t six[6];
    for (int k = 0; k < 6; ++k)
        six[k] = bus_read_u16(src + (uint32_t)k * 2u);
    const int ix = mx % (kMtW * kTile), iy = my % (kMtH * kTile);
    const int sub = (iy / kTile) * kMtW + (ix / kTile);
    const uint16_t idx = six[sub];
    if (idx >= 1024) return g_pal[0];
    const uint8_t ci = g_tiles[(size_t)idx * 64 + (size_t)(iy % kTile) * 8 +
                               (size_t)(ix % kTile)];
    return ci ? g_pal[ci] : g_pal[0];
}
}  // namespace

extern "C" bool lb_field_still_valid(const LbFieldInfo* info) {
    if (!info || info->grid_w <= 0 || info->grid_h <= 0 || !info->grid_base)
        return false;
    // A torn-down level leaves the old addresses holding whatever replaced it,
    // which is very unlikely to keep reading as valid metatile indices.
    int ok = 0, seen = 0;
    for (int i = 0; i < 16; ++i) {
        const int cx = (i * 7) % info->grid_w;
        const int cy = (i * 11) % info->grid_h;
        const uint16_t mi = grid_cell(info, cx, cy);
        ++seen;
        // "Plausible" now means the cell resolves to a table at all, which
        // covers both tables rather than only indices below the split.
        if (metatile_src(info, mi)) ++ok;
    }
    return ok * 4 >= seen * 3;      // three quarters plausible
}

extern "C" void lb_field_learn_sprites(const LbFieldInfo* info) {
    if (!info || info->grid_w <= 0) return;
    // docs/361: this reads the TRUE native picture, every frame it runs, so
    // engine A must keep drawing it (the widened composite lets it skip).
    nds_gpu2d_want_native();
    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return;
    const uint32_t* guest = nds_gpu2d_framebuffer(1);
    if (!guest) return;
    refresh_graphics(info);
    // Unpack the 3D layer once. Components are 6-bit with red in the low bits
    // (gpu2d.cpp masks 0x3F at shifts 0, 8 and 16) and alpha is 5-bit, in
    // practice only ever 0 or 31 -- so it is a clean stencil of the units.
    for (int y = 0; y < 192; ++y) {
        const uint32_t* line3d = nds_gpu3d_line(y);
        if (!line3d) return;
        for (int x = 0; x < 256; ++x) {
            const uint32_t v = line3d[x];
            const size_t o = (size_t)y * 256u + (size_t)x;
            g_layer3d_a[o] = (uint8_t)((v >> 24) & 0x1Fu);
            const uint32_t r6 = v & 0x3Fu, g6 = (v >> 8) & 0x3Fu,
                           b6 = (v >> 16) & 0x3Fu;
            g_layer3d[o] = 0xFF000000u | (((r6 << 2) | (r6 >> 4)) << 16) |
                           (((g6 << 2) | (g6 >> 4)) << 8) |
                           ((b6 << 2) | (b6 >> 4));
        }
    }
    if (g_spr.empty()) {
        g_spr.assign((size_t)kSprKeys * kSprSide * kSprSide, 0u);
        g_spr_valid.assign(kSprKeys, 0u);
        g_spr_ox.assign(kSprKeys, 0);
        g_spr_oy.assign(kSprKeys, 0);
    }

    // Capturing a 40x40 patch means ~1600 terrain lookups per unit, each with
    // several bus reads, so learning every visible unit every frame is real
    // work for no gain -- a unit's appearance changes over animation frames, not
    // over single ones. Each slot is refreshed at most every 30 frames, and the
    // slots are staggered so the cost is spread rather than spiking.
    static unsigned tick = 0u;
    ++tick;

    int owner = 0, cat = 0, ex = 0, ey = 0;
    for (int i = 0; i < kMaxEntities && lb_field_entity_at(i, &owner, &cat, &ex, &ey); ++i) {
        const int key = spr_key(owner, cat);
        if (g_spr_valid[key] && ((tick + (unsigned)key) % 30u) != 0u) continue;
        const int sx = ex - cx, sy = ey - cy;
        if (sx < kSprR || sx >= 256 - kSprR || sy < kSprR || sy >= 192 - kSprR)
            continue;
        uint32_t* slot = &g_spr[(size_t)key * kSprSide * kSprSide];
        static uint8_t mask[kSprSide * kSprSide];
        static uint32_t pix[kSprSide * kSprSide];
        int differing = 0;
        for (int py = 0; py < kSprSide; ++py) {
            for (int pxi = 0; pxi < kSprSide; ++pxi) {
                const size_t s = (size_t)(sy - kSprR + py) * 256u +
                                 (size_t)(sx - kSprR + pxi);
                const int k = py * kSprSide + pxi;
                pix[k] = g_layer3d[s];
                mask[k] = g_layer3d_a[s] ? 1u : 0u;
                differing += mask[k];
            }
        }

        // The difference mask is not the unit. A 40x40 window centred on one
        // entity also catches whatever else fails to match terrain inside it: a
        // neighbour standing within 20 px (docs/174 measured three units inside
        // 80 px in this very save), the shroud boundary, animating water, and
        // the HUD panels. Stamping all of that at the entity's position is
        // precisely what makes off-camera units read as scattered fragments and
        // as pieces of their neighbours.
        //
        // Keeping only the blob of differing pixels CONNECTED TO THE CENTRE
        // keeps the one unit the patch is centred on and drops the rest. The
        // seed is a small disc rather than the single centre pixel because the
        // entity anchor is good to roughly +/-8 px (docs/175) and the exact
        // centre may well be a transparent pixel between the minifig's legs.
        // Label EVERY blob in the patch, then take the one nearest the centre.
        //
        // This used to flood-fill from a +/-8 disc around the patch centre, on
        // the assumption that the entity record points at its own sprite. It
        // does not: measured, a category-0 sprite sits (-16,-12) from where the
        // record predicts, so the seed disc landed on empty pixels, the fill
        // found nothing, and the entity was never learned. Over 640 frames of
        // ordinary play the learner captured a single sprite.
        //
        // Labelling everything and choosing by proximity fixes that without
        // assuming the anchor is right, and without the failure mode a
        // largest-blob rule would have: a neighbour standing nearby is a bigger
        // blob than a distant minifig, and stamping a neighbour at THIS
        // entity's position is precisely the ghost this is meant to remove.
        static uint8_t keep[kSprSide * kSprSide];
        static int16_t comp[kSprSide * kSprSide];
        static int16_t queue[kSprSide * kSprSide];
        const int total_px = kSprSide * kSprSide;
        for (int k = 0; k < total_px; ++k) comp[k] = -1;

        int best_label = -1;
        long best_d2 = 1L << 30;
        int best_size = 0;
        long best_sx = 0, best_sy = 0;
        int label = 0;
        for (int seed = 0; seed < total_px; ++seed) {
            if (!mask[seed] || comp[seed] >= 0) continue;
            int qn = 0;
            comp[seed] = (int16_t)label;
            queue[qn++] = (int16_t)seed;
            long sx_sum = 0, sy_sum = 0;
            for (int q = 0; q < qn; ++q) {
                const int k = queue[q], ky = k / kSprSide, kx = k % kSprSide;
                sx_sum += kx;
                sy_sum += ky;
                const int nx[4] = {kx - 1, kx + 1, kx, kx};
                const int ny[4] = {ky, ky, ky - 1, ky + 1};
                for (int n = 0; n < 4; ++n) {
                    if (nx[n] < 0 || nx[n] >= kSprSide) continue;
                    if (ny[n] < 0 || ny[n] >= kSprSide) continue;
                    const int nk = ny[n] * kSprSide + nx[n];
                    if (mask[nk] && comp[nk] < 0) {
                        comp[nk] = (int16_t)label;
                        queue[qn++] = (int16_t)nk;
                    }
                }
            }
            if (qn >= 60) {
                const long cx = sx_sum / qn - kSprR;
                const long cy = sy_sum / qn - kSprR;
                const long d2 = cx * cx + cy * cy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_label = label;
                    best_size = qn;
                    best_sx = sx_sum;
                    best_sy = sy_sum;
                }
            }
            ++label;
        }

        std::memset(keep, 0, sizeof keep);
        if (best_label >= 0)
            for (int k = 0; k < total_px; ++k)
                if (comp[k] == (int16_t)best_label) keep[k] = 1u;

        const int total = kSprSide * kSprSide;

        // Already known from the chosen component.
        const int kept = best_size;
        const long sx_sum = best_sx, sy_sum = best_sy;

        if (kept > total * 55 / 100) continue;      // same test, after the crop

        for (int k = 0; k < total; ++k) slot[k] = keep[k] ? pix[k] : 0u;
        // A unit covers a decent blob. The old test accepted 24 differing
        // pixels anywhere in the 1600, which is what let sparse animating
        // terrain be cached as a "sprite" and then stamped wherever that entity
        // went. Requiring a connected blob of unit size instead: a minifig at
        // this scale is roughly 12x16, so 60 px is a floor, not a typical value.
        if (kept >= 60) {
            g_spr_valid[key] = 1u;
            const int cx_off = (int)(sx_sum / kept) - kSprR;
            const int cy_off = (int)(sy_sum / kept) - kSprR;
            g_spr_ox[key] = (int8_t)cx_off;
            g_spr_oy[key] = (int8_t)cy_off;
            const int c = cat & 0xFF;
            g_cat_ox[c] = (g_cat_ox[c] * g_cat_n[c] + cx_off) / (g_cat_n[c] + 1);
            g_cat_oy[c] = (g_cat_oy[c] * g_cat_n[c] + cy_off) / (g_cat_n[c] + 1);
            ++g_cat_n[c];
            if (std::getenv("LB_DIAG") && (g_cat_n[c] % 40) == 1)
                std::fprintf(stderr,
                    "[lb_anchor] category %d: mean sprite offset "
                    "(%+.1f,%+.1f) over %d samples\n",
                    c, g_cat_ox[c], g_cat_oy[c], g_cat_n[c]);
        }
    }
}

// ---- the shroud, read back from the minimap -------------------------------
//
// Five attempts to find a map-wide visibility array in RAM failed (docs/179,
// docs/188), and the fog bitmask the renderer actually consults is only
// viewport-sized, so it cannot say what the player has explored elsewhere.
//
// But the game DRAWS what it has explored, every frame, on the minimap. Reading
// that back is the same move that made the unit stencil exact: take the
// cartridge's own output instead of reconstructing what it must have meant.
//
// Sampled colours inside the minimap panel, with most of the map unexplored:
//
//     (40, 32, 48)   7322 px   unexplored
//     (40,154, 40)    688 px   explored grass
//     (40,121, 32)    129 px   explored terrain
//     (130,162,113)    44 px   explored olive
//     (251,251,251)    64 px   the camera box outline
//
// One distinctive colour for unexplored, everything else explored or UI, so the
// classifier is an equality test rather than a threshold.
//
// The resolution is coarse and honestly so: 92 minimap pixels span 1536 map
// pixels, about 1.4 minimap pixels per metatile cell. That is enough to decide a
// cell but not to place an edge precisely, so the shroud drawn from it is
// blocky at cell granularity -- which is what the game's own fog is too, being a
// per-tile mask.
namespace {
// THE MAP'S SIZE, NOT A CONSTANT. This was `constexpr int kShroudW = 64,
// kShroudH = 63` with a fixed array behind it, sized for the 64x64 map every
// early measurement was taken on. A real battle level is **96x96**, and every
// loop here clips to the buffer, so on that map:
//
//   * cells with cx < 64 and cy < 63 -- 43% of it -- get a shroud entry;
//   * the other 57% is never written and never shrouded, so it draws fully
//     revealed whatever the game's own record says;
//   * and the diagnostic printed `record 96x96: 4032 of 4032 cells hidden`,
//     naming the MAP's dimensions beside a count taken over the BUFFER's.
//     4032 is 64*63. Nothing in that line said so.
//
// Sized from the located map instead, with a cap that is a sanity bound rather
// than a policy: the largest map in the game is 96x96 and 256 leaves room
// without letting a corrupt read allocate wildly.
constexpr int kShroudMax = 256;
int g_shroud_w = 0, g_shroud_h = 0;
std::vector<uint8_t> g_explored;
bool g_explored_ready = false;
int g_explored_gen = 0;          // bumps only when the mask actually changes
// Which source last filled g_explored. The band fog reconstruction is only
// retired in favour of the host shroud when this is true: the minimap readback
// is the coarse, camera-box-contaminated fallback, and swapping a reconstruction
// out for something worse is not an improvement.
bool g_from_record = false;

inline bool minimap_unexplored(uint32_t px) {
    const int r = (int)((px >> 16) & 0xFF);
    const int g = (int)((px >> 8) & 0xFF);
    const int b = (int)(px & 0xFF);
    // (40,32,48) is the unexplored fill; (56,48,65) is its lighter companion
    // seen along panel edges. Both are far from any terrain colour.
    return (r >= 30 && r <= 66 && g >= 22 && g <= 58 && b >= 38 && b <= 75 &&
            g < r + 6 && g < b + 6);
}

// Bump the generation only on a real change. The caller rebuilds a whole-map
// texture from this, and doing that every frame is what made the wide field cost
// 1.5M shroud writes and a 6 MB upload per frame.
void commit_explored() {
    static std::vector<uint8_t> prev;
    if (prev.size() != g_explored.size() || prev != g_explored) {
        prev = g_explored;
        ++g_explored_gen;
    }
    g_explored_ready = true;
}

// Size the mask to the map. A change of dimensions is a change of level, so the
// generation bumps and the old contents are dropped rather than reinterpreted
// under a new stride -- which would put the previous battle's shroud on this
// one's terrain at an offset that grows with each row.
// LB_FOGFULL=1 SIZES THE MASK TO THE MAP. Off by default, and that default is
// a deliberate, temporary reinstatement of a bug.
//
// Sizing the mask from the map (9af03fa2) is CORRECT: a 96x96 level
// under the old fixed 64x63 left 57% of its cells with no entry, and
// lb_field_apply_shroud skipped them so that ground drew UNSHROUDED whatever
// the game's record said. Applying the record properly is more faithful.
//
// It also reads as broken fog and broken shroud. The record measured had
// 8,856 of 9,216 cells hidden -- 96.1%, an ordinary early-battle state -- so
// applying it over a 1855x988 window fills the screen with shroud hatch, and a
// recorded frame confirms it: terrain at the bottom left where the player has
// explored, fog everywhere else, with a clean boundary. The renderer is
// working. The DS never showed this because the DS never showed more than
// 256x192 of the map at once.
//
// THIS AFFECTS THE BAND SYNTHESISER TOO, which is why turning LB_HOSTTERRAIN
// off did not undo it: lb_widemap reads the same mask through
// lb_field_shroud_at().
//
// So the old dimensions are restored to put the picture back where it was on
// 2026-09-15, and the real question -- what SHOULD the widened columns show
// for ground the player has not explored: shroud, backdrop, or terrain without
// units -- is docs/334's, a design decision rather than one to settle by
// choosing a buffer size.
bool shroud_resize(int w, int h) {
    static const int cap_w = std::getenv("LB_FOGFULL") ? kShroudMax : 64;
    static const int cap_h = std::getenv("LB_FOGFULL") ? kShroudMax : 63;
    if (w > cap_w) w = cap_w;
    if (h > cap_h) h = cap_h;
    if (w <= 0 || h <= 0 || w > kShroudMax || h > kShroudMax) return false;
    if (w == g_shroud_w && h == g_shroud_h) return true;
    g_shroud_w = w;
    g_shroud_h = h;
    g_explored.assign((size_t)w * (size_t)h, 0u);
    ++g_explored_gen;
    std::fprintf(stderr, "[lb_shroud] mask resized to %dx%d cells\n", w, h);
    std::fflush(stderr);
    return true;
}

// THE REAL RECORD, preferred over the minimap readback below.
//
// The comment block above this pair describes reading the shroud back off the
// minimap, and gives the honest reason: five attempts to find a map-wide
// visibility array in RAM had failed. docs/247 then LOCATED it, out of the
// decomp rather than by correlation -- see lb_fog.h for the chain and the
// polarity argument. It grades 100.0% against the tile the guest itself draws,
// on all three metatile phases, over 400,000+ samples.
//
// The minimap readback stays as the fallback for frames where the chain does
// not validate (between battles, mid-load), because a coarse shroud is better
// than none. LB_SHROUDSRC=minimap forces the old path so the two can be A/B'd
// on one route.
//
// WHY THIS MATTERS BEYOND FIDELITY: the minimap's camera-box outline is white,
// (251,251,251), and minimap_unexplored() tests only for the dark unexplored
// fill -- so every cell under the moving box classifies as EXPLORED while the
// box is over it and reverts when it moves on. That is the reported "shroud
// re-appears glitchily as the camera pans even in explored areas": the readback
// is sampling the game's own camera indicator as if it were terrain.
bool refresh_from_record(const LbFieldInfo* info) {
    static int allow = -1;
    if (allow < 0) {
        const char* v = std::getenv("LB_SHROUDSRC");
        allow = (v && std::strcmp(v, "minimap") == 0) ? 0 : 1;
    }
    if (!allow) return false;
    if (!lb_fog_locate(info->grid_w, info->grid_h)) {
        // A FAILURE TO LOCATE IS NOT SILENT. The fallback below still produces
        // a shroud, so without this line "the record is in use" and "the record
        // never validated and you are looking at the minimap readback" are
        // indistinguishable in the log.
        static unsigned f = 0;
        if ((f++ % 300u) == 0u) {
            std::fprintf(stderr, "[lb_shroud] record did NOT validate for a "
                                 "%dx%d map; using the minimap readback\n",
                         info->grid_w, info->grid_h);
            std::fflush(stderr);
        }
        g_from_record = false;
        return false;
    }
    g_from_record = true;
    // Size the mask to THIS map before writing it. Refusing here rather than
    // clipping is deliberate: a mask smaller than the map leaves the remainder
    // permanently unshrouded, which is worse than no host shroud at all.
    if (!shroud_resize(info->grid_w, info->grid_h)) {
        static unsigned f = 0;
        if ((f++ % 300u) == 0u) {
            std::fprintf(stderr, "[lb_shroud] refusing a %dx%d mask (cap %d)\n",
                         info->grid_w, info->grid_h, kShroudMax);
            std::fflush(stderr);
        }
        g_from_record = false;
        return false;
    }

    for (int cy = 0; cy < g_shroud_h; ++cy) {
        for (int cx = 0; cx < g_shroud_w; ++cx) {
            // Outside the map is "do not hide", matching the off-panel case of
            // the readback path: these cells are not part of the battlefield.
            const bool in_map = cx < info->grid_w && cy < info->grid_h;
            g_explored[cy * g_shroud_w + cx] =
                (!in_map || lb_fog_revealed(cx, cy)) ? 1u : 0u;
        }
    }
    // Say what the record actually reported, every 60 refreshes. Without this
    // the only readouts are colour-match counters, and a composite cannot be
    // graded by colour: grey stipple in a band frame is equally consistent with
    // fog and with water, which is precisely the question this answers. Counts
    // are unambiguous where a picture is not.
    {
        static unsigned n = 0;
        if ((n++ % 60u) == 0u) {
            int hidden = 0, total = 0;
            for (int cy = 0; cy < info->grid_h && cy < g_shroud_h; ++cy)
                for (int cx = 0; cx < info->grid_w && cx < g_shroud_w; ++cx) {
                    ++total;
                    if (!g_explored[cy * g_shroud_w + cx]) ++hidden;
                }
            // THE REGION COUNTED, NOT THE MAP'S SIZE. These were the same
            // number only by accident; the line read `record 96x96: 4032 of
            // 4032 cells hidden` on a map whose mask covered 64x63 of it, and
            // 4032 = 64*63 is the only place that was visible.
            std::fprintf(stderr,
                         "[lb_shroud] record %dx%d, mask %dx%d: %d of %d "
                         "cells hidden\n",
                         info->grid_w, info->grid_h, g_shroud_w, g_shroud_h,
                         hidden, total);
            std::fflush(stderr);
        }
    }
    // NO all-explored / all-hidden rejection here. The readback needs that guard
    // because a menu frame misclassifies the whole screen; this record is
    // validated against the map's own dimensions and stride before it is read at
    // all, so "everything revealed" is a legitimate late-game state and
    // "almost nothing revealed" is a legitimate opening one. Applying the
    // readback's guard here would refuse both ends of a normal battle.
    commit_explored();
    return true;
}

void refresh_explored(const LbFieldInfo* info) {
    g_explored_ready = false;
    if (!info || info->grid_w <= 0) return;
    if (refresh_from_record(info)) return;
    const uint32_t* top = nds_gpu2d_framebuffer(0);   // the HUD/minimap screen
    if (!top) return;
    // The fallback needs the same map-sized mask; it used to inherit whatever
    // the fixed 64x63 array was.
    if (!shroud_resize(info->grid_w, info->grid_h)) return;
    const int cw = kMtW * kTile, ch = kMtH * kTile;
    int seen = 0;
    for (int cy = 0; cy < g_shroud_h && cy < info->grid_h; ++cy) {
        for (int cx = 0; cx < g_shroud_w && cx < info->grid_w; ++cx) {
            float mx = 0.0f, my = 0.0f;
            lb_field_map_to_minimap(cx * cw + cw / 2, cy * ch + ch / 2, &mx, &my);
            const int ix = (int)(mx + 0.5f), iy = (int)(my + 0.5f);
            if (ix < 0 || ix >= 256 || iy < 0 || iy >= 192) {
                g_explored[cy * g_shroud_w + cx] = 1u;   // off-panel: do not hide
                continue;
            }
            const uint32_t px = top[(size_t)iy * 256u + (size_t)ix];
            const bool dark = minimap_unexplored(px);
            g_explored[cy * g_shroud_w + cx] = dark ? 0u : 1u;
            seen += dark ? 0 : 1;
        }
    }
    // A frame where the top screen is not the minimap -- a menu, a transition --
    // would classify everything as explored or everything as hidden. Refusing
    // both extremes keeps a bad frame from blanking or revealing the map.
    const int cells = (info->grid_w < g_shroud_w ? info->grid_w : g_shroud_w) *
                      (info->grid_h < g_shroud_h ? info->grid_h : g_shroud_h);
    if (cells > 0 && seen > 0 && seen < cells) commit_explored();
}
}  // namespace

// The map chain, read from the MAIN thread, for comparison against the same
// read inside lb_widemap's gate. The gate reports map=00000000 while this
// file's own locate reads 022BD750 as a valid 64x64 map; two prints of the
// same addresses from known-different threads is what separates a threading
// problem from a timing one.
// ONCE PER FRAME, NOT ONCE PER WRITE.
//
// docs/326: two bytes of the ITCM IRQ dispatcher (0x01FF80FF and 0x01FF8100)
// are overwritten with 0xFF in roughly one run in four. The gate then refuses
// the lifted function and the interpreter takes over -- measured at
// `cache_absent_itcm = 266,145,829` and **15.10 fps against ~50**. That is the
// one remaining thing that makes the game not smooth, and it is not a renderer
// fault.
//
// The first instrument checked those bytes inside runtime_note_code_write, on
// every guest RAM write. It cost 23-38 fps against 51 AND SUPPRESSED THE BUG:
// four armed runs produced no rejection, while the very next unprobed pair
// reproduced it. A probe that changes the timing it is observing is worthless
// here.
//
// This costs two byte reads per frame. When the bytes first change it dumps the
// bus access ring, which is the port's own record of the last N accesses and
// therefore names the writer without needing to be in the write path at all.
extern "C" void lb_itcm_watch(void) {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("LB_ITCMWATCH");
        on = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (!on) return;
    // ARM only once the boot copy has put the real bytes there, or every frame
    // before that reports a false change against a zero-filled ITCM.
    // DOES IT COME BACK? That is the whole question.
    //
    // A stray store leaves the bytes wrong for good. A COPY IN PROGRESS is
    // wrong for a few frames and then correct again, because the copy finishes.
    // The first version of this watch reported the change and then went quiet,
    // which cannot tell those apart -- the same mistake as every other probe in
    // this file that stopped talking at the interesting moment.
    static int armed = 0, bad = 0;
    static unsigned bad_frames = 0, episodes = 0;
    const uint8_t a = bus_read_u8(0x01FF80FFu);
    const uint8_t b = bus_read_u8(0x01FF8100u);
    const int ok = (a == 0xEAu && b == 0x01u);
    if (!armed) { armed = ok; return; }
    if (!ok) {
        if (!bad) {
            bad = 1;
            ++episodes;
            bad_frames = 0;
            std::fprintf(stderr,
                         "[lb_itcmwatch] episode %u: 01FF80FF/0100 became "
                         "%02X %02X (expected EA 01)\n",
                         episodes, (unsigned)a, (unsigned)b);
            std::fflush(stderr);
            if (episodes == 1u) bus_dump_access_ring(24u);
        }
        ++bad_frames;
        return;
    }
    if (bad) {
        bad = 0;
        std::fprintf(stderr,
                     "[lb_itcmwatch] episode %u RECOVERED after %u frame(s) "
                     "-- a copy in progress, not a stray write\n",
                     episodes, bad_frames);
        std::fflush(stderr);
    }
}

extern "C" void lb_field_gate_probe(void) {
    const uint32_t m = bus_read_u32(0x02156CB0u);
    const bool ok = in_ram(m);
    std::fprintf(stderr,
                 "[lb_gate_main] tid=%u map=%08X w=%d h=%d t338=%08X "
                 "row0=%08X\n",
                 (unsigned)(size_t)std::hash<std::thread::id>{}(
                     std::this_thread::get_id()),
                 m,
                 ok ? (int)bus_read_u8(m + 0x14u) : -1,
                 ok ? (int)bus_read_u8(m + 0x15u) : -1,
                 ok ? bus_read_u32(m + 0x338u) : 0u,
                 ok ? bus_read_u32(m + 0x34u) : 0u);
    std::fflush(stderr);
}

extern "C" int lb_field_shroud_generation(void) { return g_explored_gen; }

// True only when the shroud currently held came from the located map record.
// Callers use this to decide whether they may stop reconstructing fog for
// themselves; the minimap fallback does not qualify.
extern "C" int lb_field_shroud_from_record(void) {
    return (g_explored_ready && g_from_record) ? 1 : 0;
}

// Refresh the explored mask without drawing anything, so the caller can notice
// a change before deciding whether to rebuild its base texture.
extern "C" void lb_field_refresh_shroud(const LbFieldInfo* info) {
    refresh_explored(info);
}

// Paint the shroud over a whole-map buffer. Separated from the per-frame
// composite because the mask changes rarely -- only when a unit reveals new
// ground -- while the composite runs every frame.
extern "C" void lb_field_apply_shroud(const LbFieldInfo* info, uint32_t* dst) {
    if (!info || !dst || !g_explored_ready) return;
    const int W = info->grid_w * kMtW * kTile;
    const int H = info->grid_h * kMtH * kTile;
    const int cw = kMtW * kTile, ch = kMtH * kTile;
    // Same window as the render. Painting the shroud over the whole map was
    // up to 1.5M more writes a frame beside the render's own.
    const int cw0 = (g_win_x1 > g_win_x0) ? g_win_x0 / (kMtW * kTile) : 0;
    const int ch0 = (g_win_y1 > g_win_y0) ? g_win_y0 / (kMtH * kTile) : 0;
    const int cw1 = (g_win_x1 > g_win_x0)
        ? std::min(info->grid_w, (g_win_x1 + kMtW * kTile - 1) / (kMtW * kTile))
        : info->grid_w;
    const int ch1 = (g_win_y1 > g_win_y0)
        ? std::min(info->grid_h, (g_win_y1 + kMtH * kTile - 1) / (kMtH * kTile))
        : info->grid_h;
    for (int cy = std::max(0, ch0); cy < ch1 && cy < g_shroud_h; ++cy) {
        for (int cx = std::max(0, cw0); cx < cw1 && cx < g_shroud_w; ++cx) {
            if (g_explored[cy * g_shroud_w + cx]) continue;
            for (int py = 0; py < ch; ++py) {
                const int my = cy * ch + py;
                if (my < 0 || my >= H) continue;
                uint32_t* row = dst + (size_t)my * (size_t)W;
                for (int px = 0; px < cw; ++px) {
                    const int mx = cx * cw + px;
                    // MATCHED TO THE GAME'S OWN CLOUD, not to the minimap.
                    //
                    // This was 0xFF1A1420, a near-black chosen to match the
                    // minimap's unexplored fill (40,32,48). But the FIELD does
                    // not draw unexplored that way -- sampled from a wide
                    // frame, the guest's fog is a light blue-grey texture:
                    //
                    //     (154,170,178) (138,146,154) (178,195,203)
                    //
                    // The guest draws that cloud over these columns, so this
                    // colour is only seen where the two disagree by a cell at
                    // the shroud boundary. With a near-black it showed there as
                    // BLACK NOTCHES along the edge of the explored region; with
                    // the cloud's own base tone the same disagreement is
                    // invisible. The minimap is a different surface with a
                    // different palette and is not the thing to match here.
                    if (mx >= 0 && mx < W) row[mx] = 0xFF9AAAB2u;
                }
            }
        }
    }
}

extern "C" int lb_field_cell_explored(int cx, int cy) {
    if (!g_explored_ready) return 1;
    if (cx < 0 || cy < 0 || cx >= g_shroud_w || cy >= g_shroud_h) return 1;
    return g_explored[cy * g_shroud_w + cx] ? 1 : 0;
}

extern "C" bool lb_field_composite_guest(const LbFieldInfo* info, uint32_t* dst) {
    if (!info || !dst) return false;
    nds_gpu2d_want_native();   // docs/361: see lb_field_learn_sprites
    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return false;

    // Screen 1 is the bottom screen, which is the battlefield here: POWCNT1 bit
    // 15 is clear so engine A drives it, and docs/156 tied engine A's BG0-3D to
    // the units.
    const uint32_t* guest = nds_gpu2d_framebuffer(1);
    if (!guest) return false;

    const int W = info->grid_w * kMtW * kTile;
    const int H = info->grid_h * kMtH * kTile;


    // The game's own units and structures, taken from the 3D LAYER rather than
    // from the composited screen.
    //
    // This used to paste the whole 256x192 guest framebuffer over the camera
    // rect, which dragged in everything else the guest had drawn there --
    // including the shroud. Over unexplored ground that produced the reported
    // artifact exactly: a grey rectangle sitting in the middle of a map the host
    // had drawn fully revealed.
    //
    // docs/156 established by layer-toggling that BG0-3D carries the units and
    // structures, and dumping the layer on its own confirms it: the castle, its
    // towers, the builder and the hero, on a transparent background, with an
    // alpha channel that is strictly 0 or 31. So the layer IS the unit mask, and
    // no differencing against terrain is needed to find it.
    //
    // Components are 6-bit with RED in the low bits (gpu2d.cpp masks 0x3F at
    // shifts 0, 8 and 16), expanded the same way the compositor does.
    for (int y = 0; y < 192; ++y) {
        const int my = cy + y;
        if (my < 0 || my >= H) continue;
        const uint32_t* line3d = nds_gpu3d_line(y);
        if (!line3d) continue;
        for (int x = 0; x < 256; ++x) {
            const int mx = cx + x;
            if (mx < 0 || mx >= W) continue;
            const uint32_t v = line3d[x];
            if (((v >> 24) & 0x1Fu) == 0u) continue;      // transparent
            const uint32_t r6 = v & 0x3Fu;
            const uint32_t g6 = (v >> 8) & 0x3Fu;
            const uint32_t b6 = (v >> 16) & 0x3Fu;
            dst[(size_t)my * (size_t)W + (size_t)mx] =
                0xFF000000u | (((r6 << 2) | (r6 >> 4)) << 16) |
                (((g6 << 2) | (g6 >> 4)) << 8) | ((b6 << 2) | (b6 >> 4));
        }
    }
    (void)guest;

    // And the learned sprites for everything outside it. lb_field_learn_sprites()
    // fills this cache every frame; drawing here is just a blit.
    if (g_spr.empty()) return true;
    int owner = 0, cat = 0, ex = 0, ey = 0;
    int diag_total = 0, diag_unknown = 0, diag_drawn = 0;
    for (int i = 0; i < kMaxEntities && lb_field_entity_at(i, &owner, &cat, &ex, &ey); ++i) {
        // Any entity of a type that has EVER been seen is drawable, whether
        // or not this particular one has been.
        const int key = spr_key(owner, cat);
        ++diag_total;
        if (!g_spr_valid[key]) { ++diag_unknown; continue; }
        const int sx = ex - cx, sy = ey - cy;
        // Fully inside the camera means the guest drew the whole sprite. Merely
        // having the ANCHOR inside does not: the guest clips at the screen edge,
        // so an entity within kSprR of it was drawn as a half unit and pasted
        // into the map that way -- and this test then suppressed the learned
        // patch that would have completed it. Those are the units that appear
        // cut off along the camera boundary. Requiring the whole patch inside
        // lets the learned sprite, captured while the unit was well inside,
        // finish the half the guest clipped.
        if (sx >= kSprR && sx < 256 - kSprR && sy >= kSprR && sy < 192 - kSprR)
            continue;   // guest drew all of it
        ++diag_drawn;
        const uint32_t* slot = &g_spr[(size_t)key * kSprSide * kSprSide];
        for (int py = 0; py < kSprSide; ++py) {
            const int my = ey - kSprR + py;
            if (my < 0 || my >= H) continue;
            // NEVER paint inside the camera rectangle. The guest drew that area
            // itself and its rendering is authoritative there; the learned patch
            // is only a reconstruction for ground the guest does not draw.
            //
            // This matters because the stamp is not perfectly registered. Point
            // units are good to about +/-8 px, but STRUCTURES are off by tens of
            // pixels, so allowing a stamp over the camera rect drew the castle a
            // second time, offset, beside the guest's own copy -- two castles
            // where the game has one. Clipping to the outside means a unit
            // straddling the boundary gets its inner half from the guest and
            // only its outer half from the cache.
            const int inside_y = (my - cy) >= 0 && (my - cy) < 192;
            for (int px = 0; px < kSprSide; ++px) {
                const uint32_t v = slot[(size_t)py * kSprSide + (size_t)px];
                if (!v) continue;
                const int mx = ex - kSprR + px;
                if (mx < 0 || mx >= W) continue;
                if (inside_y && (mx - cx) >= 0 && (mx - cx) < 256) continue;
                dst[(size_t)my * (size_t)W + (size_t)mx] = v;
            }
        }
    }

    // Coverage, so "more units appear" is a number rather than an impression.
    // Reported on CHANGE rather than on a tick. This function runs once per
    // field dump, so a "every 120th call" rule logged exactly once, from a frame
    // before the camera had moved -- a number that flatly contradicted what the
    // dump itself showed, and would have been believed over the picture.
    if (std::getenv("LB_DIAG")) {
        static int last_t = -1, last_d = -1, last_u = -1;
        if (diag_total != last_t || diag_drawn != last_d ||
            diag_unknown != last_u) {
            last_t = diag_total; last_d = diag_drawn; last_u = diag_unknown;
            std::fprintf(stderr,
                "[lb_cover] %d entities off-camera: %d drawn, %d of an "
                "unlearned type\n", diag_total, diag_drawn, diag_unknown);
        }
    }
    return true;
}
