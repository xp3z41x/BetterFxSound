"""
devvol.py - read, set, save and put back the endpoint volume (level and mute) of playback devices.

The engine sets every enabled output's real device to 100% and unmuted while it uses it, and puts the
device's own level and mute back when it stops (power off, output removed, exit). This tool checks that.

  python devvol.py list                         # every active playback device: level, mute
  python devvol.py set "JBL Clip 4" 37 [0|1]   # level in percent, optional mute
  python devvol.py save <file.json>            # all active playback devices
  python devvol.py restore <file.json>         # put back what save stored (present devices only)
  python devvol.py watch <seconds> [interval]  # print a line whenever a level or mute changes

Device names match by substring, case-insensitively, and must match exactly one device.
Needs: pip install pycaw. Changes device volumes: take the audio-lab lock first (docs/DEBUG_LAB.md).
"""

import json
import sys
import time

from pycaw.pycaw import AudioUtilities


def playback_devices():
    out = []
    for d in AudioUtilities.GetAllDevices():
        if "Active" not in str(d.state) or not d.id.startswith("{0.0.0."):
            continue
        ev = getattr(d, "EndpointVolume", None)
        if ev is None:
            continue
        out.append((d, ev))
    return out


def state():
    result = {}
    for d, ev in playback_devices():
        try:
            result[d.id] = {"name": d.FriendlyName, "level": round(ev.GetMasterVolumeLevelScalar() * 100.0, 2),
                            "mute": int(ev.GetMute())}
        except Exception as e:  # device went away meanwhile
            result[d.id] = {"name": d.FriendlyName, "error": repr(e)}
    return result


def find(name):
    hits = [(d, ev) for d, ev in playback_devices() if name.lower() in d.FriendlyName.lower()]
    if len(hits) != 1:
        raise SystemExit(f"'{name}' matches {len(hits)} active playback devices")
    return hits[0]


def show(s):
    for i, v in s.items():
        if "error" in v:
            print(f"  {v['name']}: {v['error']}")
        else:
            print(f"  {v['name']}: {v['level']:6.2f}%  {'MUTED' if v['mute'] else ''}")


def main(argv):
    if not argv or argv[0] == "list":
        show(state())
    elif argv[0] == "set":
        d, ev = find(argv[1])
        ev.SetMasterVolumeLevelScalar(float(argv[2]) / 100.0, None)
        if len(argv) > 3:
            ev.SetMute(int(argv[3]), None)
        print(f"  {d.FriendlyName}: {ev.GetMasterVolumeLevelScalar() * 100:.2f}% mute={ev.GetMute()}")
    elif argv[0] == "save":
        s = state()
        with open(argv[1], "w", encoding="utf-8") as f:
            json.dump(s, f, indent=2, ensure_ascii=False)
        show(s)
    elif argv[0] == "restore":
        with open(argv[1], encoding="utf-8") as f:
            saved = json.load(f)
        for d, ev in playback_devices():
            v = saved.get(d.id)
            if not v or "error" in v:
                continue
            ev.SetMasterVolumeLevelScalar(v["level"] / 100.0, None)
            ev.SetMute(v["mute"], None)
        show(state())
    elif argv[0] == "watch":
        end = time.time() + float(argv[1])
        interval = float(argv[2]) if len(argv) > 2 else 0.05
        last = {}
        t0 = time.time()
        while time.time() < end:
            s = state()
            for i, v in s.items():
                key = (v.get("level"), v.get("mute"))
                if last.get(i) != key:
                    print(f"{time.time() - t0:7.2f}s  {v['name']}: {v.get('level')}% mute={v.get('mute')}", flush=True)
                    last[i] = key
            time.sleep(interval)
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
