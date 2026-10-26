// lb_setup.cpp - the first-run setup: ask for the player's own LEGO Battles ROM.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md. docs/394.
//
// WHEN. main.cpp's no-argument start (double-clicking LegoBattles.exe) looks in
// rom\ beside the executable. Until docs/394 an empty rom\ printed an
// explanation to stderr and exited -- and a double-clicked program has no
// console, so a new player saw nothing happen at all. Now that start calls
// this instead, with its own small window, when rom\ has no ROM or holds one
// that is not the European image.
//
// WHAT IT ACCEPTS. The port is built on the European cartridge (C5SP): its
// recompiled code is registered for that image's SHA-1 alone. The USA
// cartridge (C5SE) is the same game with the same files; it differs only in
// the first 0x126000 bytes (header, ARM9, overlays and their table; docs/04),
// so a USA dump is turned into the European image with
// data/usa_to_eur.lbdelta (tools/lb_region_delta.py; docs/395: the European
// code is rebuilt from the player's own USA code, not shipped) and the result
// is checked against the European SHA-1 before it is kept. Either way the player's own file is only read; the
// image the port boots is written to rom\.

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "sha1.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" int lb_pick_rom_file(char* buf, int cap);   /* lb_filedlg.cpp */
extern "C" int lb_pick_save_file(char* buf, int cap, int saving);   /* lb_filedlg.cpp */

namespace {

namespace fs = std::filesystem;

// THE APP ICON. lb_app.rc puts lbapp.png into the executable as icon resource 1
// (what Explorer and shortcuts show). SDL takes a window's icon, for the title
// bar and the taskbar, from these hints, which it reads when it registers its
// window class at video init; set here, before main, they reach the setup
// window and the game's window alike.
struct AppIconHints {
    AppIconHints() {
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    }
} g_app_icon_hints;

constexpr const char* kEuSha1 = "da60469bd143eb8f47ecf0d7951181b4783c91b4";
constexpr const char* kUsSha1 = "f25d090feb840e9b4b07546b6929d897e3fb3965";
constexpr const char* kEuName = "LEGO Battles (Europe) (En,Fr,De,Es,It,Da).nds";
constexpr size_t kRomSize = 134217728u;   // both releases: 1 Gbit

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

struct Job {
    std::atomic<int> state{0};   // 0 idle, 1 working, 2 done, 3 failed
    std::mutex m;
    std::string message;         // under m
    fs::path result;             // under m
};

void say(Job* job, int state, const std::string& text) {
    std::lock_guard<std::mutex> lock(job->m);
    job->message = text;
    job->state = state;
}

bool read_all(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (std::streamoff)(kRomSize * 2)) return false;
    out->resize((size_t)n);
    f.seekg(0);
    f.read((char*)out->data(), n);
    return (bool)f;
}

std::string hex20(const uint8_t* p) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 20; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Nintendo's BLZ, as tools/nds_extract.py blz_decompress: the packed region
// ends the module, is stored reversed and carries an 8..11-byte footer
// (enc_len 24 bits including the footer, hdr_len, inc_len). False when the
// footer is not valid.
bool blz_unpack(const uint8_t* data, size_t n, std::vector<uint8_t>* out) {
    if (n < 12) return false;
    const uint32_t inc = rd32(data + n - 4);
    const uint32_t hdr = data[n - 5];
    const uint32_t enc = rd32(data + n - 8) & 0x00FFFFFFu;
    if (inc == 0 || hdr < 8 || hdr > 11 || enc == 0 || enc > n || n <= hdr) return false;
    const size_t dec = n - enc, pak_len = enc - hdr, raw_len = dec + enc + inc;
    out->assign(raw_len, 0);
    std::memcpy(out->data(), data, dec);
    std::vector<uint8_t> pak(data + dec, data + dec + pak_len);
    std::reverse(pak.begin(), pak.end());
    uint8_t* raw = out->data();
    size_t src = 0, dst = dec;
    uint32_t mask = 0, flags = 0;
    while (dst < raw_len) {
        if (mask == 0) {
            if (src >= pak_len) break;
            flags = pak[src++];
            mask = 0x80;
        }
        if (!(flags & mask)) {
            if (src >= pak_len) break;
            raw[dst++] = pak[src++];
        } else {
            if (src + 1 >= pak_len) break;
            const uint32_t token = ((uint32_t)pak[src] << 8) | pak[src + 1];
            src += 2;
            size_t length = (token >> 12) + 3;
            const size_t disp = (token & 0x0FFF) + 3;
            if (dst + length > raw_len) length = raw_len - dst;
            if (disp > dst) return false;
            for (size_t k = 0; k < length; ++k, ++dst) raw[dst] = raw[dst - disp];
        }
        mask >>= 1;
    }
    std::reverse(out->begin() + (ptrdiff_t)dec, out->end());
    return true;
}

// A bounds-checked cursor over the conversion file: any read past its end
// marks it bad and returns zeros, so the parse needs one check at the end.
struct Cursor {
    const std::vector<uint8_t>& d;
    size_t at;
    bool bad = false;
    uint32_t u32() {
        if (at + 4 > d.size()) { bad = true; return 0; }
        const uint32_t v = rd32(d.data() + at);
        at += 4;
        return v;
    }
    uint8_t u8() {
        if (at + 1 > d.size()) { bad = true; return 0; }
        return d[at++];
    }
    const uint8_t* take(size_t n) {
        if (n > d.size() - std::min(at, d.size()) || at > d.size()) { bad = true; return nullptr; }
        const uint8_t* p = d.data() + at;
        at += n;
        return p;
    }
};

struct Module {
    uint32_t us_off, us_len, eu_off, eu_len, eu_raw;
    uint8_t us_packed, eu_packed;
    const uint8_t* tail; uint32_t tail_len;
    const uint8_t* parse; uint32_t parse_len;
};

// A module packed again by replaying its stored BLZ parse (flag bytes and
// back-reference tokens, literals stripped) over the unpacked bytes; the
// inverse of tools/lb_region_delta.py parse_of. False when the parse does not
// fit the module.
bool blz_repack(const uint8_t* raw, size_t raw_len, const Module& m, uint8_t* out) {
    if (m.tail_len < 8 || m.tail_len > 11) return false;
    const uint8_t* t = m.tail + m.tail_len;
    const uint32_t enc = rd32(t - 8) & 0x00FFFFFFu, hdr = t[-5], inc = rd32(t - 4);
    if (hdr != m.tail_len || enc < hdr || (size_t)enc + inc > raw_len || enc > m.eu_len) return false;
    const size_t dec = raw_len - enc - inc, pak_len = enc - hdr;
    if (dec + enc != m.eu_len) return false;
    const size_t rev_len = raw_len - dec;     // raw[dec..] walked backwards
    std::vector<uint8_t> pak;
    pak.reserve(pak_len);
    size_t s = 0, r = 0;
    while (pak.size() < pak_len) {
        if (s >= m.parse_len) return false;
        const uint8_t f = m.parse[s++];
        pak.push_back(f);
        for (int b = 0; b < 8 && pak.size() < pak_len; ++b) {
            if (f & (0x80 >> b)) {
                if (s + 2 > m.parse_len) return false;
                const uint32_t tok = ((uint32_t)m.parse[s] << 8) | m.parse[s + 1];
                pak.push_back(m.parse[s]);
                pak.push_back(m.parse[s + 1]);
                s += 2;
                r += (tok >> 12) + 3;
            } else {
                if (r >= rev_len) return false;
                pak.push_back(raw[raw_len - 1 - r]);
                ++r;
            }
        }
    }
    std::memcpy(out, raw, dec);
    for (size_t k = 0; k < pak_len; ++k) out[dec + k] = pak[pak_len - 1 - k];
    std::memcpy(out + dec + pak_len, m.tail, m.tail_len);
    return true;
}

// The European image from the USA one, in place (docs/395). The European
// code modules are rebuilt from the USA ones -- unpacked, patched with
// bsdiff's operations, packed again by their stored parse -- and the header,
// overlay table and FAT entries are byte runs. See tools/lb_region_delta.py
// for the format.
bool apply_delta(std::vector<uint8_t>* img, const fs::path& delta, std::string* err) {
    std::vector<uint8_t> d;
    if (!read_all(delta, &d) || d.size() < 64 || std::memcmp(d.data(), "LBDELTA2", 8) != 0) {
        *err = "the conversion file data\\usa_to_eur.lbdelta is missing or damaged";
        return false;
    }
    if (hex20(d.data() + 8) != kUsSha1 || rd32(d.data() + 48) != img->size()) {
        *err = "the conversion file does not match this ROM";
        return false;
    }
    const std::string damaged = "the conversion file is damaged";
    Cursor c{d, 52};
    const uint32_t fill_a = c.u32(), fill_b = c.u32();
    const uint32_t nm = c.u32();
    if (c.bad || nm == 0 || nm > 4096) { *err = damaged; return false; }
    std::vector<Module> mods(nm);
    size_t old_len = 0, new_len = 0;
    for (Module& m : mods) {
        m.us_off = c.u32(); m.us_len = c.u32(); m.eu_off = c.u32(); m.eu_len = c.u32();
        m.eu_raw = c.u32(); m.us_packed = c.u8(); m.eu_packed = c.u8(); c.u8(); c.u8();
        m.tail_len = c.u32(); m.tail = c.take(m.tail_len);
        m.parse_len = c.u32(); m.parse = c.take(m.parse_len);
        if (c.bad || (size_t)m.us_off + m.us_len > img->size() ||
            (size_t)m.eu_off + m.eu_len > img->size() || m.eu_raw > (16u << 20)) {
            *err = damaged;
            return false;
        }
        new_len += m.eu_raw;
    }
    // The USA modules, unpacked, end to end.
    std::vector<uint8_t> old_img;
    for (const Module& m : mods) {
        const uint8_t* p = img->data() + m.us_off;
        if (m.us_packed) {
            std::vector<uint8_t> raw;
            if (!blz_unpack(p, m.us_len, &raw)) { *err = "the USA code could not be unpacked"; return false; }
            old_img.insert(old_img.end(), raw.begin(), raw.end());
        } else {
            old_img.insert(old_img.end(), p, p + m.us_len);
        }
    }
    old_len = old_img.size();
    // bsdiff's operations: control triples, the sparse differences, the
    // inserted bytes.
    const uint32_t nc = c.u32();
    if (c.bad || nc > (1u << 20)) { *err = damaged; return false; }
    std::vector<int32_t> ctrl(3 * (size_t)nc);
    for (int32_t& v : ctrl) v = (int32_t)c.u32();
    const uint32_t nr = c.u32();
    if (c.bad) { *err = damaged; return false; }
    std::vector<uint8_t> diff(new_len, 0);
    size_t dp = 0;
    for (uint32_t k = 0; k < nr; ++k) {
        const uint32_t skip = c.u32(), len = c.u32();
        const uint8_t* p = c.take(len);
        if (c.bad || dp + skip + len > new_len) { *err = damaged; return false; }
        dp += skip;
        std::memcpy(diff.data() + dp, p, len);
        dp += len;
    }
    const uint32_t ne = c.u32();
    const uint8_t* extra = c.take(ne);
    if (c.bad) { *err = damaged; return false; }
    std::vector<uint8_t> new_img(new_len);
    int64_t np = 0, op = 0, ep = 0;
    dp = 0;
    for (uint32_t k = 0; k < nc; ++k) {
        const int64_t x = ctrl[3 * k], y = ctrl[3 * k + 1], z = ctrl[3 * k + 2];
        if (x < 0 || y < 0 || np + x + y > (int64_t)new_len || op < 0 || op + x > (int64_t)old_len ||
            ep + y > (int64_t)ne) {
            *err = damaged;
            return false;
        }
        for (int64_t i = 0; i < x; ++i)
            new_img[(size_t)(np + i)] = (uint8_t)(old_img[(size_t)(op + i)] + diff[dp + (size_t)i]);
        np += x; op += x; dp += (size_t)x;
        std::memcpy(new_img.data() + np, extra + ep, (size_t)y);
        np += y; ep += y; op += z;
    }
    if (np != (int64_t)new_len) { *err = damaged; return false; }
    // Lay the European modules down.
    if (fill_a > fill_b || fill_b > img->size()) { *err = damaged; return false; }
    std::memset(img->data() + fill_a, 0xFF, fill_b - fill_a);
    size_t at = 0;
    for (const Module& m : mods) {
        const uint8_t* raw = new_img.data() + at;
        at += m.eu_raw;
        if (m.eu_packed) {
            if (!blz_repack(raw, m.eu_raw, m, img->data() + m.eu_off)) { *err = damaged; return false; }
        } else {
            if (m.eu_raw != m.eu_len) { *err = damaged; return false; }
            std::memcpy(img->data() + m.eu_off, raw, m.eu_len);
        }
    }
    // The header, overlay table and FAT entries.
    const uint32_t nk = c.u32();
    if (c.bad) { *err = damaged; return false; }
    for (uint32_t k = 0; k < nk; ++k) {
        const uint32_t off = c.u32(), len = c.u32();
        const uint8_t* p = c.take(len);
        if (c.bad || (size_t)off + len > img->size()) { *err = damaged; return false; }
        std::memcpy(img->data() + off, p, len);
    }
    return true;
}

void work(Job* job, fs::path src, fs::path root) {
    say(job, 1, "Reading the ROM...");
    std::vector<uint8_t> rom;
    if (!read_all(src, &rom)) {
        say(job, 3, "That file could not be read.");
        return;
    }
    say(job, 1, "Checking it...");
    std::string sha = gba::sha1(rom.data(), rom.size()).hex();
    const bool title = rom.size() >= 0x10 && std::memcmp(rom.data(), "LEGO BATTLES", 12) == 0;
    if (sha == kUsSha1) {
        say(job, 1, "That is the USA release. Converting it to the European image "
                    "this port is built on...");
        std::string err;
        if (!apply_delta(&rom, root / "data" / "usa_to_eur.lbdelta", &err)) {
            say(job, 3, "The USA ROM could not be converted: " + err + ".");
            return;
        }
        sha = gba::sha1(rom.data(), rom.size()).hex();
        if (sha != kEuSha1) {
            say(job, 3, "The conversion did not produce the European image (SHA-1 " +
                        sha + "). Please report this.");
            return;
        }
    } else if (sha != kEuSha1) {
        if (title)
            say(job, 3, "That is LEGO Battles, but not a clean dump of the Europe or USA "
                        "release (SHA-1 " + sha + "). A trimmed, patched or hacked ROM "
                        "will not work; please use an untouched dump of your cartridge.");
        else
            say(job, 3, "That is not a LEGO Battles ROM. Please choose your own dump of "
                        "LEGO Battles (Europe or USA, .nds).");
        return;
    }
    std::error_code ec;
    fs::create_directories(root / "rom", ec);
    const fs::path out = root / "rom" / kEuName;
    if (fs::equivalent(src, out, ec)) {
        std::lock_guard<std::mutex> lock(job->m);
        job->result = out;
        job->message = "Ready.";
        job->state = 2;
        return;
    }
    say(job, 1, "Saving it into the rom folder...");
    {
        const fs::path tmp = root / "rom" / (std::string(kEuName) + ".part");
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write((const char*)rom.data(), (std::streamsize)rom.size());
        f.close();
        if (!f) { say(job, 3, "Could not write into the rom folder."); return; }
        fs::rename(tmp, out, ec);
        if (ec) { say(job, 3, "Could not write into the rom folder: " + ec.message()); return; }
    }
    // Any other .nds in rom\ (the USA dump, a wrong file) would be picked
    // before ours next time; it is moved aside rather than deleted.
    for (const auto& e : fs::directory_iterator(root / "rom", ec)) {
        if (e.path().extension() == ".nds" && !fs::equivalent(e.path(), out, ec)) {
            fs::path aside = e.path();
            aside += ".unused";
            fs::rename(e.path(), aside, ec);
        }
    }
    std::lock_guard<std::mutex> lock(job->m);
    job->result = out;
    job->message = "Ready.";
    job->state = 2;
}

// docs/427: the optional save step. The file is copied to saves\LEGO Battles.sav
// (the retail cartridge's fixed save, docs/401) if it carries the game's tag;
// the boot then trims padding and rewrites the region tag exactly as
// Settings > Saves > Import does.
bool stage_save(const fs::path& root, const fs::path& src, std::string* msg, bool* bad) {
    std::vector<uint8_t> d;
    if (!read_all(src, &d) || d.size() < 16) {
        *msg = "Could not read that file.";
        *bad = true;
        return false;
    }
    if (std::memcmp(d.data() + 3, "_BATTLE_SAVES", 13) != 0) {
        *msg = "That file has no _BATTLE_SAVES tag, so it is not a LEGO Battles save.";
        *bad = true;
        return false;
    }
    std::error_code ec;
    const fs::path dir = root / "saves";
    fs::create_directories(dir, ec);
    const fs::path dst = dir / "LEGO Battles.sav";
    if (fs::exists(dst, ec)) fs::copy_file(dst, dir / "LEGO Battles.sav.setup-bak", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        *msg = "Could not copy the save into the saves folder.";
        *bad = true;
        return false;
    }
    *msg = "Save imported. It loads when the game starts.";
    *bad = false;
    return true;
}

}  // namespace

// Returns 1 with the image to boot in `out` (as main.cpp's paths are), 0 if
// the player closed the window. `found` is a ROM already in rom\ that needs
// looking at (empty for none).
extern "C" int lb_first_run_setup(const char* root_dir, const char* found,
                                  char* out, int cap) {
    const fs::path root(root_dir ? root_dir : ".");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return 0;
    // The game window's own placement rule (frontend.cpp, apply_hooks.py):
    // LB_DISPLAY, or the display LB_DISPLAY_W x LB_DISPLAY_H, else the first.
    // A test sets those to keep the window off the player's screen, and then
    // it must not take focus either.
    int display = 0;
    const char* dw = std::getenv("LB_DISPLAY_W");
    const char* dh = std::getenv("LB_DISPLAY_H");
    if (dw && dh) {
        for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
            SDL_Rect b{};
            if (SDL_GetDisplayBounds(i, &b) == 0 && b.w == std::atoi(dw) &&
                b.h == std::atoi(dh))
                display = i;
        }
    }
    if (const char* d = std::getenv("LB_DISPLAY")) display = std::atoi(d);
    if (dw || std::getenv("LB_DISPLAY"))
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
    SDL_Window* win = SDL_CreateWindow("LEGO Battles - first-time setup",
                                       SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                       SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                       760, 440, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return 0; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetIO().FontGlobalScale = 1.25f;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    Job job;
    std::thread worker;
    auto start = [&](const fs::path& p) {
        if (job.state.load() == 1) return;
        if (worker.joinable()) worker.join();
        job.state = 1;
        worker = std::thread(work, &job, p, root);
    };
    if (found && *found) start(fs::path(found));

    int result = 0;
    bool running = true;
    std::string save_msg;
    bool save_bad = false, save_done = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_DROPFILE && e.drop.file) {
                start(from_utf8(e.drop.file));
                SDL_free(e.drop.file);
            }
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "LEGO Battles - PC port");
        ImGui::Separator();
        ImGui::TextWrapped(
            "This port plays your own copy of LEGO Battles for the Nintendo DS. "
            "Choose your ROM (a dump of your cartridge, .nds): the Europe or the "
            "USA release both work. You can also drag the file onto this window.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Your file is only read. The image the port plays is saved "
                           "in the rom folder beside the game, so this is asked once.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        const int st = job.state.load();
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(job.m);
            msg = job.message;
        }
        ImGui::BeginDisabled(st == 1);
        if (ImGui::Button("Choose your LEGO Battles ROM...", ImVec2(360, 0))) {
            char buf[1024] = "";
            if (lb_pick_rom_file(buf, sizeof buf)) start(from_utf8(buf));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120, 0))) running = false;
        ImGui::Spacing();
        if (st == 1) ImGui::TextWrapped("%s", msg.c_str());
        if (st == 3) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        }
        if (st == 2) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Your ROM is set up.");
            // docs/427: an optional save, the same import Settings > Saves
            // does (any region, padded files, the tag rewritten): staged as
            // saves\LEGO Battles.sav, which the boot loads with
            // lb_save_lenient_load + lb_save_adopt.
            ImGui::Separator();
            ImGui::TextWrapped("Import a save from a cartridge or an emulator (optional)");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Any region's .sav works. You can also do this later in "
                               "Settings > Saves.");
            ImGui::PopStyleColor();
            if (!save_msg.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, save_bad ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                                              : ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
                ImGui::TextWrapped("%s", save_msg.c_str());
                ImGui::PopStyleColor();
            }
            bool go = false;
            if (!save_done) {
                if (ImGui::Button("Browse...", ImVec2(160, 0))) {
                    char sb[1024] = "";
                    if (lb_pick_save_file(sb, sizeof sb, 0)) {
                        save_done = stage_save(root, from_utf8(sb), &save_msg, &save_bad);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Skip", ImVec2(120, 0))) go = true;
            } else {
                if (ImGui::Button("Start the game", ImVec2(200, 0))) go = true;
            }
            if (go) {
                std::lock_guard<std::mutex> lock(job.m);
                const std::string p = job.result.string();
                std::snprintf(out, (size_t)cap, "%s", p.c_str());
                result = 1;
                running = false;
            }
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 16, 18, 24, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    if (worker.joinable()) worker.join();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(ctx);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    std::fprintf(stderr, "[lb_setup] %s\n", result ? out : "closed without a ROM");
    std::fflush(stderr);
    return result;
}

// ── docs/401: A SAVE FROM ANY REGION, ANY TOOL, UNDER ANY ROM NAME ─────────
//
// Three ways a player's save "did not apply" (2026-10-18):
//
//   * THE REGION TAG. A save begins "XXX_BATTLE_SAVES", and XXX is a constant
//     compiled into each build (func_0204B1D4 writes 'A','F','E' as
//     immediates). This port runs the European code, so a USA cartridge's
//     "AFN" save shows empty profile slots. The checksum does not cover the
//     tag (tools/lb_save.py), so rewriting the three letters is enough.
//   * THE SIZE. The runner loads only an exact-size file and otherwise
//     ignores it -- and then writes a blank one over it at the next save.
//     Emulators and flashcarts pad (to 8 KB and beyond with 0xFF, or with
//     DeSmuME's footer); the game's 512 bytes are at the front.
//   * THE NAME. The save was named after the ROM file, so renaming the ROM
//     started an empty save, and a save dropped into saves\ under any other
//     name was never read.
//
// So the retail cartridge -- the European image, which a USA ROM is converted
// to before this point -- always uses saves\LEGO Battles.sav, taken over from
// the old name (or from a lone save the player dropped there) the first time.
// A romhack keeps a save of its own name, so it can never overwrite this one.
// Every rewrite keeps the original beside it as <name>.orig.
namespace {
const char kRetailSha1[] = "da60469bd143eb8f47ecf0d7951181b4783c91b4";
const char kBuildTag[3] = {'A', 'F', 'E'};

bool battle_tagged(const unsigned char* d, size_t n) {
    return n >= 16 && std::memcmp(d + 3, "_BATTLE_SAVES", 13) == 0;
}

void keep_original(const char* path) {
    if (!path || !*path) return;
    std::error_code ec;
    const std::filesystem::path orig = std::string(path) + ".orig";
    if (!std::filesystem::exists(orig, ec))
        std::filesystem::copy_file(path, orig, ec);
}
}  // namespace

// The loaded image's tag, rewritten to this build's; 1 if it was changed.
extern "C" int lb_save_adopt(unsigned char* sram, size_t size, const char* path) {
    if (!sram || !battle_tagged(sram, size)) return 0;
    if (std::memcmp(sram, kBuildTag, 3) == 0) return 0;
    char was[4] = {(char)sram[0], (char)sram[1], (char)sram[2], 0};
    keep_original(path);
    std::memcpy(sram, kBuildTag, 3);
    std::fprintf(stderr, "[save] %s carries the %s tag (another region's build); "
                 "retagged to AFE, original kept as .orig\n", path ? path : "", was);
    return 1;
}

// A file larger than the save whose extra bytes are padding or an
// emulator's footer: its first `size` bytes. 1 if taken.
extern "C" int lb_save_lenient_load(const char* path, unsigned char* sram,
                                    size_t size) {
    if (!path || !sram || !size) return 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (buf.size() <= size || !battle_tagged(buf.data(), size)) return 0;
    bool blank = true;
    for (size_t i = size; i < buf.size() && blank; ++i)
        blank = buf[i] == 0xFFu || buf[i] == 0x00u;
    static const char kFooter[] = "|-DESMUME SAVE-|";
    const bool desmume =
        std::search(buf.begin() + (std::ptrdiff_t)size, buf.end(), kFooter,
                    kFooter + sizeof kFooter - 1) != buf.end();
    if (!blank && !desmume) return 0;
    keep_original(path);
    std::memcpy(sram, buf.data(), size);
    std::fprintf(stderr, "[save] %s is %zu bytes (%s); took its first %zu, "
                 "original kept as .orig\n", path, buf.size(),
                 desmume ? "a DeSmuME save" : "padded", size);
    return 1;
}

// The save path for this cartridge: the retail image's fixed one, taken over
// from `derived` (the old ROM-named path) when that is all there is. Only a
// save NAMED AFTER THE ROM is replaced -- the convention play.bat and the
// double-click start follow; a path a tool named itself (the harness's
// lb.sav) is left exactly as given.
extern "C" int lb_retail_save_path(const char* derived, const char* rom_path,
                                   const char* rom_sha1, char* out, int cap) {
    if (!derived || !*derived || !rom_sha1 || !out || cap <= 0) return 0;
    if (std::strcmp(rom_sha1, kRetailSha1) != 0) return 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path old_path(derived);
    if (!rom_path || old_path.stem() != fs::path(rom_path).stem()) return 0;
    const fs::path dir = old_path.has_parent_path() ? old_path.parent_path() : fs::path(".");
    const fs::path fixed = dir / "LEGO Battles.sav";
    if (!fs::exists(fixed, ec)) {
        fs::path from;
        if (fs::exists(old_path, ec)) {
            from = old_path;
        } else {
            // A save the player dropped in by hand: taken only when it is the
            // one candidate, so a guess never picks between two.
            int found = 0;
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string ext = e.path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".sav") continue;
                ++found;
                from = e.path();
            }
            if (found != 1) from.clear();
        }
        if (!from.empty()) {
            fs::copy_file(from, fixed, ec);
            std::fprintf(stderr, "[save] the cartridge's save is now %s "
                         "(copied from %s)\n", fixed.string().c_str(),
                         from.string().c_str());
        }
    }
    std::snprintf(out, (size_t)cap, "%s", fixed.string().c_str());
    return 1;
}
