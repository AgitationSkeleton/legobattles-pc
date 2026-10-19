// lb_palette.cpp - Level Palettes: a battle's terrain in vanilla or FactionWars
// colours, switchable during the battle (docs/387).
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// THE OPTION: a 'Level Palettes' choice in the Display tab -- Per-Mod (the
// default), Vanilla, Faction Wars or Random -- that can change on the fly.
//
// WHAT A LEVEL'S PALETTE IS (docs/387, verified in the disassembly). Each world
// has one: KingTileset.NCLR, PirateTileset.NCLR, MarsTileset.NCLR, 256 BGR555
// colours at +0x28 of an uncompressed RLCN. FUN_020368DC loads it at level
// load into a palette object at [view+0x18] (view = [0x02152034]); the file
// buffer stays on the heap for the whole level and every upload copies from
// it: FUN_02028B7C queues a 512-byte copy to 0x05000000 whenever the object's
// byte +0 is 2 (dirty). The game itself edits that main-RAM copy:
//   * View_SetupLevelBGs sets index 255 and the team colours 55..63;
//   * the palette cycler rotates ranges one step left and marks it dirty --
//     8..15, 16..39 and 40..47 in every world, plus 67..71 and 239..244
//     (King), 199..204 (Pirate) or 71..79 (Mars).
// So a change is made THERE, not in palette RAM (the next upload, one to three
// frames on, would undo a write to 0x05000000): every index outside the cycled
// ranges and the runtime ones takes the target colour, and each cycled range
// takes the target table at the rotation it is in now -- found by matching
// the range against the table it holds, which is unique for every range of
// both tables (docs/387). Then byte +0 = 2, and the game uploads it.
//
// THE SETS. Only two exist: REbalance's palettes are FactionWars's byte for
// byte and FUNLite's are vanilla's (docs/387). Vanilla comes from the player's
// own ROM; FactionWars is vanilla plus the bundled differences
// (lb_palette_data.h, tools/lb_genpalette.py). "Per-Mod" is whatever the
// loaded content brings; this port loads no mod content yet, so it is the
// ROM's own, vanilla. "Random" draws per level (lb_patches.cpp's level count
// and seed, which recordings carry).
//
// Cosmetic and this machine's only: the palette never reaches the simulation.

#include "lb_overlay.h"
#include "lb_palette_data.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "runtime_arm.h"
}

extern "C" int lb_demo_play_palette(void);       /* src/lb_demo.cpp */
extern "C" int lb_level_world(void);              /* src/lb_patches.cpp */
extern "C" unsigned lb_level_pick(unsigned salt); /* src/lb_patches.cpp */
extern "C" int lb_level_is_mod(void);             /* docs/410 */
extern "C" int lb_setting_mod_palette(void);      /* lb_overlay.cpp */

namespace {

constexpr uint32_t kViewPtr = 0x02152034u;

bool main_ram(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

// The two tables per world: [0] vanilla, [1] FactionWars. Built on first use.
uint16_t g_table[3][2][256];
int g_table_ok[3] = {0, 0, 0};   // 0 not yet, 1 built, -1 failed

bool build_tables(int world) {
    if (g_table_ok[world]) return g_table_ok[world] > 0;
    unsigned char nclr[0x228];
    const LbPaletteTable& t = lb_fw_palettes[world];
    const int n = lb_rom_read(t.nclr, nclr, (int)sizeof nclr);
    if (n != (int)sizeof nclr || std::memcmp(nclr, "RLCN", 4) != 0) {
        g_table_ok[world] = -1;
        std::fprintf(stderr, "[lb_palette] %s is not the 552-byte NCLR this "
                     "expects (%d bytes); palettes left alone\n", t.nclr, n);
        return false;
    }
    for (int i = 0; i < 256; ++i)
        g_table[world][0][i] = g_table[world][1][i] =
            (uint16_t)(nclr[0x28 + 2 * i] | (nclr[0x29 + 2 * i] << 8));
    for (uint32_t k = 0; k < t.count; ++k)
        g_table[world][1][t.deltas[k].index] = t.deltas[k].colour;
    g_table_ok[world] = 1;
    return true;
}

struct Range { int a, b; };   // inclusive
constexpr Range kCommon[3] = {{8, 15}, {16, 39}, {40, 47}};
constexpr Range kWorldRanges[3][2] = {
    {{67, 71}, {239, 244}},   // King
    {{199, 204}, {-1, -1}},   // Pirate
    {{71, 79}, {-1, -1}},     // Mars
};

bool in_range(int i, int world) {
    for (const Range& r : kCommon)
        if (i >= r.a && i <= r.b) return true;
    for (const Range& r : kWorldRanges[world])
        if (r.a >= 0 && i >= r.a && i <= r.b) return true;
    return false;
}

bool runtime_index(int i) { return i == 255 || (i >= 55 && i <= 63); }

// One cycled range from table `from` to table `to`, at its current rotation.
bool convert_range(uint32_t colours, const uint16_t* from, const uint16_t* to,
                   const Range& r) {
    const int n = r.b - r.a + 1;
    for (int k = 0; k < n; ++k) {
        bool match = true;
        for (int j = 0; j < n && match; ++j)
            match = bus_read_u16(colours + 2u * (uint32_t)(r.a + j)) ==
                    from[r.a + (j + k) % n];
        if (!match) continue;
        for (int j = 0; j < n; ++j)
            bus_write_u16(colours + 2u * (uint32_t)(r.a + j), to[r.a + (j + k) % n]);
        return true;
    }
    return false;
}

uint32_t g_obj = 0u;      // the palette object last seen
int g_world = -1;
int g_in_ram = 0;         // which table the RAM copy holds: 0 vanilla, 1 FW
int g_random = 0;         // this level's Random draw

}  // namespace

// Once per frame, from lb_patches_on_vblank.
extern "C" void lb_palette_tick(void) {
    const uint32_t view = bus_read_u32(kViewPtr);
    if (!main_ram(view)) return;
    const uint32_t pal = bus_read_u32(view + 0x18u);
    if (!main_ram(pal)) return;
    if (bus_read_u8(pal + 1u) != 0u) return;            // still loading
    const uint32_t hdr = bus_read_u32(pal + 0x34u);
    if (!main_ram(hdr)) return;
    const uint32_t colours = bus_read_u32(hdr + 0x0Cu);
    if (!main_ram(colours)) return;

    if (pal != g_obj) {
        // A new level's palette, as the ROM has it.
        g_obj = pal;
        g_in_ram = 0;
        g_world = lb_level_world();
        g_random = (int)(lb_level_pick(0x50414Cu) & 1u);   // 'PAL'
    }
    if (g_world < 0 || g_world > 2 || !build_tables(g_world)) return;

    const int played = lb_demo_play_palette();
    int mode = played >= 0 ? played : lb_setting_level_palette();
    // docs/410: a mods-folder map can have its own choice (0 = the same).
    const int modpal = lb_setting_mod_palette();
    if (lb_level_is_mod() && modpal > 0) mode = modpal - 1;
    // 0 Per-Mod (the ROM's own: vanilla), 1 Vanilla, 2 FactionWars, 3 Random.
    const int want = mode == 2 ? 1 : mode == 3 ? g_random : 0;
    if (want == g_in_ram) return;

    const uint16_t* from = g_table[g_world][g_in_ram];
    const uint16_t* to = g_table[g_world][want];
    for (int i = 1; i < 256; ++i) {
        if (runtime_index(i) || in_range(i, g_world)) continue;
        bus_write_u16(colours + 2u * (uint32_t)i, to[i]);
    }
    int ranges = 0, lost = 0;
    for (const Range& r : kCommon) {
        ++ranges;
        if (!convert_range(colours, from, to, r)) ++lost;
    }
    for (const Range& r : kWorldRanges[g_world]) {
        if (r.a < 0) continue;
        ++ranges;
        if (!convert_range(colours, from, to, r)) ++lost;
    }
    bus_write_u8(pal, 2u);   // dirty: the game uploads it
    g_in_ram = want;
    static const char* const kWorld[3] = {"King", "Pirate", "Mars"};
    std::fprintf(stderr, "[lb_palette] %s tileset now in %s colours%s "
                 "(%d cycled ranges kept in phase%s)\n", kWorld[g_world],
                 want ? "FactionWars" : "vanilla",
                 mode == 3 ? " (Random)" : "", ranges - lost,
                 lost ? ", some not matched and left as they were" : "");
    std::fflush(stderr);
}
