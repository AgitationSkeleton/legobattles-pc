// legobattles-decomp: xdelta patches of the player's own ROM (docs/474).
//
// Players share custom maps and units made in the port (mods/maps, mods/units).
// This file turns the cartridge the port builds into an xdelta patch of the
// ROM in rom/, and an xdelta patch someone else made back into port mods.
//
// THE FORMAT is VCDIFF (RFC 3284) as xdelta3 writes it, so xdelta3, xdeltaUI
// and DeltaPatcher read our patches and we read theirs:
//
//   header   D6 C3 C4 00, Hdr_Indicator, [application header]
//   window   Win_Indicator, [source segment length, position],
//            delta length, target length, Delta_Indicator,
//            data / instruction / address section lengths,
//            [adler32 of the target window: xdelta3's bit 0x04],
//            data, instructions, addresses
//
// The default instruction code table (RFC 3284 5.6) and address cache (near 4,
// same 3). Secondary compression (VCD_DECOMPRESS, xdelta3 -S djw/lzma) is
// refused: it is a separate compressor the standard tools also make optional.
//
// THE APPLICATION HEADER. xdelta3 writes "target//source/" there and reads it
// only up to the first NUL (it is a C string to it), to name files nobody
// named. After a NUL the port adds what the ROM cannot carry: each custom
// unit's bundle (the lobby's "LBUB" form: unit.json, its pictures, its sounds;
// lb_cunits.cpp). The units' records and sheets ARE in the patched ROM, but
// their sounds and heads live only in the port, and a bundle is what the
// import installs, so the import gets the very folder the exporter had.
//
//   "LEGO Battles (mods).nds//LEGO Battles.nds/" 00 "LBXP" 01 kind(u8)
//   u16 count, then per unit: u8 folder length, folder, u32 length, bundle

#include "lb_xdelta.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

extern "C" int lb_mods_count(void);                               // lb_mods.cpp
extern "C" const char* lb_mods_name(int k);
extern "C" int lb_cunits_count(void);                             // lb_cunits.cpp
extern "C" const char* lb_cunits_folder(int k);
extern "C" int lb_cunits_hashes(unsigned long long* out, int cap);
bool lb_cunits_bundle(uint64_t hash, std::vector<uint8_t>* out);
extern "C" int lb_settings_file_bool(const char* key, int dflt);   // lb_overlay.cpp

namespace {

namespace fs = std::filesystem;

// ---- VCDIFF primitives ----------------------------------------------------------
enum : uint8_t { kNoop = 0, kAdd = 1, kRun = 2, kCopy = 3 };
struct Code { uint8_t t1, s1, m1, t2, s2, m2; };

// RFC 3284 5.6, built as the RFC lists it.
const Code* code_table() {
    static Code t[256];
    static bool made = false;
    if (!made) {
        int i = 0;
        t[i++] = {kRun, 0, 0, kNoop, 0, 0};
        for (int s = 0; s <= 17; ++s) t[i++] = {kAdd, (uint8_t)s, 0, kNoop, 0, 0};
        for (int m = 0; m < 9; ++m) {
            t[i++] = {kCopy, 0, (uint8_t)m, kNoop, 0, 0};
            for (int s = 4; s <= 18; ++s) t[i++] = {kCopy, (uint8_t)s, (uint8_t)m, kNoop, 0, 0};
        }
        for (int m = 0; m < 6; ++m)
            for (int a = 1; a <= 4; ++a)
                for (int s = 4; s <= 6; ++s) t[i++] = {kAdd, (uint8_t)a, 0, kCopy, (uint8_t)s, (uint8_t)m};
        for (int m = 6; m < 9; ++m)
            for (int a = 1; a <= 4; ++a) t[i++] = {kAdd, (uint8_t)a, 0, kCopy, 4, (uint8_t)m};
        for (int m = 0; m < 9; ++m) t[i++] = {kCopy, 4, (uint8_t)m, kAdd, 1, 0};
        made = true;   // i == 256
    }
    return t;
}

constexpr int kNear = 4, kSame = 3;
struct AddrCache {
    uint64_t near_[kNear];
    uint64_t same_[kSame * 256];
    int next = 0;
    void reset() {
        std::memset(near_, 0, sizeof near_);
        std::memset(same_, 0, sizeof same_);
        next = 0;
    }
    void update(uint64_t a) {
        near_[next] = a;
        next = (next + 1) % kNear;
        same_[a % (kSame * 256)] = a;
    }
};

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

// A bounded reader over one section.
struct In {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    uint8_t byte() {
        if (at >= n) { ok = false; return 0; }
        return p[at++];
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (int i = 0; i < 10; ++i) {
            if (at >= n) { ok = false; return 0; }
            const uint8_t b = p[at++];
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) return v;
        }
        ok = false;
        return 0;
    }
};

void put_varint(std::vector<uint8_t>* o, uint64_t v) {
    uint8_t tmp[10];
    int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7Fu);
    while (v >>= 7) tmp[n++] = (uint8_t)(0x80u | (v & 0x7Fu));
    while (n) o->push_back(tmp[--n]);
}
int varint_len(uint64_t v) {
    int n = 1;
    while (v >>= 7) ++n;
    return n;
}

}  // namespace

// ---- decode -----------------------------------------------------------------------
bool lb_vcdiff_decode(const std::vector<uint8_t>& source, const std::vector<uint8_t>& patch,
                      std::vector<uint8_t>* target, std::vector<uint8_t>* appheader, std::string* why) {
    const Code* table = code_table();
    target->clear();
    if (appheader) appheader->clear();
    if (patch.size() < 5 || patch[0] != 0xD6 || patch[1] != 0xC3 || patch[2] != 0xC4) {
        *why = "not an xdelta / VCDIFF patch";
        return false;
    }
    if (patch[3] != 0) {
        *why = "a VCDIFF version this reader does not know";
        return false;
    }
    In h{patch.data(), patch.size(), 4};
    const uint8_t hdr = h.byte();
    if (hdr & 0x01) {
        *why = "the patch uses secondary compression (xdelta3's -S djw or lzma), which is not supported; "
               "re-create the patch with xdelta3 -S none";
        return false;
    }
    if (hdr & 0x02) {
        *why = "the patch uses its own instruction code table, which is not supported";
        return false;
    }
    if (hdr & 0xF8) {
        *why = "the patch header has flags this reader does not know";
        return false;
    }
    if (hdr & 0x04) {
        const uint64_t n = h.varint();
        if (!h.ok || n > h.n - h.at) { *why = "the patch's application header is cut short"; return false; }
        if (appheader) appheader->assign(patch.begin() + (long)h.at, patch.begin() + (long)(h.at + n));
        h.at += (size_t)n;
    }
    AddrCache cache;
    size_t windows = 0;
    while (h.at < h.n) {
        ++windows;
        char where[48];
        std::snprintf(where, sizeof where, " (window %zu)", windows);
        const uint8_t win = h.byte();
        if ((win & 0xF8) || (win & 3) == 3) { *why = std::string("a window with unknown flags") + where; return false; }
        uint64_t seg_len = 0, seg_pos = 0;
        if (win & 3) {
            seg_len = h.varint();
            seg_pos = h.varint();
            if (!h.ok) { *why = std::string("the patch is cut short") + where; return false; }
            const uint64_t lim = (win & 1) ? source.size() : target->size();
            if (seg_pos > lim || seg_len > lim - seg_pos) {
                *why = (win & 1) ? std::string("the patch reads past the end of the ROM: it was made for a "
                                               "different (larger) ROM") + where
                                 : std::string("the patch reads past its own output") + where;
                return false;
            }
        }
        const uint64_t enc_len = h.varint();
        const size_t enc_start = h.at;
        const uint64_t tlen = h.varint();
        const uint8_t del = h.byte();
        const uint64_t dlen = h.varint(), ilen = h.varint(), alen = h.varint();
        uint32_t sum = 0;
        if (win & 4) {
            for (int i = 0; i < 4; ++i) sum = (sum << 8) | h.byte();
        }
        if (!h.ok) { *why = std::string("the patch is cut short") + where; return false; }
        if (del != 0) {
            *why = "the patch uses secondary compression (xdelta3's -S djw or lzma), which is not supported; "
                   "re-create the patch with xdelta3 -S none";
            return false;
        }
        if (tlen > (256u << 20) || dlen > h.n - h.at || ilen > h.n - h.at - dlen || alen > h.n - h.at - dlen - ilen) {
            *why = std::string("a window's sections do not fit the patch") + where;
            return false;
        }
        if (enc_len != (h.at - enc_start) + dlen + ilen + alen) {
            *why = std::string("a window's length does not add up") + where;
            return false;
        }
        In data{patch.data() + h.at, (size_t)dlen};
        In inst{patch.data() + h.at + dlen, (size_t)ilen};
        In addr{patch.data() + h.at + dlen + ilen, (size_t)alen};
        h.at += (size_t)(dlen + ilen + alen);
        const size_t tbase = target->size();
        if ((uint64_t)tbase + tlen > (1ull << 31)) { *why = "the patched ROM would be over 2 GB"; return false; }
        target->resize(tbase + (size_t)tlen);
        uint8_t* t = target->data() + tbase;
        const uint8_t* seg = (win & 1) ? source.data() + seg_pos : (win & 2) ? target->data() + seg_pos : nullptr;
        cache.reset();
        uint64_t tp = 0;
        while (inst.at < inst.n) {
            const Code& c = table[inst.byte()];
            const uint8_t ty[2] = {c.t1, c.t2}, sz[2] = {c.s1, c.s2}, md[2] = {c.m1, c.m2};
            for (int k = 0; k < 2; ++k) {
                if (ty[k] == kNoop) continue;
                const uint64_t size = sz[k] ? sz[k] : inst.varint();
                if (!inst.ok || size > tlen - tp) { *why = std::string("an instruction runs past its window") + where; return false; }
                if (ty[k] == kAdd) {
                    if (size > data.n - data.at) { *why = std::string("the data section is cut short") + where; return false; }
                    std::memcpy(t + tp, data.p + data.at, (size_t)size);
                    data.at += (size_t)size;
                } else if (ty[k] == kRun) {
                    const uint8_t b = data.byte();
                    if (!data.ok) { *why = std::string("the data section is cut short") + where; return false; }
                    std::memset(t + tp, b, (size_t)size);
                } else {
                    const uint64_t here = seg_len + tp;
                    const int m = md[k];
                    uint64_t a;
                    if (m == 0) a = addr.varint();
                    else if (m == 1) a = here - addr.varint();
                    else if (m < 2 + kNear) a = cache.near_[m - 2] + addr.varint();
                    else a = cache.same_[(size_t)(m - 2 - kNear) * 256u + addr.byte()];
                    if (!addr.ok || a >= here) { *why = std::string("a copy address is out of range") + where; return false; }
                    cache.update(a);
                    if (a + size <= seg_len) {
                        std::memcpy(t + tp, seg + a, (size_t)size);
                    } else if (a >= seg_len && a - seg_len + size <= tp) {
                        std::memcpy(t + tp, t + (a - seg_len), (size_t)size);
                    } else {   // overlapping, or spanning the segment and the window
                        for (uint64_t i = 0; i < size; ++i) {
                            const uint64_t s = a + i;
                            t[tp + i] = s < seg_len ? seg[s] : t[s - seg_len];
                        }
                    }
                }
                tp += size;
            }
        }
        if (tp != tlen || data.at != data.n || addr.at != addr.n) {
            *why = std::string("a window does not decode to its stated size") + where;
            return false;
        }
        if ((win & 4) && adler32(t, (size_t)tlen) != sum) {
            *why = "the patched data fails its checksum: the patch was made for a different ROM "
                   "(the port keeps the European release in rom/), or it is damaged";
            return false;
        }
    }
    if (!windows) { *why = "the patch holds no data"; return false; }
    return true;
}

// ---- encode -----------------------------------------------------------------------
namespace {

constexpr size_t kBlock = 32;                 // source blocks indexed, target bytes hashed
constexpr size_t kWindow = 8u << 20;          // target bytes per window (xdelta3's default)
constexpr uint64_t kMaxSegment = 32u << 20;   // a window's source segment at most
constexpr uint32_t kPrime = 0x01000193u;
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Op { uint8_t kind; uint64_t a, len; };   // kAdd: a = target offset; kCopy: a = source; kRun: a = target

uint32_t block_hash(const uint8_t* p) {
    uint32_t h = 0;
    for (size_t i = 0; i < kBlock; ++i) h = h * kPrime + p[i];
    return h;
}

// An ADD region, its long runs of one byte made RUNs.
void push_add(std::vector<Op>* ops, const uint8_t* t, uint64_t a, uint64_t len) {
    uint64_t start = a, i = a;
    const uint64_t end = a + len;
    while (i < end) {
        uint64_t j = i + 1;
        while (j < end && t[j] == t[i]) ++j;
        if (j - i >= 24) {
            if (i > start) ops->push_back({kAdd, start, i - start});
            ops->push_back({kRun, i, j - i});
            start = j;
        }
        i = j;
    }
    if (end > start) ops->push_back({kAdd, start, end - start});
}

void emit_window(const std::vector<Op>& pieces, const std::vector<uint8_t>& tgt, uint64_t tstart, uint64_t tlen,
                 std::vector<uint8_t>* out) {
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const Op& p : pieces)
        if (p.kind == kCopy) {
            lo = std::min(lo, p.a);
            hi = std::max(hi, p.a + p.len);
        }
    const bool has_src = lo != UINT64_MAX;
    const uint64_t seg_len = has_src ? hi - lo : 0;
    std::vector<uint8_t> data, inst, addr;
    AddrCache cache;
    cache.reset();
    uint64_t tp = 0;
    for (const Op& p : pieces) {
        if (p.kind == kAdd) {
            if (p.len <= 17) inst.push_back((uint8_t)(1 + p.len));
            else { inst.push_back(1); put_varint(&inst, p.len); }
            data.insert(data.end(), tgt.begin() + (long)p.a, tgt.begin() + (long)(p.a + p.len));
        } else if (p.kind == kRun) {
            inst.push_back(0);
            put_varint(&inst, p.len);
            data.push_back(tgt[(size_t)p.a]);
        } else {
            const uint64_t a = p.a - lo, here = seg_len + tp;
            int mode = 0, best = varint_len(a);
            uint64_t val = a;
            if (varint_len(here - a) < best) { mode = 1; best = varint_len(here - a); val = here - a; }
            for (int i = 0; i < kNear; ++i)
                if (a >= cache.near_[i] && varint_len(a - cache.near_[i]) < best) {
                    mode = 2 + i;
                    best = varint_len(a - cache.near_[i]);
                    val = a - cache.near_[i];
                }
            const size_t si = (size_t)(a % (kSame * 256));
            const bool same = cache.same_[si] == a && best > 1;
            if (same) mode = 2 + kNear + (int)(si / 256);
            if (p.len >= 4 && p.len <= 18) inst.push_back((uint8_t)(19 + 16 * mode + (p.len - 3)));
            else { inst.push_back((uint8_t)(19 + 16 * mode)); put_varint(&inst, p.len); }
            if (same) addr.push_back((uint8_t)(si % 256));
            else put_varint(&addr, val);
            cache.update(a);
        }
        tp += p.len;
    }
    std::vector<uint8_t> body;
    put_varint(&body, tlen);
    body.push_back(0);   // Delta_Indicator: no secondary compression
    put_varint(&body, data.size());
    put_varint(&body, inst.size());
    put_varint(&body, addr.size());
    const uint32_t sum = adler32(tgt.data() + tstart, (size_t)tlen);
    for (int i = 3; i >= 0; --i) body.push_back((uint8_t)(sum >> (8 * i)));
    body.insert(body.end(), data.begin(), data.end());
    body.insert(body.end(), inst.begin(), inst.end());
    body.insert(body.end(), addr.begin(), addr.end());
    out->push_back((uint8_t)((has_src ? 0x01 : 0x00) | 0x04));   // VCD_SOURCE, xdelta3's VCD_ADLER32
    if (has_src) {
        put_varint(out, seg_len);
        put_varint(out, lo);
    }
    put_varint(out, body.size());
    out->insert(out->end(), body.begin(), body.end());
}

}  // namespace

bool lb_vcdiff_encode(const std::vector<uint8_t>& source, const std::vector<uint8_t>& target,
                      const std::vector<uint8_t>& appheader, std::vector<uint8_t>* patch,
                      const std::function<void(double)>& progress) {
    if (source.size() >= kNone || target.size() >= kNone) return false;
    const uint8_t* s = source.data();
    const uint8_t* t = target.data();
    const size_t sn = source.size(), tn = target.size();
    // 1. Index the source: the first position of each aligned block's hash.
    int bits = 16;
    while (bits < 24 && ((size_t)1 << bits) < sn / kBlock * 2) ++bits;
    std::vector<uint32_t> index((size_t)1 << bits, kNone);
    auto slot = [&](uint32_t hh) { return (size_t)((hh * 0x9E3779B1u) >> (32 - bits)); };
    for (size_t p = 0; p + kBlock <= sn; p += kBlock) {
        uint32_t& e = index[slot(block_hash(s + p))];
        if (e == kNone) e = (uint32_t)p;
    }
    // 2. Scan the target with a rolling hash; a hit is checked, then extended
    //    back over the pending ADD bytes and forward as far as it holds.
    uint32_t pow = 1;
    for (size_t i = 1; i < kBlock; ++i) pow *= kPrime;
    std::vector<Op> ops;
    size_t pos = 0, add_from = 0, next_report = 0;
    int64_t last_delta = 0;   // source - target of the last copy
    uint32_t hh = tn >= kBlock ? block_hash(t) : 0;
    while (pos + kBlock <= tn) {
        if (progress && pos >= next_report) {
            progress(0.9 * (double)pos / (double)tn);
            next_report = pos + (4u << 20);
        }
        // A long run of one byte (the padding after the last file) is a RUN:
        // a copy of it from the source stops wherever the source's run does.
        if (t[pos] == t[pos + kBlock - 1]) {
            size_t e = pos + 1;
            while (e < tn && t[e] == t[pos]) ++e;
            if (e - pos >= 256) {
                if (pos > add_from) push_add(&ops, t, add_from, pos - add_from);
                ops.push_back({kRun, pos, e - pos});
                pos = add_from = e;
                if (pos + kBlock <= tn) hh = block_hash(t + pos);
                continue;
            }
        }
        // The candidates: where the last copy would continue (a moved file),
        // the same offset (a file that kept its place), then the index.
        size_t cand = kNone;
        const int64_t ld = (int64_t)pos + last_delta;
        if (ld >= 0 && (uint64_t)ld + kBlock <= sn && std::memcmp(s + ld, t + pos, kBlock) == 0) cand = (size_t)ld;
        else if (pos + kBlock <= sn && std::memcmp(s + pos, t + pos, kBlock) == 0) cand = pos;
        else {
            const uint32_t c = index[slot(hh)];
            if (c != kNone && std::memcmp(s + c, t + pos, kBlock) == 0) cand = c;
        }
        bool hit = false;
        if (cand != kNone) {
            last_delta = (int64_t)cand - (int64_t)pos;
            size_t s0 = cand, t0 = pos;
            while (t0 > add_from && s0 > 0 && s[s0 - 1] == t[t0 - 1]) { --s0; --t0; }
            size_t e = pos + kBlock, se = cand + kBlock;
            while (e + 8 <= tn && se + 8 <= sn && std::memcmp(s + se, t + e, 8) == 0) { e += 8; se += 8; }
            while (e < tn && se < sn && s[se] == t[e]) { ++e; ++se; }
            if (t0 > add_from) push_add(&ops, t, add_from, t0 - add_from);
            ops.push_back({kCopy, s0, e - t0});
            pos = add_from = e;
            if (pos + kBlock <= tn) hh = block_hash(t + pos);
            hit = true;
        }
        if (!hit) {
            if (pos + kBlock >= tn) break;
            hh = (hh - t[pos] * pow) * kPrime + t[pos + kBlock];
            ++pos;
        }
    }
    if (tn > add_from) push_add(&ops, t, add_from, tn - add_from);
    // 3. Windows of at most kWindow target bytes and a kMaxSegment source span.
    patch->assign({0xD6, 0xC3, 0xC4, 0x00});
    patch->push_back(appheader.empty() ? 0x00 : 0x04);
    if (!appheader.empty()) {
        put_varint(patch, appheader.size());
        patch->insert(patch->end(), appheader.begin(), appheader.end());
    }
    std::vector<Op> pieces;
    uint64_t wstart = 0, wlen = 0, lo = UINT64_MAX, hi = 0;
    auto flush = [&]() {
        if (wlen) emit_window(pieces, target, wstart, wlen, patch);
        pieces.clear();
        wstart += wlen;
        wlen = 0;
        lo = UINT64_MAX;
        hi = 0;
    };
    for (size_t i = 0; i < ops.size(); ++i) {
        Op op = ops[i];
        while (op.len) {
            uint64_t take = std::min<uint64_t>(op.len, kWindow - wlen);
            if (op.kind == kCopy) {
                const uint64_t nlo = std::min(lo, op.a);
                const uint64_t nhi = std::max(hi, op.a + take);
                if (nhi - nlo > kMaxSegment) {
                    if (wlen) { flush(); continue; }
                    take = std::min<uint64_t>(take, kMaxSegment);
                }
                lo = std::min(lo, op.a);
                hi = std::max(hi, op.a + take);
            }
            pieces.push_back({op.kind, op.a, take});
            if (op.kind != kRun) op.a += take;
            op.len -= take;
            wlen += take;
            if (wlen == kWindow) flush();
        }
    }
    flush();
    if (tn == 0) emit_window({}, target, 0, 0, patch);   // an empty target is one empty window
    if (progress) progress(1.0);
    return true;
}

// ---- the jobs ---------------------------------------------------------------------
namespace {

struct Job {
    std::atomic<bool> busy{false};
    std::atomic<int> pct{0};
    std::mutex mu;
    std::string msg;
    bool bad = false, installed = false;
    std::vector<uint8_t> patched;   // the last import's ROM
    std::string failed_patch;       // a patch that did not fit rom/ (another release?)
};
Job g_job;

void set_msg(const std::string& m, bool bad, bool installed = false) {
    std::lock_guard<std::mutex> lk(g_job.mu);
    g_job.msg = m;
    g_job.bad = bad;
    g_job.installed = installed;
}

bool read_all(const std::string& path, std::vector<uint8_t>* out, size_t cap) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const auto n = fs::file_size(p, ec);
    if (ec || n == 0 || n > cap) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->resize((size_t)n);
    f.read((char*)out->data(), (std::streamsize)n);
    return (bool)f;
}
bool write_all(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)b.data(), (std::streamsize)b.size());
    return (bool)f;
}

std::string mb(size_t n) {
    char b[32];
    if (n >= (1u << 20)) std::snprintf(b, sizeof b, "%.1f MB", (double)n / (1 << 20));
    else std::snprintf(b, sizeof b, "%.0f KB", (double)n / 1024.0 + 0.5);
    return b;
}

void wr16v(std::vector<uint8_t>* o, uint32_t v) { o->push_back((uint8_t)v); o->push_back((uint8_t)(v >> 8)); }
void wr32v(std::vector<uint8_t>* o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o->push_back((uint8_t)(v >> (8 * i)));
}

// The application header: xdelta3's names, then (after a NUL) the port's
// payload. kind 0: the port's own cartridge, 1: maps in stock slots.
std::vector<uint8_t> make_appheader(int kind, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& units) {
    const std::string names = kind == 0 ? "LEGO Battles (port mods).nds//LEGO Battles (Europe).nds/"
                                        : "LEGO Battles (custom maps).nds//LEGO Battles (Europe).nds/";
    std::vector<uint8_t> a(names.begin(), names.end());
    a.push_back(0);
    a.insert(a.end(), {'L', 'B', 'X', 'P', 1, (uint8_t)kind});
    wr16v(&a, (uint32_t)units.size());
    for (const auto& u : units) {
        const std::string f = u.first.substr(0, 255);
        a.push_back((uint8_t)f.size());
        a.insert(a.end(), f.begin(), f.end());
        wr32v(&a, (uint32_t)u.second.size());
        a.insert(a.end(), u.second.begin(), u.second.end());
    }
    return a;
}

// The payload back out of an application header. False when it has none.
bool read_payload(const std::vector<uint8_t>& a, int* kind,
                  std::vector<std::pair<std::string, std::vector<uint8_t>>>* units) {
    const auto nul = std::find(a.begin(), a.end(), (uint8_t)0);
    if (nul == a.end()) return false;
    size_t p = (size_t)(nul - a.begin()) + 1;
    if (p + 8 > a.size() || std::memcmp(a.data() + p, "LBXP", 4) != 0 || a[p + 4] != 1) return false;
    *kind = a[p + 5];
    const size_t n = (size_t)a[p + 6] | ((size_t)a[p + 7] << 8);
    p += 8;
    for (size_t i = 0; i < n; ++i) {
        if (p + 1 > a.size()) return false;
        const size_t fl = a[p++];
        if (p + fl + 4 > a.size()) return false;
        std::string f((const char*)a.data() + p, fl);
        p += fl;
        const size_t bl = (size_t)a[p] | ((size_t)a[p + 1] << 8) | ((size_t)a[p + 2] << 16) | ((size_t)a[p + 3] << 24);
        p += 4;
        if (bl > a.size() - p) return false;
        units->push_back({f, std::vector<uint8_t>(a.begin() + (long)p, a.begin() + (long)(p + bl))});
        p += bl;
    }
    return true;
}

// The running cartridge's custom units, as their bundles and folders.
std::vector<std::pair<std::string, std::vector<uint8_t>>> live_units() {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> v;
    unsigned long long hs[256];
    const int n = lb_cunits_hashes(hs, 256);
    // The test aid's clone (LB_TEST_CUNIT) is built first and has no bundle.
    const int skip = std::max(0, lb_cunits_count() - n);
    for (int k = 0; k < n; ++k) {
        std::vector<uint8_t> b;
        if (!lb_cunits_bundle(hs[k], &b)) continue;
        const char* f = lb_cunits_folder(k + skip);
        std::string folder = f && *f ? f : "unit" + std::to_string(k + 1);
        const size_t slash = folder.find_last_of('/');   // "_received/<hash>"
        if (slash != std::string::npos) folder = "r" + folder.substr(slash + 1, 12);
        v.push_back({folder, std::move(b)});
    }
    return v;
}

// The port export: the running cartridge, and its units' bundles. `image`
// is filled with the target so a self-test can compare.
bool make_port_patch(std::vector<uint8_t>* patch, std::vector<uint8_t>* image, std::string* report,
                     const std::function<void(double)>& progress, int* unit_count = nullptr) {
    const std::vector<uint8_t>& orig = lb_mods_original();
    std::vector<std::pair<std::string, std::vector<uint8_t>>> units;
    if (orig.empty() || !lb_mods_copy_live(image, [&]() { units = live_units(); })) {
        *report = "The game has no cartridge image kept (no ROM loaded?).";
        return false;
    }
    if (unit_count) *unit_count = (int)units.size();
    const std::vector<uint8_t> app = make_appheader(0, units);
    if (!lb_vcdiff_encode(orig, *image, app, patch, progress)) {
        *report = "The patch could not be made (the ROM is over 4 GB?).";
        return false;
    }
    return true;
}

std::vector<LbSlotPick> default_picks() {
    // The self-test's choice: maps in order, each to the next slot.
    std::vector<LbSlotPick> v;
    const int n = std::min(lb_mods_count(), 30);
    for (int k = 0; k < n; ++k) v.push_back({k, k + 1});
    return v;
}

bool make_slot_patch(const std::vector<LbSlotPick>& picks, std::vector<uint8_t>* patch, std::vector<uint8_t>* image,
                     std::string* report, const std::function<void(double)>& progress) {
    const std::vector<uint8_t>& orig = lb_mods_original();
    if (orig.empty()) { *report = "The game has no cartridge image kept (no ROM loaded?)."; return false; }
    if (!lb_mods_build_slots(orig, picks, image, report)) return false;
    const std::vector<uint8_t> app = make_appheader(1, {});
    if (!lb_vcdiff_encode(orig, *image, app, patch, progress)) {
        *report = "The patch could not be made.";
        return false;
    }
    return true;
}

// The import, after decoding: what the image holds, installed (or, dry, only
// listed). Returns the report; *any is set when something was written.
// `orig` is the ROM the patch was applied to: rom/, or the one the player
// chose for a patch made for another release.
std::string import_image(const std::vector<uint8_t>& orig, const std::vector<uint8_t>& image,
                         const std::vector<uint8_t>& app, bool dry, bool* any, bool* bad) {
    *any = false;
    *bad = false;
    LbPatchScan scan;
    std::string why;
    if (!lb_mods_patch_scan(orig, image, &scan, &why)) {
        *bad = true;
        return "The patched ROM could not be read: " + why + ".";
    }
    std::string rep;
    int maps_new = 0, maps_had = 0, units_new = 0, units_had = 0, failed = 0;
    // Maps.
    for (const LbPatchMap& m : scan.maps) {
        std::string folder, w;
        const int r = lb_mods_install_map(m, dry, &folder, &w);
        if (r == 1) ++maps_new;
        else if (r == 2) ++maps_had;
        else ++failed;
        rep += "  map \"" + m.name + "\" (" + std::to_string(m.players) + " players, " + m.where + "): " +
               (r == 1 ? (dry ? "would install to " : "installed to ") + folder
                : r == 2 ? "already in your maps" + (folder.empty() ? std::string() : " (" + folder + ")")
                         : "failed: " + w) + "\n";
    }
    // Units: the port's bundles when the patch carries them.
    int kind = -1;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> bundles;
    const bool payload = read_payload(app, &kind, &bundles);
    for (const auto& u : bundles) {
        std::string folder, w;
        const int r = lb_cunits_install_bundle(u.second, u.first, dry, &folder, &w);
        if (r == 1) ++units_new;
        else if (r == 2) ++units_had;
        else ++failed;
        rep += "  custom unit " + u.first + ": " +
               (r == 1 ? (dry ? "would install to mods/units/" : "installed to mods/units/") + folder
                : r == 2 ? "already in your units (mods/units/" + folder + ")" : "failed: " + w) + "\n";
    }
    std::vector<std::string> notes = scan.notes;
    if (scan.ebp_changed || !scan.unit_files.empty()) {
        auto rd_orig = lb_mods_image_reader(orig, true);
        auto rd_new = lb_mods_image_reader(image, true);
        std::vector<uint8_t> se, pe;
        if (rd_orig("BP/Entities.ebp", &se) && rd_new("BP/Entities.ebp", &pe)) {
            const int block = scan.ebp_changed ? lb_cunits_block_count(se, pe) : 0;
            if (block > 0 && !payload)
                notes.push_back(std::to_string(block) + " custom unit(s) made in the port are in the ROM, but the "
                                "patch carries no unit data (it was not exported by the port, or its header was "
                                "stripped); they were not imported");
            else if (block > 0 && (int)bundles.size() < block)
                notes.push_back(std::to_string(block - (int)bundles.size()) + " of the ROM's port units have no "
                                "bundle in the patch and were not imported");
            if (block == 0) {
                std::vector<LbReskin> skins;
                int changed = 0;
                lb_cunits_find_reskins(se, pe, rd_orig, rd_new,
                                       [&](uint32_t id) { return lb_mods_image_text(image, id); }, &skins,
                                       &changed, &notes);
                for (const LbReskin& r : skins) {
                    std::string folder, w;
                    const int res = lb_cunits_install_reskin(r, dry, &folder, &w);
                    if (res == 1) ++units_new;
                    else if (res == 2) ++units_had;
                    else ++failed;
                    rep += "  re-skinned unit \"" + r.name + "\" (stock unit " + std::to_string(r.index) + "): " +
                           (res == 1 ? (dry ? "would install as a custom unit, mods/units/"
                                            : "installed as a custom unit, mods/units/") + folder
                            : res == 2 ? "already in your units (mods/units/" + folder + ")" : "failed: " + w) + "\n";
                }
                const int balance = changed - (int)skins.size();
                if (balance > 0)
                    notes.push_back(std::to_string(balance) + " stock unit record(s) have changed stats only "
                                    "(a balance mod): not imported as units. Save the patched ROM and use "
                                    "Balance > Presets' import from a ROM to take its values");
                if (!scan.unit_files.empty())
                    notes.push_back(std::to_string(scan.unit_files.size()) + " sprite, model or card file(s) "
                                    "changed; only stock units' changed sprite sheets are imported (as the "
                                    "re-skinned custom units above)");
            }
        }
    }
    if (scan.lng_changed && scan.maps.empty() && bundles.empty() && units_new + units_had == 0)
        notes.push_back("the language files changed (game text); text is not imported");
    if (!scan.other.empty()) {
        std::string list;
        for (size_t i = 0; i < scan.other.size() && i < 8; ++i) list += (i ? ", " : "") + scan.other[i];
        if (scan.other.size() > 8) list += ", ... (" + std::to_string(scan.other.size() - 8) + " more)";
        notes.push_back(std::to_string(scan.other.size()) + " other changed file(s) are not maps or units and "
                        "were not imported: " + list);
    }
    std::string head;
    if (scan.maps.empty() && bundles.empty() && units_new + units_had == 0)
        head = "The patch holds no maps or custom units the port can import.";
    else
        head = std::string(dry ? "Would import " : "Imported ") + std::to_string(maps_new) + " map(s) and " +
               std::to_string(units_new) + " custom unit(s)" +
               (maps_had + units_had ? "; " + std::to_string(maps_had + units_had) + " you already had" : "") +
               (failed ? "; " + std::to_string(failed) + " failed" : "") + "." +
               (maps_new + units_new && !dry ? " Press Load new maps and units now to play with them." : "");
    if (maps_new && !lb_settings_file_bool("mod_maps", 1))
        notes.push_back("\"Use Mod and Editor Maps\" (Settings > General) is off, so the maps won't be listed "
                        "until it is on");
    std::string out = head + "\n" + rep;
    if (!notes.empty()) {
        out += "Not imported / notes:\n";
        for (const std::string& n : notes) out += "  - " + n + "\n";
    }
    *any = maps_new + units_new > 0 && !dry;
    *bad = failed > 0 || (scan.maps.empty() && bundles.empty() && units_new + units_had == 0);
    return out;
}

bool start(std::function<void()> fn) {
    if (g_job.busy.exchange(true)) return false;
    g_job.pct = 0;
    std::thread([fn]() {
        fn();
        g_job.busy = false;
    }).detach();
    return true;
}

auto pct_fn(int from, int span) {
    return [from, span](double f) { g_job.pct = from + (int)(f * span); };
}

}  // namespace

bool lb_xdelta_busy() { return g_job.busy.load(); }
int lb_xdelta_progress() { return g_job.pct.load(); }
std::string lb_xdelta_message(bool* bad, bool* installed) {
    std::lock_guard<std::mutex> lk(g_job.mu);
    if (bad) *bad = g_job.bad;
    if (installed) *installed = g_job.installed;
    return g_job.msg;
}

bool lb_xdelta_export_port(const std::string& path) {
    return start([path]() {
        set_msg("Making the patch...", false);
        std::vector<uint8_t> patch, image;
        std::string rep;
        int units = 0;
        if (!make_port_patch(&patch, &image, &rep, pct_fn(0, 100), &units)) { set_msg(rep, true); return; }
        if (image == lb_mods_original()) {
            set_msg("The running game has no mod maps or custom units, so there is nothing to export.", true);
            return;
        }
        if (!write_all(path, patch)) { set_msg("Could not write " + path + ".", true); return; }
        set_msg("Exported " + path + " (" + mb(patch.size()) + "): the running cartridge with " +
                    std::to_string(lb_mods_count()) + " mod map(s) and " + std::to_string(units) +
                    " custom unit(s). It turns the European ROM into the cartridge this port plays; other "
                    "players of the port import it on this page. The added maps and units need this port's "
                    "code: on a DS or another emulator the game does not list them. For those, export \"For a "
                    "DS or another emulator\".",
                false);
        std::fprintf(stderr, "[lb_xdelta] exported %s: %zu bytes, target %zu bytes\n", path.c_str(), patch.size(),
                     image.size());
    });
}

bool lb_xdelta_export_slots(const std::string& path, const std::vector<LbSlotPick>& picks) {
    return start([path, picks]() {
        set_msg("Making the patch...", false);
        std::vector<uint8_t> patch, image;
        std::string rep;
        if (!make_slot_patch(picks, &patch, &image, &rep, pct_fn(0, 100))) { set_msg(rep, true); return; }
        if (!write_all(path, patch)) { set_msg("Could not write " + path + ".", true); return; }
        set_msg("Exported " + path + " (" + mb(patch.size()) + "). Apply it to the European LEGO Battles ROM "
                    "with xdelta3, xdeltaUI or DeltaPatcher.\n" + rep,
                false);
        std::fprintf(stderr, "[lb_xdelta] exported %s (stock slots): %zu bytes\n", path.c_str(), patch.size());
    });
}

bool lb_xdelta_import(const std::string& path, const std::string& original) {
    return start([path, original]() {
        set_msg("Reading the patch...", false);
        {
            std::lock_guard<std::mutex> lk(g_job.mu);
            g_job.failed_patch.clear();
        }
        std::vector<uint8_t> patch, image, app, other;
        if (!read_all(path, &patch, 1u << 30)) { set_msg("Could not read " + path + ".", true); return; }
        if (!original.empty() && !read_all(original, &other, 1u << 30)) {
            set_msg("Could not read " + original + ".", true);
            return;
        }
        const std::vector<uint8_t>& orig = original.empty() ? lb_mods_original() : other;
        if (orig.empty()) { set_msg("The game has no ROM image kept.", true); return; }
        std::string why;
        g_job.pct = 10;
        if (!lb_vcdiff_decode(orig, patch, &image, &app, &why)) {
            {
                std::lock_guard<std::mutex> lk(g_job.mu);
                g_job.failed_patch = path;
            }
            set_msg(std::string("That patch could not be applied to ") +
                        (original.empty() ? "your ROM" : "that ROM") + ": " + why + "." +
                        (original.empty() ? " The community's ROM hacks are built on the USA release; a patch "
                                            "made for it needs the USA ROM: choose it below."
                                          : ""),
                    true);
            return;
        }
        g_job.pct = 60;
        bool any = false, bad = false;
        const std::string rep = import_image(orig, image, app, false, &any, &bad);
        {
            std::lock_guard<std::mutex> lk(g_job.mu);
            g_job.patched = std::move(image);
        }
        set_msg(rep, bad, any);
        std::fprintf(stderr, "[lb_xdelta] import %s:\n%s", path.c_str(), rep.c_str());
        std::fflush(stderr);
    });
}

bool lb_xdelta_have_patched() {
    std::lock_guard<std::mutex> lk(g_job.mu);
    return !g_job.patched.empty();
}

std::string lb_xdelta_failed_patch() {
    std::lock_guard<std::mutex> lk(g_job.mu);
    return g_job.failed_patch;
}

bool lb_xdelta_save_patched(const std::string& path) {
    return start([path]() {
        std::vector<uint8_t> img;
        {
            std::lock_guard<std::mutex> lk(g_job.mu);
            img = g_job.patched;
        }
        if (img.empty()) { set_msg("No patched ROM to save: import a patch first.", true); return; }
        if (!write_all(path, img)) { set_msg("Could not write " + path + ".", true); return; }
        set_msg("Saved the patched ROM to " + path + " (" + mb(img.size()) + ").", false);
    });
}

// ---- LB_XDELTA_SELFTEST -------------------------------------------------------------
// LB_XDELTA_SELFTEST=1, once, after the cartridge is built: the port export
// (the running cartridge against rom/) and the stock-slot export (every mod
// map in slots 1, 2, ...) are each encoded, decoded back and compared, and the
// import is run on each without writing anything (what it would install).
// LB_XDELTA_SELFTEST_OUT=<folder> also writes lb_port.xdelta, lb_port.nds,
// lb_slots.xdelta and lb_slots.nds there, for a stock xdelta3 to check.
// LB_XDELTA_SELFTEST_IN=<patch> decodes that patch (one made by xdelta3, say)
// and runs the dry import on it.
void lb_xdelta_selftest_if_asked() {
    static std::atomic<bool> done{false};
    const char* v = std::getenv("LB_XDELTA_SELFTEST");
    if (!v || !*v || *v == '0' || done.exchange(true)) return;
    std::thread([]() {
        using clk = std::chrono::steady_clock;
        const char* outdir = std::getenv("LB_XDELTA_SELFTEST_OUT");
        const std::vector<uint8_t>& orig = lb_mods_original();
        std::fprintf(stderr, "[lb_xdelta] selftest: original %zu bytes, %d mod map(s), %d custom unit(s)\n",
                     orig.size(), lb_mods_count(), lb_cunits_count());
        auto run = [&](const char* name, bool slots) {
            std::vector<uint8_t> patch, image, back, app;
            std::string rep, why;
            const auto t0 = clk::now();
            const bool ok = slots ? make_slot_patch(default_picks(), &patch, &image, &rep, nullptr)
                                  : make_port_patch(&patch, &image, &rep, nullptr);
            const auto t1 = clk::now();
            if (!ok) {
                std::fprintf(stderr, "[lb_xdelta] selftest %s: not made: %s\n", name, rep.c_str());
                return;
            }
            const bool dec = lb_vcdiff_decode(orig, patch, &back, &app, &why);
            const auto t2 = clk::now();
            const bool same = dec && back == image;
            std::fprintf(stderr, "[lb_xdelta] selftest %s: target %zu bytes, patch %zu bytes, encode %lld ms, "
                         "decode %lld ms: %s%s\n", name, image.size(), patch.size(),
                         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
                         same ? "OK, decodes to the same image" : "FAIL ",
                         same ? "" : dec ? "(decoded image differs)" : why.c_str());
            if (outdir && *outdir) {
                const std::string base = std::string(outdir) + "/" + name;
                const bool w = write_all(base + ".xdelta", patch) && write_all(base + ".nds", image);
                std::fprintf(stderr, "[lb_xdelta] selftest %s: %s %s.xdelta and %s.nds\n", name,
                             w ? "wrote" : "COULD NOT WRITE", base.c_str(), base.c_str());
            }
            if (same) {
                bool any = false, bad = false;
                const std::string r = import_image(orig, back, app, true, &any, &bad);
                std::fprintf(stderr, "[lb_xdelta] selftest %s: dry import:\n%s", name, r.c_str());
            }
        };
        run("lb_port", false);
        if (lb_mods_count() > 0) run("lb_slots", true);
        if (const char* in = std::getenv("LB_XDELTA_SELFTEST_IN")) {
            // LB_XDELTA_SELFTEST_SRC=<rom>: the ROM that patch was made for, in
            // place of rom/ (a patch for the USA release, say).
            std::vector<uint8_t> patch, image, app, other;
            const char* src = std::getenv("LB_XDELTA_SELFTEST_SRC");
            const bool alt = src && *src && read_all(src, &other, 1u << 30);
            const std::vector<uint8_t>& base = alt ? other : orig;
            std::string why;
            if (!read_all(in, &patch, 1u << 30)) {
                std::fprintf(stderr, "[lb_xdelta] selftest in: cannot read %s\n", in);
            } else if (!lb_vcdiff_decode(base, patch, &image, &app, &why)) {
                std::fprintf(stderr, "[lb_xdelta] selftest in: %s does not decode: %s\n", in, why.c_str());
            } else {
                bool any = false, bad = false;
                std::fprintf(stderr, "[lb_xdelta] selftest in: %s decodes to %zu bytes; dry import:\n%s", in,
                             image.size(), import_image(base, image, app, true, &any, &bad).c_str());
            }
        }
        std::fprintf(stderr, "[lb_xdelta] selftest done\n");
        std::fflush(stderr);
    }).detach();
}
