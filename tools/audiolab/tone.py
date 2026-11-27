"""
tone.py - play a quiet continuous tone into the FxSound virtual device for N seconds, reopening the stream
whenever it stops (power off/on and carrier switches change the Windows default device, which can end a
stream). Keeps audio flowing through FxSound during volume and toggle tests, so every enabled output opens
its stream and takes its device over.

  python tone.py 120 [level]       # level 0..1, default 0.03

Needs: pip install pyaudiowpatch numpy. Plays sound: take the audio-lab lock first (docs/DEBUG_LAB.md).
"""

import sys
import time

import numpy as np
import pyaudiowpatch as pa

VIRTUAL_NAME = "FxSound Speakers"


def main(seconds, level):
    end = time.time() + seconds
    p = pa.PyAudio()
    phase = 0
    reopen = 0
    try:
        while time.time() < end:
            info = next((p.get_device_info_by_index(i) for i in range(p.get_device_count())
                         if VIRTUAL_NAME in p.get_device_info_by_index(i)["name"]
                         and p.get_device_info_by_index(i)["maxOutputChannels"] > 0
                         and p.get_device_info_by_index(i)["hostApi"] == p.get_host_api_info_by_type(pa.paWASAPI)["index"]), None)
            if info is None:
                time.sleep(0.5)
                continue
            rate = int(info["defaultSampleRate"])
            try:
                s = p.open(format=pa.paFloat32, channels=2, rate=rate, output=True, output_device_index=info["index"])
                block = rate // 50
                while time.time() < end:
                    idx = np.arange(phase, phase + block)
                    y = (level * np.sin(2 * np.pi * 440.0 * idx / rate)).astype(np.float32)
                    phase += block
                    s.write(np.repeat(y[:, None], 2, axis=1).tobytes())
                s.close()
            except Exception as e:  # stream invalidated by a device change: open it again
                reopen += 1
                print(f"reopen after {type(e).__name__}: {e}", flush=True)
                # PortAudio caches the device list (format, sample rate): start it again to see the new format.
                p.terminate()
                time.sleep(0.3)
                p = pa.PyAudio()
    finally:
        p.terminate()
    print(f"done, {reopen} reopen(s)")


if __name__ == "__main__":
    main(float(sys.argv[1]), float(sys.argv[2]) if len(sys.argv) > 2 else 0.03)
