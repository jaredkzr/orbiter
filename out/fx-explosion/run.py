#!/usr/bin/env python3
"""Explosion harness: a crash scene run offscreen (orbctl, lockstep 30 fps), stills
re-based on the break-up, a labelled contact sheet; optionally a video and frame times.

  run.py SCENE OUTDIR [--video PRE POST] [--ft] [--size WxH] [--times t,t,...] [--tag NAME] [--pausecheck T]

SCENE: sea land night small large midair vacuum (SCENES below). The break-up is the
first frame with 3 pieces of debris or more (oapi.get_debriscount(): a piece or two
breaking off before it are not it - the land crash loses a section 0.25 s before); stills at TIMES s after it
(default 0.05 .. 20), OUTDIR/sNN.png, sheet OUTDIR/../<SCENE>_sheet.png (or <tag>_sheet).
--video PRE POST: every frame from PRE s before the break-up to POST s after,
OUTDIR/video/f_NNNNN.png (then video.sh). --ft: ORBITER_VK_PROFILE=1 +
ORBITER_VK_FRAMETIMES=OUTDIR/frames.ft (perf.py). The Orbiter log is copied to
OUTDIR/orbiter.log. The sim pauses 4 s after the focus vessel is destroyed (the
flight-over menu): unpaused every frame after the break-up.
"""
import json, os, shutil, subprocess, sys, time
sys.path.insert(0, "/home/jared/projects/orbiter/Utils/Playtest")
import orbctl

ROOT = "/home/jared/projects/orbiter"
BUILD = ROOT + "/out/build/linux-x64-headless-release"
HERE = ROOT + "/out/fx-explosion"
FPS = 30
TIMES = [0.05, 0.1, 0.3, 0.5, 0.8, 1.2, 2.0, 3.0, 5.0, 8.0, 12.0, 20.0]

# the 40 m/s, 12 deg dive of out/water-video/stills.sh: a fixed observer 170 m to the side
CRASH = dict(SPEED="40", ALT="25", DIVE="12", PITCH="2", HOLD="1", CAM_X="-170", CAM_Z="110", CAM_Y="8",
             CAM_FOV="40", CAM_PHI="-97", CAM_THT="4", LOGDT="0.5", CAM_HOLD_T="60")
SCENES = {
    "sea":    ("fx_sea", dict(CRASH)),
    "land":   ("fx_land", dict(CRASH, GROUND_REL="1")),
    "night":  ("fx_night", dict(CRASH)),
    # (the sea crash from the other side: the observer south of it looking north, the sun (SSE, high) behind him:
    # the ball's sunlit side; "sea" sees its shade side)
    "sunside": ("fx_sea", dict(CRASH, CAM_X="170", CAM_PHI="97")),
    # (the sea crash from 60 m aside, 72 m from the crash, a wider view: the fireball's surface close up)
    "close":  ("fx_sea", dict(CRASH, CAM_X="-60", CAM_Z="39", CAM_FOV="70", CAM_THT="16", CAM_PHI="-72")),
    "small":  ("fx_small", dict(CRASH, SPEED="70", DIVE="15", CAM_X="-120", CAM_Z="150", CAM_Y="5")),
    "large":  ("fx_large", dict(CRASH, SPEED="60", ALT="60", CAM_X="-600", CAM_Z="250", CAM_Y="20", CAM_THT="8", CAM_PHI="-100")),
    # (two DGs collide at 300 m, 150 m/s: a ground observer 250 m aside, 60 m below)
    "midair": ("midair", dict(CAM="ground", CAM_FOV="50")),
    # (a DG into the ISS at 30 m/s: the vacuum flash)
    "vacuum": ("iss_hit", dict()),
}


def install():
    # (the harness' scratch scenes and scripts into the build tree: gitignored there)
    for sub, dst in (("Scenarios", "Scenarios/VkTest"), ("Script", "Script/VkTest")):
        src = os.path.join(HERE, "harness", sub)
        for f in os.listdir(src):
            shutil.copy(os.path.join(src, f), os.path.join(BUILD, dst, f))


def label_sheet(outdir, shots, sheet, cols=4):
    from PIL import Image, ImageDraw, ImageFont
    ims = []
    for name, t in shots:
        p = os.path.join(outdir, name + ".png")
        if not os.path.exists(p): continue
        im = Image.open(p).convert("RGB")
        w = 480; im = im.resize((w, round(im.height * w / im.width)))
        d = ImageDraw.Draw(im)
        try: font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
        except OSError: font = ImageFont.load_default()
        txt = f"t+{t:g} s"
        d.rectangle((0, 0, 9 * len(txt) + 14, 26), fill=(0, 0, 0))
        d.text((6, 3), txt, fill=(255, 255, 255), font=font)
        ims.append(im)
    if not ims: return
    rows = (len(ims) + cols - 1) // cols
    W, H = ims[0].width, ims[0].height
    S = Image.new("RGB", (cols * W, rows * H))
    for i, im in enumerate(ims): S.paste(im, ((i % cols) * W, (i // cols) * H))
    S.save(sheet)


def main():
    a = sys.argv[1:]
    scene, outdir = a[0], os.path.abspath(a[1])
    video, ft, size, times, tag = None, False, "1280x720", TIMES, None
    pausecheck = None  # (s after the break-up: paused there, three frames apart, OUTDIR/pause_N.png)
    i = 2
    while i < len(a):
        if a[i] == "--video": video = (float(a[i + 1]), float(a[i + 2])); i += 2
        elif a[i] == "--ft": ft = True
        elif a[i] == "--size": size = a[i + 1]; i += 1
        elif a[i] == "--times": times = [float(x) for x in a[i + 1].split(",")]; i += 1
        elif a[i] == "--tag": tag = a[i + 1]; i += 1
        elif a[i] == "--pausecheck": pausecheck = float(a[i + 1]); i += 1
        i += 1
    scn, env = SCENES[scene]
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)
    install()
    os.environ.update(env)
    os.environ.update(ORBCTL_MEMMAX=os.environ.get("ORBCTL_MEMMAX", "6G"), ORBITER_VK_HIDE_UI="1",
                      ORBITER_VK_CUT_LOG="1", ORBITER_VK_FIREBALL_LOG="1", ORBITER_VK_PARTICLE_STATS="1")
    if ft:
        os.environ.update(ORBITER_VK_PROFILE="1", ORBITER_VK_FRAMETIMES=os.path.join(outdir, "frames.ft"))
    sess_name = "fxexpl"
    os.chdir(BUILD)
    r = subprocess.run([sys.executable, ROOT + "/Utils/Playtest/orbctl.py", "--session", sess_name, "start",
                        f"{BUILD}/Scenarios/VkTest/{scn}.scn", "--size", size, "--damage", "on"], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr); sys.exit(1)
    sess = orbctl.load_session(sess_name)
    t0 = time.time()

    def req(line):
        q = orbctl.request(sess, line)
        if not q.get("ok"): print("ERROR", line[:100], q, flush=True)
        return q

    frame, tb, simtb = 0, None, None
    pre, post = video if video else (0.0, 0.0)
    end = max(max(times), post) + 0.2
    snaps = {}
    vid = os.path.join(outdir, "video")
    if video: os.makedirs(vid)
    ring = []   # (the frames before the break-up kept for the video: PRE s of them)
    nv = 0
    try:
        while True:
            if tb is None:
                q = req("eval (function() return string.format('%d %.4f', oapi.get_debriscount(), oapi.get_simtime()) end)()")
                n, st = q.get("result", "0 0").split()
                if int(n) >= 3:
                    tb, simtb = frame, float(st)
                    print(f"break-up at frame {frame} sim {simtb:.3f} wall {time.time()-t0:.0f} s", flush=True)
                    snaps = {round(tb + t * FPS): (f"s{k:02d}", t) for k, t in enumerate(times)}
                    # (the frames kept before it, renamed in order)
                    for p in ring: os.rename(p, os.path.join(vid, f"f_{nv:05d}.png")); nv += 1
                    ring = []
                elif frame > 60 * FPS:
                    print("no break-up in 60 s"); break
            if tb is not None and pausecheck is not None and frame == tb + round(pausecheck * FPS):
                req("lua oapi.set_pause(true)")
                for k in range(3):
                    req("wait 4"); req("screenshot " + os.path.join(outdir, f"pause_{k}.png"))
            if tb is not None:
                req("lua oapi.set_pause(false)")
                if frame - tb > end * FPS: break
            if tb is not None and frame in snaps:
                req("screenshot " + os.path.join(outdir, snaps[frame][0] + ".png"))
            elif video and (tb is None or frame - tb <= post * FPS):
                if tb is None:
                    p = os.path.join(vid, f"pre_{frame:05d}.png")
                    req("screenshot " + p); ring.append(p)
                    while len(ring) > pre * FPS: os.remove(ring.pop(0))
                else:
                    req("screenshot " + os.path.join(vid, f"f_{nv:05d}.png")); nv += 1
            else:
                req("wait 1")
            frame += 1
    finally:
        subprocess.run([sys.executable, ROOT + "/Utils/Playtest/orbctl.py", "--session", sess_name, "stop"], capture_output=True)
        log = os.path.join(sess["dir"], "Orbiter.log")
        if os.path.exists(log): shutil.copy(log, os.path.join(outdir, "orbiter.log"))
    json.dump({"scene": scene, "breakup_frame": tb, "breakup_simt": simtb, "times": times, "video": video},
              open(os.path.join(outdir, "run.json"), "w"))
    shots = sorted(snaps.values(), key=lambda s: s[1])
    label_sheet(outdir, shots, os.path.join(os.path.dirname(outdir), f"{tag or os.path.basename(outdir)}_sheet.png"))
    print(f"done: {frame} frames, wall {time.time()-t0:.0f} s", flush=True)


if __name__ == "__main__":
    main()
