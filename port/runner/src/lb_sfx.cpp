// docs/427: the notification sound. A C++ port of tools/lb_sfx.py's render:
// a UI effect of the player's own ROM (Sound/sound_data.sdat: SSAR sequence ->
// SBNK instrument -> SWAR sample, at the note's pitch) is decoded once and
// played on its own SDL audio device -- never through the emulated DS, so the
// game's sound and the simulation are untouched. No game audio ships with the
// port.
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" int lb_rom_read(const char* path, unsigned char* out, int cap);
extern "C" int lb_setting_notify_sound(void);
extern "C" int lb_setting_notify_volume(void);

namespace {

struct Rd {
    const std::vector<uint8_t>& d;
    explicit Rd(const std::vector<uint8_t>& v) : d(v) {}
    uint8_t u8(size_t o) const {
        if (o >= d.size()) throw std::out_of_range("u8");
        return d[o];
    }
    uint16_t u16(size_t o) const { return (uint16_t)(u8(o) | (u8(o + 1) << 8)); }
    uint32_t u32(size_t o) const {
        return (uint32_t)u16(o) | ((uint32_t)u16(o + 2) << 16);
    }
};

struct Sdat {
    std::vector<uint8_t> d;
    Rd r{d};
    uint32_t symb = 0, info = 0, fat = 0;
    void init() {
        symb = r.u32(0x10);
        info = r.u32(0x18);
        fat = r.u32(0x20);
    }
    std::vector<uint32_t> record(uint32_t base, uint32_t n) const {
        const uint32_t off = r.u32(base + 8 + 4 * n);
        const uint32_t count = r.u32(base + off);
        if (count > 100000) throw std::out_of_range("record");
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < count; ++i) v.push_back(r.u32(base + off + 4 + 4 * i));
        return v;
    }
    std::vector<uint8_t> file(uint32_t fid) const {
        const uint32_t off = r.u32(fat + 12 + 16 * fid), size = r.u32(fat + 12 + 16 * fid + 4);
        if ((uint64_t)off + size > d.size()) throw std::out_of_range("file");
        return std::vector<uint8_t>(d.begin() + off, d.begin() + off + size);
    }
    std::string cstr(uint32_t off) const {
        std::string s;
        while (r.u8(off)) s.push_back((char)r.u8(off++));
        return s;
    }
    bool find(const std::string& name, int* arc, int* seq) const {
        const uint32_t arcs_off = r.u32(symb + 8 + 4);
        const uint32_t count = r.u32(symb + arcs_off);
        for (uint32_t a = 0; a < count; ++a) {
            const uint32_t sub = r.u32(symb + arcs_off + 4 + 8 * a + 4);
            if (!sub) continue;
            const uint32_t n = r.u32(symb + sub);
            for (uint32_t s = 0; s < n; ++s) {
                const uint32_t no = r.u32(symb + sub + 4 + 4 * s);
                if (no && cstr(symb + no) == name) {
                    *arc = (int)a;
                    *seq = (int)s;
                    return true;
                }
            }
        }
        return false;
    }
};

struct Note { int prog, note, vel, dur; };

uint32_t varlen(const Rd& b, size_t* p) {
    uint32_t v = 0;
    for (;;) {
        const uint8_t c = b.u8((*p)++);
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return v;
    }
}

std::vector<Note> notes(const std::vector<uint8_t>& seqv, size_t start, size_t limit = 8) {
    const Rd seq(seqv);
    std::vector<Note> out;
    std::vector<size_t> todo{start};
    std::set<size_t> seen;
    while (!todo.empty() && out.size() < limit) {
        size_t p = todo.front();
        todo.erase(todo.begin());
        if (seen.count(p) || p >= seqv.size()) continue;
        seen.insert(p);
        int prog = 0;
        for (int it = 0; it < 400; ++it) {
            if (p >= seqv.size()) break;
            const int c = seq.u8(p++);
            if (c < 0x80) {
                const int vel = seq.u8(p);
                ++p;
                const uint32_t dur = varlen(seq, &p);
                out.push_back({prog, c, vel, (int)dur});
                if (out.size() >= limit) break;
            } else if (c == 0x80) {
                varlen(seq, &p);
            } else if (c == 0x81) {
                prog = (int)varlen(seq, &p);
            } else if (c == 0x93) {
                todo.push_back((size_t)seq.u8(p + 1) | ((size_t)seq.u8(p + 2) << 8) |
                               ((size_t)seq.u8(p + 3) << 16));
                p += 4;
            } else if (c == 0x94 || c == 0x95) {
                p += 3;
            } else if (c >= 0xA0 && c <= 0xA2) {
                p += 1;
            } else if (c >= 0xB0 && c <= 0xBD) {
                p += 3;
            } else if (c >= 0xC0 && c <= 0xD7) {
                p += 1;
            } else if (c >= 0xE0 && c <= 0xE3) {
                p += 2;
            } else if (c == 0xFE) {
                p += 2;
            } else if (c == 0xFC || c == 0xFD) {
            } else if (c == 0xFF) {
                break;
            }
        }
    }
    return out;
}

struct Ins { int swav, swar, base; };

bool instrument(const std::vector<uint8_t>& bankv, int prog, int note, Ins* o) {
    const Rd bank(bankv);
    const uint32_t count = bank.u32(0x38);
    if ((uint32_t)prog >= count) return false;
    const int t = bank.u8(0x3C + 4 * prog);
    const size_t off = bank.u16(0x3C + 4 * prog + 1);
    if (t >= 1 && t <= 5) {
        *o = {bank.u16(off), bank.u16(off + 2), bank.u8(off + 4)};
        return true;
    }
    if (t == 0x10) {
        const int lo = bank.u8(off), hi = bank.u8(off + 1);
        if (note < lo || note > hi) return false;
        const size_t e = off + 2 + (size_t)(note - lo) * 12;
        *o = {bank.u16(e + 2), bank.u16(e + 4), bank.u8(e + 6)};
        return true;
    }
    if (t == 0x11) {
        int r = -1;
        for (int i = 0; i < 8; ++i) {
            const int k = bank.u8(off + i);
            if (k && note <= k) { r = i; break; }
        }
        if (r < 0) return false;
        const size_t e = off + 8 + (size_t)r * 12;
        *o = {bank.u16(e + 2), bank.u16(e + 4), bank.u8(e + 6)};
        return true;
    }
    return false;
}

const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767};

void swav(const std::vector<uint8_t>& swarv, int idx, std::vector<int>* pcm, int* rate) {
    const Rd swar(swarv);
    const size_t off = swar.u32(0x3C + 4 * (size_t)idx);
    const int kind = swar.u8(off);
    *rate = swar.u16(off + 2);
    const uint32_t loop_start = swar.u16(off + 6), loop_len = swar.u32(off + 8);
    size_t n = ((size_t)loop_start + loop_len) * 4;
    const size_t base = off + 12;
    if (base > swarv.size()) throw std::out_of_range("swav");
    n = std::min(n, swarv.size() - base);
    pcm->clear();
    if (kind == 0) {
        for (size_t i = 0; i < n; ++i) pcm->push_back((int)(int8_t)swarv[base + i] * 256);
    } else if (kind == 1) {
        for (size_t i = 0; i + 1 < n; i += 2)
            pcm->push_back((int)(int16_t)(swarv[base + i] | (swarv[base + i + 1] << 8)));
    } else {
        if (n < 4) return;
        int s = (int16_t)(swarv[base] | (swarv[base + 1] << 8));
        int ix = (int16_t)(swarv[base + 2] | (swarv[base + 3] << 8));
        ix = std::max(0, std::min(88, ix));
        for (size_t i = 4; i < n; ++i) {
            const int b = swarv[base + i];
            for (int h = 0; h < 2; ++h) {
                const int nib = h == 0 ? (b & 0xF) : (b >> 4);
                const int step = kImaStep[ix];
                int diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                s = std::max(-32768, std::min(32767, (nib & 8) ? s - diff : s + diff));
                ix = std::max(0, std::min(88, ix + kImaIndex[nib]));
                pcm->push_back(s);
            }
        }
    }
}

// The effect as mono S16 at 32 kHz, peak-normalised; empty on any failure.
std::vector<int16_t> render_effect(const Sdat& sd, int arc, int seq_i) {
    const uint32_t arc_info = sd.record(sd.info, 1).at((size_t)arc);
    const std::vector<uint8_t> ssar = sd.file(sd.r.u16(sd.info + arc_info));
    const Rd sr(ssar);
    const uint32_t data_off = sr.u32(0x18);
    const uint32_t s_off = sr.u32(0x20 + 12 * (size_t)seq_i);
    const int bank_id = sr.u16(0x20 + 12 * (size_t)seq_i + 4);
    const uint32_t bank_info = sd.info + sd.record(sd.info, 2).at((size_t)bank_id);
    const std::vector<uint8_t> bank = sd.file(sd.r.u16(bank_info));
    uint16_t wave_arcs[4];
    for (int i = 0; i < 4; ++i) wave_arcs[i] = sd.r.u16(bank_info + 4 + 2 * i);
    const std::vector<uint32_t> swar_rec = sd.record(sd.info, 3);
    if (data_off > ssar.size()) return {};
    const std::vector<uint8_t> seq(ssar.begin() + data_off, ssar.end());
    std::vector<float> out;
    double at = 0.0;
    for (const Note& n : notes(seq, s_off)) {
        Ins ins;
        if (!instrument(bank, n.prog, n.note, &ins)) continue;
        const int wa = ins.swar < 4 ? wave_arcs[ins.swar] : 0xFFFF;
        if (wa == 0xFFFF || (size_t)wa >= swar_rec.size()) continue;
        const std::vector<uint8_t> swar = sd.file(sd.r.u16(sd.info + swar_rec[(size_t)wa]));
        std::vector<int> pcm;
        int rate = 0;
        swav(swar, ins.swav, &pcm, &rate);
        if (pcm.empty()) continue;
        const double step = rate * std::pow(2.0, (n.note - ins.base) / 12.0) / 32000.0;
        if (step <= 0.0) continue;
        const int length = std::min((int)(pcm.size() / step), 32000);
        const size_t start = (size_t)at;
        if (out.size() < start + (size_t)length) out.resize(start + (size_t)length, 0.0f);
        for (int k = 0; k < length; ++k) {
            const size_t j = (size_t)(k * step);
            if (j >= pcm.size()) break;
            const float fade = k < length * 0.8 ? 1.0f : (float)(length - k) / (length * 0.2f);
            out[start + (size_t)k] += pcm[j] / 32768.0f * (n.vel / 127.0f) * fade;
        }
        at += n.dur * 32000.0 / 240.0 * 2;
    }
    if (out.empty()) return {};
    float peak = 1e-6f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    std::vector<int16_t> pcm16(out.size());
    for (size_t i = 0; i < out.size(); ++i)
        pcm16[i] = (int16_t)(out[i] / peak * 26000.0f);
    return pcm16;
}

const char* const kNames[9] = {"Off", "Group set", "Select", "Accept", "Click",
                               "Enter", "Objective complete", "Screen swap", "Magic"};
const char* const kSe[9] = {nullptr, "SE_UI_GROUPSET", "SE_UI_SELECT", "SE_UI_ACCEPT",
                            "SE_UI_CLICK1", "SE_UI_ENTER1", "SE_UI_OBJECTIVECOMPLETE",
                            "SE_UI_SCREENSWAP", "SE_UI_MAGIC1"};

std::mutex g_mu;
std::map<int, std::vector<int16_t>> g_cache;   // under g_mu
std::set<int> g_started;                       // under g_mu: render launched
std::atomic<bool> g_warned{false};
SDL_AudioDeviceID g_dev = 0;
bool g_dev_tried = false;
Uint64 g_last_play = 0;

void warn_once(const char* what) {
    if (g_warned.exchange(true)) return;
    std::fprintf(stderr, "[lb_sfx] %s; the notification sound stays silent\n", what);
    std::fflush(stderr);
}

void render_job(int choice) {
    std::vector<int16_t> pcm;
    try {
        const int n = lb_rom_read("Sound/sound_data.sdat", nullptr, 0);
        if (n <= 0) {
            // maybe the ROM is not attached yet: let a later tick retry
            warn_once("sound_data.sdat is not readable from the ROM");
            std::lock_guard<std::mutex> lk(g_mu);
            g_started.erase(choice);
            return;
        } else {
            Sdat sd;
            sd.d.resize((size_t)n);
            if (lb_rom_read("Sound/sound_data.sdat", sd.d.data(), n) != n) {
                warn_once("sound_data.sdat read short");
            } else {
                sd.init();
                int arc = 0, seq = 0;
                if (!sd.find(kSe[choice], &arc, &seq)) {
                    warn_once("the effect is not in the sound archive");
                } else {
                    pcm = render_effect(sd, arc, seq);
                    if (pcm.empty()) warn_once("the effect has no renderable note");
                }
            }
        }
    } catch (const std::exception& e) {
        warn_once((std::string("the effect could not be decoded (") + e.what() + ")").c_str());
    }
    std::fprintf(stderr, "[lb_sfx] %s rendered: %zu samples\n", kSe[choice], pcm.size());
    std::fflush(stderr);
    std::lock_guard<std::mutex> lk(g_mu);
    g_cache[choice] = std::move(pcm);
}

void start_render(int choice) {
    if (choice < 1 || choice > 8) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_started.insert(choice).second) return;
    }
    std::thread(render_job, choice).detach();
}

bool open_device() {
    if (g_dev) return true;
    if (g_dev_tried) return false;
    g_dev_tried = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        warn_once("no audio subsystem");
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 32000;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        warn_once("no audio device");
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

void play(int choice, int vol_pct) {
    if (choice < 1 || choice > 8) return;
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cache.find(choice);
        if (it == g_cache.end()) {
            // not decoded yet (or being): start it; this one is skipped
            g_started.insert(choice);
            std::thread(render_job, choice).detach();
            return;
        }
        pcm = it->second;
    }
    if (pcm.empty() || !open_device()) return;
    const int v = std::max(0, std::min(100, vol_pct));
    for (int16_t& s : pcm) s = (int16_t)(s * v / 100);
    SDL_ClearQueuedAudio(g_dev);
    SDL_QueueAudio(g_dev, pcm.data(), (Uint32)(pcm.size() * sizeof(int16_t)));
}

}  // namespace

extern "C" const char* lb_sfx_choice_name(int i) {
    return (i >= 0 && i < 9) ? kNames[i] : "";
}

// Once a frame from the overlay: decodes the chosen effect in the background
// (again when the choice changes), so it is ready before it is needed.
extern "C" void lb_sfx_tick(void) {
    static Uint64 last = 0;
    const Uint64 now = SDL_GetTicks64();
    if (last && now - last < 1000) return;   // a failed read retries at 1 Hz
    last = now;
    start_render(lb_setting_notify_sound());
}

// The settings page's Play button.
extern "C" void lb_sfx_preview(void) {
    play(lb_setting_notify_sound(), lb_setting_notify_volume());
}

// An incoming chat message or a notice: at most once per 300 ms.
extern "C" void lb_sfx_notify(void) {
    const int c = lb_setting_notify_sound();
    if (c < 1) return;
    const Uint64 now = SDL_GetTicks64();
    if (g_last_play && now - g_last_play < 300) return;
    g_last_play = now;
    play(c, lb_setting_notify_volume());
}
