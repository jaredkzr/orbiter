#!/usr/bin/env python3
"""Play Orbiter from the command line: start a session, look, act, check.

Drives Orbiter's remote control (ORBITER_VK_REMOTE, see
OVP/VulkanClient/RemoteControl.h): mouse and keyboard go into the window's
event queue like a player's, screenshots show what a player sees, and the
flight's state can be read. Made for playtesting (by a person or an agent).

  orbctl.py start [SCENARIO] [--window] [--realtime] [--size WxH] [--step S] [--damage on|off]
                                     SCENARIO: a name under Scenarios/ ("Delta-glider/DG-S
                                     on runway") or a .scn file; none: the launchpad
  orbctl.py shot [NAME]              screenshot (prints its path)
  orbctl.py key KEY                  a key press (SDL names): F2, M, Space, "Keypad 8", "Keypad +",
                                     PageUp, ctrl+F1, shift+F2, alt+F1
  orbctl.py hold KEY FRAMES          hold a key for a number of frames
  orbctl.py click POS [right]        POS: x,y [pixels] or @anchor (see "anchors")
  orbctl.py drag POS POS [FRAMES]    left-button drag
  orbctl.py move POS | wheel N       mouse motion, wheel notches
  orbctl.py wait FRAMES              let frames pass (simulation time: FRAMES x step)
  orbctl.py state                    the flight's state (JSON)
  orbctl.py eval EXPR | lua CODE     Lua (Orbiter's script API); the value as JSON
  orbctl.py widgets [FILTER]         the buttons, tabs, fields... on screen, with their labels
                                     (click @ui:LABEL, @ui:WINDOW/LABEL, @ui:LABEL[2])
  orbctl.py anchors [PREFIX]         named elements (MFD buttons, map handles...)
  orbctl.py log                      what Orbiter.log gained since the last "log"
  orbctl.py info | ping              window size, frame, session; alive?
  orbctl.py pace lockstep|realtime   frames only while commands run, or real time
  orbctl.py run CMD [CMD...]         several raw commands in turn (stops at an error)
  orbctl.py send CMD                 one raw command (OVP/VulkanClient/TestInput.h)
  orbctl.py stop                     end Orbiter

Options before the command: --session NAME (default "default"; several can
run at once). Sessions live in out/playtest/NAME: links to the build tree,
their own Orbiter.cfg (the build's, windowed at --size), Orbiter.log,
scenarios saved, screenshots in shots/.

Lockstep (default): the simulation moves only while a command runs, a
fixed step per frame (--step, default 1/30 s), so taking time to decide
changes nothing. realtime: Orbiter runs on in real time.
"""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.environ.get("ORBITER_BUILD", os.path.join(ROOT, "out", "build", "linux-x64-headless-release"))
SESSIONS = os.path.join(ROOT, "out", "playtest")
# kept per session (not linked to the build tree): written by Orbiter
OWN = {"Orbiter.cfg", "Orbiter.log", "imgui.ini", "MFDWindows.cfg", "Scenarios"}
MODS = {"ctrl": "Left Ctrl", "shift": "Left Shift", "alt": "Left Alt"}


def die(msg, code=1):
    print(msg, file=sys.stderr)
    sys.exit(code)


def session_dir(name):
    return os.path.join(SESSIONS, name)


def load_session(name):
    p = os.path.join(session_dir(name), "session.json")
    if not os.path.exists(p):
        die(f"no session '{name}' (orbctl.py start)")
    with open(p) as f:
        return json.load(f)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# ---- the run directory ----------------------------------------------------

def make_run_dir(d, size, damage):
    os.makedirs(d, exist_ok=True)
    for e in os.listdir(BUILD):
        if e in OWN:
            continue
        link = os.path.join(d, e)
        if not os.path.lexists(link):
            os.symlink(os.path.join(BUILD, e), link)
    # scenarios: a folder of its own (saved states stay here), its entries linked
    sd = os.path.join(d, "Scenarios")
    os.makedirs(sd, exist_ok=True)
    for e in os.listdir(os.path.join(BUILD, "Scenarios")):
        link = os.path.join(sd, e)
        if not os.path.lexists(link) and not e.startswith("("):
            os.symlink(os.path.join(BUILD, "Scenarios", e), link)
    os.makedirs(os.path.join(d, "shots"), exist_ok=True)
    # the build's configuration, in a window of the given size
    cfg = ""
    src = os.path.join(BUILD, "Orbiter.cfg")
    if os.path.exists(src):
        with open(src) as f:
            cfg = f.read()
    w, h = size
    def setkey(text, key, value):
        pat = re.compile(r"^\s*" + key + r"\s*=.*$", re.M)
        line = f"{key} = {value}"
        return pat.sub(line, text) if pat.search(text) else text.rstrip("\n") + "\n" + line + "\n"
    cfg = setkey(cfg, "Fullscreen", "FALSE")
    cfg = setkey(cfg, "WindowWidth", str(w))
    cfg = setkey(cfg, "WindowHeight", str(h))
    if damage is not None:
        cfg = setkey(cfg, "DamageModel", "1" if damage else "0")
    with open(os.path.join(d, "Orbiter.cfg"), "w") as f:
        f.write(cfg)


# ---- talking to Orbiter ---------------------------------------------------

def request(sess, line, timeout=600.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sess["socket"])
    except OSError as e:
        die(f"cannot reach Orbiter ({e}): is the session running? (orbctl.py info)")
    s.sendall((line + "\n").encode())
    buf = b""
    try:
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                die("Orbiter closed the connection (did it exit? see its Orbiter.log)")
            buf += chunk
    except socket.timeout:
        die(f"no answer within {timeout:.0f} s")
    finally:
        s.close()
    return json.loads(buf.split(b"\n", 1)[0])


def show(reply):
    if not reply.get("ok"):
        die("error: " + str(reply.get("error")), 2)
    r = reply.get("result")
    if isinstance(r, str):
        print(r, end="" if r.endswith("\n") else "\n")
    elif r is not None:
        print(json.dumps(r, indent=1))


def split_mods(spec):
    # "ctrl+shift+F1" -> (["Left Ctrl", "Left Shift"], "F1"); only leading ctrl/shift/alt
    # count as modifiers, so a key named with a "+" ("Keypad +") stays whole
    mods = []
    while True:
        m = re.match(r"(?i)(ctrl|shift|alt)\+(.+)$", spec)
        if not m:
            return mods, spec
        mods.append(MODS[m.group(1).lower()])
        spec = m.group(2)


def key_commands(spec):
    # "ctrl+F1": modifiers held around the key
    mods, key = split_mods(spec)
    return [f"keydown {m}" for m in mods] + [f"key {key}"] + [f"keyup {m}" for m in reversed(mods)]


def run_lines(sess, lines):
    last = None
    for l in lines:
        last = request(sess, l)
        if not last.get("ok"):
            return last
    return last


# ---- commands ---------------------------------------------------------------

def cmd_start(name, args):
    scenario, window, realtime, size, step, damage = None, False, False, (1920, 1080), None, None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--window": window = True
        elif a == "--realtime": realtime = True
        elif a == "--size": i += 1; size = tuple(int(v) for v in args[i].lower().split("x"))
        elif a == "--step": i += 1; step = float(args[i])
        elif a == "--damage": i += 1; damage = args[i] in ("on", "1", "true")
        elif a.startswith("--"): die(f"unknown option {a}")
        else: scenario = a
        i += 1
    d = session_dir(name)
    old = os.path.join(d, "session.json")
    if os.path.exists(old):
        with open(old) as f:
            if alive(json.load(f)["pid"]):
                die(f"session '{name}' is running (orbctl.py --session {name} stop)")
    make_run_dir(d, size, damage)
    sock = os.path.join(d, "remote.sock")
    if os.path.exists(sock):
        os.remove(sock)
    if step is None and not realtime:
        step = 1.0 / 30.0  # (lockstep: a fixed step, or the time spent deciding would pass at once)
    env = dict(os.environ, ORBITER_VK_REMOTE=sock, ORBITER_VK_REMOTE_PACE="realtime" if realtime else "lockstep")
    if not window:
        env["ORBITER_VK_OFFSCREEN"] = "1"
    if step:
        env["ORBITER_VK_FRAMETIME"] = repr(step)
    cmd = ["./Orbiter", "-p", "VulkanClient"]
    if scenario:
        if os.path.isfile(scenario):  # (a file anywhere: its absolute path)
            cmd.append(f"--scenario={os.path.abspath(scenario)}")
        else:  # (a scenario's name, under Scenarios/)
            scn = scenario[:-4] if scenario.endswith(".scn") else scenario
            cmd.append(f"--scenario={scn}")
    if step:
        cmd.append(f"--fixedstep={step}")
    memmax = os.environ.get("ORBCTL_MEMMAX")  # e.g. 6G: run in a systemd scope that kills only Orbiter past this RAM
    if memmax:
        cmd = ["systemd-run", "--user", "--scope", "-q", "-p", f"MemoryMax={memmax}", "-p", "MemorySwapMax=0"] + cmd
    with open(os.path.join(d, "orbiter.out"), "w") as out:
        p = subprocess.Popen(cmd, cwd=d, env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    sess = {"pid": p.pid, "socket": sock, "dir": d, "scenario": scenario, "shots": 0}
    with open(old, "w") as f:
        json.dump(sess, f)
    t0 = time.time()
    while time.time() - t0 < 180:
        if p.poll() is not None:
            die(f"Orbiter exited ({p.returncode}): see {d}/Orbiter.log and orbiter.out")
        if os.path.exists(sock):
            try:
                r = request(sess, "info", timeout=60)
                print(json.dumps({"name": name, "dir": d, "pid": p.pid, **r["result"]}))
                return
            except SystemExit:
                pass
        time.sleep(0.5)
    die("Orbiter did not open its remote control in 3 minutes")


def cmd_stop(name):
    sess = load_session(name)
    if alive(sess["pid"]):
        try:
            request(sess, "quit", timeout=10)
        except SystemExit:
            pass
        for _ in range(40):
            if not alive(sess["pid"]):
                break
            time.sleep(0.25)
        if alive(sess["pid"]):
            os.killpg(sess["pid"], signal.SIGTERM)
    print("stopped")


def cmd_shot(name, args):
    sess = load_session(name)
    sess["shots"] = sess.get("shots", 0) + 1
    tag = ("-" + re.sub(r"[^A-Za-z0-9_.-]", "_", args[0])) if args else ""
    path = os.path.join(sess["dir"], "shots", f"{sess['shots']:04d}{tag}.png")
    with open(os.path.join(sess["dir"], "session.json"), "w") as f:
        json.dump(sess, f)
    r = request(sess, "screenshot " + path)
    if not r.get("ok"):
        show(r)
    print(path)


def main():
    argv = sys.argv[1:]
    name = "default"
    if len(argv) >= 2 and argv[0] == "--session":
        name, argv = argv[1], argv[2:]
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return
    c, args = argv[0], argv[1:]
    if c == "start":
        return cmd_start(name, args)
    if c == "stop":
        return cmd_stop(name)
    if c == "shot":
        return cmd_shot(name, args)
    sess = load_session(name)
    if c == "key":
        return show(run_lines(sess, key_commands(args[0])))
    if c == "hold":
        mods, key = split_mods(args[0])
        lines = [f"keydown {m}" for m in mods] + [f"keydown {key}", f"wait {int(args[1])}", f"keyup {key}"] + [f"keyup {m}" for m in reversed(mods)]
        return show(run_lines(sess, lines))
    if c == "run":
        lines = []
        for a in args:
            if a.startswith("key ") and split_mods(a[4:].strip())[0]:
                lines += key_commands(a[4:].strip())
            else:
                lines.append(a)
        return show(run_lines(sess, lines))
    if c == "send":
        return show(request(sess, " ".join(args)))
    if c in ("click", "drag", "move", "waitfor"):  # (positions with spaces quoted: "@ui:Brighton Beach")
        return show(request(sess, " ".join([c] + [f'"{a}"' if " " in a else a for a in args])))
    if c in ("wheel", "wait", "eval", "lua", "anchors", "widgets", "state", "log", "info", "ping", "pace", "down", "up", "keydown", "keyup"):
        return show(request(sess, " ".join([c] + args)))
    die(f"unknown command '{c}' (orbctl.py help)")


if __name__ == "__main__":
    main()
