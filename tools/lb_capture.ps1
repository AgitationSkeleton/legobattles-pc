# Capture the port's actual window to a PNG.
#
# Every measurement in this project until now was taken either headless (which
# skips rendering entirely, so it cannot show lag, banding or a misplaced HUD) or
# from the debug server's framebuffer command (which returns a GUEST SCREEN, not
# the composited window). Neither can see what the player sees, which is why
# faults visible within seconds of launching survived several rounds of "fixed".
#
#   powershell -ExecutionPolicy Bypass -File tools/lb_capture.ps1 -Out shot.png
#   powershell ... -Out shot.png -Width 1920 -Height 1080   # resize first
#
# Uses PrintWindow so an occluded window still captures, and only moves the
# window when a size is explicitly asked for.

# -OwnerPid RESTRICTS THIS TO ONE PROCESS, and experiments must pass it.
#
# Find() takes the first VISIBLE window whose title contains the needle, and
# every instance of the port has the same title. So when a game was already
# open and a harness run started, this resized and photographed THAT window --
# MoveWindow to 60,60 at whatever size the sweep wanted, stretching it and
# switching which view was on which screen. The measurements were also being
# taken from a window someone was playing in, which is not the configuration
# under test.
param(
    [string]$Out = "window.png",
    # The fallback needle, used only when no -OwnerPid is given. "ndsrecomp"
    # was the recompiler's placeholder window title; the port now names itself.
    [string]$Title = "LEGO Battles",
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$OwnerPid = 0,
    # RESIZE WITHOUT PHOTOGRAPHING.
    #
    # The end-of-run sweep resizes the window through eight sizes to measure
    # magnification and field of view, and it was screenshotting every one --
    # eight stretched PNGs per run that nobody reads, a waste of time and
    # space. The MEASUREMENT is worth keeping and the pictures are not, so the
    # two are now separable.
    [switch]$NoShot,
    # REPORT THE CHROME INSTEAD OF GUESSING IT.
    #
    # capture() photographs the OUTER window rect (GetWindowRect, because
    # PrintWindow wants that bitmap), while frontend_input takes CLIENT pixels
    # -- so every coordinate read off a capture is offset from every coordinate
    # clicked by the border and title bar. That offset was being inferred from
    # the difference between the capture size and the [lb_geom] window size and
    # then split in half on the assumption that the borders match, which is a
    # guess about a number a measurement of where a click lands depends on
    # completely. This prints both rects so the caller can subtract them.
    [switch]$Info
)

Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int t, bool r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static IntPtr Find(string needle, int ownerPid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            if (ownerPid > 0) {
                int wpid; GetWindowThreadProcessId(h, out wpid);
                if (wpid != ownerPid) return true;
            }
            int len = GetWindowTextLength(h);
            if (len == 0) return true;
            // A MATCHING PID IS ENOUGH; THE TITLE IS ONLY THE FALLBACK.
            //
            // This required BOTH, so renaming the window broke every resize and
            // every window capture at once, silently: Find() returned zero, the
            // caller discarded stderr, and a run that never resized went on to
            // report timings for a window nobody asked for. The pid identifies
            // the process exactly; the title is a guess that goes stale the
            // moment the product is named.
            if (ownerPid > 0) { found = h; return false; }
            var sb = new System.Text.StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) {
                found = h; return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing, System.Windows.Forms

$h = [Win]::Find($Title, $OwnerPid)
if ($h -eq [IntPtr]::Zero) { Write-Error "no visible window matching '$Title' for pid $OwnerPid"; exit 2 }

if ($Width -gt 0 -and $Height -gt 0) {
    # KEEP THE WINDOW ON THE DISPLAY IT IS ALREADY ON.
    #
    # This moved it to screen (60, 60), which is the top-left of the PRIMARY
    # display. The port can be told to open on a secondary display (it logs
    # the "[sdl] display N: WxH at X,Y" it chose and centres there), and then
    # every resize in the sweep dragged the window back onto the primary
    # display, cluttering it with test windows.
    #
    # Resizing about the window's CURRENT top-left leaves it wherever the port
    # put it. The 60,60 inset is kept only as the fallback for a window whose
    # rect cannot be read.
    $rc = New-Object Win+RECT
    $x = 60
    $y = 60
    if ([Win]::GetWindowRect($h, [ref]$rc)) {
        $x = $rc.Left
        $y = $rc.Top
    }
    # AND IT HAS TO FIT ON THAT DISPLAY.
    #
    # Keeping the current top-left stopped the sweep dragging the window onto
    # the primary display, and then broke every large capture instead. The
    # port centres itself on its display, so its top-left is well inside it
    # -- measured 444 px from the display's left edge and 315 px from its top
    # on a 1920x1080 display. Growing to 1920x1080 from there put the window's
    # right and bottom edges off the screen, and PrintWindow only fills the
    # ON-SCREEN part of the bitmap: the rest comes back as uninitialised memory.
    #
    # Every 1920x1080 capture in that run showed content in exactly
    # x 1..1475, y 1..764 -- 1920-444 = 1476 and 1080-315 = 765, the visible
    # rectangle -- with a block of RGB noise past it. That was read as "vanilla
    # rendering is broken", and vanilla was fine; the port's own 256x192 dumps
    # were clean the whole time. The give-away was that a 1600x900 capture had
    # the SAME content extent as a 1920x1080 one, which no real rendering
    # difference can produce.
    #
    # So pull the window back until it fits, rather than silently photographing
    # a window that is hanging off the edge.
    $scr = [System.Windows.Forms.Screen]::FromHandle($h).Bounds
    if ($x + $Width  -gt $scr.Right)  { $x = $scr.Right  - $Width }
    if ($y + $Height -gt $scr.Bottom) { $y = $scr.Bottom - $Height }
    if ($x -lt $scr.Left) { $x = $scr.Left }
    if ($y -lt $scr.Top)  { $y = $scr.Top }
    [void][Win]::MoveWindow($h, $x, $y, $Width, $Height, $true)
    Start-Sleep -Milliseconds 900   # let the frontend see the resize and redraw
}

if ($NoShot) {
    $r0 = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$r0)
    # POSITION AS WELL AS SIZE. The window must stay on the display it opened
    # on, and the only way to know it did is to print where it actually is:
    # an x outside that display's span is the sweep having dragged it back
    # onto the primary display.
    Write-Output ("resized  {0}x{1} at {2},{3}" -f `
        ($r0.Right - $r0.Left), ($r0.Bottom - $r0.Top), $r0.Left, $r0.Top)
    exit 0
}

$r = New-Object Win+RECT
[void][Win]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$t = $r.Bottom - $r.Top
if ($w -le 0 -or $t -le 0) { Write-Error "window has no size"; exit 3 }

if ($Info) {
    $cr = New-Object Win+RECT
    [void][Win]::GetClientRect($h, [ref]$cr)
    $o = New-Object Win+POINT
    [void][Win]::ClientToScreen($h, [ref]$o)
    # The numbers the caller needs: how far the client area's top-left sits
    # inside the captured bitmap. A capture pixel is a client pixel plus this.
    Write-Output ("INFO window={0},{1} {2}x{3} client={4}x{5} origin={6},{7} chrome={8},{9}" -f `
        $r.Left, $r.Top, $w, $t, ($cr.Right - $cr.Left), ($cr.Bottom - $cr.Top), `
        $o.X, $o.Y, ($o.X - $r.Left), ($o.Y - $r.Top))
    exit 0
}

# SAY SO WHEN THE PICTURE CANNOT BE COMPLETE.
#
# PrintWindow fills only the on-screen part of the bitmap and leaves the rest
# as whatever was in memory, which arrives as plausible-looking noise rather
# than as an error. A capture of a window hanging off its display is not
# evidence about anything, and the only way to know is to check -- so check,
# every time, and put it in the output the caller already reads.
$sb = [System.Windows.Forms.Screen]::FromHandle($h).Bounds
if ($r.Left -lt $sb.Left -or $r.Top -lt $sb.Top -or
    $r.Right -gt $sb.Right -or $r.Bottom -gt $sb.Bottom) {
    Write-Output ("WARNING: window {0},{1} {2}x{3} extends past display {4} -- " +
                  "PrintWindow will return uninitialised pixels outside it; " +
                  "this capture is NOT evidence" -f `
                  $r.Left, $r.Top, $w, $t, $sb.ToString())
}

$bmp = New-Object System.Drawing.Bitmap $w, $t
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 2 = PW_RENDERFULLCONTENT, which captures hardware-composited client areas that
# the older flag returns blank for.
[void][Win]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc)
$g.Dispose()
# AN MSYS PATH IS NOT A WINDOWS PATH, AND A FAILED SAVE IS NOT A CAPTURE.
#
# tools/lb_exp.py runs under whichever python is first on PATH, and that can be
# devkitPro's MSYS build, whose paths read /x/... (drive letter first).
# GDI+ cannot save to that ("A generic error occurred in GDI+"), and this script
# used to go on and print the path and size and exit 0 anyway. So every window
# capture the harness took under MSYS python was silently empty (found
# 2026-09-24: --select-hero's shots never existed). Convert /x/... to x:/...,
# and exit 4 if the save still fails.
if ($Out -match '^/([a-zA-Z])/(.*)$') { $Out = "$($Matches[1]):/$($Matches[2])" }
try {
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
} catch {
    $bmp.Dispose()
    Write-Error "could not save $Out : $($_.Exception.Message)"
    exit 4
}
$bmp.Dispose()
Write-Output ("{0}  {1}x{2}" -f $Out, $w, $t)
