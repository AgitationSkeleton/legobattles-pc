// docs/493: THE LEGO BATTLES MENU STYLE.
//
// Display > Menu style "LEGO Battles" draws the port's ImGui panels in the
// game's own look, every piece cut AT RUN TIME from the player's cartridge
// (nothing of the game's art is in this source or the release):
//
//   * the font: Font/EN/font.NFTR, 10x13 cells, 1 bpp, 82 glyphs, drawn by a
//     custom ImFontLoader at whole multiples of its own size (crisp at UI
//     scale 1, 2, 3). Fifteen ASCII characters the game never needed
//     (" # $ & + ; = \ ^ _ ` { | } ~) are drawn here in its style (2-px
//     strokes, capitals on rows 1-9, descenders to row 12); anything else
//     falls back to ImGui's own font, merged after it.
//   * buttons and tabs: the body of the game's brick bar (UI/AllInOne/UI_FE,
//     palette bank 1, the "Main Menu" button): rows 215-231 light, 239-255
//     dark, 17 rows -- exactly an ImGui frame at FramePadding.y 2 and the
//     13-px font, so at every whole scale the art is drawn pixel for pixel.
//     No stud row and no picture frame: the full studded bars and the framed
//     backdrop were too large and too busy for a settings window (owner
//     review of the mockups).
//   * colours: the game's framed-panel navy (#001862), its border blue
//     (#294AB4) and its orange selection, sampled from FramedBG / the Iconic
//     tiles.
//
// Hooks into ImGui (a patched copy, apply_hooks.py "lb_imgui"): RenderFrame
// and TabItemBackground call ours first and draw a flat fill when we decline;
// ImFont::RenderText draws a one-pixel dark shadow under text, as the game
// does. Display only: nothing here reaches the guest, replays or peers.
#include "imgui.h"
#include "imgui_internal.h"

#include <SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);   // lb_teams.cpp

// The hooks the patched ImGui copy calls (defined there, NULL when unused).
extern bool (*GImLbFrameHook)(ImDrawList*, const ImVec2&, const ImVec2&, ImU32, float);
extern bool (*GImLbTabHook)(ImDrawList*, const ImVec2&, const ImVec2&, int, ImU32);
extern ImU32 GImLbTextShadowCol;

namespace {

// ---- the font ----------------------------------------------------------------
constexpr int kCellW = 10, kCellH = 13;   // the NFTR cell
constexpr int kAscent = 10;               // capitals sit on rows 1-9, the baseline under row 9
struct Glyph {
    bool ok = false;
    int left = 0, w = 0, adv = 0;          // bearing, ink width, advance (the cartridge's)
    uint8_t px[kCellH][kCellW] = {};
};
std::map<unsigned, Glyph> g_glyphs;
bool g_font_tried = false, g_font_ok = false;
int g_synth = 0;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The characters the cartridge's font lacks, in its own style. '#' is ink.
// Each: the character, its left bearing, then 13 rows of its cell.
struct Synth { char c; int left; const char* rows[kCellH]; };
const Synth kSynth[] = {
    {'"', 0, {"", "##.##", "##.##", ".#..#", "", "", "", "", "", "", "", "", ""}},
    {'#', 0, {"", ".##..##.", ".##..##.", "########", "########", ".##..##.", "########", "########",
              ".##..##.", ".##..##.", "", "", ""}},
    {'$', 0, {"..##..", ".####.", "##..##", "##....", ".####.", "....##", "....##", "##..##", ".####.",
              "..##..", "", "", ""}},
    {'&', 0, {"", ".###...", "##.##..", "##.##..", ".###...", "####.##", "##.###.", "##..##.", "######.",
              ".###.##", "", "", ""}},
    {'+', 0, {"", "", "", "..##..", "..##..", "######", "######", "..##..", "..##..", "", "", "", ""}},
    {';', 1, {"", "", "", "##", "##", "", "", "", "", "##", ".#", "##", ""}},
    {'=', 0, {"", "", "", "######", "######", "", "", "######", "######", "", "", "", ""}},
    {'\\', 0, {"", "##....", ".##...", ".##...", ".##...", "..##..", "..##..", "...##.", "...##.",
               "...##.", "....##", "", ""}},
    {'^', 0, {"", "..##..", ".####.", "##..##", "", "", "", "", "", "", "", "", ""}},
    {'_', 0, {"", "", "", "", "", "", "", "", "", "", "######", "######", ""}},
    {'`', 0, {"", "##.", ".##", "", "", "", "", "", "", "", "", "", ""}},
    {'{', 0, {"", "..##", ".##.", ".##.", ".##.", "##..", "##..", ".##.", ".##.", ".##.", "..##", "", ""}},
    {'}', 0, {"", "##..", ".##.", ".##.", ".##.", "..##", "..##", ".##.", ".##.", ".##.", "##..", "", ""}},
    {'|', 1, {"", "##", "##", "##", "##", "##", "##", "##", "##", "##", "##", "##", ""}},
    {'~', 0, {"", "", "", "", ".##....", "####.##", "##.####", "....##.", "", "", "", "", ""}},
};

void font_load() {
    if (g_font_tried) return;
    g_font_tried = true;
    std::vector<uint8_t> f(64 * 1024);
    const int n = lb_rom_read_unwrapped("Font/EN/font.NFTR", f.data(), (int)f.size());
    if (n < 0x30 || n > (int)f.size() || std::memcmp(f.data(), "RTFN", 4) != 0) {
        std::fprintf(stderr, "[lb_skin] Font/EN/font.NFTR unreadable (%d): the menu keeps its font\n", n);
        return;
    }
    f.resize((size_t)n);
    int tw = 0, th = 0, tsize = 0, bpp = 0;
    const uint8_t* gdata = nullptr;
    size_t gbytes = 0;
    struct W { int left, gw, adv; };
    std::vector<W> widths;
    std::map<unsigned, int> cmap;
    const unsigned blocks = rd16(f.data() + 0x0E);
    size_t at = rd16(f.data() + 0x0C);
    for (unsigned b = 0; b < blocks && at + 8 <= f.size(); ++b) {
        const uint8_t* p = f.data() + at;
        const uint32_t size = rd32(p + 4);
        if (size < 8 || at + size > f.size()) break;
        const uint8_t* d = p + 8;
        if (!std::memcmp(p, "PLGC", 4)) {
            tw = d[0]; th = d[1]; tsize = rd16(d + 2); bpp = d[6];
            gdata = d + 8;
            gbytes = size - 16;
        } else if (!std::memcmp(p, "HDWC", 4)) {
            const unsigned first = rd16(d), last = rd16(d + 2);
            for (unsigned i = first; i <= last && 8u + 3u * (i - first) + 3u <= size - 8u; ++i) {
                if (widths.size() <= i) widths.resize(i + 1, W{0, 0, 0});
                widths[i] = {(int)(int8_t)d[8 + 3 * (i - first)], d[9 + 3 * (i - first)],
                             d[10 + 3 * (i - first)]};
            }
        } else if (!std::memcmp(p, "PAMC", 4)) {
            const unsigned first = rd16(d), last = rd16(d + 2), type = rd16(d + 4);
            const uint8_t* e = d + 12;
            if (type == 0) {
                for (unsigned c = first; c <= last; ++c) cmap[c] = (int)(rd16(e) + c - first);
            } else if (type == 1) {
                for (unsigned c = first; c <= last; ++c) {
                    const unsigned g = rd16(e + 2 * (c - first));
                    if (g != 0xFFFF) cmap[c] = (int)g;
                }
            } else if (type == 2) {
                const unsigned cnt = rd16(e);
                for (unsigned i = 0; i < cnt; ++i) cmap[rd16(e + 2 + 4 * i)] = (int)rd16(e + 4 + 4 * i);
            }
        }
        at += size;
    }
    if (!gdata || tw != kCellW || th != kCellH || bpp != 1 || tsize <= 0) {
        std::fprintf(stderr, "[lb_skin] the font is not the expected 10x13 1-bpp one (%dx%d, %d bpp): "
                     "the menu keeps its font\n", tw, th, bpp);
        return;
    }
    for (const auto& [c, gi] : cmap) {
        if ((size_t)(gi + 1) * (size_t)tsize > gbytes || (size_t)gi >= widths.size()) continue;
        Glyph g;
        const uint8_t* src = gdata + (size_t)gi * (size_t)tsize;
        for (int y = 0; y < kCellH; ++y)
            for (int x = 0; x < kCellW; ++x) {
                const int bit = y * kCellW + x;
                g.px[y][x] = (src[bit >> 3] >> (7 - (bit & 7))) & 1u;
            }
        g.left = widths[(size_t)gi].left;
        g.w = widths[(size_t)gi].gw;
        g.adv = widths[(size_t)gi].adv;
        g.ok = true;
        g_glyphs[c] = g;
    }
    for (const Synth& s : kSynth) {
        if (g_glyphs.count((unsigned char)s.c)) continue;
        Glyph g;
        int w = 0;
        for (int y = 0; y < kCellH; ++y) {
            const char* r = s.rows[y];
            for (int x = 0; r[x] && x < kCellW; ++x) {
                g.px[y][x] = r[x] == '#';
                if (r[x] == '#' && x + 1 > w) w = x + 1;
            }
        }
        g.left = s.left;
        g.w = w;
        g.adv = w + s.left;
        g.ok = true;
        g_glyphs[(unsigned char)s.c] = g;
        ++g_synth;
    }
    g_font_ok = true;
    std::fprintf(stderr, "[lb_skin] font: %zu glyph(s) from the cartridge, %d drawn in its style\n",
                 g_glyphs.size() - (size_t)g_synth, g_synth);
}

// The whole multiple of the 13-px cell a baked size is drawn at.
int font_k(float size) {
    const int k = (int)std::floor(size / (float)kCellH + 0.01f);
    return k < 1 ? 1 : k;
}

// The ImFontLoader: the glyphs at k x k pixels each, the advance one pixel
// wider than the cartridge's table (its renderer leaves that gap; the table's
// advance is the ink width, so 'HH' would touch).
bool loader_contains(ImFontAtlas*, ImFontConfig*, ImWchar c) {
    const auto it = g_glyphs.find((unsigned)c);
    return it != g_glyphs.end() && it->second.ok;
}
bool loader_baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    if (!src->MergeMode) {
        const int k = font_k(baked->Size);
        baked->Ascent = (float)(kAscent * k);
        baked->Descent = -(float)((kCellH - kAscent) * k);
    }
    return true;
}
bool loader_load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar c,
                       ImFontGlyph* out_glyph, float* out_advance_x) {
    const auto it = g_glyphs.find((unsigned)c);
    if (it == g_glyphs.end() || !it->second.ok) return false;
    const Glyph& g = it->second;
    const int k = font_k(baked->Size);
    const float advance = (float)((g.adv + 1) * k);
    if (out_advance_x) {
        *out_advance_x = advance;
        return true;
    }
    out_glyph->Codepoint = c;
    out_glyph->AdvanceX = advance;
    // The ink's columns within the cell.
    int x0 = kCellW, x1 = 0;
    for (int y = 0; y < kCellH; ++y)
        for (int x = 0; x < kCellW; ++x)
            if (g.px[y][x]) { if (x < x0) x0 = x; if (x + 1 > x1) x1 = x + 1; }
    if (x1 <= x0) return true;   // a space: advance only
    const int w = (x1 - x0) * k, h = kCellH * k;
    const ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
    if (id == ImFontAtlasRectId_Invalid) return false;
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, id);
    std::vector<unsigned char> px((size_t)w * (size_t)h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px[(size_t)y * (size_t)w + (size_t)x] = g.px[y / k][x0 + x / k] ? 255 : 0;
    out_glyph->X0 = (float)((g.left + x0) * k);
    out_glyph->Y0 = 0.0f;
    out_glyph->X1 = out_glyph->X0 + (float)w;
    out_glyph->Y1 = (float)h;
    out_glyph->Visible = true;
    out_glyph->PackId = id;
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, px.data(), ImTextureFormat_Alpha8, w);
    return true;
}
ImFontLoader g_loader;
ImFont* g_font = nullptr;

// ---- the brick bar ------------------------------------------------------------
// From UI_FE (4 bpp, 256 px wide, 32 tiles a row) in palette bank 1: x 24..95,
// light rows 215..231, dark rows 239..255.
constexpr int kBarX = 24, kBarW = 72, kBarH = 17, kLightY = 215, kDarkY = 239;
constexpr int kCap = 8;                   // left and right ends kept whole
constexpr int kEdge = 3;                  // top and bottom rows kept whole
constexpr int kBars = 4;                  // blue light, blue dark, red light, red dark
SDL_Texture* g_tex = nullptr;
SDL_Renderer* g_tex_r = nullptr;
bool g_art_tried = false, g_art_ok = false;

bool art_build(SDL_Renderer* r) {
    if (g_art_ok && g_tex && g_tex_r == r) return true;
    if (g_art_tried && g_tex_r == r) return g_art_ok;
    g_art_tried = true;
    g_art_ok = false;
    std::vector<uint8_t> chr(128 * 1024), pal(4 * 1024);
    const int nc = lb_rom_read_unwrapped("UI/AllInOne/UI_FE.NCGR", chr.data(), (int)chr.size());
    const int np = lb_rom_read_unwrapped("UI/AllInOne/UI_FE.NCLR", pal.data(), (int)pal.size());
    auto section = [](const std::vector<uint8_t>& d, int n, const char* mag) -> const uint8_t* {
        if (n < 0x10) return nullptr;
        size_t at = rd16(d.data() + 0x0C);
        const unsigned blocks = rd16(d.data() + 0x0E);
        for (unsigned b = 0; b < blocks && at + 8 <= (size_t)n; ++b) {
            const uint32_t size = rd32(d.data() + at + 4);
            if (size < 8 || at + size > (size_t)n) return nullptr;
            if (!std::memcmp(d.data() + at, mag, 4)) return d.data() + at;
            at += size;
        }
        return nullptr;
    };
    const uint8_t* ch = section(chr, nc, "RAHC");
    const uint8_t* pl = section(pal, np, "TTLP");
    if (!ch || !pl || rd32(ch + 8 + 4) != 3u) {
        std::fprintf(stderr, "[lb_skin] UI_FE unreadable (%d/%d bytes): buttons stay flat\n", nc, np);
        return false;
    }
    const uint8_t* tiles = ch + 8 + rd32(ch + 8 + 20);
    const uint32_t tbytes = rd32(ch + 8 + 16);
    const uint8_t* colours = pl + 8 + rd32(pl + 8 + 12);
    const uint32_t cbytes = rd32(pl + 8 + 8);
    if (tbytes < 32u * 32u * 32u || cbytes < 6u * 32u) return false;
    auto texel = [&](int x, int y, int bank) -> uint32_t {   // ARGB of the sheet's pixel in a bank
        const int t = (y / 8) * 32 + (x / 8);
        const uint8_t b = tiles[t * 32 + (y % 8) * 4 + (x % 8) / 2];
        const int v = (x & 1) ? (b >> 4) : (b & 15);
        if (!v) return 0u;
        const uint16_t c = rd16(colours + 2 * (16 * bank + v));
        const uint32_t r5 = c & 31u, g5 = (c >> 5) & 31u, b5 = (c >> 10) & 31u;
        return 0xFF000000u | ((r5 * 255u / 31u) << 16) | ((g5 * 255u / 31u) << 8) | (b5 * 255u / 31u);
    };
    // Four bars stacked: blue light, blue dark (bank 1), red light, red dark
    // (bank 5, the same art in the game's red).
    std::vector<uint32_t> img((size_t)kBarW * (size_t)(kBars * kBarH));
    for (int i = 0; i < kBars; ++i)
        for (int y = 0; y < kBarH; ++y)
            for (int x = 0; x < kBarW; ++x)
                img[(size_t)(i * kBarH + y) * kBarW + x] =
                    texel(kBarX + x, ((i & 1) ? kDarkY : kLightY) + y, (i & 2) ? 5 : 1);
    if (g_tex && g_tex_r == r) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kBarW, kBars * kBarH);
    g_tex_r = r;
    if (!g_tex) return false;
    SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_tex, SDL_ScaleModeNearest);
    SDL_UpdateTexture(g_tex, nullptr, img.data(), kBarW * 4);
    g_art_ok = true;
    std::fprintf(stderr, "[lb_skin] brick bar art: %dx%d from UI_FE banks 1 and 5\n", kBarW, kBarH);
    return true;
}

// One bar, 9-sliced into [x0,x1) x [y0,y1): the ends and the top and bottom
// rows at k pixels each, the flat middle stretched.
void draw_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, int bar, ImU32 tint) {
    const ImTextureID tex = (ImTextureID)(intptr_t)g_tex;
    const float k = (float)ImMax(1, (int)std::floor(ImGui::GetFontSize() / (float)kCellH + 0.01f));
    const float tw = (float)kBarW, th = (float)(kBars * kBarH);
    const float sy = (float)(bar * kBarH);
    float cap = kCap * k, edge = kEdge * k;
    if (cap * 2.0f > b.x - a.x) cap = ImFloor((b.x - a.x) * 0.5f);
    if (edge * 2.0f > b.y - a.y) edge = ImFloor((b.y - a.y) * 0.5f);
    const float xs[4] = {a.x, a.x + cap, b.x - cap, b.x};
    const float ys[4] = {a.y, a.y + edge, b.y - edge, b.y};
    const float us[4] = {0.0f, (float)kCap, (float)(kBarW - kCap), (float)kBarW};
    const float vs[4] = {sy, sy + kEdge, sy + kBarH - kEdge, sy + kBarH};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            dl->AddImage(tex, ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]),
                         ImVec2(us[i] / tw, vs[j] / th), ImVec2(us[i + 1] / tw, vs[j + 1] / th), tint);
        }
}

// The colours that mean "draw a bar". Fixed values, not the live style: the
// menu pushes its own colours for some buttons (transparent for a sidebar
// entry, red for Quit/Reset), and only these exact keys are bars.
constexpr ImU32 kKeyLight = IM_COL32(82, 131, 222, 255);    // ImGuiCol_Button
constexpr ImU32 kKeyHover = IM_COL32(106, 156, 231, 255);   // ImGuiCol_ButtonHovered
constexpr ImU32 kKeyDark = IM_COL32(8, 49, 165, 255);       // ImGuiCol_ButtonActive
ImU32 key_u32(float r, float g, float b) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f)); }

// RenderFrame: the button keys become bars; everything else stays flat.
bool frame_hook(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, float) {
    if (!g_tex) return false;
    // the menu's red buttons (menu_red_button in lb_overlay.cpp)
    static const ImU32 red = key_u32(0.62f, 0.12f, 0.12f), red_hov = key_u32(0.80f, 0.18f, 0.18f),
                       red_act = key_u32(0.90f, 0.22f, 0.22f);
    const ImU32 c = col | IM_COL32_A_MASK;   // a disabled button keeps its key at a lower alpha
    const ImU32 tint = IM_COL32(255, 255, 255, (col >> IM_COL32_A_SHIFT) & 0xFF);
    int bar = -1, glow = 0;
    if (c == kKeyLight) bar = 0;
    else if (c == kKeyHover) { bar = 0; glow = 40; }
    else if (c == kKeyDark) bar = 1;
    else if (c == red) bar = 2;
    else if (c == red_hov) { bar = 2; glow = 40; }
    else if (c == red_act) bar = 3;
    if (bar < 0) return false;
    draw_bar(dl, a, b, bar, tint);
    if (glow) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, glow));
    return true;
}

// Tabs: the open one light, the others dark.
bool tab_hook(ImDrawList* dl, const ImVec2& a, const ImVec2& b, int, ImU32 col) {
    if (!g_tex) return false;
    const bool open = col == ImGui::GetColorU32(ImGuiCol_TabSelected) ||
                      col == ImGui::GetColorU32(ImGuiCol_TabDimmedSelected);
    const bool hover = col == ImGui::GetColorU32(ImGuiCol_TabHovered);
    draw_bar(dl, a, b, (open || hover) ? 0 : 1, IM_COL32_WHITE);
    if (hover) dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 30));
    return true;
}

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

}  // namespace

// Load what the skin needs from the cartridge. Cheap after the first success;
// returns whether the font is there (the art is optional: flat buttons).
extern "C" int lb_skin_prepare(SDL_Renderer* r) {
    font_load();
    if (r) art_build(r);
    return g_font_ok ? 1 : 0;
}

// The font, added to the atlas once: the cartridge's glyphs through our
// loader, ImGui's own font merged under it for everything else.
extern "C" ImFont* lb_skin_font(void) {
    if (g_font) return g_font;
    if (!g_font_ok) return nullptr;
    ImGuiIO& io = ImGui::GetIO();
    g_loader.Name = "lb_nftr";
    g_loader.FontSrcContainsGlyph = loader_contains;
    g_loader.FontBakedInit = loader_baked_init;
    g_loader.FontBakedLoadGlyph = loader_load_glyph;
    ImFontConfig cfg;
    cfg.FontLoader = &g_loader;
    cfg.SizePixels = (float)kCellH;
    std::snprintf(cfg.Name, sizeof cfg.Name, "LEGO Battles 10x13");
    g_font = io.Fonts->AddFont(&cfg);
    ImFontConfig fb;
    fb.MergeMode = true;
    fb.SizePixels = (float)kCellH;
    io.Fonts->AddFontDefault(&fb);   // the characters the cartridge has none of
    return g_font;
}

// The style at scale 1: the game's colours, compact sizes, a 17-px frame (the
// bar's height) from the 13-px font and FramePadding.y 2.
extern "C" void lb_skin_style(ImGuiStyle* st) {
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(8, 6);
    s.FramePadding = ImVec2(6, 2);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(4, 4);
    s.IndentSpacing = 14;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.WindowRounding = 3;
    s.FrameRounding = 2;
    s.PopupRounding = 2;
    s.GrabRounding = 2;
    s.TabRounding = 2;
    s.ScrollbarRounding = 2;
    ImVec4* c = s.Colors;
    const ImVec4 navy = rgb(0, 24, 98), deep = rgb(0, 12, 56), border = rgb(41, 74, 180);
    const ImVec4 orange = rgb(255, 176, 32), orange_hi = rgb(255, 206, 90);
    c[ImGuiCol_Text] = rgb(232, 236, 255);
    c[ImGuiCol_TextDisabled] = rgb(130, 150, 210);
    c[ImGuiCol_WindowBg] = rgb(0, 16, 74, 0.96f);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(0, 14, 66, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg] = deep;
    c[ImGuiCol_FrameBgHovered] = rgb(8, 36, 120);
    c[ImGuiCol_FrameBgActive] = rgb(8, 49, 164);
    c[ImGuiCol_TitleBg] = rgb(8, 41, 123);
    c[ImGuiCol_TitleBgActive] = rgb(8, 49, 164);
    c[ImGuiCol_TitleBgCollapsed] = rgb(8, 41, 123, 0.8f);
    c[ImGuiCol_MenuBarBg] = navy;
    c[ImGuiCol_ScrollbarBg] = rgb(0, 8, 40, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = rgb(41, 74, 180);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(82, 131, 222);
    c[ImGuiCol_ScrollbarGrabActive] = orange;
    c[ImGuiCol_CheckMark] = orange;
    c[ImGuiCol_SliderGrab] = orange;
    c[ImGuiCol_SliderGrabActive] = orange_hi;
    // The three button colours are the hook's keys (the bars draw instead), so
    // they are distinct from every other colour here.
    c[ImGuiCol_Button] = rgb(82, 131, 222);
    c[ImGuiCol_ButtonHovered] = rgb(106, 156, 231);
    c[ImGuiCol_ButtonActive] = rgb(8, 49, 165);
    c[ImGuiCol_Header] = rgb(8, 49, 164, 0.85f);
    c[ImGuiCol_HeaderHovered] = rgb(41, 90, 200, 0.9f);
    c[ImGuiCol_HeaderActive] = rgb(255, 150, 20, 0.85f);
    c[ImGuiCol_Separator] = rgb(41, 74, 180, 0.7f);
    c[ImGuiCol_SeparatorHovered] = rgb(82, 131, 222);
    c[ImGuiCol_SeparatorActive] = orange;
    c[ImGuiCol_ResizeGrip] = rgb(41, 74, 180, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = rgb(82, 131, 222, 0.7f);
    c[ImGuiCol_ResizeGripActive] = orange;
    c[ImGuiCol_Tab] = rgb(8, 49, 163);
    c[ImGuiCol_TabHovered] = rgb(106, 156, 229);
    c[ImGuiCol_TabSelected] = rgb(82, 131, 221);
    c[ImGuiCol_TabDimmed] = rgb(8, 41, 122);
    c[ImGuiCol_TabDimmedSelected] = rgb(82, 131, 220);
    c[ImGuiCol_TextSelectedBg] = rgb(255, 176, 32, 0.35f);
    c[ImGuiCol_NavCursor] = orange;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 4, 20, 0.6f);
    *st = s;
}

// Turn the hooks and the text shadow on or off (called when the style changes).
extern "C" void lb_skin_hooks(int on) {
    GImLbFrameHook = on ? frame_hook : nullptr;
    GImLbTabHook = on ? tab_hook : nullptr;
    GImLbTextShadowCol = on ? IM_COL32(8, 16, 56, 255) : 0u;
}
