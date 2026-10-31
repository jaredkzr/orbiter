# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Where each texel of a texture lies on a mesh: its position, normal and
# group, rasterized from the triangles in texture space. Designs drawn on
# the vessel itself (texdesign.py) are baked into the texture through
# these maps, so they fit any layout and run on across its islands.
#
#   python3 Utils/BlenderMsh/texbake.py <msh> <texname> <size> <out.npz> [--preview <png>]
#
# The npz holds pos (H x W x 3, metres), nrm (H x W x 3), group (H x W,
# -1 where no triangle), mirror (H x W, texels used by both sides x < 0
# and x > 0), and scale (metres per texel).

import os, sys, math, argparse
import numpy as np
from scipy import ndimage
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import orbiter_msh as om

def bake (msh, texname, W, H):
	m = om.read_msh (msh) if isinstance (msh, str) else msh
	ti = [k for k, t in enumerate (m.textures) if t[0].lower ().replace ("/", "\\").endswith (texname.lower ())]
	pos = np.zeros ((H, W, 3), np.float32)
	nrm = np.zeros ((H, W, 3), np.float32)
	group = np.full ((H, W), -1, np.int32)
	side = np.zeros ((H, W), np.int8)   # (bit 1: x < 0 used, bit 2: x > 0)
	for gi, g in enumerate (m.groups):
		if g.tex not in ti: continue
		V = np.array ([v[:8] for v in g.vtx], np.float64)
		for t in g.tri:
			if len (set (t)) < 3: continue
			P, N, UV = V[list (t), 0:3], V[list (t), 3:6], V[list (t), 6:8]
			o = np.floor (UV.min (0))
			q = (UV - o) * (W, H) - 0.5   # (texel centres)
			x0, y0 = np.floor (q.min (0)).astype (int)
			x1, y1 = np.ceil (q.max (0)).astype (int)
			x0, y0, x1, y1 = max (x0, 0), max (y0, 0), min (x1, W - 1), min (y1, H - 1)
			if x1 < x0 or y1 < y0: continue
			yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
			(ax, ay), (bx, by), (cx, cy) = q
			den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
			if abs (den) < 1e-12: continue
			l0 = ((by - cy) * (xx - cx) + (cx - bx) * (yy - cy)) / den
			l1 = ((cy - ay) * (xx - cx) + (ax - cx) * (yy - cy)) / den
			l2 = 1 - l0 - l1
			e = -0.5 / max (abs (den), 1.0) ** 0.5   # (a little over the edges: no cracks)
			ins = (l0 >= e) & (l1 >= e) & (l2 >= e)
			if not ins.any (): continue
			L = np.stack ([l0[ins], l1[ins], l2[ins]], -1)
			py, px = yy[ins], xx[ins]
			pos[py, px] = L @ P
			n = L @ N
			nrm[py, px] = n / np.maximum (np.linalg.norm (n, axis=-1, keepdims=True), 1e-9)
			group[py, px] = gi
			side[py, px] |= np.where ((L @ P)[:, 0] < -0.02, 1, np.where ((L @ P)[:, 0] > 0.02, 2, 0)).astype (np.int8)
	used = group >= 0
	# metres per texel (median over the used texels, along both axes)
	gx = np.linalg.norm (np.diff (pos, axis=1), axis=-1)[used[:, 1:] & used[:, :-1]]
	gy = np.linalg.norm (np.diff (pos, axis=0), axis=-1)[used[1:] & used[:-1]]
	scale = float (np.median (np.concatenate ([gx[gx < 0.2], gy[gy < 0.2]]))) if used.any () else 0.0
	# (unused texels: their nearest used one's, for mip maps and filtering)
	if used.any ():
		_, (iy, ix) = ndimage.distance_transform_edt (~used, return_indices=True)
		pos, nrm = pos[iy, ix], nrm[iy, ix]
	return dict (pos=pos, nrm=nrm, group=group, mirror=(side == 3), scale=scale, used=used)

if __name__ == "__main__":
	ap = argparse.ArgumentParser ()
	ap.add_argument ("msh"); ap.add_argument ("texname"); ap.add_argument ("size", type=int); ap.add_argument ("out")
	ap.add_argument ("--preview")
	a = ap.parse_args ()
	b = bake (a.msh, a.texname, a.size, a.size)
	np.savez_compressed (a.out, **b)
	print ("%s: %.1f mm per texel, %.0f%% used, %.1f%% shared by both sides" % (a.out, b["scale"] * 1000, b["used"].mean () * 100, b["mirror"].mean () * 100))
	if a.preview:
		from PIL import Image
		p = b["pos"]; lo, hi = p.reshape (-1, 3).min (0), p.reshape (-1, 3).max (0)
		img = (p - lo) / (hi - lo + 1e-9)
		img[~b["used"]] *= 0.3
		img[b["mirror"]] = img[b["mirror"]] * 0.5 + np.array ([0.5, 0, 0.5]) * 0.5
		Image.fromarray ((img * 255).astype (np.uint8)).save (a.preview)
