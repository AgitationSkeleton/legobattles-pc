// lb_prof.cpp -- a sampling profiler for the emulation thread (LB_SAMPLE=1).
//
// Why this exists. docs/262 found the frame's cost "structural" from 50 gdb
// samples of the main thread, and a flat 50-sample histogram cannot rank
// anything below ~6%. NDS_PROFILE_SCHED splits a round into ARM9 / ARM7 /
// devices but says nothing about what inside the ARM9 share costs: the
// generated instruction bodies, or the runtime helpers every one of them
// calls (runtime_code_cycles, runtime_tick, runtime_mem_cycles, the bus slow
// paths, dispatch). Deciding which of those to make cheaper needs a
// histogram with thousands of samples and callers, so this takes them.
//
// How. A second thread wakes every LB_SAMPLE_US microseconds (default 500),
// suspends the thread that ran the static initialisers -- the main thread,
// which runs the frontend loop and so the whole emulation -- reads its
// context, walks up to kFrames frames with the x64 unwind tables, and resumes
// it. Nothing is allocated while the target is suspended (it may hold the
// heap lock), and the unwinder never consults a function table outside this
// image, which another module might be registering while suspended. A leaf
// in a library (memcmp, a kernel wait) gets its caller by scanning the stack
// for a return address into this image instead (docs/353); a stale return
// address can match, which the CALL-site test makes rare.
//
// Output. LB_SAMPLE_OUT (default lb_samples.bin), appended once a second,
// because the harness kills the process rather than letting it exit:
//
//   header  "LBS1" u32 frames  u64 qpc_frequency  u64 image_base
//   record  u64 qpc  u64 pc[frames]      (0-padded; pc[0] is the leaf)
//
// Addresses are rebased to the image's preferred base, so they compare
// directly with `nm nds_runner.exe`. tools/lb_prof.py reads it.
//
// Cost when LB_SAMPLE is unset: one getenv at static-initialisation time.

#if defined(_WIN32)

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr int kFrames = 12;
constexpr uint32_t kRing = 1u << 16;   // records between flushes, at most

struct Record {
    uint64_t qpc;
    uint64_t pc[kFrames];
};

Record* g_ring = nullptr;              // preallocated: never grows
std::atomic<uint32_t> g_head{0};       // written by the sampler only
uint32_t g_flushed = 0;

HANDLE g_target = nullptr;
uint64_t g_image_lo = 0, g_image_hi = 0, g_image_pref = 0;
char g_out[MAX_PATH] = "lb_samples.bin";
unsigned g_period_us = 500;

void flush_records() {
    const uint32_t head = g_head.load(std::memory_order_acquire);
    if (head == g_flushed) return;
    FILE* f = std::fopen(g_out, "ab");
    if (!f) return;
    // A burst larger than the ring is truncated to its newest kRing
    // records rather than wrapping over ones not yet written.
    uint32_t from = g_flushed;
    if (head - from > kRing) from = head - kRing;
    for (uint32_t i = from; i != head; ++i)
        std::fwrite(&g_ring[i % kRing], sizeof(Record), 1, f);
    std::fclose(f);
    g_flushed = head;
}

bool in_image(uint64_t pc) { return pc >= g_image_lo && pc < g_image_hi; }

// DOES A CALL END AT `ret`? The x64 forms a compiler emits: call rel32, call
// reg (with or without REX), and call through memory with disp8 / disp32 /
// rip-relative / SIB addressing. Reads this image's own code, nothing else.
bool after_call(uint64_t ret) {
    if (ret - g_image_lo < 8u) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ret);
    auto call_modrm = [](uint8_t m, int mod) {
        return ((m >> 3) & 7) == 2 && (m >> 6) == mod;
    };
    if (p[-5] == 0xE8) return true;                              // rel32
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return true;    // reg
    if (p[-2] == 0xFF && call_modrm(p[-1], 0) &&
        (p[-1] & 7) != 4 && (p[-1] & 7) != 5) return true;       // [reg]
    if (p[-3] == 0xFF && call_modrm(p[-2], 1) && (p[-2] & 7) != 4)
        return true;                                             // [reg+d8]
    if (p[-4] == 0xFF && call_modrm(p[-3], 1) && (p[-3] & 7) == 4)
        return true;                                             // [sib+d8]
    if (p[-6] == 0xFF && p[-5] == 0x15) return true;             // [rip+d32]
    if (p[-6] == 0xFF && call_modrm(p[-5], 2) && (p[-5] & 7) != 4)
        return true;                                             // [reg+d32]
    if (p[-7] == 0xFF && call_modrm(p[-6], 2) && (p[-6] & 7) == 4)
        return true;                                             // [sib+d32]
    return false;
}

// Words of the stack searched above a library leaf for the frame that called
// into the library: 768 bytes, well inside the main thread's stack from
// anywhere in the frontend loop.
constexpr int kScanWords = 96;

void take_sample() {
    alignas(16) CONTEXT ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(g_target) == (DWORD)-1) return;
    Record& r = g_ring[g_head.load(std::memory_order_relaxed) % kRing];
    std::memset(&r, 0, sizeof r);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    r.qpc = static_cast<uint64_t>(now.QuadPart);
    if (GetThreadContext(g_target, &ctx)) {
        for (int i = 0; i < kFrames; ++i) {
            const uint64_t pc = ctx.Rip;
            if (!pc) break;
            r.pc[i] = in_image(pc) ? pc - g_image_lo + g_image_pref : pc;
            if (!in_image(pc)) {
                // A LEAF IN A LIBRARY: memcmp in ucrtbase, a wait in ntdll.
                // Its callers are not unwound (see the header comment), so
                // the frame that called into the library is found by
                // scanning this thread's stack for the first word that
                // points into this image just after a CALL, and the walk
                // resumes there. Plain reads of the suspended thread's own
                // stack; no function table is consulted.
                if (i != 0) break;
                const uint64_t* sp = reinterpret_cast<const uint64_t*>(ctx.Rsp);
                int k = 0;
                while (k < kScanWords && !(in_image(sp[k]) && after_call(sp[k])))
                    ++k;
                if (k == kScanWords) break;
                ctx.Rip = sp[k];
                ctx.Rsp = reinterpret_cast<uint64_t>(sp + k + 1);
                continue;
            }
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, nullptr);
            if (!fn) {
                // A leaf with no unwind entry: its return address is at [rsp].
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fn, &ctx,
                             &handler_data, &establisher, nullptr);
        }
    }
    ResumeThread(g_target);
    g_head.fetch_add(1, std::memory_order_release);
}

DWORD WINAPI sampler_main(LPVOID) {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(g_period_us) * 10;  // 100 ns units
    LARGE_INTEGER last_flush;
    QueryPerformanceCounter(&last_flush);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
        take_sample();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - last_flush.QuadPart > freq.QuadPart) {
            flush_records();
            last_flush = now;
        }
    }
    return 0;
}

struct Start {
    Start() {
        const char* on = std::getenv("LB_SAMPLE");
        if (!on || !*on || *on == '0') return;
        if (const char* us = std::getenv("LB_SAMPLE_US")) {
            const int v = std::atoi(us);
            if (v >= 100 && v <= 100000) g_period_us = static_cast<unsigned>(v);
        }
        if (const char* out = std::getenv("LB_SAMPLE_OUT"))
            std::snprintf(g_out, sizeof g_out, "%s", out);
        HMODULE self = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
        g_image_lo = reinterpret_cast<uint64_t>(self);
        g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;
        g_image_pref = nt->OptionalHeader.ImageBase;
        g_ring = static_cast<Record*>(
            VirtualAlloc(nullptr, sizeof(Record) * kRing,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_ring) return;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                             GetCurrentProcess(), &g_target,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_INFORMATION,
                             FALSE, 0))
            return;
        FILE* f = std::fopen(g_out, "wb");
        if (!f) return;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        const uint32_t frames = kFrames;
        const uint64_t qf = static_cast<uint64_t>(freq.QuadPart);
        std::fwrite("LBS1", 1, 4, f);
        std::fwrite(&frames, 4, 1, f);
        std::fwrite(&qf, 8, 1, f);
        std::fwrite(&g_image_pref, 8, 1, f);
        std::fclose(f);
        HANDLE t = CreateThread(nullptr, 0, sampler_main, nullptr, 0, nullptr);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_TIME_CRITICAL);
            CloseHandle(t);
        }
        std::fprintf(stderr,
                     "[lb_prof] sampling the main thread every %u us -> %s\n",
                     g_period_us, g_out);
    }
} g_start;

}  // namespace

// docs/358: the compositor's row workers at the emulation thread's priority.
// The frontend raises the emulation thread with
// SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH), which on Windows is
// SetThreadPriority(THREAD_PRIORITY_HIGHEST); this is the same call for a
// thread SDL did not create. Here because this file already speaks Win32.
extern "C" void lb_thread_priority_high(int on) {
    SetThreadPriority(GetCurrentThread(),
                      on ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
}

// The 3D render thread (melonDS's SoftRenderer), which the emulation thread
// waits for in GetLine. It calls lb_note_render_thread() first thing; the
// handle lets the emulation thread change its priority later (LB_POOLAB=
// render).
//
// LEFT AT NORMAL PRIORITY BY DEFAULT: measured worse raised (docs/358 s5).
// Interleaved in-run under a six-process load, frames over 20 ms numbered
// 1,644 with it raised against 1,526 at normal (11 pairs, paired t 2.5), and
// the quiet runs leaned the same way. LB_RENDERPRIO=1 raises it.
namespace {
// Written by the render thread, read by the emulation thread. A handle from
// an earlier render thread (the renderer can be set up again) is left open
// rather than closed under a reader: one handle per setup.
std::atomic<HANDLE> g_render_thread{nullptr};
}
extern "C" void lb_render_thread_priority_high(int on) {
    const HANDLE h = g_render_thread.load(std::memory_order_acquire);
    if (h)
        SetThreadPriority(h, on ? THREAD_PRIORITY_HIGHEST
                                : THREAD_PRIORITY_NORMAL);
}
extern "C" void lb_note_render_thread(void) {
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &h,
                         THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    g_render_thread.store(h, std::memory_order_release);
    const char* v = std::getenv("LB_RENDERPRIO");
    if (v && v[0] == '1') lb_render_thread_priority_high(1);
}

#else

extern "C" void lb_thread_priority_high(int) {}
extern "C" void lb_render_thread_priority_high(int) {}
extern "C" void lb_note_render_thread(void) {}

#endif  // _WIN32
