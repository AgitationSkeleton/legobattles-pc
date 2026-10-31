// Round-trips every stock map through the level editor's model
// (port/runner/src/lb_levelmap.cpp):
//
//   g++ -std=c++17 -O2 -I port/runner/src tools/levelmap_test.cpp
//       port/runner/src/lb_levelmap.cpp -o levelmap_test
//   levelmap_test out/eur/plain
//
// For each Maps/*.map (decompressed): parse, write, and compare byte for byte;
// re-encode the tree mask and recompute the area plane against the stored
// ones; build the four minimaps and count pixels that differ from the
// cartridge's. Exit status 1 if any map fails to parse or round-trip.
#include "lb_levelmap.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 4bpp pixels of a minimap file, in image order, whatever its layout.
static std::vector<uint8_t> pixels(const std::vector<uint8_t>& f, bool tiled) {
    std::vector<uint8_t> px(128 * 128, 0);
    if (f.size() < 0x30 + 0x2000) return px;
    const uint8_t* d = f.data() + 0x30;
    if (tiled) {
        for (int t = 0; t < 256; ++t)
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 4; ++c) {
                    const uint8_t b = d[t * 32 + r * 4 + c];
                    const int x = (t % 16) * 8 + c * 2, y = (t / 16) * 8 + r;
                    px[y * 128 + x] = b & 15;
                    px[y * 128 + x + 1] = b >> 4;
                }
    } else {
        for (int i = 0; i < 0x2000; ++i) {
            px[i * 2] = d[i] & 15;
            px[i * 2 + 1] = d[i] >> 4;
        }
    }
    return px;
}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : "out/eur/plain";
    int maps = 0, bad_parse = 0, bad_rt = 0, bad_d = 0, bad_c = 0;
    long mini_diff = 0, mini_px = 0;
    long conf[16][16] = {};
    int hdr_diff = 0;
    for (const auto& ent : fs::directory_iterator(root / "Maps")) {
        if (ent.path().extension() != ".map") continue;
        ++maps;
        const std::vector<uint8_t> raw = slurp(ent.path());
        lvl::Map m;
        std::string why;
        if (!lvl::parse(raw.data(), raw.size(), &m, &why)) {
            std::printf("PARSE %s: %s\n", ent.path().filename().string().c_str(), why.c_str());
            ++bad_parse;
            continue;
        }
        const std::vector<uint8_t> out = lvl::write(m);
        if (out != raw) {
            size_t i = 0;
            while (i < out.size() && i < raw.size() && out[i] == raw[i]) ++i;
            std::printf("ROUNDTRIP %s: first difference at 0x%zx (sizes %zu/%zu)\n",
                        ent.path().filename().string().c_str(), i, out.size(), raw.size());
            ++bad_rt;
        }
        // The stock Free Play maps are shareable levels as they stand (story
        // maps carry mission pickups, which the check refuses).
        const std::string fn = ent.path().filename().string();
        if (fn.rfind("mp", 0) == 0) {
            const std::vector<uint8_t> tbp = slurp(root / "BP" / ("DetailTiles_" + ent.path().stem().string() + ".tbp"));
            const size_t detail = tbp.size() >= 2 ? (size_t)(tbp[0] | (tbp[1] << 8)) : 0;
            std::string vwhy;
            if (!lvl::validate(m, detail, &vwhy)) {
                std::printf("VALIDATE %s: %s\n", fn.c_str(), vwhy.c_str());
                ++bad_parse;
            }
        }
        const int cells = m.w * m.h;
        const std::vector<uint8_t> mask = lvl::trees_decode(m.D, cells);
        if (lvl::trees_encode(mask) != m.D) {
            std::printf("TREES %s: re-encoded run lengths differ\n", ent.path().filename().string().c_str());
            ++bad_d;
        }
        const std::vector<uint8_t> C = lvl::area_fill(m.A, m.w, m.h);
        if (C != m.C) {
            int k = 0;
            for (int i = 0; i < cells; ++i) k += C[i] != m.C[i];
            std::printf("AREA %s: %d of %d cells differ\n", ent.path().filename().string().c_str(), k, cells);
            ++bad_c;
        }
        const std::string stem = ent.path().stem().string();
        const char* suffix[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR", "miniNT.NCBR"};
        for (int v = 0; v < 4; ++v) {
            const fs::path mp = root / (stem + suffix[v]);
            if (!fs::exists(mp)) continue;
            const std::vector<uint8_t> stock = slurp(mp);
            const bool tiled = v % 2 == 0, trees = v < 2;
            const std::vector<uint8_t> mine = lvl::minimap(m, mask, trees, tiled);
            if (stock.size() >= 0x30 && std::memcmp(stock.data() + 0x10, mine.data() + 0x10, 0x20) != 0) ++hdr_diff;
            const std::vector<uint8_t> a = pixels(stock, tiled), b = pixels(mine, tiled);
            for (int i = 0; i < 128 * 128; ++i) {
                mini_diff += a[i] != b[i];
                if (a[i] != b[i]) ++conf[a[i] & 15][b[i] & 15];
            }
            mini_px += 128 * 128;
        }
    }
    std::printf("%d maps: %d failed to parse, %d did not round-trip, %d tree masks and %d area planes differ\n",
                maps, bad_parse, bad_rt, bad_d, bad_c);
    std::printf("minimaps: %ld of %ld pixels differ (%.3f%%); %d CHAR headers differ\n", mini_diff, mini_px,
                mini_px ? 100.0 * mini_diff / mini_px : 0.0, hdr_diff);
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            if (conf[i][j] > mini_px / 2000) std::printf("  stock %d, ours %d: %ld\n", i, j, conf[i][j]);
    return (bad_parse || bad_rt) ? 1 : 0;
}
