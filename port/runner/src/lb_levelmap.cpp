// The level editor's map model (lb_levelmap.h). Every read is bounds-checked:
// the same parser vets maps that arrive from other players.
#include "lb_levelmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lvl {
namespace {

constexpr uint8_t kMark = 0x4C;   // 'L', a record
constexpr uint8_t kEnd = 0x21;    // '!', a chunk's end
constexpr int kHeader = 0x28;     // TERR's fixed header, tileset name included

struct Reader {
    const uint8_t* d;
    size_t n, p = 0;
    bool ok = true;
    bool need(size_t k) {
        if (!ok || p + k > n) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? d[p++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = (uint16_t)(d[p] | (d[p + 1] << 8));
        p += 2;
        return v;
    }
    bool tag(const char* t, size_t k) {
        if (!need(k) || std::memcmp(d + p, t, k) != 0) return ok = false;
        p += k;
        return true;
    }
    void bytes(uint8_t* out, size_t k) {
        if (!need(k)) return;
        std::memcpy(out, d + p, k);
        p += k;
    }
};

void put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back((uint8_t)(v & 0xFF));
    o.push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i)));
}
void puts(std::vector<uint8_t>& o, const char* s) {
    o.insert(o.end(), s, s + std::strlen(s));
}

// A counted list: its count is one byte, so more than 255 cannot be written.
template <class T>
bool fits(const std::vector<T>& v) { return v.size() <= 255; }

}  // namespace

bool parse(const uint8_t* data, size_t n, Map* out, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (!data || n < 3 + kHeader + 4) return fail("too short for a map");
    Reader r{data, n};
    if (!r.tag("MAP", 3)) return fail("not a map file (no MAP)");
    if (!r.tag("TERR", 4)) return fail("no TERR chunk");
    Map m;
    m.w = r.u8();
    m.h = r.u8();
    m.u6 = r.u8();
    m.u7 = r.u8();
    if (m.w < 8 || m.h < 8) return fail("map smaller than 8x8");
    char name[33] = {};
    r.bytes(reinterpret_cast<uint8_t*>(name), 32);
    for (int i = 0; i < 32 && name[i]; ++i)
        if (name[i] < 0x20 || name[i] > 0x7E) return fail("tileset name is not text");
    m.tileset = name;
    const size_t cells = (size_t)m.w * (size_t)m.h;
    m.A.resize(cells);
    m.B.resize(cells);
    m.C.resize(cells);
    r.bytes(m.A.data(), cells);
    r.bytes(m.B.data(), cells);
    r.bytes(m.C.data(), cells);
    const uint16_t dn = r.u16();
    m.D.resize(dn);
    r.bytes(m.D.data(), dn);
    m.E.resize(cells);
    for (size_t i = 0; i < cells && r.ok; ++i) m.E[i] = r.u16();
    if (!r.ok) return fail("terrain is cut short");
    if (!r.tag("RRET", 4)) return fail("terrain does not end with RRET");

    if (!r.tag("EVNT", 4)) return fail("no EVNT chunk");
    while (r.ok && r.need(1) && data[r.p] == kMark) {
        ++r.p;
        Event e;
        e.id = r.u8();
        const int np = r.u8();
        for (int i = 0; i < np && r.ok; ++i) {
            Point q{};
            q.x = r.u8();
            q.y = r.u8();
            e.points.push_back(q);
        }
        e.area_flag = r.u8();
        if (e.area_flag) r.bytes(e.area, 4);
        const int ne = r.u8();
        for (int i = 0; i < ne && r.ok; ++i) {
            Entity x{};
            r.bytes(&x.x, 9);
            e.ents.push_back(x);
        }
        const int nk = r.u8();
        for (int i = 0; i < nk && r.ok; ++i) {
            Pickup x{};
            r.bytes(&x.x, 8);
            e.picks.push_back(x);
        }
        const int nw = r.u8();
        for (int i = 0; i < nw && r.ok; ++i) {
            Wall x{};
            r.bytes(&x.x, 3);
            e.walls.push_back(x);
        }
        if (r.ok) m.events.push_back(std::move(e));
    }
    if (r.u8() != kEnd || !r.ok) return fail("EVNT chunk is malformed");

    if (!r.tag("TRIG", 4)) return fail("no TRIG chunk");
    while (r.ok && r.need(1) && data[r.p] == kMark) {
        ++r.p;
        Trigger t;
        t.id = r.u8();
        t.flag = r.u8();
        if (t.flag) r.bytes(t.body, 10);
        if (r.ok) m.triggers.push_back(t);
    }
    if (r.u8() != kEnd || !r.ok) return fail("TRIG chunk is malformed");

    if (!r.tag("MARK", 4)) return fail("no MARK chunk");
    while (r.ok && r.need(1) && data[r.p] == kMark) {
        ++r.p;
        MarkerGroup g;
        g.id = r.u8();
        const int k = r.u8();
        for (int i = 0; i < k && r.ok; ++i) {
            Marker x{};
            r.bytes(&x.x, 3);
            g.m.push_back(x);
        }
        if (r.ok) m.markers.push_back(std::move(g));
    }
    if (r.u8() != kEnd || !r.ok) return fail("MARK chunk is malformed");

    if (!r.tag("MINE", 4)) return fail("no MINE chunk");
    while (r.ok && r.need(1) && data[r.p] == kMark) {
        ++r.p;
        std::vector<Point> g;
        const int k = r.u8();
        for (int i = 0; i < k && r.ok; ++i) {
            Point q{};
            q.x = r.u8();
            q.y = r.u8();
            g.push_back(q);
        }
        if (r.ok) m.mines.push_back(std::move(g));
    }
    if (r.u8() != kEnd || !r.ok) return fail("MINE chunk is malformed");
    if (!r.tag("PAM", 3)) return fail("file does not end with PAM");
    if (r.p != n) return fail("bytes after the end of the map");
    if (out) *out = std::move(m);
    return true;
}

std::vector<uint8_t> write(const Map& m) {
    std::vector<uint8_t> o;
    const size_t cells = (size_t)m.w * (size_t)m.h;
    o.reserve(64 + cells * 5 + m.D.size() + 4096);
    puts(o, "MAPTERR");
    o.push_back((uint8_t)m.w);
    o.push_back((uint8_t)m.h);
    o.push_back(m.u6);
    o.push_back(m.u7);
    char name[32] = {};
    std::memcpy(name, m.tileset.data(), std::min<size_t>(31, m.tileset.size()));
    o.insert(o.end(), name, name + 32);
    auto plane = [&](const std::vector<uint8_t>& p) {
        for (size_t i = 0; i < cells; ++i) o.push_back(i < p.size() ? p[i] : 0);
    };
    plane(m.A);
    plane(m.B);
    plane(m.C);
    put16(o, (uint16_t)m.D.size());
    o.insert(o.end(), m.D.begin(), m.D.end());
    for (size_t i = 0; i < cells; ++i) put16(o, i < m.E.size() ? m.E[i] : 0);
    puts(o, "RRETEVNT");
    for (const Event& e : m.events) {
        if (!fits(e.points) || !fits(e.ents) || !fits(e.picks) || !fits(e.walls)) continue;
        o.push_back(kMark);
        o.push_back(e.id);
        o.push_back((uint8_t)e.points.size());
        for (const Point& q : e.points) {
            o.push_back(q.x);
            o.push_back(q.y);
        }
        o.push_back(e.area_flag);
        if (e.area_flag) o.insert(o.end(), e.area, e.area + 4);
        o.push_back((uint8_t)e.ents.size());
        for (const Entity& x : e.ents) o.insert(o.end(), &x.x, &x.x + 9);
        o.push_back((uint8_t)e.picks.size());
        for (const Pickup& x : e.picks) o.insert(o.end(), &x.x, &x.x + 8);
        o.push_back((uint8_t)e.walls.size());
        for (const Wall& x : e.walls) o.insert(o.end(), &x.x, &x.x + 3);
    }
    o.push_back(kEnd);
    puts(o, "TRIG");
    for (const Trigger& t : m.triggers) {
        o.push_back(kMark);
        o.push_back(t.id);
        o.push_back(t.flag);
        if (t.flag) o.insert(o.end(), t.body, t.body + 10);
    }
    o.push_back(kEnd);
    puts(o, "MARK");
    for (const MarkerGroup& g : m.markers) {
        if (!fits(g.m)) continue;
        o.push_back(kMark);
        o.push_back(g.id);
        o.push_back((uint8_t)g.m.size());
        for (const Marker& x : g.m) o.insert(o.end(), &x.x, &x.x + 3);
    }
    o.push_back(kEnd);
    puts(o, "MINE");
    for (const std::vector<Point>& g : m.mines) {
        if (!fits(g)) continue;
        o.push_back(kMark);
        o.push_back((uint8_t)g.size());
        for (const Point& q : g) {
            o.push_back(q.x);
            o.push_back(q.y);
        }
    }
    o.push_back(kEnd);
    puts(o, "PAM");
    return o;
}

int start_count(const Map& m) {
    int n = 0;
    for (const Event& e : m.events) {
        if (e.ents.size() < 6) continue;
        bool base = false, one_team = true;
        for (const Entity& x : e.ents) {
            base = base || x.type == kBase;
            one_team = one_team && x.team == e.ents[0].team;
        }
        n += base && one_team;
    }
    return n;
}

bool validate(const Map& m, size_t detail, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (m.w < 32 || m.h < 32 || m.w > 128 || m.h > 128) return fail("map size");
    if (m.u6 != 3 || m.u7 != 2) return fail("header");
    if (m.tileset != "KingTileset" && m.tileset != "PirateTileset" && m.tileset != "MarsTileset")
        return fail("tileset");
    const size_t cells = (size_t)m.w * m.h;
    if (m.A.size() != cells || m.B.size() != cells || m.C.size() != cells || m.E.size() != cells)
        return fail("terrain");
    for (uint8_t a : m.A)
        if (a != kGrass && a != kStone && a != kWater && a != kMountain) return fail("terrain class");
    size_t total = 0;
    for (uint8_t r : m.D) total += r;
    if (total != cells) return fail("tree mask length");
    for (uint16_t e : m.E)
        if (e >= 440 + detail) return fail("tile index");
    if (area_fill(m.A, m.w, m.h) != m.C) return fail("area plane");
    auto on = [&](int x, int y) { return x >= 0 && y >= 0 && x < m.w && y < m.h; };
    std::vector<bool> team_start(4, false);
    for (const Event& e : m.events) {
        if (e.id > 46 || (e.id & 1)) return fail("event id");
        for (const Point& q : e.points)
            if (!on(q.x, q.y)) return fail("patrol point");
        if (e.area_flag && (!on(e.area[0], e.area[1]) || !on(e.area[2], e.area[3]))) return fail("event area");
        bool base = false, one_team = true;
        for (const Entity& x : e.ents) {
            if (!on(x.x, x.y) || x.team > 3 || x.type > kPickup || x.health > 100) return fail("entity");
            base = base || x.type == kBase;
            one_team = one_team && x.team == e.ents[0].team;
        }
        if (base && one_team && e.ents.size() >= 6) {
            if (team_start[e.ents[0].team]) return fail("two starts for one player");
            team_start[e.ents[0].team] = true;
        }
        for (const Pickup& p : e.picks)
            if (!on(p.x, p.y) || p.type != kPickup || p.subtype != 8) return fail("pickup");
        for (const Wall& wl : e.walls)
            if (!on(wl.x, wl.y) || wl.team > 3) return fail("wall");
    }
    for (const Trigger& t : m.triggers)
        if (t.flag && (!on(t.body[0], t.body[1]) || !on(t.body[2], t.body[3]))) return fail("trigger area");
    for (const MarkerGroup& g : m.markers) {
        if (g.id > 40) return fail("marker group");
        for (const Marker& k : g.m)
            if (!on(k.x, k.y) || k.flag > 1) return fail("marker");
    }
    if (m.mines.size() != 4) return fail("mine groups");
    for (const auto& g : m.mines)
        for (const Point& q : g)
            if (!on(q.x, q.y) || !on(q.x + 1, q.y + 1)) return fail("mine");
    if (start_count(m) < 2) return fail("starts");
    return true;
}

// Runs alternate off, on, off..., off first; each byte is one run and then the
// state flips, so a run longer than 255 is 255, 0 (an empty run), the rest.
std::vector<uint8_t> trees_decode(const std::vector<uint8_t>& D, int cells) {
    std::vector<uint8_t> mask((size_t)std::max(cells, 0), 0);
    size_t at = 0;
    uint8_t state = 0;
    for (uint8_t run : D) {
        for (int k = 0; k < run && at < mask.size(); ++k) mask[at++] = state;
        state ^= 1;
    }
    return mask;
}

std::vector<uint8_t> trees_encode(const std::vector<uint8_t>& mask) {
    std::vector<uint8_t> runs;
    uint8_t cur = 0;
    size_t n = 0;
    auto flush = [&]() {
        while (n > 255) {
            runs.push_back(255);
            runs.push_back(0);
            n -= 255;
        }
        runs.push_back((uint8_t)n);
    };
    for (uint8_t v : mask) {
        const uint8_t b = v ? 1 : 0;
        if (b == cur) {
            ++n;
        } else {
            flush();
            cur = b;
            n = 1;
        }
    }
    flush();
    return runs;
}

std::vector<uint8_t> area_fill(const std::vector<uint8_t>& A, int w, int h) {
    const size_t cells = (size_t)w * (size_t)h;
    std::vector<int> lab(cells, -1);
    auto fam = [&](size_t i) { const uint8_t a = A[i]; return (a == kGrass || a == kStone) ? 0 : (int)a + 1; };
    std::vector<int> st;
    int cur = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            if (lab[i] >= 0) continue;
            const int f = fam(i);
            lab[i] = cur;
            st.assign(1, (int)i);
            while (!st.empty()) {
                const int c = st.back();
                st.pop_back();
                const int cx = c % w, cy = c / w;
                // EIGHT neighbours: the diagonal fill reproduces plane C on
                // all 122 stock maps; the four-neighbour one (the map tool's,
                // and custommaps/area.py's) differs on 58 of them.
                const int nb[8][2] = {{cx + 1, cy}, {cx, cy + 1}, {cx - 1, cy}, {cx, cy - 1},
                                      {cx + 1, cy + 1}, {cx - 1, cy + 1}, {cx + 1, cy - 1},
                                      {cx - 1, cy - 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[0] >= w || q[1] < 0 || q[1] >= h) continue;
                    const size_t j = (size_t)q[1] * w + q[0];
                    if (lab[j] < 0 && fam(j) == f) {
                        lab[j] = cur;
                        st.push_back((int)j);
                    }
                }
            }
            ++cur;
        }
    std::vector<uint8_t> C(cells);
    for (size_t i = 0; i < cells; ++i) C[i] = (uint8_t)(lab[i] & 0xFF);
    return C;
}

std::vector<uint8_t> comp_wrap(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> o;
    o.reserve(raw.size() + 20);
    puts(o, "PMOC");
    const int32_t n = (int32_t)raw.size();
    put32(o, (uint32_t)n);
    put32(o, 1u);
    put32(o, (uint32_t)-n);
    put32(o, (uint32_t)-n);
    o.insert(o.end(), raw.begin(), raw.end());
    return o;
}

namespace {
enum Mini : uint8_t { kNone = 0, kMountainside = 1, kMountainM = 2, kStoneM = 3, kGrassM = 4,
                      kTreeM = 5, kShore = 6, kWaterM = 7 };

uint8_t mini_pixel(const Map& m, const std::vector<uint8_t>& trees, bool with_trees, int x, int y) {
    const float xs = (float)x / 96.0f, ys = (float)y / 96.0f;
    if (xs >= 1.0f || ys >= 1.0f) return kNone;
    const int mx = std::clamp((int)std::floor(m.w * xs), 0, m.w - 1);
    const int my = std::clamp((int)std::floor(m.h * ys), 0, m.h - 1);
    const size_t i = (size_t)my * m.w + mx;
    if (with_trees && i < trees.size() && trees[i]) {
        const int off = mx + (int)std::floor(my / 2.0f) * 2;
        return (my % 2 == 0) ? (off % 3 != 0 ? kTreeM : kGrassM) : (off % 3 == 1 ? kTreeM : kGrassM);
    }
    auto at = [&](int cx, int cy) { return m.A[(size_t)cy * m.w + cx]; };
    auto quad = [&](int cx, int cy, uint8_t t) {   // the cell, above, left, above-left
        const int ux = cx == 0 ? 0 : cx - 1, uy = cy == 0 ? 0 : cy - 1;
        return at(cx, cy) == t && at(cx, uy) == t && at(ux, cy) == t && at(ux, uy) == t;
    };
    auto from_adj = [&](uint8_t t, uint8_t centre, uint8_t border) -> uint8_t {
        if (!quad(mx, my, t)) return kStoneM;
        const bool in = quad(mx, my == 0 ? 0 : my - 1, t) && quad(mx == 0 ? 0 : mx - 1, my, t) &&
                        quad(mx == m.w - 1 ? m.w - 1 : mx + 1, my, t) &&
                        quad(mx, my == m.h - 1 ? m.h - 1 : my + 1, t);
        return in ? centre : border;
    };
    switch (m.A[i]) {
    case kGrass: return quad(mx, my, kGrass) ? kGrassM : kStoneM;
    case kWater: return from_adj(kWater, kWaterM, kShore);
    case kMountain: return from_adj(kMountain, kMountainM, kMountainside);
    default: return kStoneM;
    }
}
}  // namespace

std::vector<uint8_t> minimap(const Map& m, const std::vector<uint8_t>& trees, bool with_trees,
                             bool tiled) {
    std::vector<uint8_t> o;
    o.reserve(0x18 + 0x20 + 0x2000 + 0x10);
    puts(o, "RGCN");
    put16(o, 0xFEFF);
    put16(o, 0x0101);
    put32(o, 0x18 + 0x20 + 0x2000 + 0x10);   // file size
    put16(o, 0x10);
    put16(o, 0x2);
    puts(o, "RAHC");
    put32(o, 0x2020);
    put16(o, 0x10);
    put16(o, 0x10);
    put32(o, 3);                 // 4bpp
    put32(o, 0);
    put32(o, tiled ? 0u : 1u);   // 1: linear (the NCBR)
    put32(o, 0x80 * 0x80 / 2);
    put32(o, 0x18);
    std::vector<uint8_t> px(128 * 128);
    if (m.w > 0 && m.h > 0 && m.A.size() == (size_t)m.w * m.h)
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) px[(size_t)y * 128 + x] = mini_pixel(m, trees, with_trees, x, y);
    auto pair = [&](int x, int y) { return (uint8_t)(px[(size_t)y * 128 + x] | (px[(size_t)y * 128 + x + 1] << 4)); };
    if (tiled) {
        for (int t = 0; t < 256; ++t)
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; c += 2) o.push_back(pair((t % 16) * 8 + c, (t / 16) * 8 + r));
    } else {
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; x += 2) o.push_back(pair(x, y));
    }
    puts(o, "SOPC");
    put32(o, 0x10);
    put32(o, 0);
    put32(o, 0x100010);
    return o;
}

}  // namespace lvl
