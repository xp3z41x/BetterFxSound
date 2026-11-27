"""
volecho.py - which endpoint volume notifications does a level change produce, and with which event context?

The engine tags its own volume changes with its GUID and the carrier's volume callback ignores those, treating
every other notification as a change the user made. Some drivers (Bluetooth with absolute volume) report a level
change back a second time with another event context. This registers an IAudioEndpointVolumeCallback on the
device, sets its level with a tagged context, and prints every notification.

  python volecho.py "JBL GO 2" 43 100      # set 43%, then 100%, each tagged; restores nothing else

Needs: pip install pycaw comtypes. Changes the device's level: take the audio-lab lock first (docs/DEBUG_LAB.md).
"""

import sys
import time
import uuid

import comtypes
from comtypes import GUID
from pycaw.callbacks import AudioEndpointVolumeCallback

import devvol

TAG = GUID("{" + str(uuid.UUID("6b7f1f2e-0000-4000-8000-00000000beef")) + "}")


class Cb(AudioEndpointVolumeCallback):
    def __init__(self, t0):
        super().__init__()
        self.t0 = t0

    def on_notify(self, new_volume, new_volume_db, new_mute, event_context, channels, channel_volumes):
        ctx = str(event_context)
        who = "TAGGED (ours)" if ctx.lower() == str(TAG).lower() else ("NULL" if ctx == "{00000000-0000-0000-0000-000000000000}" else ctx)
        print(f"  {time.time() - self.t0:7.3f}s  notify level={new_volume * 100:6.2f}% mute={new_mute}  context={who}", flush=True)


def main(name, levels):
    d, ev = devvol.find(name)
    t0 = time.time()
    cb = Cb(t0)
    ev.RegisterControlChangeNotify(cb)
    try:
        for lv in levels:
            print(f"{time.time() - t0:7.3f}s  set {lv}% tagged", flush=True)
            ev.SetMasterVolumeLevelScalar(lv / 100.0, comtypes.pointer(TAG))
            time.sleep(2.5)
    finally:
        ev.UnregisterControlChangeNotify(cb)
    print(f"final: {ev.GetMasterVolumeLevelScalar() * 100:.2f}%")


if __name__ == "__main__":
    main(sys.argv[1], [float(x) for x in sys.argv[2:]])
