"""Save PNG captures of FxSound's visible windows (main window, popups, menus), no packages needed.

    python tools/audiolab/winshot.py <out-dir> [prefix]

Captures every visible top-level window of FxSound.exe with PrintWindow (works when covered)
and writes <prefix>-<n>-<title>.png. Prints what it saved.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: real pixels


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def png(path, w, h, bgra):
    rows = []
    for y in range(h):
        row = bgra[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        rows.append(b"\x00" + bytes(rgb))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


def capture(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    hdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    ok = user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT
    if not ok:  # fall back to the screen pixels
        sdc = user32.GetDC(0)
        gdi32.BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, 0x00CC0020)
        user32.ReleaseDC(0, sdc)
    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return w, h, buf.raw, (r.left, r.top)


def fxsound_windows():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq FxSound.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    pids = {int(l.split('","')[1]) for l in out.splitlines() if l.count('","') >= 2}
    found = []
    proc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            t = ctypes.create_unicode_buffer(128)
            user32.GetWindowTextW(hwnd, t, 128)
            found.append((hwnd, t.value))
        return True

    user32.EnumWindows(proc(cb), 0)
    return found


if __name__ == "__main__":
    out_dir = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "fx"
    os.makedirs(out_dir, exist_ok=True)
    for i, (hwnd, title) in enumerate(fxsound_windows()):
        shot = capture(hwnd)
        if not shot:
            continue
        w, h, data, pos = shot
        if w < 8 or h < 8:
            continue
        name = f"{prefix}-{i}-{''.join(c for c in title if c.isalnum()) or 'untitled'}.png"
        png(os.path.join(out_dir, name), w, h, data)
        print(f"saved {name} {w}x{h} at {pos}")
