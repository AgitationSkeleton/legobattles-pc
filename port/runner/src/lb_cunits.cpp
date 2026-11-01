// Custom units and buildings: APPENDED blueprint records (docs/448).
//
// A custom entity is a new blueprint at index 551 and up -- after the 182
// units, 27 projectiles and 342 collectables of BP/Entities.ebp -- so no stock
// index moves and nothing stock is replaced.
//
// WHERE ONE COMES FROM. A folder in mods/units/, the shape Balance > Custom
// units exports (lb_unitlab.cpp), named in mods/units/enabled.txt:
//
//   unit.json                   "index" (the template it was exported from),
//                               "name" (what the game calls it), "fields"
//   sheets/<sheet>.png|.NCBR    a whole sheet, by the template's sheet name
//   frames/<action>/d<d>_<n>.png  one frame (drawn over the sheet)
//   portraits/card.png, card_top_screen.png   the army-screen / battle cards
//   model/texture.png           a transport's or siege unit's re-skin
//
// Anything missing is the template's. PNGs are read as the export writes them
// (indexed on the faction palette, used as they are) or as truecolour (each
// pixel to the nearest palette entry; alpha below half is clear). An .NCBR is
// taken raw or in the game's COMP wrapper, with or without its CPOS block (the
// community's sheets, e.g. Ryncat's Power Miners, have none).
//
// HOW IT GETS INTO THE GAME (docs/448):
//
//  * At cartridge build (lb_mods_build), BP/Entities.ebp is replaced by a copy
//    with a block appended AFTER the string pool, where the game's own walk
//    (func_02001F3C: 0x227 records, then the pool) never looks:
//
//        "CUN1" u32 count, then per entity: record (0x7C) | wrapper (0xC)
//        then the entities' strings
//
//    The records live in the file loader's own buffer, which the game keeps
//    (func_02001EE0), so they need no allocation of ours. The unit's sheets,
//    cards and model go into NEW "cu" directories (Sprites/cu, Models/cu,
//    UI/FEPlayerCards/cu, UI/GamePlayerCards/cu), so no stock file id moves:
//    its internal name is "cu/<Folder>" (the cards are found by it) and its
//    asset path "Sprites/cu/<Folder>" or "Models/cu/<Folder>". Its name is a
//    new string in every language file, and +0x06 points at it.
//
//  * Once the blueprints have settled (lb_patches_blueprint_settled), each
//    record's name (+0x00) and asset path (+0x24) are turned into pointers, as
//    func_02001684 does for the stock records, its wrapper is filled in as
//    func_02002B68 builds a unit's ({0x0213AC40, rec, rec}), and the wrapper is
//    pushed onto the blueprint PtrArray at 0x02150944. func_02001E48 bounds an
//    index by that array's live count, so index 551 + k is then a blueprint.
//
// TEST AID: LB_TEST_CUNIT=<record> appends a copy of stock record <record>
// as the first custom unit and fields it in its own role for player 1 (LB_TEST_CUNIT_PLAYER=n
// for another).

#include "lb_cunits.h"
#include "lb_levelmap.h"   // lvl::comp_wrap

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <zlib.h>

#include "runtime_arm.h"   // bus_read_* / bus_write_*

extern "C" int lb_patches_blueprint_settled(void);   // lb_patches.cpp
extern "C" const char* lb_mods_dir(void);              // lb_mods.cpp

namespace {

namespace fs = std::filesystem;

constexpr int kStock = 551;
constexpr int kMaxUnits = 9;   // the blueprint array holds 560 (docs/448, run cu448a)
constexpr size_t kPool = 4 + 182 * 0x7C + 27 * 0x74 + 342 * 0x70;   // docs/11
constexpr uint32_t kPtrArray = 0x02150944u;   // data +4, count +8, capacity (bytes) +0xC
constexpr uint32_t kUnitVtable = 0x0213AC40u; // a unit blueprint's wrapper (func_02002B68)
constexpr uint32_t kMagic = 0x314E5543u;      // "CUN1"

struct Entity {
    std::vector<uint8_t> rec;   // 0x7C bytes
    std::string name, asset;    // internal name, asset path
    std::string folder, display;
    int tmpl = -1;
    int card_src = -1;           // the template's minifig card (a collectable), or -1
    size_t rec_off = 0, wrap_off = 0, name_off = 0, asset_off = 0;   // in the file
    size_t id_off = 0, id_wrap_off = 0, card_off = 0, card_wrap_off = 0;
    std::array<std::shared_ptr<const std::vector<int16_t>>, 5> snd;   // its own sounds, by index
    std::vector<uint32_t> head;  // portraits/head.png, 24x24 RGBA (0xAABBGGRR), or empty
};
size_t g_array_off = 0;    // the larger blueprint array, in the file
constexpr size_t kColBase = 4 + 182 * 0x7C + 27 * 0x74;   // collectable 209 (docs/11)
uint32_t get32(const std::vector<uint8_t>& b, size_t at);
// The minifig card whose unit (+0x60) is `tmpl`: category 0x0B, kind 2.
int find_card(const std::vector<uint8_t>& stock, int tmpl) {
    for (int i = 209; i < kStock; ++i) {
        const size_t o = kColBase + (size_t)(i - 209) * 0x70;
        if (o + 0x70 > stock.size()) break;
        if (stock[o + 8] == 2 && stock[o + 0x5C] == 0x0B && get32(stock, o + 0x60) == (uint32_t)tmpl) return i;
    }
    return -1;
}
std::vector<Entity> g_ents;
std::vector<uint8_t> g_stock;   // the stock file, for lb_cunits_finish
size_t g_block_off = 0;    // where "CUN1" sits in the file
bool g_built = false;      // the cartridge carries our block
uint32_t g_pushed_base = 0;   // the file buffer we last registered into
std::string g_report;      // what the last build did, for the menu
std::vector<uint64_t> g_hashes;               // the built set's identities, in order (docs/448)
std::vector<std::vector<uint8_t>> g_bundles;  // and its bundles (a host serves these)
bool make_bundle(const fs::path& dir, std::vector<uint8_t>* out);
uint64_t fnv64(const std::vector<uint8_t>& b);
std::string hex16(uint64_t h);

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + (size_t)i] = (uint8_t)(v >> (8 * i));
}
uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) |
           ((uint32_t)b[at + 3] << 24);
}
uint32_t get16(const std::vector<uint8_t>& b, size_t at) { return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8); }
std::string pool_str(const std::vector<uint8_t>& e, uint32_t off) {
    std::string s;
    for (size_t p = kPool + off; p < e.size() && e[p] && s.size() < 96; ++p) s += (char)e[p];
    return s;
}
std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
bool slurp(const fs::path& p, std::vector<uint8_t>* out, size_t cap = 4u << 20) {
    std::FILE* f = std::fopen(utf8(p).c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || (size_t)n > cap) { std::fclose(f); return false; }
    out->resize((size_t)n);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}
// The game's COMP wrapper, stored form only (what lvl::comp_wrap writes) and
// LZ11 (the cartridge's own), for an .NCBR a player supplies.
bool unwrap(std::vector<uint8_t>* b) {
    if (b->size() < 20 || std::memcmp(b->data(), "PMOC", 4) != 0) return true;
    const int32_t n = (int32_t)get32(*b, 4), mode = (int32_t)get32(*b, 8);
    if (mode == 1 && n > 0 && (size_t)n + 20 <= b->size()) {
        b->erase(b->begin(), b->begin() + 20);
        b->resize((size_t)n);
        return true;
    }
    // LZ11 body after the 20-byte header (the cartridge's own files).
    const std::vector<uint8_t> in(b->begin() + 20, b->end());
    if (in.size() < 4 || in[0] != 0x11) return false;
    size_t want = in[1] | (in[2] << 8) | (in[3] << 16), p = 4;
    std::vector<uint8_t> out;
    out.reserve(want);
    while (out.size() < want && p < in.size()) {
        const uint8_t flags = in[p++];
        for (int bit = 7; bit >= 0 && out.size() < want; --bit) {
            if (!(flags & (1 << bit))) {
                if (p >= in.size()) return false;
                out.push_back(in[p++]);
                continue;
            }
            if (p + 1 >= in.size()) return false;
            const int ind = in[p] >> 4;
            size_t len, disp;
            if (ind == 0) {
                if (p + 2 >= in.size()) return false;
                len = (size_t)(((in[p] & 0xF) << 4) | (in[p + 1] >> 4)) + 0x11;
                disp = (size_t)(((in[p + 1] & 0xF) << 8) | in[p + 2]) + 1;
                p += 3;
            } else if (ind == 1) {
                if (p + 3 >= in.size()) return false;
                len = (size_t)(((in[p] & 0xF) << 12) | (in[p + 1] << 4) | (in[p + 2] >> 4)) + 0x111;
                disp = (size_t)(((in[p + 2] & 0xF) << 8) | in[p + 3]) + 1;
                p += 4;
            } else {
                len = (size_t)ind + 1;
                disp = (size_t)(((in[p] & 0xF) << 8) | in[p + 1]) + 1;
                p += 2;
            }
            if (disp > out.size()) return false;
            for (size_t i = 0; i < len && out.size() < want; ++i) out.push_back(out[out.size() - disp]);
        }
    }
    if (out.size() != want) return false;
    *b = std::move(out);
    return true;
}

// ---- PNG reading -----------------------------------------------------------
struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;   // 0xAABBGGRR
    std::vector<int> idx;         // palette index per pixel when indexed, else empty
};
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
bool read_png(const std::vector<uint8_t>& b, Img* out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 33 || std::memcmp(b.data(), sig, 8) != 0) return false;
    int w = 0, h = 0, depth = 0, ctype = 0;
    std::vector<uint32_t> pal;
    std::vector<uint8_t> z;
    for (size_t p = 8; p + 12 <= b.size();) {
        const uint32_t n = be32(&b[p]);
        if (n > b.size() - p - 12) return false;
        const char* t = (const char*)&b[p + 4];
        const uint8_t* d = &b[p + 8];
        if (!std::memcmp(t, "IHDR", 4) && n >= 13) {
            w = (int)be32(d); h = (int)be32(d + 4); depth = d[8]; ctype = d[9];
            if (d[12] != 0) return false;   // interlaced: not supported
        } else if (!std::memcmp(t, "PLTE", 4)) {
            pal.clear();
            for (uint32_t i = 0; i + 2 < n; i += 3) pal.push_back(0xFF000000u | d[i] | (d[i + 1] << 8) | (d[i + 2] << 16));
        } else if (!std::memcmp(t, "tRNS", 4) && ctype == 3) {
            for (uint32_t i = 0; i < n && i < pal.size(); ++i) pal[i] = (pal[i] & 0x00FFFFFFu) | ((uint32_t)d[i] << 24);
        } else if (!std::memcmp(t, "IDAT", 4)) {
            z.insert(z.end(), d, d + n);
        } else if (!std::memcmp(t, "IEND", 4)) {
            break;
        }
        p += 12 + n;
    }
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    int ch = 0;
    switch (ctype) {
        case 0: ch = 1; break;
        case 2: ch = 3; break;
        case 3: ch = 1; break;
        case 4: ch = 2; break;
        case 6: ch = 4; break;
        default: return false;
    }
    if (ctype == 3 ? (depth != 1 && depth != 2 && depth != 4 && depth != 8) : depth != 8) return false;
    const size_t bpp_bits = (size_t)ch * depth, stride = ((size_t)w * bpp_bits + 7) / 8, bpp = std::max<size_t>(1, bpp_bits / 8);
    std::vector<uint8_t> raw((stride + 1) * (size_t)h);
    uLongf rn = (uLongf)raw.size();
    if (uncompress(raw.data(), &rn, z.data(), (uLong)z.size()) != Z_OK || rn != raw.size()) return false;
    std::vector<uint8_t> cur(stride), prev(stride, 0);
    out->w = w; out->h = h;
    out->rgba.assign((size_t)w * h, 0u);
    out->idx.clear();
    if (ctype == 3) out->idx.assign((size_t)w * h, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t f = raw[(size_t)y * (stride + 1)];
        const uint8_t* s = &raw[(size_t)y * (stride + 1) + 1];
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0, up = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int v = s[i];
            switch (f) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += up; break;
                case 3: v += (a + up) / 2; break;
                case 4: { const int pp = a + up - c, pa = std::abs(pp - a), pb = std::abs(pp - up), pc = std::abs(pp - c);
                          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); break; }
                default: return false;
            }
            cur[i] = (uint8_t)v;
        }
        for (int x = 0; x < w; ++x) {
            uint32_t c = 0;
            const size_t o = (size_t)y * w + x;
            if (ctype == 3) {
                const int per = 8 / depth, sh = 8 - depth * (1 + x % per);
                const int k = (cur[(size_t)x / per] >> sh) & ((1 << depth) - 1);
                out->idx[o] = k;
                c = (size_t)k < pal.size() ? pal[(size_t)k] : 0u;
            } else if (ctype == 0) { const uint8_t g = cur[(size_t)x]; c = 0xFF000000u | g | (g << 8) | (g << 16); }
            else if (ctype == 4) { const uint8_t g = cur[(size_t)x * 2]; c = ((uint32_t)cur[(size_t)x * 2 + 1] << 24) | g | (g << 8) | (g << 16); }
            else if (ctype == 2) { const uint8_t* q = &cur[(size_t)x * 3]; c = 0xFF000000u | q[0] | (q[1] << 8) | (q[2] << 16); }
            else { const uint8_t* q = &cur[(size_t)x * 4]; c = ((uint32_t)q[3] << 24) | q[0] | (q[1] << 8) | (q[2] << 16); }
            out->rgba[o] = c;
        }
        std::swap(cur, prev);
    }
    if (ctype == 3 && pal.size() > 16) out->idx.clear();   // not one of our 16-colour exports
    return true;
}

// ---- colours ----------------------------------------------------------------
uint32_t ds_rgba(uint16_t c) {   // 0xAABBGGRR, through the 6-bit step
    auto chn = [](int v) { const int s = v * 2; return (uint32_t)(((s << 2) | (s >> 4)) & 255); };
    return 0xFF000000u | (chn((c >> 10) & 31) << 16) | (chn((c >> 5) & 31) << 8) | chn(c & 31);
}
uint16_t to555(uint32_t c) {
    return (uint16_t)(((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) | ((((c >> 16) & 0xFF) >> 3) << 10));
}
int dist(uint32_t a, uint32_t b) {
    const int dr = (int)(a & 0xFF) - (int)(b & 0xFF), dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF),
              db = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}
// The nearest of pal[1..n-1]; 0 for a pixel under half alpha.
int nearest(uint32_t c, const std::vector<uint32_t>& pal) {
    if ((c >> 24) < 128) return 0;
    int best = 1, bd = 1 << 30;
    for (size_t i = 1; i < pal.size(); ++i) {
        const int d = dist(c, pal[i]);
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}
// A palette of at most n colours (entry 0 clear) for a picture: the n-1 most
// used 15-bit colours.
std::vector<uint32_t> make_palette(const std::vector<uint32_t>& px, size_t n) {
    std::map<uint16_t, int> count;
    for (uint32_t c : px)
        if ((c >> 24) >= 128) ++count[to555(c)];
    std::vector<std::pair<int, uint16_t>> v;
    for (const auto& kv : count) v.push_back({-kv.second, kv.first});
    std::sort(v.begin(), v.end());
    std::vector<uint32_t> pal(1, 0u);
    for (size_t i = 0; i < v.size() && pal.size() < n; ++i) pal.push_back(ds_rgba(v[i].second));
    return pal;
}

// ---- sheets ------------------------------------------------------------------
struct Sheet { std::vector<uint8_t> file; int w = 0, h = 0; size_t data = 0; std::vector<uint8_t> px; };
// An NCBR: its CHAR block's size, depth and data (4bpp, linear rows).
bool parse_sheet(std::vector<uint8_t> f, Sheet* s) {
    if (!unwrap(&f) || f.size() < 0x30 || std::memcmp(f.data() + 0x10, "RAHC", 4) != 0) return false;
    const uint32_t th = get16(f, 0x18), tw = get16(f, 0x1A), depth = get32(f, 0x1C);
    const uint32_t dsize = get32(f, 0x28), doff = get32(f, 0x2C);
    const size_t base = 0x18 + doff;
    if (depth != 3 || !tw || !th || tw == 0xFFFF || base + dsize > f.size()) return false;
    s->w = (int)tw * 8;
    s->h = (int)th * 8;
    if ((size_t)s->w * s->h / 2 > dsize) return false;
    s->data = base;
    s->px.assign((size_t)s->w * s->h, 0);
    for (size_t i = 0; i < (size_t)s->w * s->h / 2; ++i) {
        s->px[i * 2] = f[base + i] & 15;
        s->px[i * 2 + 1] = f[base + i] >> 4;
    }
    s->file = std::move(f);
    return true;
}
std::vector<uint8_t> encode_sheet(const Sheet& s) {
    std::vector<uint8_t> f = s.file;
    for (size_t i = 0; i < (size_t)s.w * s.h / 2; ++i)
        f[s.data + i] = (uint8_t)((s.px[i * 2] & 15) | (s.px[i * 2 + 1] << 4));
    return f;
}
// Draw a picture into the sheet at (x, y): indices as they are when the PNG is
// one of ours (16-colour indexed), else the nearest faction colour.
void blit(Sheet& s, const Img& im, int x0, int y0, const std::vector<uint32_t>& fpal) {
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= s.w || Y >= s.h) continue;
            const size_t o = (size_t)y * im.w + x;
            const int v = !im.idx.empty() ? (im.idx[o] & 15) : nearest(im.rgba[o], fpal);
            s.px[(size_t)Y * s.w + X] = (uint8_t)v;
        }
}

// The frames of a template's layout (lb_unitlab.cpp's, by category).
struct Frame { std::string action, sheet; int dir, n, x, y, w, h; };
std::vector<Frame> frames_of(int cat, const std::string& base, const std::vector<uint8_t>& rec) {
    std::vector<Frame> v;
    if (cat == 0)
        for (int d = 0; d < 5; ++d)
            for (int f = 0; f < 6; ++f) {
                v.push_back({"walk", base + "_w" + std::to_string(d), d, f, f * 24, 0, 24, 24});
                v.push_back({"attack", base + "_a" + std::to_string(d), d, f, f * 24, 0, 24, 24});
            }
    else if (cat >= 1 && cat <= 3)
        for (int d = 0; d < 5; ++d) {
            v.push_back({"idle", base + "_0", d, 0, d * 24, 0, 24, 24});
            for (int f = 0; f < 5; ++f) {
                v.push_back({"walk", base + "_1", d, f, f * 24, d * 24, 24, 24});
                v.push_back({"attack", base + "_2", d, f, f * 24, d * 24, 24, 24});
            }
        }
    else if (cat == 4)
        for (int d = 0; d < 5; ++d) {
            v.push_back({"idle", base, d, 0, 0, 96 + d * 32, 32, 32});
            for (int f = 0; f < 5; ++f) v.push_back({"walk", base, d, f, f * 32, 96 + d * 32, 32, 32});
            for (int f = 5; f < 8; ++f) v.push_back({"attack", base, d, f - 5, f * 32, 96 + d * 32, 32, 32});
        }
    else if (cat >= 7) {
        const int cell = (int)get16(rec, 0x20), cw = rec[0x1E], chh = rec[0x1F];
        for (int d = 0; d < 5; ++d) v.push_back({"idle", base, d, 0, (cell & 31) * 8, (cell >> 5) * 8, cw * 8, chh * 8});
    }
    return v;
}
std::vector<std::string> sheets_of(int cat, const std::string& base) {
    if (cat == 0) {
        std::vector<std::string> v;
        for (int d = 0; d < 5; ++d) { v.push_back(base + "_w" + std::to_string(d)); v.push_back(base + "_a" + std::to_string(d)); }
        return v;
    }
    if (cat >= 1 && cat <= 3) return {base + "_0", base + "_1", base + "_2"};
    return {base};
}

// ---- cards (NCER cell over NCGR, NCLR) ----------------------------------------
// The template's card re-encoded from a picture of the same cell size: a new
// palette from the picture (255 colours at 8bpp, 15 at 4bpp), its pixels into
// the same OBJ tiles. False when the picture does not fit.
bool encode_card(std::vector<uint8_t>* ncgr, std::vector<uint8_t>* nclr, const std::vector<uint8_t>& ncer,
                 const Img& im) {
    std::vector<uint8_t>& g = *ncgr;
    std::vector<uint8_t>& c = *nclr;
    if (ncer.size() < 0x30 || g.size() < 0x30 || c.size() < 0x28 + 32) return false;
    if (std::memcmp(ncer.data() + 0x10, "KBEC", 4) != 0 || std::memcmp(g.data() + 0x10, "RAHC", 4) != 0) return false;
    const uint32_t ncell = get16(ncer, 0x18), cattr = get16(ncer, 0x1A), coff = get32(ncer, 0x1C);
    if (!ncell) return false;
    const size_t cells = 0x18 + coff, esz = cattr == 1 ? 16 : 8, oams = cells + ncell * esz;
    const uint32_t noam = get16(ncer, cells), ooff = get32(ncer, cells + 4);
    const size_t chars = 0x18 + get32(g, 0x2C), csize = get32(g, 0x28);
    if ((get32(g, 0x24) & 0xFFu) == 1u || chars + csize > g.size()) return false;   // tiled only
    static const int kW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static const int kH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    struct O { int x, y, w, h, tile, pal; bool b8; };
    std::vector<O> os;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (uint32_t k = 0; k < noam && k < 128; ++k) {
        const size_t at = oams + ooff + k * 6;
        if (at + 6 > ncer.size()) break;
        const uint32_t a0 = get16(ncer, at), a1 = get16(ncer, at + 2), a2 = get16(ncer, at + 4);
        const int shape = (int)(a0 >> 14) & 3, size = (int)(a1 >> 14) & 3;
        if (shape == 3) continue;
        O o{(int)(a1 & 0x1FF), (int)(int8_t)(a0 & 0xFF), kW[shape][size], kH[shape][size], (int)(a2 & 0x3FF),
            (int)(a2 >> 12), (a0 & 0x2000u) != 0};
        if (o.x >= 256) o.x -= 512;
        os.push_back(o);
        x0 = std::min(x0, o.x); y0 = std::min(y0, o.y);
        x1 = std::max(x1, o.x + o.w); y1 = std::max(y1, o.y + o.h);
    }
    if (os.empty() || im.w != x1 - x0 || im.h != y1 - y0) return false;
    const bool b8 = os[0].b8;
    const std::vector<uint32_t> pal = make_palette(im.rgba, b8 ? 256 : 16);
    const size_t ncol = (c.size() - 0x28) / 2;
    for (size_t i = 0; i < ncol; ++i) {
        const uint16_t v = i < pal.size() ? to555(pal[i]) : 0;
        c[0x28 + i * 2] = (uint8_t)v;
        c[0x28 + i * 2 + 1] = (uint8_t)(v >> 8);
    }
    for (const O& o : os) {
        const size_t tbytes = o.b8 ? 64 : 32;
        for (int y = 0; y < o.h; ++y)
            for (int x = 0; x < o.w; ++x) {
                const size_t t = (size_t)(y / 8) * (o.w / 8) + (x / 8), in = (size_t)(y % 8) * 8 + (x % 8);
                const size_t byte = (size_t)o.tile * 32 + t * tbytes + (o.b8 ? in : in / 2);
                if (byte >= csize) continue;
                int v = nearest(im.rgba[(size_t)(o.y - y0 + y) * im.w + (o.x - x0 + x)], pal);
                if (!o.b8) v = std::min(v, 15);
                uint8_t& dst = g[chars + byte];
                if (o.b8) dst = (uint8_t)v;
                else dst = (x % 2) ? (uint8_t)((dst & 0x0F) | (v << 4)) : (uint8_t)((dst & 0xF0) | v);
            }
    }
    return true;
}

// ---- model textures ----------------------------------------------------------
// A copy of the template's .nsbmd with its one TEX0 texture (format 3, 16
// colours: every model measured, docs/51) and palette taken from a picture of
// the same size. False when it does not fit.
bool encode_model_texture(std::vector<uint8_t>* m, const Img& im) {
    std::vector<uint8_t>& d = *m;
    if (d.size() < 0x14 || std::memcmp(d.data(), "BMD0", 4) != 0) return false;
    size_t t0 = 0;
    for (uint32_t i = 0; i < get16(d, 0x0E) && i < 8; ++i) {
        const uint32_t o = get32(d, 0x10 + 4 * i);
        if (o + 4 <= d.size() && std::memcmp(d.data() + o, "TEX0", 4) == 0) t0 = o;
    }
    if (!t0) return false;
    const size_t b = t0 + 8;
    const size_t td = t0 + get16(d, b + 6), tdata = t0 + get32(d, b + 12);
    const size_t pd = t0 + get32(d, b + 0x2C), pdata = t0 + get32(d, b + 0x30);
    if (td + 8 > d.size() || pd + 8 > d.size() || !d[td + 1] || !d[pd + 1]) return false;
    const size_t te = td + get16(d, td + 6) + 4, pe = pd + get16(d, pd + 6) + 4;
    const uint32_t p = get32(d, te);
    const int fmt = (int)((p >> 26) & 7), w = 8 << ((p >> 20) & 7), h = 8 << ((p >> 23) & 7);
    const bool clear0 = ((p >> 29) & 1u) != 0;
    if (fmt != 3 || im.w != w || im.h != h) return false;
    const size_t img = tdata + ((size_t)(p & 0xFFFF) << 3), pal_at = pdata + ((size_t)get16(d, pe) << 3);
    if (img + (size_t)w * h / 2 > d.size() || pal_at + 32 > d.size()) return false;
    std::vector<uint32_t> pal = make_palette(im.rgba, 16);
    if (!clear0) pal.erase(pal.begin());   // colour 0 is a colour when not clear
    for (size_t i = 0; i < 16; ++i) {
        const uint16_t v = i < pal.size() ? to555(pal[i]) : 0;
        d[pal_at + i * 2] = (uint8_t)v;
        d[pal_at + i * 2 + 1] = (uint8_t)(v >> 8);
    }
    for (int i = 0; i < w * h; ++i) {
        int v;
        if (clear0) v = nearest(im.rgba[(size_t)i], pal);
        else {
            int best = 0, bd = 1 << 30;
            for (size_t k = 0; k < pal.size(); ++k) {
                const int dd = dist(im.rgba[(size_t)i], pal[k]);
                if (dd < bd) { bd = dd; best = (int)k; }
            }
            v = best;
        }
        uint8_t& dst = d[img + (size_t)i / 2];
        dst = (i & 1) ? (uint8_t)((dst & 0x0F) | ((v & 15) << 4)) : (uint8_t)((dst & 0xF0) | (v & 15));
    }
    return true;
}

// ---- unit.json --------------------------------------------------------------
// A small reader for the keys the export writes: top-level "index" and "name",
// and the number pairs of "fields". Bounded; anything it does not understand
// is ignored.
std::string jstr(const std::string& s, const std::string& key) {
    const size_t k = s.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t p = s.find(':', k);
    if (p == std::string::npos) return "";
    p = s.find('"', p);
    if (p == std::string::npos) return "";
    std::string out;
    for (++p; p < s.size() && s[p] != '"' && out.size() < 64; ++p) {
        if (s[p] == '\\' && p + 1 < s.size()) ++p;
        out += s[p];
    }
    return out;
}
bool jnum(const std::string& s, size_t from, size_t to, const std::string& key, long* v) {
    const size_t k = s.find("\"" + key + "\"", from);
    if (k == std::string::npos || k >= to) return false;
    const size_t p = s.find(':', k);
    if (p == std::string::npos || p >= to) return false;
    char* end = nullptr;
    const long x = std::strtol(s.c_str() + p + 1, &end, 10);
    if (end == s.c_str() + p + 1) return false;
    *v = x;
    return true;
}
struct Field { const char* key; int off, width; long max; };
const Field kFields[] = {
    {"cost", 0x5E, 2, 65535}, {"build_time", 0x60, 2, 65535}, {"health", 0x62, 2, 65535}, {"mana", 0x64, 2, 65535},
    {"speed", 0x0C, 4, 1 << 20}, {"melee_damage", 0x68, 2, 65535}, {"melee_spread", 0x6A, 2, 65535},
    {"attack_interval", 0x6D, 1, 255}, {"attack_range", 0x6F, 1, 255}, {"resource_yield", 0x6C, 1, 255},
    {"projectile", 0x66, 2, 65535}, {"spell_1", 0x72, 1, 0x22}, {"spell_2", 0x73, 1, 0x22}, {"spell_3", 0x74, 1, 0x22},
    {"spell_4", 0x75, 1, 0x22}, {"spell_5", 0x76, 1, 0x22}, {"land", 0x17, 1, 1}, {"water", 0x18, 1, 1},
    {"trees", 0x19, 1, 1}, {"building_collision", 0x1A, 1, 1}, {"land_water", 0x1B, 1, 1},
    {"hitbox_size", 0x1D, 1, 255}, {"not_water_only", 0x16, 1, 1}, {"ai_priority", 0x70, 1, 255},
    {"defogged_area", 0x71, 1, 255},
};
bool name_ok(const std::string& n) {
    if (n.empty() || n.size() > 24) return false;
    for (char ch : n)
        if (!(std::isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '\'' || ch == '.' || ch == '!')) return false;
    return true;
}
std::string safe_folder(const std::string& n) {
    std::string o;
    for (char ch : n)
        if (std::isalnum((unsigned char)ch) || ch == '_') o += ch;
    return o.substr(0, 20);
}

fs::path units_dir() { return fs::u8path(lb_mods_dir()) / "units"; }

// The folders to build: a lobby's set when one is waiting (mods/units/lobby.txt,
// written by lb_cunits_lobby_set, used once and removed), else enabled.txt.
// A lobby line may name a received unit, "_received/<16 hex digits>".
std::vector<std::string> enabled_folders() {
    std::vector<std::string> v;
    std::vector<uint8_t> b;
    std::error_code ec;
    const bool lobby = slurp(units_dir() / "lobby.txt", &b, 64u << 10);
    if (lobby) fs::remove(units_dir() / "lobby.txt", ec);
    else if (!slurp(units_dir() / "enabled.txt", &b, 64u << 10)) return v;
    std::string s(b.begin(), b.end()), line;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n' || s[i] == '\r') {
            if (lobby && line.size() == 26 && line.compare(0, 10, "_received/") == 0 &&
                line.find_first_not_of("0123456789ABCDEF", 10) == std::string::npos) {
                v.push_back(line);
                line.clear();
                continue;
            }
            const std::string f = safe_folder(line);
            if (!f.empty() && std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
            line.clear();
        } else {
            line += s[i];
        }
    }
    return v;
}

// One enabled folder into an Entity and its files. `why` says what was wrong
// when it returns false.
bool load_unit(const std::string& folder, const std::string& tag, const std::vector<uint8_t>& stock, const CuReader& read,
               const std::vector<uint32_t>& fpal, Entity* e, std::vector<CuFile>* files, std::string* why) {
    const fs::path dir = units_dir() / fs::u8path(folder);
    std::vector<uint8_t> jb;
    if (!slurp(dir / "unit.json", &jb, 1u << 20)) { *why = "no unit.json"; return false; }
    const std::string j(jb.begin(), jb.end());
    long tmpl = -1;
    if (!jnum(j, 0, j.size(), "index", &tmpl) || tmpl < 0 || tmpl >= 182) { *why = "unit.json has no template \"index\" 0..181"; return false; }
    const size_t r = 4 + (size_t)tmpl * 0x7C;
    e->rec.assign(stock.begin() + (long)r, stock.begin() + (long)r + 0x7C);
    const int cat = e->rec[0x5C];
    e->tmpl = (int)tmpl;
    e->folder = folder;
    e->display = jstr(j, "name");
    if (!name_ok(e->display)) e->display = folder;
    // The stats: every known field, clamped to its width and range.
    const size_t fb = j.find("\"fields\"");
    const size_t fe = fb == std::string::npos ? std::string::npos : j.find('}', fb);
    if (fb != std::string::npos && fe != std::string::npos)
        for (const Field& f : kFields) {
            long v;
            if (!jnum(j, fb, fe, f.key, &v)) continue;
            v = std::clamp(v, 0L, f.max);
            for (int i = 0; i < f.width; ++i) e->rec[(size_t)f.off + (size_t)i] = (uint8_t)((unsigned long)v >> (8 * i));
        }
    const std::string tasset = pool_str(stock, get32(stock, r + 0x24));
    const std::string tname = pool_str(stock, get32(stock, r + 0x00));
    e->name = "cu/" + tag;
    // Sprites: each template sheet, overlaid with the folder's sheet, then its frames.
    if (tasset.compare(0, 8, "Sprites/") == 0) {
        const std::string tbase = tasset.substr(8);
        e->asset = "Sprites/cu/" + tag;
        const std::vector<Frame> frs = frames_of(cat, tbase, e->rec);
        for (const std::string& ts : sheets_of(cat, tbase)) {
            std::vector<uint8_t> raw;
            Sheet s;
            if (!read("Sprites/" + ts + ".NCBR", &raw) || !parse_sheet(raw, &s)) { *why = "the template's sheet " + ts + " is unreadable"; return false; }
            std::vector<uint8_t> mine;
            Sheet ms;
            Img im;
            if (slurp(dir / "sheets" / (ts + ".NCBR"), &mine) && parse_sheet(mine, &ms) && ms.w == s.w && ms.h == s.h) s.px = ms.px;
            else if (slurp(dir / "sheets" / (ts + ".png"), &mine) && read_png(mine, &im) && im.w == s.w && im.h == s.h) blit(s, im, 0, 0, fpal);
            for (const Frame& fr : frs) {
                if (fr.sheet != ts) continue;
                char nm[32];
                std::snprintf(nm, sizeof nm, "d%d_%d.png", fr.dir, fr.n);
                if (slurp(dir / "frames" / fr.action / nm, &mine) && read_png(mine, &im) && im.w == fr.w && im.h == fr.h)
                    blit(s, im, fr.x, fr.y, fpal);
            }
            const std::string mine_name = tag + ts.substr(tbase.size());   // k_eng_0 -> Folder_0
            files->push_back({"Sprites", mine_name + ".NCBR", lvl::comp_wrap(encode_sheet(s))});
        }
    } else if (tasset.compare(0, 7, "Models/") == 0) {
        e->asset = "Models/cu/" + tag;
        std::vector<uint8_t> mdl, anim, mine;
        if (!read(tasset + ".nsbmd", &mdl)) { *why = "the template's model is unreadable"; return false; }
        Img im;
        if (slurp(dir / "model" / "texture.png", &mine) && read_png(mine, &im) && !encode_model_texture(&mdl, im))
            std::fprintf(stderr, "[lb_cunits] %s: model/texture.png is not the template's texture size; ignored\n", folder.c_str());
        // Raw, as the cartridge ships its models (BMD0/BCA0, no COMP wrapper).
        files->push_back({"Models", tag + ".nsbmd", mdl});
        if (read(tasset + ".nsbca", &anim)) files->push_back({"Models", tag + ".nsbca", anim});
    } else {
        *why = "the template's asset is neither a sprite nor a model";
        return false;
    }
    // Its head (the army screen's and the Team Builder's): portraits/head.png,
    // fitted to 24x24 by nearest pixel.
    {
        std::vector<uint8_t> hb;
        Img hi;
        if (slurp(dir / "portraits" / "head.png", &hb, 1u << 20) && read_png(hb, &hi) && hi.w > 0 && hi.h > 0 &&
            hi.w <= 512 && hi.h <= 512) {
            e->head.assign(576, 0u);
            for (int y = 0; y < 24; ++y)
                for (int x = 0; x < 24; ++x)
                    e->head[(size_t)(y * 24 + x)] = hi.rgba[(size_t)(y * hi.h / 24) * (size_t)hi.w + (size_t)(x * hi.w / 24)];
        }
    }
    // Cards, found by the internal name. A missing army-screen card hangs the
    // game (docs/436), so the template's is always copied.
    const struct { const char* parent; const char* pic; } kCards[2] = {
        {"UI/FEPlayerCards", "card.png"}, {"UI/GamePlayerCards", "card_top_screen.png"}};
    for (const auto& k : kCards) {
        std::vector<uint8_t> g, c, r2, a;
        const std::string tp = std::string(k.parent) + "/" + tname;
        if (!read(tp + ".NCGR", &g) || !read(tp + ".NCLR", &c) || !read(tp + ".NCER", &r2)) continue;
        read(tp + ".NANR", &a);
        std::vector<uint8_t> mine;
        Img im;
        if (slurp(dir / "portraits" / k.pic, &mine) && read_png(mine, &im) && !encode_card(&g, &c, r2, im))
            std::fprintf(stderr, "[lb_cunits] %s: portraits/%s is not the template's card size; ignored\n", folder.c_str(), k.pic);
        // As the cartridge ships a card: the NCGR COMP-wrapped, the NCLR, NCER
        // and NANR raw. A wrapped NCLR/NCER is never finished loading and the
        // army screen's top screen stays black (run army448f).
        files->push_back({k.parent, tag + ".NCGR", lvl::comp_wrap(g)});
        files->push_back({k.parent, tag + ".NCLR", c});
        files->push_back({k.parent, tag + ".NCER", r2});
        if (!a.empty()) files->push_back({k.parent, tag + ".NANR", a});
    }
    return true;
}

// The test aid's clone, or nothing.
void collect_test(const std::vector<uint8_t>& stock) {
    if (const char* v = std::getenv("LB_TEST_CUNIT")) {
        const int src = std::atoi(v);
        if (src >= 0 && src < 182) {
            const size_t r = 4 + (size_t)src * 0x7C;
            Entity e;
            e.rec.assign(stock.begin() + (long)r, stock.begin() + (long)r + 0x7C);
            e.name = pool_str(stock, get32(stock, r + 0x00));
            e.asset = pool_str(stock, get32(stock, r + 0x24));
            e.tmpl = src;
            e.folder = "test";
            e.card_src = find_card(stock, src);
            g_ents.push_back(std::move(e));
        }
    }
}

}  // namespace

void load_unit_sounds(const fs::path& dir, std::array<std::shared_ptr<const std::vector<int16_t>>, 5>* snd);   // below

bool lb_cunits_prepare(const std::vector<uint8_t>& stock, const CuReader& read, std::vector<CuFile>* files,
                       std::vector<std::string>* names) {
    g_ents.clear();
    g_hashes.clear();
    g_bundles.clear();
    g_built = false;
    g_report.clear();
    files->clear();
    names->clear();
    if (stock.size() < kPool + 4 || std::memcmp(stock.data(), "BPNZ", 4) != 0) return false;
    g_stock = stock;
    collect_test(stock);
    // The faction palette (KingFaction.NCLR bank 0, team shades as Red's).
    std::vector<uint32_t> fpal;
    {
        std::vector<uint8_t> c;
        if (read("Sprites/KingFaction.NCLR", &c) && c.size() >= 0x28 + 32)
            for (int i = 0; i < 16; ++i) fpal.push_back(ds_rgba((uint16_t)get16(c, 0x28 + (size_t)i * 2)));
        static const uint16_t kRed[3] = {0x113F, 0x005C, 0x0015};
        if (fpal.size() == 16)
            for (int k = 0; k < 3; ++k) fpal[12 + (size_t)k] = ds_rgba(kRed[k]);
    }
    for (const std::string& folder : enabled_folders()) {
        if ((int)g_ents.size() >= kMaxUnits) {
            g_report += folder + ": left out (at most " + std::to_string(kMaxUnits) + " custom units)\n";
            continue;
        }
        Entity e;
        std::vector<CuFile> fl;
        std::string why;
        // Its identity: the hash of what a lobby would send of it. Its files
        // and internal name are named after that, not the folder, so a joiner
        // that received it builds the very same records (docs/448).
        std::vector<uint8_t> bundle;
        if (!make_bundle(units_dir() / fs::u8path(folder), &bundle)) {
            g_report += folder + ": too large to share (over 1 MB)\n";
            continue;
        }
        const uint64_t hash = fnv64(bundle);
        const std::string tag = "U" + hex16(hash).substr(0, 12);
        if (fpal.size() != 16 || !load_unit(folder, tag, stock, read, fpal, &e, &fl, &why)) {
            g_report += folder + ": " + (why.empty() ? std::string("the faction palette is unreadable") : why) + "\n";
            std::fprintf(stderr, "[lb_cunits] %s left out: %s\n", folder.c_str(), why.c_str());
            continue;
        }
        files->insert(files->end(), std::make_move_iterator(fl.begin()), std::make_move_iterator(fl.end()));
        e.card_src = find_card(stock, e.tmpl);
        load_unit_sounds(units_dir() / fs::u8path(folder), &e.snd);
        g_report += folder + ": unit " + std::to_string((int)g_ents.size() + 1) + " (" + e.display + ")\n";
        g_ents.push_back(std::move(e));
        g_hashes.push_back(hash);
        g_bundles.push_back(std::move(bundle));
    }
    for (const Entity& e : g_ents) names->push_back(e.display.empty() ? e.name : e.display);
    return !g_ents.empty();
}

bool lb_cunits_finish(int first_name_id, std::vector<uint8_t>* out) {
    if (g_ents.empty() || g_stock.empty()) return false;
    const size_t n = g_ents.size();
    std::vector<uint8_t> f = g_stock;
    while (f.size() & 3u) f.push_back(0);
    g_block_off = f.size();
    f.resize(f.size() + 8, 0);
    put32(f, g_block_off, kMagic);
    put32(f, g_block_off + 4, (uint32_t)n);
    // Per unit, three blueprints (docs/448). The battle records are NOT first:
    // 0x227 (551) is the army screen's "no unit" (its top-screen card shows the
    // unknown silhouette for it, FUN_020C1828), so a picked unit's index must
    // never be 551.
    //   551 + k        ITS IDENTITY: the same record with +0x04 = its battle
    //                  index (551 + n + k). The army screen reaches a unit through
    //                  its card (+0x60 -> FUN_02001D74) and matches and writes
    //                  picks by that +0x04 (FUN_020B859C), so it must read the
    //                  unit's own index.
    //   551 + n + k    THE BATTLE RECORD. +0x04 KEEPS THE TEMPLATE'S INDEX: the
    //                  game reaches a unit's blueprint by pointer (entity +0x160
    //                  -> +8), so stats, sprites and name are this record's,
    //                  while the lookups keyed by the number at +0x04 -- the
    //                  sound bank (FUN_02088A18 -> FUN_02089AAC, which marks a
    //                  bank started at 0x02155878 + bank * 0x14 and with 551
    //                  wrote 1.3 MB past it: run cu448c crashed), the head key
    //                  table -- see the template. Those are called directly by
    //                  lifted code, so no override can stand in for them. The
    //                  roster and the Barracks use this index (run cu448i).
    //   551 + 2n + k   ITS CARD: the template's minifig card (category 0x0B,
    //                  kind 2) with +0x60 = the identity and its own +0x04; the
    //                  template's unlock bits, so it is unlocked when that is.
    for (size_t k = 0; k < n; ++k) {
        Entity& e = g_ents[k];
        if (first_name_id >= 0 && e.folder != "test") {
            const uint32_t id = (uint32_t)(first_name_id + (int)k);
            e.rec[6] = (uint8_t)id;
            e.rec[7] = (uint8_t)(id >> 8);
        }
        e.rec_off = f.size();
        f.insert(f.end(), e.rec.begin(), e.rec.end());
        e.id_off = f.size();
        f.insert(f.end(), e.rec.begin(), e.rec.end());
        f[e.id_off + 4] = (uint8_t)((kStock + n + k) & 0xFF);
        f[e.id_off + 5] = (uint8_t)((kStock + n + k) >> 8);
        e.card_off = 0;
        if (e.card_src >= 0) {
            const size_t src = kColBase + (size_t)(e.card_src - 209) * 0x70;
            e.card_off = f.size();
            f.insert(f.end(), g_stock.begin() + (long)src, g_stock.begin() + (long)src + 0x70);
            const uint32_t own = (uint32_t)(kStock + 2 * n + k), unit = (uint32_t)(kStock + k);
            f[e.card_off + 4] = (uint8_t)own;
            f[e.card_off + 5] = (uint8_t)(own >> 8);
            put32(f, e.card_off + 0x60, unit);
        }
        e.wrap_off = f.size();
        f.resize(f.size() + 0x10, 0);
        e.id_wrap_off = f.size();
        f.resize(f.size() + 0x10, 0);
        e.card_wrap_off = f.size();
        f.resize(f.size() + 0x10, 0);
    }
    // The larger blueprint array: the stock 551, then the three groups.
    g_array_off = f.size();
    f.resize(f.size() + (size_t)(kStock + 3 * n) * 4u, 0);
    for (Entity& e : g_ents) {
        e.name_off = f.size();
        f.insert(f.end(), e.name.begin(), e.name.end());
        f.push_back(0);
        e.asset_off = f.size();
        f.insert(f.end(), e.asset.begin(), e.asset.end());
        f.push_back(0);
    }
    *out = lvl::comp_wrap(f);
    g_built = true;
    std::fprintf(stderr, "[lb_cunits] %zu custom blueprint(s) appended to BP/Entities.ebp "
                 "(%zu -> %zu bytes)\n", n, g_stock.size(), f.size());
    for (size_t k = 0; k < n; ++k)
        std::fprintf(stderr, "[lb_cunits]   %d  %-20s from %d (card %d), asset %s\n", kStock + (int)(n + k),
                     g_ents[k].display.c_str(), g_ents[k].tmpl, g_ents[k].card_src, g_ents[k].asset.c_str());
    std::fflush(stderr);
    return true;
}

extern "C" int lb_cunits_count(void) { return g_built ? (int)g_ents.size() : 0; }
// Custom unit k's battle blueprint (the index a roster, a Barracks and the
// army screen's picks use), and back; -1 for anything else.
extern "C" int lb_cunits_battle(int k) {
    return g_built && k >= 0 && k < (int)g_ents.size() ? kStock + (int)g_ents.size() + k : -1;
}
extern "C" int lb_cunits_of_battle(int bp) {
    const int n = g_built ? (int)g_ents.size() : 0;
    return bp >= kStock + n && bp < kStock + 2 * n ? bp - kStock - n : -1;
}
// Custom unit k of any of its three blueprints (identity, battle, card), or -1.
extern "C" int lb_cunits_of_any(int bp) {
    const int n = g_built ? (int)g_ents.size() : 0;
    return n > 0 && bp >= kStock && bp < kStock + 3 * n ? (bp - kStock) % n : -1;
}
// Custom unit k's own head (portraits/head.png) as 24x24 RGBA; 0 when it has none.
extern "C" int lb_cunits_head(int k, unsigned int out[576]) {
    if (!g_built || k < 0 || k >= (int)g_ents.size() || g_ents[(size_t)k].head.size() != 576u) return 0;
    for (int i = 0; i < 576; ++i) out[i] = g_ents[(size_t)k].head[(size_t)i];
    return 1;
}
extern "C" int lb_cunits_template(int k) { return g_built && k >= 0 && k < (int)g_ents.size() ? g_ents[(size_t)k].tmpl : -1; }
extern "C" int lb_cunits_category(int k) {
    return g_built && k >= 0 && k < (int)g_ents.size() ? g_ents[(size_t)k].rec[0x5C] : -1;
}
extern "C" const char* lb_cunits_name(int k) {
    return g_built && k >= 0 && k < (int)g_ents.size() ? g_ents[(size_t)k].display.c_str() : "";
}
extern "C" const char* lb_cunits_folder(int k) {
    return g_built && k >= 0 && k < (int)g_ents.size() ? g_ents[(size_t)k].folder.c_str() : "";
}
extern "C" const char* lb_cunits_report(void) { return g_report.c_str(); }

static void census(uint32_t base);

// The army screen's two 551-bounded walks (docs/448): FUN_020B96FC lists the
// cards, FUN_020B859C finds the card of each pick. Their literal-pool words are
// raised to cover the custom cards while they are registered; every other 0x227
// walk (the collection screen, the store, the cheats) keeps 551.
constexpr uint32_t kWalkLits[2] = {0x020B9890u, 0x020B868Cu};
struct Saved { uint32_t data = 0, count = 0, cap = 0; bool live = false; };
Saved g_saved;   // the game's own array while ours stands in for it

// Puts the game's array and the walks back (before the game reloads the file:
// lb_cunits_before_reload, from the override of func_02001EE0).
extern "C" void lb_cunits_before_reload(void) {
    if (!g_saved.live) return;
    bus_write_u32(kPtrArray + 4u, g_saved.data);
    bus_write_u32(kPtrArray + 8u, g_saved.count);
    bus_write_u32(kPtrArray + 0xCu, g_saved.cap);
    for (uint32_t a : kWalkLits)
        if (bus_read_u32(a) == (uint32_t)(kStock + 3 * g_ents.size())) bus_write_u32(a, (uint32_t)kStock);
    g_saved.live = false;
    g_pushed_base = 0;
    std::fprintf(stderr, "[lb_cunits] the blueprint file is reloading: the game's own array is back\n");
    std::fflush(stderr);
}

// Once a frame (lb_patches.cpp): register the appended records with the game
// whenever a freshly loaded blueprint table does not have them.
extern "C" void lb_cunits_tick(void) {
    if (!g_built || !lb_patches_blueprint_settled()) return;
    const size_t n = g_ents.size();
    const uint32_t data = bus_read_u32(kPtrArray + 4u);
    const uint32_t count = bus_read_u32(kPtrArray + 8u);
    const uint32_t cap = bus_read_u32(kPtrArray + 0xCu);
    if (data < 0x02000000u || data >= 0x02400000u || count < (uint32_t)kStock) return;
    const uint32_t w0 = bus_read_u32(data);
    const uint32_t rec0 = w0 >= 0x02000000u && w0 < 0x02400000u ? bus_read_u32(w0 + 4u) : 0u;
    if (rec0 < 0x02000004u || rec0 >= 0x02400000u) return;
    const uint32_t base = rec0 - 4u;   // the file buffer: "BPNZ", then record 0
    if (base == g_pushed_base && g_saved.live) {
        static unsigned t = 0;
        if (std::getenv("LB_TEST_CUNIT") && (++t % 60u) == 0u) census(base);
        return;
    }
    if (bus_read_u32(base) != 0x5A4E5042u ||                       // "BPNZ"
        bus_read_u32(base + (uint32_t)g_block_off) != kMagic) {
        static bool said = false;
        if (!said) {
            said = true;
            std::fprintf(stderr, "[lb_cunits] the loaded blueprint file is not ours "
                         "(base %08X); custom units are off\n", (unsigned)base);
        }
        return;
    }
    if (count != (uint32_t)kStock) return;   // someone else's table shape: leave it
    for (uint32_t a : kWalkLits)
        if (bus_read_u32(a) != (uint32_t)kStock) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "[lb_cunits] the army screen's walk at %08X is not the "
                             "cartridge's; custom units are off\n", (unsigned)a);
            }
            return;
        }
    // The card wrapper's class: a stock minifig card's (kind 2).
    uint32_t card_vt = 0;
    for (const Entity& e : g_ents)
        if (e.card_src >= 0) {
            const uint32_t w = bus_read_u32(data + (uint32_t)e.card_src * 4u);
            if (w >= 0x02000000u && w < 0x02400000u) card_vt = bus_read_u32(w);
            break;
        }
    const uint32_t arr = base + (uint32_t)g_array_off;
    for (uint32_t i = 0; i < (uint32_t)kStock; ++i) bus_write_u32(arr + i * 4u, bus_read_u32(data + i * 4u));
    for (size_t k = 0; k < n; ++k) {
        const Entity& e = g_ents[k];
        const uint32_t name = base + (uint32_t)e.name_off, asset = base + (uint32_t)e.asset_off;
        const uint32_t rec = base + (uint32_t)e.rec_off, idr = base + (uint32_t)e.id_off;
        for (uint32_t r : {rec, idr}) {
            bus_write_u32(r + 0x00u, name);
            bus_write_u32(r + 0x24u, asset);
        }
        const uint32_t w1 = base + (uint32_t)e.wrap_off, w2 = base + (uint32_t)e.id_wrap_off;
        bus_write_u32(w1 + 0u, kUnitVtable); bus_write_u32(w1 + 4u, rec); bus_write_u32(w1 + 8u, rec);
        bus_write_u32(w2 + 0u, kUnitVtable); bus_write_u32(w2 + 4u, idr); bus_write_u32(w2 + 8u, idr);
        bus_write_u32(arr + (uint32_t)(kStock + n + k) * 4u, w1);
        bus_write_u32(arr + (uint32_t)(kStock + k) * 4u, w2);
        uint32_t w3 = 0;
        if (e.card_off && card_vt) {
            // The card's strings are its template card's (the loader made those pointers).
            const uint32_t card = base + (uint32_t)e.card_off;
            const uint32_t sw = bus_read_u32(data + (uint32_t)e.card_src * 4u);
            const uint32_t srec = sw >= 0x02000000u && sw < 0x02400000u ? bus_read_u32(sw + 4u) : 0u;
            if (srec >= 0x02000000u && srec < 0x02400000u) {
                bus_write_u32(card + 0x00u, bus_read_u32(srec + 0x00u));
                bus_write_u32(card + 0x24u, bus_read_u32(srec + 0x24u));
                w3 = base + (uint32_t)e.card_wrap_off;
                bus_write_u32(w3 + 0u, card_vt); bus_write_u32(w3 + 4u, card); bus_write_u32(w3 + 8u, card);
            }
        }
        // A unit without a card still needs a valid entry there: its identity
        // wrapper, which the card walk skips (its category is not 0x0B).
        bus_write_u32(arr + (uint32_t)(kStock + 2 * n + k) * 4u, w3 ? w3 : w2);
    }
    const uint32_t total = (uint32_t)(kStock + 3 * n);
    g_saved = Saved{data, count, cap, true};
    bus_write_u32(kPtrArray + 4u, arr);
    bus_write_u32(kPtrArray + 0xCu, total * 4u);
    bus_write_u32(kPtrArray + 8u, total);
    for (uint32_t a : kWalkLits) bus_write_u32(a, total);
    g_pushed_base = base;
    std::fprintf(stderr, "[lb_cunits] registered %zu custom blueprint(s) at %d.., identities at "
                 "%d.., cards at %d.. (array %08X, %u entries)\n", n, kStock + (int)n, kStock,
                 kStock + 2 * (int)n, (unsigned)arr, (unsigned)total);
    std::fflush(stderr);
}

// Live units of each appended blueprint, per owner, logged when it changes
// (the evidence that a roster pick, a Barracks or the AI made one).
static void census(uint32_t base) {
    static std::string last;
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (pool < 0x02000000u || pool >= 0x02400000u) return;
    const uint32_t array = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    if (array < 0x02000000u || array >= 0x02400000u) return;
    std::string now;
    for (size_t k = 0; k < g_ents.size(); ++k) {
        const uint32_t rec = base + (uint32_t)g_ents[k].rec_off;
        int per[8] = {0};
        for (int32_t i = 0; i < count && i < 4096; ++i) {
            const uint32_t o = bus_read_u32(array + (uint32_t)i * 4u);
            if (o < 0x02000000u || o >= 0x02400000u || bus_read_u8(o + 0x227u) == 0u) continue;
            const uint32_t t = bus_read_u32(o + 0x160u);   // +8 is the record (lb_field.cpp)
            if (t < 0x02000000u || t >= 0x02400000u || bus_read_u32(t + 8u) != rec) continue;
            ++per[bus_read_u8(o + 0xE4u) & 7u];
        }
        char b[96];
        std::snprintf(b, sizeof b, " #%zu: %d %d %d %d", (size_t)kStock + g_ents.size() + k, per[0], per[1], per[2], per[3]);
        now += b;
    }
    if (now != last) {
        last = now;
        std::fprintf(stderr, "[lb_cunits] live units by owner 0..3:%s\n", now.c_str());
        std::fflush(stderr);
    }
}

// TEST AID (LB_TEST_CUNIT): the army record field player 1 fields the appended
// blueprint in -- the source's own role: hero +0x08, builder +0x0A, melee
// +0x0C, ranged +0x0E, mounted +0x10 -- or -1. A builder or hero clone is on
// the field from the first frame (Free Play starts with them). The first
// custom unit when no clone is asked for but LB_TEST_CUNIT_FIELD is set.
extern "C" int lb_cunits_test_field(void) {
    if (!g_built || g_ents.empty() || !(std::getenv("LB_TEST_CUNIT") || std::getenv("LB_TEST_CUNIT_FIELD"))) return -1;
    const int cat = g_ents[0].rec[0x5C];
    return cat >= 0 && cat <= 4 ? 0x08 + 2 * cat : -1;
}
extern "C" int lb_cunits_test_pick(void) { return lb_cunits_test_field() >= 0 ? lb_cunits_battle(0) : -1; }

// ---- the menu's preview ------------------------------------------------------
extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);   // lb_teams.cpp

bool lb_cunits_folder_sheet(const std::string& folder, int tmpl, const std::string& tsheet, int* w, int* h,
                            std::vector<uint8_t>* px) {
    auto rom = [](const std::string& path, std::vector<uint8_t>* out) {
        const int n = lb_rom_read_unwrapped(path.c_str(), nullptr, 0);
        if (n <= 0 || n > (16 << 20)) return false;
        out->resize((size_t)n);
        return lb_rom_read_unwrapped(path.c_str(), out->data(), n) == n;
    };
    static std::vector<uint8_t> stock;
    if (stock.empty() && !rom("BP/Entities.ebp", &stock)) return false;
    if (tmpl < 0 || tmpl >= 182 || stock.size() < kPool) return false;
    std::vector<uint8_t> raw;
    Sheet s;
    if (!rom("Sprites/" + tsheet + ".NCBR", &raw) || !parse_sheet(raw, &s)) return false;
    std::vector<uint32_t> fpal;
    std::vector<uint8_t> c;
    if (rom("Sprites/KingFaction.NCLR", &c) && c.size() >= 0x28 + 32)
        for (int i = 0; i < 16; ++i) fpal.push_back(ds_rgba((uint16_t)get16(c, 0x28 + (size_t)i * 2)));
    if (fpal.size() != 16) return false;
    static const uint16_t kRed[3] = {0x113F, 0x005C, 0x0015};
    for (int k = 0; k < 3; ++k) fpal[12 + (size_t)k] = ds_rgba(kRed[k]);
    const fs::path dir = units_dir() / fs::u8path(safe_folder(folder));
    const size_t r = 4 + (size_t)tmpl * 0x7C;
    const std::vector<uint8_t> rec(stock.begin() + (long)r, stock.begin() + (long)r + 0x7C);
    const std::string tasset = pool_str(stock, get32(stock, r + 0x24));
    const std::string tbase = tasset.size() > 8 ? tasset.substr(8) : tasset;
    std::vector<uint8_t> mine;
    Sheet ms;
    Img im;
    if (slurp(dir / "sheets" / (tsheet + ".NCBR"), &mine) && parse_sheet(mine, &ms) && ms.w == s.w && ms.h == s.h) s.px = ms.px;
    else if (slurp(dir / "sheets" / (tsheet + ".png"), &mine) && read_png(mine, &im) && im.w == s.w && im.h == s.h) blit(s, im, 0, 0, fpal);
    for (const Frame& fr : frames_of(rec[0x5C], tbase, rec)) {
        if (fr.sheet != tsheet) continue;
        char nm[32];
        std::snprintf(nm, sizeof nm, "d%d_%d.png", fr.dir, fr.n);
        if (slurp(dir / "frames" / fr.action / nm, &mine) && read_png(mine, &im) && im.w == fr.w && im.h == fr.h)
            blit(s, im, fr.x, fr.y, fpal);
    }
    *w = s.w;
    *h = s.h;
    *px = std::move(s.px);
    return true;
}

// ---- a lobby's custom units (docs/448) ---------------------------------------
//
// Every machine in a match must have built the SAME custom units in the SAME
// order: the records are simulation data and their indices are what armies
// hold. The host's rules carry its set as an ordered list of bundle hashes
// (lb_teams.cpp, the 'U' page). A joiner whose built set differs fetches each
// bundle it lacks from the host (lb_maptx.cpp, the map transfer's chunks) into
// mods/units/_received/<hash>/, writes the host's list to mods/units/lobby.txt,
// and the cartridge is rebuilt with exactly that list once (lb_cunits_prepare
// reads lobby.txt in place of enabled.txt and removes it). Until its build
// matches, the joiner cannot Ready (lb_cunits_lobby_ok).
//
// A bundle is what the importer reads, nothing else:
//   "LBUB" 1, u16 count, then per file: u16 path length, path, u32 length, bytes
// over unit.json, sheets/*, frames/*/*.png, portraits/card.png,
// portraits/card_top_screen.png, model/texture.png. Its identity is the FNV-1a
// 64 of the bundle bytes.

// ---- sounds (docs/448) ---------------------------------------------------------
// A unit's sounds/<NAME>.wav, by the name's ending: ONSELECT, ONDEATH,
// ONSTRIKE, ONHIT, ONSPECIAL (the export's names, e.g. SE_KING_BUILDER_ONHIT),
// or plain SELECT, DEATH, STRIKE, HIT, SPECIAL. These are the cartridge's
// sequence indices 0..4 in an entity's own archive (func_02089270). Each is
// read as PCM (8 or 16 bit, mono or stereo, any rate) and kept as mono 16-bit
// at the game's output rate, then mixed into the game's own audio (the
// frontend's audio callback, apply_hooks.py) when the cartridge asks a custom
// unit for that sound.

namespace {

constexpr int kMixRate = 33513982 / 1024;   // frontend.cpp kAudioFrequency
constexpr size_t kMaxSoundSamples = (size_t)kMixRate * 6;   // 6 s at most

int sound_role(std::string stem) {
    for (char& c : stem) c = (char)std::toupper((unsigned char)c);
    static const char* const kRoles[5] = {"SELECT", "DEATH", "STRIKE", "HIT", "SPECIAL"};
    for (int i = 0; i < 5; ++i) {
        const std::string r = kRoles[i], on = std::string("ON") + kRoles[i];
        if (stem == r) return i;
        if (stem.size() >= on.size() && stem.compare(stem.size() - on.size(), on.size(), on) == 0) return i;
    }
    return -1;
}

// RIFF WAVE PCM to mono 16-bit at kMixRate; false when it is not one.
bool wav_to_mix(const std::vector<uint8_t>& b, std::vector<int16_t>* out) {
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t data = 0, dlen = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        const uint32_t n = get32(b, p + 4);
        if (n > b.size() - p - 8) break;
        if (!std::memcmp(b.data() + p, "fmt ", 4) && n >= 16) {
            fmt = b[p + 8] | (b[p + 9] << 8);
            ch = b[p + 10] | (b[p + 11] << 8);
            rate = (int)get32(b, p + 12);
            bits = b[p + 22] | (b[p + 23] << 8);
        } else if (!std::memcmp(b.data() + p, "data", 4)) {
            data = p + 8;
            dlen = n;
        }
        p += 8 + n + (n & 1);
    }
    if (fmt != 1 || ch < 1 || ch > 2 || rate < 4000 || rate > 96000 || (bits != 8 && bits != 16) || !data) return false;
    const size_t frame = (size_t)ch * (size_t)(bits / 8);
    const size_t frames = dlen / frame;
    if (!frames) return false;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (int c = 0; c < ch; ++c) {
            const size_t o = data + i * frame + (size_t)c * (size_t)(bits / 8);
            s += bits == 16 ? (float)(int16_t)(b[o] | (b[o + 1] << 8)) : (float)((int)b[o] - 128) * 256.0f;
        }
        mono[i] = s / (float)ch;
    }
    const double step = (double)rate / (double)kMixRate;
    const size_t n = std::min(kMaxSoundSamples, (size_t)((double)frames / step));
    out->resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = (double)i * step;
        const size_t a = (size_t)x;
        const float t = (float)(x - (double)a);
        const float v = mono[std::min(a, frames - 1)] * (1.0f - t) + mono[std::min(a + 1, frames - 1)] * t;
        (*out)[i] = (int16_t)std::max(-32768.0f, std::min(32767.0f, v));
    }
    return n > 0;
}

// The voices playing now; the audio thread mixes them.
struct Voice { std::shared_ptr<const std::vector<int16_t>> pcm; size_t pos = 0; };
std::mutex g_mix_mu;
std::vector<Voice> g_voices;

}  // namespace

// A unit folder's sounds, by role (lb_cunits_prepare).
void load_unit_sounds(const fs::path& dir, std::array<std::shared_ptr<const std::vector<int16_t>>, 5>* snd) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "sounds", ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".wav") continue;
        const int role = sound_role(utf8(e.path().stem()));
        if (role < 0 || (*snd)[(size_t)role]) continue;
        std::vector<uint8_t> b;
        auto pcm = std::make_shared<std::vector<int16_t>>();
        if (slurp(e.path(), &b, 4u << 20) && wav_to_mix(b, pcm.get())) (*snd)[(size_t)role] = pcm;
    }
}

// The frontend's audio callback: add the voices to `frames` stereo frames.
extern "C" void lb_cunits_audio_mix(int16_t* stereo, unsigned frames) {
    std::lock_guard<std::mutex> lk(g_mix_mu);
    if (g_voices.empty()) return;
    for (unsigned i = 0; i < frames; ++i) {
        int add = 0;
        for (Voice& v : g_voices)
            if (v.pos < v.pcm->size()) add += (*v.pcm)[v.pos++];
        if (!add) continue;
        for (int c = 0; c < 2; ++c) {
            const int s = stereo[i * 2u + (unsigned)c] + add;
            stereo[i * 2u + (unsigned)c] = (int16_t)std::max(-32768, std::min(32767, s));
        }
    }
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                  [](const Voice& v) { return v.pos >= v.pcm->size(); }),
                   g_voices.end());
}

// func_02089270's override (lb_patches.cpp): the cartridge asks entity `ent`
// (resolved from its handle) for sound `index`. 1 when it is a custom unit
// with its own sound for that index, which then plays (when the cartridge's
// audible window holds the entity) instead of its template's.
extern "C" int lb_cunits_entity_sound(uint32_t mgr, uint32_t ent, int index) {
    if (!g_built || !g_pushed_base || index < 0 || index > 4 || ent < 0x02000000u || ent >= 0x02400000u) return 0;
    const uint32_t t = bus_read_u32(ent + 0x160u);
    if (t < 0x02000000u || t >= 0x02400000u) return 0;
    const uint32_t rec = bus_read_u32(t + 8u);
    for (size_t k = 0; k < g_ents.size(); ++k) {
        if (rec != g_pushed_base + (uint32_t)g_ents[k].rec_off) continue;
        const auto& pcm = g_ents[k].snd[(size_t)index];
        if (!pcm || pcm->empty()) return 0;
        // The cartridge's window test (func_0208938C over func_0205A00C), on
        // the window it last cached; with none cached, the sound plays.
        if (bus_read_u8(mgr + 0x2Cu)) {
            const uint8_t cx = bus_read_u8(ent + 0x128u), cy = bus_read_u8(ent + 0x129u);
            if (cx < bus_read_u8(mgr + 0x18u) || cy < bus_read_u8(mgr + 0x19u) ||
                cx > bus_read_u8(mgr + 0x20u) || cy > bus_read_u8(mgr + 0x21u))
                return 1;
        }
        // One start per unit and sound at a time, as the cartridge's started
        // array allows one per archive and index a pass.
        std::lock_guard<std::mutex> lk(g_mix_mu);
        for (const Voice& v : g_voices)
            if (v.pcm == pcm && v.pos < (size_t)kMixRate / 10) return 1;
        if (g_voices.size() < 8) g_voices.push_back(Voice{pcm, 0});
        static unsigned said = 0;
        if (said < 24) {
            ++said;
            std::fprintf(stderr, "[lb_cunits] %s plays its own sound %d (%zu samples)\n",
                         g_ents[k].display.c_str(), index, pcm->size());
            std::fflush(stderr);
        }
        return 1;
    }
    return 0;
}

extern "C" void lb_maptx_want_unit(unsigned long long hash);   // lb_maptx.cpp
extern "C" int lb_maptx_idle(void);
extern "C" int lb_mods_request_rebuild(void);                  // lb_mods.cpp
extern "C" void lb_overlay_notice(const char* text);            // lb_overlay.cpp

namespace {

constexpr size_t kMaxUnitBundle = 1u << 20;

uint64_t fnv64(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// The files of a unit folder a bundle carries, as relative generic paths.
std::vector<std::string> bundle_files(const fs::path& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    if (fs::exists(dir / "unit.json", ec)) v.push_back("unit.json");
    for (const char* sub : {"sheets"}) {
        for (const auto& e : fs::directory_iterator(dir / sub, ec)) {
            const std::string n = utf8(e.path().filename());
            const std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
            if (e.is_regular_file(ec) && (ext == ".png" || ext == "NCBR")) v.push_back(std::string(sub) + "/" + n);
        }
    }
    for (const auto& a : fs::directory_iterator(dir / "frames", ec)) {
        if (!a.is_directory(ec)) continue;
        for (const auto& e : fs::directory_iterator(a.path(), ec)) {
            const std::string n = utf8(e.path().filename());
            if (e.is_regular_file(ec) && n.size() > 4 && n.substr(n.size() - 4) == ".png")
                v.push_back("frames/" + utf8(a.path().filename()) + "/" + n);
        }
    }
    for (const auto& e : fs::directory_iterator(dir / "sounds", ec)) {
        const std::string n = utf8(e.path().filename());
        if (e.is_regular_file(ec) && n.size() > 4 && n.substr(n.size() - 4) == ".wav") v.push_back("sounds/" + n);
    }
    for (const char* f : {"portraits/card.png", "portraits/card_top_screen.png", "portraits/head.png", "model/texture.png"})
        if (fs::exists(dir / f, ec)) v.push_back(f);
    std::sort(v.begin(), v.end());
    return v;
}

bool make_bundle(const fs::path& dir, std::vector<uint8_t>* out) {
    const std::vector<std::string> files = bundle_files(dir);
    out->assign({'L', 'B', 'U', 'B', 1});
    out->push_back((uint8_t)files.size());
    out->push_back((uint8_t)(files.size() >> 8));
    for (const std::string& f : files) {
        std::vector<uint8_t> d;
        if (!slurp(dir / fs::u8path(f), &d, kMaxUnitBundle)) d.clear();
        out->push_back((uint8_t)f.size());
        out->push_back((uint8_t)(f.size() >> 8));
        out->insert(out->end(), f.begin(), f.end());
        for (int i = 0; i < 4; ++i) out->push_back((uint8_t)(d.size() >> (8 * i)));
        out->insert(out->end(), d.begin(), d.end());
        if (out->size() > kMaxUnitBundle) return false;
    }
    return true;
}

// A path a bundle may write: one of the shapes above, no "..", no absolute.
bool bundle_path_ok(const std::string& p) {
    if (p.empty() || p.size() > 96 || p.find("..") != std::string::npos || p[0] == '/' || p.find('\\') != std::string::npos ||
        p.find(':') != std::string::npos)
        return false;
    for (char c : p)
        if (!(std::isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '.')) return false;
    return p == "unit.json" || p.compare(0, 7, "sheets/") == 0 || p.compare(0, 7, "frames/") == 0 ||
           (p.compare(0, 7, "sounds/") == 0 && p.size() > 11 && p.compare(p.size() - 4, 4, ".wav") == 0 &&
            p.find('/', 7) == std::string::npos) ||
           p == "portraits/card.png" || p == "portraits/card_top_screen.png" || p == "portraits/head.png" ||
           p == "model/texture.png";
}

std::string hex16(uint64_t h) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)h);
    return b;
}

std::vector<uint64_t> g_lobby;                // the host's set, as last received
bool g_lobby_has = false;                     // a 'U' page has been seen in this lobby
bool g_lobby_rebuilt = false;                 // the rebuild was asked for this set

// The folder holding a unit with bundle `h`: one of this machine's own (any
// folder in mods/units) or a received one. Empty when none.
std::string folder_for(uint64_t h) {
    std::error_code ec;
    const fs::path rec = units_dir() / "_received" / hex16(h);
    if (fs::exists(rec / "unit.json", ec)) return "_received/" + hex16(h);
    for (const auto& e : fs::directory_iterator(units_dir(), ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string n = utf8(e.path().filename());
        if (n == "export" || n == "_received") continue;
        std::vector<uint8_t> b;
        if (make_bundle(e.path(), &b) && fnv64(b) == h) return n;
    }
    return "";
}

}  // namespace

// The built set's identities, in order (the host's 'U' page).
extern "C" int lb_cunits_hashes(unsigned long long* out, int cap) {
    int n = 0;
    for (uint64_t h : g_hashes)
        if (n < cap) out[n++] = h;
    return n;
}

// The host: a built unit's bundle by its identity.
bool lb_cunits_bundle(uint64_t hash, std::vector<uint8_t>* out) {
    for (size_t k = 0; k < g_hashes.size() && k < g_bundles.size(); ++k)
        if (g_hashes[k] == hash) {
            *out = g_bundles[k];
            return true;
        }
    return false;
}

// A bundle's files written to mods/units/<folder> (replacing it), after
// checking its shape and every path. True when written.
bool lb_cunits_unpack(const std::vector<uint8_t>& b, const std::string& folder) {
    if (b.size() < 7 || b.size() > kMaxUnitBundle || std::memcmp(b.data(), "LBUB\x01", 5) != 0) return false;
    if (folder.empty() || folder.find("..") != std::string::npos || folder[0] == '/' ||
        folder.find('\\') != std::string::npos || folder.find(':') != std::string::npos)
        return false;
    const size_t count = (size_t)b[5] | ((size_t)b[6] << 8);
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    size_t at = 7;
    for (size_t i = 0; i < count; ++i) {
        if (at + 2 > b.size()) return false;
        const size_t pl = (size_t)b[at] | ((size_t)b[at + 1] << 8);
        at += 2;
        if (at + pl + 4 > b.size()) return false;
        const std::string p((const char*)b.data() + at, pl);
        at += pl;
        const size_t dl = get32(b, at);
        at += 4;
        if (dl > kMaxUnitBundle || at + dl > b.size() || !bundle_path_ok(p)) return false;
        files.push_back({p, std::vector<uint8_t>(b.begin() + (long)at, b.begin() + (long)(at + dl))});
        at += dl;
    }
    if (at != b.size()) return false;
    std::error_code ec;
    const fs::path dir = units_dir() / fs::u8path(folder);
    fs::remove_all(dir, ec);
    for (const auto& f : files) {
        const fs::path p = dir / fs::u8path(f.first);
        fs::create_directories(p.parent_path(), ec);
        std::FILE* o = std::fopen(utf8(p).c_str(), "wb");
        if (!o) return false;
        const bool ok = std::fwrite(f.second.data(), 1, f.second.size(), o) == f.second.size();
        std::fclose(o);
        if (!ok) return false;
    }
    return true;
}

// A folder's bundle (what a lobby or Share online sends); false when over 1 MB.
bool lb_cunits_folder_bundle(const std::string& folder, std::vector<uint8_t>* out) {
    return make_bundle(units_dir() / fs::u8path(folder), out);
}

// The joiner: a bundle that arrived, checked against its identity and written
// to mods/units/_received/<hash>/. True when written.
bool lb_cunits_received(uint64_t hash, const std::vector<uint8_t>& b) {
    return b.size() >= 7 && fnv64(b) == hash && lb_cunits_unpack(b, "_received/" + hex16(hash));
}

// A PNG as RGBA (0xAABBGGRR), for the community list's pictures.
bool lb_cunits_png_rgba(const std::vector<uint8_t>& png, int* w, int* h, std::vector<uint32_t>* px) {
    Img im;
    if (!read_png(png, &im) || im.w <= 0 || im.h <= 0) return false;
    *w = im.w;
    *h = im.h;
    *px = std::move(im.rgba);
    return true;
}

// The joiner, each time the host's rules arrive: the host's set (n hashes, in
// order; n = -1 when its rules carry no 'U' page, i.e. an older host: none).
extern "C" void lb_cunits_lobby_set(const unsigned long long* hashes, int n) {
    std::vector<uint64_t> want;
    for (int i = 0; i < n; ++i) want.push_back(hashes[i]);
    if (!g_lobby_has || want != g_lobby) g_lobby_rebuilt = false;
    g_lobby = want;
    g_lobby_has = true;
    if (want == g_hashes || g_lobby_rebuilt) return;
    // Fetch what is missing, one at a time.
    for (uint64_t h : want)
        if (folder_for(h).empty()) {
            if (lb_maptx_idle()) lb_maptx_want_unit(h);
            return;
        }
    // All here: build with exactly the host's set.
    std::string o;
    for (uint64_t h : want) o += folder_for(h) + "\n";
    std::error_code ec;
    fs::create_directories(units_dir(), ec);
    std::FILE* f = std::fopen(utf8(units_dir() / "lobby.txt").c_str(), "wb");
    if (f) {
        std::fwrite(o.data(), 1, o.size(), f);
        std::fclose(f);
    }
    g_lobby_rebuilt = true;
    std::fprintf(stderr, "[lb_cunits] the host's %zu custom unit(s) are here; rebuilding with them\n", want.size());
    std::fflush(stderr);
    lb_overlay_notice(lb_mods_request_rebuild()
                          ? "The host's custom units are loading: the game resets. Join the lobby again."
                          : "Restart the game to load the host's custom units, then join again.");
}

// Leaving a lobby.
extern "C" void lb_cunits_lobby_forget(void) {
    g_lobby.clear();
    g_lobby_has = false;
    g_lobby_rebuilt = false;
}

// 1 when this machine's build matches the host's set (or no host set is known).
extern "C" int lb_cunits_lobby_ok(void) { return (!g_lobby_has || g_lobby == g_hashes) ? 1 : 0; }
