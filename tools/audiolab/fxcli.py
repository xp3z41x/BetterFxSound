"""Drive a running FxSound through its command line and read status.json back.

Used by the UI/CLI debug pass (docs/DEBUG_LAB.md). Take the audio lab lock first.

    python tools/audiolab/fxcli.py status                 # print the outputs from --status
    python tools/audiolab/fxcli.py send '<raw args>'      # forward a raw command line, then print outputs
    python tools/audiolab/fxcli.py matrix [--report FILE] # run the CLI edge-case matrix (restores what it changes)

The raw command line is passed to CreateProcess verbatim (no Python quoting), so it
reaches FxSound exactly as typed, like from cmd.exe.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

EXE = os.environ.get("FXSOUND_EXE", r"C:\Program Files\BetterFxSound\FxSound.exe")
STATUS = os.path.join(os.environ["APPDATA"], "FxSound", "status.json")


def run_raw(args, timeout=15):
    """Runs FxSound.exe with args appended verbatim to the command line."""
    cmdline = f'"{EXE}" {args}' if args else f'"{EXE}"'
    t0 = time.time()
    p = subprocess.Popen(cmdline, cwd=os.path.dirname(EXE))
    try:
        rc = p.wait(timeout)
    except subprocess.TimeoutExpired:
        return None, time.time() - t0
    return rc, time.time() - t0


def status(timeout=4.0):
    before = os.stat(STATUS).st_mtime_ns if os.path.exists(STATUS) else 0
    run_raw("--status")
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(STATUS) and os.stat(STATUS).st_mtime_ns != before:
            time.sleep(0.05)
            with open(STATUS, encoding="utf-8") as f:
                return json.load(f)
        time.sleep(0.05)
    raise RuntimeError("status.json not updated (is FxSound running?)")


def outputs(st=None):
    st = st or status()
    return {o["name"]: o for o in st["outputs"]}


def fmt(o):
    return f'{"on " if o["enabled"] else "off"} {"present" if o["present"] else "GONE   "} vol={o["volume"]:3} delay={o["delay_ms"]:3} {o["channel_mode"]}'


def send(args, settle=0.4):
    rc, dt = run_raw(args)
    time.sleep(settle)
    return rc, dt


user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def fxsound_windows():
    """Visible top-level windows of FxSound.exe processes: [(hwnd, title, visible)]."""
    pids = set()
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq FxSound.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[1].isdigit():
            pids.add(int(parts[1]))
    found = []

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids:
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            found.append((hwnd, buf.value, bool(user32.IsWindowVisible(hwnd))))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found


def main_window_visible():
    return any(t == "FxSound" and v for _, t, v in fxsound_windows())


def matrix(report=None):
    lines = []

    def log(s):
        print(s)
        lines.append(s)

    base = outputs()
    log("baseline:")
    for n, o in base.items():
        log(f"  {n!r}: {fmt(o)}")
    enabled = [n for n, o in base.items() if o["enabled"] and o["present"]]
    disabled = [n for n, o in base.items() if not o["enabled"] and o["present"]]
    if not enabled:
        raise SystemExit("needs an enabled, connected output")
    tgt = enabled[-1]
    off = disabled[0] if disabled else None
    b = base[tgt]

    cases = [
        # (label, raw args, expected change: dict field->value on tgt, or None for "nothing changes")
        ("volume, whole value quoted", f'--output-volume="{tgt}:80"', {"volume": 80}),
        ("volume, back to baseline", f'--output-volume="{tgt}:{b["volume"]}"', {"volume": b["volume"]}),
        ("delay, whole value quoted", f'--output-delay="{tgt}:40"', {"delay_ms": 40}),
        ("delay, back to baseline", f'--output-delay="{tgt}:{b["delay_ms"]}"', {"delay_ms": b["delay_ms"]}),
        ("volume, cmd.exe style quotes around the name only", f'--output-volume="{tgt}":70', {"volume": 70}),
        ("volume, back to baseline", f'--output-volume="{tgt}:{b["volume"]}"', {"volume": b["volume"]}),
        ("volume, whole argument quoted (PowerShell style)", f'"--output-volume={tgt}:75"', {"volume": 75}),
        ("volume, back to baseline", f'--output-volume="{tgt}:{b["volume"]}"', {"volume": b["volume"]}),
        ("volume, space before number", f'--output-volume="{tgt}: 65"', {"volume": 65}),
        ("volume, back to baseline", f'--output-volume="{tgt}:{b["volume"]}"', {"volume": b["volume"]}),
        ("volume, leading zeros 0080", f'--output-volume="{tgt}:0080"', {"volume": 80}),
        ("volume, back to baseline", f'--output-volume="{tgt}:{b["volume"]}"', {"volume": b["volume"]}),
        ("volume 101 (out of range)", f'--output-volume="{tgt}:101"', None),
        ("volume -1", f'--output-volume="{tgt}:-1"', None),
        ("volume abc", f'--output-volume="{tgt}:abc"', None),
        ("volume empty number", f'--output-volume="{tgt}:"', None),
        ("volume 99999", f'--output-volume="{tgt}:99999"', None),
        ("volume 50.5", f'--output-volume="{tgt}:50.5"', None),
        ("volume without number", f'--output-volume="{tgt}"', None),
        ("delay 501 (out of range)", f'--output-delay="{tgt}:501"', None),
        ("volume, space instead of =", f'--output-volume "{tgt}:50"', None),
        ("volume, unknown name", '--output-volume="No Such Device:50"', None),
        ("volume, unknown name with ':'", '--output-volume="Dock: Line Out:50"', None),
        ("volume, unknown accented name", f'--output-volume="{tgt}\u00e9:50"', None),
        ("volume, name with different case", f'--output-volume="{tgt.upper()}:50"', None),
        ("enable, unknown name", '--enable-output="No Such Device"', None),
        ("disable, unknown name", '--disable-output="No Such Device"', None),
        ("output, unknown name", '--output="No Such Device"', None),
        ("enable an already enabled output", f'--enable-output="{tgt}"', None),
    ]
    results = []
    for label, args, expect in cases:
        before = outputs()
        win_before = main_window_visible()
        rc, dt = send(args)
        after = outputs()
        win_after = main_window_visible()
        changed = {n: {k: (before[n][k], after[n][k]) for k in ("enabled", "volume", "delay_ms", "channel_mode") if before.get(n, {}).get(k) != after[n][k]}
                   for n in after if n not in before or any(before[n][k] != after[n][k] for k in ("enabled", "volume", "delay_ms", "channel_mode"))}
        if expect is None:
            ok = not changed
        else:
            ok = all(after[tgt][k] == v for k, v in expect.items()) and set(changed) <= {tgt}
        results.append(ok)
        log(f"[{'ok ' if ok else 'BAD'}] {label}: {args}  rc={rc} {dt:.1f}s  changed={changed}  window {win_before}->{win_after}")

    if off:
        for label, args, expect_enabled in [
            ("enable a disabled output", f'--enable-output="{off}"', True),
            ("disable it again", f'--disable-output="{off}"', False),
            ("enable and disable in one command line", f'--enable-output="{off}" --disable-output="{off}"', False),
        ]:
            rc, dt = send(args, settle=1.0)
            got = outputs()[off]["enabled"]
            ok = got == expect_enabled
            results.append(ok)
            log(f"[{'ok ' if ok else 'BAD'}] {label}: {args}  rc={rc} {dt:.1f}s  enabled={got}")

    final = outputs()
    drift = {n: fmt(final[n]) for n in base if n in final and fmt(final[n]) != fmt(base[n])}
    log(f"drift from baseline after the matrix: {drift or 'none'}")
    log(f"{sum(results)}/{len(results)} as expected")
    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "status":
        for n, o in outputs().items():
            print(f"{n!r}: {fmt(o)}")
    elif cmd == "send":
        print(send(sys.argv[2]))
        for n, o in outputs().items():
            print(f"{n!r}: {fmt(o)}")
    elif cmd == "windows":
        print(fxsound_windows())
    elif cmd == "matrix":
        rep = sys.argv[sys.argv.index("--report") + 1] if "--report" in sys.argv else None
        matrix(rep)
