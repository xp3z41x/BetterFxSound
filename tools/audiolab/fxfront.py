"""Bring FxSound's main window to the foreground without synthetic input.

JUCE dismisses a CallOutBox (outputs popup, output settings) at once when its app is not the
foreground app, so UI Automation clicks on a background FxSound open popups that vanish.
A real click activates the app first; this does the same for scripted tests.

    python tools/audiolab/fxfront.py
"""
import ctypes
import ctypes.wintypes as wt

import winshot

u = ctypes.windll.user32
k = ctypes.windll.kernel32


def bring_to_front():
    target = next((h for h, t in winshot.fxsound_windows() if t == "FxSound"), None)
    if target is None:
        raise SystemExit("FxSound's main window is not visible")
    fg = u.GetForegroundWindow()
    fg_tid = u.GetWindowThreadProcessId(fg, None)
    my_tid = k.GetCurrentThreadId()
    attached = u.AttachThreadInput(my_tid, fg_tid, True) if fg_tid != my_tid else False
    u.BringWindowToTop(target)
    ok = u.SetForegroundWindow(target)
    if attached:
        u.AttachThreadInput(my_tid, fg_tid, False)
    return ok, target, u.GetForegroundWindow()


if __name__ == "__main__":
    ok, target, fg = bring_to_front()
    print("SetForegroundWindow:", bool(ok), "target", target, "foreground now", fg)
