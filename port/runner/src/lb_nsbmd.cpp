// A small software renderer for NSBMD (BMD0) models: see lb_nsbmd.h.
//
// The layout is the Nitro G3D one (docs/51 for the header identities):
//   MDL0  dictionary -> model: +0x04 SBC, +0x08 materials, +0x0C shapes,
//         +0x10 envelope matrices, +0x17 node count, +0x1C/+0x20 the position
//         scale pair, +0x40 the node dictionary (each node a flagged SRT)
//   TEX0  +0x0E texture dictionary, +0x14 texel data, +0x24/+0x28 4x4 texels
//         and their palette indices, +0x34 palette dictionary, +0x38 palettes
// The geometry is drawn the way the hardware draws it, so nothing about the
// exporter's conventions has to be guessed: the SBC (render bytecode) builds
// the matrix stack, and each shape's display list restores, scales and draws
// from it. The cartridge's models with a position scale apply it inside their
// display lists (a GX SCALE after every MTX_RESTORE), and 12 of the 31 skin
// through NODEMIX; both only come out right done exactly as the GX does them.
// (Checked on K_Dragon: in the rest pose every NODEMIX term, inverse bind x
// joint, is the identity, so its rest pose is its bind pose.)

#include "lb_nsbmd.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

// Bounds-checked little-endian reads: a short or damaged file reads zeros.
struct Rd {
    const uint8_t* d;
    size_t n;
    uint32_t u8(size_t a) const { return a < n ? d[a] : 0u; }
    uint32_t u16(size_t a) const { return u8(a) | (u8(a + 1) << 8); }
    uint32_t u32(size_t a) const { return u16(a) | (u16(a + 2) << 16); }
    int32_t s32(size_t a) const { return (int32_t)u32(a); }
    int16_t s16(size_t a) const { return (int16_t)u16(a); }
};

// A G3D dictionary (docs/51): count at +1, entry header at +6, entries after
// the 4-byte header, `esize` bytes each.
struct Dict {
    unsigned count = 0, esize = 0;
    size_t first = 0;
    size_t entry(unsigned i) const { return first + (size_t)i * esize; }
};
Dict read_dict(const Rd& r, size_t at) {
    Dict d;
    if (at + 8 > r.n) return d;
    const size_t head = at + r.u16(at + 6);
    if (head + 4 > r.n) return d;
    d.count = r.u8(at + 1);
    d.esize = r.u16(head);
    d.first = head + 4;
    if (d.first + (size_t)d.count * d.esize > r.n) d.count = 0;
    return d;
}

// The GX's convention: row vectors, v' = v * M, so M = Local * Parent and a
// translation is row 3.
struct Mtx {
    float m[4][4];
};
Mtx identity() {
    Mtx r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}
Mtx mul(const Mtx& a, const Mtx& b) {
    Mtx r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
// The scale and translate commands multiply on the left, as the GX does.
void scale_left(Mtx& c, float x, float y, float z) {
    for (int j = 0; j < 4; ++j) { c.m[0][j] *= x; c.m[1][j] *= y; c.m[2][j] *= z; }
}
void translate_left(Mtx& c, float x, float y, float z) {
    for (int j = 0; j < 4; ++j) c.m[3][j] += x * c.m[0][j] + y * c.m[1][j] + z * c.m[2][j];
}
// fx32 words: 4x4 (16), 4x3 (12, row 3 the translation) or 3x3 (9).
Mtx read_mtx(const uint32_t* p, int rows, int cols) {
    Mtx r = identity();
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) r.m[i][j] = (float)(int32_t)p[i * cols + j] / 4096.0f;
    return r;
}

struct Vec3 {
    float x, y, z;
};
Vec3 xform(const Vec3& v, const Mtx& m) {
    return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0],
            v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1],
            v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]};
}

// A node's rest-pose matrix, S * R * T. The record (NNSG3dResNodeData): u16
// flags, s16 _00, then translation (3 fx32) unless bit 0, rotation unless bit
// 1, scale (3 fx32 and 3 inverses) unless bit 2. A rotation with bit 3 is the
// compressed "pivot" form: one entry (bits 4-7 pick it) is +-1 (bit 8 negative),
// its row and column are zero, and the other four are A, B, +-B, +-A (bits 9
// and 10 negate the last two). Otherwise _00 and eight s16 follow, row-major.
Mtx node_matrix(const Rd& r, size_t p) {
    const uint32_t flag = r.u16(p);
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t[3] = {0, 0, 0}, s[3] = {1, 1, 1};
    const float m00 = r.s16(p + 2) / 4096.0f;
    p += 4;
    if (!(flag & 1u)) {
        for (int i = 0; i < 3; ++i) t[i] = r.s32(p + 4 * i) / 4096.0f;
        p += 12;
    }
    if (!(flag & 2u)) {
        if (flag & 8u) {
            static const int kRest[9][4] = {{4, 5, 7, 8}, {3, 5, 6, 8}, {3, 4, 6, 7}, {1, 2, 7, 8}, {0, 2, 6, 8},
                                            {0, 1, 6, 7}, {1, 2, 4, 5}, {0, 2, 3, 5}, {0, 1, 3, 4}};
            const unsigned piv = (flag >> 4) & 15u;
            const float a = r.s16(p) / 4096.0f, b = r.s16(p + 2) / 4096.0f;
            p += 4;
            if (piv < 9) {
                std::fill(rot, rot + 9, 0.0f);
                rot[piv] = (flag & 0x100u) ? -1.0f : 1.0f;
                rot[kRest[piv][0]] = a;
                rot[kRest[piv][1]] = b;
                rot[kRest[piv][2]] = (flag & 0x200u) ? -b : b;
                rot[kRest[piv][3]] = (flag & 0x400u) ? -a : a;
            }
        } else {
            rot[0] = m00;
            for (int i = 1; i < 9; ++i) rot[i] = r.s16(p + 2 * (i - 1)) / 4096.0f;
            p += 16;
        }
    }
    if (!(flag & 4u))
        for (int i = 0; i < 3; ++i) s[i] = r.s32(p + 4 * i) / 4096.0f;
    Mtx m = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.m[i][j] = s[i] * rot[i * 3 + j];
    for (int j = 0; j < 3; ++j) m.m[3][j] = t[j];
    return m;
}

// A decoded texture with the material's wrap bits.
struct Tex {
    int w = 0, h = 0;
    bool rep_s = false, rep_t = false, flip_s = false, flip_t = false;
    std::vector<uint32_t> px;   // 0xAABBGGRR
};

uint32_t rgb555(uint32_t c, uint32_t a8 = 255) {
    auto chn = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return (a8 << 24) | (chn((c >> 10) & 31) << 16) | (chn((c >> 5) & 31) << 8) | chn(c & 31);
}

// texImageParam: offset (<< 3) bits 0-15, size bits 20-22/23-25, format 26-28,
// colour 0 transparent bit 29. pal is the byte address of the palette (with
// the team bank already added), pal_end its limit.
void decode_texture(const Rd& r, size_t t0, uint32_t param, size_t pal, size_t pal_end, Tex& out) {
    const int fmt = (int)((param >> 26) & 7);
    const int w = 8 << ((param >> 20) & 7), h = 8 << ((param >> 23) & 7);
    const bool clear0 = (param >> 29) & 1u;
    const size_t off = (size_t)(param & 0xFFFF) << 3;
    out.w = w;
    out.h = h;
    out.px.assign((size_t)w * h, 0u);
    if (fmt == 0) return;
    auto colour = [&](size_t i, uint32_t a8 = 255) -> uint32_t {
        const size_t at = pal + i * 2;
        return at + 2 <= pal_end ? rgb555(r.u16(at), a8) : 0u;
    };
    if (fmt == 5) {
        // 4x4 compressed: per 4x4 block, four rows of 2-bit texels, and a u16
        // in the index store (half the texel offset): palette offset in 4-byte
        // units (bits 0-13), mode (14-15).
        const size_t texels = t0 + r.u32(t0 + 0x24) + off, idx = t0 + r.u32(t0 + 0x28) + off / 2;
        const int bw = w / 4;
        for (int by = 0; by < h / 4; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const size_t blk = (size_t)by * bw + bx;
                const uint32_t bits = r.u32(texels + blk * 4), pi = r.u16(idx + blk * 2);
                const size_t base = pal + (size_t)(pi & 0x3FFF) * 4;
                const int mode = (int)(pi >> 14);
                auto c = [&](int k) { return base + k * 2 + 2 <= pal_end ? r.u16(base + k * 2) : 0u; };
                auto mix = [](uint32_t a, uint32_t b, int wa, int wb) {
                    uint32_t o = 0;
                    for (int s = 0; s < 15; s += 5)
                        o |= ((((a >> s) & 31) * wa + ((b >> s) & 31) * wb) / (wa + wb)) << s;
                    return o;
                };
                uint32_t cs[4] = {rgb555(c(0)), rgb555(c(1)), 0, 0};
                switch (mode) {
                    case 0: cs[2] = rgb555(c(2)); break;
                    case 1: cs[2] = rgb555(mix(c(0), c(1), 1, 1)); break;
                    case 2: cs[2] = rgb555(c(2)); cs[3] = rgb555(c(3)); break;
                    default: cs[2] = rgb555(mix(c(0), c(1), 5, 3)); cs[3] = rgb555(mix(c(0), c(1), 3, 5)); break;
                }
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        out.px[(size_t)(by * 4 + y) * w + bx * 4 + x] = cs[(bits >> ((y * 4 + x) * 2)) & 3];
            }
        return;
    }
    const size_t img = t0 + r.u32(t0 + 0x14) + off;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            uint32_t c = 0;
            switch (fmt) {
                case 1: { const uint32_t v = r.u8(img + i), a = v >> 5; c = colour(v & 31, (a * 4 + a / 2) * 255 / 31); break; }
                case 2: { const uint32_t v = (r.u8(img + i / 4) >> ((i % 4) * 2)) & 3; c = (v || !clear0) ? colour(v) : 0; break; }
                case 3: { const uint32_t v = (r.u8(img + i / 2) >> ((i % 2) * 4)) & 15; c = (v || !clear0) ? colour(v) : 0; break; }
                case 4: { const uint32_t v = r.u8(img + i); c = (v || !clear0) ? colour(v) : 0; break; }
                case 6: { const uint32_t v = r.u8(img + i); c = colour(v & 7, (v >> 3) * 255 / 31); break; }
                case 7: { const uint32_t v = r.u16(img + i * 2); c = (v & 0x8000u) ? rgb555(v) : 0; break; }
                default: break;
            }
            out.px[i] = c;
        }
}

struct Vert {
    Vec3 p;
    float s, t;   // texels
};
struct Tri {
    Vert v[3];
    int mat;
};
struct Mat {
    int tex = -1;           // index into the decoded textures, -1 untextured
    uint32_t poly = 0;      // polygon attributes: bit 6 back, bit 7 front, alpha 16-20
    uint32_t colour = 0xFFFFFFFFu;
};

// GX parameter counts per command (0x00..0x72), -1 unknown.
int gx_params(uint32_t c) {
    switch (c) {
        case 0x00: case 0x11: case 0x15: case 0x41: return 0;
        case 0x10: case 0x12: case 0x13: case 0x14: case 0x20: case 0x21: case 0x22: case 0x24: case 0x25:
        case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x30: case 0x31: case 0x32:
        case 0x33: case 0x40: case 0x50: case 0x60: case 0x72: return 1;
        case 0x23: case 0x71: return 2;
        case 0x1B: case 0x1C: case 0x70: return 3;
        case 0x1A: return 9;
        case 0x17: case 0x19: return 12;
        case 0x16: case 0x18: return 16;
        case 0x34: return 32;
        default: return -1;
    }
}

// Runs one shape's display list from the current matrix and stack, appending
// its polygons (positions already in model space).
void run_display_list(const Rd& r, size_t dl, size_t size, Mtx cur, Mtx* stack, int mat, std::vector<Tri>& out) {
    const size_t end = std::min(dl + size, r.n);
    Vec3 v{0, 0, 0};
    float s = 0, t = 0;
    int prim = 0;
    std::vector<Vert> strip;
    auto emit = [&](const Vert& a, const Vert& b, const Vert& c) { out.push_back({{a, b, c}, mat}); };
    auto vertex = [&]() {
        strip.push_back({xform(v, cur), s, t});
        const size_t k = strip.size();
        switch (prim) {
            case 0: if (k % 3 == 0) emit(strip[k - 3], strip[k - 2], strip[k - 1]); break;
            case 1: if (k % 4 == 0) { emit(strip[k - 4], strip[k - 3], strip[k - 2]); emit(strip[k - 4], strip[k - 2], strip[k - 1]); } break;
            case 2:   // every other strip triangle is reversed, to keep one winding
                if (k >= 3) {
                    if (k % 2) emit(strip[k - 3], strip[k - 2], strip[k - 1]);
                    else emit(strip[k - 2], strip[k - 3], strip[k - 1]);
                }
                break;
            default:   // quad strip: v0 v1 v3 v2 make each quad
                if (k >= 4 && k % 2 == 0) {
                    emit(strip[k - 4], strip[k - 3], strip[k - 1]);
                    emit(strip[k - 4], strip[k - 1], strip[k - 2]);
                }
                break;
        }
    };
    auto s10 = [](uint32_t x) { return (int32_t)(x << 22) >> 22; };
    size_t pc = dl;
    while (pc + 4 <= end) {
        const uint32_t packed = r.u32(pc);
        pc += 4;
        for (int k = 0; k < 4; ++k) {
            const uint32_t c = (packed >> (8 * k)) & 0xFF;
            const int np = gx_params(c);
            if (np < 0) return;   // not a display list we understand
            uint32_t p[32];
            for (int i = 0; i < np; ++i) p[i] = r.u32(pc + 4 * (size_t)i);
            pc += 4 * (size_t)np;
            switch (c) {
                case 0x13: stack[p[0] & 31] = cur; break;
                case 0x14: cur = stack[p[0] & 31]; break;
                case 0x15: cur = identity(); break;
                case 0x16: cur = read_mtx(p, 4, 4); break;
                case 0x17: cur = read_mtx(p, 4, 3); break;
                case 0x18: cur = mul(read_mtx(p, 4, 4), cur); break;
                case 0x19: cur = mul(read_mtx(p, 4, 3), cur); break;
                case 0x1A: cur = mul(read_mtx(p, 3, 3), cur); break;
                case 0x1B: scale_left(cur, (int32_t)p[0] / 4096.0f, (int32_t)p[1] / 4096.0f, (int32_t)p[2] / 4096.0f); break;
                case 0x1C: translate_left(cur, (int32_t)p[0] / 4096.0f, (int32_t)p[1] / 4096.0f, (int32_t)p[2] / 4096.0f); break;
                case 0x22: s = (int16_t)(p[0] & 0xFFFF) / 16.0f; t = (int16_t)(p[0] >> 16) / 16.0f; break;
                case 0x23: v = {(int16_t)(p[0] & 0xFFFF) / 4096.0f, (int16_t)(p[0] >> 16) / 4096.0f, (int16_t)(p[1] & 0xFFFF) / 4096.0f}; vertex(); break;
                case 0x24: v = {s10(p[0]) / 64.0f, s10(p[0] >> 10) / 64.0f, s10(p[0] >> 20) / 64.0f}; vertex(); break;
                case 0x25: v.x = (int16_t)(p[0] & 0xFFFF) / 4096.0f; v.y = (int16_t)(p[0] >> 16) / 4096.0f; vertex(); break;
                case 0x26: v.x = (int16_t)(p[0] & 0xFFFF) / 4096.0f; v.z = (int16_t)(p[0] >> 16) / 4096.0f; vertex(); break;
                case 0x27: v.y = (int16_t)(p[0] & 0xFFFF) / 4096.0f; v.z = (int16_t)(p[0] >> 16) / 4096.0f; vertex(); break;
                case 0x28: v.x += s10(p[0]) / 4096.0f; v.y += s10(p[0] >> 10) / 4096.0f; v.z += s10(p[0] >> 20) / 4096.0f; vertex(); break;
                case 0x40: prim = (int)(p[0] & 3); strip.clear(); break;
                case 0x41: strip.clear(); break;
                default: break;   // colours, normals, attributes: not used by this renderer
            }
        }
    }
}

struct Model {
    std::vector<Tri> tris;
    std::vector<Mat> mats;
    std::vector<Tex> texs;
};

bool load_model(const Rd& r, int bank, Model& M) {
    if (r.n < 0x14 || std::memcmp(r.d, "BMD0", 4) != 0) return false;
    size_t mdl0 = 0, tex0 = 0;
    for (uint32_t i = 0, nb = r.u16(0x0E); i < nb && i < 8; ++i) {
        const size_t o = r.u32(0x10 + 4 * i);
        if (o + 8 > r.n) continue;
        if (std::memcmp(r.d + o, "MDL0", 4) == 0) mdl0 = o;
        if (std::memcmp(r.d + o, "TEX0", 4) == 0) tex0 = o;
    }
    if (!mdl0) return false;
    const Dict models = read_dict(r, mdl0 + 8);
    if (!models.count) return false;
    const size_t mo = mdl0 + r.u32(models.entry(0));
    const size_t sbc = mo + r.u32(mo + 4), matb = mo + r.u32(mo + 8), shpb = mo + r.u32(mo + 12);
    const size_t evp = mo + r.u32(mo + 0x10), mend = mo + r.u32(mo);
    const unsigned nnode = r.u8(mo + 0x17);
    const float pos_scale = r.s32(mo + 0x1C) / 4096.0f, inv_scale = r.s32(mo + 0x20) / 4096.0f;
    if (mo >= r.n || sbc >= r.n || matb >= r.n || shpb >= r.n) return false;

    // Textures: each TEX0 texture decoded once per palette it is bound with.
    // A material's texture and palette come from the two binding dictionaries
    // at the head of the material block (entry: u16 offset to a list of
    // material ids, u8 count).
    const Dict mats = read_dict(r, matb + 4);
    M.mats.assign(mats.count, Mat{});
    std::vector<int> mat_tex(mats.count, -1), mat_pal(mats.count, -1);
    auto bind = [&](size_t dict_at, std::vector<int>& to) {
        const Dict d = read_dict(r, dict_at);
        for (unsigned i = 0; i < d.count; ++i) {
            const size_t e = d.entry(i), list = matb + r.u16(e);
            for (unsigned k = 0, n = r.u8(e + 2); k < n; ++k) {
                const unsigned m = r.u8(list + k);
                if (m < to.size()) to[m] = (int)i;
            }
        }
    };
    bind(matb + r.u16(matb), mat_tex);
    bind(matb + r.u16(matb + 2), mat_pal);
    Dict texd, pald;
    size_t pal_data = 0, pal_end = 0;
    if (tex0) {
        texd = read_dict(r, tex0 + r.u16(tex0 + 0x0E));
        pald = read_dict(r, tex0 + r.u32(tex0 + 0x34));
        pal_data = tex0 + r.u32(tex0 + 0x38);
        pal_end = std::min(r.n, pal_data + ((size_t)r.u32(tex0 + 0x30) << 3));
    }
    for (unsigned m = 0; m < mats.count; ++m) {
        const size_t a = matb + r.u32(mats.entry(m));
        Mat& mt = M.mats[m];
        mt.poly = r.u32(a + 0x0C);
        const uint32_t tip = r.u32(a + 0x14);
        if (mat_tex[m] < 0 || mat_tex[m] >= (int)texd.count) continue;
        const uint32_t param = r.u32(texd.entry((unsigned)mat_tex[m]));
        size_t pal = pal_data;
        if (mat_pal[m] >= 0 && mat_pal[m] < (int)pald.count) pal += (size_t)r.u16(pald.entry((unsigned)mat_pal[m])) << 3;
        // The team colour: Building_Draw3D adds 2 per colour slot to the
        // material's palette offset in 16-byte units (docs/412, docs/437), so
        // the palette moves on by bank * 16 colours.
        const size_t banked = pal + (size_t)std::max(bank, 0) * 32u;
        if (banked + 32 <= pal_end) pal = banked;
        Tex tx;
        decode_texture(r, tex0, param, pal, pal_end, tx);
        tx.rep_s = (tip >> 16) & 1u;
        tx.rep_t = (tip >> 17) & 1u;
        tx.flip_s = (tip >> 18) & 1u;
        tx.flip_t = (tip >> 19) & 1u;
        mt.tex = (int)M.texs.size();
        M.texs.push_back(std::move(tx));
    }

    // The rest pose: the SBC run against a 32-entry matrix stack.
    const Dict nodes = read_dict(r, mo + 0x40);
    const Dict shps = read_dict(r, shpb);
    Mtx stack[32];
    for (Mtx& m : stack) m = identity();
    Mtx cur = identity();
    std::vector<bool> visible(256, true);
    int node = 0, mat = 0;
    auto local = [&](unsigned id) {
        return id < nodes.count ? node_matrix(r, mo + 0x40 + r.u32(nodes.entry(id))) : identity();
    };
    // NODEMIX's inverse bind matrices: per node a 4x3 (then a 3x3 for normals).
    auto inv_bind = [&](unsigned id) {
        const size_t a = evp + (size_t)id * 84u;
        if (id >= nnode || a + 48 > mend) return identity();
        uint32_t p[12];
        for (int i = 0; i < 12; ++i) p[i] = r.u32(a + 4 * (size_t)i);
        return read_mtx(p, 4, 3);
    };
    size_t pc = sbc;
    for (int guard = 0; pc < matb && guard < 4096; ++guard) {
        const uint32_t op = r.u8(pc), base = op & 0x1F, var = op >> 5;
        if (base == 0x00) { ++pc; continue; }
        if (base == 0x01) break;   // RET
        switch (base) {
            case 0x02: node = (int)r.u8(pc + 1); visible[node] = (r.u8(pc + 2) & 1u) != 0; pc += 3; break;
            case 0x03: cur = stack[r.u8(pc + 1) & 31]; pc += 2; break;
            case 0x04: mat = (int)r.u8(pc + 1); pc += 2; break;
            case 0x05: {
                const unsigned id = r.u8(pc + 1);
                pc += 2;
                if (!visible[node] || id >= shps.count) break;
                const size_t sh = shpb + r.u32(shps.entry(id));
                run_display_list(r, sh + r.u32(sh + 8), r.u32(sh + 12), cur, stack, mat, M.tris);
                break;
            }
            case 0x06:   // NODEDESC node, parent, flags [, store] [, restore]
            case 0x07:   // BB / BBY: the node's matrix without the billboard turn
            case 0x08: {
                const unsigned id = r.u8(pc + 1);
                size_t p = pc + (base == 0x06 ? 4 : 2);
                int dst = -1, src = -1;
                if (var == 1) dst = (int)r.u8(p++);
                if (var == 2) src = (int)r.u8(p++);
                if (var == 3) { dst = (int)r.u8(p); src = (int)r.u8(p + 1); p += 2; }
                if (src >= 0) cur = stack[src & 31];
                if (base == 0x06) cur = mul(local(id), cur);
                if (dst >= 0) stack[dst & 31] = cur;
                pc = p;
                break;
            }
            case 0x09: {   // NODEMIX dest, count, (stack slot, node, weight/256) x count
                const unsigned dst = r.u8(pc + 1), cnt = r.u8(pc + 2);
                Mtx acc{};
                for (unsigned i = 0; i < cnt; ++i) {
                    const size_t e = pc + 3 + 3 * (size_t)i;
                    const Mtx m = mul(inv_bind(r.u8(e + 1)), stack[r.u8(e) & 31]);
                    const float w = r.u8(e + 2) / 256.0f;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) acc.m[y][x] += w * m.m[y][x];
                }
                stack[dst & 31] = cur = acc;
                pc += 3 + 3 * (size_t)cnt;
                break;
            }
            case 0x0A: pc += 9; break;   // CALLDL: not used by the cartridge's models
            case 0x0B: { const float k = var ? inv_scale : pos_scale; scale_left(cur, k, k, k); ++pc; break; }
            case 0x0C: case 0x0D: pc += 3; break;   // ENVMAP / PRJMAP
            default: pc = matb; break;              // unknown: stop
        }
    }
    return !M.tris.empty();
}

// Nearest texel with the material's repeat / flip / clamp.
uint32_t sample(const Tex& t, float s, float u) {
    auto wrap = [](int c, int n, bool rep, bool flip) {
        if (!rep) return std::min(std::max(c, 0), n - 1);
        if (!flip) return ((c % n) + n) % n;
        const int m = ((c % (2 * n)) + 2 * n) % (2 * n);
        return m < n ? m : 2 * n - 1 - m;
    };
    const int x = wrap((int)std::floor(s), t.w, t.rep_s, t.flip_s);
    const int y = wrap((int)std::floor(u), t.h, t.rep_t, t.flip_t);
    return t.px[(size_t)y * t.w + x];
}

}  // namespace

// SG-0064: the model in the teal team colour. As teal_models_paint does in the
// game (docs/412), bank 5 of every palette becomes a copy of bank 6 with
// entries 12-14 set to `shades` (three BGR555 words), then bank 5 is drawn.
bool lb_nsbmd_render_teal(const uint8_t* bmd, size_t n, const uint16_t shades[3], float yaw, float pitch, int w,
                          int h, uint32_t* out) {
    if (!bmd || !shades || n < 0x14 || std::memcmp(bmd, "BMD0", 4) != 0)
        return lb_nsbmd_render(bmd, n, 5, yaw, pitch, w, h, out);
    std::vector<uint8_t> copy(bmd, bmd + n);
    const Rd r{copy.data(), copy.size()};
    size_t tex0 = 0;
    for (uint32_t i = 0, nb = r.u16(0x0E); i < nb && i < 8; ++i) {
        const size_t o = r.u32(0x10 + 4 * i);
        if (o + 8 <= r.n && std::memcmp(r.d + o, "TEX0", 4) == 0) tex0 = o;
    }
    if (tex0) {
        const Dict pald = read_dict(r, tex0 + r.u32(tex0 + 0x34));
        const size_t pal_data = tex0 + r.u32(tex0 + 0x38);
        const size_t pal_end = std::min(r.n, pal_data + ((size_t)r.u32(tex0 + 0x30) << 3));
        for (unsigned i = 0; i < pald.count; ++i) {
            const size_t base = pal_data + ((size_t)r.u16(pald.entry(i)) << 3);
            if (base + 7 * 32 > pal_end) continue;   // no bank 6: leave this palette alone
            std::memcpy(copy.data() + base + 5 * 32, copy.data() + base + 6 * 32, 32);
            // BR-0082 (docs/485): a re-skinned custom model (encode_model_texture)
            // keeps its team pixels at the picture's own entries, each holding
            // that bank's team shade, not only at 12-14. Every entry of bank 6
            // that differs from Red's bank (1) and equals one of bank 6's team
            // shades takes the matching teal shade, so it is not left grey.
            auto at16 = [&](size_t o) { return (uint16_t)(copy[o] | (copy[o + 1] << 8)); };
            uint16_t grey[3];
            for (int k = 0; k < 3; ++k) grey[k] = at16(base + 6 * 32 + (size_t)(12 + k) * 2);
            for (size_t j = 0; j < 16; ++j) {
                uint16_t v = at16(base + 6 * 32 + j * 2);
                if (j >= 12 && j <= 14) v = shades[j - 12];
                else if (v != at16(base + 1 * 32 + j * 2))
                    for (int k = 0; k < 3; ++k)
                        if (v == grey[k]) { v = shades[k]; break; }
                copy[base + 5 * 32 + j * 2] = (uint8_t)v;
                copy[base + 5 * 32 + j * 2 + 1] = (uint8_t)(v >> 8);
            }
        }
    }
    return lb_nsbmd_render(copy.data(), copy.size(), 5, yaw, pitch, w, h, out);
}

bool lb_nsbmd_render(const uint8_t* bmd, size_t n, int bank, float yaw, float pitch, int w, int h,
                     uint32_t* out) {
    if (!out || w <= 0 || h <= 0) return false;
    std::fill(out, out + (size_t)w * h, 0u);
    if (!bmd) return false;
    Model M;
    if (!load_model(Rd{bmd, n}, bank, M)) return false;

    // Fit: centre on the bounding box; a cylinder around the vertical axis
    // bounds every yaw, so the size holds still while the model turns.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Tri& t : M.tris)
        for (const Vert& v : t.v) {
            lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y), std::min(lo.z, v.p.z)};
            hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y), std::max(hi.z, v.p.z)};
        }
    const Vec3 c{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
    float rxz = 1e-6f;
    for (const Tri& t : M.tris)
        for (const Vert& v : t.v) rxz = std::max(rxz, std::hypot(v.p.x - c.x, v.p.z - c.z));
    const float hy = (hi.y - lo.y) / 2, cp = std::cos(pitch), sp = std::sin(pitch);
    const float half_h = std::max(hy * std::fabs(cp) + rxz * std::fabs(sp), 1e-6f);
    const float k = 0.95f * std::min(w / (2 * rxz), h / (2 * half_h));
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    auto view = [&](const Vec3& p) {
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        const float x1 = dx * cy + dz * sy, z1 = -dx * sy + dz * cy;
        const float y2 = dy * cp - z1 * sp, z2 = dy * sp + z1 * cp;
        return Vec3{w * 0.5f + x1 * k, h * 0.5f - y2 * k, z2};   // z: larger is nearer
    };

    std::vector<float> zb((size_t)w * h, -1e30f);
    const float L[3] = {-0.35f, 0.55f, 0.76f};   // upper left, towards the viewer
    for (const Tri& t : M.tris) {
        const Mat& mt = M.mats.empty() ? Mat{} : M.mats[std::min<size_t>((size_t)std::max(t.mat, 0), M.mats.size() - 1)];
        const Vec3 a = view(t.v[0].p), b = view(t.v[1].p), d = view(t.v[2].p);
        const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        if (std::fabs(area) < 1e-9f) continue;
        // The material's front/back bits (6 back, 7 front). Front faces wind
        // anticlockwise on screen here (y down: negative area); measured, as
        // that winding is what makes the closed hulls draw their outsides.
        // The cartridge's materials are all front-only, which leaves one-sided
        // sails, far wheels and wings invisible from behind as the preview
        // turns, so a culled face is still drawn, but pushed back by `bias` so
        // any front face at the same depth wins over it.
        const bool front = area < 0;
        const float bias = (front ? mt.poly & 0x80u : mt.poly & 0x40u) ? 0.0f : 0.02f * rxz;
        // A mild flat shade from the face normal: the cartridge's materials
        // light to saturation (emission 28 + ambient 18), so the game shows
        // the texture flat; this only keeps the shape readable.
        const float ux = b.x - a.x, uy = -(b.y - a.y), uz = b.z - a.z;
        const float vx = d.x - a.x, vy = -(d.y - a.y), vz = d.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        float shade = 1.0f;
        if (nl > 0) shade = 0.72f + 0.28f * std::fabs((nx * L[0] + ny * L[1] + nz * L[2]) / nl);
        const int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, d.x})));
        const int x1 = std::min(w - 1, (int)std::ceil(std::max({a.x, b.x, d.x})));
        const int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, d.y})));
        const int y1 = std::min(h - 1, (int)std::ceil(std::max({a.y, b.y, d.y})));
        const Tex* tx = mt.tex >= 0 ? &M.texs[(size_t)mt.tex] : nullptr;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                float w0 = (b.x - px) * (d.y - py) - (b.y - py) * (d.x - px);
                float w1 = (d.x - px) * (a.y - py) - (d.y - py) * (a.x - px);
                float w2 = (a.x - px) * (b.y - py) - (a.y - py) * (b.x - px);
                w0 /= area; w1 /= area; w2 /= area;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = w0 * a.z + w1 * b.z + w2 * d.z - bias;
                float& zz = zb[(size_t)y * w + x];
                if (z <= zz) continue;
                uint32_t col = mt.colour;
                if (tx) {
                    const float s = w0 * t.v[0].s + w1 * t.v[1].s + w2 * t.v[2].s;
                    const float u = w0 * t.v[0].t + w1 * t.v[1].t + w2 * t.v[2].t;
                    col = sample(*tx, s, u);
                }
                if ((col >> 24) < 128) continue;   // cutout alpha
                zz = z;
                auto ch = [&](int sh) { return (uint32_t)std::min(255.0f, ((col >> sh) & 255) * shade) << sh; };
                out[(size_t)y * w + x] = 0xFF000000u | ch(16) | ch(8) | ch(0);
            }
    }
    return true;
}
