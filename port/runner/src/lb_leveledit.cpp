// lb_leveledit.cpp - the level editor (Maps > Level Editor).
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file.
//
// WHAT IT EDITS. The cartridge's .map (lb_levelmap.h): plane A's terrain
// classes and the tree mask are what the brush paints; plane E's metatiles,
// plane B's tree edges and plane C's areas follow from them. The metatile a
// cell gets is the one the stock maps of that world use most for the same 3x3
// neighbourhood of classes and tree flag (tools/custommaps/build_map.py's
// "donor" rule), learned at run time from the player's own cartridge, so the
// editor ships no game data. Starts, mines, bridge sites, markers, golden
// bricks and other buildings are the map's own records.
//
// WHERE IT GOES. Save writes a loose map folder under mods/maps/ (docs/413:
// map.map, the four minimaps, the detail tiles), which the next map reload
// lists in Free Play and multiplayer, and lobbies send to players who lack
// it (docs/432). Export writes the same files as an .lbz (the map tool's
// package). Import reads an .lbz, a .map, or any stock map.
#include "lb_levelmap.h"

#include "imgui.h"
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <zlib.h>

extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);   // lb_teams.cpp
extern "C" const char* lb_mods_dir(void);                                              // lb_mods.cpp
extern "C" int lb_mods_request_rebuild(void);                                          // lb_mods.cpp
extern "C" int lb_pick_level_file(char* buf, int cap, int saving);                     // lb_filedlg.cpp
extern "C" int lb_maps_entry(int i, char* token, int tcap, char* name, int ncap, int* players);
extern "C" const char* lb_swear_shown(const char* text);                               // lb_overlay.cpp
bool lb_mods_unwrap(std::vector<uint8_t>* b);                                          // lb_mods.cpp
bool lb_lbz_unpack(const std::string& zip, std::string* map, std::string* tbp,
                   std::string* manifest);                                             // lb_mapfetch.cpp

namespace fs = std::filesystem;

namespace {

constexpr int kMetaW = 24, kMetaH = 16;   // one cell: 3x2 tiles of 8x8
constexpr int kTableMetas = 440;          // E below this: the world's table
constexpr int kMinSize = 32, kMaxSize = 128;

// ── Worlds ───────────────────────────────────────────────────────────────────
struct WorldDef { const char* name; const char* tileset; const char* prefix; const char* mine; };
const WorldDef kWorlds[3] = {
    {"King", "KingTileset", "King", "Mine"},
    {"Pirate", "PirateTileset", "Pirate", "Quarry"},
    {"Mars", "MarsTileset", "Mars", "Well cap"},
};
int world_of(const std::string& tileset) {
    for (int w = 0; w < 3; ++w)
        if (tileset == kWorlds[w].tileset) return w;
    return 0;
}

struct Meta { uint16_t t[6]; };

// The fallback metatile per class when no donor knows the neighbourhood
// (build_map.py's FALL; the first 440 entries are the same slots in every
// world, docs/387).
uint16_t fall_meta(uint8_t a) {
    return a == lvl::kStone ? 96 : a == lvl::kWater ? 64 : a == lvl::kMountain ? 89 : 116;
}

// Classes packed 3 bits each: 0 grass, 1 stone, 2 water, 3 mountain, 4 off
// the map, 5 anything else.
uint32_t cls3(int a) {
    return a == 0 ? 0u : a == 2 ? 1u : a == 3 ? 2u : a == 5 ? 3u : a == 9 ? 4u : 5u;
}

struct WorldAssets {
    bool tried = false, ok = false;
    std::string why;
    std::vector<uint8_t> chr;              // 1024 tiles x 64 bytes, 8bpp
    uint32_t pal[256] = {};                // ABGR8888
    std::vector<Meta> table;               // the world's metatile table
    // Donors: neighbourhood key -> metatile -> how often the stock maps use it.
    std::unordered_map<uint32_t, std::map<uint16_t, int>> k9, k5, k1;
    std::vector<int> e2a;                  // the class each table metatile is drawn for
    std::vector<uint8_t> e2b;              // its usual tree-edge byte
    std::vector<bool> flat;                // drawn as one flat colour
    SDL_Texture* atlas = nullptr;          // the 440 metatiles, 20 a row
    SDL_Renderer* atlas_ren = nullptr;
};
WorldAssets g_world[3];

// The stock maps, with the world each is drawn in (docs/data/maps.tsv).
struct StockMap { const char* stem; char world; };
const StockMap kStock[] = {
    {"ck1_1",'K'},{"ck1_2",'K'},{"ck1_3",'K'},{"ck1_4",'K'},{"ck1_6",'K'},{"ck2_1",'K'},{"ck2_2",'K'},
    {"ck2_3",'K'},{"ck2_4",'K'},{"ck2_5",'K'},{"ck2_6",'K'},{"ck3_1",'K'},{"ck3_2",'K'},{"ck3_3",'K'},
    {"ck3_4",'K'},{"cw1_1",'K'},{"cw1_2",'K'},{"cw1_3",'K'},{"cw1_4",'K'},{"cw1_5",'K'},{"cw1_6",'K'},
    {"cw2_1",'K'},{"cw2_2",'K'},{"cw2_3",'K'},{"cw2_4",'K'},{"cw2_6",'K'},{"cw3_1",'K'},{"cw3_2",'K'},
    {"cw3_3",'K'},{"cw3_4",'K'},{"ma1_1",'M'},{"ma1_3",'M'},{"ma1_4",'M'},{"ma1_6",'M'},{"ma1_7",'M'},
    {"ma2_1",'M'},{"ma2_2",'M'},{"ma2_3",'M'},{"ma2_5",'M'},{"ma2_6",'M'},{"ma3_1",'M'},{"ma3_2",'M'},
    {"ma3_4",'M'},{"ma3_5",'M'},{"ma3_6",'M'},{"mh1_1",'M'},{"mh1_2",'M'},{"mh1_3",'M'},{"mh1_5",'M'},
    {"mh1_6",'M'},{"mh1_7",'M'},{"mh2_1",'M'},{"mh2_3",'M'},{"mh2_4",'M'},{"mh2_5",'M'},{"mh2_6",'M'},
    {"mh3_2",'M'},{"mh3_3",'M'},{"mh3_5",'M'},{"mh3_6",'M'},{"mp01",'K'},{"mp02",'M'},{"mp03",'P'},
    {"mp04",'K'},{"mp05",'M'},{"mp06",'M'},{"mp07",'K'},{"mp08",'P'},{"mp09",'M'},{"mp10",'K'},
    {"mp11",'P'},{"mp12",'M'},{"mp13",'K'},{"mp14",'P'},{"mp15",'M'},{"mp16",'K'},{"mp17",'K'},
    {"mp18",'M'},{"mp19",'K'},{"mp20",'M'},{"mp21",'P'},{"mp22",'K'},{"mp23",'P'},{"mp24",'M'},
    {"mp25",'P'},{"mp26",'P'},{"mp27",'P'},{"mp28",'M'},{"mp29",'K'},{"mp30",'P'},{"pi1_1",'P'},
    {"pi1_2",'P'},{"pi1_3",'P'},{"pi1_4",'P'},{"pi1_5",'P'},{"pi2_1",'P'},{"pi2_2",'P'},{"pi2_3",'P'},
    {"pi2_4",'P'},{"pi2_5",'P'},{"pi3_1",'P'},{"pi3_2",'P'},{"pi3_3",'P'},{"pi3_4",'P'},{"pi3_5",'P'},
    {"pi3_6",'P'},{"pp1_1",'P'},{"pp1_2",'P'},{"pp1_3",'P'},{"pp1_4",'P'},{"pp1_5",'P'},{"pp2_1",'P'},
    {"pp2_3",'P'},{"pp2_4",'P'},{"pp2_5",'P'},{"pp2_6",'P'},{"pp3_1",'P'},{"pp3_3",'P'},{"pp3_4",'P'},
    {"pp3_5",'P'},{"pp3_6",'P'},
};
int stock_world(char c) { return c == 'P' ? 1 : c == 'M' ? 2 : 0; }

// A cartridge file, decompressed. Empty when missing.
std::vector<uint8_t> rom_file(const std::string& path) {
    const int n = lb_rom_read_unwrapped(path.c_str(), nullptr, 0);
    if (n <= 0 || n > (16 << 20)) return {};
    std::vector<uint8_t> b((size_t)n);
    if (lb_rom_read_unwrapped(path.c_str(), b.data(), n) != n) return {};
    return b;
}

std::vector<Meta> read_tbp(const std::vector<uint8_t>& b) {
    std::vector<Meta> out;
    if (b.size() < 2) return out;
    const size_t n = (size_t)(b[0] | (b[1] << 8));
    if (2 + n * 12 > b.size()) return out;
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 6; ++k) {
            const size_t at = 2 + i * 12 + k * 2;
            out[i].t[k] = (uint16_t)(b[at] | (b[at + 1] << 8));
        }
    return out;
}
std::vector<uint8_t> write_tbp(const std::vector<Meta>& t) {
    std::vector<uint8_t> o;
    o.push_back((uint8_t)(t.size() & 0xFF));
    o.push_back((uint8_t)(t.size() >> 8));
    for (const Meta& m : t)
        for (int k = 0; k < 6; ++k) {
            o.push_back((uint8_t)(m.t[k] & 0xFF));
            o.push_back((uint8_t)(m.t[k] >> 8));
        }
    return o;
}

// One metatile's pixels into `dst` (row stride `pitch` in pixels).
void draw_meta(const WorldAssets& wa, const Meta& m, uint32_t* dst, int pitch) {
    for (int k = 0; k < 6; ++k) {
        const uint16_t v = m.t[k];
        const int tile = v & 0x3FF;
        const bool hf = (v & 0x400) != 0, vf = (v & 0x800) != 0;
        const int ox = (k % 3) * 8, oy = (k / 3) * 8;
        const uint8_t* src = wa.chr.data() + (size_t)tile * 64;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int sx = hf ? 7 - x : x, sy = vf ? 7 - y : y;
                dst[(size_t)(oy + y) * pitch + ox + x] = wa.pal[src[sy * 8 + sx]];
            }
    }
}

uint32_t nb_key9(const std::vector<uint8_t>& A, int w, int h, int x, int y, int tree) {
    uint32_t k = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            const int a = (nx < 0 || ny < 0 || nx >= w || ny >= h) ? 9 : A[(size_t)ny * w + nx];
            k = k * 8 + cls3(a);
        }
    return (k << 1) | (uint32_t)(tree & 1);
}
uint32_t nb_key5(const std::vector<uint8_t>& A, int w, int h, int x, int y, int tree) {
    const int d[5][2] = {{0, 0}, {0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    uint32_t k = 0;
    for (const auto& q : d) {
        const int nx = x + q[0], ny = y + q[1];
        const int a = (nx < 0 || ny < 0 || nx >= w || ny >= h) ? 9 : A[(size_t)ny * w + nx];
        k = k * 8 + cls3(a);
    }
    return (k << 1) | (uint32_t)(tree & 1);
}

// Loads a world's graphics and learns its donor tables. Once per run.
WorldAssets& world_assets(int w) {
    WorldAssets& wa = g_world[w];
    if (wa.tried) return wa;
    wa.tried = true;
    const WorldDef& d = kWorlds[w];
    const std::vector<uint8_t> ncgr = rom_file(std::string(d.tileset) + ".NCGR");
    const std::vector<uint8_t> nclr = rom_file(std::string(d.tileset) + ".NCLR");
    wa.table = read_tbp(rom_file(std::string("BP/") + d.prefix + "Tiles.tbp"));
    if (ncgr.size() < 0x30 + 1024 * 64 || nclr.size() < 0x28 + 512 || wa.table.size() < kTableMetas) {
        wa.why = "the cartridge's tileset files could not be read";
        return wa;
    }
    wa.chr.assign(ncgr.begin() + 0x30, ncgr.begin() + 0x30 + 1024 * 64);
    for (int i = 0; i < 256; ++i) {
        const uint16_t c = (uint16_t)(nclr[0x28 + i * 2] | (nclr[0x29 + i * 2] << 8));
        const uint32_t r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
        wa.pal[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    // Flat metatiles: all six tiles one colour each, the same colour.
    wa.flat.assign(kTableMetas, false);
    for (int e = 0; e < kTableMetas; ++e) {
        int colour = -1;
        bool flat = true;
        for (int k = 0; k < 6 && flat; ++k) {
            const uint8_t* t = wa.chr.data() + (size_t)(wa.table[e].t[k] & 0x3FF) * 64;
            for (int p = 0; p < 64 && flat; ++p) flat = t[p] == t[0];
            if (colour < 0) colour = t[0];
            flat = flat && t[0] == colour;
        }
        wa.flat[e] = flat && colour != 0;
    }
    // The donors: every stock map of this world.
    std::vector<std::map<int, int>> a_of(kTableMetas), b_of(kTableMetas);
    const char wc = w == 1 ? 'P' : w == 2 ? 'M' : 'K';
    for (const StockMap& s : kStock) {
        if (s.world != wc) continue;
        const std::vector<uint8_t> raw = rom_file(std::string("Maps/") + s.stem + ".map");
        lvl::Map m;
        if (raw.empty() || !lvl::parse(raw.data(), raw.size(), &m, nullptr)) continue;
        const std::vector<uint8_t> trees = lvl::trees_decode(m.D, m.w * m.h);
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const size_t i = (size_t)y * m.w + x;
                const uint16_t e = m.E[i];
                if (e >= kTableMetas) continue;
                ++a_of[e][m.A[i]];
                ++b_of[e][m.B[i]];
                ++wa.k9[nb_key9(m.A, m.w, m.h, x, y, trees[i])][e];
                ++wa.k5[nb_key5(m.A, m.w, m.h, x, y, trees[i])][e];
                ++wa.k1[(cls3(m.A[i]) << 1) | trees[i]][e];
            }
    }
    wa.e2a.assign(kTableMetas, -1);
    wa.e2b.assign(kTableMetas, 0);
    for (int e = 0; e < kTableMetas; ++e) {
        int best = 0;
        for (const auto& kv : a_of[e])
            if (kv.second > best) { best = kv.second; wa.e2a[e] = kv.first; }
        best = 0;
        for (const auto& kv : b_of[e])
            if (kv.second > best) { best = kv.second; wa.e2b[e] = (uint8_t)kv.first; }
    }
    if (wa.k1.empty()) {
        wa.why = "the cartridge's maps could not be read";
        return wa;
    }
    wa.ok = true;
    return wa;
}

SDL_Texture* world_atlas(int w, SDL_Renderer* ren) {
    WorldAssets& wa = world_assets(w);
    if (!wa.ok || !ren) return nullptr;
    if (wa.atlas && wa.atlas_ren == ren) return wa.atlas;
    const int cols = 20, rows = (kTableMetas + cols - 1) / cols;
    std::vector<uint32_t> px((size_t)cols * kMetaW * rows * kMetaH, 0);
    for (int e = 0; e < kTableMetas; ++e)
        draw_meta(wa, wa.table[e],
                  px.data() + (size_t)(e / cols) * kMetaH * cols * kMetaW + (size_t)(e % cols) * kMetaW,
                  cols * kMetaW);
    wa.atlas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, cols * kMetaW,
                                 rows * kMetaH);
    if (wa.atlas) {
        SDL_UpdateTexture(wa.atlas, nullptr, px.data(), cols * kMetaW * 4);
        SDL_SetTextureScaleMode(wa.atlas, SDL_ScaleModeNearest);
    }
    wa.atlas_ren = ren;
    return wa.atlas;
}

// ── The document ─────────────────────────────────────────────────────────────
enum Tool { kToolBrush, kToolLine, kToolRect, kToolEllipse, kToolFill, kToolPick,
            kToolStart, kToolMine, kToolBridge, kToolMarker, kToolBrick, kToolBuilding, kToolErase,
            kToolSelect, kToolCount };
const char* const kToolNames[kToolCount] = {
    "Brush", "Line", "Rectangle", "Ellipse", "Fill", "Eyedropper",
    "Start base", "Mine", "Bridge site", "AI marker", "Golden brick", "Building / unit", "Remove object",
    "Select (copy / paste)",
};
enum Material { kMatGrass, kMatStone, kMatWater, kMatMountain, kMatTrees, kMatCount };
const char* const kMatNames[kMatCount] = {"Grass", "Stone", "Water", "Mountain", "Trees"};
const ImU32 kMatColours[kMatCount] = {IM_COL32(70, 160, 60, 255), IM_COL32(150, 140, 120, 255),
                                      IM_COL32(50, 110, 200, 255), IM_COL32(110, 80, 60, 255),
                                      IM_COL32(20, 90, 30, 255)};
const ImU32 kTeamColours[4] = {IM_COL32(60, 110, 255, 255), IM_COL32(230, 50, 50, 255),
                               IM_COL32(60, 200, 70, 255), IM_COL32(240, 200, 40, 255)};
const char* const kTeamNames[4] = {"Player 1 (blue)", "Player 2 (red)", "Player 3 (green)",
                                   "Player 4 (yellow)"};

struct EntityKind { uint8_t type; const char* name; int w, h; };
const EntityKind kBuildings[] = {
    {lvl::kBase, "Base", 3, 3}, {lvl::kFarm, "Farm", 2, 2}, {lvl::kBarracks, "Barracks", 2, 2},
    {lvl::kSpecialFactory, "Special factory", 2, 2}, {lvl::kHarvester, "Harvester / mill", 2, 2},
    {lvl::kMineB, "Mine / quarry / well cap building", 2, 2}, {lvl::kShipyard, "Shipyard", 2, 2},
    {lvl::kTower, "Tower", 1, 1}, {lvl::kTower2, "Tower (upgraded)", 1, 1},
    {lvl::kTower3, "Tower (upgraded twice)", 1, 1}, {lvl::kGate, "Gate", 1, 1}, {lvl::kWall, "Wall", 1, 1},
    {lvl::kHero, "Hero", 1, 1}, {lvl::kBuilder, "Builder", 1, 1}, {lvl::kMelee, "Melee unit", 1, 1},
    {lvl::kRanged, "Ranged unit", 1, 1}, {lvl::kMounted, "Mounted unit", 1, 1},
    {lvl::kSpecial, "Special unit", 1, 1}, {lvl::kTransport, "Transport", 1, 1},
};
const EntityKind* kind_of(uint8_t type) {
    for (const EntityKind& k : kBuildings)
        if (k.type == type) return &k;
    return nullptr;
}
void footprint(uint8_t type, int* w, int* h) {
    const EntityKind* k = kind_of(type);
    *w = k ? k->w : 1;
    *h = k ? k->h : 1;
}

struct Snapshot { lvl::Map m; std::vector<uint8_t> trees; std::vector<Meta> detail; };

struct Doc {
    bool open = false;
    lvl::Map m;
    std::vector<uint8_t> trees;       // one byte per cell
    std::vector<Meta> detail;         // the map's own metatiles (E >= 440)
    int world = 0;
    char name[48] = "My level";
    char author[32] = "";
    bool dirty = false;
    std::vector<Snapshot> undo, redo;
    // The canvas picture, redrawn by cell.
    std::vector<uint32_t> px;
    SDL_Texture* tex = nullptr;
    SDL_Renderer* tex_ren = nullptr;
    int tex_w = 0, tex_h = 0;
    int dirty_x0 = 0, dirty_y0 = 0, dirty_x1 = -1, dirty_y1 = -1;   // cells, inclusive
};
Doc g_doc;

struct Ui {
    int tool = kToolBrush, material = kMatGrass, brush = 2;
    bool round_brush = true, filled = true;
    int team = 0, building = 0, building_team = 0, marker_group = 0, bridge_len = 6;
    float zoom = 1.0f;
    ImVec2 pan = ImVec2(0, 0);
    bool dragging = false, painting = false;
    int drag_x = 0, drag_y = 0;
    bool grid = false, show_objects = true, show_areas = false, show_markers = true;
    int new_w = 64, new_h = 64, new_world = 0;
    // The selection (cells, inclusive; sel_x1 < sel_x0 when none) and a
    // paste in progress (the clipboard follows the pointer until a click).
    int sel_x0 = 0, sel_y0 = 0, sel_x1 = -1, sel_y1 = -1;
    bool pasting = false;
    int stock_pick = 60;   // mp01
    char status[256] = "";
    bool status_bad = false;
    char path[1024] = "";
};
Ui g_ui;

void say(bool bad, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_ui.status, sizeof g_ui.status, fmt, ap);
    va_end(ap);
    g_ui.status_bad = bad;
}

size_t at(int x, int y) { return (size_t)y * g_doc.m.w + x; }
bool inside(int x, int y) { return x >= 0 && y >= 0 && x < g_doc.m.w && y < g_doc.m.h; }

void mark(int x0, int y0, int x1, int y1) {
    Doc& d = g_doc;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(d.m.w - 1, x1); y1 = std::min(d.m.h - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    if (d.dirty_x1 < d.dirty_x0) {
        d.dirty_x0 = x0; d.dirty_y0 = y0; d.dirty_x1 = x1; d.dirty_y1 = y1;
    } else {
        d.dirty_x0 = std::min(d.dirty_x0, x0); d.dirty_y0 = std::min(d.dirty_y0, y0);
        d.dirty_x1 = std::max(d.dirty_x1, x1); d.dirty_y1 = std::max(d.dirty_y1, y1);
    }
}

void push_undo() {
    Doc& d = g_doc;
    d.undo.push_back({d.m, d.trees, d.detail});
    if (d.undo.size() > 64) d.undo.erase(d.undo.begin());
    d.redo.clear();
    d.dirty = true;
}
void restore(Snapshot&& s) {
    Doc& d = g_doc;
    d.m = std::move(s.m);
    d.trees = std::move(s.trees);
    d.detail = std::move(s.detail);
    mark(0, 0, d.m.w - 1, d.m.h - 1);
}
void do_undo() {
    Doc& d = g_doc;
    if (d.undo.empty()) return;
    d.redo.push_back({d.m, d.trees, d.detail});
    Snapshot s = std::move(d.undo.back());
    d.undo.pop_back();
    restore(std::move(s));
}
void do_redo() {
    Doc& d = g_doc;
    if (d.redo.empty()) return;
    d.undo.push_back({d.m, d.trees, d.detail});
    Snapshot s = std::move(d.redo.back());
    d.redo.pop_back();
    restore(std::move(s));
}

// ── Tiling ───────────────────────────────────────────────────────────────────
uint16_t pick_meta(int x, int y) {
    Doc& d = g_doc;
    WorldAssets& wa = world_assets(d.world);
    const size_t i = at(x, y);
    const uint8_t a = d.m.A[i];
    if (!wa.ok) return fall_meta(a);
    const int t = d.trees[i];
    bool interior = true;
    for (int dy = -1; dy <= 1 && interior; ++dy)
        for (int dx = -1; dx <= 1 && interior; ++dx)
            if (inside(x + dx, y + dy) && d.m.A[at(x + dx, y + dy)] != a) interior = false;
    const uint32_t keys[3] = {nb_key9(d.m.A, d.m.w, d.m.h, x, y, t), nb_key5(d.m.A, d.m.w, d.m.h, x, y, t),
                              (cls3(a) << 1) | (uint32_t)t};
    const std::unordered_map<uint32_t, std::map<uint16_t, int>>* tabs[3] = {&wa.k9, &wa.k5, &wa.k1};
    for (int k = 0; k < 3; ++k) {
        const auto it = tabs[k]->find(keys[k]);
        if (it == tabs[k]->end()) continue;
        int best = 0, sum = 0;
        uint16_t e = 0;
        for (const auto& kv : it->second) {
            if (kv.first >= kTableMetas || wa.e2a[kv.first] != a) continue;
            if (!interior && wa.flat[kv.first]) continue;
            sum += kv.second;
            if (kv.second > best) { best = kv.second; e = kv.first; }
        }
        if (best > 0 && (k == 2 || sum >= 3)) return e;
    }
    return fall_meta(a);
}

// Re-picks the metatiles of the cells in a box, one cell of margin outside it
// (their neighbourhoods changed too).
void retile(int x0, int y0, int x1, int y1) {
    Doc& d = g_doc;
    WorldAssets& wa = world_assets(d.world);
    x0 = std::max(0, x0 - 1); y0 = std::max(0, y0 - 1);
    x1 = std::min(d.m.w - 1, x1 + 1); y1 = std::min(d.m.h - 1, y1 + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = at(x, y);
            const uint16_t e = pick_meta(x, y);
            d.m.E[i] = e;
            d.m.B[i] = (wa.ok && e < kTableMetas) ? wa.e2b[e] : 0;
        }
    mark(x0, y0, x1, y1);
}

// Writes the pads of every mine into plane E (the stock pad, metatiles
// 13 14 / 18 19), on stone.
void stamp_mines() {
    Doc& d = g_doc;
    for (const auto& g : d.m.mines)
        for (const lvl::Point& p : g) {
            const uint16_t pad[4] = {13, 14, 18, 19};
            for (int k = 0; k < 4; ++k) {
                const int x = p.x + (k & 1), y = p.y + (k >> 1);
                if (!inside(x, y)) continue;
                d.m.A[at(x, y)] = lvl::kStone;
                d.trees[at(x, y)] = 0;
                d.m.E[at(x, y)] = pad[k];
                d.m.B[at(x, y)] = 0;
            }
            mark(p.x, p.y, p.x + 1, p.y + 1);
        }
}

// ── Painting ─────────────────────────────────────────────────────────────────
void set_cell(int x, int y, int mat) {
    if (!inside(x, y)) return;
    Doc& d = g_doc;
    const size_t i = at(x, y);
    const uint8_t a = mat == kMatStone ? lvl::kStone : mat == kMatWater ? lvl::kWater
                    : mat == kMatMountain ? lvl::kMountain : lvl::kGrass;
    d.m.A[i] = a;
    d.trees[i] = mat == kMatTrees ? 1 : 0;
}

// The cells a shape covers, in a box; painted, then retiled once.
void paint_cells(const std::vector<std::pair<int, int>>& cells, int mat) {
    if (cells.empty()) return;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    for (const auto& c : cells) {
        set_cell(c.first, c.second, mat);
        x0 = std::min(x0, c.first); y0 = std::min(y0, c.second);
        x1 = std::max(x1, c.first); y1 = std::max(y1, c.second);
    }
    retile(x0, y0, x1, y1);
    stamp_mines();
}

void brush_cells(int cx, int cy, int size, bool round, std::vector<std::pair<int, int>>* out) {
    const int r = size - 1;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (round && dx * dx + dy * dy > r * r + r) continue;
            if (inside(cx + dx, cy + dy)) out->push_back({cx + dx, cy + dy});
        }
}

std::vector<std::pair<int, int>> shape_cells(int tool, int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int, int>> out;
    if (tool == kToolLine) {
        int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy, x = x0, y = y0;
        for (;;) {
            brush_cells(x, y, g_ui.brush, g_ui.round_brush, &out);
            if (x == x1 && y == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    } else if (tool == kToolRect) {
        const int ax = std::min(x0, x1), bx = std::max(x0, x1), ay = std::min(y0, y1), by = std::max(y0, y1);
        const int t = std::max(1, g_ui.brush);
        for (int y = ay; y <= by; ++y)
            for (int x = ax; x <= bx; ++x)
                if (g_ui.filled || x - ax < t || bx - x < t || y - ay < t || by - y < t)
                    if (inside(x, y)) out.push_back({x, y});
    } else if (tool == kToolEllipse) {
        const float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
        const float rx = std::abs(x1 - x0) / 2.0f + 0.5f, ry = std::abs(y1 - y0) / 2.0f + 0.5f;
        const float t = (float)std::max(1, g_ui.brush);
        for (int y = std::min(y0, y1); y <= std::max(y0, y1); ++y)
            for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x) {
                const float nx = (x - cx) / rx, ny = (y - cy) / ry;
                const float r2 = nx * nx + ny * ny;
                if (r2 > 1.0f) continue;
                if (!g_ui.filled) {
                    const float ix = (x - cx) / std::max(0.5f, rx - t), iy = (y - cy) / std::max(0.5f, ry - t);
                    if (ix * ix + iy * iy <= 1.0f) continue;
                }
                if (inside(x, y)) out.push_back({x, y});
            }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void flood(int x, int y, int mat) {
    Doc& d = g_doc;
    if (!inside(x, y)) return;
    const uint8_t a = d.m.A[at(x, y)];
    const uint8_t t = d.trees[at(x, y)];
    std::vector<uint8_t> seen(d.m.A.size(), 0);
    std::vector<std::pair<int, int>> st{{x, y}}, cells;
    seen[at(x, y)] = 1;
    while (!st.empty()) {
        const auto c = st.back();
        st.pop_back();
        cells.push_back(c);
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& q : nb) {
            const int nx = c.first + q[0], ny = c.second + q[1];
            if (!inside(nx, ny) || seen[at(nx, ny)]) continue;
            if (d.m.A[at(nx, ny)] != a || d.trees[at(nx, ny)] != t) continue;
            seen[at(nx, ny)] = 1;
            st.push_back({nx, ny});
        }
    }
    paint_cells(cells, mat);
}

// ── Objects ──────────────────────────────────────────────────────────────────
// A start is an event holding a base: the game's own rule (docs/414) is six
// or more entities of one team.
int find_start(int team) {
    const auto& ev = g_doc.m.events;
    for (size_t i = 0; i < ev.size(); ++i)
        for (const lvl::Entity& e : ev[i].ents)
            if (e.type == lvl::kBase && e.team == team && ev[i].ents.size() >= 6) return (int)i;
    return -1;
}
int start_count() {
    int n = 0;
    for (int t = 0; t < 4; ++t) n += find_start(t) >= 0;
    return n;
}
// The event holding loose entities (buildings and units that are no start):
// the last id-0 event without a base, made when needed.
int loose_event(bool make) {
    auto& ev = g_doc.m.events;
    for (int i = (int)ev.size() - 1; i >= 0; --i) {
        if (ev[i].id != 0 || ev[i].area_flag || !ev[i].points.empty()) continue;
        bool base = false;
        for (const lvl::Entity& e : ev[i].ents) base = base || (e.type == lvl::kBase && ev[i].ents.size() >= 6);
        if (!base && !ev[i].ents.empty()) return i;
    }
    if (!make) return -1;
    ev.push_back(lvl::Event{});
    return (int)ev.size() - 1;
}
int brick_event(bool make) {
    auto& ev = g_doc.m.events;
    for (size_t i = 0; i < ev.size(); ++i)
        if (ev[i].id == 0 && !ev[i].picks.empty() && ev[i].ents.empty()) return (int)i;
    if (!make) return -1;
    ev.push_back(lvl::Event{});
    return (int)ev.size() - 1;
}

lvl::Entity ent(int x, int y, int team, uint8_t type, int extra) {
    lvl::Entity e{};
    e.x = (uint8_t)x; e.y = (uint8_t)y; e.team = (uint8_t)team; e.type = type;
    e.subtype = 0; e.health = 100;
    e.e0 = e.e1 = e.e2 = (uint8_t)extra;
    return e;
}

// The map tool's start layout (CreateNewProcessor.createBase): the base at
// the cell, barracks at (-1,+4), farm at (+2,+4), and the hero and two
// builders below the base for a start in the top half, above it otherwise.
std::vector<lvl::Entity> start_layout(int sx, int sy, int team, int map_h) {
    const int uy = sy < map_h / 2 ? 7 : -3;
    std::vector<lvl::Entity> v;
    v.push_back(ent(sx, sy, team, lvl::kBase, 1));
    v.push_back(ent(sx - 1, sy + 4, team, lvl::kBarracks, 1));
    v.push_back(ent(sx + 2, sy + 4, team, lvl::kFarm, 1));
    v.push_back(ent(sx + 1, sy + uy, team, lvl::kHero, 1));
    v.push_back(ent(sx, sy + uy + 1, team, lvl::kBuilder, 1));
    v.push_back(ent(sx + 2, sy + uy + 1, team, lvl::kBuilder, 1));
    return v;
}

void place_start(int x, int y) {
    push_undo();
    const int team = g_ui.team;
    const int i = find_start(team);
    std::vector<lvl::Entity> v = start_layout(x, y, team, g_doc.m.h);
    if (i >= 0) {
        // Keep everything else the record carries; replace its six.
        g_doc.m.events[i].ents = v;
    } else {
        lvl::Event e;
        e.ents = v;
        g_doc.m.events.push_back(std::move(e));
    }
}

// The first water cell of a crossing from (x, y) in the drag's direction.
bool bridge_ok(int x, int y, int horizontal, int len, std::string* why) {
    const int dx = horizontal ? 1 : 0, dy = horizontal ? 0 : 1;
    const int px = horizontal ? 0 : 1, py = horizontal ? 1 : 0;   // the second lane
    auto water = [&](int cx, int cy) { return inside(cx, cy) && g_doc.m.A[at(cx, cy)] == lvl::kWater; };
    auto land = [&](int cx, int cy) {
        return inside(cx, cy) && (g_doc.m.A[at(cx, cy)] == lvl::kGrass || g_doc.m.A[at(cx, cy)] == lvl::kStone);
    };
    for (int lane = 0; lane < 2; ++lane) {
        const int ox = x + px * lane, oy = y + py * lane;
        if (!land(ox - dx, oy - dy) || !land(ox + dx * len, oy + dy * len)) {
            if (why) *why = "both ends of a bridge must be on grass or stone";
            return false;
        }
        for (int k = 0; k < len; ++k)
            if (!water(ox + dx * k, oy + dy * k)) {
                if (why) *why = "a bridge must cross only water";
                return false;
            }
    }
    return true;
}

// Makes a crossing valid: water along the span, stone landings at its ends.
void carve_bridge(int x, int y, int horizontal, int len) {
    const int dx = horizontal ? 1 : 0, dy = horizontal ? 0 : 1;
    const int px = horizontal ? 0 : 1, py = horizontal ? 1 : 0;
    std::vector<std::pair<int, int>> water, stone;
    for (int lane = 0; lane < 2; ++lane) {
        const int ox = x + px * lane, oy = y + py * lane;
        for (int k = 0; k < len; ++k) water.push_back({ox + dx * k, oy + dy * k});
        stone.push_back({ox - dx, oy - dy});
        stone.push_back({ox + dx * len, oy + dy * len});
    }
    for (const auto& c : water) set_cell(c.first, c.second, kMatWater);
    for (const auto& c : stone) set_cell(c.first, c.second, kMatStone);
    const int x1 = x + dx * len + px, y1 = y + dy * len + py;
    retile(x - dx, y - dy, x1, y1);
}

lvl::MarkerGroup& marker_group(uint8_t id) {
    for (lvl::MarkerGroup& g : g_doc.m.markers)
        if (g.id == id) return g;
    g_doc.m.markers.push_back(lvl::MarkerGroup{});
    g_doc.m.markers.back().id = id;
    return g_doc.m.markers.back();
}

void ensure_mine_groups() {
    while (g_doc.m.mines.size() < 4) g_doc.m.mines.push_back({});
}

// Removes the object under a cell (markers, mines, bricks, buildings; a start
// as a whole when its base is hit). True when something went.
bool remove_at(int x, int y) {
    lvl::Map& m = g_doc.m;
    for (lvl::MarkerGroup& g : m.markers)
        for (size_t k = 0; k < g.m.size(); ++k)
            if (g.m[k].x == x && g.m[k].y == y) {
                push_undo();
                g.m.erase(g.m.begin() + (long)k);
                return true;
            }
    for (auto& g : m.mines)
        for (size_t k = 0; k < g.size(); ++k)
            if (x >= g[k].x && x <= g[k].x + 1 && y >= g[k].y && y <= g[k].y + 1) {
                push_undo();
                g.erase(g.begin() + (long)k);
                return true;
            }
    for (size_t i = 0; i < m.events.size(); ++i) {
        lvl::Event& e = m.events[i];
        for (size_t k = 0; k < e.picks.size(); ++k)
            if (e.picks[k].x == x && e.picks[k].y == y) {
                push_undo();
                e.picks.erase(e.picks.begin() + (long)k);
                return true;
            }
        for (size_t k = 0; k < e.ents.size(); ++k) {
            int w, h;
            footprint(e.ents[k].type, &w, &h);
            if (x < e.ents[k].x || x >= e.ents[k].x + w || y < e.ents[k].y || y >= e.ents[k].y + h) continue;
            push_undo();
            const bool start = e.ents[k].type == lvl::kBase && e.ents.size() >= 6;
            if (start) e.ents.clear();
            else e.ents.erase(e.ents.begin() + (long)k);
            if (e.ents.empty() && e.picks.empty() && e.walls.empty() && e.id == 0 && !e.area_flag &&
                e.points.empty())
                m.events.erase(m.events.begin() + (long)i);
            return true;
        }
    }
    return false;
}

// ── Copy and paste ───────────────────────────────────────────────────────────
// A copied block: terrain classes, trees and metatiles cell by cell, and the
// objects inside it (mines, markers, golden bricks, buildings and units that
// are no start) relative to its corner. Starts are not copied: each player
// has one.
struct Clip {
    int w = 0, h = 0, world = -1;
    std::vector<uint8_t> A, T;
    std::vector<uint16_t> E;
    std::vector<lvl::Point> mines;
    std::vector<std::pair<uint8_t, lvl::Marker>> markers;   // group, marker
    std::vector<lvl::Pickup> picks;
    std::vector<lvl::Entity> ents;
};
Clip g_clip;

bool has_selection() { return g_ui.sel_x1 >= g_ui.sel_x0 && g_ui.sel_y1 >= g_ui.sel_y0; }
bool in_sel(int x, int y) {
    return x >= g_ui.sel_x0 && x <= g_ui.sel_x1 && y >= g_ui.sel_y0 && y <= g_ui.sel_y1;
}
bool is_start_event(const lvl::Event& e) {
    return e.ents.size() >= 6 &&
           std::any_of(e.ents.begin(), e.ents.end(), [](const lvl::Entity& x) { return x.type == lvl::kBase; });
}

void copy_selection() {
    if (!has_selection()) return;
    Doc& d = g_doc;
    Clip c;
    c.w = g_ui.sel_x1 - g_ui.sel_x0 + 1;
    c.h = g_ui.sel_y1 - g_ui.sel_y0 + 1;
    c.world = d.world;
    for (int y = g_ui.sel_y0; y <= g_ui.sel_y1; ++y)
        for (int x = g_ui.sel_x0; x <= g_ui.sel_x1; ++x) {
            c.A.push_back(d.m.A[at(x, y)]);
            c.T.push_back(d.trees[at(x, y)]);
            c.E.push_back(d.m.E[at(x, y)]);
        }
    const int ox = g_ui.sel_x0, oy = g_ui.sel_y0;
    for (const auto& g : d.m.mines)
        for (const lvl::Point& p : g)
            if (in_sel(p.x, p.y) && in_sel(p.x + 1, p.y + 1))
                c.mines.push_back({(uint8_t)(p.x - ox), (uint8_t)(p.y - oy)});
    for (const lvl::MarkerGroup& g : d.m.markers)
        for (const lvl::Marker& k : g.m)
            if (in_sel(k.x, k.y)) c.markers.push_back({g.id, {(uint8_t)(k.x - ox), (uint8_t)(k.y - oy), k.flag}});
    for (const lvl::Event& e : d.m.events) {
        for (lvl::Pickup p : e.picks)
            if (in_sel(p.x, p.y)) {
                p.x = (uint8_t)(p.x - ox);
                p.y = (uint8_t)(p.y - oy);
                c.picks.push_back(p);
            }
        if (is_start_event(e)) continue;
        for (lvl::Entity x : e.ents) {
            int w, h;
            footprint(x.type, &w, &h);
            if (!in_sel(x.x, x.y) || !in_sel(x.x + w - 1, x.y + h - 1)) continue;
            x.x = (uint8_t)(x.x - ox);
            x.y = (uint8_t)(x.y - oy);
            c.ents.push_back(x);
        }
    }
    g_clip = std::move(c);
    say(false, "Copied %d x %d cells.", g_clip.w, g_clip.h);
}

// Clears the selection to grass and removes the objects inside it. The
// caller has taken the undo snapshot.
void clear_selection_cells() {
    if (!has_selection()) return;
    Doc& d = g_doc;
    for (int y = g_ui.sel_y0; y <= g_ui.sel_y1; ++y)
        for (int x = g_ui.sel_x0; x <= g_ui.sel_x1; ++x) set_cell(x, y, kMatGrass);
    for (auto& g : d.m.mines)
        g.erase(std::remove_if(g.begin(), g.end(), [](const lvl::Point& p) { return in_sel(p.x, p.y); }), g.end());
    for (lvl::MarkerGroup& g : d.m.markers)
        g.m.erase(std::remove_if(g.m.begin(), g.m.end(), [](const lvl::Marker& k) { return in_sel(k.x, k.y); }),
                  g.m.end());
    for (lvl::Event& e : d.m.events) {
        e.picks.erase(std::remove_if(e.picks.begin(), e.picks.end(),
                                     [](const lvl::Pickup& p) { return in_sel(p.x, p.y); }),
                      e.picks.end());
        if (is_start_event(e)) continue;
        e.ents.erase(std::remove_if(e.ents.begin(), e.ents.end(),
                                    [](const lvl::Entity& x) { return in_sel(x.x, x.y); }),
                     e.ents.end());
    }
    d.m.events.erase(std::remove_if(d.m.events.begin(), d.m.events.end(),
                                    [](const lvl::Event& e) {
                                        return e.id == 0 && !e.area_flag && e.points.empty() && e.ents.empty() &&
                                               e.picks.empty() && e.walls.empty();
                                    }),
                     d.m.events.end());
    retile(g_ui.sel_x0, g_ui.sel_y0, g_ui.sel_x1, g_ui.sel_y1);
    stamp_mines();
}

void delete_selection() {
    if (!has_selection()) return;
    push_undo();
    clear_selection_cells();
}

void cut_selection() {
    if (!has_selection()) return;
    copy_selection();
    push_undo();
    clear_selection_cells();
}

// The clipboard with its corner at (x, y). Cells off the map are dropped.
// Copied metatiles are kept (a remade map's own tiles only within the same
// level's world); the ring around the block is re-picked so it joins in.
void paste_at(int x, int y) {
    if (g_clip.w <= 0) return;
    Doc& d = g_doc;
    push_undo();
    const bool same_world = g_clip.world == d.world;
    for (int cy = 0; cy < g_clip.h; ++cy)
        for (int cx = 0; cx < g_clip.w; ++cx) {
            const int tx = x + cx, ty = y + cy;
            if (!inside(tx, ty)) continue;
            const size_t k = (size_t)cy * g_clip.w + cx;
            d.m.A[at(tx, ty)] = g_clip.A[k];
            d.trees[at(tx, ty)] = g_clip.T[k];
        }
    // Tiles: the copied ones where they are valid here, picked otherwise.
    retile(x, y, x + g_clip.w - 1, y + g_clip.h - 1);
    WorldAssets& wa = world_assets(d.world);
    for (int cy = 0; cy < g_clip.h; ++cy)
        for (int cx = 0; cx < g_clip.w; ++cx) {
            const int tx = x + cx, ty = y + cy;
            const uint16_t e = g_clip.E[(size_t)cy * g_clip.w + cx];
            if (!inside(tx, ty) || !same_world) continue;
            if (e >= kTableMetas && (size_t)(e - kTableMetas) >= d.detail.size()) continue;
            // An interior cell keeps its exact tile; the outer ring stays re-picked.
            if (cx == 0 || cy == 0 || cx == g_clip.w - 1 || cy == g_clip.h - 1) continue;
            d.m.E[at(tx, ty)] = e;
            d.m.B[at(tx, ty)] = (wa.ok && e < kTableMetas) ? wa.e2b[e] : 0;
        }
    auto fits = [&](int px, int py, int w, int h) { return inside(px, py) && inside(px + w - 1, py + h - 1); };
    ensure_mine_groups();
    for (const lvl::Point& p : g_clip.mines)
        if (fits(x + p.x, y + p.y, 2, 2)) d.m.mines[1].push_back({(uint8_t)(x + p.x), (uint8_t)(y + p.y)});
    for (const auto& gm : g_clip.markers)
        if (fits(x + gm.second.x, y + gm.second.y, 1, 1))
            marker_group(gm.first).m.push_back({(uint8_t)(x + gm.second.x), (uint8_t)(y + gm.second.y), gm.second.flag});
    if (!g_clip.picks.empty()) {
        const int i = brick_event(true);
        for (lvl::Pickup p : g_clip.picks)
            if (fits(x + p.x, y + p.y, 1, 1)) {
                p.x = (uint8_t)(x + p.x);
                p.y = (uint8_t)(y + p.y);
                d.m.events[i].picks.push_back(p);
            }
    }
    if (!g_clip.ents.empty()) {
        const int i = loose_event(true);
        for (lvl::Entity e : g_clip.ents) {
            int w, h;
            footprint(e.type, &w, &h);
            if (!fits(x + e.x, y + e.y, w, h)) continue;
            e.x = (uint8_t)(x + e.x);
            e.y = (uint8_t)(y + e.y);
            d.m.events[i].ents.push_back(e);
        }
    }
    stamp_mines();
    mark(x - 1, y - 1, x + g_clip.w, y + g_clip.h);
    g_ui.sel_x0 = std::max(0, x);
    g_ui.sel_y0 = std::max(0, y);
    g_ui.sel_x1 = std::min(d.m.w - 1, x + g_clip.w - 1);
    g_ui.sel_y1 = std::min(d.m.h - 1, y + g_clip.h - 1);
}

// ── Checks ───────────────────────────────────────────────────────────────────
struct Check { bool bad; std::string text; };
std::vector<Check> run_checks() {
    std::vector<Check> out;
    lvl::Map& m = g_doc.m;
    const int starts = start_count();
    if (starts < 2) out.push_back({true, "A Free Play level needs at least two start bases."});
    for (int t = 0; t < 4; ++t) {
        const int i = find_start(t);
        if (i < 0) continue;
        for (const lvl::Entity& e : m.events[i].ents) {
            int w, h;
            footprint(e.type, &w, &h);
            for (int y = e.y; y < e.y + h; ++y)
                for (int x = e.x; x < e.x + w; ++x) {
                    if (!inside(x, y)) {
                        out.push_back({true, std::string(kTeamNames[t]) + "'s start runs off the map."});
                        goto next_team;
                    }
                    const uint8_t a = m.A[at(x, y)];
                    if ((a != lvl::kGrass && a != lvl::kStone) || g_doc.trees[at(x, y)]) {
                        out.push_back({true, std::string(kTeamNames[t]) +
                                                 "'s start stands on water, mountain or trees."});
                        goto next_team;
                    }
                }
        }
    next_team:;
    }
    // Starts must reach each other over land (plane C), or by a bridge site.
    if (starts >= 2) {
        const std::vector<uint8_t> C = lvl::area_fill(m.A, m.w, m.h);
        int first = -1;
        bool split = false;
        for (int t = 0; t < 4; ++t) {
            const int i = find_start(t);
            if (i < 0) continue;
            const lvl::Entity& b = m.events[i].ents[0];
            if (!inside(b.x, b.y)) continue;
            const int c = C[at(b.x, b.y)];
            if (first < 0) first = c;
            else if (c != first) split = true;
        }
        bool bridges = false;
        for (const lvl::MarkerGroup& g : m.markers) bridges = bridges || ((g.id == 7 || g.id == 8) && !g.m.empty());
        if (split)
            out.push_back({!bridges, bridges ? "Some starts are only connected by bridge sites."
                                             : "Some starts cannot reach each other over land."});
    }
    for (const lvl::MarkerGroup& g : m.markers) {
        if (g.id != 7 && g.id != 8) continue;
        for (const lvl::Marker& mk : g.m) {
            int len = 0;
            for (int L : {9, 6, 3})
                if (bridge_ok(mk.x, mk.y, g.id == 7, L, nullptr)) { len = L; break; }
            if (!len) {
                char b[160];
                std::snprintf(b, sizeof b, "The bridge site at %d,%d is not a 3, 6 or 9 cell water crossing.",
                              mk.x, mk.y);
                out.push_back({true, b});
            }
        }
    }
    int mines = 0;
    for (const auto& g : m.mines) mines += (int)g.size();
    if (mines == 0) out.push_back({false, std::string("No ") + kWorlds[g_doc.world].mine +
                                              "s: players will only have trees to harvest."});
    size_t markers0 = 0, markers3 = 0;
    for (const lvl::MarkerGroup& g : m.markers) {
        if (g.id == 0) markers0 += g.m.size();
        if (g.id == 3) markers3 += g.m.size();
    }
    if (markers0 < 4 || markers3 < 4)
        out.push_back({false, "Few AI markers: computer players may play poorly (Objects > AI markers > Place "
                              "automatically)."});
    for (const lvl::Event& e : m.events)
        if (e.ents.size() > 255 || e.picks.size() > 255)
            out.push_back({true, "Too many objects in one group (255 at most)."});
    if (out.empty()) out.push_back({false, "No problems found."});
    return out;
}

// Golden bricks and AI markers, placed for a map that has none (build_map.py's
// rules, simplified): markers on tree clusters and open ground, shipyard
// markers beside water.
void auto_markers() {
    push_undo();
    lvl::Map& m = g_doc.m;
    for (uint8_t id : {0, 3, 6}) marker_group(id).m.clear();
    auto add = [&](uint8_t id, int x, int y, int spacing, size_t cap) {
        lvl::MarkerGroup& g = marker_group(id);
        if (g.m.size() >= cap) return;
        for (const lvl::Marker& k : g.m)
            if (std::abs(k.x - x) < spacing && std::abs(k.y - y) < spacing) return;
        g.m.push_back({(uint8_t)x, (uint8_t)y, 0});
    };
    for (int y = 1; y < m.h - 1; ++y)
        for (int x = 1; x < m.w - 1; ++x) {
            int trees = 0, ground = 0, water = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const size_t j = at(x + dx, y + dy);
                    trees += g_doc.trees[j];
                    ground += (m.A[j] == lvl::kGrass || m.A[j] == lvl::kStone) && !g_doc.trees[j];
                    water += m.A[j] == lvl::kWater;
                }
            if (trees == 9) add(0, x, y, 8, 26);
            else if (ground == 9) add(3, x, y, 10, 22);
            else if (ground >= 3 && water >= 3 && !g_doc.trees[at(x, y)]) add(6, x, y, 12, 10);
        }
}

// ── Load and save ────────────────────────────────────────────────────────────
void canvas_reset() {
    Doc& d = g_doc;
    if (d.tex) SDL_DestroyTexture(d.tex);
    d.tex = nullptr;
    d.tex_w = d.tex_h = 0;
    d.px.clear();
    mark(0, 0, d.m.w - 1, d.m.h - 1);
}

void adopt(lvl::Map&& m, std::vector<Meta>&& detail, const char* name) {
    Doc& d = g_doc;
    d.m = std::move(m);
    d.trees = lvl::trees_decode(d.m.D, d.m.w * d.m.h);
    d.detail = std::move(detail);
    d.world = world_of(d.m.tileset);
    ensure_mine_groups();
    d.undo.clear();
    d.redo.clear();
    d.open = true;
    d.dirty = false;
    if (name) std::snprintf(d.name, sizeof d.name, "%s", name);
    canvas_reset();
    g_ui.zoom = 1.0f;
    g_ui.pan = ImVec2(0, 0);
}

void new_map(int w, int h, int world) {
    lvl::Map m;
    m.w = std::clamp(w, kMinSize, kMaxSize);
    m.h = std::clamp(h, kMinSize, kMaxSize);
    m.tileset = kWorlds[world].tileset;
    const size_t cells = (size_t)m.w * m.h;
    m.A.assign(cells, lvl::kGrass);
    m.B.assign(cells, 0);
    m.C.assign(cells, 0);
    m.E.assign(cells, 116);
    m.D = lvl::trees_encode(std::vector<uint8_t>(cells, 0));
    m.mines.assign(4, {});
    adopt(std::move(m), {}, "My level");
    // A two-cell tree border, as the map tool's template has.
    for (int y = 0; y < g_doc.m.h; ++y)
        for (int x = 0; x < g_doc.m.w; ++x)
            if (x < 2 || y < 2 || x >= g_doc.m.w - 2 || y >= g_doc.m.h - 2) set_cell(x, y, kMatTrees);
    retile(0, 0, g_doc.m.w - 1, g_doc.m.h - 1);
    g_doc.dirty = false;
}

bool load_bytes(std::vector<uint8_t> raw, std::vector<uint8_t> tbp, const char* name) {
    if (!lb_mods_unwrap(&raw)) {
        say(true, "That map is not in the game's format.");
        return false;
    }
    lvl::Map m;
    std::string why;
    if (!lvl::parse(raw.data(), raw.size(), &m, &why)) {
        say(true, "That map could not be read: %s.", why.c_str());
        return false;
    }
    if (m.w > kMaxSize || m.h > kMaxSize) {
        say(true, "That map is larger than %dx%d, which the editor does not handle.", kMaxSize, kMaxSize);
        return false;
    }
    std::vector<Meta> detail;
    if (!tbp.empty() && lb_mods_unwrap(&tbp)) detail = read_tbp(tbp);
    adopt(std::move(m), std::move(detail), name);
    say(false, "Opened %s.", name);
    return true;
}

bool load_stock(int k) {
    const StockMap& s = kStock[k];
    std::vector<uint8_t> raw, tbp;
    const int n = lb_rom_read_unwrapped((std::string("Maps/") + s.stem + ".map").c_str(), nullptr, 0);
    if (n <= 0) {
        say(true, "The cartridge has no %s.", s.stem);
        return false;
    }
    raw = rom_file(std::string("Maps/") + s.stem + ".map");
    tbp = rom_file(std::string("BP/DetailTiles_") + s.stem + ".tbp");
    // Already decompressed: wrap so load_bytes' unwrap is a no-op either way.
    return load_bytes(raw, tbp, s.stem);
}

std::vector<uint8_t> slurp(const fs::path& p, size_t cap) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return b;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0 && (size_t)n <= cap) {
        b.resize((size_t)n);
        if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    }
    std::fclose(f);
    return b;
}

std::string toml_value(const std::string& toml, const char* key) {
    const size_t k = toml.find(key);
    if (k == std::string::npos) return {};
    const size_t q0 = toml.find('"', k);
    if (q0 == std::string::npos) return {};
    const size_t q1 = toml.find('"', q0 + 1);
    if (q1 == std::string::npos || q1 - q0 > 64) return {};
    return toml.substr(q0 + 1, q1 - q0 - 1);
}

bool load_file(const char* path) {
    const fs::path p = fs::u8path(path);
    std::string ext = p.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    const std::vector<uint8_t> b = slurp(p, 16u << 20);
    if (b.empty()) {
        say(true, "Could not read that file.");
        return false;
    }
    if (ext == ".lbz") {
        std::string map, tbp, manifest;
        if (!lb_lbz_unpack(std::string(b.begin(), b.end()), &map, &tbp, &manifest)) {
            say(true, "That .lbz holds no map.");
            return false;
        }
        std::string name = toml_value(manifest, "name");
        if (name.empty()) name = p.stem().string();
        const bool ok = load_bytes(std::vector<uint8_t>(map.begin(), map.end()),
                                   std::vector<uint8_t>(tbp.begin(), tbp.end()), name.c_str());
        const std::string who = toml_value(manifest, "creator");
        if (ok && !who.empty()) std::snprintf(g_doc.author, sizeof g_doc.author, "%s", who.c_str());
        return ok;
    }
    // A loose .map: its folder's detail tiles, when there are any.
    std::vector<uint8_t> tbp;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
        std::string n = e.path().filename().string();
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.rfind("detailtiles_", 0) == 0 && e.path().extension() == ".tbp") tbp = slurp(e.path(), 8u << 20);
    }
    const std::string name = p.parent_path().filename().string();
    return load_bytes(b, tbp, name.empty() ? p.stem().string().c_str() : name.c_str());
}

// The map ready to ship: derived planes rebuilt.
std::vector<uint8_t> finish_map() {
    Doc& d = g_doc;
    d.m.tileset = kWorlds[d.world].tileset;
    d.m.D = lvl::trees_encode(d.trees);
    d.m.C = lvl::area_fill(d.m.A, d.m.w, d.m.h);
    ensure_mine_groups();
    return lvl::write(d.m);
}

// A folder name from the level's name: letters, digits, space, - and _.
std::string safe_name(const char* s) {
    std::string out;
    for (const char* p = s; *p && out.size() < 40; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_') out += (char)c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Untitled level") : out;
}

struct Files { std::vector<uint8_t> map, mini[4], tbp; };
Files build_files() {
    Files f;
    f.map = lvl::comp_wrap(finish_map());
    const char order[4][2] = {{1, 1}, {1, 0}, {0, 1}, {0, 0}};   // mini.NCGR, mini.NCBR, miniNT.NCGR, miniNT.NCBR
    for (int k = 0; k < 4; ++k)
        f.mini[k] = lvl::comp_wrap(lvl::minimap(g_doc.m, g_doc.trees, order[k][0] != 0, order[k][1] != 0));
    if (!g_doc.detail.empty()) f.tbp = lvl::comp_wrap(write_tbp(g_doc.detail));
    return f;
}
const char* const kMiniSuffix[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR", "miniNT.NCBR"};

bool write_file(const fs::path& p, const std::vector<uint8_t>& b) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

fs::path level_folder() {
    return fs::u8path(lb_mods_dir()) / "maps" / "Level Editor" / fs::u8path(safe_name(g_doc.name));
}

bool save_folder() {
    const Files f = build_files();
    const fs::path dir = level_folder();
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Only this editor's files: a folder that holds anything else is refused.
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        const bool ours = n == "map.map" || n == "DetailTiles_map.tbp" || n == "level.txt" ||
                          (n.rfind("map", 0) == 0 && (n.find("mini") != std::string::npos));
        if (!ours) {
            say(true, "%s holds other files; choose another level name.", dir.string().c_str());
            return false;
        }
    }
    bool ok = write_file(dir / "map.map", f.map);
    for (int k = 0; k < 4; ++k) ok = ok && write_file(dir / (std::string("map") + kMiniSuffix[k]), f.mini[k]);
    if (!f.tbp.empty()) ok = ok && write_file(dir / "DetailTiles_map.tbp", f.tbp);
    else fs::remove(dir / "DetailTiles_map.tbp", ec);
    const std::string meta = std::string("name=") + g_doc.name + "\nauthor=" + g_doc.author + "\n";
    ok = ok && write_file(dir / "level.txt", std::vector<uint8_t>(meta.begin(), meta.end()));
    if (!ok) {
        say(true, "Could not write %s.", dir.string().c_str());
        return false;
    }
    g_doc.dirty = false;
    say(false, "Saved to %s. Load new maps to play it.", dir.string().c_str());
    return true;
}

// A zip of stored entries (the map tool's .lbz layout).
std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> out, cd;
    auto p16 = [](std::vector<uint8_t>& o, uint32_t v) { o.push_back(v & 0xFF); o.push_back((v >> 8) & 0xFF); };
    auto p32 = [&](std::vector<uint8_t>& o, uint32_t v) { p16(o, v & 0xFFFF); p16(o, v >> 16); };
    for (const auto& f : files) {
        const uint32_t crc = (uint32_t)crc32(0L, f.second.data(), (uInt)f.second.size());
        const uint32_t off = (uint32_t)out.size();
        p32(out, 0x04034B50); p16(out, 20); p16(out, 0); p16(out, 0); p16(out, 0); p16(out, 0x21);
        p32(out, crc); p32(out, (uint32_t)f.second.size()); p32(out, (uint32_t)f.second.size());
        p16(out, (uint32_t)f.first.size()); p16(out, 0);
        out.insert(out.end(), f.first.begin(), f.first.end());
        out.insert(out.end(), f.second.begin(), f.second.end());
        p32(cd, 0x02014B50); p16(cd, 20); p16(cd, 20); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0x21);
        p32(cd, crc); p32(cd, (uint32_t)f.second.size()); p32(cd, (uint32_t)f.second.size());
        p16(cd, (uint32_t)f.first.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0);
        p32(cd, off);
        cd.insert(cd.end(), f.first.begin(), f.first.end());
    }
    const uint32_t cd_off = (uint32_t)out.size();
    out.insert(out.end(), cd.begin(), cd.end());
    p32(out, 0x06054B50); p16(out, 0); p16(out, 0); p16(out, (uint32_t)files.size());
    p16(out, (uint32_t)files.size()); p32(out, (uint32_t)cd.size()); p32(out, cd_off); p16(out, 0);
    return out;
}

// TOML string: printable ASCII only, quotes and backslashes dropped.
std::string toml_safe(const char* s) {
    std::string o;
    for (const char* p = s; *p; ++p)
        if (*p >= 0x20 && *p < 0x7F && *p != '"' && *p != '\\') o += *p;
    return o;
}

std::vector<uint8_t> build_lbz() {
    const Files f = build_files();
    std::vector<std::pair<std::string, std::vector<uint8_t>>> z;
    z.push_back({"map.map", f.map});
    const char* names[4] = {"mapimages/@mini.NCGR", "mapimages/@mini.NCBR", "mapimages/@miniNT.NCGR",
                            "mapimages/@miniNT.NCBR"};
    for (int k = 0; k < 4; ++k) z.push_back({names[k], f.mini[k]});
    if (!f.tbp.empty()) z.push_back({"detailtiles/detailtiles_@.tbp", f.tbp});
    const std::string man = "type = \"map\"\nname = \"" + toml_safe(g_doc.name) + "\"\ncreator = \"" +
                            toml_safe(g_doc.author) + "\"\nargs = [ \"mp01\" ]\n";
    z.push_back({"manifest.toml", std::vector<uint8_t>(man.begin(), man.end())});
    return make_zip(z);
}

// ── Community levels: the master server's library ────────────────────────────
//
// Uploads go to the master server (services/master-server-cloudflare,
// src/levels.js), where the moderator approves them in Discord; approved ones
// are listed on Maps > Community levels. Nothing a reply holds becomes a path:
// a download is parsed and checked (lvl::validate) and written under fixed
// file names in a folder named from its sanitised name and id.

// A small JSON reader: objects, arrays, strings (with escapes), numbers,
// true/false/null; depth and length bounded. For the server's replies, which
// carry player-typed names, so the overlay's key scanner is not used here.
struct JVal {
    enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (const auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k) const {
        const JVal* v = get(k);
        return v && v->kind == kStr ? v->s : std::string();
    }
    double num(const char* k) const {
        const JVal* v = get(k);
        return v && v->kind == kNum ? v->n : 0.0;
    }
    bool flag(const char* k) const {
        const JVal* v = get(k);
        return v && v->kind == kBool && v->b;
    }
};
struct JParse {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool lit(const char* w) {
        const size_t k = std::strlen(w);
        if ((size_t)(e - p) < k || std::strncmp(p, w, k) != 0) return false;
        p += k;
        return true;
    }
    bool str(std::string* out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= e) return false;
                c = *p++;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'r') c = '\r';
                else if (c == 'b' || c == 'f') c = ' ';
                else if (c == 'u') {
                    if (e - p < 4) return false;
                    unsigned v = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = *p++;
                        v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                                 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    }
                    c = v < 0x80 ? (char)v : '?';   // names are ASCII by the server's rule
                }
            }
            if (out->size() < (1u << 20)) out->push_back(c);
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool val(JVal* v) {
        ws();
        if (p >= e || ++depth > 16) return false;
        bool ok = true;
        if (*p == '{') {
            v->kind = JVal::kObj;
            ++p;
            ws();
            if (p < e && *p == '}') ++p;
            else
                for (;;) {
                    std::string k;
                    ws();
                    JVal x;
                    if (!str(&k)) { ok = false; break; }
                    ws();
                    if (p >= e || *p != ':') { ok = false; break; }
                    ++p;
                    if (!val(&x)) { ok = false; break; }
                    if (v->o.size() < 4096) v->o.push_back({k, std::move(x)});
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == '}') { ++p; break; }
                    ok = false;
                    break;
                }
        } else if (*p == '[') {
            v->kind = JVal::kArr;
            ++p;
            ws();
            if (p < e && *p == ']') ++p;
            else
                for (;;) {
                    JVal x;
                    if (!val(&x)) { ok = false; break; }
                    if (v->a.size() < 4096) v->a.push_back(std::move(x));
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == ']') { ++p; break; }
                    ok = false;
                    break;
                }
        } else if (*p == '"') {
            v->kind = JVal::kStr;
            ok = str(&v->s);
        } else if (lit("true")) {
            v->kind = JVal::kBool;
            v->b = true;
        } else if (lit("false")) {
            v->kind = JVal::kBool;
        } else if (lit("null")) {
            v->kind = JVal::kNull;
        } else {
            char* end = nullptr;
            const std::string num(p, (size_t)std::min<ptrdiff_t>(e - p, 32));
            v->n = std::strtod(num.c_str(), &end);
            if (end == num.c_str()) ok = false;
            else {
                v->kind = JVal::kNum;
                p += end - num.c_str();
            }
        }
        --depth;
        return ok;
    }
};
bool json_parse(const std::string& text, JVal* out) {
    JParse j{text.data(), text.data() + text.size()};
    return j.val(out);
}

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

// The server's name rule (src/levels.js NAME_OK): 1..32 of letters, digits,
// space, _ - ' . ! ?, and no swearing.
extern "C" int lb_swear_filter(const char* in, char* out, int cap);   // lb_chat.cpp
bool name_ok(const char* s, bool empty_ok) {
    const size_t n = std::strlen(s);
    if (n == 0) return empty_ok;
    if (n > 32) return false;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (!(std::isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '\'' || c == '.' || c == '!' || c == '?'))
            return false;
    }
    char tmp[64];
    return lb_swear_filter(s, tmp, sizeof tmp) == 0;
}

extern "C" int lb_net_http_get(const char* host, int port, const char* path, char* out, int cap);
extern "C" int lb_net_http_post(const char* host, int port, const char* path, const char* body, char* out,
                                int cap);
extern "C" const char* lb_setting_player_key(void);
extern "C" void lb_setting_master(char* host, int cap, int* port);

// One request at a time, on a worker thread; the page polls its result.
struct Net {
    std::mutex mu;
    std::atomic<bool> busy{false};
    int done_kind = 0;   // 1 upload, 2 list, 3 preview, 4 download
    bool done = false;
    std::string reply;
    bool failed = false;
};
Net g_net;

void net_run(int kind, std::string method, std::string path, std::string body) {
    char host[128];
    int port = 80;
    lb_setting_master(host, sizeof host, &port);
    std::vector<char> buf(1u << 20);
    const int n = method == "POST"
                      ? lb_net_http_post(host, port, path.c_str(), body.c_str(), buf.data(), (int)buf.size())
                      : lb_net_http_get(host, port, path.c_str(), buf.data(), (int)buf.size());
    std::lock_guard<std::mutex> lk(g_net.mu);
    g_net.done_kind = kind;
    g_net.done = true;
    g_net.failed = n < 0;
    g_net.reply.assign(buf.data(), n < 0 ? std::strlen(buf.data()) : (size_t)n);
    g_net.busy = false;
}
bool net_start(int kind, const char* method, const std::string& path, const std::string& body) {
    if (g_net.busy.exchange(true)) return false;
    std::thread(net_run, kind, std::string(method), path, body).detach();
    return true;
}

// The upload: the raw map and detail tiles, as the server wants them.
void start_upload() {
    std::string why;
    if (!name_ok(g_doc.name, false)) {
        say(true, "Give the level a name of 1-32 letters, digits, spaces or _-'.!? (no swearing).");
        return;
    }
    if (!name_ok(g_doc.author, true)) {
        say(true, "The author name may use 1-32 letters, digits, spaces or _-'.!? (no swearing).");
        return;
    }
    for (const Check& c : run_checks())
        if (c.bad) {
            say(true, "Fix the problems under Checks first: %s", c.text.c_str());
            return;
        }
    const std::vector<uint8_t> raw = finish_map();
    if (!lvl::validate(g_doc.m, g_doc.detail.size(), &why)) {
        say(true, "This level cannot be shared (%s).", why.c_str());
        return;
    }
    const std::string body = std::string("{\"key\":\"") + lb_setting_player_key() + "\",\"name\":\"" + g_doc.name +
                             "\",\"author\":\"" + g_doc.author + "\",\"map\":\"" + b64_encode(raw) +
                             "\",\"tbp\":\"" + (g_doc.detail.empty() ? std::string() : b64_encode(write_tbp(g_doc.detail))) +
                             "\"}";
    if (net_start(1, "POST", "/levels/upload", body)) say(false, "Uploading...");
}

struct Row { int id, players, world, w, h, downloads; std::string name, author; };
struct Community {
    std::vector<Row> rows;
    int pick = -1;
    std::string message;
    bool bad = false;
    bool listed = false;
    // The picked level, fetched for its preview.
    int shown_id = 0;
    lvl::Map map;
    std::vector<Meta> detail;
    std::vector<uint8_t> raw_map, raw_tbp;
    SDL_Texture* tex = nullptr;
    SDL_Renderer* tex_ren = nullptr;
};
Community g_com;

void com_preview_texture(SDL_Renderer* ren) {
    Community& c = g_com;
    if (c.tex) SDL_DestroyTexture(c.tex);
    c.tex = nullptr;
    if (!ren || c.map.w <= 0) return;
    const std::vector<uint8_t> trees = lvl::trees_decode(c.map.D, c.map.w * c.map.h);
    std::vector<uint32_t> px((size_t)c.map.w * c.map.h);
    for (size_t i = 0; i < px.size(); ++i) {
        const uint8_t a = c.map.A[i];
        px[i] = trees[i] ? 0xFF1E5F19u : a == lvl::kWater ? 0xFFC86932u : a == lvl::kMountain ? 0xFF3C506Eu
              : a == lvl::kStone ? 0xFF788C96u : 0xFF3C9646u;
    }
    c.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, c.map.w, c.map.h);
    if (c.tex) {
        SDL_UpdateTexture(c.tex, nullptr, px.data(), c.map.w * 4);
        SDL_SetTextureScaleMode(c.tex, SDL_ScaleModeNearest);
    }
    c.tex_ren = ren;
}

// A fetched level, decoded and checked: true when it may be used.
bool com_take(const JVal& j) {
    Community& c = g_com;
    std::vector<uint8_t> m, t;
    std::string why;
    if (!b64_decode(j.str("map"), &m, 256u << 10) || !b64_decode(j.str("tbp"), &t, 64u << 10)) {
        c.message = "The server sent a damaged level.";
        c.bad = true;
        return false;
    }
    lvl::Map map;
    std::vector<Meta> detail = t.empty() ? std::vector<Meta>() : read_tbp(t);
    if (!t.empty() && detail.empty()) {
        c.message = "The server sent damaged detail tiles.";
        c.bad = true;
        return false;
    }
    if (!lvl::parse(m.data(), m.size(), &map, &why) || !lvl::validate(map, detail.size(), &why)) {
        c.message = "That level failed the checks (" + why + ") and was not used.";
        c.bad = true;
        return false;
    }
    c.map = std::move(map);
    c.detail = std::move(detail);
    c.raw_map = std::move(m);
    c.raw_tbp = std::move(t);
    c.shown_id = (int)j.num("id");
    return true;
}

// Writes the checked level to mods/maps/Community/<name> (<id>)/.
bool com_install(const Row& r) {
    Community& c = g_com;
    const fs::path dir = fs::u8path(lb_mods_dir()) / "maps" / "Community" /
                         fs::u8path(safe_name(r.name.c_str()) + " (" + std::to_string(r.id) + ")");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::vector<uint8_t> trees = lvl::trees_decode(c.map.D, c.map.w * c.map.h);
    bool ok = write_file(dir / "map.map", lvl::comp_wrap(c.raw_map));
    const char order[4][2] = {{1, 1}, {1, 0}, {0, 1}, {0, 0}};
    for (int k = 0; k < 4; ++k)
        ok = ok && write_file(dir / (std::string("map") + kMiniSuffix[k]),
                              lvl::comp_wrap(lvl::minimap(c.map, trees, order[k][0] != 0, order[k][1] != 0)));
    if (!c.raw_tbp.empty()) ok = ok && write_file(dir / "DetailTiles_map.tbp", lvl::comp_wrap(c.raw_tbp));
    return ok;
}

void net_poll(SDL_Renderer* ren) {
    std::string reply;
    int kind = 0;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lk(g_net.mu);
        if (!g_net.done) return;
        g_net.done = false;
        reply = g_net.reply;
        kind = g_net.done_kind;
        failed = g_net.failed;
    }
    JVal j;
    const bool parsed = !failed && json_parse(reply, &j) && j.kind == JVal::kObj;
    const std::string err = parsed ? j.str("error") : std::string();
    if (kind == 1) {
        if (!parsed) say(true, "Upload failed: %s", failed ? reply.c_str() : "the server's reply was not understood");
        else if (!j.flag("ok")) say(true, "Upload refused: %s.", err.c_str());
        else if (j.str("status") == "approved") say(false, "Uploaded and published as level #%d.", (int)j.num("id"));
        else say(false, "Uploaded as level #%d. It appears under Community levels once the moderator approves it.",
                 (int)j.num("id"));
        return;
    }
    Community& c = g_com;
    c.bad = false;
    if (!parsed || !j.flag("ok")) {
        c.message = !parsed ? (failed ? "Could not reach the master server: " + reply
                                      : std::string("The server's reply was not understood."))
                            : (err == "levels not configured" ? std::string("Community levels need the main master server.")
                                                              : "The server said: " + err);
        c.bad = true;
        return;
    }
    if (kind == 2) {
        c.rows.clear();
        if (const JVal* a = j.get("levels"))
            for (const JVal& v : a->a) {
                Row r;
                r.id = (int)v.num("id");
                r.players = (int)v.num("players");
                r.world = std::clamp((int)v.num("world"), 0, 2);
                r.w = (int)v.num("width");
                r.h = (int)v.num("height");
                r.downloads = (int)v.num("downloads");
                r.name = v.str("name").substr(0, 32);
                r.author = v.str("author").substr(0, 32);
                if (r.id > 0) c.rows.push_back(r);
            }
        c.listed = true;
        c.pick = -1;
        c.message = c.rows.empty() ? "No community levels yet." : "";
    } else if (kind == 3 || kind == 4) {
        if (!com_take(j)) return;
        com_preview_texture(ren);
        if (kind == 4) {
            const Row* r = nullptr;
            for (const Row& x : c.rows)
                if (x.id == c.shown_id) r = &x;
            if (r && com_install(*r))
                c.message = "Downloaded to mods/maps/Community. Load new maps now to play it.";
            else {
                c.message = "Could not write the level to the mods folder.";
                c.bad = true;
            }
        }
    }
}

// ── Canvas ───────────────────────────────────────────────────────────────────
void compose_cells(SDL_Renderer* ren) {
    Doc& d = g_doc;
    WorldAssets& wa = world_assets(d.world);
    const int tw = d.m.w * kMetaW, th = d.m.h * kMetaH;
    if (!d.tex || d.tex_ren != ren || d.tex_w != tw || d.tex_h != th) {
        if (d.tex) SDL_DestroyTexture(d.tex);
        d.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (d.tex) SDL_SetTextureScaleMode(d.tex, SDL_ScaleModeNearest);
        d.tex_ren = ren;
        d.tex_w = tw;
        d.tex_h = th;
        d.px.assign((size_t)tw * th, 0xFF000000u);
        d.dirty_x0 = 0; d.dirty_y0 = 0; d.dirty_x1 = d.m.w - 1; d.dirty_y1 = d.m.h - 1;
    }
    if (!d.tex || d.dirty_x1 < d.dirty_x0) return;
    for (int y = d.dirty_y0; y <= d.dirty_y1; ++y)
        for (int x = d.dirty_x0; x <= d.dirty_x1; ++x) {
            uint32_t* dst = d.px.data() + (size_t)y * kMetaH * tw + (size_t)x * kMetaW;
            const uint16_t e = d.m.E[at(x, y)];
            if (wa.ok && e < kTableMetas) {
                draw_meta(wa, wa.table[e], dst, tw);
            } else if (wa.ok && e >= kTableMetas && (size_t)(e - kTableMetas) < d.detail.size()) {
                draw_meta(wa, d.detail[e - kTableMetas], dst, tw);
            } else {
                const uint8_t a = d.m.A[at(x, y)];
                const uint32_t c = a == lvl::kWater ? 0xFFC86E32u : a == lvl::kMountain ? 0xFF3C506Eu
                                 : a == lvl::kStone ? 0xFF788C96u : 0xFF3CA046u;
                for (int r = 0; r < kMetaH; ++r)
                    for (int q = 0; q < kMetaW; ++q) dst[(size_t)r * tw + q] = c;
            }
            // Trees are not in plane E: the game spawns one per masked cell
            // (FUN_020A30E8) and draws it as a sprite. A canopy stands in.
            if (d.trees[at(x, y)]) {
                const uint32_t dark = d.world == 2 ? 0xFF3A6A1Eu : 0xFF1E5A1Eu;
                const uint32_t mid = d.world == 2 ? 0xFF4E8C2Au : 0xFF2C8030u;
                const uint32_t lite = d.world == 2 ? 0xFF6CB040u : 0xFF46A848u;
                for (int r = 0; r < kMetaH; ++r)
                    for (int q = 0; q < kMetaW; ++q) {
                        // Two overlapping crowns per cell, lit from the top left.
                        const float c1 = std::hypot((q - 7.5f) / 7.5f, (r - 7.0f) / 7.0f);
                        const float c2 = std::hypot((q - 16.5f) / 7.5f, (r - 9.0f) / 7.0f);
                        const float c = std::min(c1, c2);
                        if (c > 1.0f) continue;
                        const float hl = c1 < c2 ? std::hypot((q - 5.5f) / 4.0f, (r - 4.5f) / 3.5f)
                                                 : std::hypot((q - 14.5f) / 4.0f, (r - 6.5f) / 3.5f);
                        dst[(size_t)r * tw + q] = c > 0.82f ? dark : hl < 1.0f ? lite : mid;
                    }
            }
        }
    SDL_Rect r{d.dirty_x0 * kMetaW, d.dirty_y0 * kMetaH, (d.dirty_x1 - d.dirty_x0 + 1) * kMetaW,
               (d.dirty_y1 - d.dirty_y0 + 1) * kMetaH};
    SDL_UpdateTexture(d.tex, &r, d.px.data() + (size_t)r.y * tw + r.x, tw * 4);
    d.dirty_x1 = d.dirty_x0 - 1;
}

void draw_ghost_start(ImDrawList* dl, ImVec2 o, float cw, float ch, int sx, int sy, int team, bool ghost) {
    const ImU32 col = kTeamColours[team & 3];
    const ImU32 fill = (col & 0x00FFFFFFu) | (ghost ? 0x50000000u : 0x70000000u);
    for (const lvl::Entity& e : start_layout(sx, sy, team, g_doc.m.h)) {
        int w, h;
        footprint(e.type, &w, &h);
        const ImVec2 a(o.x + e.x * cw, o.y + e.y * ch), b(o.x + (e.x + w) * cw, o.y + (e.y + h) * ch);
        if (w > 1) {
            dl->AddRectFilled(a, b, fill, 3.0f);
            dl->AddRect(a, b, col, 3.0f, 0, 2.0f);
            const char* label = e.type == lvl::kBase ? "Base" : e.type == lvl::kFarm ? "Farm" : "Barracks";
            dl->AddText(ImVec2(a.x + 3, a.y + 2), IM_COL32(255, 255, 255, ghost ? 160 : 230), label);
        } else {
            dl->AddCircleFilled(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), std::max(3.0f, ch * 0.35f), col);
            dl->AddText(ImVec2(b.x + 1, a.y), IM_COL32(255, 255, 255, ghost ? 140 : 200),
                        e.type == lvl::kHero ? "Hero" : "Builder");
        }
    }
}

void draw_objects(ImDrawList* dl, ImVec2 o, float cw, float ch) {
    lvl::Map& m = g_doc.m;
    // Mines.
    for (const auto& g : m.mines)
        for (const lvl::Point& p : g) {
            const ImVec2 a(o.x + p.x * cw, o.y + p.y * ch), b(o.x + (p.x + 2) * cw, o.y + (p.y + 2) * ch);
            dl->AddRect(a, b, IM_COL32(255, 215, 0, 255), 2.0f, 0, 2.0f);
            dl->AddText(ImVec2(a.x + 2, a.y + 1), IM_COL32(255, 215, 0, 255), kWorlds[g_doc.world].mine);
        }
    // Markers.
    if (g_ui.show_markers)
        for (const lvl::MarkerGroup& g : m.markers)
            for (const lvl::Marker& k : g.m) {
                const ImVec2 c(o.x + (k.x + 0.5f) * cw, o.y + (k.y + 0.5f) * ch);
                if (g.id == 7 || g.id == 8) {
                    int len = 0;
                    for (int L : {9, 6, 3})
                        if (bridge_ok(k.x, k.y, g.id == 7, L, nullptr)) { len = L; break; }
                    const int L = len ? len : 3;
                    const ImVec2 a(o.x + k.x * cw, o.y + k.y * ch);
                    const ImVec2 b = g.id == 7 ? ImVec2(o.x + (k.x + L) * cw, o.y + (k.y + 2) * ch)
                                               : ImVec2(o.x + (k.x + 2) * cw, o.y + (k.y + L) * ch);
                    const ImU32 col = len ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 80, 80, 255);
                    dl->AddRectFilled(a, b, (col & 0x00FFFFFFu) | 0x40000000u);
                    dl->AddRect(a, b, col, 0, 0, 2.0f);
                    dl->AddText(ImVec2(a.x + 2, a.y + 1), col, len ? "Bridge" : "Bridge?");
                    continue;
                }
                const ImU32 col = g.id == 0 ? IM_COL32(60, 255, 60, 220) : g.id == 3 ? IM_COL32(255, 160, 60, 220)
                                : g.id == 6 ? IM_COL32(80, 200, 255, 220) : IM_COL32(200, 200, 200, 200);
                const float r = std::max(3.0f, ch * 0.3f);
                dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                                  ImVec2(c.x - r, c.y), col);
            }
    // Events: starts, buildings, bricks, walls.
    for (const lvl::Event& e : m.events) {
        const bool start = e.ents.size() >= 6 &&
                           std::any_of(e.ents.begin(), e.ents.end(), [](const lvl::Entity& x) { return x.type == lvl::kBase; });
        if (start) {
            draw_ghost_start(dl, o, cw, ch, e.ents[0].x, e.ents[0].y, e.ents[0].team, false);
            for (size_t k = 6; k < e.ents.size(); ++k) {
                const lvl::Entity& x = e.ents[k];
                const ImVec2 c(o.x + (x.x + 0.5f) * cw, o.y + (x.y + 0.5f) * ch);
                dl->AddCircle(c, std::max(3.0f, ch * 0.35f), kTeamColours[x.team & 3], 0, 2.0f);
            }
        } else {
            for (const lvl::Entity& x : e.ents) {
                int w, h;
                footprint(x.type, &w, &h);
                const ImVec2 a(o.x + x.x * cw, o.y + x.y * ch), b(o.x + (x.x + w) * cw, o.y + (x.y + h) * ch);
                const ImU32 col = x.team < 4 ? kTeamColours[x.team] : IM_COL32(200, 200, 200, 255);
                dl->AddRectFilled(a, b, (col & 0x00FFFFFFu) | 0x50000000u, 2.0f);
                dl->AddRect(a, b, col, 2.0f, 0, 1.5f);
                const EntityKind* k = kind_of(x.type);
                if (cw >= 12) dl->AddText(ImVec2(a.x + 2, a.y + 1), IM_COL32(255, 255, 255, 220), k ? k->name : "?");
            }
        }
        for (const lvl::Pickup& p : e.picks) {
            const ImVec2 c(o.x + (p.x + 0.5f) * cw, o.y + (p.y + 0.5f) * ch);
            dl->AddCircleFilled(c, std::max(3.0f, ch * 0.3f), IM_COL32(255, 200, 0, 255));
            dl->AddCircle(c, std::max(3.0f, ch * 0.3f), IM_COL32(120, 80, 0, 255));
        }
        for (const lvl::Wall& wl : e.walls) {
            const ImVec2 a(o.x + wl.x * cw, o.y + wl.y * ch);
            dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), IM_COL32(160, 160, 160, 180));
        }
        if (e.area_flag) {
            const ImVec2 a(o.x + e.area[0] * cw, o.y + e.area[1] * ch);
            const ImVec2 b(o.x + (e.area[2] + 1) * cw, o.y + (e.area[3] + 1) * ch);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 90), 0, 0, 1.0f);
        }
    }
    for (const lvl::Trigger& t : m.triggers) {
        if (!t.flag) continue;
        const ImVec2 a(o.x + t.body[0] * cw, o.y + t.body[1] * ch);
        const ImVec2 b(o.x + (t.body[2] + 1) * cw, o.y + (t.body[3] + 1) * ch);
        dl->AddRect(a, b, IM_COL32(255, 0, 255, 140), 0, 0, 1.0f);
    }
}

void apply_click(int x, int y, bool first) {
    Doc& d = g_doc;
    switch (g_ui.tool) {
    case kToolBrush: {
        // A stroke joins the cells the pointer passed between frames, so a
        // fast drag paints a line, not dots.
        static int last_x = 0, last_y = 0;
        if (first) {
            push_undo();
            last_x = x;
            last_y = y;
        }
        if (!first && x == last_x && y == last_y) break;
        const std::vector<std::pair<int, int>> cells = shape_cells(kToolLine, last_x, last_y, x, y);
        paint_cells(cells, g_ui.material);
        last_x = x;
        last_y = y;
        break;
    }
    case kToolFill:
        if (!first) break;
        push_undo();
        flood(x, y, g_ui.material);
        break;
    case kToolPick: {
        if (!first) break;
        const uint8_t a = d.m.A[at(x, y)];
        g_ui.material = d.trees[at(x, y)] ? kMatTrees : a == lvl::kStone ? kMatStone : a == lvl::kWater ? kMatWater
                      : a == lvl::kMountain ? kMatMountain : kMatGrass;
        g_ui.tool = kToolBrush;
        break;
    }
    case kToolStart:
        if (first) place_start(x, y);
        break;
    case kToolMine: {
        if (!first || !inside(x + 1, y + 1)) break;
        push_undo();
        ensure_mine_groups();
        d.m.mines[1].push_back({(uint8_t)x, (uint8_t)y});
        stamp_mines();
        retile(x - 1, y - 1, x + 2, y + 2);
        stamp_mines();
        break;
    }
    case kToolMarker:
        if (!first) break;
        push_undo();
        marker_group((uint8_t)(g_ui.marker_group == 0 ? 0 : g_ui.marker_group == 1 ? 3 : 6))
            .m.push_back({(uint8_t)x, (uint8_t)y, 0});
        break;
    case kToolBrick: {
        if (!first) break;
        push_undo();
        const int i = brick_event(true);
        lvl::Pickup p{};
        p.x = (uint8_t)x; p.y = (uint8_t)y; p.team = 0; p.type = lvl::kPickup; p.subtype = 8; p.hp = 0xFF;
        p.e0 = 2; p.e1 = 1;
        d.m.events[i].picks.push_back(p);
        break;
    }
    case kToolBuilding: {
        if (!first) break;
        push_undo();
        const int i = loose_event(true);
        d.m.events[i].ents.push_back(
            ent(x, y, g_ui.building_team, kBuildings[g_ui.building].type, 1));
        break;
    }
    case kToolErase:
        if (first && !remove_at(x, y)) say(false, "Nothing to remove there.");
        break;
    default:
        break;
    }
}

void canvas(SDL_Renderer* ren) {
    Doc& d = g_doc;
    compose_cells(ren);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(200.0f, avail.x), std::max(200.0f, avail.y - 4));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(20, 20, 24, 255));
    const float cw = kMetaW * g_ui.zoom, ch = kMetaH * g_ui.zoom;
    const ImVec2 o(p0.x + g_ui.pan.x, p0.y + g_ui.pan.y);
    // Zoom about the pointer; pan with the right or middle button.
    if (hovered && io.MouseWheel != 0.0f) {
        const float old = g_ui.zoom;
        g_ui.zoom = std::clamp(g_ui.zoom * (io.MouseWheel > 0 ? 1.25f : 0.8f), 0.125f, 4.0f);
        const ImVec2 mp(io.MousePos.x - o.x, io.MousePos.y - o.y);
        g_ui.pan.x -= mp.x * (g_ui.zoom / old - 1.0f);
        g_ui.pan.y -= mp.y * (g_ui.zoom / old - 1.0f);
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        g_ui.pan.x += io.MouseDelta.x;
        g_ui.pan.y += io.MouseDelta.y;
    }
    if (d.tex)
        dl->AddImage((ImTextureID)(intptr_t)d.tex, o, ImVec2(o.x + d.m.w * cw, o.y + d.m.h * ch));
    if (g_ui.show_areas) {
        const std::vector<uint8_t> C = lvl::area_fill(d.m.A, d.m.w, d.m.h);
        for (int y = 0; y < d.m.h; ++y)
            for (int x = 0; x < d.m.w; ++x) {
                const uint32_t c = C[at(x, y)] * 2654435761u;
                dl->AddRectFilled(ImVec2(o.x + x * cw, o.y + y * ch), ImVec2(o.x + (x + 1) * cw, o.y + (y + 1) * ch),
                                  IM_COL32((c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255, 70));
            }
    }
    if (g_ui.grid && cw >= 8) {
        for (int x = 0; x <= d.m.w; ++x)
            dl->AddLine(ImVec2(o.x + x * cw, o.y), ImVec2(o.x + x * cw, o.y + d.m.h * ch), IM_COL32(0, 0, 0, 60));
        for (int y = 0; y <= d.m.h; ++y)
            dl->AddLine(ImVec2(o.x, o.y + y * ch), ImVec2(o.x + d.m.w * cw, o.y + y * ch), IM_COL32(0, 0, 0, 60));
    }
    dl->AddRect(o, ImVec2(o.x + d.m.w * cw, o.y + d.m.h * ch), IM_COL32(255, 255, 255, 120));
    if (g_ui.show_objects) draw_objects(dl, o, cw, ch);

    // The cell under the pointer, and what the tool would do there.
    const int mx = (int)std::floor((io.MousePos.x - o.x) / cw), my = (int)std::floor((io.MousePos.y - o.y) / ch);
    const bool on_map = hovered && inside(mx, my);
    const bool shape = g_ui.tool == kToolLine || g_ui.tool == kToolRect || g_ui.tool == kToolEllipse ||
                       g_ui.tool == kToolBridge || g_ui.tool == kToolSelect;
    // The selection.
    if (has_selection()) {
        const ImVec2 a(o.x + g_ui.sel_x0 * cw, o.y + g_ui.sel_y0 * ch);
        const ImVec2 b(o.x + (g_ui.sel_x1 + 1) * cw, o.y + (g_ui.sel_y1 + 1) * ch);
        dl->AddRectFilled(a, b, IM_COL32(120, 180, 255, 40));
        dl->AddRect(a, b, IM_COL32(120, 180, 255, 255), 0, 0, 2.0f);
    }
    // A paste follows the pointer; a click places it, Esc or a right click
    // drops it.
    if (g_ui.pasting) {
        if (on_map) {
            const int px = mx, py = my;
            for (int cy = 0; cy < g_clip.h; ++cy)
                for (int cx = 0; cx < g_clip.w; ++cx) {
                    const uint8_t a = g_clip.A[(size_t)cy * g_clip.w + cx];
                    const int mat = g_clip.T[(size_t)cy * g_clip.w + cx] ? kMatTrees
                                  : a == lvl::kStone ? kMatStone : a == lvl::kWater ? kMatWater
                                  : a == lvl::kMountain ? kMatMountain : kMatGrass;
                    const ImVec2 q(o.x + (px + cx) * cw, o.y + (py + cy) * ch);
                    dl->AddRectFilled(q, ImVec2(q.x + cw, q.y + ch), (kMatColours[mat] & 0x00FFFFFFu) | 0x90000000u);
                }
            dl->AddRect(ImVec2(o.x + px * cw, o.y + py * ch),
                        ImVec2(o.x + (px + g_clip.w) * cw, o.y + (py + g_clip.h) * ch), IM_COL32(255, 255, 255, 220),
                        0, 0, 2.0f);
            ImGui::SetTooltip("Click to paste here (Esc cancels)");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                paste_at(px, py);
                g_ui.pasting = false;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) g_ui.pasting = false;
        dl->PopClipRect();
        return;
    }
    if (on_map) {
        if (g_ui.tool == kToolStart) {
            draw_ghost_start(dl, o, cw, ch, mx, my, g_ui.team, true);
        } else if (g_ui.tool == kToolMine) {
            dl->AddRectFilled(ImVec2(o.x + mx * cw, o.y + my * ch), ImVec2(o.x + (mx + 2) * cw, o.y + (my + 2) * ch),
                              IM_COL32(255, 215, 0, 90));
        } else if (g_ui.tool == kToolBuilding) {
            int w, h;
            footprint(kBuildings[g_ui.building].type, &w, &h);
            dl->AddRectFilled(ImVec2(o.x + mx * cw, o.y + my * ch), ImVec2(o.x + (mx + w) * cw, o.y + (my + h) * ch),
                              (kTeamColours[g_ui.building_team] & 0x00FFFFFFu) | 0x60000000u);
        } else if (!shape || !g_ui.dragging) {
            std::vector<std::pair<int, int>> cells;
            if (g_ui.tool == kToolBrush) brush_cells(mx, my, g_ui.brush, g_ui.round_brush, &cells);
            else cells.push_back({mx, my});
            for (const auto& c : cells)
                dl->AddRect(ImVec2(o.x + c.first * cw, o.y + c.second * ch),
                            ImVec2(o.x + (c.first + 1) * cw, o.y + (c.second + 1) * ch), IM_COL32(255, 255, 255, 140));
        }
        ImGui::SetTooltip("%d, %d", mx, my);
    }
    // Clicks and drags.
    if (on_map && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (shape) {
            g_ui.dragging = true;
            g_ui.drag_x = mx;
            g_ui.drag_y = my;
        } else {
            g_ui.painting = true;
            apply_click(mx, my, true);
        }
    }
    if (g_ui.painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && on_map && g_ui.tool == kToolBrush)
        apply_click(mx, my, false);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_ui.painting = false;
    if (g_ui.dragging) {
        const int ex = std::clamp(mx, 0, d.m.w - 1), ey = std::clamp(my, 0, d.m.h - 1);
        if (g_ui.tool == kToolSelect) {
            const int ax = std::min(g_ui.drag_x, ex), bx = std::max(g_ui.drag_x, ex);
            const int ay = std::min(g_ui.drag_y, ey), by = std::max(g_ui.drag_y, ey);
            g_ui.sel_x0 = ax; g_ui.sel_y0 = ay; g_ui.sel_x1 = bx; g_ui.sel_y1 = by;
            ImGui::SetTooltip("%d x %d", bx - ax + 1, by - ay + 1);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_ui.dragging = false;
        } else if (g_ui.tool == kToolBridge) {
            const bool horiz = std::abs(ex - g_ui.drag_x) >= std::abs(ey - g_ui.drag_y);
            const int L = g_ui.bridge_len;
            std::string why;
            const bool ok = bridge_ok(g_ui.drag_x, g_ui.drag_y, horiz, L, &why);
            const ImVec2 a(o.x + g_ui.drag_x * cw, o.y + g_ui.drag_y * ch);
            const ImVec2 b = horiz ? ImVec2(a.x + L * cw, a.y + 2 * ch) : ImVec2(a.x + 2 * cw, a.y + L * ch);
            dl->AddRectFilled(a, b, ok ? IM_COL32(80, 255, 80, 90) : IM_COL32(255, 80, 80, 90));
            ImGui::SetTooltip(ok ? "Release to place a %s bridge site" : "%s (release to carve it anyway)",
                              ok ? (horiz ? "horizontal" : "vertical") : why.c_str());
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                push_undo();
                if (!ok) carve_bridge(g_ui.drag_x, g_ui.drag_y, horiz, L);
                marker_group(horiz ? 7 : 8).m.push_back({(uint8_t)g_ui.drag_x, (uint8_t)g_ui.drag_y, 0});
                g_ui.dragging = false;
            }
        } else {
            for (const auto& c : shape_cells(g_ui.tool, g_ui.drag_x, g_ui.drag_y, ex, ey))
                dl->AddRectFilled(ImVec2(o.x + c.first * cw, o.y + c.second * ch),
                                  ImVec2(o.x + (c.first + 1) * cw, o.y + (c.second + 1) * ch),
                                  (kMatColours[g_ui.material] & 0x00FFFFFFu) | 0x90000000u);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                push_undo();
                paint_cells(shape_cells(g_ui.tool, g_ui.drag_x, g_ui.drag_y, ex, ey), g_ui.material);
                g_ui.dragging = false;
            }
        }
    }
    dl->PopClipRect();
}

// ── Panels ───────────────────────────────────────────────────────────────────
void tool_button(int t) {
    const bool on = g_ui.tool == t;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.86f, 1.0f));
    if (ImGui::Button(kToolNames[t], ImVec2(-FLT_MIN, 0))) g_ui.tool = t;
    if (on) ImGui::PopStyleColor();
}

void panel_file() {
    if (!ImGui::CollapsingHeader("Level", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::InputText("Name", g_doc.name, sizeof g_doc.name);
    ImGui::InputText("Author", g_doc.author, sizeof g_doc.author);
    if (ImGui::Button("Save to my maps")) save_folder();
    ImGui::SetItemTooltip("Writes the level to mods/maps/Level Editor/<name>. Load new maps (below) to\n"
                          "play it in Free Play and multiplayer; lobbies send it to players who lack it.");
    ImGui::SameLine();
    if (ImGui::Button("Export .lbz")) {
        std::snprintf(g_ui.path, sizeof g_ui.path, "%s.lbz", safe_name(g_doc.name).c_str());
        if (lb_pick_level_file(g_ui.path, sizeof g_ui.path, 1)) {
            const std::vector<uint8_t> z = build_lbz();
            say(!write_file(fs::u8path(g_ui.path), z), "Exported %s.", g_ui.path);
        }
    }
    ImGui::SetItemTooltip("The map tool's single-file package, for sharing or the community map sites.");
    if (ImGui::Button("Load new maps now")) {
        if (g_doc.dirty) save_folder();
        lb_mods_request_rebuild();
    }
    ImGui::SetItemTooltip("Returns the game to the title screen with your saved levels in the map lists.\n"
                          "A match in progress ends.");
}

void panel_share() {
    if (!ImGui::CollapsingHeader("Share online")) return;
    ImGui::TextWrapped("Upload this level to the community library. The moderator reviews each upload before "
                       "it appears for everyone under Maps > Community levels.");
    ImGui::BeginDisabled(g_net.busy.load());
    if (ImGui::Button("Upload level")) start_upload();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Needs a level name (and optionally an author) of letters, digits, spaces or _-'.!?,\n"
                          "no problems under Checks, and two or more start bases. Uploads go to the\n"
                          "master server set under Network.");
}

void panel_open() {
    if (!ImGui::CollapsingHeader("New / open")) return;
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Width", &g_ui.new_w, 8, 16);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Height", &g_ui.new_h, 8, 16);
    g_ui.new_w = std::clamp(g_ui.new_w, kMinSize, kMaxSize);
    g_ui.new_h = std::clamp(g_ui.new_h, kMinSize, kMaxSize);
    const char* wn[3] = {kWorlds[0].name, kWorlds[1].name, kWorlds[2].name};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("World##new", &g_ui.new_world, wn, 3);
    if (ImGui::Button("New level")) new_map(g_ui.new_w, g_ui.new_h, g_ui.new_world);
    ImGui::SetItemTooltip("Stock Free Play maps are 64x64 or 96x96.");
    ImGui::Separator();
    // The stock maps, Free Play first with their names.
    static std::vector<std::string> labels;
    if (labels.empty()) {
        for (const StockMap& s : kStock) {
            std::string l = s.stem;
            if (std::strncmp(s.stem, "mp", 2) == 0) {
                char tok[32], name[64];
                int pl = 0;
                const int k = std::atoi(s.stem + 2) - 1;
                if (lb_maps_entry(k, tok, sizeof tok, name, sizeof name, &pl) && name[0])
                    l = std::string("Free Play: ") + name;
            } else {
                const char* who = !std::strncmp(s.stem, "ck", 2) ? "King" : !std::strncmp(s.stem, "cw", 2) ? "Wizard"
                                : !std::strncmp(s.stem, "pi", 2) ? "Pirate" : !std::strncmp(s.stem, "pp", 2) ? "Imperial"
                                : !std::strncmp(s.stem, "mh", 2) ? "Space Police" : !std::strncmp(s.stem, "ma", 2) ? "Alien"
                                : "Test";
                l = std::string("Story, ") + who + " " + std::string(s.stem + 2);
            }
            labels.push_back(l + "  (" + s.stem + ")");
        }
    }
    const int nstock = (int)(sizeof kStock / sizeof kStock[0]);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##stock", labels[(size_t)std::clamp(g_ui.stock_pick, 0, nstock - 1)].c_str())) {
        for (int k = 0; k < nstock; ++k)
            if (ImGui::Selectable(labels[(size_t)k].c_str(), k == g_ui.stock_pick)) g_ui.stock_pick = k;
        ImGui::EndCombo();
    }
    if (ImGui::Button("Open game map")) load_stock(g_ui.stock_pick);
    ImGui::SetItemTooltip("Any Free Play or story map from your cartridge, to study or remake. Story missions'\n"
                          "scripted events stay with the cartridge: a saved level plays as a Free Play map.");
    ImGui::SameLine();
    if (ImGui::Button("Open file...")) {
        g_ui.path[0] = 0;
        if (lb_pick_level_file(g_ui.path, sizeof g_ui.path, 0)) load_file(g_ui.path);
    }
    ImGui::SetItemTooltip("An .lbz package or a .map (with its folder's detail tiles).");
}

void panel_world() {
    if (!ImGui::CollapsingHeader("World and size")) return;
    const char* wn[3] = {kWorlds[0].name, kWorlds[1].name, kWorlds[2].name};
    int w = g_doc.world;
    if (ImGui::Combo("World", &w, wn, 3) && w != g_doc.world) {
        push_undo();
        g_doc.world = w;
        g_doc.m.tileset = kWorlds[w].tileset;
        g_doc.detail.clear();
        retile(0, 0, g_doc.m.w - 1, g_doc.m.h - 1);
        stamp_mines();
        canvas_reset();
    }
    ImGui::SetItemTooltip("King (castle), Pirate (tropical) or Mars. The terrain keeps its shape; its tiles,\n"
                          "colours and the minimap palette follow the world. A remade map's own decorative\n"
                          "tiles are dropped.");
    ImGui::Text("%d x %d cells", g_doc.m.w, g_doc.m.h);
    if (ImGui::Button("Tidy terrain")) {
        push_undo();
        retile(0, 0, g_doc.m.w - 1, g_doc.m.h - 1);
        stamp_mines();
    }
    ImGui::SetItemTooltip("Re-picks every cell's tile from its neighbours (also replaces a remade map's\n"
                          "own decorative tiles with the world's).");
}

void panel_terrain() {
    if (!ImGui::CollapsingHeader("Terrain tools", ImGuiTreeNodeFlags_DefaultOpen)) return;
    for (int t : {kToolBrush, kToolLine, kToolRect, kToolEllipse, kToolFill, kToolPick}) tool_button(t);
    ImGui::Separator();
    for (int k = 0; k < kMatCount; ++k) {
        ImGui::PushID(k);
        ImGui::ColorButton("##m", ImGui::ColorConvertU32ToFloat4(kMatColours[k]), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(18, 18));
        ImGui::SameLine();
        if (ImGui::RadioButton(kMatNames[k], g_ui.material == k)) g_ui.material = k;
        ImGui::PopID();
    }
    ImGui::Separator();
    tool_button(kToolSelect);
    ImGui::BeginDisabled(!has_selection());
    if (ImGui::Button("Copy")) copy_selection();
    ImGui::SameLine();
    if (ImGui::Button("Cut")) cut_selection();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) delete_selection();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_clip.w <= 0);
    if (ImGui::Button("Paste")) g_ui.pasting = true;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Ctrl+C / X / V, Delete, Ctrl+A; Ctrl+Z undo,\nCtrl+Y or Ctrl+Shift+Z redo, Ctrl+S save.");
    ImGui::Separator();
    ImGui::SliderInt("Size", &g_ui.brush, 1, 12);
    ImGui::Checkbox("Round brush", &g_ui.round_brush);
    ImGui::Checkbox("Filled shapes", &g_ui.filled);
    ImGui::TextDisabled("Right-drag or middle-drag pans, the wheel zooms.");
}

void panel_objects() {
    if (!ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::TreeNodeEx("Start bases", ImGuiTreeNodeFlags_DefaultOpen)) {
        tool_button(kToolStart);
        ImGui::Combo("Player", &g_ui.team, kTeamNames, 4);
        ImGui::TextDisabled("%d of 4 placed. Each start is a base, barracks, farm,\nhero and two builders; "
                            "the base's corner is the cell you click.", start_count());
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Resources", ImGuiTreeNodeFlags_DefaultOpen)) {
        tool_button(kToolMine);
        ImGui::SetItemTooltip("A 2x2 %s spot (a mine on King maps, a quarry on Pirate, a well cap on Mars).",
                              kWorlds[g_doc.world].mine);
        tool_button(kToolBrick);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Bridge sites")) {
        tool_button(kToolBridge);
        ImGui::RadioButton("3", &g_ui.bridge_len, 3);
        ImGui::SameLine();
        ImGui::RadioButton("6", &g_ui.bridge_len, 6);
        ImGui::SameLine();
        ImGui::RadioButton("9", &g_ui.bridge_len, 9);
        ImGui::TextDisabled("Drag from the first water cell across the\nwater. A crossing that is not exactly that\n"
                            "many water cells is carved to fit.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("AI markers")) {
        tool_button(kToolMarker);
        const char* g[3] = {"Trees to harvest", "Places to build", "Shipyard places"};
        ImGui::Combo("Kind", &g_ui.marker_group, g, 3);
        if (ImGui::Button("Place automatically")) auto_markers();
        ImGui::SetItemTooltip("Replaces the three kinds with markers on tree clusters, open ground and shores,\n"
                              "so computer players can find resources and room to build.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Buildings and units")) {
        tool_button(kToolBuilding);
        std::vector<const char*> names;
        for (const EntityKind& k : kBuildings) names.push_back(k.name);
        ImGui::Combo("Type", &g_ui.building, names.data(), (int)names.size());
        ImGui::Combo("Owner", &g_ui.building_team, kTeamNames, 4);
        ImGui::TextDisabled("Extra buildings and units present at the start.\nFor a story map's NPC buildings, "
                            "give them a\nplayer that is not in the match.");
        ImGui::TreePop();
    }
    tool_button(kToolErase);
}

void panel_view() {
    if (!ImGui::CollapsingHeader("View")) return;
    ImGui::Checkbox("Grid", &g_ui.grid);
    ImGui::Checkbox("Objects", &g_ui.show_objects);
    ImGui::Checkbox("AI markers", &g_ui.show_markers);
    ImGui::Checkbox("Connected areas", &g_ui.show_areas);
    ImGui::SetItemTooltip("Tints each region units can walk within; starts in different regions\n"
                          "need a bridge site between them.");
    ImGui::SliderFloat("Zoom", &g_ui.zoom, 0.125f, 4.0f, "%.2f");
}

void panel_checks() {
    if (!ImGui::CollapsingHeader("Checks", ImGuiTreeNodeFlags_DefaultOpen)) return;
    for (const Check& c : run_checks())
        ImGui::TextColored(c.bad ? ImVec4(1, 0.45f, 0.35f, 1) : ImVec4(0.75f, 0.85f, 0.75f, 1), "%s %s",
                           c.bad ? "!" : "-", c.text.c_str());
}

}  // namespace

// Maps > Community levels (lb_overlay.cpp's page).
void lb_community_draw(SDL_Renderer* ren) {
    net_poll(ren);
    Community& c = g_com;
    if (!c.listed && !g_net.busy.load()) {
        c.listed = true;
        net_start(2, "GET", "/levels/list?limit=100", "");
    }
    ImGui::BeginDisabled(g_net.busy.load());
    if (ImGui::Button("Refresh")) net_start(2, "GET", "/levels/list?limit=100", "");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(g_net.busy.load() ? "Asking the server..." : "%d level(s)", (int)c.rows.size());
    if (!c.message.empty())
        ImGui::TextColored(c.bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", c.message.c_str());
    ImGui::BeginChild("##com_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##com", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("By");
        ImGui::TableSetupColumn("Players");
        ImGui::TableSetupColumn("World");
        ImGui::TableSetupColumn("Downloads");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)c.rows.size(); ++i) {
            const Row& r = c.rows[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(r.id);
            if (ImGui::Selectable(lb_swear_shown(r.name.c_str()), c.pick == i, ImGuiSelectableFlags_SpanAllColumns)) {
                c.pick = i;
                if (r.id != c.shown_id) net_start(3, "GET", "/levels/get?id=" + std::to_string(r.id), "");
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lb_swear_shown(r.author.c_str()));
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.players);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kWorlds[r.world].name);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.downloads);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##com_pick", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (c.pick >= 0 && c.pick < (int)c.rows.size()) {
        const Row& r = c.rows[(size_t)c.pick];
        ImGui::TextUnformatted(lb_swear_shown(r.name.c_str()));
        ImGui::TextDisabled("by %s - %d players - %s - %d x %d", r.author.empty() ? "unknown" : lb_swear_shown(r.author.c_str()),
                            r.players, kWorlds[r.world].name, r.w, r.h);
        if (c.shown_id == r.id && c.tex) {
            const float s = std::min(ImGui::GetContentRegionAvail().x, 384.0f) / (float)std::max(c.map.w, c.map.h);
            ImGui::Image((ImTextureID)(intptr_t)c.tex, ImVec2(c.map.w * s, c.map.h * s));
            ImGui::BeginDisabled(g_net.busy.load());
            if (ImGui::Button("Download"))
                net_start(4, "GET", "/levels/get?count=1&id=" + std::to_string(r.id), "");
            ImGui::SetItemTooltip("Into mods/maps/Community, checked first; Load new maps now (Level Editor\n"
                                  "page) to play it in Free Play or online.");
            ImGui::SameLine();
            if (ImGui::Button("Open in editor")) {
                lvl::Map m = c.map;
                std::vector<Meta> d = c.detail;
                adopt(std::move(m), std::move(d), r.name.c_str());
                std::snprintf(g_doc.author, sizeof g_doc.author, "%s", r.author.c_str());
                say(false, "Opened community level #%d. Saving makes your own copy.", r.id);
            }
            ImGui::EndDisabled();
        } else if (g_net.busy.load()) {
            ImGui::TextDisabled("Fetching the level...");
        }
    } else {
        ImGui::TextDisabled("Pick a level to see it.");
    }
    ImGui::EndChild();
}

// Maps > Level Editor (lb_overlay.cpp's page).
void lb_leveledit_draw(SDL_Renderer* ren) {
    net_poll(ren);
    if (!g_doc.open) {
        new_map(64, 64, 0);
        // Test aid: LB_EDITOR_OPEN=<stem> opens that cartridge map first.
        if (const char* want = std::getenv("LB_EDITOR_OPEN"))
            for (int k = 0; k < (int)(sizeof kStock / sizeof kStock[0]); ++k)
                if (std::strcmp(kStock[k].stem, want) == 0) load_stock(k);
    }
    WorldAssets& wa = world_assets(g_doc.world);
    if (!wa.ok) {
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "The editor needs the cartridge: %s.", wa.why.c_str());
        return;
    }
    // Shortcuts, while the page has the keyboard.
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) do_undo();
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z))))
            do_redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) save_folder();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) copy_selection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) cut_selection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && g_clip.w > 0) g_ui.pasting = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            g_ui.tool = kToolSelect;
            g_ui.sel_x0 = 0; g_ui.sel_y0 = 0; g_ui.sel_x1 = g_doc.m.w - 1; g_ui.sel_y1 = g_doc.m.h - 1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_selection();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            g_ui.pasting = false;
            g_ui.sel_x1 = g_ui.sel_x0 - 1;
        }
    }
    ImGui::BeginChild("##leveledit_side", ImVec2(300, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(g_doc.undo.empty());
    if (ImGui::Button("Undo")) do_undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_doc.redo.empty());
    if (ImGui::Button("Redo")) do_redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", lb_swear_shown(g_doc.name), g_doc.dirty ? " *" : "");
    if (g_ui.status[0])
        ImGui::TextColored(g_ui.status_bad ? ImVec4(1, 0.5f, 0.4f, 1) : ImVec4(0.5f, 1, 0.5f, 1), "%s", g_ui.status);
    panel_file();
    panel_share();
    panel_open();
    panel_world();
    panel_terrain();
    panel_objects();
    panel_view();
    panel_checks();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##leveledit_canvas", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    canvas(ren);
    ImGui::EndChild();
}
