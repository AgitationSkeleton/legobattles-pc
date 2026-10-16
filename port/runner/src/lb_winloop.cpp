// lb_winloop.cpp - the game keeps running while its window is moved.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// THE FAULT (docs/385). The game froze while its window was being moved,
// which is a problem for online matches. Pressing on a window's title bar or border, or opening its system menu,
// makes DefWindowProc run a MODAL LOOP of its own -- WM_ENTERSIZEMOVE (or
// WM_ENTERMENULOOP) ... WM_EXITSIZEMOVE -- and it runs inside the dispatch of
// that press, which is inside the frontend's SDL_PollEvent. Nothing returns to
// the frame loop until the button is released: emulation, presentation, audio
// feeding and the network all stop. The network is on this thread too
// (lb_net.cpp: "the emulation thread may be inside sendto/recvfrom"), so a
// lockstep peer simply waits on this machine for as long as the title bar is
// held.
//
// THE FIX MOVES THE PUMP, NOT THE GAME. Emulation cannot leave this thread:
// SDL's renderer, the GL context and the windows all belong to it. So the
// frontend's `while (SDL_PollEvent(&e))` becomes lb_winloop_pump() followed by
// SDL_PeepEvents to drain the queue, and lb_winloop_pump() runs SDL_PumpEvents
// on a second FIBER of the same thread. Each SDL window's procedure is
// subclassed here: WM_ENTERSIZEMOVE / WM_ENTERMENULOOP start a timer, and each
// WM_TIMER the modal loop dispatches switches back to the main fiber -- the
// frame loop runs a frame, paced by its own deadline as ever, and its next
// pump switches back into the modal loop where it was. WM_EXIT* stops the
// timer. Outside a modal loop nothing is different: the pump fiber pumps once
// and switches back, which is what SDL_PollEvent's first call did.
//
// It is the timer SDL3 itself uses for the same modal loops; SDL2 has no
// equivalent for an application with its own frame loop.
//
// LB_WINLOOP=0 turns it off (the pump runs on the main fiber, as before).
// LB_WINLOOPLOG=1 logs each modal loop and the frames that ran inside it.
//
// THE HARNESS cannot drag a title bar without moving the desktop cursor, so a
// window posted WM_APP+0x4C (wParam = milliseconds) runs a nested loop shaped
// like DefWindowProc's -- WM_ENTERSIZEMOVE, dispatch until done,
// WM_EXITSIZEMOVE, all inside that message's dispatch. What it does not
// exercise is only Windows' own promise that its modal loops dispatch
// WM_TIMER to the window, which is what SDL3's live resize rests on.

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <SDL.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <SDL_syswm.h>

namespace {

LPVOID g_main_fiber = nullptr;
LPVOID g_pump_fiber = nullptr;
bool g_failed = false;
int g_modal = 0;                 // modal loops in progress
HWND g_timer_hwnd = nullptr;
unsigned g_modal_frames = 0;     // frames run inside the current one
uint64_t g_modal_t0 = 0;
constexpr UINT_PTR kTimerId = 0x4C42u;   // 'LB'
// A modal loop of our own, for the harness: WM_APP + 0x4C with wParam = ms.
constexpr UINT kTestMessage = WM_APP + 0x4Cu;

struct Sub {
    HWND hwnd;
    WNDPROC prev;
};
Sub g_sub[4];
int g_nsub = 0;

bool log_on() {
    static const bool on = std::getenv("LB_WINLOOPLOG") != nullptr;
    return on;
}

bool disabled() {
    static const bool off = [] {
        const char* v = std::getenv("LB_WINLOOP");
        return v && v[0] == '0';
    }();
    return off;
}

WNDPROC prev_of(HWND h) {
    for (int i = 0; i < g_nsub; ++i)
        if (g_sub[i].hwnd == h) return g_sub[i].prev;
    return nullptr;
}

void CALLBACK pump_proc(LPVOID) {
    for (;;) {
        SDL_PumpEvents();
        SwitchToFiber(g_main_fiber);
    }
}

void modal_enter(HWND h, const char* what) {
    if (g_modal++ == 0) {
        g_timer_hwnd = h;
        g_modal_frames = 0;
        g_modal_t0 = SDL_GetTicks64();
        SetTimer(h, kTimerId, USER_TIMER_MINIMUM, nullptr);
        if (log_on())
            std::fprintf(stderr, "[lb_winloop] %s: %s\n", what,
                         g_pump_fiber ? "the frame loop keeps running"
                                      : "the frame loop waits (no pump fiber)");
    }
}

void modal_leave() {
    if (g_modal > 0 && --g_modal == 0) {
        if (g_timer_hwnd) KillTimer(g_timer_hwnd, kTimerId);
        g_timer_hwnd = nullptr;
        if (log_on())
            std::fprintf(stderr, "[lb_winloop] modal loop left after %llu ms; "
                         "%u frames ran inside it\n",
                         (unsigned long long)(SDL_GetTicks64() - g_modal_t0),
                         g_modal_frames);
    }
}

// Back to the frame loop, if this is the pump fiber inside a modal loop. The
// frame loop's next lb_winloop_pump() returns here.
void yield_to_frame_loop() {
    if (g_modal > 0 && g_pump_fiber && GetCurrentFiber() == g_pump_fiber)
        SwitchToFiber(g_main_fiber);
}

LRESULT CALLBACK sub_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC prev = prev_of(h);
    switch (m) {
        case WM_ENTERSIZEMOVE:
            modal_enter(h, "window move/size loop entered");
            break;
        case WM_ENTERMENULOOP:
            modal_enter(h, "window menu loop entered");
            break;
        case WM_EXITSIZEMOVE:
        case WM_EXITMENULOOP:
            modal_leave();
            break;
        case WM_TIMER:
            if (w == kTimerId) {
                yield_to_frame_loop();
                return 0;
            }
            break;
        case kTestMessage: {
            // A nested loop shaped like DefWindowProc's: ENTER, dispatch
            // until done, EXIT -- all inside this message's dispatch.
            SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
            const uint64_t end = SDL_GetTicks64() + (uint64_t)w;
            MSG msg;
            while (SDL_GetTicks64() < end) {
                if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                } else {
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 5,
                                              QS_ALLINPUT);
                }
            }
            SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
            return 0;
        }
        case WM_NCDESTROY: {
            for (int i = 0; i < g_nsub; ++i) {
                if (g_sub[i].hwnd != h) continue;
                g_sub[i] = g_sub[--g_nsub];
                break;
            }
            if (g_timer_hwnd == h) {
                g_modal = 1;
                modal_leave();
            }
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)prev);
            break;
        }
        default:
            break;
    }
    return prev ? CallWindowProcW(prev, h, m, w, l)
                : DefWindowProcW(h, m, w, l);
}

void install(SDL_Window* win) {
    if (!win) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) ||
        info.subsystem != SDL_SYSWM_WINDOWS)
        return;
    const HWND h = info.info.win.window;
    if (!h || prev_of(h)) return;
    if (g_nsub >= (int)(sizeof g_sub / sizeof g_sub[0])) return;
    const WNDPROC prev = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!prev || prev == sub_proc) return;
    g_sub[g_nsub++] = {h, prev};
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)sub_proc);
    if (log_on())
        std::fprintf(stderr, "[lb_winloop] window %p subclassed\n", (void*)h);
}

bool init() {
    if (g_pump_fiber) return true;
    if (g_failed) return false;
    g_main_fiber = IsThreadAFiber()
                       ? GetCurrentFiber()
                       : ConvertThreadToFiberEx(nullptr,
                                                FIBER_FLAG_FLOAT_SWITCH);
    if (g_main_fiber)
        g_pump_fiber = CreateFiberEx(64 * 1024, 4 * 1024 * 1024,
                                     FIBER_FLAG_FLOAT_SWITCH, pump_proc,
                                     nullptr);
    if (!g_pump_fiber) {
        g_failed = true;
        std::fprintf(stderr, "[lb_winloop] no fibers (error %lu); a moved "
                     "window will pause the game\n", GetLastError());
        return false;
    }
    return true;
}

}  // namespace

// The frontend's pump, in place of SDL_PollEvent's. The caller drains the
// queue with SDL_PeepEvents afterwards.
extern "C" void lb_winloop_pump(SDL_Window* a, SDL_Window* b) {
    // The subclass is installed either way, so LB_WINLOOP=0 still answers
    // the harness's modal loop -- by freezing, as the old pump did.
    install(a);
    if (b != a) install(b);
    if (disabled() || !init()) {
        SDL_PumpEvents();
        return;
    }
    if (g_modal > 0) ++g_modal_frames;
    SwitchToFiber(g_pump_fiber);
}

#else  // !_WIN32

extern "C" void lb_winloop_pump(SDL_Window*, SDL_Window*) {
    SDL_PumpEvents();
}

#endif
