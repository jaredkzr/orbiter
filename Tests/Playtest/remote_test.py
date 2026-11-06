#!/usr/bin/env python3
"""Remote control (OVP/VulkanClient/RemoteControl.h, Utils/Playtest/orbctl.py):
a session in lockstep answers, stands still between commands and advances
a fixed step per frame; keys and clicks (by a widget's label) act like a
player's; screenshots are written; failing commands answer with an error
and the session goes on; it stops on request.
"""

import json
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ORBCTL = [sys.executable, os.path.join(ROOT, "Utils", "Playtest", "orbctl.py"), "--session", "ctest"]
fails = 0


def run(*args, ok=True):
    p = subprocess.run(ORBCTL + list(args), capture_output=True, text=True, timeout=600)
    if ok and p.returncode != 0:
        raise SystemExit(f"orbctl {' '.join(args)}: exit {p.returncode}: {p.stderr.strip()}")
    return p


def js(*args):
    out = run(*args).stdout
    return json.loads(out) if out.strip() else None


def check(name, cond, msg=""):
    global fails
    print(f"{'PASS' if cond else 'FAIL'} {name} {msg}", flush=True)
    if not cond:
        fails += 1


def main():
    if os.environ.get("ORBITER_BUILD"):
        os.environ["ORBITER_BUILD"] = os.path.abspath(os.environ["ORBITER_BUILD"])
    start = js("start", os.path.join(ROOT, "Tests", "Input", "Orbit.scn"), "--size", "1280x720")
    try:
        check("start", start["window"] == [1280, 720] and start["session"] and start["pace"] == "lockstep", json.dumps(start))
        check("ping", run("ping").stdout.strip() == "pong")

        # lockstep: still between commands, a fixed step per frame
        run("wait", "10")
        t0 = js("state")["simt"]
        time.sleep(2.0)
        t1 = js("state")["simt"]
        check("still", t1 == t0, f"{t0} -> {t1} over 2 s")
        run("wait", "30")
        t2 = js("state")["simt"]
        check("step", abs((t2 - t1) - 30 / 30.0) < 1e-6, f"30 frames: {t2 - t1:.6f} s")
        st = js("state")
        check("state", st["vessel"]["name"] == "GL-I" and st["vessel"]["class"] == "DeltaGlider" and st["camera"] == "locked",
              f"{st['vessel']['name']} {st['camera']}")

        # keys: F2 cycles the camera (Locked -> Flyby)
        run("key", "F2")
        run("wait", "3")
        check("key", js("state")["camera"] == "flyby", js("state")["camera"])

        # a screenshot, at the window's size
        shot = run("shot", "test").stdout.strip()
        ok = os.path.exists(shot)
        if ok:
            with open(shot, "rb") as f:
                head = f.read(24)
            w, h = struct.unpack(">II", head[16:24])
            ok = head[:8] == b"\x89PNG\r\n\x1a\n" and (w, h) == (1280, 720)
        check("screenshot", ok, shot)

        # widgets by label: the camera dialog's Free mode
        run("key", "ctrl+F1")
        run("wait", "5")
        labels = [w["label"] for w in js("widgets", "camera")]
        check("widgets", "Free" in labels and "Locked" in labels, ", ".join(labels[:12]))
        run("click", "@ui:Camera/Free")
        run("wait", "20")
        check("click_label", js("state")["camera"] == "free", js("state")["camera"])

        # errors are answered, the session goes on
        p = run("eval", "nosuch.thing ()", ok=False)
        check("lua_error", p.returncode == 2 and "nosuch" in p.stderr, p.stderr.strip())
        run("send", "timeout 1")
        p = run("click", "@ui:No such button", ok=False)
        check("missing_widget", p.returncode == 2 and "not found" in p.stderr, p.stderr.strip())
        check("alive", run("ping").stdout.strip() == "pong")
        p = run("log", ok=False)
        check("log", p.returncode == 0 and "Camera" not in p.stderr, f"{len(p.stdout)} characters")
    finally:
        run("stop", ok=False)
    sess = json.load(open(os.path.join(ROOT, "out", "playtest", "ctest", "session.json")))
    gone = False
    for _ in range(40):
        try:
            os.kill(sess["pid"], 0)
        except OSError:
            gone = True
            break
        time.sleep(0.25)
    check("stopped", gone)
    print(f"{fails} failed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
