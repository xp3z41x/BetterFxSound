"""Close the processes that use a file through the Windows Restart Manager, without forcing.

This is what Inno Setup's CloseApplications=yes does before it replaces files, so it shows
whether an installer can close a running FxSound gracefully (its shutdown() then runs:
the Windows default device and the devices' own volumes are handed back).

    python tools/audiolab/rm_close.py "C:\\Program Files\\BetterFxSound\\FxSound.exe"

Exit code 0 when every process using the file exited; prints what the app logged.
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

rm = ctypes.WinDLL("rstrtmgr")


class RM_UNIQUE_PROCESS(ctypes.Structure):
    _fields_ = [("dwProcessId", wt.DWORD), ("ProcessStartTime", wt.FILETIME)]


class RM_PROCESS_INFO(ctypes.Structure):
    _fields_ = [("Process", RM_UNIQUE_PROCESS), ("strAppName", wt.WCHAR * 256),
                ("strServiceShortName", wt.WCHAR * 64), ("ApplicationType", ctypes.c_int),
                ("AppStatus", wt.ULONG), ("TSSessionId", wt.DWORD), ("bRestartable", wt.BOOL)]


def main(path):
    log = os.path.join(os.environ["APPDATA"], "FxSound", "fxsound.log")
    log_size = os.path.getsize(log) if os.path.exists(log) else 0

    session = wt.DWORD()
    key = ctypes.create_unicode_buffer(64)
    err = rm.RmStartSession(ctypes.byref(session), 0, key)
    if err:
        raise SystemExit(f"RmStartSession failed: {err}")
    try:
        files = (wt.LPCWSTR * 1)(path)
        err = rm.RmRegisterResources(session, 1, files, 0, None, 0, None)
        if err:
            raise SystemExit(f"RmRegisterResources failed: {err}")
        needed, count, reasons = wt.UINT(), wt.UINT(16), wt.DWORD()
        infos = (RM_PROCESS_INFO * 16)()
        err = rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), infos, ctypes.byref(reasons))
        if err:
            raise SystemExit(f"RmGetList failed: {err}")
        pids = [infos[i].Process.dwProcessId for i in range(count.value)]
        for i in range(count.value):
            print(f"in use by pid {pids[i]} '{infos[i].strAppName}' type={infos[i].ApplicationType}")
        if not pids:
            print("no process uses the file")
            return 0
        t0 = time.time()
        err = rm.RmShutdown(session, 0, None)  # 0 = no RmForceShutdown
        print(f"RmShutdown -> {err} after {time.time() - t0:.1f}s")
    finally:
        rm.RmEndSession(session)

    alive = []
    for pid in pids:
        h = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if h:
            still = ctypes.windll.kernel32.WaitForSingleObject(h, 10000) != 0
            ctypes.windll.kernel32.CloseHandle(h)
            if still:
                alive.append(pid)
    print("still running:", alive or "none")
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            f.seek(log_size)
            print("--- fxsound.log since the request ---")
            print(f.read().strip() or "(nothing)")
    return 1 if alive else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
