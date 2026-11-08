// Balance > Custom units (docs/448), phase 1: export and preview.
//
// Every unit and building record (BP/Entities.ebp records 0..181) can be
// previewed animated, compared side by side with another, and exported to
// mods/units/export/<InternalName>/ as a template for a custom unit:
//
//   unit.json                 the record's known fields by name, its names,
//                             category and asset, and the raw 0x7C bytes
//   sheets/<sheet>.NCBR       the cartridge's sheets, unwrapped (RGCN)
//   sheets/<sheet>.png        the same sheets as indexed PNGs
//   frames/<action>/<dir>_<n>.png   every frame, indexed
//
// The PNGs are INDEXED on the faction palette (KingFaction.NCLR bank 0, with
// the team shades in entries 12-14 shown as Red's), so an edited frame can be
// imported back without guessing colours. Index 0 is transparent.
//
// SHEET LAYOUTS, read from the sheets (docs/448, measured by opaque runs and
// by matching each walk and attack cell against the idle frames):
//   hero (category 0)          <s>_w<d> walk and <s>_a<d> attack, one sheet
//                              per direction d, six 24x24 frames in a row
//   builder/melee/ranged (1-3) <s>_0 idle, one 24x24 frame per direction in a
//                              row; <s>_1 walk and <s>_2 attack, a row per
//                              direction, five frames per row
//   mounted (4)                rows y 96..255 of <faction>_bld_mtd, one per
//                              direction, 32x32: frames 0-4 walk, 5-7 attack
//   buildings (7+)             one cell of <faction>_bld_mtd: u16 +0x20 the
//                              8-px tile index in the 32-tile-wide sheet,
//                              +0x1E/+0x1F the size in tiles
//   transports, siege (5, 6)   3D models; not exported in phase 1
// Five directions are stored, d0 (facing away) to d4 (facing the camera);
// the other three are drawn mirrored. The compass names in the preview are a
// reading of the art, not traced in the code.

#include "imgui.h"
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include "lb_cunits.h"   // the custom-unit preview and folders
#include "lb_nsbmd.h"    // the 3D preview of the model units

extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);   // lb_teams.cpp
extern "C" const char* lb_mods_dir(void);                                              // lb_mods.cpp
extern "C" const char* lb_teams_unit_name_stock(int blueprint);                        // lb_teams.cpp
extern "C" const char* lb_spell_label(int id);                                        // lb_overlay.cpp
extern "C" int lb_spell_listed(int id);

namespace {

namespace fs = std::filesystem;

std::vector<uint8_t> rom_file(const std::string& path) {
    const int n = lb_rom_read_unwrapped(path.c_str(), nullptr, 0);
    if (n <= 0 || n > (16 << 20)) return {};
    std::vector<uint8_t> b((size_t)n);
    if (lb_rom_read_unwrapped(path.c_str(), b.data(), n) != n) return {};
    return b;
}

// ── The records ──────────────────────────────────────────────────────────────
// docs/11: 182 unit/building records of 0x7C, then 27 projectiles of 0x74 and
// 342 collectables of 0x70, from offset 4; the string pool follows them.
constexpr int kUnits = 182;
constexpr size_t kPool = 4 + 182 * 0x7C + 27 * 0x74 + 342 * 0x70;

struct Rec {
    int index = 0;
    std::string name, internal, asset;
    int category = 0;
    int cell = 0, cw = 0, ch = 0;   // buildings: tile index and size in tiles
    std::vector<uint8_t> raw;       // the 0x7C bytes
};

const std::vector<Rec>& records() {
    static std::vector<Rec> v;
    static bool done = false;
    if (done) return v;
    done = true;
    const std::vector<uint8_t> e = rom_file("BP/Entities.ebp");
    if (e.size() < kPool) return v;
    auto u16 = [&](size_t a) { return (uint32_t)(e[a] | (e[a + 1] << 8)); };
    auto u32 = [&](size_t a) { return u16(a) | (u16(a + 2) << 16); };
    auto str = [&](uint32_t off) {
        std::string s;
        for (size_t p = kPool + off; p < e.size() && e[p] && s.size() < 64; ++p) s += (char)e[p];
        return s;
    };
    for (int i = 0; i < kUnits; ++i) {
        const size_t r = 4 + (size_t)i * 0x7C;
        Rec x;
        x.index = i;
        x.internal = str(u32(r + 0x00));
        x.asset = str(u32(r + 0x24));
        x.category = e[r + 0x5C];
        x.cell = (int)u16(r + 0x20);
        x.cw = e[r + 0x1E];
        x.ch = e[r + 0x1F];
        x.raw.assign(e.begin() + (long)r, e.begin() + (long)r + 0x7C);
        const char* shown = lb_teams_unit_name_stock(i);
        x.name = (shown && shown[0] && shown[0] != '?' && std::strncmp(shown, "unit ", 5) != 0) ? shown : x.internal;
        v.push_back(std::move(x));
    }
    return v;
}

const char* category_name(int c) {
    static const char* const k[] = {"Hero", "Builder", "Melee", "Ranged", "Mounted", "Transport", "Special"};
    return c >= 0 && c < 7 ? k[c] : "Building";
}

// ── Sheets and layouts ───────────────────────────────────────────────────────
struct Sheet { int w = 0, h = 0; std::vector<uint8_t> px; std::vector<uint8_t> file; };
std::map<std::string, Sheet> g_sheets;
const Sheet& sheet(const std::string& name) {
    auto it = g_sheets.find(name);
    if (it != g_sheets.end()) return it->second;
    Sheet s;
    s.file = rom_file("Sprites/" + name + ".NCBR");
    const std::vector<uint8_t>& d = s.file;
    if (d.size() >= 0x30 && std::memcmp(d.data() + 0x10, "RAHC", 4) == 0) {
        auto u16 = [&](size_t a) { return (uint32_t)(d[a] | (d[a + 1] << 8)); };
        auto u32 = [&](size_t a) { return u16(a) | (u16(a + 2) << 16); };
        const uint32_t th = u16(0x18), tw = u16(0x1A), dsize = u32(0x28), doff = u32(0x2C);
        const size_t base = 0x18 + doff;
        if (u32(0x1C) == 3 && tw && th && tw != 0xFFFF && base + dsize <= d.size()) {
            s.w = (int)tw * 8;
            s.h = (int)th * 8;
            s.px.assign((size_t)s.w * s.h, 0);
            for (size_t i = 0; i < dsize && i * 2 + 1 < s.px.size(); ++i) {
                s.px[i * 2] = d[base + i] & 15;
                s.px[i * 2 + 1] = d[base + i] >> 4;
            }
        }
    }
    return g_sheets.emplace(name, std::move(s)).first->second;
}

struct Frame { std::string sheet; int x = 0, y = 0, w = 0, h = 0; };
struct Action { std::string name; std::array<std::vector<Frame>, 5> dirs; };
struct Layout { std::vector<Action> actions; std::vector<std::string> sheets; std::string note; };

bool frame_blank(const Frame& f) {
    const Sheet& s = sheet(f.sheet);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            const int X = f.x + x, Y = f.y + y;
            if (X < s.w && Y < s.h && s.px[(size_t)Y * s.w + X]) return false;
        }
    return true;
}

Layout layout_of(const Rec& r) {
    Layout L;
    const std::string pre = "Sprites/";
    if (r.asset.compare(0, pre.size(), pre) != 0) {
        L.note = "A 3D model (" + r.asset + "): its texture is shown and exported for re-skinning.";
        return L;
    }
    const std::string base = r.asset.substr(pre.size());
    auto add = [&](Action& a, int d, const std::string& sh, int x, int y, int w, int h) {
        Frame f{sh, x, y, w, h};
        if (!sheet(sh).px.empty() && !frame_blank(f)) a.dirs[(size_t)d].push_back(f);
    };
    auto keep = [&](const std::string& sh) {
        if (std::find(L.sheets.begin(), L.sheets.end(), sh) == L.sheets.end() && !sheet(sh).px.empty())
            L.sheets.push_back(sh);
    };
    Action idle{"idle", {}}, walk{"walk", {}}, attack{"attack", {}};
    if (r.category == 0) {
        for (int d = 0; d < 5; ++d) {
            const std::string w = base + "_w" + std::to_string(d), a = base + "_a" + std::to_string(d);
            keep(w);
            keep(a);
            add(idle, d, w, 0, 0, 24, 24);
            for (int f = 0; f < 6; ++f) add(walk, d, w, f * 24, 0, 24, 24);
            for (int f = 0; f < 6; ++f) add(attack, d, a, f * 24, 0, 24, 24);
        }
    } else if (r.category >= 1 && r.category <= 3) {
        for (int k = 0; k < 3; ++k) keep(base + "_" + std::to_string(k));
        for (int d = 0; d < 5; ++d) {
            add(idle, d, base + "_0", d * 24, 0, 24, 24);
            for (int f = 0; f < 5; ++f) add(walk, d, base + "_1", f * 24, d * 24, 24, 24);
            for (int f = 0; f < 5; ++f) add(attack, d, base + "_2", f * 24, d * 24, 24, 24);
        }
    } else if (r.category == 4) {
        keep(base);
        for (int d = 0; d < 5; ++d) {
            add(idle, d, base, 0, 96 + d * 32, 32, 32);
            // Mounts have 4 walk and 4 attack frames (BR-0045/0048), not 5 and 3.
            for (int f = 0; f < 4; ++f) add(walk, d, base, f * 32, 96 + d * 32, 32, 32);
            for (int f = 4; f < 8; ++f) add(attack, d, base, f * 32, 96 + d * 32, 32, 32);
        }
    } else {
        keep(base);
        Frame f{base, (r.cell & 31) * 8, (r.cell >> 5) * 8, r.cw * 8, r.ch * 8};
        if (f.w > 0 && f.h > 0 && f.w <= 256 && f.h <= 256) {
            for (int d = 0; d < 5; ++d) idle.dirs[(size_t)d].push_back(f);
            L.actions.push_back(idle);
        }
        L.note = "Buildings have one picture; it does not turn or animate.";
        return L;
    }
    for (Action* a : {&idle, &walk, &attack}) {
        bool any = false;
        for (const auto& d : a->dirs) any = any || !d.empty();
        if (any) L.actions.push_back(*a);
    }
    if (L.actions.empty()) L.note = "No frames found in " + base + "'s sheets.";
    return L;
}

// ── Palette ──────────────────────────────────────────────────────────────────
// The team shades (0x02127F38): Red, Blue, Green, Yellow, Purple, Grey.
const uint16_t kTeamShades[6][3] = {{0x113F, 0x005C, 0x0015}, {0x7628, 0x6D60, 0x5880},
                                    {0x03AF, 0x02A6, 0x01E0}, {0x035F, 0x023F, 0x011D},
                                    {0x7C1F, 0x4419, 0x1C12}, {0x3D8A, 0x3106, 0x1880}};
// SG-0064: colour 6, Teal, light/mid/dark by lb_setting_teal_style(): 0 the
// cartridge's unused teal (KingFaction.NCLR banks 10/11), 1 the port's ramp.
// The same words as lb_patches.cpp's kTealShades (docs/412).
const uint16_t kTealShades[2][3] = {{0x7FFF, 0x6B73, 0x56EA}, {0x6764, 0x4E80, 0x31A0}};
const char* const kTeamNames[7] = {"Red", "Blue", "Green", "Yellow", "Purple", "Grey", "Teal"};
constexpr int kTeams = 7;
extern "C" int lb_setting_teal_style(void);   // lb_overlay.cpp

// The preview's team number with the teal style folded in (6 or 7), so the
// texture cache follows the style setting.
int team_key(int t) { return t == 6 ? 6 + (lb_setting_teal_style() ? 1 : 0) : t; }
const uint16_t* team_shades(int t) {
    return t >= 6 ? kTealShades[t == 7 ? 1 : 0] : kTeamShades[t];
}

uint32_t ds_rgba(uint16_t c) {
    auto ch = [](int v) { const int s = v * 2; return (uint32_t)(((s << 2) | (s >> 4)) & 255); };
    return 0xFF000000u | (ch((c >> 10) & 31) << 16) | (ch((c >> 5) & 31) << 8) | ch(c & 31);
}

// Bank 0 of KingFaction.NCLR with team `t`'s shades in 12-14, as ABGR.
std::array<uint32_t, 16> palette(int t) {
    static uint16_t bank[16];
    static bool ok = false;
    if (!ok) {
        const std::vector<uint8_t> d = rom_file("Sprites/KingFaction.NCLR");
        if (d.size() >= 0x28 + 32)
            for (int i = 0; i < 16; ++i) bank[i] = (uint16_t)(d[0x28 + i * 2] | (d[0x29 + i * 2] << 8));
        ok = true;
    }
    std::array<uint32_t, 16> p{};
    for (int i = 0; i < 16; ++i) p[(size_t)i] = ds_rgba(bank[i]);
    const uint16_t* sh = team_shades(t < 0 || t > 7 ? 0 : t);   // t: 0..5, 6 or 7 teal (team_key)
    for (int k = 0; k < 3; ++k) p[(size_t)(12 + k)] = ds_rgba(sh[k]);
    p[0] = 0;
    return p;
}

// ── PNG (indexed) ────────────────────────────────────────────────────────────
void put32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24)); o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8));  o.push_back((uint8_t)v);
}
void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& d) {
    put32(o, (uint32_t)d.size());
    const size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), d.begin(), d.end());
    put32(o, (uint32_t)crc32(0, o.data() + at, (uInt)(4 + d.size())));
}
bool indexed_png(const Sheet& s, int sx, int sy, int w, int h, std::vector<uint8_t>* out,
                 const std::array<uint32_t, 16>* with_pal = nullptr) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const int X = sx + x, Y = sy + y;
            raw.push_back(X < s.w && Y < s.h ? s.px[(size_t)Y * s.w + X] : 0);
        }
    }
    uLongf cap = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(cap);
    if (compress2(z.data(), &cap, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
    z.resize(cap);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out->assign(sig, sig + 8);
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(3); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(*out, "IHDR", ihdr);
    const std::array<uint32_t, 16> pal = with_pal ? *with_pal : palette(0);
    std::vector<uint8_t> plte, trns;
    for (uint32_t c : pal) {
        plte.push_back((uint8_t)c); plte.push_back((uint8_t)(c >> 8)); plte.push_back((uint8_t)(c >> 16));
    }
    trns.push_back(0);   // index 0 transparent, the rest opaque
    chunk(*out, "PLTE", plte);
    chunk(*out, "tRNS", trns);
    chunk(*out, "IDAT", z);
    chunk(*out, "IEND", {});
    return true;
}

// A path as UTF-8 in a std::string, under C++17 and C++20 alike.
std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool write_file(const fs::path& p, const std::vector<uint8_t>& b) {
    std::FILE* f = std::fopen(utf8(p).c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

// An RGBA32 picture (ABGR in memory, 0 = clear) as a truecolour PNG.
bool rgba_png(int w, int h, const uint32_t* px, std::vector<uint8_t>* out) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint32_t c = px[(size_t)y * w + x];
            raw.push_back((uint8_t)c); raw.push_back((uint8_t)(c >> 8));
            raw.push_back((uint8_t)(c >> 16)); raw.push_back((uint8_t)(c >> 24));
        }
    }
    uLongf cap = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(cap);
    if (compress2(z.data(), &cap, raw.data(), (uLong)raw.size(), 9) != Z_OK) return false;
    z.resize(cap);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out->assign(sig, sig + 8);
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(*out, "IHDR", ihdr);
    chunk(*out, "IDAT", z);
    chunk(*out, "IEND", {});
    return true;
}

// ── Cards: one NCER cell drawn from its NCGR and NCLR ────────────────────────
// UI/FEPlayerCards/<InternalName> (the army screen's card) and
// UI/GamePlayerCards/<InternalName> (the battle's top screen). Measured on
// K_King: one cell of six 64x64 8bpp OBJs (OAM attr0/1/2, 1D mapping with
// 32-byte tile units), 128x192, tiled NCGR, a 256-colour NCLR.
struct Pic { int w = 0, h = 0; std::vector<uint32_t> px; };

Pic decode_cell(const std::string& base) {
    Pic out;
    const std::vector<uint8_t> e = rom_file(base + ".NCER");
    const std::vector<uint8_t> g = rom_file(base + ".NCGR");
    const std::vector<uint8_t> c = rom_file(base + ".NCLR");
    if (e.size() < 0x30 || g.size() < 0x30 || c.size() < 0x28 + 32) return out;
    auto u16 = [](const std::vector<uint8_t>& b, size_t a) { return (uint32_t)(b[a] | (b[a + 1] << 8)); };
    auto u32 = [&](const std::vector<uint8_t>& b, size_t a) { return u16(b, a) | (u16(b, a + 2) << 16); };
    if (std::memcmp(e.data() + 0x10, "KBEC", 4) != 0 || std::memcmp(g.data() + 0x10, "RAHC", 4) != 0) return out;
    const uint32_t ncell = u16(e, 0x18), cattr = u16(e, 0x1A), coff = u32(e, 0x1C);
    if (ncell == 0) return out;
    const size_t cells = 0x18 + coff, esz = cattr == 1 ? 16 : 8;
    const size_t oams = cells + ncell * esz;
    if (oams > e.size()) return out;
    const uint32_t noam = u16(e, cells), ooff = u32(e, cells + 4);
    const size_t chars = 0x18 + u32(g, 0x2C), csize = u32(g, 0x28);
    const bool linear = (u32(g, 0x24) & 0xFFu) == 1u;
    if (chars + csize > g.size()) return out;
    const size_t ncol = std::min<size_t>(256, (c.size() - 0x28) / 2);
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    struct O { int x, y, w, h, tile, pal; bool bpp8; };
    std::vector<O> os;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (uint32_t k = 0; k < noam && k < 128; ++k) {
        const size_t at = oams + ooff + k * 6;
        if (at + 6 > e.size()) break;
        const uint32_t a0 = u16(e, at), a1 = u16(e, at + 2), a2 = u16(e, at + 4);
        const int shape = (int)(a0 >> 14) & 3, size = (int)(a1 >> 14) & 3;
        if (shape == 3) continue;
        O o;
        o.y = (int)(int8_t)(a0 & 0xFF);
        o.x = (int)(a1 & 0x1FF);
        if (o.x >= 256) o.x -= 512;
        o.w = kW[shape][size];
        o.h = kH[shape][size];
        o.tile = (int)(a2 & 0x3FF);
        o.pal = (int)(a2 >> 12);
        o.bpp8 = (a0 & 0x2000u) != 0;
        os.push_back(o);
        x0 = std::min(x0, o.x); y0 = std::min(y0, o.y);
        x1 = std::max(x1, o.x + o.w); y1 = std::max(y1, o.y + o.h);
    }
    if (os.empty() || x1 - x0 > 512 || y1 - y0 > 512) return out;
    out.w = x1 - x0;
    out.h = y1 - y0;
    out.px.assign((size_t)out.w * out.h, 0u);
    for (const O& o : os) {
        const size_t tbytes = o.bpp8 ? 64 : 32;
        for (int y = 0; y < o.h; ++y)
            for (int x = 0; x < o.w; ++x) {
                size_t byte;
                int idx;
                if (linear) {
                    const size_t p = (size_t)o.tile * 32 * (o.bpp8 ? 1 : 2) + (size_t)y * o.w + x;
                    byte = o.bpp8 ? p : p / 2;
                } else {
                    const size_t t = (size_t)(y / 8) * (o.w / 8) + (x / 8);
                    const size_t in = (size_t)(y % 8) * 8 + (x % 8);
                    byte = (size_t)o.tile * 32 + t * tbytes + (o.bpp8 ? in : in / 2);
                }
                if (byte >= csize) continue;
                const uint8_t v = g[chars + byte];
                idx = o.bpp8 ? v : ((x % 2) ? (v >> 4) : (v & 15));
                if (!o.bpp8 && linear) idx = (((size_t)y * o.w + x) % 2) ? (v >> 4) : (v & 15);
                if (idx == 0) continue;
                const size_t ci = o.bpp8 ? (size_t)idx : (size_t)(o.pal * 16 + idx);
                if (ci >= ncol) continue;
                out.px[(size_t)(o.y - y0 + y) * out.w + (o.x - x0 + x)] =
                    ds_rgba((uint16_t)u16(c, 0x28 + ci * 2));
            }
    }
    return out;
}

// ── Model textures (transports and siege, categories 5 and 6) ─────────────────
// docs/51: every .nsbmd has one TEX0 texture and one palette. The texture
// dictionary's first entry is texImageParam (format bits 26-28, size bits
// 20-22/23-25, data offset bits 0-15 << 3); the palette dictionary's is the
// palette offset << 3. Measured: format 3 (16 colours) at 32x32. Formats 1-4,
// 6 and 7 are decoded; 5 (4x4 compressed) is not used by these models.
Pic decode_model_texture(const std::string& asset) {
    Pic out;
    const std::vector<uint8_t> d = rom_file(asset + ".nsbmd");
    if (d.size() < 0x14 || std::memcmp(d.data(), "BMD0", 4) != 0) return out;
    auto u16 = [&](size_t a) { return a + 2 <= d.size() ? (uint32_t)(d[a] | (d[a + 1] << 8)) : 0u; };
    auto u32 = [&](size_t a) { return u16(a) | (u16(a + 2) << 16); };
    const uint32_t nblk = u16(0x0E);
    size_t t0 = 0;
    for (uint32_t i = 0; i < nblk && i < 8; ++i) {
        const uint32_t o = u32(0x10 + 4 * i);
        if (o + 4 <= d.size() && std::memcmp(d.data() + o, "TEX0", 4) == 0) t0 = o;
    }
    if (!t0) return out;
    const size_t b = t0 + 8;
    const size_t tex_dict = t0 + u16(b + 6), tex_data = t0 + u32(b + 12);
    const size_t pal_dict = t0 + u32(b + 0x2C), pal_data = t0 + u32(b + 0x30);
    auto first_entry = [&](size_t dict) -> size_t {
        if (dict + 8 > d.size() || d[dict + 1] == 0) return 0;
        return dict + u16(dict + 6) + 4;   // the entry header, then the entries
    };
    const size_t te = first_entry(tex_dict), pe = first_entry(pal_dict);
    if (!te || !pe) return out;
    const uint32_t p = u32(te);
    const int fmt = (int)((p >> 26) & 7), w = 8 << ((p >> 20) & 7), h = 8 << ((p >> 23) & 7);
    const bool col0_clear = ((p >> 29) & 1u) != 0;
    const size_t img = tex_data + ((size_t)(p & 0xFFFF) << 3);
    const size_t pal = pal_data + ((size_t)u16(pe) << 3);
    if (fmt == 0 || fmt == 5 || w > 1024 || h > 1024) return out;
    out.w = w;
    out.h = h;
    out.px.assign((size_t)w * h, 0u);
    auto colour = [&](size_t i) { return pal + i * 2 + 2 <= d.size() ? ds_rgba((uint16_t)u16(pal + i * 2)) : 0u; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            uint32_t c = 0;
            int a = 31;
            switch (fmt) {
                case 2: { const size_t at = img + i / 4; if (at < d.size()) { const int v = (d[at] >> ((i % 4) * 2)) & 3; c = (v || !col0_clear) ? colour((size_t)v) : 0; } break; }
                case 3: { const size_t at = img + i / 2; if (at < d.size()) { const int v = (i & 1) ? d[at] >> 4 : d[at] & 15; c = (v || !col0_clear) ? colour((size_t)v) : 0; } break; }
                case 4: { const size_t at = img + i; if (at < d.size()) { const int v = d[at]; c = (v || !col0_clear) ? colour((size_t)v) : 0; } break; }
                case 1: { const size_t at = img + i; if (at < d.size()) { c = colour(d[at] & 31); a = (d[at] >> 5) * 31 / 7; } break; }
                case 6: { const size_t at = img + i; if (at < d.size()) { c = colour(d[at] & 7); a = d[at] >> 3; } break; }
                case 7: { const size_t at = img + i * 2; if (at + 2 <= d.size()) { const uint32_t v = u16(at); c = (v & 0x8000u) ? ds_rgba((uint16_t)v) : 0; } break; }
                default: break;
            }
            if (c && a < 31) c = (c & 0x00FFFFFFu) | ((uint32_t)(a * 255 / 31) << 24);
            out.px[i] = c;
        }
    return out;
}

// ── Sounds (lb_sfx.cpp) ──────────────────────────────────────────────────────
extern "C" int lb_sfx_export_begin(void);
extern "C" void lb_sfx_export_end(void);
extern "C" const char* lb_sfx_arc_name(int arc);
extern "C" int lb_sfx_arc_by_name(const char* name);
extern "C" int lb_sfx_arc_seqs(int arc);
extern "C" const char* lb_sfx_seq_name(int arc, int seq);
extern "C" int lb_sfx_render(int arc, int seq, int16_t* out, int cap);
extern "C" int lb_teams_head_rgba(int bp, unsigned int out[576]);       // lb_teams.cpp
extern "C" int lb_teams_hud_icon_rgba(int bp, unsigned int out[576]);   // lb_teams.cpp

// The sequence archive a record's sounds are in, or -1. Records 0..119 are the
// six factions' 20 roles in sound_data.sadl's own order (SEQARC__KING_HEROM =
// 2 ... __KING_TRANSPORT = 11, __KING_BASE = 62 ... __KING_SHIPYARD = 71, ten
// per faction); the rest are found by name: "SpacePoliceCaptain" ->
// SEQARC__SPACEPOLICEHERO, a siege unit -> ...SIEGE, a building -> ...BASE.
int sound_arc(const Rec& r) {
    if (r.index < 120) {
        const int f = r.index / 20, role = r.index % 20;
        return role < 10 ? 2 + f * 10 + role : 62 + f * 10 + (role - 10);
    }
    std::string n;
    for (char ch : r.internal)
        if (std::isalnum((unsigned char)ch)) n += (char)std::toupper((unsigned char)ch);
    // Names that do not follow the pattern (sound_data.sadl spells the beach
    // "WHIPWRECK"; the halls and temples are their faction's BASE).
    static const struct { const char* prefix; const char* arc; } kAlias[] = {
        {"BRIDGE", "BRIDGE"}, {"GATE", "GATE"}, {"SHIPWRECKBEACH", "WHIPWRECKBEACH"},
        {"DWARVENHALL", "DWARFBASE"}, {"TROLLHALL", "TROLLBASE"}, {"ISLANDERTEMPLE", "ISLANDERBASE"},
        {"NINJATEMPLE", "NINJABASE"}, {"WARJUNK", "NINJASIEGE"}};
    for (const auto& k : kAlias)
        if (n.compare(0, std::strlen(k.prefix), k.prefix) == 0)
            return lb_sfx_arc_by_name((std::string("SEQARC__") + k.arc).c_str());
    int a = lb_sfx_arc_by_name(("SEQARC__" + n).c_str());
    if (a >= 0) return a;
    static const char* const kSuffix[] = {"CAPTAIN", "LEADER", "KING", "CHIEF", "SHIP", "BATTLEWHEEL",
                                          "WARCANOE", "MINEDEFENDER", "COLOR"};
    std::string stem = n;
    for (const char* s : kSuffix) {
        const size_t l = std::strlen(s);
        if (stem.size() > l && stem.compare(stem.size() - l, l, s) == 0) { stem.resize(stem.size() - l); break; }
    }
    const char* role = r.category == 0 ? "HERO" : r.category == 6 ? "SIEGE" : r.category >= 7 ? "BASE" : "";
    for (const std::string& c : {stem + role, stem}) {
        a = lb_sfx_arc_by_name(("SEQARC__" + c).c_str());
        if (a >= 0) return a;
    }
    return -1;
}

extern "C" void lb_sfx_play_pcm(const int16_t* pcm, int n);            // lb_sfx.cpp

// A record's sounds for the preview, rendered once. The SDAT stays loaded
// while the page is used (63 MB; lb_sfx_export_begin is a no-op when loaded).
struct SoundSet {
    std::vector<std::string> names;   // "ONSELECT", "ONSTRIKE", ...
    std::vector<std::vector<int16_t>> pcm;
};
std::map<int, SoundSet> g_snd;
const SoundSet& sounds_for(const Rec& r);

std::vector<uint8_t> wav_bytes(const int16_t* pcm, int n, int rate) {
    std::vector<uint8_t> o;
    auto u32le = [&](uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); };
    auto u16le = [&](uint16_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); };
    const uint32_t data = (uint32_t)n * 2;
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); u32le(36 + data);
    o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); u32le(16);
    u16le(1); u16le(1); u32le((uint32_t)rate); u32le((uint32_t)rate * 2); u16le(2); u16le(16);
    o.insert(o.end(), {'d', 'a', 't', 'a'}); u32le(data);
    for (int i = 0; i < n; ++i) u16le((uint16_t)pcm[i]);
    return o;
}

const SoundSet& sounds_for(const Rec& r) {
    auto it = g_snd.find(r.index);
    if (it != g_snd.end()) return it->second;
    SoundSet s;
    if (lb_sfx_export_begin()) {
        const int arc = sound_arc(r);
        std::vector<int16_t> buf(32000 * 8);
        for (int q = 0, n = arc >= 0 ? lb_sfx_arc_seqs(arc) : 0; q < n; ++q) {
            const int got = lb_sfx_render(arc, q, buf.data(), (int)buf.size());
            if (got <= 0) continue;
            std::string nm = lb_sfx_seq_name(arc, q);
            const size_t us = nm.rfind('_');
            if (us != std::string::npos && nm.size() > us + 3 && nm.compare(us + 1, 2, "ON") == 0)
                nm = nm.substr(us + 3);   // SE_KING_HEROM_ONSELECT -> SELECT
            for (size_t i = 1; i < nm.size(); ++i) nm[i] = (char)std::tolower((unsigned char)nm[i]);
            s.names.push_back(nm.empty() ? "sound " + std::to_string(q) : nm);
            s.pcm.emplace_back(buf.begin(), buf.begin() + got);
        }
    }
    return g_snd.emplace(r.index, std::move(s)).first->second;
}

// ── Export ───────────────────────────────────────────────────────────────────
// The record's known fields (lb_balance.cpp's kUnitFields, docs/11/106/426).
struct Field { const char* key; int off, width; };
const Field kFields[] = {
    {"cost", 0x5E, 2}, {"build_time", 0x60, 2}, {"health", 0x62, 2}, {"mana", 0x64, 2},
    {"speed", 0x0C, 4}, {"melee_damage", 0x68, 2}, {"melee_spread", 0x6A, 2},
    {"attack_interval", 0x6D, 1}, {"attack_range", 0x6F, 1}, {"resource_yield", 0x6C, 1},
    {"projectile", 0x66, 2}, {"spell_1", 0x72, 1}, {"spell_2", 0x73, 1}, {"spell_3", 0x74, 1},
    {"spell_4", 0x75, 1}, {"spell_5", 0x76, 1}, {"land", 0x17, 1}, {"water", 0x18, 1},
    {"trees", 0x19, 1}, {"building_collision", 0x1A, 1}, {"land_water", 0x1B, 1},
    {"hitbox_size", 0x1D, 1}, {"not_water_only", 0x16, 1}, {"ai_priority", 0x70, 1},
    {"defogged_area", 0x71, 1},
    // docs/463: the port's capability bytes (the record's padding, stock 0xA2).
    {"mountains", 0x79, 1}, {"hotwire_immune", 0x7A, 1}, {"emp_immune", 0x7B, 1},
    {"esp_immune", 0x22, 1}, {"construction_style", 0x23, 1},
    {"display_name_id", 0x06, 2},
};

// docs/463: the largest value of a capability field (1 for the on/off ones,
// 3 for the construction style), or 0 for any other field. A template's stock
// 0xA2 is neither, so it shows, and saves, as 0.
int port_field_max(const char* key) {
    if (!std::strcmp(key, "mountains")) return 1;
    // SG-0073 (docs/470): 0 normal, 1 immune, 2 always affected.
    if (!std::strcmp(key, "hotwire_immune") || !std::strcmp(key, "emp_immune") ||
        !std::strcmp(key, "esp_immune"))
        return 2;
    return !std::strcmp(key, "construction_style") ? 3 : 0;
}
long fold_port_value(const char* key, long v) {
    const int mx = port_field_max(key);
    return mx && (v < 0 || v > mx) ? 0 : v;
}
const char* port_field_tip(const char* key) {
    if (!std::strcmp(key, "mountains"))
        return "Can walk over mountains and cliffs (terrain class 5), which no unit can in the\n"
               "stock game. Give it Land as well. Online, every player has the same record.";
    if (!std::strcmp(key, "hotwire_immune"))
        return "Normal: the stock rule (Hotwire takes an enemy transport or special).\n"
               "Immune: the Hotwire spell cannot take this unit.\n"
               "Always: Hotwire can take it whatever its kind.";
    if (!std::strcmp(key, "emp_immune"))
        return "Normal: the stock rule (EMP stuns specials).\n"
               "Immune: the EMP spell skips this unit.\n"
               "Always: EMP stuns it whatever its kind.";
    if (!std::strcmp(key, "esp_immune"))
        return "Normal: the stock rule (ESP stuns heroes, builders, foot and mounted units).\n"
               "Immune: the ESP spell skips this unit.\n"
               "Always: ESP stuns it whatever its kind.";
    return "The brick animation shown while this unit is trained.\n"
           "Auto: its faction's (a custom unit is Human). Skeleton is the Wizards', Alien the Aliens'.";
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

std::string safe(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    return o.empty() ? std::string("unit") : o;
}

const char* const kDirNames[5] = {"d0", "d1", "d2", "d3", "d4"};

// ── The how-to guide ─────────────────────────────────────────────────────────
// Written as HOW-TO.txt into mods/units and into every exported unit folder
// (so a new unit's folder has it too), and shown step by step in the tab.
struct HowStep { const char* title; const char* body; };
const HowStep kHowTo[] = {
    {"Pick a starting unit",
     "In Balance > Custom units, choose a unit or building in the list at the top. Your\n"
     "unit starts as a copy of it: its role (hero, builder, melee, ranged, mounted,\n"
     "transport, siege or building), its stats, art, sounds, head and cards. Use\n"
     "Action, Direction and Team to look at its animations."},
    {"Make the custom unit",
     "Type a name (1-24 letters, digits, spaces or _-'.!?) and press Make a custom\n"
     "unit from this. The game writes the folder mods/units/<name> and ticks it in\n"
     "My custom units. Press Open folder to see it. At most 9 custom units can be\n"
     "loaded at once."},
    {"Change its stats and name",
     "Use the Stats tab, or open unit.json in a text editor. \"name\" is what the\n"
     "menus list it by; \"display_name\" (optional) is what the game shows in battle,\n"
     "and need not be unique (\"Builder\"). Under \"fields\" change cost, build_time,\n"
     "health, speed, damage and the rest; values are kept within what the game can\n"
     "store. The Stats tab names the projectile and spells, and has tick boxes for\n"
     "mountains (walk over cliffs) and immunity to Hotwire, EMP and ESP, and a\n"
     "construction style for the brick animation when it is trained.\n"
     "\"index\" is the unit it was made from:\n"
     "leave it. The other entries (sheets, animations, raw_record) are for reference\n"
     "and are not read back."},
    {"Pick another model or sprite set (optional)",
     "\"asset\" names the art it starts from. A transport or siege unit uses a 3D\n"
     "model: set it to another stock model, e.g. \"Models/E_Clawtank\", to start from\n"
     "that one. A sprite unit can name another unit's sprite base of the same role,\n"
     "e.g. \"Sprites/k_mel\". The game's log says whether it was used."},
    {"Redraw the sprites",
     "Edit the PNGs in frames/<action>/ (d0 facing away ... d4 facing the camera,\n"
     "then the frame number), or whole sheets in sheets/. Keep each image's size.\n"
     "The PNGs use the faction palette: index 0 is transparent and indexes 12-14\n"
     "(shown red) become the owner's team colour in play. A full-colour PNG also\n"
     "works; each pixel takes the nearest palette colour. A frame drawn in frames/\n"
     "is laid over the sheet."},
    {"Re-skin a model (transports and siege)",
     "Edit model/texture.png and keep its size. The model's shape stays the stock\n"
     "one."},
    {"Portraits",
     "portraits/head.png is the small round head on the army screen, the Team\n"
     "Builder and the top screen (any size, fitted to 24x24). portraits/card.png and\n"
     "card_top_screen.png are the big cards: keep their size, or they are ignored and\n"
     "the starting unit's cards are used."},
    {"Sounds",
     "Put WAV files in sounds/ named for when they play: SELECT, DEATH, STRIKE,\n"
     "HIT or SPECIAL (a name ending in one of these also counts, e.g.\n"
     "MyUnit_OnDeath.wav). Any sample rate, mono or stereo, up to 6 seconds. Roles\n"
     "with no WAV keep the starting unit's sounds."},
    {"Load and test it",
     "Back in the game press Load new units now (the game returns to the title\n"
     "screen), or restart. Under My custom units, Preview shows your art; the line\n"
     "after Last build says what was loaded or why not. Your unit then appears on\n"
     "the army screen with the units of its role, and in the Team Builder."},
    {"Share it (optional)",
     "Type your name and press Share online. A moderator approves it before it\n"
     "appears in Community units, where others can subscribe to it. In an online\n"
     "lobby the host's custom units are sent to everyone who joins."},
};

std::string how_to_text() {
    std::string t = "HOW TO MAKE A CUSTOM UNIT (LEGO Battles PC)\n"
                    "===========================================\n\n"
                    "This folder holds one unit's files. The game writes this guide; it is also\n"
                    "in Balance > Custom units > How to make a custom unit.\n\n";
    int n = 1;
    for (const HowStep& s : kHowTo) {
        t += std::to_string(n++) + ". " + s.title + "\n";
        t += s.body;
        t += "\n\n";
    }
    t += "Files in a unit folder\n"
         "  unit.json                  name, stats, starting unit, asset\n"
         "  sheets/                    whole sprite sheets (.png or .NCBR)\n"
         "  frames/<action>/d<D>_<N>.png  single frames\n"
         "  model/texture.png          a 3D unit's texture\n"
         "  portraits/                 head.png, card.png, card_top_screen.png\n"
         "  sounds/                    WAVs by role\n"
         "Anything missing is taken from the starting unit.\n";
#ifdef _WIN32
    std::string crlf;
    for (char c : t) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
#else
    return t;
#endif
}

// Returns the number of files written, or -1.
int export_one(const Rec& r, const fs::path& root) {
    std::error_code ec;
    const fs::path dir = root / safe(r.internal);
    fs::create_directories(dir / "sheets", ec);
    int files = 0;
    const Layout L = layout_of(r);
    auto rd = [&](int off, int w) {
        uint32_t v = 0;
        for (int i = 0; i < w; ++i) v |= (uint32_t)r.raw[(size_t)(off + i)] << (8 * i);
        return v;
    };
    std::string j = "{\n";
    j += "  \"format\": \"lb-unit 1\",\n";
    j += "  \"index\": " + std::to_string(r.index) + ",\n";
    j += "  \"name\": " + json_str(r.name) + ",\n";
    j += "  \"internal_name\": " + json_str(r.internal) + ",\n";
    j += "  \"category\": " + std::to_string(r.category) + ",\n";
    j += "  \"category_name\": " + json_str(category_name(r.category)) + ",\n";
    j += "  \"asset\": " + json_str(r.asset) + ",\n";
    if (r.category >= 7)
        j += "  \"building_cell\": {\"tile\": " + std::to_string(r.cell) + ", \"w\": " + std::to_string(r.cw) +
             ", \"h\": " + std::to_string(r.ch) + "},\n";
    j += "  \"fields\": {\n";
    for (size_t i = 0; i < sizeof kFields / sizeof kFields[0]; ++i)
        j += "    " + json_str(kFields[i].key) + ": " +
             std::to_string(fold_port_value(kFields[i].key, (long)rd(kFields[i].off, kFields[i].width))) +
             (i + 1 < sizeof kFields / sizeof kFields[0] ? ",\n" : "\n");
    j += "  },\n";
    j += "  \"sheets\": [";
    for (size_t i = 0; i < L.sheets.size(); ++i) j += (i ? ", " : "") + json_str(L.sheets[i]);
    j += "],\n  \"animations\": {";
    for (size_t a = 0; a < L.actions.size(); ++a) {
        j += std::string(a ? "," : "") + "\n    " + json_str(L.actions[a].name) + ": {";
        for (int d = 0; d < 5; ++d) {
            j += std::string(d ? ", " : "") + json_str(kDirNames[d]) + ": [";
            const auto& fr = L.actions[a].dirs[(size_t)d];
            for (size_t f = 0; f < fr.size(); ++f) {
                char b[96];
                std::snprintf(b, sizeof b, "%s{\"sheet\": \"%s\", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}",
                              f ? ", " : "", fr[f].sheet.c_str(), fr[f].x, fr[f].y, fr[f].w, fr[f].h);
                j += b;
            }
            j += "]";
        }
        j += "}";
    }
    j += "\n  },\n";
    if (!L.note.empty()) j += "  \"note\": " + json_str(L.note) + ",\n";
    j += "  \"raw_record\": \"";
    for (uint8_t b : r.raw) { char h[3]; std::snprintf(h, sizeof h, "%02x", b); j += h; }
    j += "\"\n}\n";
    if (!write_file(dir / "unit.json", std::vector<uint8_t>(j.begin(), j.end()))) return -1;
    ++files;
    {
        const std::string h = how_to_text();
        if (write_file(dir / "HOW-TO.txt", std::vector<uint8_t>(h.begin(), h.end()))) ++files;
    }
    for (const std::string& sh : L.sheets) {
        const Sheet& s = sheet(sh);
        std::vector<uint8_t> png;
        if (write_file(dir / "sheets" / (sh + ".NCBR"), s.file)) ++files;
        if (indexed_png(s, 0, 0, s.w, s.h, &png) && write_file(dir / "sheets" / (sh + ".png"), png)) ++files;
    }
    for (const Action& a : L.actions) {
        const fs::path ad = dir / "frames" / a.name;
        fs::create_directories(ad, ec);
        for (int d = 0; d < 5; ++d)
            for (size_t f = 0; f < a.dirs[(size_t)d].size(); ++f) {
                const Frame& fr = a.dirs[(size_t)d][f];
                std::vector<uint8_t> png;
                char nm[32];
                std::snprintf(nm, sizeof nm, "%s_%zu.png", kDirNames[d], f);
                if (indexed_png(sheet(fr.sheet), fr.x, fr.y, fr.w, fr.h, &png) && write_file(ad / nm, png)) ++files;
            }
    }
    // Models: the texture as a PNG (re-skin it), the model and its animation.
    if (r.asset.compare(0, 7, "Models/") == 0) {
        fs::create_directories(dir / "model", ec);
        const Pic t = decode_model_texture(r.asset);
        std::vector<uint8_t> png;
        if (t.w > 0 && rgba_png(t.w, t.h, t.px.data(), &png) && write_file(dir / "model" / "texture.png", png)) ++files;
        const std::string leaf = r.asset.substr(7);
        for (const char* ext : {".nsbmd", ".nsbca"}) {
            const std::vector<uint8_t> m = rom_file(r.asset + ext);
            if (!m.empty() && write_file(dir / "model" / (leaf + ext), m)) ++files;
        }
    }
    // Portraits: the two cards, the army-screen head and the battle HUD icon.
    fs::create_directories(dir / "portraits", ec);
    const struct { const char* folder; const char* file; } kCards[2] = {
        {"UI/FEPlayerCards/", "card.png"}, {"UI/GamePlayerCards/", "card_top_screen.png"}};
    for (const auto& k : kCards) {
        const Pic p = decode_cell(k.folder + r.internal);
        std::vector<uint8_t> png;
        if (p.w > 0 && rgba_png(p.w, p.h, p.px.data(), &png) && write_file(dir / "portraits" / k.file, png)) ++files;
        for (const char* ext : {".NCGR", ".NCLR", ".NCER", ".NANR"}) {
            const std::vector<uint8_t> b = rom_file(k.folder + r.internal + ext);
            if (!b.empty() && write_file(dir / "portraits" / (std::string(k.file).substr(0, std::strlen(k.file) - 4) + ext), b))
                ++files;
        }
    }
    unsigned int icon[576];
    std::vector<uint8_t> png;
    if (lb_teams_head_rgba(r.index, icon) && rgba_png(24, 24, icon, &png) && write_file(dir / "portraits" / "head.png", png))
        ++files;
    if (lb_teams_hud_icon_rgba(r.index, icon) && rgba_png(24, 24, icon, &png) &&
        write_file(dir / "portraits" / "hud_icon.png", png))
        ++files;
    // Sounds: every sequence of the record's archive, as 32 kHz mono WAV.
    const int arc = sound_arc(r);
    if (arc >= 0) {
        fs::create_directories(dir / "sounds", ec);
        const std::string an = lb_sfx_arc_name(arc);
        std::vector<int16_t> pcm(32000 * 8);
        for (int s = 0, n = lb_sfx_arc_seqs(arc); s < n; ++s) {
            std::string sn = lb_sfx_seq_name(arc, s);
            if (sn.empty()) sn = "seq" + std::to_string(s);
            const int got = lb_sfx_render(arc, s, pcm.data(), (int)pcm.size());
            if (got > 0 && write_file(dir / "sounds" / (safe(sn) + ".wav"), wav_bytes(pcm.data(), got, 32000))) ++files;
        }
        std::string sj = "{\"archive\": " + std::to_string(arc) + ", \"name\": " + json_str(an) + "}\n";
        if (write_file(dir / "sounds" / "sounds.json", std::vector<uint8_t>(sj.begin(), sj.end()))) ++files;
    }
    return files;
}

// ── Preview ──────────────────────────────────────────────────────────────────
struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; };
std::map<std::string, Tex> g_tex;   // "<sheet>#<team>"
SDL_Renderer* g_ren = nullptr;

const Tex& sheet_tex(const std::string& name, int team) {
    team = team_key(team);
    const std::string key = name + "#" + std::to_string(team);
    auto it = g_tex.find(key);
    if (it != g_tex.end()) return it->second;
    Tex t;
    const Sheet& s = sheet(name);
    if (!s.px.empty() && g_ren) {
        const std::array<uint32_t, 16> pal = palette(team);
        std::vector<uint32_t> px(s.px.size());
        for (size_t i = 0; i < px.size(); ++i) px[i] = pal[s.px[i]];
        t.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, s.w, s.h);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, px.data(), s.w * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.t, SDL_ScaleModeNearest);
        }
        t.w = s.w;
        t.h = s.h;
    }
    return g_tex.emplace(key, t).first->second;
}

// A decoded picture's texture, made once per key.
template <class F>
const Tex& pic_tex(const std::string& key, F make) {
    auto it = g_tex.find(key);
    if (it != g_tex.end()) return it->second;
    Tex t;
    const Pic p = make();
    if (p.w > 0 && p.h > 0 && g_ren) {
        t.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, p.w, p.h);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, p.px.data(), p.w * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.t, SDL_ScaleModeNearest);
        }
        t.w = p.w;
        t.h = p.h;
    }
    return g_tex.emplace(key, t).first->second;
}

// Eight compass directions from the five stored ones: 0..4 as stored, 5..7
// mirror 3..1.
const char* const kCompass[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

struct View {
    int pick = 0;   // record index (a custom unit: its template)
    char filter[32] = "";
    std::string folder;   // a custom unit's folder in mods/units, or empty
    std::string title;    // its name
};

// A custom unit's sheet as the importer will build it (lb_cunits.cpp).
const Tex& folder_tex(const std::string& folder, int tmpl, const std::string& name, int team) {
    team = team_key(team);
    const std::string key = "F:" + folder + ":" + name + "#" + std::to_string(team);
    auto it = g_tex.find(key);
    if (it != g_tex.end()) return it->second;
    Tex t;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (g_ren && lb_cunits_folder_sheet(folder, tmpl, name, &w, &h, &px)) {
        const std::array<uint32_t, 16> pal = palette(team);
        std::vector<uint32_t> rgba(px.size());
        for (size_t i = 0; i < px.size(); ++i) rgba[i] = pal[px[i] & 15];
        t.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, rgba.data(), w * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.t, SDL_ScaleModeNearest);
        }
        t.w = w;
        t.h = h;
    }
    return g_tex.emplace(key, t).first->second;
}
struct State {
    View a, b;
    bool compare = false;
    int action = 1;      // 0 idle, 1 walk, 2 attack (whichever exist)
    int dir = 4;
    int team = 1;
    bool playing = true;
    float fps = 8.0f, zoom = 4.0f;
    double t0 = 0.0;
    std::string message;
    bool bad = false;
};
State g_st;

bool pick_combo(const char* id, View& v) {
    const std::vector<Rec>& rs = records();
    if (rs.empty()) return false;
    v.pick = std::clamp(v.pick, 0, (int)rs.size() - 1);
    char label[96];
    std::snprintf(label, sizeof label, "%s (%s)", v.folder.empty() ? rs[(size_t)v.pick].name.c_str() : v.title.c_str(),
                  category_name(rs[(size_t)v.pick].category));
    bool changed = false;
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo(id, label, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##f", "Filter", v.filter, sizeof v.filter);
        std::string q = v.filter;
        for (char& c : q) c = (char)std::tolower((unsigned char)c);
        for (const Rec& r : rs) {
            std::string hay = r.name + " " + r.internal + " " + category_name(r.category);
            for (char& c : hay) c = (char)std::tolower((unsigned char)c);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            char row[128];
            std::snprintf(row, sizeof row, "%s  [%s, %s]##%d", r.name.c_str(), category_name(r.category),
                          r.internal.c_str(), r.index);
            if (ImGui::Selectable(row, r.index == v.pick && v.folder.empty())) {
                v.pick = r.index;
                v.folder.clear();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// One button per sound of the record (sounds_for), played on the
// notification device.
void draw_sounds(const Rec& r) {
    const SoundSet& ss = sounds_for(r);
    if (ss.names.empty()) {
        ImGui::TextDisabled("No sounds.");
        return;
    }
    ImGui::TextDisabled("Sounds:");
    for (size_t i = 0; i < ss.names.size(); ++i) {
        ImGui::SameLine();
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(ss.names[i].c_str()))
            lb_sfx_play_pcm(ss.pcm[i].data(), (int)ss.pcm[i].size());
        ImGui::PopID();
    }
}

std::string read_text(const fs::path& p);                       // below
std::string json_get_s(const std::string& j, const char* key);  // below
fs::path units_root();                                          // below

void draw_view(const View& v, int frame_tick) {
    const std::vector<Rec>& rs = records();
    if (rs.empty()) return;
    const Rec& r = rs[(size_t)v.pick];
    const Layout L = layout_of(r);
    ImGui::BeginGroup();
    // The page names the unit above its tabs; side by side, each view says which it is.
    if (g_st.compare) {
        if (v.folder.empty()) {
            ImGui::Text("%s", r.name.c_str());
            ImGui::TextDisabled("#%d %s, %s, %s", r.index, r.internal.c_str(), category_name(r.category), r.asset.c_str());
        } else {
            ImGui::Text("%s", v.title.c_str());
            ImGui::TextDisabled("custom, from %s (%s), mods/units/%s", r.name.c_str(), category_name(r.category), v.folder.c_str());
        }
    }
    if (L.actions.empty()) {
        // A 3D unit: the model itself, turning while Play is on (else at the
        // Direction), in the Team's colour, and its texture beside it.
        std::string asset = r.asset;
        if (!v.folder.empty()) {   // a custom unit's own choice (unit.json "asset")
            const std::string a = json_get_s(read_text(units_root() / fs::u8path(v.folder) / "unit.json"), "asset");
            if (a.compare(0, 7, "Models/") == 0) asset = a;
        }
        if (asset.compare(0, 7, "Models/") == 0) {
            static std::string held_name;
            static std::vector<uint8_t> held;
            if (held_name != asset) {
                held = rom_file(asset + ".nsbmd");
                held_name = asset;
            }
            constexpr int kM = 256;
            static SDL_Texture* mtex = nullptr;
            static SDL_Renderer* mren = nullptr;
            if (mren != g_ren) {
                if (mtex) SDL_DestroyTexture(mtex);
                mtex = g_ren ? SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, kM, kM) : nullptr;
                if (mtex) SDL_SetTextureBlendMode(mtex, SDL_BLENDMODE_BLEND);
                mren = g_ren;
            }
            static const int kBank[6] = {1, 0, 2, 3, 4, 6};   // kTeamNames order -> palette bank (docs/412)
            const float yaw = g_st.playing ? (float)ImGui::GetTime() * 0.9f : (float)(g_st.dir & 7) * 0.785398f;
            static std::vector<uint32_t> px(kM * kM);
            const bool teal = g_st.team >= 6;   // SG-0064: bank 5 rebuilt in the teal shades
            if (mtex && !held.empty() &&
                (teal ? lb_nsbmd_render_teal(held.data(), held.size(), team_shades(team_key(g_st.team)), yaw, 0.45f, kM, kM,
                                             px.data())
                      : lb_nsbmd_render(held.data(), held.size(), kBank[g_st.team % 6], yaw, 0.45f, kM, kM, px.data()))) {
                SDL_UpdateTexture(mtex, nullptr, px.data(), kM * 4);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float side = 64.0f * std::max(2.0f, g_st.zoom);
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(52, 92, 52, 255), 4.0f);
                ImGui::Image((ImTextureID)(intptr_t)mtex, ImVec2(side, side));
                ImGui::SameLine();
            }
        }
        ImGui::BeginGroup();
        if (asset == r.asset) ImGui::TextWrapped("%s", L.note.c_str());
        else ImGui::TextWrapped("A 3D model (%s, chosen in unit.json): its texture is shown for re-skinning.", asset.c_str());
        const Tex mt = pic_tex("model:" + asset, [&] { return decode_model_texture(asset); });
        if (mt.t) ImGui::Image((ImTextureID)(intptr_t)mt.t, ImVec2(mt.w * 2.0f, mt.h * 2.0f));
        ImGui::EndGroup();
        draw_sounds(r);
        ImGui::EndGroup();
        return;
    }
    const Action& a = L.actions[(size_t)std::min(g_st.action, (int)L.actions.size() - 1)];
    const int d8 = g_st.dir & 7;
    const bool mirror = d8 > 4;
    const int d5 = mirror ? 8 - d8 : d8;
    const std::vector<Frame>& fr = a.dirs[(size_t)d5];
    const float z = g_st.zoom;
    const ImVec2 box(32.0f * z + 16.0f, 32.0f * z + 16.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), IM_COL32(52, 92, 52, 255), 4.0f);
    if (!fr.empty()) {
        const Frame& f = fr[(size_t)frame_tick % fr.size()];
        const Tex& t = v.folder.empty() ? sheet_tex(f.sheet, g_st.team) : folder_tex(v.folder, r.index, f.sheet, g_st.team);
        if (t.t) {
            float u0 = (float)f.x / t.w, u1 = (float)(f.x + f.w) / t.w;
            if (mirror) std::swap(u0, u1);
            const float v0 = (float)f.y / t.h, v1 = (float)(f.y + f.h) / t.h;
            const ImVec2 c(p.x + box.x * 0.5f, p.y + box.y * 0.5f);
            // A building cell (up to 64x64 and more) fits the box instead of spilling out of it.
            const float fz = std::min(z, (box.x - 16.0f) / (float)std::max(f.w, f.h));
            const ImVec2 h(f.w * fz * 0.5f, f.h * fz * 0.5f);
            dl->AddImage((ImTextureID)(intptr_t)t.t, ImVec2(c.x - h.x, c.y - h.y), ImVec2(c.x + h.x, c.y + h.y),
                         ImVec2(u0, v0), ImVec2(u1, v1));
        }
    }
    ImGui::Dummy(box);
    // The card and the head beside it, at the zoom's half (a card is 128x192).
    const Tex card = pic_tex("card:" + std::to_string(r.index), [&] { return decode_cell("UI/FEPlayerCards/" + r.internal); });
    const Tex head = pic_tex("head:" + std::to_string(r.index), [&] {
        Pic p;
        unsigned int px[576];
        if (lb_teams_head_rgba(r.index, px)) { p.w = p.h = 24; p.px.assign(px, px + 576); }
        return p;
    });
    if (card.t) {
        ImGui::SameLine();
        const float s = std::max(1.0f, z * 0.5f);
        ImGui::Image((ImTextureID)(intptr_t)card.t, ImVec2(card.w * s * 0.5f, card.h * s * 0.5f));
    }
    if (head.t) {
        ImGui::SameLine();
        ImGui::Image((ImTextureID)(intptr_t)head.t, ImVec2(24.0f * z * 0.5f, 24.0f * z * 0.5f));
    }
    ImGui::TextDisabled("%s, %s (%s%s), %zu frame%s", a.name.c_str(), kCompass[d8], kDirNames[d5],
                        mirror ? " mirrored" : "", fr.size(), fr.size() == 1 ? "" : "s");
    if (!L.note.empty()) ImGui::TextDisabled("%s", L.note.c_str());
    draw_sounds(r);
    ImGui::EndGroup();
}

// ── My custom units (mods/units) ─────────────────────────────────────────────
// A folder per custom unit, the shape the export writes; enabled.txt names the
// ones the cartridge is built with (lb_cunits.cpp). "Load new units now"
// rebuilds the cartridge, which returns the game to the title screen.
extern "C" int lb_mods_request_rebuild(void);        // lb_mods.cpp
extern "C" int lb_cunits_count(void);                 // lb_cunits.cpp
extern "C" const char* lb_cunits_folder(int k);
extern "C" const char* lb_cunits_report(void);

struct Mine { std::string folder, name; int tmpl = -1; bool ok = false; std::string why; };

fs::path units_root() { return fs::u8path(lb_mods_dir()) / "units"; }

std::vector<std::string> read_enabled() {
    std::vector<std::string> v;
    std::FILE* f = std::fopen(utf8(units_root() / "enabled.txt").c_str(), "rb");
    if (!f) return v;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) v.push_back(l);
    }
    std::fclose(f);
    return v;
}
void write_enabled(const std::vector<std::string>& v) {
    std::error_code ec;
    fs::create_directories(units_root(), ec);
    std::string o;
    for (const std::string& x : v) o += x + "\n";
    write_file(units_root() / "enabled.txt", std::vector<uint8_t>(o.begin(), o.end()));
}

// unit.json's template index and name (what the importer reads).
Mine read_mine(const fs::path& dir) {
    Mine m;
    m.folder = utf8(dir.filename());
    std::FILE* f = std::fopen(utf8(dir / "unit.json").c_str(), "rb");
    if (!f) { m.why = "no unit.json"; return m; }
    std::string j;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && j.size() < (1u << 20)) j.append(buf, n);
    std::fclose(f);
    const size_t ki = j.find("\"index\"");
    if (ki != std::string::npos) m.tmpl = std::atoi(j.c_str() + j.find(':', ki) + 1);
    const size_t kn = j.find("\"name\"");
    if (kn != std::string::npos) {
        const size_t q = j.find('"', j.find(':', kn) + 1);
        const size_t e = q == std::string::npos ? q : j.find('"', q + 1);
        if (e != std::string::npos) m.name = j.substr(q + 1, std::min<size_t>(e - q - 1, 40));
    }
    if (m.tmpl < 0 || m.tmpl >= 182) m.why = "unit.json has no template \"index\"";
    else m.ok = true;
    if (m.name.empty()) m.name = m.folder;
    return m;
}

std::string safe_name(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum((unsigned char)c) || c == '_') o += c;
    return o.substr(0, 20);
}

// ── Editing a unit in the game ───────────────────────────────────────────────
// unit.json is edited in place: only the values set here are rewritten, the
// rest of the file stays as the player or the export wrote it.
std::string read_text(const fs::path& p) {
    std::string j;
    std::FILE* f = std::fopen(utf8(p).c_str(), "rb");
    if (!f) return j;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && j.size() < (1u << 20)) j.append(buf, n);
    std::fclose(f);
    return j;
}

// The value after "key": in [from, to): its start and end, or false.
bool json_value_at(const std::string& j, size_t from, size_t to, const char* key, size_t* vb, size_t* ve) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t p = j.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    size_t c = j.find(':', p + k.size());
    if (c == std::string::npos || c >= to) return false;
    size_t b = c + 1;
    while (b < j.size() && (j[b] == ' ' || j[b] == '\t' || j[b] == '\r' || j[b] == '\n')) ++b;
    size_t e = b;
    if (b < j.size() && j[b] == '"') {
        for (e = b + 1; e < j.size() && j[e] != '"'; ++e)
            if (j[e] == '\\') ++e;
        ++e;
    } else {
        while (e < j.size() && (std::isdigit((unsigned char)j[e]) || j[e] == '-' || j[e] == '.')) ++e;
    }
    *vb = b;
    *ve = e;
    return e > b;
}

std::string json_get_s(const std::string& j, const char* key) {
    size_t b, e;
    if (!json_value_at(j, 0, j.size(), key, &b, &e) || j[b] != '"') return std::string();
    return j.substr(b + 1, e - b - 2);
}

// Sets a top-level string (added after "index" when missing).
void json_set_s(std::string& j, const char* key, const std::string& v) {
    size_t b, e;
    const size_t fb = j.find("\"fields\"");
    if (json_value_at(j, 0, fb == std::string::npos ? j.size() : fb, key, &b, &e)) {
        j.replace(b, e - b, json_str(v));
        return;
    }
    const size_t ib = j.find("\"index\"");
    const size_t nl = ib == std::string::npos ? std::string::npos : j.find('\n', ib);
    if (nl != std::string::npos) j.insert(nl + 1, "  \"" + std::string(key) + "\": " + json_str(v) + ",\n");
}

// SG-0070: the credits line, who else worked on a unit ("sprites by Ryncat").
// Kept to the server's allow-list (src/units.js CREDITS_OK): letters, digits,
// space and _ - ' . , & ! ?, at most 60. Anything else is dropped.
std::string credits_clean(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (o.size() >= 60) break;
        if (std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '\'' || c == '.' || c == ',' || c == '&' ||
            c == '!' || c == '?')
            o += (char)c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// The "fields" object's span.
bool fields_span(const std::string& j, size_t* b, size_t* e) {
    const size_t f = j.find("\"fields\"");
    if (f == std::string::npos) return false;
    *b = j.find('{', f);
    *e = *b == std::string::npos ? std::string::npos : j.find('}', *b);
    return *e != std::string::npos;
}

struct Edit {
    std::string folder;
    char name[25] = "";
    char ingame[25] = "";       // SG-0053: "display_name", the name the game shows
    char credits[61] = "";      // SG-0070: "credits", the collaborators
    std::string asset;
    long field[sizeof kFields / sizeof kFields[0]] = {};
    bool has[sizeof kFields / sizeof kFields[0]] = {};
    std::string msg;
};
Edit g_edit;

void edit_open(const Mine& m, const Rec& tmpl) {
    Edit e;
    e.folder = m.folder;
    const std::string j = read_text(units_root() / fs::u8path(m.folder) / "unit.json");
    std::snprintf(e.name, sizeof e.name, "%s", json_get_s(j, "name").c_str());
    std::snprintf(e.ingame, sizeof e.ingame, "%s", json_get_s(j, "display_name").c_str());
    std::snprintf(e.credits, sizeof e.credits, "%s", credits_clean(json_get_s(j, "credits")).c_str());
    e.asset = json_get_s(j, "asset");
    if (e.asset.empty()) e.asset = tmpl.asset;
    size_t fb, fe;
    const bool f = fields_span(j, &fb, &fe);
    for (size_t i = 0; i < sizeof kFields / sizeof kFields[0]; ++i) {
        uint32_t v = 0;   // the template's value unless unit.json sets it
        for (int k = 0; k < kFields[i].width; ++k) v |= (uint32_t)tmpl.raw[(size_t)(kFields[i].off + k)] << (8 * k);
        e.field[i] = fold_port_value(kFields[i].key, (long)v);
        size_t b, en;
        if (f && json_value_at(j, fb, fe, kFields[i].key, &b, &en)) {
            e.field[i] = fold_port_value(kFields[i].key, std::atol(j.c_str() + b));
            e.has[i] = true;
        }
    }
    g_edit = e;
}

bool edit_save() {
    const fs::path p = units_root() / fs::u8path(g_edit.folder) / "unit.json";
    std::string j = read_text(p);
    if (j.empty()) return false;
    json_set_s(j, "name", g_edit.name);
    if (g_edit.ingame[0] || !json_get_s(j, "display_name").empty()) json_set_s(j, "display_name", g_edit.ingame);
    if (g_edit.credits[0] || !json_get_s(j, "credits").empty())
        json_set_s(j, "credits", credits_clean(g_edit.credits));
    json_set_s(j, "asset", g_edit.asset);
    size_t fb, fe;
    if (!fields_span(j, &fb, &fe)) {
        const size_t last = j.rfind('}');
        if (last == std::string::npos) return false;
        size_t q = last;
        while (q > 0 && (j[q - 1] == ' ' || j[q - 1] == '\n' || j[q - 1] == '\r')) --q;
        j.insert(q, ",\n  \"fields\": {\n  }");
        if (!fields_span(j, &fb, &fe)) return false;
    }
    for (size_t i = 0; i < sizeof kFields / sizeof kFields[0]; ++i) {
        // The name's text id is the game's to set for a custom unit; kept only if the file had it.
        if (std::strcmp(kFields[i].key, "display_name_id") == 0 && !g_edit.has[i]) continue;
        fields_span(j, &fb, &fe);
        size_t b, en;
        const std::string v = std::to_string(g_edit.field[i]);
        if (json_value_at(j, fb, fe, kFields[i].key, &b, &en)) {
            j.replace(b, en - b, v);
        } else {
            // A new line at the object's end; a comma after the last value.
            size_t q = fe;
            while (q > fb && (j[q - 1] == ' ' || j[q - 1] == '\n' || j[q - 1] == '\r')) --q;
            const bool first = j[q - 1] == '{';
            j.insert(q, std::string(first ? "" : ",") + "\n    " + json_str(kFields[i].key) + ": " + v);
        }
    }
    return write_file(p, std::vector<uint8_t>(j.begin(), j.end()));
}

// The stock assets a unit made from `t` can start from: models for a model
// template, sprite sets of the same role for a sprite one.
std::vector<std::pair<std::string, std::string>> asset_choices(const std::vector<Rec>& rs, const Rec& t) {
    std::vector<std::pair<std::string, std::string>> v;   // asset, label
    const bool model = t.asset.compare(0, 7, "Models/") == 0;
    for (const Rec& r : rs) {
        const bool rm = r.asset.compare(0, 7, "Models/") == 0;
        if (rm != model || r.asset.empty()) continue;
        if (!model && (r.category != t.category || r.asset.compare(0, 8, "Sprites/") != 0)) continue;
        if (std::any_of(v.begin(), v.end(), [&](const auto& x) { return x.first == r.asset; })) continue;
        v.push_back({r.asset, r.name + "  (" + r.asset + ")"});
    }
    return v;
}

// What the folder holds against what the unit can use (the build reads the
// same names; a sheet named for another unit counts by its role suffix).
void draw_files(const Mine& m, const Rec& t) {
    const fs::path dir = units_root() / fs::u8path(m.folder);
    std::error_code ec;
    auto row = [&](const std::string& what, bool there, const char* note) {
        ImGui::TextColored(there ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%s %s", there ? "[x]" : "[ ]",
                           what.c_str());
        if (note && *note) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", note);
        }
    };
    const Layout L = layout_of(t);
    const std::string tbase = t.asset.size() > 8 ? t.asset.substr(8) : t.asset;
    for (const std::string& sh : L.sheets) {
        // In sheets/ or, the community's way, beside unit.json.
        const bool own = fs::exists(dir / "sheets" / (sh + ".png"), ec) || fs::exists(dir / "sheets" / (sh + ".NCBR"), ec) ||
                         fs::exists(dir / (sh + ".png"), ec) || fs::exists(dir / (sh + ".NCBR"), ec);
        row("sheets/" + sh + " (.png or .NCBR)", own, own ? "" : "missing: the starting unit's is used");
    }
    if (t.asset.compare(0, 7, "Models/") == 0)
        row("model/texture.png", fs::exists(dir / "model" / "texture.png", ec), "a 3D unit's re-skin");
    row("portraits/head.png", fs::exists(dir / "portraits" / "head.png", ec), "the round head");
    row("portraits/card.png", fs::exists(dir / "portraits" / "card.png", ec), "the army screen's card");
    row("portraits/card_top_screen.png", fs::exists(dir / "portraits" / "card_top_screen.png", ec), "the battle card");
    int wavs = 0;
    for (const auto& de : fs::directory_iterator(dir / "sounds", ec))
        if (de.path().extension() == ".wav") ++wavs;
    row("sounds/*.wav (" + std::to_string(wavs) + ")", wavs > 0, "SELECT, DEATH, STRIKE, HIT, SPECIAL");
    ImGui::TextDisabled("Sheets named for another unit (e.g. %s_w0 -> any_w0) are matched by their ending;\n"
                        "Last build below says what was used and what was skipped.", tbase.c_str());
}

// SG-0052: the projectile (+0x66) is a blueprint index, 182..208 (the 27
// projectile records after the units, docs/11), or 65535 for none (every
// melee unit and building). Labelled with the record's name and the stock
// units that fire it.
const std::vector<std::pair<int, std::string>>& projectile_choices(const std::vector<Rec>& rs) {
    static std::vector<std::pair<int, std::string>> v;
    if (!v.empty()) return v;
    v.push_back({65535, "None (melee)"});
    const std::vector<uint8_t> e = rom_file("BP/Entities.ebp");
    if (e.size() < kPool) return v;
    for (int j = 0; j < 27; ++j) {
        const size_t r = 4 + (size_t)kUnits * 0x7C + (size_t)j * 0x74;
        const uint32_t off = (uint32_t)(e[r] | (e[r + 1] << 8) | (e[r + 2] << 16) | ((uint32_t)e[r + 3] << 24));
        std::string nm;
        for (size_t p = kPool + off; p < e.size() && e[p] && nm.size() < 32; ++p) nm += (char)e[p];
        std::string users;
        int n = 0;
        for (const Rec& u : rs) {
            if ((u.raw[0x66] | (u.raw[0x67] << 8)) != kUnits + j) continue;
            if (n++ < 3) users += (users.empty() ? "" : ", ") + u.name;
        }
        if (n > 3) users += ", ...";
        v.push_back({kUnits + j, nm + (users.empty() ? "  (no stock unit)" : "  (" + users + ")")});
    }
    return v;
}
std::string projectile_label(int value, const std::vector<Rec>& rs) {
    for (const auto& c : projectile_choices(rs))
        if (c.first == value) return c.second;
    return std::to_string(value);
}
std::string spell_label(int id) { return id == 0 ? "None" : lb_spell_label(id); }

// The fields chosen from a list rather than typed (SG-0052). True when drawn.
bool draw_named_field(size_t i, const std::vector<Rec>& rs) {
    const char* key = kFields[i].key;
    const bool proj = std::strcmp(key, "projectile") == 0;
    const bool spell = std::strncmp(key, "spell_", 6) == 0;
    // docs/463: the capability fields are a tick box, or a short list.
    if (const int pmax = port_field_max(key)) {
        if (pmax == 1) {
            bool on = g_edit.field[i] == 1;
            if (ImGui::Checkbox(key, &on)) g_edit.field[i] = on ? 1 : 0;
        } else if (pmax == 2) {   // SG-0073: Normal / Immune / Always
            static const char* const kTri[3] = {"Normal", "Immune", "Always"};
            const int cur = (int)std::clamp<long>(g_edit.field[i], 0, 2);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo(key, kTri[cur])) {
                for (int s = 0; s < 3; ++s)
                    if (ImGui::Selectable(kTri[s], s == cur)) g_edit.field[i] = s;
                ImGui::EndCombo();
            }
        } else {
            static const char* const kStyle[4] = {"Auto", "Human", "Skeleton", "Alien"};
            const int cur = (int)std::clamp<long>(g_edit.field[i], 0, 3);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo(key, kStyle[cur])) {
                for (int s = 0; s < 4; ++s)
                    if (ImGui::Selectable(kStyle[s], s == cur)) g_edit.field[i] = s;
                ImGui::EndCombo();
            }
        }
        ImGui::SetItemTooltip("%s", port_field_tip(key));
        return true;
    }
    if (!proj && !spell) return false;
    const int cur = (int)g_edit.field[i];
    ImGui::SetNextItemWidth(proj ? 300.0f : 180.0f);
    const std::string shown = proj ? projectile_label(cur, rs) : spell_label(cur);
    if (ImGui::BeginCombo(key, shown.c_str())) {
        if (proj) {
            for (const auto& c : projectile_choices(rs))
                if (ImGui::Selectable(c.second.c_str(), c.first == cur)) g_edit.field[i] = c.first;
        } else {
            for (int id = 0; id <= 0x20; ++id) {
                if (!lb_spell_listed(id) && id != cur) continue;
                ImGui::PushID(id);   // ids 1 and 2 are both "Unit Heal"
                if (ImGui::Selectable(spell_label(id).c_str(), id == cur)) g_edit.field[i] = id;
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(proj ? "What it fires. None for a melee unit."
                               : "A hero's spells, in the order its yellow brick lists them.\n"
                                 "Other units do not cast.");
    return true;
}

// The Stats tab of a custom unit: its name, the stock art it starts from and
// every known value, saved into unit.json by the Save button above the tabs.
void draw_edit_fields(const Mine& m, const std::vector<Rec>& rs) {
    const Rec& t = rs[(size_t)m.tmpl];
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Name", g_edit.name, sizeof g_edit.name);
    ImGui::SetItemTooltip("What the menus list it by (the Team Builder, this page, Community units).");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("In-game name", g_edit.name[0] ? g_edit.name : "the Name", g_edit.ingame,
                             sizeof g_edit.ingame);
    ImGui::SetItemTooltip("What the game itself shows in battle and on its cards. Empty: the Name.\n"
                          "It need not be unique: call it \"Builder\" like the stock builders.\n"
                          "Letters, digits, spaces and - ' . ! only, at most 24.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("Credits", "e.g. sprites by a friend", g_edit.credits, sizeof g_edit.credits);
    ImGui::SetItemTooltip("Who helped make this unit. Shown with the unit under Community units\n"
                          "and sent when you share it. Letters, digits, spaces and _ - ' . , & ! ?,\n"
                          "at most 60, no swearing.");
    const auto choices = asset_choices(rs, t);
    std::string cur = g_edit.asset;
    for (const auto& c : choices)
        if (c.first == g_edit.asset) cur = c.second;
    ImGui::SetNextItemWidth(380.0f);
    if (ImGui::BeginCombo(t.asset.compare(0, 7, "Models/") == 0 ? "Model" : "Sprite set", cur.c_str())) {
        for (const auto& c : choices)
            if (ImGui::Selectable(c.second.c_str(), c.first == g_edit.asset)) g_edit.asset = c.first;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The stock art it starts from. Your files in the folder are drawn over it.");
    ImGui::SeparatorText("Values");
    if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < sizeof kFields / sizeof kFields[0]; ++i) {
            if (std::strcmp(kFields[i].key, "display_name_id") == 0) continue;
            ImGui::TableNextColumn();
            if (draw_named_field(i, rs)) continue;
            const long mx = kFields[i].width == 1 ? 255L : kFields[i].width == 2 ? 65535L : 0x7FFFFFFFL;
            int v = (int)std::min<long>(g_edit.field[i], 0x7FFFFFFFL);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputInt(kFields[i].key, &v)) g_edit.field[i] = std::clamp<long>(v, 0, mx);
        }
        ImGui::EndTable();
    }
}

// A stock unit's values, to read (a custom unit made from it starts with them).
void draw_stock_fields(const Rec& r) {
    ImGui::TextDisabled("The starting unit's values. Make a custom unit from it to change them.");
    if (ImGui::BeginTable("##stockstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
        for (size_t i = 0; i < sizeof kFields / sizeof kFields[0]; ++i) {
            if (std::strcmp(kFields[i].key, "display_name_id") == 0) continue;
            uint32_t v = 0;
            for (int k = 0; k < kFields[i].width; ++k) v |= (uint32_t)r.raw[(size_t)(kFields[i].off + k)] << (8 * k);
            v = (uint32_t)fold_port_value(kFields[i].key, (long)v);   // docs/463: stock padding reads 0
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kFields[i].key);
            ImGui::TableNextColumn();
            if (std::strcmp(kFields[i].key, "projectile") == 0)
                ImGui::TextUnformatted(projectile_label((int)v, records()).c_str());
            else if (std::strncmp(kFields[i].key, "spell_", 6) == 0)
                ImGui::TextUnformatted(spell_label((int)v).c_str());
            else
                ImGui::Text("%u", (unsigned)v);
        }
        ImGui::EndTable();
    }
}

void start_share(const Mine& m, const std::vector<Rec>& rs);   // below
bool share_busy();
extern char g_author[25];

// The guide in the tab: one step at a time, with the file beside it.
void draw_how_to() {
    if (!ImGui::CollapsingHeader("How to make a custom unit")) return;
    static int step = 0;
    const int n = (int)(sizeof kHowTo / sizeof kHowTo[0]);
    step = std::clamp(step, 0, n - 1);
    ImGui::Text("Step %d of %d: %s", step + 1, n, kHowTo[step].title);
    ImGui::Indent();
    ImGui::TextUnformatted(kHowTo[step].body);
    ImGui::Unindent();
    ImGui::BeginDisabled(step == 0);
    if (ImGui::Button("< Back")) --step;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(step == n - 1);
    if (ImGui::Button("Next >")) ++step;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open HOW-TO.txt")) {
        std::error_code ec;
        fs::create_directories(units_root(), ec);
        const fs::path p = units_root() / "HOW-TO.txt";
        const std::string h = how_to_text();
        write_file(p, std::vector<uint8_t>(h.begin(), h.end()));
        std::string url = "file:///" + utf8(p);
        for (char& c : url) if (c == '\\') c = '/';
        SDL_OpenURL(url.c_str());
    }
    ImGui::SetItemTooltip("The same guide as a text file (mods/units/HOW-TO.txt). Every unit folder\n"
                          "the game writes has a copy.");
    ImGui::Spacing();
}

// ── The page: a list of units on the left, the one picked on the right ─────
bool is_community(const std::string& f) {
    return f.size() > 1 && f[0] == 'c' && std::isdigit((unsigned char)f[1]);
}

std::vector<Mine> list_mine() {
    std::vector<Mine> mine;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(units_root(), ec)) {
        if (!e.is_directory(ec) || e.path().filename() == "export" || e.path().filename() == "_received") continue;
        mine.push_back(read_mine(e.path()));
    }
    std::sort(mine.begin(), mine.end(), [](const Mine& a, const Mine& b) { return a.folder < b.folder; });
    return mine;
}

void drop_folder_textures(const std::string& folder) {   // empty: every custom unit's
    const std::string pre = folder.empty() ? std::string("F:") : "F:" + folder + ":";
    for (auto it = g_tex.begin(); it != g_tex.end();) {
        if (it->first.compare(0, pre.size(), pre) == 0) {
            if (it->second.t) SDL_DestroyTexture(it->second.t);
            it = g_tex.erase(it);
        } else {
            ++it;
        }
    }
}

void select_stock(int idx) {
    g_st.a.pick = idx;
    g_st.a.folder.clear();
    g_st.a.title.clear();
    g_edit = Edit();
}

void select_custom(const Mine& m, const std::vector<Rec>& rs) {
    g_st.a.pick = m.ok ? m.tmpl : g_st.a.pick;
    g_st.a.folder = m.folder;
    g_st.a.title = m.name;
    if (m.ok && g_edit.folder != m.folder) edit_open(m, rs[(size_t)m.tmpl]);
}

// A new custom unit from stock unit `r`: its export copied into mods/units/<name>,
// renamed and turned on. The folder name, or empty (s.message says why).
std::string make_custom(const Rec& r, const char* name, const fs::path& export_root) {
    State& s = g_st;
    const std::string folder = safe_name(name);
    std::error_code ec;
    const fs::path dst = units_root() / fs::u8path(folder);
    if (fs::exists(dst, ec)) {
        s.bad = true;
        s.message = "mods/units/" + folder + " already exists.";
        return std::string();
    }
    if (export_one(r, export_root) < 0) {
        s.bad = true;
        s.message = "Could not export the starting unit.";
        return std::string();
    }
    fs::copy(export_root / safe(r.internal), dst, fs::copy_options::recursive, ec);
    if (ec) {
        s.bad = true;
        s.message = "Could not copy the export.";
        return std::string();
    }
    std::string j = read_text(dst / "unit.json");
    json_set_s(j, "name", name);
    write_file(dst / "unit.json", std::vector<uint8_t>(j.begin(), j.end()));
    std::vector<std::string> en = read_enabled();
    if (std::find(en.begin(), en.end(), folder) == en.end()) en.push_back(folder);
    write_enabled(en);
    s.bad = false;
    s.message = "Made mods/units/" + folder + ". Change it in the tabs on the right, then Load new units now.";
    return folder;
}

void draw_lab_list(const std::vector<Rec>& rs, const std::vector<Mine>& mine) {
    State& s = g_st;
    static char q[32] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##labq", "Search units", q, sizeof q);
    std::string ql = q;
    for (char& c : ql) c = (char)std::tolower((unsigned char)c);
    auto hit = [&](std::string hay) {
        for (char& c : hay) c = (char)std::tolower((unsigned char)c);
        return ql.empty() || hay.find(ql) != std::string::npos;
    };
    ImGui::SeparatorText("My custom units");
    std::vector<std::string> en = read_enabled();
    int shown = 0;
    for (const Mine& m : mine) {
        if (!hit(m.name + " " + m.folder)) continue;
        ++shown;
        ImGui::PushID(m.folder.c_str());
        bool on = std::find(en.begin(), en.end(), m.folder) != en.end();
        if (ImGui::Checkbox("##on", &on)) {
            if (on) en.push_back(m.folder);
            else en.erase(std::remove(en.begin(), en.end(), m.folder), en.end());
            write_enabled(en);
        }
        ImGui::SetItemTooltip("Build the game with this unit (after Load new units now).");
        ImGui::SameLine();
        bool built = false;
        for (int k = 0; k < lb_cunits_count(); ++k) built = built || m.folder == lb_cunits_folder(k);
        std::string label = m.name + (m.ok ? "" : "  (!)");
        if (ImGui::Selectable(label.c_str(), s.a.folder == m.folder)) select_custom(m, rs);
        if (ImGui::IsItemHovered()) {
            if (!m.ok) ImGui::SetTooltip("%s", m.why.c_str());
            else ImGui::SetTooltip("From %s%s; %s", rs[(size_t)m.tmpl].name.c_str(),
                                   is_community(m.folder) ? ", from Community units" : "",
                                   built ? "in the game" : "not loaded yet");
        }
        ImGui::PopID();
    }
    if (mine.empty()) ImGui::TextDisabled("None yet: pick a unit below,\nthen Make a custom unit.");
    else if (!shown) ImGui::TextDisabled("None match.");
    ImGui::SeparatorText("Starting units");
    static const int kOrder[8] = {0, 1, 2, 3, 4, 5, 6, 7};   // category_name: 7+ are buildings
    for (int c : kOrder) {
        std::vector<const Rec*> v;
        for (const Rec& r : rs) {
            const int rc = r.category >= 7 ? 7 : r.category;
            if (rc == c && hit(r.name + " " + r.internal)) v.push_back(&r);
        }
        if (v.empty()) continue;
        const char* title = c == 7 ? "Buildings" : category_name(c);
        if (!ql.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::TreeNode(title)) {
            for (const Rec* r : v) {
                char row[96];
                std::snprintf(row, sizeof row, "%s##%d", r->name.c_str(), r->index);
                if (ImGui::Selectable(row, s.a.folder.empty() && s.a.pick == r->index)) select_stock(r->index);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %s", r->internal.c_str(), r->asset.c_str());
            }
            ImGui::TreePop();
        }
    }
}

void draw_lab_detail(const std::vector<Rec>& rs, const std::vector<Mine>& mine, const fs::path& export_root) {
    State& s = g_st;
    s.a.pick = std::clamp(s.a.pick, 0, (int)rs.size() - 1);
    const Rec& r = rs[(size_t)s.a.pick];
    const Mine* m = nullptr;
    for (const Mine& x : mine)
        if (!s.a.folder.empty() && x.folder == s.a.folder) m = &x;
    if (!s.a.folder.empty() && !m) select_stock(s.a.pick);   // its folder went away

    // The unit and what can be done with it.
    if (m) {
        ImGui::Text("%s", m->name.c_str());
        ImGui::TextDisabled("Custom, from %s (%s): mods/units/%s", r.name.c_str(), category_name(r.category),
                            m->folder.c_str());
        if (m->ok) {
            if (ImGui::Button("Save"))
                g_edit.msg = edit_save() ? "Saved. Press Load new units now to use it." : "Could not write unit.json.";
            ImGui::SetItemTooltip("Writes the Stats tab's name, art and values into unit.json.");
            ImGui::SameLine();
            if (ImGui::Button("Reload art")) drop_folder_textures(m->folder);
            ImGui::SetItemTooltip("Reads the folder's sheets and frames again after you edit them.");
            ImGui::SameLine();
        }
        if (ImGui::Button("Open this folder")) {
            std::string url = "file:///" + utf8(units_root() / fs::u8path(m->folder));
            for (char& c : url) if (c == '\\') c = '/';
            SDL_OpenURL(url.c_str());
        }
        if (m->ok && !is_community(m->folder)) {
            ImGui::SameLine();
            ImGui::BeginDisabled(share_busy());
            if (ImGui::Button("Share online")) start_share(*m, rs);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Uploads this unit to Community units. The moderator approves it first.\n"
                                  "Sends its art, unit.json and the WAVs in its sounds folder.");
        }
        if (!m->ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", m->why.c_str());
        if (!g_edit.msg.empty()) ImGui::TextDisabled("%s", g_edit.msg.c_str());
    } else {
        ImGui::Text("%s", r.name.c_str());
        ImGui::TextDisabled("#%d %s, %s, %s", r.index, r.internal.c_str(), category_name(r.category), r.asset.c_str());
        static char new_name[25] = "";
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##newname", "New unit's name", new_name, sizeof new_name);
        ImGui::SameLine();
        ImGui::BeginDisabled(safe_name(new_name).empty());
        if (ImGui::Button("Make a custom unit from this")) {
            const std::string folder = make_custom(r, new_name, export_root);
            if (!folder.empty()) {
                new_name[0] = 0;
                select_custom(read_mine(units_root() / fs::u8path(folder)), rs);
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Copies this unit into mods/units/<name> as a new unit and turns it on.\n"
                              "It keeps this unit's role, art, sounds and head until you change them.");
        ImGui::SameLine();
        if (ImGui::Button("Export")) {
            const bool snd = lb_sfx_export_begin() != 0;
            const int n = export_one(r, export_root);
            (void)snd;   // kept loaded for the sound preview (sounds_for)
            s.bad = n < 0;
            s.message = n < 0 ? "Could not write the export." :
                        "Wrote " + std::to_string(n) + " files to " + utf8(export_root / safe(r.internal));
        }
        ImGui::SetItemTooltip("Writes this unit's files to mods/units/export (sheets, frames, cards,\n"
                              "head, sounds, unit.json) to look at or start from.");
    }
    ImGui::Spacing();

    // The tabs.
    if (ImGui::BeginTabBar("##labtabs")) {
        if (ImGui::BeginTabItem("Look")) {
            const char* actions[3] = {"Idle", "Walk", "Attack"};
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Action", &s.action, actions, 3);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderInt("Direction", &s.dir, 0, 7, kCompass[s.dir & 7]);
            ImGui::SetItemTooltip("Five directions are drawn; SW, W and NW are SE, E and NE mirrored.\n"
                                  "The compass names are a reading of the art.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::Combo("Team", &s.team, kTeamNames, kTeams);
            ImGui::Checkbox("Play", &s.playing);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Frames per second", &s.fps, 1.0f, 20.0f, "%.0f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Zoom", &s.zoom, 1.0f, 8.0f, "%.0fx");
            ImGui::Checkbox("Compare with", &s.compare);
            if (s.compare) {
                ImGui::SameLine();
                pick_combo("##unit_b", s.b);
            }
            static int tick = 0;
            if (s.playing) tick = (int)(ImGui::GetTime() * s.fps);
            ImGui::Separator();
            draw_view(s.a, tick);
            if (s.compare) {
                ImGui::SameLine(0.0f, 40.0f);
                draw_view(s.b, tick);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stats")) {
            if (m && m->ok && g_edit.folder == m->folder) draw_edit_fields(*m, rs);
            else if (m && m->ok) edit_open(*m, rs[(size_t)m->tmpl]);
            else draw_stock_fields(r);
            ImGui::EndTabItem();
        }
        if (m && m->ok && ImGui::BeginTabItem("Files")) {
            draw_files(*m, rs[(size_t)m->tmpl]);
            const char* rep = lb_cunits_report();
            if (rep && rep[0]) {
                ImGui::SeparatorText("Last build");
                ImGui::TextDisabled("%s", rep);
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ── Community units (the master server's /units, docs/448 phase 4) ───────────
// Share online uploads a folder's bundle with a preview sheet, a thumbnail and
// its sounds; the moderator approves it in Discord as for levels. Subscribing
// writes a unit into mods/units/c<id>_<Name> and turns it on.
extern "C" int lb_net_http_get(const char* host, int port, const char* path, char* out, int cap);
extern "C" int lb_net_http_post(const char* host, int port, const char* path, const char* body, char* out,
                                int cap);
extern "C" const char* lb_setting_player_key(void);
extern "C" void lb_setting_master(char* host, int cap, int* port);
extern "C" int lb_swear_filter(const char* in, char* out, int cap);   // lb_chat.cpp

std::string b64_encode(const std::vector<uint8_t>& d) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < d.size() ? (uint32_t)d[i + 1] << 8 : 0) |
                           (i + 2 < d.size() ? d[i + 2] : 0);
        o += k[(v >> 18) & 63];
        o += k[(v >> 12) & 63];
        o += i + 1 < d.size() ? k[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? k[v & 63] : '=';
    }
    return o;
}
bool b64_decode(const std::string& s, std::vector<uint8_t>* out, size_t cap) {
    out->clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26
              : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : c == '=' ? -2 : -1;
        if (x == -2) break;
        if (x < 0) return false;
        v = (v << 6) | (uint32_t)x;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out->size() >= cap) return false;
            out->push_back((uint8_t)(v >> bits));
        }
    }
    return true;
}

// The server's replies are flat objects (and one array of them, "units"):
// keys to values, strings unescaped, other values as written.
using Flat = std::map<std::string, std::string>;
bool flat_string(const std::string& s, size_t& p, std::string* out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out->clear();
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            ++p;
            const char c = s[p];
            if (c == 'u' && p + 4 < s.size()) {
                const unsigned v = (unsigned)std::strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16);
                *out += v < 0x80 ? (char)v : '?';
                p += 4;
            } else {
                *out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
            }
        } else {
            *out += s[p];
        }
        ++p;
    }
    if (p >= s.size()) return false;
    ++p;
    return true;
}
void flat_ws(const std::string& s, size_t& p) {
    while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
}
// One object at p; a nested array or object value is skipped (its key maps to
// where it starts, as "@<offset>").
bool flat_obj(const std::string& s, size_t& p, Flat* out) {
    flat_ws(s, p);
    if (p >= s.size() || s[p] != '{') return false;
    ++p;
    for (;;) {
        flat_ws(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        std::string k, v;
        if (!flat_string(s, p, &k)) return false;
        flat_ws(s, p);
        if (p >= s.size() || s[p] != ':') return false;
        ++p;
        flat_ws(s, p);
        if (p < s.size() && s[p] == '"') {
            if (!flat_string(s, p, &v)) return false;
        } else if (p < s.size() && (s[p] == '[' || s[p] == '{')) {
            v = "@" + std::to_string(p);
            int depth = 0;
            bool str = false;
            for (; p < s.size(); ++p) {
                const char c = s[p];
                if (str) { if (c == '\\') ++p; else if (c == '"') str = false; continue; }
                if (c == '"') str = true;
                else if (c == '[' || c == '{') ++depth;
                else if ((c == ']' || c == '}') && --depth == 0) { ++p; break; }
            }
        } else {
            const size_t b = p;
            while (p < s.size() && s[p] != ',' && s[p] != '}' && !std::isspace((unsigned char)s[p])) ++p;
            v = s.substr(b, p - b);
        }
        (*out)[k] = v;
        flat_ws(s, p);
        if (p < s.size() && s[p] == ',') ++p;
    }
}
int flat_int(const Flat& f, const char* k) {
    auto it = f.find(k);
    return it == f.end() ? 0 : std::atoi(it->second.c_str());
}
std::string flat_str(const Flat& f, const char* k) {
    auto it = f.find(k);
    return it == f.end() ? std::string() : it->second;
}
// SG-0088: a list row's `created` (ms since the epoch; too big for atoi) as
// the local date, "" when absent or nonsense.
std::string upload_date(const std::string& ms_text) {
    const long long ms = std::strtoll(ms_text.c_str(), nullptr, 10);
    if (ms < 946684800000LL || ms > 4102444800000LL) return {};   // 2000..2100
    const std::time_t t = (std::time_t)(ms / 1000);
    char b[16] = "";
    if (const std::tm* lt = std::localtime(&t)) std::strftime(b, sizeof b, "%Y-%m-%d", lt);
    return b;
}

// One request at a time on a worker thread; the page polls the result.
struct UNet {
    std::mutex mu;
    std::atomic<bool> busy{false};
    bool done = false, failed = false;
    int kind = 0, arg = 0;   // kind: 1 upload, 2 list, 3 subscribe, 4 like
    std::string reply;
};
UNet g_un;
bool share_busy() { return g_un.busy; }
void unet_run(int kind, int arg, std::string method, std::string path, std::string body) {
    char host[128];
    int port = 80;
    lb_setting_master(host, sizeof host, &port);
    std::vector<char> buf(3u << 20);
    const int n = method == "POST" ? lb_net_http_post(host, port, path.c_str(), body.c_str(), buf.data(), (int)buf.size())
                                   : lb_net_http_get(host, port, path.c_str(), buf.data(), (int)buf.size());
    std::lock_guard<std::mutex> lk(g_un.mu);
    g_un.kind = kind;
    g_un.arg = arg;
    g_un.done = true;
    g_un.failed = n < 0;
    g_un.reply.assign(buf.data(), n < 0 ? std::strlen(buf.data()) : (size_t)n);
    g_un.busy = false;
}
bool unet_start(int kind, int arg, const char* method, const std::string& path, const std::string& body) {
    if (g_un.busy.exchange(true)) return false;
    std::thread(unet_run, kind, arg, std::string(method), path, body).detach();
    return true;
}

// Thumbnails: their own worker, one id after another.
struct Thumbs {
    std::mutex mu;
    std::vector<int> want;
    std::map<int, std::vector<uint8_t>> got;   // id -> PNG (empty: failed)
    std::atomic<bool> running{false};
};
Thumbs g_th;
void thumb_worker() {
    for (;;) {
        int id = 0;
        {
            std::lock_guard<std::mutex> lk(g_th.mu);
            if (g_th.want.empty()) { g_th.running = false; return; }
            id = g_th.want.back();
            g_th.want.pop_back();
        }
        char host[128];
        int port = 80;
        lb_setting_master(host, sizeof host, &port);
        std::vector<char> buf(256u << 10);
        const int n = lb_net_http_get(host, port, ("/units/thumb?id=" + std::to_string(id)).c_str(), buf.data(),
                                      (int)buf.size());
        std::vector<uint8_t> png;
        Flat f;
        size_t p = 0;
        if (n > 0) {
            const std::string r(buf.data(), (size_t)n);
            if (flat_obj(r, p, &f)) b64_decode(flat_str(f, "png"), &png, 64u << 10);
        }
        std::lock_guard<std::mutex> lk(g_th.mu);
        g_th.got[id] = std::move(png);
    }
}

// The server's name rule (src/units.js NAME_OK): 1..24 of letters, digits,
// _, space, - ' . ! ?, and no swearing.
bool share_name_ok(const std::string& s, bool empty_ok) {
    if (s.empty()) return empty_ok;
    if (s.size() > 24) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '\'' || c == '.' || c == '!' || c == '?'))
            return false;
    char tmp[64];
    return lb_swear_filter(s.c_str(), tmp, sizeof tmp) == 0;
}

bool slurp_file(const fs::path& p, std::vector<uint8_t>* out, size_t cap) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    if (ec || n > cap) return false;
    std::FILE* f = std::fopen(utf8(p).c_str(), "rb");
    if (!f) return false;
    out->resize((size_t)n);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

// ── Convert: .NCBR <-> .PNG (SG-0061) ───────────────────────────────────────
// The lab page's "Convert..." panel. An .NCBR (raw or COMP-wrapped, 4bpp) is
// written as an indexed PNG on an .NCLR's palette bank (or the faction palette
// when none is chosen); a PNG is written as an .NCBR the same way the importer
// reads one (lb_cunits.cpp: a 16-colour indexed PNG keeps its indices, any
// other picture takes the nearest palette colour).
extern "C" int lb_pick_art_file(char* buf, int cap, int saving, int kind);   // lb_filedlg.cpp

struct Conv {
    bool open = false;
    char ncbr[512] = "", nclr[512] = "", png[512] = "";
    int bank = 0;
    bool comp = true;
    std::string msg;
    bool bad = false;
};
Conv g_conv;

// The palette the conversion uses: the chosen NCLR's bank, else the faction's.
std::array<uint32_t, 16> convert_palette(std::string* note) {
    std::array<uint32_t, 16> p = palette(0);
    if (!g_conv.nclr[0]) {
        *note = "faction palette";
        return p;
    }
    std::vector<uint8_t> b;
    uint32_t c[16];
    if (slurp_file(fs::u8path(g_conv.nclr), &b, 1u << 20) && lb_cunits_nclr_decode(b, g_conv.bank, c)) {
        for (int i = 0; i < 16; ++i) p[(size_t)i] = c[i];
        *note = "the NCLR's bank " + std::to_string(g_conv.bank);
    } else {
        *note = "faction palette (the NCLR could not be read)";
    }
    return p;
}

// `from` with its extension changed to `ext` (with the dot), into `to`.
void swap_ext(const char* from, const char* ext, char* to, size_t cap) {
    fs::path p = fs::u8path(from);
    p.replace_extension(ext);
    std::snprintf(to, cap, "%s", utf8(p).c_str());
}

void draw_convert() {
    Conv& c = g_conv;
    ImGui::SeparatorText("Convert sprite files");
    ImGui::TextWrapped("Turn an .NCBR sheet into a PNG, or a PNG into an .NCBR. A palette (.NCLR) is optional: "
                       "without one the faction palette is used.");
    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("##cnclr", c.nclr, sizeof c.nclr);
    ImGui::SameLine();
    if (ImGui::Button("Palette (.NCLR)...")) lb_pick_art_file(c.nclr, (int)sizeof c.nclr, 0, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputInt("Bank", &c.bank, 0, 0);
    c.bank = std::max(0, std::min(15, c.bank));
    ImGui::SetItemTooltip("Which 16-colour bank of the NCLR to use (0 for the cartridge's sprite palettes).");

    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("##cncbr", c.ncbr, sizeof c.ncbr);
    ImGui::SameLine();
    if (ImGui::Button("Open .NCBR...")) lb_pick_art_file(c.ncbr, (int)sizeof c.ncbr, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button("Write PNG...") && c.ncbr[0]) {
        std::vector<uint8_t> b, idx, png;
        int w = 0, h = 0;
        std::string why, note;
        if (!slurp_file(fs::u8path(c.ncbr), &b, 4u << 20)) {
            c.bad = true;
            c.msg = "Could not read the .NCBR.";
        } else if (!lb_cunits_ncbr_decode(b, &w, &h, &idx, &why)) {
            c.bad = true;
            c.msg = "Could not convert: " + why + ".";
        } else {
            char out[512];
            swap_ext(c.ncbr, ".png", out, sizeof out);
            if (lb_pick_art_file(out, (int)sizeof out, 1, 2)) {
                Sheet sh;
                sh.w = w;
                sh.h = h;
                sh.px = std::move(idx);
                const std::array<uint32_t, 16> pal = convert_palette(&note);
                c.bad = !(indexed_png(sh, 0, 0, w, h, &png, &pal) && write_file(fs::u8path(out), png));
                c.msg = c.bad ? "Could not write the PNG."
                              : "Wrote " + std::to_string(w) + "x" + std::to_string(h) + " PNG (" + note + ") to " + out;
            }
        }
    }
    ImGui::SetItemTooltip("Choose where to save the PNG. Colour 0 is transparent.");

    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("##cpng", c.png, sizeof c.png);
    ImGui::SameLine();
    if (ImGui::Button("Open PNG...")) lb_pick_art_file(c.png, (int)sizeof c.png, 0, 2);
    ImGui::SameLine();
    if (ImGui::Button("Write .NCBR...") && c.png[0]) {
        std::vector<uint8_t> b, ncbr;
        std::string why, note;
        if (!slurp_file(fs::u8path(c.png), &b, 4u << 20)) {
            c.bad = true;
            c.msg = "Could not read the PNG.";
        } else {
            const std::array<uint32_t, 16> pal = convert_palette(&note);
            if (!lb_cunits_png_to_ncbr(b, pal.data(), c.comp, &ncbr, &why)) {
                c.bad = true;
                c.msg = "Could not convert: " + why + ".";
            } else {
                char out[512];
                swap_ext(c.png, ".NCBR", out, sizeof out);
                if (lb_pick_art_file(out, (int)sizeof out, 1, 0)) {
                    c.bad = !write_file(fs::u8path(out), ncbr);
                    c.msg = c.bad ? "Could not write the .NCBR."
                                  : "Wrote " + std::to_string(ncbr.size()) + " bytes (" + note + ") to " + out;
                }
            }
        }
    }
    ImGui::SetItemTooltip("A 16-colour indexed PNG (the lab's exports) keeps its colour numbers; any other picture\n"
                          "takes the nearest palette colour. The size is rounded up to whole 8-pixel tiles.");
    ImGui::SameLine();
    ImGui::Checkbox("COMP-wrapped", &c.comp);
    ImGui::SetItemTooltip("On: compressed and wrapped like the game's own files. Off: the raw file.");
    if (!c.msg.empty())
        ImGui::TextColored(c.bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", c.msg.c_str());
    ImGui::Separator();
}

// The preview sheet (every sheet of the unit stacked, as the importer builds
// it, in Red) and the thumbnail (its first idle frame facing the camera).
bool share_pictures(const Mine& m, const Rec& r, std::vector<uint8_t>* sheet_png, std::vector<uint8_t>* thumb_png) {
    const Layout L = layout_of(r);
    const std::array<uint32_t, 16> pal = palette(0);
    struct Got { int w, h; std::vector<uint8_t> px; };
    std::map<std::string, Got> got;
    int W = 0, H = 0;
    for (const std::string& name : L.sheets) {
        Got g{0, 0, {}};
        if (!lb_cunits_folder_sheet(m.folder, r.index, name, &g.w, &g.h, &g.px) || g.w <= 0) continue;
        W = std::max(W, g.w);
        H += g.h;
        got[name] = std::move(g);
    }
    if (W > 0 && W <= 4096 && H <= 4096) {
        std::vector<uint32_t> px((size_t)W * H, 0);
        int y0 = 0;
        for (const std::string& name : L.sheets) {
            auto it = got.find(name);
            if (it == got.end()) continue;
            const Got& g = it->second;
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    px[(size_t)(y0 + y) * W + x] = pal[g.px[(size_t)y * g.w + x] & 15];
            y0 += g.h;
        }
        if (!rgba_png(W, H, px.data(), sheet_png)) return false;
    } else {
        // A 3D unit: its model texture (the folder's, else the template's).
        if (!slurp_file(units_root() / fs::u8path(m.folder) / "model" / "texture.png", sheet_png, 768u << 10)) {
            const Pic p = decode_model_texture(r.asset);
            if (p.w <= 0 || !rgba_png(p.w, p.h, p.px.data(), sheet_png)) return false;
        }
    }
    // The thumbnail.
    if (!L.actions.empty() && !L.actions[0].dirs[4].empty()) {
        const Frame& f = L.actions[0].dirs[4][0];
        auto it = got.find(f.sheet);
        if (it != got.end() && f.w <= 256 && f.h <= 256) {
            const Got& g = it->second;
            std::vector<uint32_t> px((size_t)f.w * f.h, 0);
            for (int y = 0; y < f.h; ++y)
                for (int x = 0; x < f.w; ++x) {
                    const int X = f.x + x, Y = f.y + y;
                    if (X < g.w && Y < g.h) px[(size_t)y * f.w + x] = pal[g.px[(size_t)Y * g.w + X] & 15];
                }
            if (rgba_png(f.w, f.h, px.data(), thumb_png) && thumb_png->size() <= (64u << 10)) return true;
        }
    }
    unsigned int head[576];
    if (lb_teams_head_rgba(r.index, head)) {
        std::vector<uint32_t> px(head, head + 576);
        return rgba_png(24, 24, px.data(), thumb_png);
    }
    return false;
}

struct Com {
    struct Row { int id = 0, category = 0, tmpl = 0, sounds = 0, downloads = 0, likes = 0; std::string name, author, credits, date; };
    std::vector<Row> rows;
    bool listed = false;
    int sort = 0, category = -1;   // -1 all
    std::string message;
    bool bad = false;
    std::set<int> liked;
    bool liked_loaded = false;
    std::map<int, Tex> thumbs;      // id -> texture (t null: asked or failed)
    std::set<int> asked;
};
Com g_com;
char g_author[25] = "";   // the name shared units carry

fs::path liked_path() { return fs::u8path(lb_mods_dir()) / "cache" / "units_liked.txt"; }
void load_liked() {
    if (g_com.liked_loaded) return;
    g_com.liked_loaded = true;
    std::vector<uint8_t> b;
    if (!slurp_file(liked_path(), &b, 64u << 10)) return;
    std::string s(b.begin(), b.end());
    for (size_t p = 0; p < s.size();) {
        const int id = std::atoi(s.c_str() + p);
        if (id > 0) g_com.liked.insert(id);
        const size_t nl = s.find('\n', p);
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
}
void save_liked() {
    std::error_code ec;
    fs::create_directories(liked_path().parent_path(), ec);
    std::string o;
    for (int id : g_com.liked) o += std::to_string(id) + "\n";
    write_file(liked_path(), std::vector<uint8_t>(o.begin(), o.end()));
}

// Subscribed units: folders c<id>_<Name> in mods/units.
std::map<int, std::string> subscribed() {
    std::map<int, std::string> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(units_root(), ec)) {
        const std::string n = utf8(e.path().filename());
        if (!e.is_directory(ec) || n.size() < 3 || n[0] != 'c' || !std::isdigit((unsigned char)n[1])) continue;
        const size_t us = n.find('_');
        if (us == std::string::npos) continue;
        v[std::atoi(n.c_str() + 1)] = n;
    }
    return v;
}

void start_share(const Mine& m, const std::vector<Rec>& rs) {
    State& s = g_st;
    const Rec& r = rs[(size_t)m.tmpl];
    if (!share_name_ok(m.name, false)) {
        s.bad = true;
        s.message = "To share it, give the unit a name of 1-24 letters, digits, spaces or _-'.!? in unit.json (no swearing).";
        return;
    }
    if (!share_name_ok(g_author, true)) {
        s.bad = true;
        s.message = "Your name may use 1-24 letters, digits, spaces or _-'.!? (no swearing).";
        return;
    }
    if (r.category > 19) {
        s.bad = true;
        s.message = "Units made from this record cannot be shared.";
        return;
    }
    const std::string credits = credits_clean(json_get_s(read_text(units_root() / fs::u8path(m.folder) / "unit.json"), "credits"));
    if (!credits.empty()) {
        char tmp[128];
        if (lb_swear_filter(credits.c_str(), tmp, sizeof tmp) != 0) {
            s.bad = true;
            s.message = "The credits may not contain swearing.";
            return;
        }
    }
    std::vector<uint8_t> bundle, sheet, thumb;
    if (!lb_cunits_folder_bundle(m.folder, &bundle)) {
        s.bad = true;
        s.message = "This unit is over 1 MB and cannot be shared.";
        return;
    }
    if (!share_pictures(m, r, &sheet, &thumb) || sheet.size() > (768u << 10)) {
        s.bad = true;
        s.message = "Could not draw this unit's preview.";
        return;
    }
    // TEST AID: LB_UNITSHARE_DUMP=<dir> keeps the moderator's preview sheet and
    // the catalogue thumbnail as sent.
    if (const char* d = std::getenv("LB_UNITSHARE_DUMP")) {
        write_file(fs::u8path(d) / "sheet.png", sheet);
        write_file(fs::u8path(d) / "thumb.png", thumb);
    }
    std::string sounds;
    std::error_code ec;
    int ns = 0;
    std::vector<fs::path> wavs;
    for (const auto& e : fs::directory_iterator(units_root() / fs::u8path(m.folder) / "sounds", ec))
        if (e.path().extension() == ".wav") wavs.push_back(e.path());
    std::sort(wavs.begin(), wavs.end());
    for (const fs::path& w : wavs) {
        std::vector<uint8_t> b;
        std::string nm = utf8(w.stem());
        bool ok = !nm.empty() && nm.size() <= 40;
        for (unsigned char c : nm) ok = ok && (std::isalnum(c) || c == '_');
        if (!ok || ns >= 8 || !slurp_file(w, &b, 256u << 10)) continue;
        sounds += std::string(ns ? "," : "") + "{\"name\":\"" + nm + "\",\"wav\":\"" + b64_encode(b) + "\"}";
        ++ns;
    }
    const std::string body = std::string("{\"key\":\"") + lb_setting_player_key() + "\",\"name\":\"" + m.name +
                             "\",\"author\":\"" + g_author + "\",\"credits\":\"" + credits + "\",\"category\":" + std::to_string(r.category) +
                             ",\"template\":" + std::to_string(r.index) + ",\"bundle\":\"" + b64_encode(bundle) +
                             "\",\"sheet\":\"" + b64_encode(sheet) + "\",\"thumb\":\"" + b64_encode(thumb) +
                             "\",\"sounds\":[" + sounds + "]}";
    if (unet_start(1, 0, "POST", "/units/upload", body)) {
        s.bad = false;
        s.message = "Uploading " + m.name + "...";
    }
}

void com_refresh() {
    static const char* const kSorts[4] = {"new", "old", "downloads", "likes"};
    std::string path = std::string("/units/list?limit=100&sort=") + kSorts[g_com.sort & 3];
    if (g_com.category >= 0) path += "&category=" + std::to_string(g_com.category);
    unet_start(2, 0, "GET", path, "");
}

void unet_poll() {
    std::string reply;
    int kind = 0, arg = 0;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lk(g_un.mu);
        if (!g_un.done) return;
        g_un.done = false;
        reply.swap(g_un.reply);
        kind = g_un.kind;
        arg = g_un.arg;
        failed = g_un.failed;
    }
    Flat j;
    size_t p = 0;
    const bool parsed = !failed && flat_obj(reply, p, &j);
    const bool ok = parsed && flat_str(j, "ok") == "true";
    const std::string err = parsed ? flat_str(j, "error") : std::string();
    if (kind == 1) {
        State& s = g_st;
        s.bad = !ok;
        if (!parsed) s.message = "Upload failed: " + (failed ? reply : std::string("the server's reply was not understood"));
        else if (!ok) s.message = "Upload refused: " + err + ".";
        else if (flat_str(j, "status") == "approved") s.message = "Shared as community unit #" + flat_str(j, "id") + ".";
        else s.message = "Uploaded as unit #" + flat_str(j, "id") + ". It appears under Community units once the moderator approves it.";
        return;
    }
    Com& c = g_com;
    c.bad = !ok;
    if (!ok) {
        c.message = !parsed ? (failed ? "Could not reach the master server: " + reply : std::string("The server's reply was not understood."))
                            : (err == "levels not configured" ? std::string("Community units need the main master server.")
                                                              : "The server said: " + err);
        return;
    }
    if (kind == 2) {
        c.rows.clear();
        const std::string at = flat_str(j, "units");
        size_t q = at.size() > 1 && at[0] == '@' ? (size_t)std::atoll(at.c_str() + 1) : std::string::npos;
        if (q != std::string::npos && q < reply.size() && reply[q] == '[') {
            ++q;
            for (;;) {
                flat_ws(reply, q);
                Flat u;
                if (q >= reply.size() || reply[q] != '{' || !flat_obj(reply, q, &u)) break;
                Com::Row r;
                r.id = flat_int(u, "id");
                r.category = flat_int(u, "category");
                r.tmpl = flat_int(u, "template");
                r.sounds = flat_int(u, "sounds");
                r.downloads = flat_int(u, "downloads");
                r.likes = flat_int(u, "likes");
                r.name = flat_str(u, "name").substr(0, 24);
                r.author = flat_str(u, "author").substr(0, 24);
                r.credits = credits_clean(flat_str(u, "credits"));   // SG-0070; absent from older servers
                r.date = upload_date(flat_str(u, "created"));          // SG-0088
                if (r.id > 0 && r.tmpl >= 0 && r.tmpl < 182) c.rows.push_back(r);
                flat_ws(reply, q);
                if (q < reply.size() && reply[q] == ',') ++q;
            }
        }
        c.listed = true;
        c.message = c.rows.empty() ? "No community units yet." : "";
    } else if (kind == 3) {   // a subscribe
        std::vector<uint8_t> b;
        Com::Row* r = nullptr;
        for (Com::Row& x : c.rows)
            if (x.id == arg) r = &x;
        const std::string folder = "c" + std::to_string(arg) + "_" + safe_name(r ? r->name : std::string("unit"));
        if (!r || !b64_decode(flat_str(j, "bundle"), &b, 1u << 20) || !lb_cunits_unpack(b, folder)) {
            c.bad = true;
            c.message = "Could not write the unit to the mods folder.";
            return;
        }
        ++r->downloads;
        std::vector<std::string> en = read_enabled();
        if (std::find(en.begin(), en.end(), folder) == en.end()) en.push_back(folder);
        write_enabled(en);
        c.message = "Subscribed to " + r->name + ". Load new units now to use it.";
    } else if (kind == 4) {   // a like
        const int id = flat_int(j, "id");
        for (Com::Row& r : c.rows)
            if (r.id == id) r.likes = flat_int(j, "likes");
        if (flat_str(j, "liked") == "true") c.liked.insert(id);
        else c.liked.erase(id);
        save_liked();
    }
}

const char* kKinds[20] = {"Hero", "Builder", "Melee", "Ranged", "Mounted", "Transport", "Special", "Base",
                          "Lumber mill", "Mine", "Farm", "Barracks", "Factory", "Tower", "Tower 2", "Tower 3",
                          "Shipyard", "Bridge", "Gate", "Wall"};

const Tex* com_thumb(int id) {
    Com& c = g_com;
    auto it = c.thumbs.find(id);
    if (it != c.thumbs.end()) return it->second.t ? &it->second : nullptr;
    std::vector<uint8_t> png;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_th.mu);
        auto g = g_th.got.find(id);
        if (g != g_th.got.end()) {
            png.swap(g->second);
            g_th.got.erase(g);
            have = true;
        } else if (!c.asked.count(id)) {
            c.asked.insert(id);
            g_th.want.push_back(id);
            if (!g_th.running.exchange(true)) std::thread(thumb_worker).detach();
        }
    }
    if (!have) return nullptr;
    Tex t;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    if (g_ren && lb_cunits_png_rgba(png, &w, &h, &px) && w > 0 && w <= 256 && h <= 256) {
        t.t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t.t) {
            SDL_UpdateTexture(t.t, nullptr, px.data(), w * 4);
            SDL_SetTextureBlendMode(t.t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.t, SDL_ScaleModeNearest);
        }
        t.w = w;
        t.h = h;
    }
    return c.thumbs.emplace(id, t).first->second.t ? &c.thumbs[id] : nullptr;
}

void draw_community(const std::vector<Rec>& rs) {
    Com& c = g_com;
    State& s = g_st;
    load_liked();
    const bool busy = g_un.busy;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(c.listed ? "Refresh" : "Show community units")) com_refresh();
    ImGui::EndDisabled();
    ImGui::SameLine();
    static const char* const kSortNames[4] = {"Newest", "Oldest", "Most downloaded", "Most liked"};
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("Sort", &c.sort, kSortNames, 4) && !busy) com_refresh();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("Kind", c.category < 0 ? "All" : kKinds[c.category])) {
        if (ImGui::Selectable("All", c.category < 0)) { c.category = -1; if (!busy) com_refresh(); }
        for (int k = 0; k < 20; ++k)
            if (ImGui::Selectable(kKinds[k], c.category == k)) { c.category = k; if (!busy) com_refresh(); }
        ImGui::EndCombo();
    }
    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("Working...");
    }
    if (!c.message.empty())
        ImGui::TextColored(c.bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", c.message.c_str());
    if (c.rows.empty()) return;
    const std::map<int, std::string> subs = subscribed();
    if (!ImGui::BeginTable("##comunits", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit))
        return;
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Downloads");
    ImGui::TableSetupColumn("Likes");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (Com::Row& r : c.rows) {
        ImGui::PushID(r.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const Tex* t = com_thumb(r.id);
        const float side = 48.0f;
        if (t) {
            const float k = side / (float)std::max(t->w, t->h);
            ImGui::Image((ImTextureID)(intptr_t)t->t, ImVec2(t->w * k, t->h * k));
        } else {
            ImGui::Dummy(ImVec2(side, side));
        }
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.name.c_str());
        ImGui::TextDisabled("by %s, from %s%s", r.author.empty() ? "someone" : r.author.c_str(),
                            rs[(size_t)r.tmpl].name.c_str(), r.sounds ? ", has sounds" : "");
        if (!r.date.empty()) ImGui::TextDisabled("Uploaded %s", r.date.c_str());   // SG-0088
        if (!r.credits.empty()) ImGui::TextDisabled("Credits: %s", r.credits.c_str());   // SG-0070
        ImGui::TableNextColumn();
        ImGui::Text("%s", r.category >= 0 && r.category < 20 ? kKinds[r.category] : "?");
        ImGui::TableNextColumn();
        ImGui::Text("%d", r.downloads);
        ImGui::TableNextColumn();
        ImGui::Text("%d", r.likes);
        ImGui::TableNextColumn();
        auto sub = subs.find(r.id);
        ImGui::BeginDisabled(busy);
        if (sub != subs.end()) {
            if (ImGui::SmallButton("Preview")) {
                s.a.pick = r.tmpl;
                s.a.folder = sub->second;
                s.a.title = r.name;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Unsubscribe")) {
                std::error_code ec;
                fs::remove_all(units_root() / fs::u8path(sub->second), ec);
                std::vector<std::string> en = read_enabled();
                en.erase(std::remove(en.begin(), en.end(), sub->second), en.end());
                write_enabled(en);
                if (s.a.folder == sub->second) s.a.folder.clear();
                if (s.b.folder == sub->second) s.b.folder.clear();
                c.bad = false;
                c.message = "Unsubscribed from " + r.name + ". Load new units now to drop it.";
            }
        } else if (ImGui::SmallButton("Subscribe")) {
            unet_start(3, r.id, "GET", "/units/get?count=1&id=" + std::to_string(r.id), "");
        }
        ImGui::SameLine();
        const bool liked = c.liked.count(r.id) != 0;
        if (ImGui::SmallButton(liked ? "Unlike" : "Like")) {
            const std::string body = std::string("{\"key\":\"") + lb_setting_player_key() + "\",\"id\":" +
                                     std::to_string(r.id) + ",\"like\":" + (liked ? "false" : "true") + "}";
            unet_start(4, r.id, "POST", "/units/like", body);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

// Balance > Custom units (lb_overlay.cpp's page).
// The textures belong to one renderer; a new one (a window rebuilt) starts over.
static void unitlab_renderer(SDL_Renderer* ren) {
    if (ren != g_ren) {
        for (auto& kv : g_tex)
            if (kv.second.t) SDL_DestroyTexture(kv.second.t);
        g_tex.clear();
        g_ren = ren;
    }
}

// Balance > Community units: other players' units, on a page of their own.
void lb_unitlab_draw_community(SDL_Renderer* ren) {
    unitlab_renderer(ren);
    unet_poll();
    const std::vector<Rec>& rs = records();
    if (rs.empty()) {
        ImGui::TextDisabled("The cartridge's BP/Entities.ebp could not be read.");
        return;
    }
    draw_community(rs);
}

void lb_unitlab_draw(SDL_Renderer* ren) {
    unitlab_renderer(ren);
    State& s = g_st;
    unet_poll();
    const std::vector<Rec>& rs = records();
    if (rs.empty()) {
        ImGui::TextDisabled("The cartridge's BP/Entities.ebp could not be read.");
        return;
    }
    draw_how_to();
    const fs::path root = fs::u8path(lb_mods_dir()) / "units" / "export";
    // What applies to every custom unit.
    if (ImGui::Button("Load new units now")) {
        if (lb_mods_request_rebuild()) {
            s.bad = false;
            s.message = "Loading the custom units: the game returns to the title screen.";
            drop_folder_textures(std::string());
        } else {
            s.bad = true;
            s.message = "Restart the game to load the custom units.";
        }
    }
    ImGui::SetItemTooltip("Rebuilds the game with the ticked custom units. A match in progress ends.");
    ImGui::SameLine();
    if (ImGui::Button("Open units folder")) {
        std::error_code ec;
        fs::create_directories(units_root(), ec);
        std::string url = "file:///" + utf8(units_root());
        for (char& c : url) if (c == '\\') c = '/';
        SDL_OpenURL(url.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Export all")) {
        int total = 0, failed = 0;
        const bool snd = lb_sfx_export_begin() != 0;
        for (const Rec& r : rs) {
            const int n = export_one(r, root);
            if (n < 0) ++failed;
            else total += n;
        }
        (void)snd;   // kept loaded for the sound preview (sounds_for)
        s.bad = failed > 0;
        s.message = "Wrote " + std::to_string(total) + " files for " + std::to_string((int)rs.size() - failed) +
                    " units and buildings to " + utf8(root) +
                    (failed ? " (" + std::to_string(failed) + " failed)" : std::string());
    }
    ImGui::SetItemTooltip("Every unit and building's files, into mods/units/export.");
    ImGui::SameLine();
    if (ImGui::Button("Convert...")) g_conv.open = !g_conv.open;   // SG-0061
    ImGui::SetItemTooltip("Convert an .NCBR (and .NCLR palette) to a PNG, or a PNG to an .NCBR.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::InputTextWithHint("##author", "Your name (for sharing)", g_author, sizeof g_author);
    if (!s.message.empty())
        ImGui::TextColored(s.bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", s.message.c_str());

    if (g_conv.open) draw_convert();
    const std::vector<Mine> mine = list_mine();
    const float h = std::max(200.0f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("##lab", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("units", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##lablist", ImVec2(0, h));
        draw_lab_list(rs, mine);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##labdetail", ImVec2(0, h));
        draw_lab_detail(rs, mine, root);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}
