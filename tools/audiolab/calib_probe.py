"""
calib_probe.py - watch a "Sync delays with microphone" run from the outside.

Records, at the same time and on one clock (time.perf_counter):
  - the WASAPI loopback of every output device (what Windows mixed for that endpoint,
    that is what FxSound rendered to it, before the device and the air);
  - the loopback of the FxSound virtual device (where DelayCalibration plays its sweeps);
  - the default microphone (what was heard in the room).

Then finds the calibration sweep (500 Hz -> 6 kHz, 100 ms) in every stream and prints,
for each sweep played into the virtual device, which output carried it, how loud, and
whether the microphone heard it. Loopback streams only deliver audio while something
renders to the endpoint, so gaps in a loopback stream show when an output's stream was
stopped (idle) or not yet running.

Needs: pip install pyaudiowpatch numpy

  python calib_probe.py list
  python calib_probe.py record --seconds 25 --out run1          # start this, then press Sync
  python calib_probe.py analyze run1 [--targets "JBL GO Essential,JBL GO 2,JBL Clip 4"]

Nothing is played and no setting is changed by this tool.
"""

import argparse
import json
import os
import sys
import threading
import time
import wave

import numpy as np

CHIRP_SECONDS = 0.1
CHIRP_F0 = 500.0
CHIRP_F1 = 6000.0
CHIRP_FADE = 0.005


def make_chirp(rate):
    n = int(CHIRP_SECONDS * rate)
    fade = int(CHIRP_FADE * rate)
    k = np.log(CHIRP_F1 / CHIRP_F0)
    t = np.arange(n) / rate
    phase = 2 * np.pi * CHIRP_F0 * CHIRP_SECONDS / k * (np.exp(t * k / CHIRP_SECONDS) - 1.0)
    g = np.ones(n)
    i = np.arange(fade)
    g[:fade] = 0.5 - 0.5 * np.cos(np.pi * i / fade)
    g[n - fade:] = (0.5 - 0.5 * np.cos(np.pi * i / fade))[::-1]
    return (np.sin(phase) * g).astype(np.float64)


def wasapi(p):
    import pyaudiowpatch as pa
    return p.get_host_api_info_by_type(pa.paWASAPI)


def list_devices():
    import pyaudiowpatch as pa
    p = pa.PyAudio()
    w = wasapi(p)
    print("default output:", p.get_device_info_by_index(w["defaultOutputDevice"])["name"])
    print("default input: ", p.get_device_info_by_index(w["defaultInputDevice"])["name"])
    for i in range(p.get_device_count()):
        d = p.get_device_info_by_index(i)
        if d["hostApi"] != w["index"]:
            continue
        kind = "loopback" if d.get("isLoopbackDevice") else ("in" if d["maxInputChannels"] else "out")
        print(f"{i:3d} {kind:8s} {int(d['defaultSampleRate']):6d} Hz  {d['name']}")
    p.terminate()


def record(seconds, out_dir, mic_name=None):
    import pyaudiowpatch as pa
    os.makedirs(out_dir, exist_ok=True)
    p = pa.PyAudio()
    w = wasapi(p)

    sources = []
    if mic_name:
        mic = next(p.get_device_info_by_index(i) for i in range(p.get_device_count())
                   if p.get_device_info_by_index(i)["hostApi"] == w["index"]
                   and p.get_device_info_by_index(i)["maxInputChannels"] > 0
                   and not p.get_device_info_by_index(i).get("isLoopbackDevice")
                   and mic_name in p.get_device_info_by_index(i)["name"])
    else:
        mic = p.get_device_info_by_index(w["defaultInputDevice"])
    sources.append(("mic", mic))
    for d in p.get_loopback_device_info_generator():
        sources.append(("lb", d))

    streams = []
    for kind, d in sources:
        rate = int(d["defaultSampleRate"])
        ch = int(d["maxInputChannels"])
        rec = {"name": d["name"], "kind": kind, "rate": rate, "channels": ch, "chunks": [], "times": []}

        def cb(in_data, frame_count, time_info, status, rec=rec):
            rec["times"].append((time.perf_counter(), frame_count))
            rec["chunks"].append(in_data)
            return (None, pa.paContinue)

        try:
            s = p.open(format=pa.paFloat32, channels=ch, rate=rate, input=True,
                       input_device_index=d["index"], frames_per_buffer=int(rate / 100),
                       stream_callback=cb)
        except Exception as e:  # noqa: BLE001
            print("cannot open", d["name"], e)
            continue
        streams.append((s, rec))

    t0 = time.perf_counter()
    for s, _ in streams:
        s.start_stream()
    print(f"recording {len(streams)} streams for {seconds} s ...", flush=True)
    time.sleep(seconds)
    for s, _ in streams:
        s.stop_stream()
        s.close()
    p.terminate()

    meta = {"t0": t0, "streams": []}
    for idx, (_, rec) in enumerate(streams):
        data = np.frombuffer(b"".join(rec["chunks"]), dtype=np.float32)
        fname = f"s{idx:02d}.npy"
        np.save(os.path.join(out_dir, fname), data)
        meta["streams"].append({"name": rec["name"], "kind": rec["kind"], "rate": rec["rate"],
                                "channels": rec["channels"], "file": fname,
                                "times": [[t - t0, n] for t, n in rec["times"]]})
    with open(os.path.join(out_dir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    print("saved to", out_dir)


def sample_times(stream):
    """Time (s, since t0) of every frame, from the arrival time of each callback."""
    times = np.empty(sum(n for _, n in stream["times"]))
    pos = 0
    rate = stream["rate"]
    for arrival, n in stream["times"]:
        times[pos:pos + n] = arrival - (n - np.arange(n)) / rate
        pos += n
    return times


def gaps(stream, min_gap=0.15):
    out = []
    prev = None
    for arrival, n in stream["times"]:
        if prev is not None and arrival - prev > min_gap:
            out.append((prev, arrival))
        prev = arrival
    return out


def detect(signal, rate, min_ncc=0.3, min_sep=0.5):
    """Matched filter for the sweep, normalized (correlation coefficient, 0..1, so room noise and
    music score low and an undistorted sweep scores near 1). Returns [(frame index, ncc, peak amplitude)]."""
    if len(signal) < rate:
        return []
    chirp = make_chirp(rate)
    m = len(chirp)
    n = len(signal) + m
    nfft = 1 << (n - 1).bit_length()
    corr = np.fft.irfft(np.fft.rfft(signal, nfft) * np.conj(np.fft.rfft(chirp, nfft)), nfft)[:len(signal)]
    c2 = np.concatenate(([0.0], np.cumsum(signal * signal)))
    idx = np.arange(len(signal))
    energy = c2[np.minimum(idx + m, len(signal))] - c2[idx]
    ncc = np.abs(corr) / (np.sqrt(np.maximum(energy, 1e-20)) * np.linalg.norm(chirp))
    ncc[energy < 1e-12 * m] = 0.0
    hits = []
    order = np.argsort(ncc)[::-1]
    taken = np.zeros(len(ncc), dtype=bool)
    sep = int(min_sep * rate)
    for i in order[:20000]:
        if ncc[i] < min_ncc:
            break
        if taken[max(0, i - sep):i + sep].any():
            continue
        taken[i] = True
        seg = signal[i:i + m]
        hits.append((int(i), float(ncc[i]), float(np.max(np.abs(seg))) if len(seg) else 0.0))
    return sorted(hits)


def analyze(run_dir, targets):
    with open(os.path.join(run_dir, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)

    results = {}
    for s in meta["streams"]:
        data = np.load(os.path.join(run_dir, s["file"])).astype(np.float64)
        ch = s["channels"]
        data = data[: len(data) // ch * ch].reshape(-1, ch)
        mono = data.mean(axis=1)
        t = sample_times(s)[: len(mono)]
        hits = detect(mono, s["rate"])
        results[s["name"]] = {"kind": s["kind"], "hits": [(t[i], c, a) for i, c, a in hits],
                              "gaps": gaps(s), "span": (t[0], t[-1]) if len(t) else None,
                              "peak": float(np.max(np.abs(mono))) if len(mono) else 0.0,
                              "lr": [float(np.sqrt(np.mean(data[:, c] ** 2))) for c in range(ch)]}

    virtual = next((n for n in results if "FxSound" in n), None)
    if virtual is None or not results[virtual]["hits"]:
        print("no sweep found in the FxSound virtual device loopback")
    emissions = [h[0] for h in results[virtual]["hits"]] if virtual else []

    print(f"\nsweeps played into the virtual device: {len(emissions)}")
    for name, r in results.items():
        span = r["span"]
        print(f"\n== {name} [{r['kind']}]  peak {r['peak']:.4f}  span {span[0]:.2f}-{span[1]:.2f} s" if span else f"\n== {name}: no data")
        for a, b in r["gaps"]:
            print(f"   gap {a:7.2f} -> {b:7.2f} s ({b - a:.2f} s without audio)")
        for (tm, c, a) in r["hits"]:
            print(f"   sweep at {tm:7.3f} s  ncc {c:5.2f}  amp {a:.4f}")

    if not emissions:
        return
    names = [n for n in results if results[n]["kind"] == "lb" and n != virtual]
    tlist = [x.strip() for x in targets.split(",")] if targets else []
    print("\nper sweep: delay from emission to each output's loopback / mic (ms), amp")
    hdr = "  #  target            " + "".join(f"{n[:22]:>24s}" for n in names) + f"{'mic':>16s}"
    print(hdr)
    mic = next((n for n in results if results[n]["kind"] == "mic"), None)
    for k, e in enumerate(emissions):
        target = tlist[k % len(tlist)] if tlist else "?"
        line = f"{k:3d}  {target[:16]:16s}"
        for n in names:
            m = [h for h in results[n]["hits"] if 0 <= h[0] - e < 1.2]
            line += f"{'-':>24s}" if not m else f"{(m[0][0] - e) * 1000:12.0f} ms {m[0][2]:.4f}".rjust(24)
        if mic:
            m = [h for h in results[mic]["hits"] if 0 <= h[0] - e < 1.5]
            line += f"{'-':>16s}" if not m else f"{(m[0][0] - e) * 1000:8.0f} n{m[0][1]:.2f}".rjust(16)
        print(line)


def _load_mono(run_dir, stream):
    data = np.load(os.path.join(run_dir, stream["file"])).astype(np.float64)
    ch = stream["channels"]
    data = data[: len(data) // ch * ch].reshape(-1, ch).mean(axis=1)
    return data, sample_times(stream)[: len(data)]


def _find_arrival(rec, chirp, start, max_lag, rate, look_back_ms):
    """DelayCalibration's findArrival. look_back_ms=None is the 1.0.0 rule (earliest peak >= 50%
    anywhere before the strongest one). Returns (lag ms, confidence of the strongest peak,
    confidence of the chosen peak) or None."""
    seg = rec[start:start + max_lag + len(chirp)]
    if len(seg) < max_lag + len(chirp):
        return None
    nfft = 1 << (len(seg) + len(chirp)).bit_length()
    corr = np.abs(np.fft.irfft(np.fft.rfft(seg, nfft) * np.conj(np.fft.rfft(chirp, nfft)), nfft)[:max_lag])
    peak = corr.max()
    rms = np.sqrt(np.mean(corr ** 2))
    if peak <= 0 or rms <= 0:
        return None
    top = int(np.argmax(corr))
    lb = 0 if look_back_ms is None else max(0, top - int(look_back_ms * rate / 1000))
    lag = lb + int(np.argmax(corr[lb:top + 1] >= 0.5 * peak))
    end = min(max_lag, lag + rate // 500)
    best = lag + int(np.argmax(corr[lag:end]))
    return best * 1000.0 / rate, peak / rms, corr[best] / rms


def _combine_old(vals):
    ok = [v for v in vals if v and v[1] >= 6.0]
    return float(np.median([v[0] for v in ok])) if len(ok) >= 2 else None


def _combine_new(vals, min_conf=8.0, agree_ms=40.0):
    ok = [v for v in vals if v and v[2] >= min_conf]
    best = None
    for v in ok:
        group = [w for w in ok if abs(w[0] - v[0]) <= agree_ms]
        key = (len(group), sum(w[2] for w in group))
        if best is None or key > best[0]:
            best = (key, group)
    if not best or len(best[1]) < 2:
        return None
    return float(np.median([w[0] for w in best[1]]))


def replay(run_dir, targets, delays):
    """Runs DelayCalibration's analysis, as in 1.0.0 and as fixed, on a recording of a real sync.
    The sweeps are located in the FxSound virtual device's loopback (emission) and in the mic."""
    with open(os.path.join(run_dir, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    mic = next(s for s in meta["streams"] if s["kind"] == "mic")
    virt = next(s for s in meta["streams"] if "FxSound" in s["name"])
    vx, vt = _load_mono(run_dir, virt)
    emissions = [vt[i] for i, _, _ in detect(vx, virt["rate"])]
    mx, mt = _load_mono(run_dir, mic)
    rate = mic["rate"]
    chirp = make_chirp(rate)
    names = [x.strip() for x in targets.split(",")]
    dl = [int(x) for x in delays.split(",")] if delays else [0] * len(names)
    per = {n: ([], []) for n in names}
    for k, e in enumerate(emissions):
        t = k % len(names)
        i0 = int(np.searchsorted(mt, e))
        per[names[t]][0].append(_find_arrival(mx, chirp, i0, rate, rate, None))
        per[names[t]][1].append(_find_arrival(mx, chirp, i0, int(rate * (1.0 + min(max(dl[t], 0), 500) / 1000)), rate, 30))
    fmt = lambda vals: " ".join(f"{v[0]:5.0f}/{v[2]:4.1f}" if v else "    none " for v in vals)
    show = lambda x: "not measured" if x is None else f"{x:.0f} ms"
    print(f"{len(emissions)} sweeps, {len(names)} targets; each repetition is lag ms / confidence of the chosen peak")
    for n in names:
        old, new = per[n]
        print(f"  {n:24s} 1.0.0: {fmt(old)} -> {show(_combine_old(old)):12s}  fixed: {fmt(new)} -> {show(_combine_new(new))}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("record")
    r.add_argument("--seconds", type=float, default=25)
    r.add_argument("--out", required=True)
    r.add_argument("--mic", default=None, help="part of the microphone's name (default: Windows default)")
    a = sub.add_parser("analyze")
    a.add_argument("dir")
    a.add_argument("--targets", default="", help="comma separated, in measurement order")
    p = sub.add_parser("replay", help="DelayCalibration's analysis, 1.0.0 vs fixed, on a recording")
    p.add_argument("dir")
    p.add_argument("--targets", required=True, help="comma separated, in measurement order (carrier first)")
    p.add_argument("--delays", default="", help="each target's delay in ms when the sync ran, same order")
    args = ap.parse_args()
    if args.cmd == "list":
        list_devices()
    elif args.cmd == "record":
        record(args.seconds, args.out, args.mic)
    elif args.cmd == "replay":
        replay(args.dir, args.targets, args.delays)
    else:
        analyze(args.dir, args.targets)


if __name__ == "__main__":
    sys.exit(main())
