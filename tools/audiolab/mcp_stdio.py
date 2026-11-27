"""Minimal MCP stdio client for fxmcp.exe: calls its tools the way an MCP host would.

    python tools/audiolab/mcp_stdio.py <fxmcp.exe> list
    python tools/audiolab/mcp_stdio.py <fxmcp.exe> call <tool> '<json args>' [<tool> '<json args>' ...]

Prints each result (structured content or error text) and fxmcp's stderr log.
Take the audio lab lock first when a call changes FxSound (docs/DEBUG_LAB.md).
"""
import json
import subprocess
import sys
import threading
import time


class Client:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self.stderr = []
        threading.Thread(target=self._drain, daemon=True).start()
        self.next_id = 1
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "audiolab", "version": "0"}})
        self.notify("notifications/initialized", {})

    def _drain(self):
        for line in self.p.stderr:
            self.stderr.append(line.rstrip())

    def notify(self, method, params):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n")
        self.p.stdin.flush()

    def request(self, method, params, timeout=120):
        rid = self.next_id
        self.next_id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        end = time.time() + timeout
        while time.time() < end:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("fxmcp closed stdout")
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg
        raise TimeoutError(method)

    def call(self, name, args):
        t0 = time.time()
        msg = self.request("tools/call", {"name": name, "arguments": args})
        dt = time.time() - t0
        if "error" in msg:
            return dt, False, msg["error"]
        res = msg["result"]
        text = " ".join(c.get("text", "") for c in res.get("content", []))
        return dt, not res.get("isError", False), res.get("structuredContent") or text

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


if __name__ == "__main__":
    exe, cmd = sys.argv[1], sys.argv[2]
    c = Client(exe)
    try:
        if cmd == "list":
            tools = c.request("tools/list", {})["result"]["tools"]
            for t in tools:
                print(t["name"], "-", json.dumps(t.get("inputSchema", {}).get("properties", {}))[:200])
        elif cmd == "call":
            rest = sys.argv[3:]
            for i in range(0, len(rest), 2):
                name, args = rest[i], json.loads(rest[i + 1])
                dt, ok, out = c.call(name, args)
                print(f"{'OK ' if ok else 'ERR'} {name} {json.dumps(args, ensure_ascii=False)} ({dt:.1f}s): "
                      f"{json.dumps(out, ensure_ascii=False)[:600]}")
    finally:
        c.close()
        print("--- fxmcp stderr ---")
        print("\n".join(c.stderr))
