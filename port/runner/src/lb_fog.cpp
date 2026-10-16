// lb_fog.cpp - the map-space visibility record. See lb_fog.h for the decomp
// chain, the polarity argument and why this is a separate translation unit.

#include "lb_fog.h"

#include "runtime_arm.h"

namespace {

// DAT_02036eb8, read out of the static ARM9 image. Its sibling literal
// DAT_02036ebc is 0x02156CB0 -- the map pointer this port already uses -- which
// is the independent check that func_02036D1C is the map-space visibility test
// and not some other rectangle.
const uint32_t kFogHolder = 0x021569E4u;

uint32_t g_rowtab = 0;              // descriptor +0x14
int g_w = 0, g_h = 0;
bool g_ready = false;

inline bool main_ram(uint32_t p) {
    return p >= 0x02000000u && p < 0x02400000u;
}

}  // namespace

bool lb_fog_locate(int map_w, int map_h) {
    // VALIDATED, NOT TRUSTED. The holder is a live pointer, and a frame caught
    // between battles can have it pointing at a freed or half-built object. The
    // dimensions must match the map's own and the stride must be the one the
    // setter computes from the width, or this is not the record it claims to
    // be. Four earlier candidates were adopted and dropped for want of exactly
    // this check.
    if (map_w <= 0 || map_h <= 0) return false;

    const uint32_t holder = bus_read_u32(kFogHolder);
    if (!main_ram(holder)) return false;
    const uint32_t desc = bus_read_u32(holder + 0x10u);
    if (!main_ram(desc)) return false;

    const uint32_t stride = bus_read_u32(desc + 4u);
    const uint32_t w = bus_read_u32(desc + 8u);
    const uint32_t h = bus_read_u32(desc + 0xCu);
    const uint32_t rowtab = bus_read_u32(desc + 0x14u);
    if (!main_ram(rowtab)) return false;

    // The width and height here are the MAP's metatiles (64x64), not the
    // viewport's (32x24). The two viewport-sized bitmasks at 0x02151F28 and
    // view+0x164 were both mistaken for this record and both read 0 of 3072
    // bits, before and after a 20 s linger: they are allocated and never
    // populated.
    if (w != uint32_t(map_w) || h != uint32_t(map_h)) return false;
    if (stride != (w + 7u) / 8u) return false;
    if (bus_read_u32(rowtab) == 0u || !main_ram(bus_read_u32(rowtab)))
        return false;

    g_rowtab = rowtab;
    g_w = int(w);
    g_h = int(h);
    g_ready = true;
    return true;
}

bool lb_fog_ready(void) { return g_ready; }

void lb_fog_drop(void) {
    g_ready = false;
    g_rowtab = 0;
    g_w = g_h = 0;
}

bool lb_fog_revealed(int mx, int my) {
    if (!g_ready) return false;
    if (mx < 0 || my < 0 || mx >= g_w || my >= g_h) return false;
    const uint32_t row = bus_read_u32(g_rowtab + uint32_t(my) * 4u);
    if (!main_ram(row)) return false;
    return ((bus_read_u8(row + uint32_t(mx >> 3)) >> (mx & 7)) & 1u) != 0u;
}
