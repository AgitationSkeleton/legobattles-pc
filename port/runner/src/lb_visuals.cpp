// legobattles-decomp docs/506: VISUALS 2.0.
//
// Visuals 2.0 gathers the presentation improvements of Jaime A. Vives'
// LEGO Battles PC port -- a from-scratch re-implementation of the game,
// whose work he gave to this project -- adapted to a port that runs the
// cartridge's own code. Every part is optional (Settings > Display >
// Visuals); Visuals 1.0, the look before it, stays the default.
//
// PHASE 1, CUTSCENES. The cartridge plays its Mobiclip films itself (lifted
// ARM9 code): 256x144 at 15 fps, letterboxed into rows 24..167 of the TOP
// screen -- which in Enhanced is the small Top Screen panel, while the main
// window shows only the bottom screen's pause / play / skip buttons.
//   * A film is recognised by its picture: the top screen black above row 24
//     and below row 168, something in between, and that something changing
//     at film speed (6 or more new pictures in the last second; films run at
//     15). A paused film keeps its bars, so the theatre stays for it; menu
//     art and a top screen gone black do not qualify. (The movie VRAM
//     transfer's byte at *0x02027418 looked like a frame counter in the
//     decompilation; measured, it moves once per film, so it is not used.)
//   * The theatre (lb_visuals_front.inc) shows the film large in the main
//     window, with the bottom screen's buttons in a strip under it; clicks map
//     through the rect the field is drawn into, as they always have.
//   * Jaime's filter chain cleans and doubles each new frame on one worker
//     thread: deblocking on the codec's 4-pixel grid, a 3x3 bilateral filter
//     against the codec's mosquito noise, Anime4K Restore_CNN_Soft_S, then
//     Anime4K Upscale_Denoise_CNN_x2_S over his edge-directed bilinear base.
//     About 30 ms a frame on one core; a film changes 15 times a second, and
//     a frame that did not change (hash) is never redone.
// Presentation only: nothing here writes guest state, so lockstep, replays
// and the exactness gate are untouched.

#include "lb_anime4k_cnn.hpp"
#include "runtime_arm.h"
#include "imgui.h"

#include <atomic>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" int lb_setting_v2_movie_theatre(void);
extern "C" int lb_setting_v2_movie_upscale(void);

namespace {

namespace a4k = lb::nds::anime4k;

constexpr int kMovW = 256, kMovH = 144, kMovY = 24, kTopH = 192;
// A paused film keeps the theatre this long (its picture stops changing
// while its bars stay); past it the ordinary screen comes back.
constexpr uint64_t kPausedMs = 15000;
// New pictures in the last second that make a film.
constexpr int kFilmChanges = 6;

uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline uint8_t clip_u8(int v) { return (uint8_t)std::clamp(v, 0, 255); }

// Jaime A. Vives' cutscene chain (his upscale_2x_anime4k_offline), on an
// RGBA picture: w x h in, 2w x 2h out.
void anime4k_chain(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& dst) {
    std::vector<uint8_t> deblocked = src;
    auto px_at = [&](const std::vector<uint8_t>& buf, int x, int y, int c) -> int {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return int(buf[(size_t(y * w + x) * 4) + size_t(c)]);
    };
    // Pass 0: the codec's block boundaries, across then down.
    for (int y = 0; y < h; ++y)
        for (int x = 4; x < w - 2; x += 4)
            for (int c = 0; c < 3; ++c) {
                int p1 = px_at(src, x - 2, y, c), p0 = px_at(src, x - 1, y, c);
                int q0 = px_at(src, x, y, c), q1 = px_at(src, x + 1, y, c);
                if (std::abs(p0 - q0) < 22 && std::abs(p1 - p0) < 14 && std::abs(q1 - q0) < 14) {
                    int d = std::clamp(((q0 - p0) * 3 + (p1 - q1) + 4) >> 3, -6, 6);
                    deblocked[(size_t(y * w + (x - 1)) * 4) + size_t(c)] = clip_u8(p0 + d);
                    deblocked[(size_t(y * w + x) * 4) + size_t(c)] = clip_u8(q0 - d);
                }
            }
    for (int y = 4; y < h - 2; y += 4)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                int p1 = px_at(deblocked, x, y - 2, c), p0 = px_at(deblocked, x, y - 1, c);
                int q0 = px_at(deblocked, x, y, c), q1 = px_at(deblocked, x, y + 1, c);
                if (std::abs(p0 - q0) < 22 && std::abs(p1 - p0) < 14 && std::abs(q1 - q0) < 14) {
                    int d = std::clamp(((q0 - p0) * 3 + (p1 - q1) + 4) >> 3, -6, 6);
                    deblocked[(size_t((y - 1) * w + x) * 4) + size_t(c)] = clip_u8(p0 + d);
                    deblocked[(size_t(y * w + x) * 4) + size_t(c)] = clip_u8(q0 - d);
                }
            }
    // The 3x3 bilateral filter, so the networks do not sharpen DCT ringing.
    std::vector<a4k::Vec4> cnn_in(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int cr = px_at(deblocked, x, y, 0), cg = px_at(deblocked, x, y, 1), cb = px_at(deblocked, x, y, 2);
            float sr = float(cr) * 4.0f, sg = float(cg) * 4.0f, sb = float(cb) * 4.0f, sw = 4.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    int nr = px_at(deblocked, x + dx, y + dy, 0);
                    int ng = px_at(deblocked, x + dx, y + dy, 1);
                    int nb = px_at(deblocked, x + dx, y + dy, 2);
                    int diff = std::abs(cr - nr) + std::abs(cg - ng) + std::abs(cb - nb);
                    if (diff < 36) {
                        float wgt = (36.0f - float(diff)) * (1.0f / 36.0f);
                        sr += float(nr) * wgt;
                        sg += float(ng) * wgt;
                        sb += float(nb) * wgt;
                        sw += wgt;
                    }
                }
            float inv = 1.0f / (sw * 255.0f);
            cnn_in[size_t(y * w + x)] = {sr * inv, sg * inv, sb * inv, 1.0f};
        }
    std::vector<a4k::Vec4> b0, b1, cnn_2x;
    a4k::apply_restore_cnn(cnn_in, w, h, b0, b1);
    a4k::apply_upscale_cnn_x2(cnn_in, w, h, a4k::kUpscaleDenoiseX2S, b0, b1, cnn_2x);
    dst.resize(size_t(w) * 2 * size_t(h) * 2 * 4);
    for (size_t i = 0; i < cnn_2x.size(); ++i) {
        dst[i * 4 + 0] = clip_u8(int(std::round(cnn_2x[i].x * 255.0f)));
        dst[i * 4 + 1] = clip_u8(int(std::round(cnn_2x[i].y * 255.0f)));
        dst[i * 4 + 2] = clip_u8(int(std::round(cnn_2x[i].z * 255.0f)));
        dst[i * 4 + 3] = 255;
    }
}

// The worker: one frame in, one frame out, the newest wins.
struct Work {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint32_t> in;     // ARGB, kMovW x kMovH
    bool in_ready = false;
    bool in_upscale = false;
    std::vector<uint32_t> out;    // ARGB, out_w x out_h
    int out_w = 0, out_h = 0;
    unsigned gen = 0;
    bool started = false;
} g_work;

void worker() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    std::vector<uint32_t> in;
    std::vector<uint8_t> rgba, big;
    for (;;) {
        bool upscale;
        {
            std::unique_lock<std::mutex> lk(g_work.mu);
            g_work.cv.wait(lk, [] { return g_work.in_ready; });
            in.swap(g_work.in);
            g_work.in_ready = false;
            upscale = g_work.in_upscale;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> out;
        int ow = kMovW, oh = kMovH;
        if (upscale) {
            rgba.resize(in.size() * 4);
            for (size_t i = 0; i < in.size(); ++i) {
                rgba[i * 4 + 0] = uint8_t(in[i] >> 16);
                rgba[i * 4 + 1] = uint8_t(in[i] >> 8);
                rgba[i * 4 + 2] = uint8_t(in[i]);
                rgba[i * 4 + 3] = 255;
            }
            anime4k_chain(rgba, kMovW, kMovH, big);
            ow = kMovW * 2;
            oh = kMovH * 2;
            out.resize(size_t(ow) * size_t(oh));
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = 0xFF000000u | (uint32_t(big[i * 4]) << 16) | (uint32_t(big[i * 4 + 1]) << 8) |
                         uint32_t(big[i * 4 + 2]);
        } else {
            out = in;
        }
        static const bool timing = std::getenv("LB_V2_TIMING") != nullptr;
        if (timing) {
            std::fprintf(stderr, "[lb_visuals] film frame %dx%d in %.1f ms\n", ow, oh,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            std::fflush(stderr);
        }
        std::lock_guard<std::mutex> lk(g_work.mu);
        g_work.out.swap(out);
        g_work.out_w = ow;
        g_work.out_h = oh;
        ++g_work.gen;
    }
}

// Main-thread state.
uint64_t g_seen_hash = 0;          // the film region's last picture
uint64_t g_changes[16] = {};       // when it last changed, newest at g_change_at
int g_change_at = 0;
uint64_t g_last_change = 0;
bool g_active = false;
uint64_t g_last_hash = 0;          // the last picture sent to the worker
bool g_last_upscale = false;

bool dark(uint32_t p) { return ((p >> 16) & 0xFF) < 24 && ((p >> 8) & 0xFF) < 24 && (p & 0xFF) < 24; }

// The film's bars, above and below it, are black; the film itself is not
// all black (a top screen gone dark when the film ends is not a film).
bool looks_like_film(const uint32_t* top, int w) {
    for (int y = 0; y < kTopH; y += 3) {
        if (y >= kMovY && y < kMovY + kMovH) continue;
        for (int x = 0; x < kMovW && x < w; x += 4)
            if (!dark(top[size_t(y) * size_t(w) + size_t(x)])) return false;
    }
    for (int y = kMovY; y < kMovY + kMovH; y += 4)
        for (int x = 0; x < kMovW && x < w; x += 4)
            if (!dark(top[size_t(y) * size_t(w) + size_t(x)])) return true;
    return false;
}

uint64_t hash_film(const uint32_t* top, int w) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int y = kMovY; y < kMovY + kMovH; ++y) {
        const uint32_t* row = top + size_t(y) * size_t(w);
        for (int x = 0; x < kMovW; ++x) {
            h ^= row[x];
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

int changes_within(uint64_t now, uint64_t ms) {
    int n = 0;
    for (uint64_t t : g_changes)
        if (t && now - t <= ms) ++n;
    return n;
}

}  // namespace

// Once a presented frame, with the finished top screen (256 x 192 ARGB).
extern "C" void lb_visuals_top_frame(const uint32_t* top, int w) {
    if (!top || w < kMovW || !lb_setting_v2_movie_theatre()) {
        g_active = false;
        return;
    }
    const uint64_t now = now_ms();
    const bool film = looks_like_film(top, w);
    const uint64_t h = film ? hash_film(top, w) : 0;
    if (film && h != g_seen_hash) {
        g_seen_hash = h;
        g_change_at = (g_change_at + 1) % 16;
        g_changes[g_change_at] = now;
        g_last_change = now;
    }
    const int per_s = changes_within(now, 1000);
    {   // LB_V2_PROBE=1: what the detection sees, once a second (a test aid)
        static const bool probe = std::getenv("LB_V2_PROBE") != nullptr;
        static uint64_t last = 0;
        if (probe && now - last >= 1000) {
            std::fprintf(stderr, "[lb_visuals] probe: film=%d changes/s=%d active=%d\n", film ? 1 : 0, per_s,
                         g_active ? 1 : 0);
            std::fflush(stderr);
            last = now;
        }
    }
    if (film && per_s >= kFilmChanges) {
        if (!g_active) {
            static const bool verbose = std::getenv("LB_V2_TIMING") != nullptr;
            if (verbose) std::fprintf(stderr, "[lb_visuals] film: theatre on\n");
            std::lock_guard<std::mutex> lk(g_work.mu);
            g_work.out.clear();
            g_work.out_w = g_work.out_h = 0;
            ++g_work.gen;
            g_last_hash = 0;
        }
        g_active = true;
    }
    if (g_active && (!film || now - g_last_change > kPausedMs)) g_active = false;
    if (!g_active) return;
    const bool upscale = lb_setting_v2_movie_upscale() != 0;
    if (h == g_last_hash && upscale == g_last_upscale) return;
    g_last_hash = h;
    g_last_upscale = upscale;
    std::lock_guard<std::mutex> lk(g_work.mu);
    g_work.in.resize(size_t(kMovW) * size_t(kMovH));
    for (int y = 0; y < kMovH; ++y)
        std::memcpy(&g_work.in[size_t(y) * kMovW], top + size_t(y + kMovY) * size_t(w),
                    kMovW * sizeof(uint32_t));
    g_work.in_ready = true;
    g_work.in_upscale = upscale;
    if (!g_work.started) {
        g_work.started = true;
        std::thread(worker).detach();
    }
    g_work.cv.notify_one();
}

extern "C" int lb_visuals_theatre_active(void) { return g_active ? 1 : 0; }

// The newest film frame for the theatre: copied into dst (cap pixels) when it
// is newer than *gen; 1 then, with its size; 0 when there is nothing new.
extern "C" int lb_visuals_theatre_frame(uint32_t* dst, int cap, int* w, int* h, unsigned* gen) {
    std::lock_guard<std::mutex> lk(g_work.mu);
    if (!gen || g_work.gen == *gen) return 0;
    *gen = g_work.gen;
    const int n = g_work.out_w * g_work.out_h;
    *w = g_work.out_w;
    *h = g_work.out_h;
    if (n <= 0 || n > cap || !dst) {
        *w = *h = 0;
        return 1;
    }
    std::memcpy(dst, g_work.out.data(), size_t(n) * sizeof(uint32_t));
    return 1;
}

// ============================================================================
// PHASE 2, PRESENTATION EFFECTS (docs/506).
//
// * Victory fireworks. When the local player's team turns to "won" (the team
//   pool at 0x0215540C, each team's state at +0x9C: 2 won, its owner byte at
//   +0x14; the local player's owner id is the word at 0x02156A24 + 100), nine
//   bursts go off over the battlefield across three seconds: a rocket climbs,
//   then sixteen sparks fly out, fall and fade. Jaime's port draws the same
//   show with the cartridge's star sprite; here it is drawn with ImGui shapes
//   over the window.
// * A screen shake for Fireball (0x0E), Artillery (0x17) and Space Laser
//   (0x1C), with Jaime's curve: horizontal, sin(62 t) + 0.45 cos(97 t) times
//   (3.5 + 7.5 k) k guest pixels, k the time left over 1.45 s, for 0.95 s.
//   EARTHQUAKE IS LEFT OUT ON PURPOSE: the cartridge already shakes the
//   screen for it (Battles::EarthQuakeEffect's update, 0x02015880, walks a
//   shake table for 35 frames), so adding his would shake it twice.
// Both are presentation only.

extern "C" int lb_setting_v2_fireworks(void);
extern "C" int lb_setting_v2_spell_shake(void);

namespace {

std::atomic<uint64_t> g_shake_end{0};
constexpr uint64_t kShakeMs = 950;

struct Spark {
    float x, y, vx, vy, born, life;
    ImU32 col;
};
struct Rocket {
    float x, y0, y1, t0, rise;
    ImU32 col;
    bool burst;
};
std::vector<Spark> g_sparks;
std::vector<Rocket> g_rockets;
float g_show_t0 = -1.0f;
int g_last_state = -1;
uint32_t g_rng = 0x2545F491u;

float frand() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return float(g_rng >> 8) * (1.0f / 16777216.0f);
}

float now_s() { return float(double(now_ms() % 100000000ull) / 1000.0); }

// The local player's team result: 0 playing, 1 defeated, 2 won, 3 lost, or -1.
int local_team_state() {
    const uint32_t pool = bus_read_u32(0x0215540Cu);
    if (pool < 0x02000000u || pool >= 0x02400000u) return -1;
    const uint32_t arr = bus_read_u32(pool + 8u), cnt = bus_read_u32(pool + 0xCu);
    if (arr < 0x02000000u || arr >= 0x02400000u || cnt == 0u || cnt > 8u) return -1;
    const uint32_t local = bus_read_u32(0x02156A24u + 100u);
    for (uint32_t k = 0; k < cnt; ++k) {
        const uint32_t t = bus_read_u32(arr + 4u * k);
        if (t < 0x02000000u || t >= 0x02400000u) continue;
        if (bus_read_u8(t + 0x14u) == (local & 0xFFu)) return (int)bus_read_u32(t + 0x9Cu);
    }
    return -1;
}

void start_show(float w, float h) {
    static const ImU32 kCols[] = {IM_COL32(255, 213, 74, 255), IM_COL32(255, 90, 90, 255),
                                  IM_COL32(90, 160, 255, 255), IM_COL32(107, 227, 107, 255),
                                  IM_COL32(255, 255, 255, 255), IM_COL32(214, 120, 255, 255)};
    g_rockets.clear();
    g_sparks.clear();
    const float t = now_s();
    g_show_t0 = t;
    for (int i = 0; i < 9; ++i) {
        Rocket r;
        r.x = w * (0.15f + 0.7f * frand());
        r.y0 = h * 1.02f;
        r.y1 = h * (0.15f + 0.35f * frand());
        r.t0 = t + 3.0f * float(i) / 9.0f + 0.1f * frand();
        r.rise = 0.45f + 0.15f * frand();
        r.col = kCols[i % 6];
        r.burst = false;
        g_rockets.push_back(r);
    }
    std::fprintf(stderr, "[lb_visuals] victory: fireworks\n");
    std::fflush(stderr);
}

void draw_star(ImDrawList* dl, float x, float y, float r, ImU32 col) {
    dl->AddQuadFilled(ImVec2(x, y - r * 2.2f), ImVec2(x + r * 0.55f, y), ImVec2(x, y + r * 2.2f),
                      ImVec2(x - r * 0.55f, y), col);
    dl->AddQuadFilled(ImVec2(x - r * 2.2f, y), ImVec2(x, y - r * 0.55f), ImVec2(x + r * 2.2f, y),
                      ImVec2(x, y + r * 0.55f), col);
}

ImU32 fade(ImU32 col, float a) {
    const int alpha = (int)(float((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
}

}  // namespace

// From the cartridge's spell casts (lb_patches.cpp, cast_log): the spell id.
extern "C" void lb_visuals_spell(unsigned id) {
    if (id != 0x0Eu && id != 0x17u && id != 0x1Cu) return;
    if (!lb_setting_v2_spell_shake()) return;
    g_shake_end = now_ms() + kShakeMs;
}

// The field's horizontal shake this frame, in window pixels (scale: window
// pixels per guest pixel). 0 when there is none.
extern "C" float lb_visuals_shake_px(float scale) {
    const uint64_t end = g_shake_end.load(), now = now_ms();
    if (!end || now >= end) return 0.0f;
    const float left = float(end - now) / 1000.0f;
    const float k = left / 1.45f;
    const float amp = (3.5f + 7.5f * k) * k;
    const float t = now_s();
    return (std::sin(t * 62.0f) * amp + std::cos(t * 97.0f) * amp * 0.45f) * scale;
}

// Once a frame, from the overlay (ImGui is between NewFrame and Render): the
// victory watch, and the fireworks drawn over the window.
extern "C" void lb_visuals_overlay(void) {
    const int st = lb_setting_v2_fireworks() ? local_team_state() : -1;
    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    {   // Test aids: LB_V2_FIREWORKS_TEST=<s> starts the show that many seconds
        // after a battle begins; LB_V2_SHAKE_TEST=1 shakes the field every 3 s.
        static const char* fw = std::getenv("LB_V2_FIREWORKS_TEST");
        static const bool sk = std::getenv("LB_V2_SHAKE_TEST") != nullptr;
        static uint64_t t_start = 0;
        static bool fired = false;
        if (fw && !t_start && local_team_state() == 0) {
            t_start = now_ms();
            std::fprintf(stderr, "[lb_visuals] test: a battle is on; fireworks in %s s\n", fw);
            std::fflush(stderr);
        }
        if (fw && t_start && !fired && now_ms() - t_start >= uint64_t(std::atoi(fw)) * 1000ull && w > 0 && h > 0) {
            fired = true;
            start_show(w, h);
        }
        static uint64_t next_shake = 0;
        if (sk && now_ms() >= next_shake) {
            next_shake = now_ms() + 3000;
            g_shake_end = now_ms() + kShakeMs;
        }
    }
    if (st == 2 && g_last_state != 2 && g_last_state != -2 && w > 0 && h > 0) start_show(w, h);
    g_last_state = st;
    if (g_rockets.empty() && g_sparks.empty()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();   // over the game, under the panels
    const float t = now_s();
    const float u = h / 720.0f;   // sizes follow the window
    for (Rocket& r : g_rockets) {
        if (t < r.t0 || r.burst) continue;
        const float p = (t - r.t0) / r.rise;
        if (p >= 1.0f) {
            r.burst = true;
            for (int i = 0; i < 16; ++i) {
                const float a = 6.2831853f * (float(i) + 0.3f * frand()) / 16.0f;
                const float v = (150.0f + 110.0f * frand()) * u;
                g_sparks.push_back({r.x, r.y1, std::cos(a) * v, std::sin(a) * v, t, 1.1f + 0.4f * frand(), r.col});
            }
            continue;
        }
        const float e = 1.0f - (1.0f - p) * (1.0f - p);
        const float y = r.y0 + (r.y1 - r.y0) * e;
        dl->AddLine(ImVec2(r.x, y), ImVec2(r.x, y + 26.0f * u), fade(r.col, 0.45f), 2.0f * u);
        draw_star(dl, r.x, y, 2.6f * u, r.col);
    }
    size_t keep = 0;
    for (const Spark& s0 : g_sparks) {
        const float age = t - s0.born;
        if (age >= s0.life) continue;
        Spark s = s0;
        const float x = s.x + s.vx * age, y = s.y + s.vy * age + 0.5f * 220.0f * u * age * age;
        const float a = 1.0f - age / s.life;
        draw_star(dl, x, y, (2.4f + 2.0f * a) * u, fade(s.col, a));
        g_sparks[keep++] = s0;
    }
    g_sparks.resize(keep);
    bool live = false;
    for (const Rocket& r : g_rockets) live = live || !r.burst;
    if (!live && g_sparks.empty()) g_rockets.clear();
}

// ============================================================================
// PHASE 3: JAIME'S HUD ART, FROM THE ROM (docs/506).
//
// His HUD is drawn from the cartridge's own UI graphics, decoded the way his
// renderer decodes them (his nitro_gfx parsers, below in brief):
//
//   0  the HUD digits: UI/WorldViewTop_Back.NCGR x 216..255, y 0..23, through
//      WorldViewTop_Back.NCLR sub-palette 0 ("1 2 3 4 5 / 6 7 8 9 0 / / - + . :")
//   1  UI/GameAnims.NCBR through WorldViewTop_Back.NCLR sub-palette 7: the
//      spinning brick (row 0), star (row 1) and minifig head (row 2)
//   2  the blue stud baseplate tile: UI/WorldViewTop_0.NSCR rendered through
//      WorldViewTop_Back, rows 144..159, columns 0..15
//   3  his red baseplate tile, the blue one recoloured by his own formula
//
// Images are 0xAARRGGBB (SDL's ARGB8888), made once.
//
// And the resource bar's values: the cartridge draws them on its top screen as
// 8x8 tiles of exactly that digit font, on the tile row at y 176 (measured:
// "500" at x 24, "1/4" at x 104, "0/0" at x 176, every glyph an exact match),
// so they are read back tile by tile and redrawn in his style.

extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap);   // lb_teams.cpp

namespace {

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

std::vector<uint8_t> rom_file(const char* path) {
    const int n = lb_rom_read_unwrapped(path, nullptr, 0);
    std::vector<uint8_t> v(n > 0 ? (size_t)n : 0u);
    if (n <= 0 || lb_rom_read_unwrapped(path, v.data(), n) != n) v.clear();
    return v;
}

uint32_t argb555(uint16_t c) {
    const uint32_t r = (c & 31u) * 255u / 31u, g = ((c >> 5) & 31u) * 255u / 31u, b = ((c >> 10) & 31u) * 255u / 31u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool nclr(const std::vector<uint8_t>& d, std::vector<uint32_t>& out) {
    if (d.size() < 0x28 || std::memcmp(d.data(), "RLCN", 4) != 0) return false;
    const uint16_t hs = rd16(&d[0x0C]);
    if (hs + 24u > d.size() || std::memcmp(&d[hs], "TTLP", 4) != 0) return false;
    const uint32_t size = rd32(&d[hs + 0x10]), off = rd32(&d[hs + 0x14]);
    const size_t st = hs + 8u + off;
    if (st > d.size()) return false;
    const size_t n = std::min<size_t>(size, d.size() - st) / 2u;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = argb555(rd16(&d[st + 2 * i]));
    return n > 0;
}

struct Gfx {
    int w = 0, h = 0, bpp = 8;
    std::vector<uint8_t> px;   // palette indices, linear
};

bool ncgr(const std::vector<uint8_t>& d, Gfx& g) {
    if (d.size() < 0x30 || std::memcmp(d.data(), "RGCN", 4) != 0) return false;
    const uint16_t hs = rd16(&d[0x0C]);
    if (hs + 0x20u > d.size() || std::memcmp(&d[hs], "RAHC", 4) != 0) return false;
    uint16_t ty = rd16(&d[hs + 8]), tx = rd16(&d[hs + 10]);
    const uint32_t fmt = rd32(&d[hs + 0x0C]), flag = rd32(&d[hs + 0x14]);
    const uint32_t size = rd32(&d[hs + 0x18]), off = rd32(&d[hs + 0x1C]);
    g.bpp = fmt == 4 ? 8 : 4;
    const size_t st = hs + 8u + off;
    if (st > d.size()) return false;
    const size_t avail = std::min<size_t>(size, d.size() - st);
    const uint8_t* s = &d[st];
    const size_t bpt = g.bpp == 8 ? 64u : 32u;
    if (tx == 0xFFFF || ty == 0xFFFF) {
        tx = 16;
        ty = (uint16_t)((avail / bpt + 15u) / 16u);
    }
    g.w = tx * 8;
    g.h = ty * 8;
    g.px.assign((size_t)g.w * (size_t)g.h, 0);
    if (flag & 1u) {   // a linear bitmap (NCBR)
        if (g.bpp == 8) {
            std::memcpy(g.px.data(), s, std::min(avail, g.px.size()));
        } else {
            for (size_t i = 0; i < avail && 2 * i < g.px.size(); ++i) {
                g.px[2 * i] = s[i] & 15u;
                if (2 * i + 1 < g.px.size()) g.px[2 * i + 1] = s[i] >> 4;
            }
        }
        return true;
    }
    for (size_t t = 0; t < avail / bpt; ++t) {
        const int c = int(t % tx), r = int(t / tx);
        if (r >= ty) break;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const uint8_t v = g.bpp == 8 ? s[t * 64u + (size_t)(y * 8 + x)]
                                             : ((s[t * 32u + (size_t)(y * 4 + x / 2)] >> ((x & 1) * 4)) & 15u);
                g.px[(size_t)(r * 8 + y) * (size_t)g.w + (size_t)(c * 8 + x)] = v;
            }
    }
    return true;
}

struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};
Img g_hud_img[4];
int g_hud_state = 0;   // 0 not tried, 1 ready, -1 failed

void hud_load() {
    g_hud_state = -1;
    std::vector<uint32_t> pal;
    Gfx back, anims;
    if (!nclr(rom_file("UI/WorldViewTop_Back.NCLR"), pal) || !ncgr(rom_file("UI/WorldViewTop_Back.NCGR"), back)) {
        std::fprintf(stderr, "[lb_visuals] HUD art: WorldViewTop_Back not readable\n");
        return;
    }
    auto colour = [&](const Gfx& g, int i, int sub, bool transparent0) -> uint32_t {
        const uint8_t v = g.px[(size_t)i];
        if (transparent0 && v == 0) return 0u;
        const size_t ci = (size_t)(g.bpp == 4 ? sub * 16 : 0) + v;
        return ci < pal.size() ? pal[ci] : 0xFFFF00FFu;
    };
    // 0: the digits.
    if (back.w >= 256 && back.h >= 24) {
        Img& d = g_hud_img[0];
        d.w = 40;
        d.h = 24;
        d.px.resize(40u * 24u);
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 40; ++x) d.px[(size_t)(y * 40 + x)] = colour(back, y * back.w + 216 + x, 0, true);
    }
    // 1: the spinning icons.
    if (ncgr(rom_file("UI/GameAnims.NCBR"), anims)) {
        Img& a = g_hud_img[1];
        const int sub = pal.size() >= 8u * 16u ? 7 : 1;
        a.w = anims.w;
        a.h = anims.h;
        a.px.resize((size_t)a.w * (size_t)a.h);
        for (int i = 0; i < a.w * a.h; ++i) a.px[(size_t)i] = colour(anims, i, sub, true);
    }
    // 2 and 3: the baseplate tile, from the top screen's board.
    const std::vector<uint8_t> scr = rom_file("UI/WorldViewTop_0.NSCR");
    if (scr.size() >= 0x24 && std::memcmp(scr.data(), "RCSN", 4) == 0) {
        const uint16_t hs = rd16(&scr[0x0C]);
        const int tw = rd16(&scr[hs + 8]) / 8;
        const size_t st = hs + 0x14u;
        Img& b = g_hud_img[2];
        b.w = b.h = 16;
        b.px.assign(256, 0xFF000000u);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const int gy = 144 + y, gx = x;
                const size_t ei = st + 2u * (size_t)((gy / 8) * tw + gx / 8);
                if (ei + 2 > scr.size()) continue;
                const uint16_t e = rd16(&scr[ei]);
                const int tid = e & 0x3FF, sp = (e >> 12) & 15;
                int px = gx & 7, py = gy & 7;
                if (e & 0x400) px = 7 - px;
                if (e & 0x800) py = 7 - py;
                const int tiles_x = back.w / 8;
                const int sx = (tid % tiles_x) * 8 + px, sy = (tid / tiles_x) * 8 + py;
                if (sy < back.h) b.px[(size_t)(y * 16 + x)] = colour(back, sy * back.w + sx, sp, false);
            }
        Img& r = g_hud_img[3];
        r = b;
        for (uint32_t& c : r.px) {   // his red baseplate, by his formula
            const int R = (int)((c >> 16) & 255), G = (int)((c >> 8) & 255), B = (int)(c & 255);
            int nr = std::clamp(B + 12, 0, 255), ng = std::clamp(R * 70 / 100 + G * 18 / 100, 0, 220),
                nb = std::clamp(R * 58 / 100 + G * 12 / 100, 0, 210);
            if (R > 130 && G > 170 && B > 200) {
                nr = 255;
                ng = 162;
                nb = 154;
            }
            c = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
        }
    }
    g_hud_state = (g_hud_img[0].w && g_hud_img[1].w && g_hud_img[2].w) ? 1 : -1;
    std::fprintf(stderr, "[lb_visuals] HUD art: digits %dx%d, anims %dx%d, studs %dx%d: %s\n", g_hud_img[0].w,
                 g_hud_img[0].h, g_hud_img[1].w, g_hud_img[1].h, g_hud_img[2].w, g_hud_img[2].h,
                 g_hud_state > 0 ? "ready" : "incomplete");
    std::fflush(stderr);
}

}  // namespace

// One of the HUD images above (0..3), or 0 when the ROM could not supply it.
extern "C" int lb_v2_hud_image(int which, const uint32_t** px, int* w, int* h) {
    if (g_hud_state == 0) hud_load();
    if (g_hud_state < 0 || which < 0 || which > 3 || !g_hud_img[which].w) return 0;
    *px = g_hud_img[which].px.data();
    *w = g_hud_img[which].w;
    *h = g_hud_img[which].h;
    return 1;
}

// The values on the battle's resource bar, read off the top screen (256x192,
// 0xAARRGGBB): out[0] bricks, out[1] minifigures, out[2] the third count (each
// "" when none is read). Returns how many digits were read.
extern "C" int lb_v2_hud_read_bar(const uint32_t* top, char out[3][16]) {
    for (int k = 0; k < 3; ++k) out[k][0] = 0;
    if (!top) return 0;
    if (g_hud_state == 0) hud_load();
    if (g_hud_state < 0) return 0;
    static const char kNames[] = "12345" "67890" "/-+.:";
    static uint64_t mask[15];
    static bool made = false;
    if (!made) {
        const Img& d = g_hud_img[0];
        for (int k = 0; k < 15; ++k) {
            uint64_t m = 0;
            const int gx = (k % 5) * 8, gy = (k / 5) * 8;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const uint32_t c = d.px[(size_t)((gy + y) * 40 + gx + x)];
                    const int s = (int)((c >> 16) & 255) + (int)((c >> 8) & 255) + (int)(c & 255);
                    if ((c >> 24) && s > 450) m |= 1ull << (y * 8 + x);
                }
            mask[k] = m;
        }
        made = true;
    }
    static const int kCellEnd[3] = {80, 152, 208};
    int got = 0;
    for (int tx = 0; tx < 26; ++tx) {
        uint64_t m = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const uint32_t c = top[(176 + y) * 256 + tx * 8 + x];
                const int mx = std::max({(int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255)});
                const int s = (int)((c >> 16) & 255) + (int)((c >> 8) & 255) + (int)(c & 255);
                if (mx > 180 && s > 380) m |= 1ull << (y * 8 + x);
            }
        if (!m) continue;
        // The closest glyph, and only a close one ('0' and '8' differ by a
        // couple of pixels, so the first near match is not good enough).
        int best = -1, best_d = 3;
        for (int k = 0; k < 15; ++k) {
            const int d = __builtin_popcountll(m ^ mask[k]);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        if (best < 0) continue;
        const int cell = tx * 8 < kCellEnd[0] ? 0 : tx * 8 < kCellEnd[1] ? 1 : 2;
        const size_t n = std::strlen(out[cell]);
        if (n + 1 < 16) {
            out[cell][n] = kNames[best];
            out[cell][n + 1] = 0;
            ++got;
        }
    }
    return got;
}
