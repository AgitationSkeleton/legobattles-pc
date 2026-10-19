// lb_filedlg.cpp - a native file picker for the Saves tab.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// WHY THIS EXISTS
//
// The Saves tab shipped with a bare InputText and the caption "Full path to a
// .sav file. Type it in; there is no native file picker here." Save import
// needs a file picker. Asking someone to type an absolute Windows path into an ImGui
// text box to load a save is not a port, it is a debug console.
//
// WHY ITS OWN TRANSLATION UNIT
//
// <windows.h> cannot be included into lb_overlay.cpp. It defines min/max as
// macros, defines `near`/`far`, and on C++17 and later its `byte` collides with
// std::byte -- and lb_overlay.cpp is 2500 lines of ImGui that uses all of them.
// NOMINMAX and WIN32_LEAN_AND_MEAN suppress some of that but not the rest, and
// the suppressions are load-bearing in a way that breaks silently later.
//
// A separate TU with a two-function C interface costs one file and keeps the
// Windows headers entirely out of everything else.
//
// PORTABILITY. Non-Windows builds get a stub that returns 0, and the caller
// keeps the text field it already had, so nothing regresses if this is ever
// built on Linux.

#include <cstddef>
#include <string>
#include <ctime>
#include <cstdio>
#include <cstdlib>   /* legobattles-decomp: lb_open_session_log */

#ifdef _WIN32

// Order matters: the guards must precede the header, and commdlg.h needs the
// core header first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <commdlg.h>

namespace {

// The filter is a DOUBLE-NUL terminated list of NUL-separated pairs, which is
// why this is a wide string literal with embedded \0 rather than anything
// friendlier. Getting this wrong shows an empty file-type dropdown.
const wchar_t kSaveFilter[] =
    L"LEGO Battles save (*.sav)\0*.sav\0"
    L"All files (*.*)\0*.*\0";

// UTF-8 in, UTF-16 out, into a caller-owned buffer.
bool widen(const char* in, wchar_t* out, int out_chars) {
    if (!in || !in[0]) { out[0] = L'\0'; return true; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_chars);
    if (n <= 0) { out[0] = L'\0'; }
    return true;
}

}  // namespace

// saving == 0 opens an existing file, saving != 0 chooses a destination.
// Returns 1 and overwrites `buf` on success, 0 if the user cancelled or the
// dialog failed. `buf` is left untouched on failure, so a cancel keeps whatever
// the user had typed.
namespace {
int pick_file(char* buf, int cap, int saving, const wchar_t* filter,
              const wchar_t* ext);
}

extern "C" int lb_pick_save_file(char* buf, int cap, int saving) {
    return pick_file(buf, cap, saving, kSaveFilter, L"sav");
}

// docs/404: a Balance tab set, exported or imported.
extern "C" int lb_pick_json_file(char* buf, int cap, int saving) {
    static const wchar_t kJsonFilter[] =
        L"Balance values (*.json)\0*.json\0"
        L"All files (*.*)\0*.*\0";
    return pick_file(buf, cap, saving, kJsonFilter, L"json");
}

namespace {
int pick_file(char* buf, int cap, int saving, const wchar_t* filter,
              const wchar_t* ext) {
    if (!buf || cap < 2) return 0;

    wchar_t path[1024];
    widen(buf, path, 1024);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    // No owner window. Passing the SDL window's HWND would make the dialog
    // modal to it, which is tidier, but reaching SDL_GetWindowWMInfo from here
    // would drag SDL into this TU for a cosmetic gain.
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = ext;
    // OFN_NOCHANGEDIR matters: without it the dialog changes the PROCESS's
    // working directory to wherever the user browsed, and every later relative
    // path in the runner -- settings.json, the demo file, screenshots -- starts
    // resolving somewhere else. That is a genuinely nasty bug to trace back to
    // a file dialog.
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (saving) ofn.Flags |= OFN_OVERWRITEPROMPT;
    else        ofn.Flags |= OFN_FILEMUSTEXIST;

    const BOOL ok = saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return 0;   // includes plain cancel; CommDlgExtendedError() would
                         // distinguish a real failure, but there is nothing
                         // useful to do differently about one here.

    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, cap,
                                      NULL, NULL);
    if (n <= 0) return 0;
    buf[cap - 1] = '\0';
    return 1;
}
}  // namespace

// THE PORT KEEPS ITS OWN LOG, because the launcher cannot be relied on.
//
// Every diagnostic in this port writes to stderr -- the ITCM dispatch probes,
// the HUD decline counters, the placement footprint, the camera clamp. Players
// run `LegoBattles.exe` from build/ directly rather than through
// play.bat, and a process started that way has nowhere for stderr to go, so a
// crashed session produced NO log at all and the only evidence was one stray
// dispatch_misses.log. Redirecting inside play.bat fixed a launcher nobody
// uses.
//
// ONLY WHEN STDERR HAS NOWHERE TO GO. If it is already a disk file or a pipe,
// something upstream is capturing it deliberately -- the harness runs with
// stderr on a file and would lose every measurement if this stole it.
// GetFileType tells them apart: FILE_TYPE_DISK and FILE_TYPE_PIPE are left
// alone; a console, or no handle at all, is not.
//
// IT LIVES HERE for the reason this file exists at all, stated at the top:
// <windows.h> cannot go into lb_overlay.cpp or main.cpp. Both were tried and
// both cost a build.
extern "C" void lb_open_session_log(void) {
    if (std::getenv("LB_NO_SESSION_LOG")) return;
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        const DWORD t = GetFileType(h);
        if (t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE) return;
    }
    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return;
    std::string dir(exe);
    const size_t cut = dir.find_last_of("\\/");
    if (cut == std::string::npos) return;
    dir.resize(cut);
    const std::string logs = dir + "\\logs";
    CreateDirectoryA(logs.c_str(), NULL);
    const std::time_t now = std::time(NULL);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    char stamp[32] = {0};
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    const std::string path = logs + "\\session-" + stamp + ".log";
    if (std::freopen(path.c_str(), "w", stderr)) {
        // Line buffered: a crash must not lose the lines that explain it.
        std::setvbuf(stderr, NULL, _IOLBF, 4096);
        std::fprintf(stderr, "[lb_log] session log: %s\n", path.c_str());
        std::fflush(stderr);
        // docs/394: A DOUBLE-CLICK OPENS A CONSOLE FOR NOTHING. The exe is a
        // console program, so Explorer gives it a window of its own, which
        // sat behind the game for the whole session. When this process is the
        // console's only one -- a double-click, not a terminal -- and stderr
        // now goes to the log, the window has no use and is hidden. HIDDEN,
        // not freed: FreeConsole here cost every line after this one (the log
        // stopped at 166 bytes against 11,896 with the console kept).
        DWORD ids[2];
        const HWND con = GetConsoleWindow();
        if (!std::getenv("LB_KEEP_CONSOLE") && con &&
            GetConsoleProcessList(ids, 2) == 1)
            ShowWindow(con, SW_HIDE);
    }
}

// docs/394: the first-run setup's "choose your ROM". Same rules as the save
// picker above (no owner window, no directory change); UTF-8 out.
extern "C" int lb_pick_rom_file(char* buf, int cap) {
    if (!buf || cap < 2) return 0;
    static const wchar_t kRomFilter[] =
        L"Nintendo DS ROM (*.nds)\0*.nds\0"
        L"All files (*.*)\0*.*\0";
    wchar_t path[1024] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = kRomFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Choose your LEGO Battles ROM (Europe or USA)";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST |
                OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return 0;
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, cap, NULL, NULL);
    if (n <= 0) return 0;
    buf[cap - 1] = '\0';
    return 1;
}

#else   // !_WIN32

extern "C" int lb_pick_save_file(char* buf, int cap, int saving) {
    (void)buf; (void)cap; (void)saving;
    return 0;   // caller falls back to the text field
}

extern "C" int lb_pick_json_file(char* buf, int cap, int saving) {
    (void)buf; (void)cap; (void)saving;
    return 0;
}

extern "C" int lb_pick_rom_file(char* buf, int cap) {
    (void)buf; (void)cap;
    return 0;   // drag the file onto the window instead
}

extern "C" void lb_open_session_log(void) {}

#endif  // _WIN32
