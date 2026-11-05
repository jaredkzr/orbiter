#!/usr/bin/env python3
"""Download the built star catalogue and dust map of the simulated sky.

  download.py [--dir Config/CSphere/Sky] [--url BASE]

stars.bin (140 MB) and dust.bin (50 MB) are too large for git: they are
published as the assets of a GitHub release and checked against their
SHA-256 here. Run once after cloning, from the repository's root (the build
copies Config into the build tree). Files already present and correct are
left alone. To build them from their sources instead, see README.md.

Credits: Gaia DR3 and Hipparcos: ESA/Gaia/DPAC, ESA (CC BY-NC 3.0 IGO);
dust: Edenhofer et al. 2024 (CC BY 4.0); see README.md.
"""

import argparse
import hashlib
import os
import sys
import urllib.request

BASE = "https://github.com/jaredkzr/orbiter/releases/download/sky-data-1"
FILES = {
    "stars.bin": "caff3ad6f4025bec966e8e6c2ec78dcf56860c37a13a71b5cb310d56c708a09c",
    "dust.bin": "524bb61b3a3beeca37a3e2697825efd0d4f40d5352936bf292aeae32b4ef407b",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=os.path.join("Config", "CSphere", "Sky"))
    ap.add_argument("--url", default=BASE)
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        sys.exit(f"{a.dir}: no such directory (run from the repository's root, or give --dir)")
    for name, digest in FILES.items():
        path = os.path.join(a.dir, name)
        if os.path.exists(path) and sha256(path) == digest:
            print(f"{name}: present")
            continue
        url = f"{a.url}/{name}"
        tmp = path + ".part"
        print(f"{name}: downloading {url}")
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            for block in iter(lambda: r.read(1 << 20), b""):
                f.write(block)
                done += len(block)
                if total:
                    print(f"\r  {done >> 20} / {total >> 20} MB", end="", flush=True)
            print()
        if sha256(tmp) != digest:
            os.remove(tmp)
            sys.exit(f"{name}: checksum mismatch (download damaged, or the release changed)")
        os.replace(tmp, path)
        print(f"{name}: ok")


if __name__ == "__main__":
    main()
