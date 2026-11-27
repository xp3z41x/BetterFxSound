"""
lr_probe.py - measure what each output device really plays from the left and the right channel.

Plays a stereo test signal into the Windows default playback device (normally the FxSound
virtual device, so it goes through FxSound like any app): a 750 Hz tone on the LEFT channel
only and a 1250 Hz tone on the RIGHT channel only, at the same time. Meanwhile it records the
WASAPI loopback of every real output device (what Windows mixed for that endpoint, that is what
FxSound rendered to it) and measures, in short windows, how much of each tone every channel of
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

Optional triggers run FxSound command-line options at given times during the capture, so a
transient leak after "disable output", "power off/on" and so on shows up in the time series:

  python lr_probe.py list
  python lr_probe.py run --seconds 8
  python lr_probe.py run --seconds 12 --at "3:--power=0" --at "6:--power=1"
  python lr_probe.py run --seconds 12 --at "3:--disable-output=Speakers (X)" --json out.json

Needs: pip install pyaudiowpatch numpy. Plays sound: take the audio-lab lock first (see
docs/DEBUG_LAB.md). Changes nothing by itself except what the --at triggers do.
"""

import argparse
import json
import os
import subprocess
import sys
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
    """Real output endpoints with their loopback twins: [(name, out_info, loopback_info)]."""
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


def render_sessions():
    """Audio sessions on every active playback endpoint: [(name, roles, [(process, pid, state)])].
    A process other than FxSound with an active session on a real device plays there directly,
    bypassing FxSound and its channel modes. Needs pycaw and psutil; returns [] without them."""
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


def bypassing(sessions, own_pid):
    """Active sessions of other processes on real (non-FxSound) devices."""
    found = []
    for name, roles, ss in sessions:
        if "FxSound" in name:
            continue
        for pname, pid, state in ss:
            if state == "active" and pname.lower() != "fxsound.exe" and pid != own_pid:
                found.append(f"{pname} (pid {pid}) on {name}")
    return found


def cmd_sessions(_args):
    for name, roles, ss in render_sessions():
        print(f"{name}{'  [default ' + ', '.join(roles) + ']' if roles else ''}")
        for pname, pid, state in ss:
            print(f"    {pname:28s} pid {pid:6d}  {state}")


def fxsound_status(fxsound):
    """Runs FxSound --status and returns {device name: output dict} from status.json."""
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


class Recorder:
    def __init__(self, p, info, label):
        self.label = label
        self.rate = int(info["defaultSampleRate"])
        self.channels = int(info["maxInputChannels"])
        self.chunks = []        # (t_first_frame, array[frames, channels])
        self.lock = threading.Lock()
        self.stream = None
        self.p = p
        self.info = info

    def _cb(self, in_data, frame_count, time_info, status):
        import pyaudiowpatch as pa
        now = time.perf_counter()
        a = np.frombuffer(in_data, dtype=np.float32).reshape(-1, self.channels).copy()
        with self.lock:
            self.chunks.append((now - frame_count / self.rate, a))
        return (None, pa.paContinue)

    def start(self):
        import pyaudiowpatch as pa
        self.stream = self.p.open(format=pa.paFloat32, channels=self.channels, rate=self.rate, input=True,
                                  input_device_index=self.info["index"], frames_per_buffer=512,
                                  stream_callback=self._cb)

    def stop(self):
        if self.stream is not None:
            self.stream.stop_stream()
            self.stream.close()

    def windows(self, t0):
        """Yields (t_rel, block[frames, channels]) on a regular grid of WINDOW_S, aligned to the
        first captured frame; gaps (no rendering) are simply missing windows."""
        with self.lock:
            chunks = list(self.chunks)
        if not chunks:
            return
        n = int(WINDOW_S * self.rate)
        # The loopback stream only delivers while something renders to the endpoint, so split
        # the capture into contiguous segments (by callback time) and window each one.
        segments = []
        for t, a in chunks:
            if segments:
                seg = segments[-1]
                expected = seg[0] + seg[2] / self.rate
                if abs(t - expected) < 0.05:
                    seg[1].append(a)
                    seg[2] += a.shape[0]
                    continue
            segments.append([t, [a], a.shape[0]])
        for start_t, parts, _count in segments:
            data = np.concatenate(parts, axis=0)
            for k in range(0, data.shape[0] - n + 1, n):
                yield (start_t - t0 + k / self.rate, data[k:k + n])


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
    """Returns (leak_db or None, note). leak_db is the unwanted tone relative to the wanted one."""
    if mode == "left":
        return float(db(rv.max()) - db(lv.min())), ""
    if mode == "right":
        return float(db(lv.max()) - db(rv.min())), ""
    if mode == "mono":
        # Both tones should be equal everywhere; report the worst imbalance as a positive number.
        diff = np.abs(db(lv) - db(rv))
        return float(-diff.max()) if diff.max() < 1e9 else None, "mono: -|L-R| dB"
    if mode == "both" and nch >= 2:
        # Crossfeed of each tone into the other channel (FxSound effects do this by design).
        x0 = db(rv[0]) - db(lv[0])
        x1 = db(lv[1]) - db(rv[1])
        return float(max(x0, x1)), "both: crossfeed"
    return None, ""


def cmd_run(args):
    import pyaudiowpatch as pa

    triggers = []
    for spec in args.at or []:
        t, _, opt = spec.partition(":")
        triggers.append((float(t), opt))
    triggers.sort()

    modes, status = ({}, {}) if args.no_status else fxsound_status(args.fxsound)

    p = pa.PyAudio()
    try:
        api = wasapi_info(p)
        default = p.get_device_info_by_index(api["defaultOutputDevice"])
        devices = output_devices(p)
        if args.only:
            devices = [d for d in devices if any(s.lower() in d[0].lower() for s in args.only)]

        recorders = []
        for name, out, loop in devices:
            if not args.include_default and name == default["name"]:
                rec = Recorder(p, loop, name + " (virtual, input reference)")
            else:
                rec = Recorder(p, loop, name)
            recorders.append(rec)

        play_rate = int(default["defaultSampleRate"])
        phase = [0]
        stop_play = threading.Event()

        def play_cb(in_data, frame_count, time_info, st):
            i = phase[0] + np.arange(frame_count)
            phase[0] += frame_count
            left = TONE_LEVEL * np.sin(2 * np.pi * F_LEFT * i / play_rate)
            right = TONE_LEVEL * np.sin(2 * np.pi * F_RIGHT * i / play_rate)
            if args.silent:
                left[:] = 0
                right[:] = 0
            out = np.empty((frame_count, 2), dtype=np.float32)
            out[:, 0] = left
            out[:, 1] = right
            return (out.tobytes(), pa.paComplete if stop_play.is_set() else pa.paContinue)

        for r in recorders:
            r.start()
        t0 = time.perf_counter()
        player = p.open(format=pa.paFloat32, channels=2, rate=play_rate, output=True,
                        output_device_index=default["index"], frames_per_buffer=512, stream_callback=play_cb)
        print(f"Playing L={F_LEFT:.0f} Hz / R={F_RIGHT:.0f} Hz into '{default['name']}' for {args.seconds} s")

        fired = []
        for t, opt in triggers:
            delay = t - (time.perf_counter() - t0)
            if delay > 0:
                time.sleep(delay)
            ts = time.perf_counter() - t0
            print(f"  t={ts:5.2f}s trigger: FxSound {opt}")
            subprocess.Popen([args.fxsound, opt])
            fired.append((ts, opt))
        rest = args.seconds - (time.perf_counter() - t0)
        if rest > 0:
            time.sleep(rest)

        # Who else plays straight to a real device while the test sound runs.
        bypass = bypassing(render_sessions(), os.getpid())

        stop_play.set()
        time.sleep(0.2)
        player.stop_stream()
        player.close()
        for r in recorders:
            r.stop()
    finally:
        p.terminate()

    if triggers and not args.no_status:
        modes_after, _ = fxsound_status(args.fxsound)
    else:
        modes_after = modes

    report = {"left_hz": F_LEFT, "right_hz": F_RIGHT, "window_s": WINDOW_S, "triggers": fired,
              "bypassing_sessions": bypass, "devices": []}
    for r in recorders:
        name = r.label.split(" (virtual")[0]
        o = modes.get(name, {})
        mode = o.get("channel_mode")
        if "(virtual" in r.label:
            mode = "both"
        enabled = o.get("enabled")
        rows = []
        for t, block in r.windows(t0):
            lv, rv = tone_levels(block, r.rate)
            peak = max(lv.max(), rv.max())
            silent = db(peak) < SILENT_DBFS
            leak, note = (None, "") if silent else judge(mode, lv, rv, r.channels)
            rows.append({"t": round(t, 3), "silent": bool(silent), "left_db": [round(float(x), 1) for x in db(lv)],
                         "right_db": [round(float(x), 1) for x in db(rv)], "leak_db": None if leak is None else round(leak, 1)})
        report["devices"].append({"name": r.label, "mode": mode, "enabled": enabled, "channels": r.channels,
                                  "rate": r.rate, "windows": rows})

    print_report(report, args.threshold)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        print("wrote", args.json)


def print_report(report, threshold):
    print()
    for b in report.get("bypassing_sessions", []):
        print("BYPASS: plays directly to a real device, FxSound's channel mode cannot apply:", b)
    print(f"{'device':45s} {'mode':6s} {'en':3s} {'ch':>2s} {'win':>4s} {'sil':>4s}  "
          f"{'L tone dB/ch':>18s} {'R tone dB/ch':>18s}  {'leak med':>8s} {'leak max':>8s} {'bad':>4s}")
    for d in report["devices"]:
        rows = d["windows"]
        audible = [w for w in rows if not w["silent"]]
        leaks = [w["leak_db"] for w in audible if w["leak_db"] is not None]
        mid = audible[len(audible) // 2] if audible else None
        lstr = "/".join(f"{x:.0f}" for x in mid["left_db"]) if mid else "-"
        rstr = "/".join(f"{x:.0f}" for x in mid["right_db"]) if mid else "-"
        # "bad" only makes sense for left/right modes, where the leak should be very low.
        bad = [w for w in audible if d["mode"] in ("left", "right") and w["leak_db"] is not None and w["leak_db"] > threshold]
        med = f"{np.median(leaks):8.1f}" if leaks else "       -"
        mx = f"{max(leaks):8.1f}" if leaks else "       -"
        en = "" if d["enabled"] is None else ("on" if d["enabled"] else "off")
        print(f"{d['name'][:45]:45s} {str(d['mode']):6s} {en:3s} {d['channels']:2d} {len(rows):4d} {len(rows) - len(audible):4d}  "
              f"{lstr:>18s} {rstr:>18s}  {med} {mx} {len(bad):4d}")
        if bad:
            spans = []
            for w in bad:
                if spans and abs(w["t"] - spans[-1][1] - report["window_s"]) < 1e-6 + report["window_s"] / 2:
                    spans[-1][1] = w["t"]
                    spans[-1][2] = max(spans[-1][2], w["leak_db"])
                else:
                    spans.append([w["t"], w["t"], w["leak_db"]])
            for a, b, worst in spans[:12]:
                print(f"    leak > {threshold} dB from t={a:.2f}s to t={b + report['window_s']:.2f}s, worst {worst:.1f} dB")


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
    r.add_argument("--include-default", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.cmd == "list":
        cmd_list(args)
    elif args.cmd == "sessions":
        cmd_sessions(args)
    else:
        cmd_run(args)


if __name__ == "__main__":
    sys.exit(main())
