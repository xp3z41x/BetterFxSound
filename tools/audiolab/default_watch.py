"""
default_watch.py - poll the Windows default playback device every couple of milliseconds and report every change,
optionally while running FxSound.exe commands at given times. During a carrier switch the default must stay the
FxSound virtual device: while a real device is the default, apps play to it directly, unprocessed.

  python default_watch.py 12 --exe "C:\\...\\FxSound.exe" --at "2:--disable-output=\\"Fones (X)\\"" --at "7:--enable-output=\\"Fones (X)\\""

Needs: pip install pycaw comtypes. With --at it changes FxSound's outputs: take the audio-lab lock first.
"""

import argparse
import os
import subprocess
import time

import comtypes
from pycaw.pycaw import AudioUtilities
from pycaw.constants import EDataFlow, ERole


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", type=float)
    ap.add_argument("--exe", default=r"C:\Program Files\BetterFxSound\FxSound.exe")
    ap.add_argument("--at", action="append", default=[])
    a = ap.parse_args()
    names = {d.id: d.FriendlyName for d in AudioUtilities.GetAllDevices()}
    enum = AudioUtilities.GetDeviceEnumerator()
    trig = [[float(s.split(":", 1)[0]), s.split(":", 1)[1], False] for s in a.at]
    t0 = time.perf_counter()
    last = None
    since = t0
    away = []
    while True:
        now = time.perf_counter()
        if now - t0 > a.seconds:
            break
        for tr in trig:
            if not tr[2] and now - t0 >= tr[0]:
                tr[2] = True
                subprocess.Popen(f'"{a.exe}" {tr[1]}', cwd=os.path.dirname(a.exe))
                print(f"{now - t0:8.3f}s  FxSound.exe {tr[1]}", flush=True)
        try:
            dev = enum.GetDefaultAudioEndpoint(EDataFlow.eRender.value, ERole.eMultimedia.value)
            cur = dev.GetId()
        except comtypes.COMError:
            cur = "<none>"
        if cur != last:
            if last is not None and "FxSound" not in names.get(last, ""):
                away.append((since - t0, now - since, names.get(last, last)))
            print(f"{now - t0:8.3f}s  default -> {names.get(cur, cur)}", flush=True)
            last, since = cur, now
        time.sleep(0.002)
    print("real device as the default:", ", ".join(f"{n} for {d * 1000:.0f} ms at {s:.2f}s" for s, d, n in away) or "never")


if __name__ == "__main__":
    main()
