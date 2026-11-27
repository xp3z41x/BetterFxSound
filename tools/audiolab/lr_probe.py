"""
lr_probe.py - measure what each output device really plays from the left and the right channel.

Plays a stereo test signal into the Windows default playback device (normally the FxSound
virtual device, so it goes through FxSound like any app): a 750 Hz tone on the LEFT channel
only and a 1250 Hz tone on the RIGHT channel only, at the same time. Meanwhile it records the
WASAPI loopback of every playback device (what Windows mixed for that endpoint, that is what
FxSound rendered to it) and measures, in 100 ms windows, how much of each tone every channel of
every device carries.

For each device the channel mode FxSound reports in status.json is the expectation:
  left   -> only the 750 Hz tone, on every channel; the 1250 Hz tone is a leak
  right  -> only the 1250 Hz tone, on every channel; the 750 Hz tone is a leak
  mono   -> both tones, equal, on every channel
  both   -> 750 Hz on channel 0 and 1250 Hz on channel 1 (crossfeed reported, not a failure)

"leak" = level of the tone that should be absent relative to the one that should be present,
in dB, worst channel. With FxSound's Surround/Ambience effects on, the processed left channel
legitimately contains some right-channel content (mid/side widening, stereo reverb); zero
those effects (--set_effect="surround:0,ambience:0") to measure the routing alone.

The player and the recorders run in their own processes and reopen their streams when a device
changes format or comes and goes, and the player follows the Windows default device like an
ordinary app (so with FxSound off it plays straight to the real device). Reopens and underflows
of the player are logged; output windows right after one are tagged "src" and not counted.

Optional triggers run FxSound command-line options at given times during the capture, so a
transient leak after "disable output", "power off/on" and so on shows up in the time series:

  python lr_probe.py list
  python lr_probe.py sessions
  python lr_probe.py run --seconds 8
  python lr_probe.py run --seconds 12 --at "3:--power=0" --at "6:--power=1"
  python lr_probe.py run --seconds 12 --at "3:--disable-output=Speakers (X)" --json out.json
  python lr_probe.py run --seconds 12 --at "3:!Disable-PnpDevice -InstanceId '<endpoint>' -Confirm:$false" ...

A trigger starting with "!" is run as a PowerShell command (disabling and enabling an audio
endpoint with Disable-PnpDevice/Enable-PnpDevice, as admin, simulates unplugging it).

Needs: pip install pyaudiowpatch numpy (and pycaw psutil for the session list). Plays sound:
take the audio-lab lock first (see docs/DEBUG_LAB.md). Changes nothing by itself except what
the --at triggers do.
"""

import argparse
import json
import os
import pickle
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

F_LEFT = 750.0
F_RIGHT = 1250.0
TONE_LEVEL = 0.25          # -12 dBFS per tone
WINDOW_S = 0.1             # analysis window
SILENT_DBFS = -70.0        # below this a window counts as silent for the device
DEFAULT_FXSOUND = r"C:\Program Files\BetterFxSound\FxSound.exe"


def db(x):
    return 20.0 * np.log10(np.maximum(x, 1e-12))


def wasapi_info(p):
    import pyaudiowpatch as pa
    return p.get_host_api_info_by_type(pa.paWASAPI)


def output_devices(p):
    """Output endpoints with their loopback twins: [(name, out_info, loopback_info)]."""
    api = wasapi_info(p)["index"]
    outs = {}
    loops = {}
    for i in range(p.get_device_count()):
        d = p.get_device_info_by_index(i)
        if d["hostApi"] != api:
            continue
        if d.get("isLoopbackDevice"):
            loops[d["name"].replace(" [Loopback]", "")] = d
        elif d["maxOutputChannels"] > 0:
            outs[d["name"]] = d
    return [(n, outs[n], loops[n]) for n in outs if n in loops]


def cmd_list(_args):
    import pyaudiowpatch as pa
    p = pa.PyAudio()
    try:
        default = p.get_device_info_by_index(wasapi_info(p)["defaultOutputDevice"])["name"]
        print("Windows default output:", default)
        for name, out, loop in output_devices(p):
            print(f"  {name}: {loop['maxInputChannels']} ch loopback @ {int(loop['defaultSampleRate'])} Hz")
    finally:
        p.terminate()


# ----------------------------------------------------------------------------------------------
# Sessions: who plays straight to a real device

def render_sessions():
    """Audio sessions on every active playback endpoint: [(name, roles, [(process, pid, state)])].
    A process other than FxSound with an active session on a real device plays there directly,
    bypassing FxSound and its channel modes. Loopback recorders show up as sessions too.
    Needs pycaw and psutil; returns [] without them."""
    try:
        import comtypes
        import psutil
        from comtypes import CLSCTX_ALL
        from pycaw.pycaw import AudioUtilities, IAudioSessionControl2, IAudioSessionManager2
    except ImportError:
        return []
    comtypes.CoInitialize()
    enum = AudioUtilities.GetDeviceEnumerator()
    roles = {}
    for role, rname in ((0, "console"), (1, "multimedia"), (2, "communications")):
        try:
            roles[rname] = enum.GetDefaultAudioEndpoint(0, role).GetId()
        except Exception:  # noqa: BLE001
            roles[rname] = None
    out = []
    coll = enum.EnumAudioEndpoints(0, 1)  # eRender, DEVICE_STATE_ACTIVE
    for i in range(coll.GetCount()):
        dev = coll.Item(i)
        dev_id = dev.GetId()
        name = AudioUtilities.CreateDevice(dev).FriendlyName
        sessions = []
        try:
            mgr = dev.Activate(IAudioSessionManager2._iid_, CLSCTX_ALL, None).QueryInterface(IAudioSessionManager2)
            se = mgr.GetSessionEnumerator()
            for k in range(se.GetCount()):
                ctl = se.GetSession(k).QueryInterface(IAudioSessionControl2)
                pid = ctl.GetProcessId()
                state = ["inactive", "active", "expired"][ctl.GetState()]
                try:
                    pname = psutil.Process(pid).name() if pid else "system"
                except Exception:  # noqa: BLE001
                    pname = f"pid {pid}"
                sessions.append((pname, pid, state))
        except Exception:  # noqa: BLE001
            pass
        out.append((name, [r for r, v in roles.items() if v == dev_id], sessions))
    return out


def bypassing(sessions, own_pids):
    """Active sessions of other processes on real (non-FxSound) devices."""
    found = []
    for name, roles, ss in sessions:
        if "FxSound" in name:
            continue
        for pname, pid, state in ss:
            if state == "active" and pname.lower() != "fxsound.exe" and pid not in own_pids:
                found.append(f"{pname} (pid {pid}) on {name}")
    return found


def cmd_sessions(_args):
    for name, roles, ss in render_sessions():
        print(f"{name}{'  [default ' + ', '.join(roles) + ']' if roles else ''}")
        for pname, pid, state in ss:
            print(f"    {pname:28s} pid {pid:6d}  {state}")


def default_render_name():
    """Friendly name of the Windows default playback device, or None (needs pycaw)."""
    try:
        import comtypes
        from pycaw.pycaw import AudioUtilities
        comtypes.CoInitialize()
        enum = AudioUtilities.GetDeviceEnumerator()
        return AudioUtilities.CreateDevice(enum.GetDefaultAudioEndpoint(0, 1)).FriendlyName
    except Exception:  # noqa: BLE001
        return None


# ----------------------------------------------------------------------------------------------
# FxSound state

def fxsound_status(fxsound):
    """Runs FxSound --status and returns ({device name: output dict}, status) from status.json."""
    path = os.path.expandvars(r"%APPDATA%\FxSound\status.json")
    try:
        before = os.path.getmtime(path)
    except OSError:
        before = 0
    try:
        subprocess.run([fxsound, "--status"], timeout=10)
    except Exception as e:  # noqa: BLE001
        print("warning: FxSound --status failed:", e)
    for _ in range(40):
        try:
            if os.path.getmtime(path) != before:
                break
        except OSError:
            pass
        time.sleep(0.1)
    try:
        with open(path, encoding="utf-8") as f:
            status = json.load(f)
    except Exception as e:  # noqa: BLE001
        print("warning: cannot read status.json:", e)
        return {}, {}
    return {o["name"]: o for o in status.get("outputs", [])}, status


# ----------------------------------------------------------------------------------------------
# Workers (own processes, so they can re-enumerate devices and reopen streams)

def cmd_record(args):
    """Records the loopback of one device until args.until (perf_counter), reopening as needed.
    Saves [(t_first_frame, rate, array[frames, channels])] with pickle to args.out."""
    import pyaudiowpatch as pa
    chunks = []
    lock = threading.Lock()
    while time.perf_counter() < args.until:
        p = pa.PyAudio()
        stream = None
        try:
            loop = None
            for name, _out, lb in output_devices(p):
                if name == args.name:
                    loop = lb
            if loop is not None:
                rate = int(loop["defaultSampleRate"])
                ch = int(loop["maxInputChannels"])

                def cb(in_data, frame_count, time_info, status, rate=rate, ch=ch):
                    now = time.perf_counter()
                    a = np.frombuffer(in_data, dtype=np.float32).reshape(-1, ch).copy()
                    with lock:
                        chunks.append((now - frame_count / rate, rate, a))
                    return (None, pa.paContinue)

                stream = p.open(format=pa.paFloat32, channels=ch, rate=rate, input=True,
                                input_device_index=loop["index"], frames_per_buffer=512, stream_callback=cb)
                while time.perf_counter() < args.until and stream.is_active():
                    time.sleep(0.05)
        except Exception:  # noqa: BLE001
            pass
        finally:
            if stream is not None:
                try:
                    stream.stop_stream()
                    stream.close()
                except Exception:  # noqa: BLE001
                    pass
            p.terminate()
        if time.perf_counter() < args.until:
            time.sleep(0.3)
    with lock, open(args.out, "wb") as f:
        pickle.dump(chunks, f)


def cmd_play(args):
    """Plays the test tones into the Windows default device until args.until, following default
    device changes and reopening after format changes. Prints one JSON event per line."""
    import pyaudiowpatch as pa

    def event(kind, **kw):
        kw.update(ev=kind, t=time.perf_counter())
        print(json.dumps(kw), flush=True)

    phase = [0]
    while time.perf_counter() < args.until:
        p = pa.PyAudio()
        stream = None
        try:
            default = p.get_device_info_by_index(wasapi_info(p)["defaultOutputDevice"])
            rate = int(default["defaultSampleRate"])
            underflows = []

            def cb(in_data, frame_count, time_info, status, rate=rate):
                if status & pa.paOutputUnderflow:
                    underflows.append(time.perf_counter())
                i = phase[0] + np.arange(frame_count)
                phase[0] += frame_count
                out = np.empty((frame_count, 2), dtype=np.float32)
                swapped = args.swap_at is not None and time.perf_counter() >= args.swap_at
                if args.silent:
                    out[:] = 0
                else:
                    out[:, 1 if swapped else 0] = TONE_LEVEL * np.sin(2 * np.pi * F_LEFT * i / rate)
                    out[:, 0 if swapped else 1] = TONE_LEVEL * np.sin(2 * np.pi * F_RIGHT * i / rate)
                return (out.tobytes(), pa.paContinue)

            stream = p.open(format=pa.paFloat32, channels=2, rate=rate, output=True,
                            output_device_index=default["index"], frames_per_buffer=1024, stream_callback=cb)
            event("open", device=default["name"], rate=rate)
            last_check = time.perf_counter()
            while time.perf_counter() < args.until and stream.is_active():
                time.sleep(0.02)
                while underflows:
                    print(json.dumps({"ev": "underflow", "t": underflows.pop(0)}), flush=True)
                if time.perf_counter() - last_check > 0.1:
                    last_check = time.perf_counter()
                    name = default_render_name()
                    if name is not None and name != default["name"]:
                        event("default-changed", device=name)
                        break
        except Exception as e:  # noqa: BLE001
            event("error", error=str(e))
        finally:
            if stream is not None:
                try:
                    stream.stop_stream()
                    stream.close()
                except Exception:  # noqa: BLE001
                    pass
            p.terminate()
            event("closed")
        if time.perf_counter() < args.until:
            time.sleep(0.05)


# ----------------------------------------------------------------------------------------------
# Analysis

def windows(chunks, t0):
    """Yields (t_rel, rate, block[frames, channels]) on a grid of WINDOW_S per contiguous segment.
    The loopback stream only delivers while something renders to the endpoint."""
    segments = []
    for t, rate, a in chunks:
        if segments:
            seg = segments[-1]
            expected = seg[0] + seg[3] / seg[1]
            if seg[1] == rate and seg[2][-1].shape[1] == a.shape[1] and abs(t - expected) < 0.05:
                seg[2].append(a)
                seg[3] += a.shape[0]
                continue
        segments.append([t, rate, [a], a.shape[0]])
    for start_t, rate, parts, _count in segments:
        data = np.concatenate(parts, axis=0)
        n = int(WINDOW_S * rate)
        for k in range(0, data.shape[0] - n + 1, n):
            yield (start_t - t0 + k / rate, rate, data[k:k + n])


def tone_levels(block, rate):
    """Amplitude of the F_LEFT and F_RIGHT tones on each channel: arrays [channels], [channels]."""
    n = block.shape[0]
    w = np.hanning(n)
    norm = 2.0 / w.sum()
    t = np.arange(n) / rate
    out = []
    for f in (F_LEFT, F_RIGHT):
        ref = w * np.exp(-2j * np.pi * f * t)
        out.append(np.abs(ref @ block) * norm)
    return out[0], out[1]


def judge(mode, lv, rv, nch):
    """Returns leak_db or None: the unwanted tone relative to the wanted one (worst channel).
    An unwanted tone below SILENT_DBFS is not counted (the click of a stream starting or
    stopping spreads a little energy everywhere); the leak is then reported as at most -60 dB."""
    if mode == "left":
        if db(rv.max()) < SILENT_DBFS:
            return float(min(db(rv.max()) - db(lv.min()), -60.0))
        return float(db(rv.max()) - db(lv.min()))
    if mode == "right":
        if db(lv.max()) < SILENT_DBFS:
            return float(min(db(lv.max()) - db(rv.min()), -60.0))
        return float(db(lv.max()) - db(rv.min()))
    if mode == "mono":
        # Both tones should be equal everywhere; the worst imbalance, as a negative number.
        return float(-np.abs(db(lv) - db(rv)).max())
    if mode == "both" and nch >= 2:
        # Crossfeed of each tone into the other channel (FxSound effects do this by design).
        return float(max(db(rv[0]) - db(lv[0]), db(lv[1]) - db(rv[1])))
    return None


def cmd_run(args):
    import pyaudiowpatch as pa

    triggers = []
    for spec in args.at or []:
        t, _, opt = spec.partition(":")
        triggers.append((float(t), opt))
    triggers.sort()

    modes, _status = ({}, {}) if args.no_status else fxsound_status(args.fxsound)

    p = pa.PyAudio()
    try:
        default_name = p.get_device_info_by_index(wasapi_info(p)["defaultOutputDevice"])["name"]
        devices = [d[0] for d in output_devices(p)]
    finally:
        p.terminate()
    if args.only:
        devices = [d for d in devices if any(s.lower() in d.lower() for s in args.only)]

    tmp = tempfile.mkdtemp(prefix="lr_probe_")
    t0 = time.perf_counter() + 1.5           # time for the workers to start
    until = t0 + args.seconds
    me = os.path.abspath(__file__)
    procs = []
    files = {}
    for i, name in enumerate(devices):
        files[name] = os.path.join(tmp, f"rec{i}.pkl")
        procs.append(subprocess.Popen([sys.executable, me, "record", "--name", name, "--until", repr(until + 0.3),
                                       "--out", files[name]]))
    play_args = [sys.executable, me, "play", "--until", repr(until)]
    if args.silent:
        play_args.append("--silent")
    if args.swap_at is not None:
        play_args += ["--swap-at", repr(t0 + args.swap_at)]
    player = subprocess.Popen(play_args, stdout=subprocess.PIPE, text=True)
    player_events = []
    reader = threading.Thread(target=lambda: [player_events.append(json.loads(line)) for line in player.stdout if line.strip()],
                              daemon=True)
    reader.start()

    # Times a file changes (FxSound saves its settings right when a setting is changed in the UI).
    file_changes = []
    if args.watch_file:
        def watch():
            last = None
            while time.perf_counter() < until:
                try:
                    m = os.stat(args.watch_file).st_mtime_ns
                except OSError:
                    m = None
                if last is not None and m != last:
                    file_changes.append(round(time.perf_counter() - t0, 3))
                last = m
                time.sleep(0.005)
        threading.Thread(target=watch, daemon=True).start()

    while time.perf_counter() < t0:
        time.sleep(0.01)
    print(f"Playing L={F_LEFT:.0f} Hz / R={F_RIGHT:.0f} Hz into the Windows default device for {args.seconds} s")

    fired = []
    for t, opt in triggers:
        delay = t - (time.perf_counter() - t0)
        if delay > 0:
            time.sleep(delay)
        ts = time.perf_counter() - t0
        if opt.startswith("!"):
            # A PowerShell command instead of an FxSound option, e.g. disabling an audio endpoint.
            print(f"  t={ts:5.2f}s trigger: {opt[1:]}")
            subprocess.Popen(["powershell", "-NoProfile", "-Command", opt[1:]])
        else:
            print(f"  t={ts:5.2f}s trigger: FxSound {opt}")
            subprocess.Popen([args.fxsound, opt])
        fired.append((round(ts, 2), opt))
    while time.perf_counter() < until - 1.0:
        time.sleep(0.05)
    # Who else plays straight to a real device while the test sound runs.
    bypass = bypassing(render_sessions(), {os.getpid(), player.pid} | {pr.pid for pr in procs})

    player.wait(timeout=30)
    reader.join(timeout=5)
    for pr in procs:
        pr.wait(timeout=30)

    if triggers and not args.no_status:
        modes_after, _ = fxsound_status(args.fxsound)
    else:
        modes_after = modes

    # Moments the test sound itself was not clean: player (re)opened, closed, or ran late.
    src_events = [(e["t"] - t0, e["ev"], e.get("device", "")) for e in player_events]
    glitch_times = [t for t, ev, _ in src_events if ev in ("open", "closed", "underflow", "error", "default-changed")]
    # Spans where the player, following the Windows default, played straight to a real device:
    # every app that follows the default does the same then, and FxSound cannot apply anything.
    direct = []
    current = None
    for t, ev, dev in src_events:
        if ev == "open":
            current = (t, dev)
        elif ev == "closed" and current is not None:
            if "FxSound" not in current[1]:
                direct.append((current[0], t, current[1]))
            current = None

    report = {"left_hz": F_LEFT, "right_hz": F_RIGHT, "window_s": WINDOW_S, "triggers": fired,
              "player_events": [(round(t, 3), ev, d) for t, ev, d in src_events],
              "file_changes": file_changes,
              "bypassing_sessions": bypass, "devices": []}
    for name in devices:
        try:
            with open(files[name], "rb") as f:
                chunks = pickle.load(f)
        except Exception:  # noqa: BLE001
            chunks = []
        o = modes.get(name, {})
        virtual = "FxSound" in name
        mode = "both" if virtual else o.get("channel_mode")
        mode_after = "both" if virtual else modes_after.get(name, {}).get("channel_mode")
        rows = []
        for t, rate, block in windows(chunks, t0):
            if t < 0 or t > args.seconds:
                continue
            lv, rv = tone_levels(block, rate)
            silent = db(max(lv.max(), rv.max())) < SILENT_DBFS
            leak = None if silent else judge(mode, lv, rv, block.shape[1])
            src = any(-0.15 <= t - g <= 0.6 for g in glitch_times)
            is_direct = any(dev == name and a - 0.05 <= t <= b + 0.3 for a, b, dev in direct)
            rows.append({"t": round(t, 3), "silent": bool(silent), "src": bool(src), "direct": bool(is_direct), "rate": rate,
                         "left_db": [round(float(x), 1) for x in db(lv)],
                         "right_db": [round(float(x), 1) for x in db(rv)],
                         "leak_db": None if leak is None else round(leak, 1)})
        report["devices"].append({"name": name + (" (virtual: FxSound input)" if virtual else ""),
                                  "mode": mode, "mode_after": mode_after, "enabled": o.get("enabled"),
                                  "windows": rows})

    print_report(report, args.threshold)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        print("wrote", args.json)


def print_report(report, threshold):
    print()
    for b in report.get("bypassing_sessions", []):
        print("BYPASS (plays straight to a real device, no FxSound channel mode):", b)
    for t, ev, d in report.get("player_events", []):
        if ev != "underflow":
            print(f"  player t={t:6.2f}s {ev} {d}")
    print(f"{'device':45s} {'mode':6s} {'en':3s} {'win':>4s} {'sil':>4s}  "
          f"{'L tone dB/ch':>14s} {'R tone dB/ch':>14s}  {'leak med':>8s} {'leak max':>8s} {'bad':>4s}")
    for d in report["devices"]:
        rows = d["windows"]
        audible = [w for w in rows if not w["silent"]]
        clean = [w for w in audible if not w["src"]]
        leaks = [w["leak_db"] for w in clean if w["leak_db"] is not None]
        mid = clean[len(clean) // 2] if clean else None
        lstr = "/".join(f"{x:.0f}" for x in mid["left_db"]) if mid else "-"
        rstr = "/".join(f"{x:.0f}" for x in mid["right_db"]) if mid else "-"
        # "bad" only makes sense for left/right modes, where the leak should be very low.
        bad = [w for w in clean if d["mode"] in ("left", "right") and w["leak_db"] is not None and w["leak_db"] > threshold]
        med = f"{np.median(leaks):8.1f}" if leaks else "       -"
        mx = f"{max(leaks):8.1f}" if leaks else "       -"
        en = "" if d["enabled"] is None else ("on" if d["enabled"] else "off")
        mode = str(d["mode"]) + ("" if d.get("mode_after") in (None, d["mode"]) else f"->{d['mode_after']}")
        print(f"{d['name'][:45]:45s} {mode:6s} {en:3s} {len(rows):4d} {len(rows) - len(audible):4d}  "
              f"{lstr:>14s} {rstr:>14s}  {med} {mx} {len(bad):4d}")
        if bad:
            spans = []
            for w in bad:
                if spans and w["t"] - spans[-1][1] < 1.5 * report["window_s"]:
                    spans[-1][1] = w["t"]
                    spans[-1][2] = max(spans[-1][2], w["leak_db"])
                else:
                    spans.append([w["t"], w["t"], w["leak_db"]])
            for a, b, worst in spans[:12]:
                print(f"    leak > {threshold:.0f} dB from t={a:.2f}s to t={b + report['window_s']:.2f}s, worst {worst:.1f} dB")
        direct_rows = [w for w in audible if w.get("direct")]
        if direct_rows:
            worst = max((w["leak_db"] for w in direct_rows if w["leak_db"] is not None), default=None)
            print(f"    apps played straight to this device (Windows default) from t={direct_rows[0]['t']:.2f}s "
                  f"to t={direct_rows[-1]['t'] + report['window_s']:.2f}s: L {direct_rows[0]['left_db']} "
                  f"R {direct_rows[0]['right_db']}" + (f", leak up to {worst:.1f} dB" if worst is not None else ""))
        gaps = []
        for prev, cur in zip(rows, rows[1:]):
            if cur["t"] - prev["t"] > 1.5 * report["window_s"]:
                gaps.append(f"{prev['t'] + report['window_s']:.2f}-{cur['t']:.2f}s")
        if gaps:
            print(f"    no audio rendered: {', '.join(gaps[:8])}")
    if report.get("triggers"):
        print("triggers:", ", ".join(f"t={t:.2f}s {o}" for t, o in report["triggers"]))
    if report.get("file_changes"):
        print("watched file changed at:", ", ".join(f"t={t:.3f}s" for t in report["file_changes"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("sessions", help="audio sessions per playback device (who bypasses FxSound)")
    r = sub.add_parser("run")
    r.add_argument("--seconds", type=float, default=6.0)
    r.add_argument("--at", action="append", help='"<seconds>:<FxSound option>", repeatable')
    r.add_argument("--only", action="append", help="record only devices whose name contains this text")
    r.add_argument("--json", help="write the full time series here")
    r.add_argument("--threshold", type=float, default=-30.0, help="leak above this (dB) counts as bad, default -30")
    r.add_argument("--fxsound", default=DEFAULT_FXSOUND)
    r.add_argument("--no-status", action="store_true", help="do not call FxSound --status")
    r.add_argument("--silent", action="store_true", help="play silence (to see what else plays)")
    r.add_argument("--swap-at", type=float, help="from this time on, play 750 Hz on the right and 1250 Hz on the "
                                                 "left (the analysis keeps the original expectation): stale audio replayed "
                                                 "after the swap then shows up as the other tone")
    r.add_argument("--watch-file", help="log when this file changes, e.g. %%APPDATA%%\\FxSound\\FxSound.settings, "
                                        "which FxSound saves the moment a setting is changed in its UI")
    rec = sub.add_parser("record", help=argparse.SUPPRESS)
    rec.add_argument("--name", required=True)
    rec.add_argument("--until", type=float, required=True)
    rec.add_argument("--out", required=True)
    pl = sub.add_parser("play", help=argparse.SUPPRESS)
    pl.add_argument("--until", type=float, required=True)
    pl.add_argument("--silent", action="store_true")
    pl.add_argument("--swap-at", type=float)
    args = ap.parse_args()
    {"list": cmd_list, "sessions": cmd_sessions, "run": cmd_run, "record": cmd_record, "play": cmd_play}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
