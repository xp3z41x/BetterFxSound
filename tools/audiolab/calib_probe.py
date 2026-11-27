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
    args = ap.parse_args()
    if args.cmd == "list":
        list_devices()
    elif args.cmd == "record":
        record(args.seconds, args.out, args.mic)
    else:
        analyze(args.dir, args.targets)


if __name__ == "__main__":
    sys.exit(main())
