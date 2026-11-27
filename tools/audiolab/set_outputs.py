"""
set_outputs.py - edit the per-output settings in FxSound.settings while FxSound is closed.

There is no command-line option for an output's channel mode, so live tests set it here:
close FxSound, run this, start FxSound again (it reads the `outputs` key at startup).

  python set_outputs.py show
  python set_outputs.py set "JBL GO Essential=left" "JBL Clip 4=right,on" "GT808=both,off,delay:0"

Each spec is "<part of the device name>=<items>", items comma separated:
  both | left | right | mono      channel mode
  on | off                         enabled
  delay:<ms>  volume:<0-100>       delay and trim
A device that has no entry yet gets one (its id is looked up in status.json's outputs).

The file is %APPDATA%\\FxSound\\FxSound.settings unless --file is given. Back it up first
(docs/DEBUG_LAB.md): this tool does not.
"""

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET

MODES = {"both": 0, "left": 1, "right": 2, "mono": 3}
NAMES = {v: k for k, v in MODES.items()}


def settings_path(args):
    return args.file or os.path.expandvars(r"%APPDATA%\FxSound\FxSound.settings")


def load(path):
    tree = ET.parse(path)
    for v in tree.getroot():
        if v.get("name") == "outputs":
            return tree, v, json.loads(v.get("val"))
    return tree, None, []


def show(entries):
    for e in entries:
        print(f"  {e.get('name') or e['id']}: {'on ' if e.get('enabled') else 'off'} "
              f"{NAMES.get(e.get('channel_mode', 0), '?'):5s} delay {e.get('delay_ms', 0)} ms, volume {e.get('volume', 100)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("show")
    s = sub.add_parser("set")
    s.add_argument("specs", nargs="+")
    args = ap.parse_args()

    path = settings_path(args)
    tree, node, entries = load(path)
    if args.cmd == "show":
        show(entries)
        return 0

    known = {}
    try:
        with open(os.path.expandvars(r"%APPDATA%\FxSound\status.json"), encoding="utf-8") as f:
            for o in json.load(f).get("outputs", []):
                known[o["name"]] = o["id"]
    except Exception:  # noqa: BLE001
        pass

    for spec in args.specs:
        key, _, items = spec.partition("=")
        matches = [e for e in entries if key.lower() in (e.get("name") or "").lower()]
        if not matches:
            ids = [(n, i) for n, i in known.items() if key.lower() in n.lower()]
            if len(ids) != 1:
                print(f"no single device matches '{key}'")
                return 1
            entry = {"id": ids[0][1], "name": ids[0][0], "enabled": False, "delay_ms": 0, "volume": 100, "channel_mode": 0}
            entries.append(entry)
            matches = [entry]
        if len(matches) > 1:
            print(f"'{key}' matches several devices")
            return 1
        e = matches[0]
        for item in filter(None, items.split(",")):
            item = item.strip().lower()
            if item in MODES:
                e["channel_mode"] = MODES[item]
            elif item in ("on", "off"):
                e["enabled"] = item == "on"
            elif item.startswith("delay:"):
                e["delay_ms"] = int(item[6:])
            elif item.startswith("volume:"):
                e["volume"] = int(item[7:])
            else:
                print(f"unknown item '{item}'")
                return 1

    if node is None:
        node = ET.SubElement(tree.getroot(), "VALUE", {"name": "outputs"})
    node.set("val", json.dumps(entries, indent=2).replace("\n", "\r\n"))
    tree.write(path, encoding="UTF-8", xml_declaration=True)
    show(entries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
