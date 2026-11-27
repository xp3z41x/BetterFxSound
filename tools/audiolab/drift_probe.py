"""
drift_probe.py - latency, drift, clicks and dropouts of every FxSound output over a long run.

Plays a test signal into the Windows default playback device (normally the FxSound virtual device,
so it goes through FxSound like any app): a quiet continuous 1 kHz tone plus a short 2-8 kHz chirp
marker every --period seconds. Meanwhile it records the WASAPI loopback of the FxSound virtual device
(the reference: what the apps played) and of every real output device given with --device (what
Windows mixed for that endpoint, that is what FxSound rendered to it).

Each capture stream gets a wall-clock timebase: every callback logs time.perf_counter() and the
frame count, and a linear fit maps frames to QPC time. That also gives each device's clock rate
against the system clock (ppm). For every marker the tool then computes:
  latency  = time it reached the output's endpoint mix - time it reached the virtual device's mix
  relative = latency - latency of the carrier (or of the first --device)
and reports mean, spread, trend (ms per hour: a non-zero trend means the latency creeps), the largest
jump between consecutive markers, and missing markers. Between markers it looks for clicks (spikes of
the second difference far above the tone's own) and dropouts (the tone's level falling by > 20 dB).

  python drift_probe.py list
  python drift_probe.py run --seconds 600 --device "JBL GO Essential" --device "JBL GO 2" --out DIR
  python drift_probe.py run --seconds 180 --device ... --at "60:--output-delay=\"Alto-falantes (JBL GO 2)\":169" --out DIR
  python drift_probe.py analyze DIR

--at "<seconds>:<raw FxSound.exe arguments>" runs FxSound.exe with those arguments (passed verbatim)
at that time, so steps (delay change, output toggles, power off/on) show up in the latency series.

Needs: pip install pyaudiowpatch numpy. Plays sound: take the audio-lab lock first (docs/DEBUG_LAB.md).
Captures are written as .npy files in --out (about 0.2 MB per device per second).
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

DEFAULT_FXSOUND = os.environ.get("FXSOUND_EXE", r"C:\Program Files\BetterFxSound\FxSound.exe")
VIRTUAL_NAME = "FxSound Speakers"
TONE_HZ = 1000.0
TONE_LEVEL = 0.05
CHIRP_F0, CHIRP_F1, CHIRP_S = 2000.0, 8000.0, 0.02
CHIRP_LEVEL = 0.3
MASK_BEFORE_S, MASK_AFTER_S = 0.05, 0.45   # around a marker, for click/dropout detection


def chirp(rate):
    n = int(round(CHIRP_S * rate))
    t = np.arange(n) / rate
    k = (CHIRP_F1 - CHIRP_F0) / CHIRP_S
    x = np.sin(2 * np.pi * (CHIRP_F0 * t + 0.5 * k * t * t)) * np.hanning(n)
    return x.astype(np.float64)


# ----------------------------------------------------------------------------------------- capture

class Capture:
    def __init__(self, p, info, seconds, out_dir, label):
        import pyaudiowpatch as pa
        self.label = label
        self.name = info["name"]
        self.rate = int(info["defaultSampleRate"])
        self.ch = int(info["maxInputChannels"])
        self.path = os.path.join(out_dir, f"{label}.npy")
        self.buf = np.lib.format.open_memmap(self.path, mode="w+", dtype=np.float32, shape=(int(self.rate * (seconds + 15)),))
        self.n = 0
        self.log = []          # (perf_counter, frames so far, status flags)
        self.stream = p.open(format=pa.paFloat32, channels=self.ch, rate=self.rate, input=True,
                             input_device_index=info["index"], frames_per_buffer=pa.paFramesPerBufferUnspecified,
                             stream_callback=self.callback, start=False)

    def callback(self, in_data, frame_count, time_info, status):
        import pyaudiowpatch as pa
        t = time.perf_counter()
        x = np.frombuffer(in_data, dtype=np.float32).reshape(-1, self.ch)
        m = x[:, :2].mean(axis=1) if self.ch >= 2 else x[:, 0]
        k = min(len(m), len(self.buf) - self.n)
        if k > 0:
            self.buf[self.n:self.n + k] = m[:k]
            self.n += k
        self.log.append((t, self.n, int(status)))
        return (None, pa.paContinue)


class Source:
    """Tone + periodic chirp markers into the default output device."""

    def __init__(self, p, info, period):
        import pyaudiowpatch as pa
        self.rate = int(info["defaultSampleRate"])
        self.ch = max(2, min(int(info["maxOutputChannels"]), 2))
        self.period = int(round(period * self.rate))
        self.marker = (chirp(self.rate) * CHIRP_LEVEL).astype(np.float32)
        self.first = self.rate  # first marker after one second of tone
        self.n = 0
        self.log = []
        self.stream = p.open(format=pa.paFloat32, channels=self.ch, rate=self.rate, output=True,
                             output_device_index=info["index"], frames_per_buffer=pa.paFramesPerBufferUnspecified,
                             stream_callback=self.callback, start=False)

    def callback(self, in_data, frame_count, time_info, status):
        import pyaudiowpatch as pa
        idx = np.arange(self.n, self.n + frame_count)
        y = (TONE_LEVEL * np.sin(2 * np.pi * TONE_HZ * idx / self.rate)).astype(np.float32)
        rel = idx - self.first
        pos = np.mod(rel, self.period)
        sel = (rel >= 0) & (pos < len(self.marker))
        y[sel] += self.marker[pos[sel]]
        self.log.append((time.perf_counter(), self.n + frame_count, int(status)))
        self.n += frame_count
        return (np.repeat(y[:, None], self.ch, axis=1).tobytes(), pa.paContinue)


def loopbacks(p):
    return list(p.get_loopback_device_info_generator())


def cmd_list(args):
    import pyaudiowpatch as pa
    p = pa.PyAudio()
    try:
        out = p.get_device_info_by_index(p.get_host_api_info_by_type(pa.paWASAPI)["defaultOutputDevice"])
        if out:
            print("default output:", out["name"], int(out["defaultSampleRate"]), "Hz")
        for d in loopbacks(p):
            print(f"  [{d['index']}] {d['name']}  {int(d['defaultSampleRate'])} Hz  {d['maxInputChannels']} ch")
    finally:
        p.terminate()


def run_fxsound(exe, raw):
    cmdline = f'"{exe}" {raw}'
    return subprocess.Popen(cmdline, cwd=os.path.dirname(exe))


def cmd_run(args):
    import pyaudiowpatch as pa
    os.makedirs(args.out, exist_ok=True)
    p = pa.PyAudio()
    caps = []
    try:
        lbs = loopbacks(p)

        def pick(sub):
            hits = [d for d in lbs if sub.lower() in d["name"].lower()]
            if len(hits) != 1:
                raise SystemExit(f"'{sub}' matches {len(hits)} loopback devices: {[d['name'] for d in hits]}")
            return hits[0]

        wasapi = p.get_host_api_info_by_type(pa.paWASAPI)
        out_info = p.get_device_info_by_index(wasapi["defaultOutputDevice"])
        print("source ->", out_info["name"])
        src = Source(p, out_info, args.period)
        caps.append(Capture(p, pick(VIRTUAL_NAME), args.seconds, args.out, "ref"))
        for i, name in enumerate(args.device):
            caps.append(Capture(p, pick(name), args.seconds, args.out, f"dev{i}"))

        triggers = []
        for spec in args.at:
            t, raw = spec.split(":", 1)
            triggers.append([float(t), raw, None])

        for c in caps:
            c.stream.start_stream()
        time.sleep(0.3)
        t_start = time.perf_counter()
        src.stream.start_stream()
        print(f"running {args.seconds} s ...", flush=True)
        last_print = 0
        while True:
            now = time.perf_counter() - t_start
            if now >= args.seconds:
                break
            for tr in triggers:
                if tr[2] is None and now >= tr[0]:
                    tr[2] = time.perf_counter()
                    run_fxsound(args.exe, tr[1])
                    print(f"  {now:7.2f}s  FxSound.exe {tr[1]}", flush=True)
            if now - last_print >= 30:
                last_print = now
                print(f"  {now:5.0f}s  " + "  ".join(f"{c.label}:{c.n / c.rate:6.1f}s" for c in caps), flush=True)
            time.sleep(0.01)
        src.stream.stop_stream()
        time.sleep(1.0)
        for c in caps:
            c.stream.stop_stream()
            c.stream.close()
        src.stream.close()

        meta = {
            "seconds": args.seconds, "period": args.period, "t_start": t_start,
            "source": {"name": out_info["name"], "rate": src.rate, "first": src.first, "period_frames": src.period, "log": src.log},
            "captures": [{"label": c.label, "name": c.name, "rate": c.rate, "frames": c.n, "log": c.log} for c in caps],
            "triggers": [{"t": tr[0], "args": tr[1], "perf": tr[2]} for tr in triggers],
            "devices": args.device,
        }
        with open(os.path.join(args.out, "meta.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f)
    finally:
        p.terminate()
    analyze(args.out)


# ---------------------------------------------------------------------------------------- analysis

def timebase(log, rate):
    """Least squares t = a + b * frames on the callbacks, refit on the earliest half (callbacks run late,
    never early). Returns (a, b, ppm vs nominal, residual ms p99)."""
    arr = np.array([(t, n) for t, n, _ in log if n > 0], dtype=np.float64)
    arr = arr[arr[:, 1] > rate * 2.0]           # skip the first 2 s (stream start-up)
    t, n = arr[:, 0], arr[:, 1]
    b, a = np.polyfit(n, t, 1)
    r = t - (a + b * n)
    keep = r <= np.median(r)
    b, a = np.polyfit(n[keep], t[keep], 1)
    r = t - (a + b * n)
    ppm = (1.0 / b / rate - 1.0) * 1e6
    return a, b, ppm, float(np.percentile(np.abs(r - np.median(r)), 99) * 1000)


def correlate_envelope(x, tmpl, chunk=1 << 20):
    """Envelope of the cross-correlation of x with tmpl (analytic signal), overlap-save."""
    m = len(tmpl)
    nfft = chunk
    step = nfft - m
    T = np.conj(np.fft.fft(tmpl, nfft))
    # analytic: zero negative frequencies, double positive ones
    h = np.zeros(nfft)
    h[0] = 1
    h[1:nfft // 2] = 2
    h[nfft // 2] = 1
    out = np.zeros(len(x), dtype=np.float32)
    pos = 0
    while pos < len(x):
        seg = x[pos:pos + nfft].astype(np.float64)
        if len(seg) < m:
            break
        c = np.fft.ifft(np.fft.fft(seg, nfft) * T * h)
        valid = min(step, len(x) - pos)
        out[pos:pos + valid] = np.abs(c[:valid])
        pos += step
    return out


def find_markers(env, rate, period_s, first_guess=None):
    """Peak per period: returns list of (sample position with sub-sample refinement, height)."""
    per = int(round(period_s * rate))
    n = len(env)
    # first marker: strongest peak in the first 3 periods
    lim = min(n, 3 * per)
    if lim <= 0:
        return []
    top = float(np.max(env[:lim]))
    thr = 0.35 * top
    first = int(np.argmax(env[:lim] > thr))
    first = first + int(np.argmax(env[first:first + int(0.05 * rate)]))
    peaks = []
    pos = first
    while pos < n - 2:
        lo = max(0, pos - int(0.3 * per))
        hi = min(n, pos + int(0.3 * per))
        seg = env[lo:hi]
        if len(seg) < 3:
            break
        i = int(np.argmax(seg)) + lo
        h = float(env[i])
        if h < 0.25 * top or i <= 0 or i >= n - 1:
            peaks.append((np.nan, h))
        else:
            y0, y1, y2 = float(env[i - 1]), float(env[i]), float(env[i + 1])
            d = (y0 - y2) / (2 * (y0 - 2 * y1 + y2)) if (y0 - 2 * y1 + y2) != 0 else 0.0
            peaks.append((i + d, h))
            pos = i
        pos += per
    return peaks


def loud_windows(x, rate, markers, start, end):
    """10 ms windows more than 10 dB above the median level: unprocessed audio reaching the output at full
    scale (for example while the real device is briefly the Windows default during a carrier switch)."""
    win = int(0.01 * rate)
    n = (end - start) // win
    if n <= 0:
        return []
    r = np.sqrt(np.mean(np.asarray(x[start:start + n * win], dtype=np.float64).reshape(n, win) ** 2, axis=1))
    free = np.ones(n, dtype=bool)          # windows clear of the markers (they are 15 dB over the tone)
    for m, _ in markers:
        if np.isnan(m):
            continue
        a = max(0, int((m - MASK_BEFORE_S * rate - start) // win))
        b = min(n, int((m + MASK_AFTER_S * rate - start) // win) + 1)
        if b > 0:
            free[a:b] = False
    if not free.any():
        return []
    med = float(np.median(r[free])) + 1e-12
    hits = np.where(free & (r > med * 3.16))[0]
    out = []
    last = -10
    for h in hits:
        if h - last > 5:
            out.append(((start + int(h) * win) / rate, float(20 * np.log10(r[h] / med))))
        last = h
    return out


def clicks_and_dropouts(x, rate, markers, start, end):
    """Second-difference spikes and level drops outside the marker regions."""
    mask = np.zeros(len(x), dtype=bool)
    mask[:start] = True
    mask[end:] = True
    for m, _ in markers:
        if np.isnan(m):
            continue
        a = max(0, int(m - MASK_BEFORE_S * rate))
        b = min(len(x), int(m + MASK_AFTER_S * rate))
        mask[a:b] = True
    d2 = np.zeros(len(x), dtype=np.float32)
    d2[2:] = x[2:] - 2 * x[1:-1] + x[:-2]
    blk = int(rate)
    clicks = []
    drops = []
    win = int(0.005 * rate)
    for s in range(start, end - blk, blk):
        seg_mask = mask[s:s + blk]
        if seg_mask.all():
            continue
        seg = x[s:s + blk]
        sd = np.abs(d2[s:s + blk])
        free = ~seg_mask
        scale = float(np.median(sd[free])) + 1e-12
        tone_rms = float(np.sqrt(np.median(seg[free] ** 2) * 2)) + 1e-12
        # clicks: 2nd difference far above the tone's own (a 1 kHz tone has |d2| <= ~0.02 * amplitude)
        hit = np.where(free & (sd > max(12 * scale, 0.25 * tone_rms)))[0]
        last = -10 ** 9
        for h in hit:
            if h - last > win:
                clicks.append((s + int(h)) / rate)
            last = h
        # dropouts: 5 ms windows far below the tone level
        nw = blk // win
        r = np.sqrt(np.mean(seg[:nw * win].reshape(nw, win) ** 2, axis=1))
        fm = free[:nw * win].reshape(nw, win).all(axis=1)
        low = np.where(fm & (r < 0.1 * tone_rms / np.sqrt(2)))[0]
        last = -10
        for w in low:
            if w - last > 1:
                drops.append((s + int(w) * win) / rate)
            last = w
    return clicks, drops


def stats(series):
    v = np.array(series, dtype=np.float64)
    ok = ~np.isnan(v)
    if ok.sum() < 3:
        return None
    k = np.arange(len(v))
    slope, icpt = np.polyfit(k[ok], v[ok], 1)
    dv = np.abs(np.diff(v[ok]))
    return {"mean": float(np.mean(v[ok])), "std": float(np.std(v[ok])), "min": float(np.min(v[ok])),
            "max": float(np.max(v[ok])), "trend_per_marker": float(slope), "max_jump": float(np.max(dv)) if len(dv) else 0.0,
            "missing": int((~ok).sum()), "count": int(len(v))}


def analyze(out_dir):
    with open(os.path.join(out_dir, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    period = meta["period"]
    t_start = meta["t_start"]
    result = {"devices": {}, "triggers": meta["triggers"]}
    series = {}
    walls = {}
    for c in meta["captures"]:
        x = np.load(os.path.join(out_dir, f"{c['label']}.npy"), mmap_mode="r")[:c["frames"]]
        rate = c["rate"]
        a, b, ppm, jit = timebase(c["log"], rate)
        env = correlate_envelope(np.asarray(x), chirp(rate))
        peaks = find_markers(env, rate, period)
        wall = [(a + b * m - t_start) if not np.isnan(m) else np.nan for m, _ in peaks]
        walls[c["label"]] = np.array(wall)
        start = int(2.0 * rate)
        end = len(x) - int(1.0 * rate)
        clicks, drops = clicks_and_dropouts(np.asarray(x), rate, peaks, start, end)
        loud = loud_windows(x, rate, peaks, start, end)
        to_wall = lambda s: a + b * (s * rate) - t_start
        result["devices"][c["label"]] = {
            "name": c["name"], "rate": rate, "clock_ppm_vs_qpc": round(ppm, 2), "timebase_jitter_ms_p99": round(jit, 3),
            "markers": len(peaks), "clicks": len(clicks), "click_times_s": [round(to_wall(s), 3) for s in clicks[:50]],
            "dropouts": len(drops), "dropout_times_s": [round(to_wall(s), 3) for s in drops[:50]],
            "overflow_flags": int(sum(1 for _, _, s in c["log"] if s)),
            "loud": len(loud), "loud_times_s_db": [(round(to_wall(t), 3), round(db_, 1)) for t, db_ in loud[:50]],
        }
    ref = walls.get("ref")
    labels = [c["label"] for c in meta["captures"] if c["label"] != "ref"]
    if ref is not None:
        for lab in labels:
            # Each output marker belongs to the last reference marker before it (latency < period).
            lat = np.full(len(ref), np.nan)
            ref_ok = np.where(~np.isnan(ref))[0]
            for w in walls[lab]:
                if np.isnan(w) or len(ref_ok) == 0:
                    continue
                j = np.searchsorted(ref[ref_ok], w, side="right") - 1
                if j < 0:
                    continue
                k = ref_ok[j]
                d = w - ref[k]
                if 0 <= d < period:
                    lat[k] = d * 1000.0
            series[lab] = lat
            s = stats(lat)
            if s:
                s["trend_ms_per_hour"] = s.pop("trend_per_marker") * 3600.0 / period
            result["devices"][lab]["latency_ms"] = s
        if labels:
            base = labels[0]
            for lab in labels[1:]:
                k = min(len(series[lab]), len(series[base]))
                rel = series[lab][:k] - series[base][:k]
                s = stats(rel)
                if s:
                    s["trend_ms_per_hour"] = s.pop("trend_per_marker") * 3600.0 / period
                result["devices"][lab]["relative_to_" + base + "_ms"] = s
    np.savez(os.path.join(out_dir, "series.npz"), **{k: v for k, v in series.items()}, ref=ref if ref is not None else np.array([]))
    with open(os.path.join(out_dir, "result.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print_result(result, series, meta)
    return result


def print_result(result, series, meta):
    for lab, d in result["devices"].items():
        print(f"{lab}: {d['name']}  {d['rate']} Hz  clock {d['clock_ppm_vs_qpc']:+.1f} ppm vs QPC  "
              f"markers {d['markers']}  clicks {d['clicks']}  dropouts {d['dropouts']}  flags {d['overflow_flags']}")
        for key in [k for k in d if k.startswith("latency") or k.startswith("relative")]:
            s = d[key]
            if s:
                print(f"    {key}: mean {s['mean']:.2f}  std {s['std']:.2f}  min {s['min']:.2f}  max {s['max']:.2f}  "
                      f"trend {s['trend_ms_per_hour']:+.2f} ms/h  max jump {s['max_jump']:.2f}  missing {s['missing']}/{s['count']}")
        if d["clicks"]:
            print(f"    click times: {d['click_times_s'][:12]}")
        if d["dropouts"]:
            print(f"    dropout times: {d['dropout_times_s'][:12]}")
        if d.get("loud"):
            print(f"    LOUD (>10 dB over median, 10 ms windows): {d['loud_times_s_db'][:12]}")
    for tr in meta.get("triggers", []):
        if tr.get("perf") is None:
            continue
        t = tr["perf"] - meta["t_start"]
        k0 = int((t - 1.0) / meta["period"])
        print(f"trigger at {t:.2f}s: {tr['args']}")
        for lab, s in series.items():
            seg = s[max(0, k0 - 2):k0 + 25]
            print(f"   {lab}: " + " ".join("nan" if np.isnan(v) else f"{v:.0f}" for v in seg))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("list")
    r = sub.add_parser("run")
    r.add_argument("--seconds", type=float, default=120)
    r.add_argument("--period", type=float, default=2.0)
    r.add_argument("--device", action="append", default=[], help="loopback device name substring (first = carrier, for relative timing)")
    r.add_argument("--at", action="append", default=[])
    r.add_argument("--exe", default=DEFAULT_FXSOUND)
    r.add_argument("--out", required=True)
    a = sub.add_parser("analyze")
    a.add_argument("dir")
    args = ap.parse_args()
    if args.cmd == "list":
        cmd_list(args)
    elif args.cmd == "run":
        cmd_run(args)
    elif args.cmd == "analyze":
        analyze(args.dir)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
