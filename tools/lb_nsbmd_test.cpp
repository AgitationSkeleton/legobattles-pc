// Renders every <dir>/*.nsbmd through lb_nsbmd_render into contact sheets:
// <out>/<Model>.png (4 yaws across, banks 0 Blue and 1 Red down) and
// <out>/_all.png (every model at one yaw, bank 1).
//
// Build (from the repo root):
//   C:\msys64\ucrt64\bin\g++.exe -O2 -std=c++17 -Iport/runner/src tools/lb_nsbmd_test.cpp port/runner/src/lb_nsbmd.cpp -o lb_nsbmd_test.exe
// Run: lb_nsbmd_test.exe <Models dir> <out dir>

#include "lb_nsbmd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// An RGB PNG with stored (uncompressed) deflate blocks: no zlib needed.
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
static void be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i)));
}
static void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
    be32(o, (uint32_t)data.size());
    const size_t at = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, crc32(o.data() + at, o.size() - at) ^ 0xFFFFFFFFu);
}
static bool write_png(const std::string& path, int w, int h, const std::vector<uint32_t>& abgr) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const uint32_t c = abgr[(size_t)y * w + x];
            raw.push_back((uint8_t)c); raw.push_back((uint8_t)(c >> 8)); raw.push_back((uint8_t)(c >> 16));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size() || i == 0; i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)i, raw.begin() + (long)(i + n));
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}, ihdr;
    be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(o, "IHDR", ihdr);
    chunk(o, "IDAT", z);
    chunk(o, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(o.data(), 1, o.size(), f);
    std::fclose(f);
    return true;
}

// Pastes a tile onto a sheet over a checker, so clear pixels show.
static void paste(std::vector<uint32_t>& sheet, int sw, int ox, int oy, const std::vector<uint32_t>& tile, int t) {
    for (int y = 0; y < t; ++y)
        for (int x = 0; x < t; ++x) {
            const uint32_t c = tile[(size_t)y * t + x];
            const uint32_t bg = ((x / 16 + y / 16) & 1) ? 0xFF505050u : 0xFF606060u;
            sheet[(size_t)(oy + y) * sw + ox + x] = c ? c : bg;
        }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <Models dir> <out dir>\n", argv[0]); return 2; }
    fs::create_directories(argv[2]);
    const int T = 192;
    const float pitch = 0.5f;   // about 29 degrees down, near the game's camera
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(argv[1]))
        if (e.path().extension() == ".nsbmd") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    const int cols = 6, rows = ((int)files.size() + cols - 1) / cols;
    std::vector<uint32_t> all((size_t)cols * T * rows * T, 0xFF202020u), tile((size_t)T * T);
    int ok = 0, idx = 0;
    for (const fs::path& p : files) {
        std::vector<uint8_t> d;
        if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            d.resize((size_t)std::ftell(f));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
            std::fclose(f);
        }
        std::vector<uint32_t> sheet((size_t)4 * T * 2 * T);
        bool good = true;
        for (int bank = 0; bank < 2; ++bank)
            for (int k = 0; k < 4; ++k) {
                good = lb_nsbmd_render(d.data(), d.size(), bank, k * 1.5707963f + 0.6f, pitch, T, T, tile.data()) && good;
                paste(sheet, 4 * T, k * T, bank * T, tile, T);
            }
        lb_nsbmd_render(d.data(), d.size(), 1, 0.6f, pitch, T, T, tile.data());
        paste(all, cols * T, (idx % cols) * T, (idx / cols) * T, tile, T);
        ++idx;
        ok += good;
        write_png((fs::path(argv[2]) / (p.stem().string() + ".png")).string(), 4 * T, 2 * T, sheet);
        std::printf("%-24s %s\n", p.filename().string().c_str(), good ? "ok" : "FAILED");
    }
    write_png((fs::path(argv[2]) / "_all.png").string(), cols * T, rows * T, all);
    std::printf("%d of %zu rendered\n", ok, files.size());
    return ok == (int)files.size() ? 0 : 1;
}
