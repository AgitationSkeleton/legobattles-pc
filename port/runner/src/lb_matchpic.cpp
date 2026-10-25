// docs/428: THE PICTURE OF A MATCH for the master server's Discord log.
//
// The host captures every player's army at the multiplayer start (the roster
// builder's override, lb_patches.cpp, calls lb_matchpic_note_player), and when
// the match is under way (lb_net's "playing") draws one RGBA picture on the CPU:
//   * a vertical blue gradient sampled from the main menu's own background
//     palette (UI/AllInOne/Iconic_MainMenu_BG.NCLR);
//   * one column per player: the name in the player's colour, then the nine
//     units as 24x24 battle-HUD icons (lb_teams) with their names;
//   * the map's Free Play preview (mpNNmini.NCGR, 4 bpp tiled), scaled up, and
//     the map's name;
//   * a "Banned:" strip of the host's banned units' icons;
// all text in the game's UI font (Font/EN/font.NFTR, 1 bpp glyph tiles), or a
// built-in 5x7 font if that cannot be read. It is encoded as PNG (zlib) and
// posted by lb_net. Nothing here touches the simulation.
//
// Test aids: LB_MATCHPIC_FILE=<path> also writes the PNG there;
// LB_MATCHPIC_TEST=1 draws one from fake data once the front end is up.

#include "lb_overlay.h"

#include <zlib.h>

extern "C" {
#include "runtime_arm.h"   // bus_read_u8: the lobby's player records
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" void lb_net_post_match_image(const unsigned char* png, size_t n,
                                        const char* map_name);
extern "C" int lb_mods_world(int k);   // lb_mods.cpp: 0 King, 1 Pirate, 2 Mars

namespace {

// ---- the picture --------------------------------------------------------------
struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // R,G,B,A in memory order
};
inline uint32_t rgba(int r, int g, int b, int a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
inline void blend(Img& im, int x, int y, uint32_t c, int alpha) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h || alpha <= 0) return;
    uint32_t& d = im.px[(size_t)y * im.w + x];
    const int a = alpha > 255 ? 255 : alpha;
    const int r = (int)(c & 255u), g = (int)((c >> 8) & 255u), b = (int)((c >> 16) & 255u);
    const int dr = (int)(d & 255u), dg = (int)((d >> 8) & 255u), db = (int)((d >> 16) & 255u);
    d = rgba((r * a + dr * (255 - a)) / 255, (g * a + dg * (255 - a)) / 255,
             (b * a + db * (255 - a)) / 255);
}
void fill_rect(Img& im, int x0, int y0, int w, int h, uint32_t c, int alpha) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) blend(im, x, y, c, alpha);
}

// ---- the game's UI font (NFTR) -------------------------------------------------
uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

struct Font {
    bool ok = false;
    int tw = 0, th = 0, tsize = 0, bpp = 1, line = 13;
    std::vector<uint8_t> data;            // the glyph bitmaps
    struct W { int left, gw, cw; };
    std::vector<W> widths;                // per glyph index
    std::map<uint32_t, int> cmap;         // character -> glyph index
    W def{0, 6, 6};
};
Font g_font;
bool g_font_tried = false;

void font_load() {
    if (g_font_tried) return;
    g_font_tried = true;
    std::vector<uint8_t> f(64 * 1024);
    const int n = lb_rom_read_unwrapped("Font/EN/font.NFTR", f.data(), (int)f.size());
    if (n < 0x30 || n > (int)f.size() || std::memcmp(f.data(), "RTFN", 4) != 0) {
        std::fprintf(stderr, "[lb_matchpic] Font/EN/font.NFTR unreadable (%d); using "
                     "the built-in 5x7 font\n", n);
        return;
    }
    f.resize((size_t)n);
    Font ft;
    const unsigned blocks = u16(f.data() + 0x0E);
    size_t at = u16(f.data() + 0x0C);
    bool have_glyphs = false;
    for (unsigned b = 0; b < blocks && at + 8 <= f.size(); ++b) {
        const uint8_t* p = f.data() + at;
        const uint32_t size = u32(p + 4);
        if (size < 8 || at + size > f.size()) break;
        const uint8_t* d = p + 8;
        if (!std::memcmp(p, "FNIF", 4)) {
            ft.line = d[1];
            ft.def = {(int)(int8_t)d[4], d[5], d[6]};
        } else if (!std::memcmp(p, "PLGC", 4)) {
            ft.tw = d[0]; ft.th = d[1]; ft.tsize = u16(d + 2); ft.bpp = d[6];
            ft.data.assign(d + 8, p + size);
            have_glyphs = ft.tw > 0 && ft.th > 0 && ft.tsize > 0 &&
                          (ft.bpp == 1 || ft.bpp == 2);
        } else if (!std::memcmp(p, "HDWC", 4)) {
            const unsigned first = u16(d), last = u16(d + 2);
            if (last >= first) {
                if (ft.widths.size() < (size_t)last + 1) ft.widths.resize((size_t)last + 1, ft.def);
                for (unsigned i = first; i <= last && 8u + 3u * (i - first) + 3u <= size - 8u; ++i)
                    ft.widths[i] = {(int)(int8_t)d[8 + 3 * (i - first)],
                                    d[9 + 3 * (i - first)], d[10 + 3 * (i - first)]};
            }
        } else if (!std::memcmp(p, "PAMC", 4)) {
            const unsigned first = u16(d), last = u16(d + 2), type = u16(d + 4);
            const uint8_t* e = d + 12;
            if (type == 0) {
                const unsigned base = u16(e);
                for (unsigned c = first; c <= last; ++c) ft.cmap[c] = (int)(base + c - first);
            } else if (type == 1) {
                for (unsigned c = first; c <= last; ++c) {
                    const unsigned g = u16(e + 2 * (c - first));
                    if (g != 0xFFFF) ft.cmap[c] = (int)g;
                }
            } else if (type == 2) {
                const unsigned cnt = u16(e);
                for (unsigned i = 0; i < cnt; ++i)
                    ft.cmap[u16(e + 2 + 4 * i)] = (int)u16(e + 4 + 4 * i);
            }
        }
        at += size;
    }
    if (!have_glyphs || ft.cmap.empty()) {
        std::fprintf(stderr, "[lb_matchpic] Font/EN/font.NFTR parsed no glyphs; using "
                     "the built-in 5x7 font\n");
        return;
    }
    ft.ok = true;
    g_font = ft;
    std::fprintf(stderr, "[lb_matchpic] UI font Font/EN/font.NFTR: %dx%d tiles, %d bpp, "
                 "%zu glyph(s) mapped\n", ft.tw, ft.th, ft.bpp, ft.cmap.size());
}

// The built-in fallback: the classic 5x7 font, 0x20..0x7E, 5 columns, bit 0 = top.
const uint8_t k5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},
    {0x00,0x40,0x34,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},{0x3E,0x41,0x5D,0x59,0x4E},
    {0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x41,0x51,0x73},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32},{0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x03,0x07,0x08,0x00},{0x20,0x54,0x54,0x78,0x40},
    {0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},{0x38,0x44,0x44,0x28,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x00,0x08,0x7E,0x09,0x02},{0x18,0xA4,0xA4,0x9C,0x78},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x40,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0xFC,0x18,0x24,0x24,0x18},
    {0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},
    {0x04,0x04,0x3F,0x44,0x24},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x4C,0x90,0x90,0x90,0x7C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x77,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02}};

// One character as UTF-8 -> code point; advances `s`.
uint32_t next_cp(const char*& s) {
    const unsigned char c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && ((unsigned char)*s & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)*s++ & 0x3F);
    return cp;
}

int glyph_index(uint32_t cp) {
    auto it = g_font.cmap.find(cp);
    return it == g_font.cmap.end() ? -1 : it->second;
}
int advance(uint32_t cp) {
    if (g_font.ok) {
        const int g = glyph_index(cp);
        if (g < 0 || (size_t)g >= g_font.widths.size()) return g_font.def.cw;
        return g_font.widths[(size_t)g].cw;
    }
    return 6;
}
int text_width(const char* s) {
    int w = 0;
    while (*s) w += advance(next_cp(s));
    return w;
}
int text_height() { return g_font.ok ? g_font.line : 7; }

void draw_glyph(Img& im, int x, int y, uint32_t cp, uint32_t col) {
    if (g_font.ok) {
        const int g = glyph_index(cp);
        if (g < 0 || (size_t)(g + 1) * (size_t)g_font.tsize > g_font.data.size()) return;
        const int left = (size_t)g < g_font.widths.size() ? g_font.widths[(size_t)g].left : 0;
        const uint8_t* t = g_font.data.data() + (size_t)g * (size_t)g_font.tsize;
        const int maxv = (1 << g_font.bpp) - 1;
        for (int py = 0; py < g_font.th; ++py)
            for (int px = 0; px < g_font.tw; ++px) {
                const int bit = (py * g_font.tw + px) * g_font.bpp;
                int v = 0;
                for (int k = 0; k < g_font.bpp; ++k)
                    v = (v << 1) | ((t[(bit + k) >> 3] >> (7 - ((bit + k) & 7))) & 1);
                if (v) blend(im, x + left + px, y + py, col, v * 255 / maxv);
            }
        return;
    }
    if (cp < 0x20 || cp > 0x7E) return;
    for (int cx = 0; cx < 5; ++cx)
        for (int cy = 0; cy < 7; ++cy)
            if ((k5x7[cp - 0x20][cx] >> cy) & 1) blend(im, x + cx, y + cy, col, 255);
}

// Text with a one-pixel shadow, clipped to maxw (0: no limit). Returns its width.
int draw_text(Img& im, int x, int y, const char* s, uint32_t col, int maxw = 0,
              bool bold = false) {
    int pen = 0;
    const uint32_t shadow = rgba(0, 0, 0);
    while (*s) {
        const uint32_t cp = next_cp(s);
        const int adv = advance(cp);
        if (maxw && pen + adv > maxw) break;
        draw_glyph(im, x + pen + 1, y + 1, cp, shadow);
        if (bold) draw_glyph(im, x + pen + 1, y, cp, col);
        draw_glyph(im, x + pen, y, cp, col);
        pen += adv + (bold ? 1 : 0);
    }
    return pen;
}

// ---- the backdrop ---------------------------------------------------------------
void gradient(Img& im) {
    // The main menu background's blues: palette entries 2 (darkest), 44, 22.
    int top[3] = {0x0A, 0x2A, 0x6E}, bot[3] = {0x2F, 0x6F, 0xD6};
    std::vector<uint8_t> n(2048);
    const int sz = lb_rom_read_unwrapped("UI/AllInOne/Iconic_MainMenu_BG.NCLR", n.data(), (int)n.size());
    bool got = false;
    if (sz > 0x30 && sz <= (int)n.size()) {
        for (int i = 0x10; i + 24 <= sz; ++i)
            if (!std::memcmp(n.data() + i, "TTLP", 4)) {
                const uint8_t* pal = n.data() + i + 8 + u32(n.data() + i + 20);
                const int cnt = (int)u32(n.data() + i + 16) / 2;
                if (cnt >= 45 && pal + 90 <= n.data() + sz) {
                    auto col = [&](int k, int* o) {
                        const unsigned c = u16(pal + 2 * k);
                        o[0] = (int)(c & 31) * 255 / 31;
                        o[1] = (int)((c >> 5) & 31) * 255 / 31;
                        o[2] = (int)((c >> 10) & 31) * 255 / 31;
                    };
                    col(2, top);
                    col(22, bot);
                    got = true;
                }
                break;
            }
    }
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[lb_matchpic] backdrop gradient %s: (%d,%d,%d) -> "
                     "(%d,%d,%d)\n", got ? "from Iconic_MainMenu_BG.NCLR" :
                     "fallback #0A2A6E -> #2F6FD6", top[0], top[1], top[2], bot[0],
                     bot[1], bot[2]);
    }
    for (int y = 0; y < im.h; ++y) {
        const int t = y * 255 / (im.h > 1 ? im.h - 1 : 1);
        const uint32_t c = rgba(top[0] + (bot[0] - top[0]) * t / 255,
                                top[1] + (bot[1] - top[1]) * t / 255,
                                top[2] + (bot[2] - top[2]) * t / 255);
        for (int x = 0; x < im.w; ++x) im.px[(size_t)y * im.w + x] = c;
    }
}

// ---- the match ------------------------------------------------------------------
struct Player {
    bool have = false;
    int bp[9] = {};
    std::string name;
    uint32_t colour = 0;   // BR-0008: the lobby's team colour (0: not known)
};
constexpr int kMaxPlayers = 8;

// BR-0008: a player's team colour. The lobby record (0x0215B814 + 0x2C per
// station) keeps the assigned colour in bits 2-4 of byte +0x20 (docs/412): 0 red,
// 1 blue, 2 green, 3 yellow, 4 purple, 5 grey. The game's own sprite table
// (0x02127F38, 6 colours x 3 shades, BGR555) gives the colour; shade 0 is the
// light one, eased toward white so a name reads on the blue backdrop.
uint32_t bgr555_to_rgba(unsigned c);
uint32_t team_colour_rgba(unsigned c);
uint32_t lobby_colour(int player) {
    if (player < 0 || player >= 3) return 0;
    return team_colour_rgba((bus_read_u8(0x0215B814u + 0x2Cu * (uint32_t)player + 0x20u) >> 2) & 7u);
}
uint32_t team_colour_rgba(unsigned c) {
    if (c > 5u) return 0;
    const uint32_t v = bgr555_to_rgba(bus_read_u16(0x02127F38u + 6u * c));
    const int r = (int)(v & 255u), g = (int)((v >> 8) & 255u), b = (int)((v >> 16) & 255u);
    return rgba(r + (255 - r) / 4, g + (255 - g) / 4, b + (255 - b) / 4);
}
Player g_players[kMaxPlayers];
bool g_posted = false;   // this match's picture has gone

// The army record's offsets, in the picture's order: hero, builder, melee, ranged,
// mounted, the three siege units, transport (lb_teams' slot order).
const int kRecOff[9] = {0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x00, 0x02, 0x04, 0x06};

const uint32_t kTeamColour[kMaxPlayers] = {
    rgba(255, 96, 96), rgba(110, 170, 255), rgba(110, 220, 120), rgba(255, 220, 80),
    rgba(230, 130, 255), rgba(90, 230, 230), rgba(255, 160, 70), rgba(220, 220, 220)};

struct Match {
    std::vector<Player> players;
    std::string map_name, map_stem;
    std::vector<int> banned;
};

// The map's Free Play preview: <stem>mini.NCGR, 4 bpp, 8x8 tiles in a 16-tile row.
// Index 0 is clear. Its palette is not a file of its own (the map select gets it
// at run time), so a terrain ramp is used.
bool decode_preview(const std::vector<uint8_t>& f, int n, std::vector<uint8_t>* idx,
                    int* size) {
    if (n < 0x30 + 0x2000 || n > (int)f.size() || std::memcmp(f.data(), "RGCN", 4) != 0)
        return false;
    const uint8_t* pix = f.data() + 0x30;
    const int tilesW = u16(f.data() + 0x1A);   // 0x10
    if (tilesW != 16) return false;
    idx->assign(128 * 128, 0);
    int maxx = 0, maxy = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int t = (y >> 3) * 16 + (x >> 3);
            const uint8_t b = pix[t * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
            const int v = (x & 1) ? (b >> 4) : (b & 15);
            (*idx)[(size_t)y * 128 + x] = (uint8_t)v;
            if (v) { maxx = std::max(maxx, x + 1); maxy = std::max(maxy, y + 1); }
        }
    *size = std::max(maxx, maxy);
    return *size > 0;
}
bool load_preview(const std::string& stem, std::vector<uint8_t>* idx, int* size) {
    std::vector<uint8_t> f(32 * 1024);
    const int n = lb_rom_read_unwrapped((stem + "mini.NCGR").c_str(), f.data(), (int)f.size());
    return decode_preview(f, n, idx, size);
}

// SG-0004 (docs/430): the map select's real palette. ARM9 table 0x020C4A54 is
// indexed by world (0 King, 1 Pirate, 2 Mars) and points at a 16-entry BGR555
// bank; the framework presents a 5-bit channel c as the 6-bit v = 2c + (c > 0),
// then (v << 2) | (v >> 4).
int widen5(unsigned c) {
    const unsigned v = c * 2u + (c > 0u ? 1u : 0u);
    return (int)(((v << 2) | (v >> 4)) & 255u);
}
uint32_t bgr555_to_rgba(unsigned c) {
    return rgba(widen5(c & 31u), widen5((c >> 5) & 31u), widen5((c >> 10) & 31u));
}

// The map's world: a stock "mpNN" stem by the mission table (28-byte entries at
// 0x02147DDC, the world in the field at +0x18); a mod map ("z/NN") by lb_mods.
int map_world(const std::string& stem) {
    if (stem.size() > 2 && stem[0] == 'z' && stem[1] == '/')
        return lb_mods_world(std::atoi(stem.c_str() + 2) - 1);
    for (uint32_t e = 0; e < 30u; ++e) {
        const uint32_t ent = 0x02147DDCu + 28u * e;
        const uint32_t s = bus_read_u32(ent);
        if (s < 0x02000000u || s >= 0x02400000u) continue;
        bool same = true;
        for (size_t i = 0; i <= stem.size(); ++i)
            if ((char)bus_read_u8(s + (uint32_t)i) != (i < stem.size() ? stem[i] : '\0')) {
                same = false;
                break;
            }
        if (same) return (int)bus_read_u32(ent + 0x18u);
    }
    return 0;
}

bool world_palette(int world, uint32_t out[16]) {
    if (world < 0 || world > 2) world = 0;
    const uint32_t bank = bus_read_u32(0x020C4A54u + 4u * (uint32_t)world);
    if (bank < 0x02000000u || bank >= 0x02400000u) return false;
    bool any = false;
    for (int i = 0; i < 16; ++i) {
        const unsigned c = bus_read_u16(bank + 2u * (uint32_t)i);
        out[i] = i ? bgr555_to_rgba(c) : 0u;
        any = any || c != 0u;
    }
    return any;
}

void draw_preview(Img& im, int x, int y, int w, const std::vector<uint8_t>& idx, int size,
                  int world) {
    static const uint32_t kRamp[16] = {
        0, rgba(52, 70, 46), rgba(30, 44, 34), rgba(78, 116, 72), rgba(104, 142, 88),
        rgba(136, 168, 112), rgba(64, 90, 150), rgba(214, 196, 120), rgba(180, 150, 90),
        rgba(160, 160, 160), rgba(200, 200, 200), rgba(255, 255, 255), rgba(200, 80, 80),
        rgba(80, 80, 200), rgba(80, 200, 80), rgba(230, 230, 60)};
    uint32_t pal[16];
    const bool real = world_palette(world, pal);
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[lb_matchpic] preview palette: %s (world %d)\n",
                     real ? "the map select's" : "fallback ramp", world);
    }
    const uint32_t* kPal = real ? pal : kRamp;
    const int s = std::max(1, std::min(3, (w - 8) / size));
    const int px = x + (w - size * s) / 2;
    fill_rect(im, px - 3, y - 3, size * s + 6, size * s + 6, rgba(8, 14, 30), 200);
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const int v = idx[(size_t)j * 128 + i];
            const uint32_t c = v ? kPal[v] : rgba(18, 30, 60);
            fill_rect(im, px + i * s, y + j * s, s, s, c, 255);
        }
}

void draw_icon(Img& im, int x, int y, int bp) {
    unsigned int icon[576];
    if (!lb_teams_hud_icon_rgba(bp, icon)) {
        fill_rect(im, x, y, 24, 24, rgba(0, 0, 0), 90);
        return;
    }
    fill_rect(im, x, y, 24, 24, rgba(0, 0, 0), 70);
    for (int j = 0; j < 24; ++j)
        for (int i = 0; i < 24; ++i) {
            const unsigned c = icon[j * 24 + i];
            if (c >> 24) blend(im, x + i, y + j, c, 255);
        }
}

Img render(const Match& m) {
    font_load();
    const int N = (int)m.players.size();
    const int W = 640;
    const int kHead = 24, kPitch = 25;
    const int rows_bottom = kHead + 9 * kPitch;
    const int H = rows_bottom + (m.banned.empty() ? 6 : 36);
    Img im;
    im.w = W; im.h = H;
    im.px.assign((size_t)W * H, 0);
    gradient(im);

    const int mapW = N <= 2 ? 240 : N == 3 ? 176 : 150;
    const int colW = (W - mapW) / std::max(N, 1);
    int mapX = (W - mapW) / 2;
    std::vector<int> colX;
    if (N <= 2) {
        colX.push_back(0);
        if (N == 2) colX.push_back(W - colW);
    } else {
        for (int i = 0; i < N; ++i) colX.push_back(i * colW);
        mapX = N * colW;
    }
    for (int i = 0; i < N; ++i) {
        const Player& p = m.players[(size_t)i];
        const int x0 = colX[(size_t)i];
        fill_rect(im, x0 + 4, 4, colW - 8, rows_bottom - 6, rgba(0, 0, 0), 60);
        const uint32_t col = p.colour ? p.colour : kTeamColour[i % kMaxPlayers];
        draw_text(im, x0 + 10, 7, p.name.c_str(), col, colW - 20, true);
        for (int k = 0; k < 9; ++k) {
            const int y = kHead + k * kPitch;
            draw_icon(im, x0 + 8, y, p.bp[k]);
            draw_text(im, x0 + 8 + 28, y + (24 - text_height()) / 2,
                      lb_teams_unit_name(p.bp[k]), rgba(255, 255, 255), colW - 8 - 32 - 4);
        }
    }
    // The map.
    fill_rect(im, mapX + 4, 4, mapW - 8, rows_bottom - 6, rgba(0, 0, 0), 60);
    std::vector<uint8_t> idx;
    int size = 0;
    int py = kHead + 4;
    if (!m.map_stem.empty() && load_preview(m.map_stem, &idx, &size)) {
        draw_preview(im, mapX + 4, py, mapW - 8, idx, size, map_world(m.map_stem));
        const int s = std::max(1, std::min(3, (mapW - 16) / size));
        py += size * s + 12;
    } else {
        std::fprintf(stderr, "[lb_matchpic] no preview for \"%s\"\n", m.map_stem.c_str());
        py += 40;
    }
    {
        const int w = text_width(m.map_name.c_str());
        const int tw = std::min(w, mapW - 16);
        draw_text(im, mapX + (mapW - tw) / 2, py, m.map_name.c_str(), rgba(255, 240, 150), mapW - 16, true);
    }
    // The banned strip.
    if (!m.banned.empty()) {
        const int y = rows_bottom + 4;
        fill_rect(im, 4, y - 2, W - 8, 30, rgba(0, 0, 0), 60);
        const int lw = draw_text(im, 12, y + (24 - text_height()) / 2, "Banned:",
                                 rgba(255, 150, 150), 0, true);
        int x = 12 + lw + 10;
        int shown = 0;
        for (int bp : m.banned) {
            if (x + 24 > W - 40 && shown + 1 < (int)m.banned.size()) {
                char more[24];
                std::snprintf(more, sizeof more, "+%d", (int)m.banned.size() - shown);
                draw_text(im, x, y + (24 - text_height()) / 2, more, rgba(255, 255, 255));
                break;
            }
            draw_icon(im, x, y, bp);
            x += 26;
            ++shown;
        }
    }
    return im;
}

// ---- PNG ------------------------------------------------------------------------
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
bool encode_png(const Img& im, std::vector<uint8_t>* out) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)im.h * (1 + (size_t)im.w * 4));
    for (int y = 0; y < im.h; ++y) {
        raw.push_back(0);   // filter: none
        for (int x = 0; x < im.w; ++x) {
            const uint32_t c = im.px[(size_t)y * im.w + x];
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
    put32(ihdr, (uint32_t)im.w); put32(ihdr, (uint32_t)im.h);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(*out, "IHDR", ihdr);
    chunk(*out, "IDAT", z);
    chunk(*out, "IEND", {});
    return true;
}

bool build_png(const Match& m, std::vector<uint8_t>* png) {
    const Img im = render(m);
    if (!encode_png(im, png)) return false;
    if (png->size() > 400u * 1024u) {
        std::fprintf(stderr, "[lb_matchpic] the picture is %zu bytes (limit 400 KB)\n", png->size());
        return false;
    }
    if (const char* path = std::getenv("LB_MATCHPIC_FILE")) {
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(png->data(), 1, png->size(), f);
            std::fclose(f);
            std::fprintf(stderr, "[lb_matchpic] wrote %s (%zu bytes, %dx%d)\n", path,
                         png->size(), im.w, im.h);
        }
    }
    std::fflush(stderr);
    return true;
}

std::vector<int> banned_list() {
    unsigned char bits[24];
    std::vector<int> out;
    if (!lb_teams_local_bans(bits, (int)sizeof bits)) return out;
    unsigned int icon[576];
    for (int bp = 0; bp < 182; ++bp)
        if (((bits[bp >> 3] >> (bp & 7)) & 1) && !lb_teams_is_king_default(bp) &&
            lb_teams_hud_icon_rgba(bp, icon))
            out.push_back(bp);
    return out;
}

}  // namespace

// docs/431: the map browser's thumbnail. A map's <stem>mini.NCGR (plain or
// COMP-wrapped) drawn with the map select's own palette for its world; 128x128
// pixels in `out` (R,G,B,A in memory order; index 0 is the dark backdrop),
// `*size` the used square. Returns 0 when the file is not a Free Play preview.
bool lb_mods_unwrap(std::vector<uint8_t>* b);   // lb_mods.cpp
extern "C" int lb_matchpic_mini_rgba(const unsigned char* ncgr, size_t n, int world,
                                     unsigned int* out, int* size) {
    std::vector<uint8_t> f(ncgr, ncgr + n);
    if (!lb_mods_unwrap(&f)) return 0;
    std::vector<uint8_t> idx;
    if (!decode_preview(f, (int)f.size(), &idx, size)) return 0;
    uint32_t pal[16];
    if (!world_palette(world, pal)) {
        // Before the game's memory holds the bank: a plain terrain ramp.
        static const uint32_t kRamp[16] = {
            0, rgba(52, 70, 46), rgba(30, 44, 34), rgba(78, 116, 72), rgba(104, 142, 88),
            rgba(136, 168, 112), rgba(64, 90, 150), rgba(214, 196, 120), rgba(180, 150, 90),
            rgba(160, 160, 160), rgba(200, 200, 200), rgba(255, 255, 255), rgba(200, 80, 80),
            rgba(80, 80, 200), rgba(80, 200, 80), rgba(230, 230, 60)};
        std::memcpy(pal, kRamp, sizeof pal);
    }
    for (int i = 0; i < 128 * 128; ++i) {
        const int v = idx[(size_t)i];
        out[i] = v ? pal[v] : rgba(18, 30, 60);
    }
    return 1;
}

// docs/429: THE RANDOM ENTRY DOES NOT SHOW ITS DRAW. The map
// select uploads the selected map's preview (`<stem>mini`, a 128x128 4bpp
// bitmap, 8192 bytes) through FUN_0204E1D0; while a Random entry is selected, the
// Random opponent's "?" is drawn into that buffer instead, scaled to the drawn
// map's preview size and matched to its world's map-select palette (index 0
// stays clear). Returns 1 when painted.
extern "C" int lb_teams_question_rgba(unsigned int out[56 * 56]);   /* lb_teams.cpp */
extern "C" int lb_matchpic_paint_random_preview2(uint32_t buf, uint32_t n, const char* name,
                                                 int tiled);
extern "C" int lb_matchpic_paint_random_preview(uint32_t buf, uint32_t n, const char* name) {
    if (!name) return 0;
    std::string s = name;
    if (s.size() < 5 || s.compare(s.size() - 4, 4, "mini") != 0) return 0;
    return lb_matchpic_paint_random_preview2(buf, n, name, 0);
}

// `name` "<stem>mini" gives the world's palette (the map select's upload, a
// bitmap); without one (the top screen's tiled copy, which arrives with no
// name) white and black are entries 14 and 11, the same in all three banks.
// `tiled`: the .NCGR layout (8x8 tiles of 32 bytes, 16 to a row) instead of
// the bitmap (64 bytes a row); its clear value is its own corner pixel (that
// copy has no transparent index).
extern "C" int lb_matchpic_paint_random_preview2(uint32_t buf, uint32_t n, const char* name,
                                                 int tiled) {
    if (n != 8192u || buf < 0x02000000u || buf + n > 0x02400000u) return 0;
    static unsigned int q[56 * 56];
    if (!lb_teams_question_rgba(q)) return 0;
    uint32_t pal[16];
    bool have_pal = false;
    if (name) {
        std::string stem = name;
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        have_pal = world_palette(map_world(stem), pal);
    }
    auto addr = [&](int px, int py) {
        return tiled ? buf + (uint32_t)(((py >> 3) * 16 + (px >> 3)) * 32 + (py & 7) * 4 +
                                        ((px & 7) >> 1))
                     : buf + (uint32_t)(py * 64 + (px >> 1));
    };
    auto pixel = [&](int px, int py) {
        const uint8_t b = bus_read_u8(addr(px, py));
        return (px & 1) ? (b >> 4) : (b & 15);
    };
    // The bitmap's index 0 is clear; the tiled copy draws every index (its 0
    // is magenta), so its square is black and its "?" white (below).
    const int clear = tiled ? 11 : 0;
    // The drawn map's extent, from its own preview: a 64- or 96-tile map.
    int size = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            if (pixel(x, y) != 0) size = std::max(size, std::max(x + 1, y + 1));
    if (size < 32) size = 64;
    if (size > 128) size = 128;
    auto nearest = [&](unsigned int c) {
        if (!have_pal) return (c & 0xFFFFFFu) ? 14 : 11;
        const int r = (int)(c & 255u), g = (int)((c >> 8) & 255u), b = (int)((c >> 16) & 255u);
        int best = 1, bd = 1 << 30;
        for (int k = 1; k < 16; ++k) {
            const int dr = r - (int)(pal[k] & 255u), dg = g - (int)((pal[k] >> 8) & 255u),
                      db = b - (int)((pal[k] >> 16) & 255u);
            const int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = k; }
        }
        return best;
    };
    // THE GLYPH ONLY (no tinted tile, no gold border). The cell is
    // a dark navy "?" (000828) on a blue tile inside a gold frame: the glyph
    // is the dark pixels of the tile's interior (the frame and its shadow are
    // the outer 8 pixels). It is drawn black with a white outline around it,
    // both of which every world palette has, so it looks the same on any map,
    // and the rest is clear (index 0) over the preview frame's blue.
    auto lum = [](unsigned int c) {
        return (int)((c & 255u) * 3u + ((c >> 8) & 255u) * 6u + ((c >> 16) & 255u)) / 10;
    };
    static uint8_t glyph[56 * 56];   // 0 clear, 1 white outline, 2 black body
    for (int y = 0; y < 56; ++y)
        for (int x = 0; x < 56; ++x) {
            const unsigned int c = q[(size_t)y * 56u + (size_t)x];
            glyph[y * 56 + x] = (x >= 8 && x < 50 && y >= 6 && y < 50 && (c >> 24) &&
                                 lum(c) < 40)
                                    ? 2
                                    : 0;
        }
    for (int y = 0; y < 56; ++y)
        for (int x = 0; x < 56; ++x) {
            if (glyph[y * 56 + x]) continue;
            bool near = false;
            for (int dy = -2; dy <= 2 && !near; ++dy)
                for (int dx = -2; dx <= 2 && !near; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    near = xx >= 0 && yy >= 0 && xx < 56 && yy < 56 && glyph[yy * 56 + xx] == 2;
                }
            if (near) glyph[y * 56 + x] = 1;
        }
    const int white = nearest(0xFFFFFFFFu), black = nearest(0xFF000000u);
    // The "?" fills most of the map's square, centred.
    const int box = size * 7 / 8, off = (size - box) / 2;
    const uint8_t fill = (uint8_t)(clear | (clear << 4));
    for (uint32_t i = 0; i < n; ++i) bus_write_u8(buf + i, fill);
    for (int y = 0; y < box; ++y)
        for (int x = 0; x < box; ++x) {
            const uint8_t gk = glyph[(size_t)(y * 56 / box) * 56u + (size_t)(x * 56 / box)];
            if (!gk) continue;
            const int px = off + x, py = off + y;
            const uint32_t at = addr(px, py);
            uint8_t b = bus_read_u8(at);
            // On the tiled copy's black square the body is white, its outline black.
            const int v = tiled ? (gk == 2 ? white : black) : (gk == 1 ? white : black);
            b = (px & 1) ? (uint8_t)((b & 0x0Fu) | (v << 4)) : (uint8_t)((b & 0xF0u) | v);
            bus_write_u8(at, b);
        }
    return 1;
}

extern "C" void lb_matchpic_note_player(int player, const unsigned char rec[20]) {
    if (player < 0 || player >= kMaxPlayers) return;
    if (g_players[player].have) {   // a player seen again: a new match
        for (Player& p : g_players) p = Player();
        g_posted = false;
    }
    Player& p = g_players[player];
    p.have = true;
    char name[64] = "";
    if (player == 0) lb_net_profile_name(name, (int)sizeof name);
    // The others: the lobby's own player records (0x0215B814 + 0x2C per
    // station, the name first, 11 bytes; the hero at +0x14 is what
    // lb_net_local_hero reads), already through the swear filter.
    if (!name[0] && player < 3) {
        const uint32_t rec_at = 0x0215B814u + 0x2Cu * (uint32_t)player;
        int n = 0;
        bool ok = true;
        for (; n < 11; ++n) {
            const uint8_t c = bus_read_u8(rec_at + (uint32_t)n);
            if (!c) break;
            if (c < 0x20 || c > 0x7E) { ok = false; break; }
            name[n] = (char)c;
        }
        name[ok ? n : 0] = '\0';
    }
    p.colour = lobby_colour(player);
    p.name = name[0] ? name : ("Player " + std::to_string(player + 1));
    std::string ids;
    for (int k = 0; k < 9; ++k) {
        p.bp[k] = rec[kRecOff[k]] | (rec[kRecOff[k] + 1] << 8);
        ids += (k ? " " : "") + std::to_string(p.bp[k]);
    }
    std::fprintf(stderr, "[lb_matchpic] player %d %s: %s\n", player, p.name.c_str(), ids.c_str());
    std::fflush(stderr);
}

// ---- SG-0019: who won ------------------------------------------------------------
// The game keeps every team in a pool (pointer at 0x0215540C -> object with the
// team pointers' array at +8 and the count at +0xC). A team's state is the word
// at +0x9C: 0 playing, 1 defeated, 2 won, 3 lost (FUN_02086770, docs/414). While
// the host's match is on, the teams are read every frame and each team's last
// non-zero state is kept (the pool is gone by the time the lobby closes). Team k
// is player k (verified against the roster's order in the test runs, docs/430).
// Read only. lb_net sends the result with the end-of-match /remove.
int g_team_state[kMaxPlayers];
bool g_watch = false;       // a host's match is on
bool g_result_said = false;

void watch_teams() {
    const uint32_t pool = bus_read_u32(0x0215540Cu);
    if (pool < 0x02000000u || pool >= 0x02400000u) return;
    const uint32_t arr = bus_read_u32(pool + 8u);
    const uint32_t cnt = bus_read_u32(pool + 0xCu);
    if (arr < 0x02000000u || arr >= 0x02400000u || cnt == 0u || cnt > 8u) return;
    static uint32_t said_cnt = 0;
    if (said_cnt != cnt) {   // once: the teams and their owner bytes, to check the order
        said_cnt = cnt;
        for (uint32_t k = 0; k < cnt; ++k) {
            const uint32_t t = bus_read_u32(arr + 4u * k);
            if (t >= 0x02000000u && t < 0x02400000u)
                std::fprintf(stderr, "[lb_matchpic] team %u: owner byte %02X, state %u\n", k,
                             (unsigned)bus_read_u8(t + 0x14u), (unsigned)bus_read_u32(t + 0x9Cu));
        }
        std::fflush(stderr);
    }
    for (uint32_t k = 0; k < cnt; ++k) {
        const uint32_t t = bus_read_u32(arr + 4u * k);
        if (t < 0x02000000u || t >= 0x02400000u) continue;
        const uint32_t st = bus_read_u32(t + 0x9Cu);
        if (st == 0u || st > 4u || g_team_state[k] == (int)st) continue;
        g_team_state[k] = (int)st;
        std::fprintf(stderr, "[lb_matchpic] team %u (owner byte %02X) is now state %u\n",
                     k, (unsigned)bus_read_u8(t + 0x14u), st);
        std::fflush(stderr);
    }
}

// Test aid: LB_MATCHPIC_FAKE_RESULT=1 -- when the game decided nothing, the
// first player is the winner and the others lost (exercises the wire path).
static void fake_result_if_asked() {
    if (!std::getenv("LB_MATCHPIC_FAKE_RESULT")) return;
    bool any = false;
    for (int s : g_team_state) any = any || s != 0;
    if (!any)
        for (int k = 0; k < kMaxPlayers; ++k) g_team_state[k] = k == 0 ? 2 : 1;
}

extern "C" void lb_matchpic_on_playing(void) {
    for (int& s : g_team_state) s = 0;
    g_watch = true;
    g_result_said = false;
    if (!lb_setting_match_picture() || g_posted) return;
    Match m;
    for (const Player& p : g_players)
        if (p.have) m.players.push_back(p);
    if (m.players.empty()) {
        std::fprintf(stderr, "[lb_matchpic] no roster was captured; no picture\n");
        return;
    }
    g_posted = true;
    m.map_name = lb_level_map_name();
    m.map_stem = lb_level_map_stem();
    m.banned = banned_list();
    std::vector<uint8_t> png;
    if (!build_png(m, &png)) return;
    lb_net_post_match_image(png.data(), png.size(), m.map_name.c_str());
}

// The result for lb_net's end-of-match /remove: player names, one per line, in
// two lists. Returns 1 when the game had decided anything. The match is over for
// the watcher afterwards.
extern "C" int lb_matchpic_result(char* winners, int wcap, char* losers, int lcap) {
    if (wcap > 0) winners[0] = '\0';
    if (lcap > 0) losers[0] = '\0';
    const bool watched = g_watch;
    g_watch = false;
    if (!watched) return 0;
    fake_result_if_asked();
    std::string w, l;
    for (int k = 0; k < kMaxPlayers; ++k) {
        if (!g_players[k].have || g_team_state[k] == 0) continue;
        std::string& dst = g_team_state[k] == 2 ? w : l;
        if (g_team_state[k] == 4) continue;   // allied win in a game with no online flag
        dst += g_players[k].name + "\n";
    }
    std::snprintf(winners, (size_t)wcap, "%s", w.c_str());
    std::snprintf(losers, (size_t)lcap, "%s", l.c_str());
    std::fprintf(stderr, "[lb_matchpic] result: winners [%s] losers [%s]\n",
                 w.empty() ? "" : w.substr(0, w.size() - 1).c_str(),
                 l.empty() ? "" : l.substr(0, l.size() - 1).c_str());
    return !w.empty() || !l.empty();
}

// SG-0018: the host's report of its match for the score tables, as the JSON
// fields of POST /score after the code and token: the ruleset (the balance in
// force), the cheats, the banned units and each player's line (won, hero, the
// stats lb_scores.cpp read). Call it before lb_matchpic_result, which ends the
// watch. Returns 0 when there is nothing to report: no match was watched, the
// game decided nobody, or cheats were on (the server refuses those anyway).
extern "C" int lb_matchpic_score_json(char* out, int cap) {
    if (cap > 0) out[0] = '\0';
    if (!g_watch) return 0;
    fake_result_if_asked();
    const unsigned cheats = (unsigned)lb_cheat_mask_live();
    if (cheats) {
        std::fprintf(stderr, "[lb_scores] cheats were on: the match is not reported\n");
        std::fflush(stderr);
        return 0;
    }
    std::string ruleset = "Vanilla";
    const int slot = lb_balance_applied_slot();
    if (slot > 0) ruleset = lb_balance_name(slot);
    if (lb_balance_custom_size() > 0) ruleset = "Custom Rules";
    auto esc = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if ((unsigned char)c >= 0x20) o += c;
        }
        return o + "\"";
    };
    std::string j = "\"ruleset\":" + esc(ruleset) + ",\"cheats\":0,\"banned\":[";
    const std::vector<int> banned = banned_list();
    for (size_t i = 0; i < banned.size(); ++i) j += (i ? "," : "") + std::to_string(banned[i]);
    j += "],\"players\":[";
    int n = 0;
    for (int k = 0; k < kMaxPlayers; ++k) {
        const int st = g_team_state[k];
        if (!g_players[k].have || (st != 1 && st != 2 && st != 3)) continue;
        LbMatchStats s{};
        if (!lb_scores_player_stats(k, &s)) {
            s.units_built = s.units_lost = s.units_killed = -1;
            s.buildings_built = s.buildings_lost = s.buildings_killed = s.bricks = -1;
        }
        char line[400];
        std::snprintf(line, sizeof line,
                      "{\"name\":%s,\"won\":%s,\"hero\":%d,\"units_built\":%d,"
                      "\"units_lost\":%d,\"units_killed\":%d,\"buildings_built\":%d,"
                      "\"buildings_lost\":%d,\"buildings_killed\":%d,\"bricks\":%d}",
                      esc(g_players[k].name).c_str(), st == 2 ? "true" : "false",
                      g_players[k].bp[0], s.units_built, s.units_lost, s.units_killed,
                      s.buildings_built, s.buildings_lost, s.buildings_killed, s.bricks);
        j += (n++ ? "," : "") + std::string(line);
    }
    j += "]";
    if (n < 2) return 0;
    if ((int)j.size() >= cap) return 0;
    std::memcpy(out, j.c_str(), j.size() + 1);
    std::fprintf(stderr, "[lb_scores] report: %s\n", out);
    std::fflush(stderr);
    return 1;
}

// LB_MATCHPIC_TEST=1: two teams of the King default army, map 1, once the front
// end has loaded the icons and the unit names.
extern "C" void lb_matchpic_tick(void) {
    static int state = 0;   // 0 untested, 1 waiting, 2 done
    static int frames = 0;
    if (g_watch) watch_teams();
    if (state == 2) return;
    if (state == 0) {
        const char* e = std::getenv("LB_MATCHPIC_TEST");
        state = (e && std::atoi(e)) ? 1 : 2;
        if (state == 2) return;
    }
    unsigned int icon[576];
    const char* nm = lb_teams_unit_name(0);
    if (++frames > 20000) { std::fprintf(stderr, "[lb_matchpic] test: gave up waiting\n"); state = 2; return; }
    if (!lb_teams_hud_icon_rgba(0, icon) || !std::strncmp(nm, "unit ", 5) || !std::strcmp(nm, "?"))
        return;
    static const int kKing[9] = {0, 2, 3, 4, 5, 6, 7, 8, 9};
    Match m;
    // LB_MATCHPIC_TEST_PLAYERS=2..4: that many teams (docs/428's 3-team layout).
    const char* tp = std::getenv("LB_MATCHPIC_TEST_PLAYERS");
    const int np = tp ? std::max(2, std::min(4, std::atoi(tp))) : 2;
    static const unsigned kTestColour[4] = {0u, 4u, 1u, 3u};   // red, purple, blue, yellow
    for (int i = 0; i < np; ++i) {
        Player p;
        p.have = true;
        p.name = "Tester" + std::to_string(i + 1);
        p.colour = team_colour_rgba(kTestColour[i]);
        for (int k = 0; k < 9; ++k) p.bp[k] = kKing[k] + (i ? 20 : 0) * 0;
        m.players.push_back(p);
    }
    // LB_MATCHPIC_TEST_MAP=mp02 picks the map (a Mars one, say).
    const char* tm = std::getenv("LB_MATCHPIC_TEST_MAP");
    m.map_stem = tm && *tm ? tm : "mp01";
    m.map_name = "Test Map (" + m.map_stem + ")";
    m.banned = {12, 13, 14, 31};
    std::vector<uint8_t> png;
    build_png(m, &png);
    state = 2;
}
