"""Drive FxSound's UI through UI Automation (JUCE exposes its components), without the mouse.

    python tools/audiolab/uia.py tree                      # names, roles and patterns of FxSound's UI
    python tools/audiolab/uia.py invoke "<name>" [index]   # Invoke (click) the index-th element with that name
    python tools/audiolab/uia.py toggle "<name>" [index]   # Toggle a checkbox
    python tools/audiolab/uia.py value "<name>" <number> [index]  # set a slider (RangeValue)

Needs comtypes. Take the audio lab lock first when it changes FxSound (docs/DEBUG_LAB.md).
"""
import subprocess
import sys

import comtypes
import comtypes.client

UIA = comtypes.client.GetModule("UIAutomationCore.dll")
uia = comtypes.client.CreateObject("{ff48dba4-60ef-4201-aa87-54103eef594e}", interface=UIA.IUIAutomation)


def fxsound_pids():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq FxSound.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return {int(l.split('","')[1]) for l in out.splitlines() if l.count('","') >= 2}


def top_windows():
    pids = fxsound_pids()
    root = uia.GetRootElement()
    kids = root.FindAll(UIA.TreeScope_Children, uia.CreateTrueCondition())
    return [kids.GetElement(i) for i in range(kids.Length) if kids.GetElement(i).CurrentProcessId in pids]


def walk(el, depth=0, out=None):
    out = [] if out is None else out
    out.append((depth, el))
    kids = el.FindAll(UIA.TreeScope_Children, uia.CreateTrueCondition())
    for i in range(kids.Length):
        walk(kids.GetElement(i), depth + 1, out)
    return out


PATTERNS = {"Invoke": UIA.UIA_InvokePatternId, "Toggle": UIA.UIA_TogglePatternId,
            "RangeValue": UIA.UIA_RangeValuePatternId, "Value": UIA.UIA_ValuePatternId,
            "ExpandCollapse": UIA.UIA_ExpandCollapsePatternId}


def patterns(el):
    return [n for n, pid in PATTERNS.items() if el.GetCurrentPattern(pid)]


def find(name, index=0):
    hits = [el for w in top_windows() for _, el in walk(w) if el.CurrentName == name]
    if len(hits) <= index:
        raise SystemExit(f"no element #{index} named {name!r} ({len(hits)} found)")
    return hits[index]


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "tree":
        for w in top_windows():
            for depth, el in walk(w):
                r = el.CurrentBoundingRectangle
                print(f"{'  ' * depth}{el.CurrentLocalizedControlType} {el.CurrentName!r} "
                      f"[{r.left},{r.top},{r.right},{r.bottom}] {' '.join(patterns(el))}")
    elif cmd == "invoke":
        el = find(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
        el.GetCurrentPattern(UIA.UIA_InvokePatternId).QueryInterface(UIA.IUIAutomationInvokePattern).Invoke()
        print("invoked", el.CurrentName)
    elif cmd == "toggle":
        el = find(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
        el.GetCurrentPattern(UIA.UIA_TogglePatternId).QueryInterface(UIA.IUIAutomationTogglePattern).Toggle()
        print("toggled", el.CurrentName)
    elif cmd == "value":
        el = find(sys.argv[2], int(sys.argv[4]) if len(sys.argv) > 4 else 0)
        el.GetCurrentPattern(UIA.UIA_RangeValuePatternId).QueryInterface(UIA.IUIAutomationRangeValuePattern).SetValue(float(sys.argv[3]))
        print("set", el.CurrentName, sys.argv[3])
