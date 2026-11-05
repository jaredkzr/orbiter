#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Preview images of the scenarios for the launchpad.

Each scenario is run offscreen with the Vulkan client and a frame without
the user interface is saved as <scenario>.jpg beside the scenario file (the
launchpad shows Scenarios/<path>.jpg|png). A scenario that starts in the
cockpit is shown from outside, behind its vessel.

  Utils/ScenarioShots/render.py [--build DIR] [--only TEXT] [--force]

--build: the build directory (the Orbiter installation; default
out/build/linux-x64-headless-release); --only: scenarios whose path contains
TEXT; --force: render scenarios that have an image already. The images are
written into the source tree's Scenarios directory (the build copies them).
Orbiter.cfg of the build directory is restored after each run.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

SKIP = ("Tests/", "Bench/", "VkTest/", "Scratch/", "Quicksave/")
SIZE = (960, 540)
FRAME = 240  # frames before the image (the scene settled, the exposure adapted)

# switch a scenario starting in the cockpit to an outside view
CAMERA = """wait 20
lua v = vessel.get_focusinterface ()
lua if v then local c, p = oapi.get_cameraglobalpos (), v:get_globalpos (); local d = math.sqrt ((c.x-p.x)^2 + (c.y-p.y)^2 + (c.z-p.z)^2); if d < v:get_size () then oapi.set_cameramode ({mode='track', trackmode='relative', reldist=3.2, phi=205, tht=12}) end end
"""


def main():
    ap = argparse.ArgumentParser (description=__doc__.split ("\n")[0])
    ap.add_argument ("--build", default="out/build/linux-x64-headless-release")
    ap.add_argument ("--only", default="")
    ap.add_argument ("--force", action="store_true")
    a = ap.parse_args ()

    src = os.path.abspath ("Scenarios")
    build = os.path.abspath (a.build)
    if not os.path.isdir (src) or not os.path.isfile (os.path.join (build, "Orbiter")):
        sys.exit ("run from the source tree's root, with a build in --build")
    cfg = os.path.join (build, "Orbiter.cfg")
    tmp = tempfile.mkdtemp (prefix="scnshots")
    cfgSaved = os.path.join (tmp, "Orbiter.cfg")
    shutil.copy (cfg, cfgSaved)
    script = os.path.join (tmp, "camera.input")
    with open (script, "w") as f:
        f.write (CAMERA)

    todo = []
    for root, dirs, files in os.walk (src):
        dirs.sort ()
        for n in sorted (files):
            if not n.endswith (".scn"):
                continue
            rel = os.path.relpath (os.path.join (root, n), src)[:-4]
            if rel.startswith (SKIP) or a.only not in rel or rel == "(Current state)":
                continue
            out = os.path.join (src, rel + ".jpg")
            if os.path.exists (out) and not a.force:
                continue
            todo.append (rel)

    failed = []
    for i, rel in enumerate (todo):
        shot = os.path.join (tmp, "shot.png")
        if os.path.exists (shot):
            os.remove (shot)
        env = dict (os.environ, ORBITER_VK_OFFSCREEN="1", ORBITER_VK_SCREENSHOT=shot,
                    ORBITER_VK_SCREENSHOT_FRAME=str (FRAME), ORBITER_VK_SCREENSHOT_EXIT="1",
                    ORBITER_VK_SCREENSHOT_NOUI="1", ORBITER_VK_TEST_INPUT=script)
        try:
            subprocess.run (["./Orbiter", "-p", "VulkanClient", "--scenariox=" + os.path.join (build, "Scenarios", rel + ".scn")],
                            cwd=build, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=240)
        except subprocess.TimeoutExpired:
            pass
        shutil.copy (cfgSaved, cfg)
        if not os.path.exists (shot):
            failed.append (rel)
            print ("[%d/%d] %s: no image" % (i + 1, len (todo), rel), flush=True)
            continue
        im = Image.open (shot).convert ("RGB")
        # (cropped to 16:9, then scaled)
        w, h = im.size
        if w * 9 > h * 16:
            c = (w - h * 16 // 9) // 2
            im = im.crop ((c, 0, c + h * 16 // 9, h))
        elif w * 9 < h * 16:
            c = (h - w * 9 // 16) // 2
            im = im.crop ((0, c, w, c + w * 9 // 16))
        im = im.resize (SIZE, Image.LANCZOS)
        out = os.path.join (src, rel + ".jpg")
        im.save (out, "JPEG", quality=80, optimize=True, progressive=True)
        print ("[%d/%d] %s" % (i + 1, len (todo), rel), flush=True)

    shutil.rmtree (tmp, ignore_errors=True)
    if failed:
        print ("no image for: " + ", ".join (failed))


if __name__ == "__main__":
    main ()
