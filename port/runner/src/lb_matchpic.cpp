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
};
constexpr int kMaxPlayers = 8;
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
bool load_preview(const std::string& stem, std::vector<uint8_t>* idx, int* size) {
    std::vector<uint8_t> f(32 * 1024);
    const int n = lb_rom_read_unwrapped((stem + "mini.NCGR").c_str(), f.data(), (int)f.size());
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

void draw_preview(Img& im, int x, int y, int w, const std::vector<uint8_t>& idx, int size) {
    static const uint32_t kRamp[16] = {
        0, rgba(52, 70, 46), rgba(30, 44, 34), rgba(78, 116, 72), rgba(104, 142, 88),
        rgba(136, 168, 112), rgba(64, 90, 150), rgba(214, 196, 120), rgba(180, 150, 90),
        rgba(160, 160, 160), rgba(200, 200, 200), rgba(255, 255, 255), rgba(200, 80, 80),
        rgba(80, 80, 200), rgba(80, 200, 80), rgba(230, 230, 60)};
    const int s = std::max(1, std::min(3, (w - 8) / size));
    const int px = x + (w - size * s) / 2;
    fill_rect(im, px - 3, y - 3, size * s + 6, size * s + 6, rgba(8, 14, 30), 200);
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const int v = idx[(size_t)j * 128 + i];
            const uint32_t c = v ? kRamp[v] : rgba(18, 30, 60);
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
        const uint32_t col = kTeamColour[i % kMaxPlayers];
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
        draw_preview(im, mapX + 4, py, mapW - 8, idx, size);
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
    p.name = name[0] ? name : ("Player " + std::to_string(player + 1));
    std::string ids;
    for (int k = 0; k < 9; ++k) {
        p.bp[k] = rec[kRecOff[k]] | (rec[kRecOff[k] + 1] << 8);
        ids += (k ? " " : "") + std::to_string(p.bp[k]);
    }
    std::fprintf(stderr, "[lb_matchpic] player %d %s: %s\n", player, p.name.c_str(), ids.c_str());
    std::fflush(stderr);
}

extern "C" void lb_matchpic_on_playing(void) {
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

// LB_MATCHPIC_TEST=1: two teams of the King default army, map 1, once the front
// end has loaded the icons and the unit names.
extern "C" void lb_matchpic_tick(void) {
    static int state = 0;   // 0 untested, 1 waiting, 2 done
    static int frames = 0;
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
    for (int i = 0; i < 2; ++i) {
        Player p;
        p.have = true;
        p.name = i ? "Tester2" : "Tester1";
        for (int k = 0; k < 9; ++k) p.bp[k] = kKing[k] + (i ? 20 : 0) * 0;
        m.players.push_back(p);
    }
    m.map_stem = "mp01";
    m.map_name = lb_level_map_name()[0] ? lb_level_map_name() : "Test Map (mp01)";
    m.banned = {12, 13, 14, 31};
    std::vector<uint8_t> png;
    build_png(m, &png);
    state = 2;
}
