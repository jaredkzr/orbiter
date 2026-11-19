#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""The Cape's decal sheet (Textures/BaseKit/cape_decals.dds and its maps):
what the Vehicle Assembly Building wears on its south face, drawn for
blender_kit.py cape (uv 'decal': DECALS gives each one's rectangle on the
sheet, u, v in 0..1). The sheet is 2048 px square and all of it used: each
decal is stretched down its rectangle (the mesh maps it back), so its
texels are finer along the height than its real proportions would give.
The sheet is flat: no normal map is written. The wall under the decals is
capesurf.py's (cape_vab: the building's faces, painted as wholes); WALL here
is its white, for the insignia's edge.

  - the flag of the United States, hung vertically as on the building
    (63.7 x 33.5 m: its stripes upright, the union uppermost on the
    observer's left)
  - the NASA insignia (the "meatball", 40.2 m wide x 33.5 m tall as on the
    building: 132 x 110 ft), rendered from the official artwork
    (Utils/BaseKit/assets/NASA_insignia.svg: Wikimedia Commons
    File:NASA_logo.svg, the NASA insignia, public domain as a work of the US
    government) with librsvg (PyGObject: gi Rsvg, cairo). The parts of its
    rectangle it paints - the sphere and the vector's ends out past it - are
    written to assets/NASA_insignia_patches.json for blender_kit.py, which cuts
    the decal's faces to them (the wall between stays the building's own).

    capetex.py [--preview DIR]
"""

import argparse, math, os, sys
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
sys.path.insert (0, HERE)

N = 2048
# (u0, v0, u1, v1) of each decal on the sheet (v down from its top)
DECALS = {
	'flag': (0.0, 0.0, 0.390625, 1.0),
	'nasa': (0.390625, 0.0, 1.0, 1.0),
}
RED, WHITE, BLUE = (178, 34, 52), (245, 245, 245), (60, 59, 110)
WALL = (226, 226, 225)  # (the building's white where the decals are: capesurf.py VAB_WHITE; about the insignia)
SS = 4  # (supersampling)

# ----------------------------------------------------------------------
# the insignia (no PIL, no gi here: blender_kit.py reads its patches)

INSIGNIA_SIZE = (40.2, 33.5)   # (on the building [m]: across, up)
INSIGNIA_SVG = os.path.join (HERE, 'assets', 'NASA_insignia.svg')
INSIGNIA_PATCHES = os.path.join (HERE, 'assets', 'NASA_insignia_patches.json')

def insignia_patches ():
	"""what of the insignia's rectangle is painted, as convex polygons (points s, t in 0..1 of
	the rectangle, t down): the sphere and the vector's ends out past it (made by main ())"""
	import json
	return json.load (open (INSIGNIA_PATCHES))

def render_svg (path, W, H):
	"""the SVG drawn into W x H px (its aspect kept, centred): RGBA floats 0..1, not premultiplied"""
	import gi
	gi.require_version ('Rsvg', '2.0')
	from gi.repository import Rsvg
	import cairo
	surf = cairo.ImageSurface (cairo.FORMAT_ARGB32, W, H)
	ctx = cairo.Context (surf)
	h = Rsvg.Handle.new_from_file (path)
	vp = Rsvg.Rectangle (); vp.x, vp.y, vp.width, vp.height = 0, 0, W, H
	h.render_document (ctx, vp)
	surf.flush ()
	a = np.frombuffer (surf.get_data (), np.uint8).reshape (H, surf.get_stride () // 4, 4)[:, :W].astype (np.float32) / 255.0
	b, g, r, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]   # (cairo: premultiplied BGRA, little-endian)
	k = np.where (al > 0, 1.0 / np.maximum (al, 1e-6), 0.0)
	return np.stack ([r * k, g * k, b * k, al], axis=2)

def _hull (pts):
	pts = sorted (set (pts))
	def half (ps):
		h = []
		for p in ps:
			while len (h) >= 2 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0: h.pop ()
			h.append (p)
		return h[:-1]
	return half (pts) + half (pts[::-1])

def make_patches (W=1206, H=1005, n=72):
	"""the painted parts of the insignia's rectangle: its sphere (the SVG's circle, a hair
	larger), and the hull of each piece of it out past the sphere (the vector's ends), grown
	by a few px - in s, t of the rectangle"""
	import json
	from scipy import ndimage
	al = render_svg (INSIGNIA_SVG, W, H)[..., 3]
	sc = min (W / 110.0, H / 92.0); ox, oy = (W - 110.0 * sc) / 2, (H - 92.0 * sc) / 2   # (the viewBox, fitted)
	cx, cy, r = ox + 50.049 * sc, oy + 45.0 * sc, 40.14 * sc                              # (its sphere: the SVG's circle)
	yy, xx = np.mgrid[0:H, 0:W]
	out = (al > 0.02) & (np.hypot (xx - cx, yy - cy) > r * 1.004)
	lab, k = ndimage.label (ndimage.binary_dilation (out, iterations=4))
	polys = [[((cx + 1.012 * r * math.cos (2 * math.pi * i / n)) / W, (cy + 1.012 * r * math.sin (2 * math.pi * i / n)) / H) for i in range (n)]]
	for c in range (1, k + 1):
		ys, xs = np.nonzero (lab == c)
		if len (xs) < 20: continue
		# (and a little into the sphere, so the pieces overlap it)
		pts = [(x + dx, y + dy) for x, y in zip (xs, ys) for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1))]
		polys.append ([(min (max (x / W, 0), 1), min (max (y / H, 0), 1)) for x, y in _hull (pts)])
	json.dump (polys, open (INSIGNIA_PATCHES, 'w'), indent=0)
	return polys

def star (d, cx, cy, r, fill, rot=0.0):
	pts = []
	for k in range (10):
		a = rot - math.pi / 2 + k * math.pi / 5
		rr = r if k % 2 == 0 else r * 0.382
		pts.append ((cx + rr * math.cos (a), cy + rr * math.sin (a)))
	d.polygon (pts, fill=fill)


def flag (w, h):
	"""the flag hung vertically, fitted to w x h px: drawn level in its own proportions (1.9:
	fly W across, hoist H down), then turned a quarter clockwise and mirrored - stripes
	upright, the union top left"""
	H = w * SS; W = int (round (1.9 * H))
	im = Image.new ('RGB', (W, H), WHITE)
	d = ImageDraw.Draw (im)
	sh = H / 13.0
	for k in range (13):
		if k % 2 == 0: d.rectangle ([0, k * sh, W, (k + 1) * sh], fill=RED)
	cw, ch = 0.76 * H, 7 * sh
	d.rectangle ([0, 0, cw, ch], fill=BLUE)
	for row in range (9):
		n = 6 if row % 2 == 0 else 5
		for i in range (n):
			x = cw * (2 * i + (1 if row % 2 else 0) + 1) / 12.0
			y = ch * (row + 1) / 10.0
			star (d, x, y, 0.0616 * H / 2 * 1.0, WHITE)
	im = im.transpose (Image.Transpose.ROTATE_270).transpose (Image.Transpose.FLIP_LEFT_RIGHT)
	return im.resize ((w, h), Image.Resampling.LANCZOS)


def insignia (w, h):
	"""the NASA insignia (the SVG) in w x h px: rendered at its proportions on the wall's paint,
	supersampled, then fitted to the rectangle"""
	iw, ih = INSIGNIA_SIZE
	Wp = w * SS; Hp = int (round (Wp * ih / iw))
	rgba = render_svg (INSIGNIA_SVG, Wp, Hp)
	wall = np.array (WALL, np.float32) / 255.0
	rgb = rgba[..., :3] * rgba[..., 3:] + wall * (1 - rgba[..., 3:])
	im = Image.fromarray ((np.clip (rgb, 0, 1) * 255 + 0.5).astype (np.uint8))
	return im.resize ((w, h), Image.Resampling.LANCZOS)


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--preview')
	a = ap.parse_args ()
	import kittex
	global Image, ImageDraw, ImageFont
	from PIL import Image, ImageDraw, ImageFont
	make_patches ()
	sheet = Image.new ('RGB', (N, N), WALL)  # (the wall's paint between decals)
	for name, (u0, v0, u1, v1) in DECALS.items ():
		w, h = int (round ((u1 - u0) * N)), int (round ((v1 - v0) * N))
		im = flag (w, h) if name == 'flag' else insignia (w, h)
		sheet.paste (im, (int (round (u0 * N)), int (round (v0 * N))))
	rgb = np.asarray (sheet).astype (np.float32) / 255.0  # (sRGB, as write_dds stores it)
	z = np.zeros ((N, N), np.float32)
	kittex.write ('cape_decals', rgb, z + 0.35, z, 64.0, preview=a.preview, norm=False)  # (flat: no normal map)


if __name__ == '__main__':
	main ()
