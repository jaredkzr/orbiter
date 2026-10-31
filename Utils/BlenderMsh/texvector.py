# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Stock textures redrawn at a higher resolution from vectors, in their
# original layout (meshes and skins still fit):
#
#   extract  traces a stock texture into an SVG (coordinates: the
#            original's texels): panel seams (polylines: thin lines darker
#            than their surroundings, joined across gaps and straightened),
#            material regions (paths, one per material of a palette,
#            classified without the seams, a panel the seams enclose of one
#            material where its colours allow), and what the palette does
#            not describe (decals, lettering) as image patches. The SVG can
#            be edited (Inkscape, a text editor).
#   render   draws the SVG at the target resolution (antialiased): the
#            original's shading and dirt smoothed (where it shows markings,
#            the shading around them), seams as grooves of one width,
#            fasteners along them, optional chips of worn paint; markings
#            of an overlay drawn anew as flat paint.
#            Writes <out>.png (colour), <out>_norm.png, <out>_rghn.png
#            (smoothness), <out>_metal.png; ddsenc.py makes DDS of them.
#
#   out/assetvenv/bin/python Utils/BlenderMsh/texvector.py extract <palette.json> <stock.dds> <out.svg>
#   out/assetvenv/bin/python Utils/BlenderMsh/texvector.py render <palette.json> <stock.dds> <in.svg> <out stem>
#          [--size 2048] [--mesh <msh> --texname <name> [--matcfg <cfg> --meshname <name>]] [--overlay <json>]
#
# Palette (JSON): materials as for texrefine.py, each also with
#   "wear": colour under the paint where it is worn (e.g. primer),
#   "seam": brightness of the paint in its seams (default "seam_shade"),
#   "fasteners": true for materials whose panel lines have rivets;
# and settings: "line_contrast" (lightness, L*, of a seam below its
# surroundings), "line_sharpness", "join_gap", "line_width" (groove width,
# original texels), "straighten" (tolerance, texels), "fastener_spacing",
# "fastener_offset", "grime" (strength), "wear", "chip_size", "wear_reach",
# "seam_grime" (strength).
# Overlay (JSON): "symbols": trefoil, text, disc, polyline (arrow, dash),
# fill (a polygon given to a material where it has one of "from"); with
# "clear", the material painted under a symbol first.

import os, sys, re, json, math, argparse, base64, io
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from skimage import measure, morphology, draw
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import texrefine as tr

UP = 4   # (extraction works at four times the original's resolution)

# ----------------------------------------------------------------------
# extraction

def classify (pal, img, scale):
	"""material of each texel of the image enlarged by scale, and the
	confidence of it (as texrefine)"""
	h, w = img.shape[:2]
	big = np.asarray (Image.fromarray ((img * 255).astype (np.uint8)).resize ((w * scale, h * scale), Image.LANCZOS), np.float32) / 255.0
	smooth = ndimage.gaussian_filter (big, (scale * 0.35, scale * 0.35, 0))
	lab = tr.to_lab (smooth)
	dist = []
	for m in pal["materials"]:
		cl = tr.to_lab (np.array (m["colors"], np.float32) / 255.0)
		# (lightness counts half: dirt and soot darken a material, they do
		# not make it another one)
		d = (lab[:, :, None, :] - cl[None, None, :, :]) * np.array ([0.5, 1.0, 1.0])
		dist.append (np.min (np.linalg.norm (d, axis=-1), axis=-1))
	dist = np.stack (dist, -1)
	conf = np.clip (1.0 - (dist.min (-1) - 10.0) / 25.0, 0.0, 1.0)
	classify.dist = dist
	return dist.argmin (-1), conf, big

def lum (img):
	return (tr.srgb_to_lin (img) * np.array ([0.2126, 0.7152, 0.0722])).sum (-1)

def line_features (pal, img):
	"""thin features darker than their surroundings (panel seams, painted
	lines): how much darker (0..1, relative), and the image without them
	(each channel closed: the colour around them). Only elongated ones:
	lettering and small marks stay in the image."""
	size = 2 * pal.get ("line_max_width", 2) + 1
	closed = np.stack ([ndimage.grey_closing (img[..., c], size=(size, size)) for c in range (3)], -1)
	# (perceptual lightness: seams on red paint are as visible as on white)
	Lc, L = tr.to_lab (closed)[..., 0], tr.to_lab (img)[..., 0]
	rel = np.clip ((Lc - L) / pal.get ("line_contrast", 6.0), 0, 4) / 4
	# (hysteresis: faint stretches of a line count where they continue a
	# clear one: outlines broken by scuffs, or fading in the original)
	weak = rel > 0.25 * pal.get ("line_weak", 0.45)
	# (sharp ones only: soot streaks fade out, seams are cut in. Very dark
	# ones are painted marks: they stay in the image, a material or decal)
	soft = ndimage.gaussian_filter (L, 1.0)
	weak &= (soft - L) > pal.get ("line_sharpness", 0.25) * (Lc - L)
	weak &= L / np.maximum (Lc, 1.0) > pal.get ("mark_ratio", 0.35)
	lab_, n = ndimage.label (weak, structure=np.ones ((3, 3)))
	strong = np.zeros (n + 1, bool)
	strong[np.unique (lab_[weak & (rel > 0.25)])] = True
	strong[0] = False
	mask = strong[lab_]
	lab_, n = ndimage.label (mask, structure=np.ones ((3, 3)))
	if n:
		ext = np.zeros (n + 1)
		for k, sl in enumerate (ndimage.find_objects (lab_)):
			if sl: ext[k + 1] = math.hypot (sl[0].stop - sl[0].start, sl[1].stop - sl[1].start)
		mask &= ext[lab_] >= pal.get ("line_min_length", 5.0)
	rel = np.where (mask, np.maximum (rel, 0.3), 0.0)
	m = ndimage.binary_dilation (mask)
	clean = np.where (m[..., None], closed, img)
	return rel, clean

def trace_skeleton (sk):
	"""polylines (lists of (row, col)) along a one-pixel skeleton"""
	pts = set (zip (*np.nonzero (sk)))
	nb = lambda p: [(p[0] + dy, p[1] + dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy or dx) and (p[0] + dy, p[1] + dx) in pts]
	deg = {p: len (nb (p)) for p in pts}
	used = set ()   # (edges walked)
	lines = []
	def walk (a, b):
		line = [a, b]
		used.add ((a, b)); used.add ((b, a))
		prev, cur = a, b
		while deg[cur] == 2:
			nxt = [q for q in nb (cur) if q != prev and (cur, q) not in used]
			if not nxt: break
			prev, cur = cur, nxt[0]
			used.add ((prev, cur)); used.add ((cur, prev))
			line.append (cur)
		return line
	for p in [p for p in pts if deg[p] != 2]:
		for q in nb (p):
			if (p, q) not in used: lines.append (walk (p, q))
	for p in pts:   # (closed loops)
		for q in nb (p):
			if (p, q) not in used: lines.append (walk (p, q))
	return lines

def straighten (poly, tol):
	"""simplified, with nearly horizontal or vertical segments made exactly so"""
	p = measure.approximate_polygon (np.array (poly, float), tolerance=tol)
	for k in range (len (p) - 1):
		d = p[k + 1] - p[k]
		ang = math.degrees (math.atan2 (abs (d[0]), abs (d[1]))) if np.any (d) else 0
		if ang < 4.0: m = 0.5 * (p[k][0] + p[k + 1][0]); p[k][0] = p[k + 1][0] = m
		elif ang > 86.0: m = 0.5 * (p[k][1] + p[k + 1][1]); p[k][1] = p[k + 1][1] = m
	return p

def soft_regions_out (pal, img, label):
	"""regions of materials marked only_sharp whose edges are soft in the
	original (shading, soot: not parts) given to the material around them"""
	L = (tr.srgb_to_lin (img) * np.array ([0.2126, 0.7152, 0.0722])).sum (-1)
	gm = np.hypot (*np.gradient (ndimage.zoom (L, UP, order=1)))
	thr = pal.get ("sharp_edge", 0.02)
	for k, m in enumerate (pal["materials"]):
		if not m.get ("only_sharp"): continue
		comp, n = ndimage.label (label == k)
		for c in range (1, n + 1):
			r = comp == c
			edge = ndimage.binary_dilation (r, iterations=2) & ~ndimage.binary_erosion (r, iterations=2)
			if not edge.any () or gm[edge].mean () >= thr: continue
			ring = ndimage.binary_dilation (r, iterations=3) & ~r
			around = label[ring]
			around = around[around != k]
			if around.size: label[r] = np.bincount (around).argmax ()
	return label

def panel_vote (pal, label, conf, dist, seams):
	"""each panel (an area the seams enclose) of one material where its
	colours allow it: shading and dirt do not split a panel into two"""
	comp, n = ndimage.label (~ndimage.binary_dilation (seams))
	if not n: return label
	K = dist.shape[-1]
	votes = np.bincount ((comp * K + label).ravel (), weights=conf.ravel (), minlength=(n + 1) * K).reshape (n + 1, K)
	area = np.bincount (comp.ravel (), minlength=n + 1)
	maj = votes.argmax (1)
	share = votes.max (1) / np.maximum (votes.sum (1), 1e-6)
	ok = (share > pal.get ("vote_share", 0.6)) & (area < pal.get ("vote_area", 4000) * UP * UP)
	ok[0] = False
	m = maj[comp]
	near = np.take_along_axis (dist, m[..., None], -1)[..., 0] < dist.min (-1) + pal.get ("vote_tolerance", 15.0)
	return np.where (ok[comp] & near, m, label)

def join_lines (polys, gap, cos_min=0.8, long_gap=6.0, corner=3.0):
	"""polylines joined into one where their ends meet: within gap nearly
	in line, within long_gap exactly in line, or at a corner both point to
	(within corner of each); a line whose own ends meet so closed. The
	cheapest joins first, each end joined once."""
	from scipy.spatial import cKDTree
	polys = [np.asarray (p, float) for p in polys if len (p) >= 2]
	n = len (polys)
	if not n: return polys
	# (end e: polyline e % n, its start for e < n, its end otherwise)
	P = np.array ([p[0] for p in polys] + [p[-1] for p in polys])
	length = np.array ([np.linalg.norm (np.diff (p, axis=0), axis=1).sum () for p in polys])
	def out_dir (e):
		p = polys[e % n]
		d = p[-1] - p[-2] if e >= n else p[0] - p[1]
		l = np.linalg.norm (d)
		return d / l if l > 1e-9 else d
	D = np.array ([out_dir (e) for e in range (2 * n)])
	cand = []
	for a, b in cKDTree (P).query_pairs (max (long_gap, 2 * corner)):
		if a % n == b % n and length[a % n] < 6 * gap: continue
		da, db = D[a], D[b]
		g = P[b] - P[a]; gl = np.linalg.norm (g)
		gu = g / gl if gl > 1e-6 else da
		cont = -np.dot (da, db)   # (continuing each other)
		if gl <= gap and cont >= cos_min and (gl <= 0.3 or (np.dot (gu, da) >= cos_min and -np.dot (gu, db) >= cos_min)):
			cand.append ((gl, a, b, [])); continue
		if gl <= long_gap and cont >= 0.97 and np.dot (gu, da) >= 0.97 and -np.dot (gu, db) >= 0.97:
			cand.append ((1.5 * gl, a, b, [])); continue
		if abs (da[0] * db[1] - da[1] * db[0]) > 0.5:
			# (corner: P[a] + ta da = P[b] + tb db)
			ta, tb = np.linalg.solve (np.array ([[da[0], -db[0]], [da[1], -db[1]]]), g)
			if 0 <= ta <= corner and 0 <= tb <= corner:
				cand.append ((ta + tb + 1.0, a, b, [P[a] + ta * da]))
	link = {}   # (end: the end it is joined to, the points between)
	for _, a, b, br in sorted (cand, key=lambda c: c[0]):
		if a in link or b in link: continue
		link[a] = (b, br); link[b] = (a, br[::-1])
	other = lambda e: e + n if e < n else e - n
	oriented = lambda e: polys[e % n] if e < n else polys[e % n][::-1]   # (entered at end e)
	seen = np.zeros (n, bool)
	out = []
	for i in range (n):
		if seen[i]: continue
		e = i   # (back to the head of the chain, or round a loop)
		while e in link:
			prev = other (link[e][0])
			if prev % n == i: e = i; break
			e = prev
		start = e
		chain = [oriented (e)]
		seen[e % n] = True
		while other (e) in link:
			nxt, br = link[other (e)]
			if nxt == start: chain += [np.array (br).reshape (-1, 2), chain[0][:1]]; break   # (a loop: closed)
			if seen[nxt % n]: break
			chain += [np.array (br).reshape (-1, 2), oriented (nxt)]
			seen[nxt % n] = True
			e = nxt
		out.append (np.concatenate (chain))
	return out

def turning (p):
	"""total turning of a polyline (radians), and its length"""
	q = measure.approximate_polygon (np.asarray (p), 0.5)
	d = np.diff (q, axis=0)
	L = np.linalg.norm (d, axis=1).sum ()
	if len (d) < 2: return 0.0, L
	a = np.arctan2 (d[:, 1], d[:, 0])
	t = np.diff (a); t = (t + np.pi) % (2 * np.pi) - np.pi
	return np.abs (t).sum (), L

def min_rect (pts):
	"""the smallest rotated rectangle round the points: centre, axes
	(unit), half sizes"""
	from scipy.spatial import ConvexHull
	try: h = pts[ConvexHull (pts).vertices]
	except Exception: return None
	best = None
	for a, b in zip (h, np.roll (h, -1, 0)):
		u = b - a; l = np.linalg.norm (u)
		if l < 1e-9: continue
		u /= l; v = np.array ([-u[1], u[0]])
		pu, pv = h @ u, h @ v
		area = (pu.max () - pu.min ()) * (pv.max () - pv.min ())
		if best is None or area < best[0]:
			best = (area, u, v, pu.min (), pu.max (), pv.min (), pv.max ())
	if best is None: return None
	_, u, v, u0, u1, v0, v1 = best
	return u * (u0 + u1) / 2 + v * (v0 + v1) / 2, u, v, (u1 - u0) / 2, (v1 - v0) / 2

def rounded_rect (c, u, v, hu, hv, r, step=0.5):
	"""outline (closed polyline) of a rounded rectangle"""
	r = min (r, 0.45 * min (hu, hv))
	pts = []
	for cu, cv, a0 in ((1, 1, 0), (-1, 1, 90), (-1, -1, 180), (1, -1, 270)):
		k = c + u * cu * (hu - r) + v * cv * (hv - r)
		n = max (2, int (math.pi / 2 * r / step))
		for t in np.linspace (a0, a0 + 90, n):
			pts.append (k + r * (u * math.cos (math.radians (t)) + v * math.sin (math.radians (t))))
	pts.append (pts[0])
	return np.array (pts)

def _sample (p, step=0.25):
	out = [p[:1]]
	for a, b in zip (p[:-1], p[1:]):
		m = max (1, int (math.ceil (np.linalg.norm (b - a) / step)))
		out.append (a + (b - a) * (np.arange (1, m + 1) / m)[:, None])
	return np.concatenate (out)

def fit_panels (polys, radius, tol=1.0, cover=0.6, min_side=3.0):
	"""outlines that are most of a rounded rectangle (panels, hatches) made
	that rectangle, whole; the fragments of it the original shows gone"""
	from scipy.spatial import cKDTree
	samples = [_sample (p) for p in polys]
	rects, used = [], set ()
	order = sorted (range (len (polys)), key=lambda k: -len (samples[k]))
	for k in order:
		if k in used or len (samples[k]) < 40: continue
		mr = min_rect (samples[k])
		if mr is None: continue
		c, u, v, hu, hv = mr
		if min (hu, hv) * 2 < min_side: continue
		rr = rounded_rect (c, u, v, hu, hv, radius)
		rs = _sample (rr)
		tree_r = cKDTree (rs)
		# (the outline on the rectangle, nearly all of it)
		if (tree_r.query (samples[k])[0] < tol).mean () < 0.9: continue
		# (other fragments on it too)
		members = [k] + [j for j in range (len (polys)) if j != k and j not in used and (tree_r.query (samples[j])[0] < tol).mean () > 0.9]
		have = cKDTree (np.concatenate ([samples[j] for j in members]))
		if (have.query (rs)[0] < tol).mean () < cover: continue
		rects.append (rr); used.update (members)
	return [p for k, p in enumerate (polys) if k not in used], rects

def close_outlines (polys, reach, ratio=0.4):
	"""open outlines that nearly come round (ends closer than ratio of their
	length, and than reach) closed: at the corner both ends point to, or
	straight across where they face each other"""
	out = []
	for p in polys:
		L = np.linalg.norm (np.diff (p, axis=0), axis=1).sum ()
		g = p[-1] - p[0]; gl = np.linalg.norm (g)
		tt, _ = turning (p)
		# (smooth outlines only: about once round, gently)
		if gl < 0.05 or gl > reach or gl > ratio * L or L < 3 * reach / 2 or tt > 1.4 * 2 * np.pi or tt / L > 0.15:
			out.append (p); continue
		d0 = p[0] - p[1]; d0 /= np.linalg.norm (d0) or 1
		d1 = p[-1] - p[-2]; d1 /= np.linalg.norm (d1) or 1
		closed = None
		if abs (d1[0] * d0[1] - d1[1] * d0[0]) > 0.5:
			t1, t0 = np.linalg.solve (np.array ([[d1[0], -d0[0]], [d1[1], -d0[1]]]), p[0] - p[-1])
			if 0 <= t1 <= reach and 0 <= t0 <= reach: closed = np.vstack ([p, p[-1] + t1 * d1, p[:1]])
		if closed is None and np.dot (d1, -g / gl) > 0.5 and np.dot (d0, g / gl) > 0.5:
			closed = np.vstack ([p, p[:1]])
		out.append (closed if closed is not None else p)
	return out

def snap_ends (polys, gap):
	"""free ends near another line (T junctions, corners left open) carried
	on to it"""
	from scipy.spatial import cKDTree
	pts, ids, arc, total = [], [], [], []
	for k, p in enumerate (polys):
		s = 0.0
		for a, b in zip (p[:-1], p[1:]):
			l = np.linalg.norm (b - a)
			m = max (1, int (math.ceil (l / 0.2)))
			for t in np.arange (m) / m:
				pts.append (a + (b - a) * t); ids.append (k); arc.append (s + l * t)
			s += l
		pts.append (p[-1]); ids.append (k); arc.append (s); total.append (s)
	if not pts: return polys
	pts = np.array (pts); ids = np.array (ids); arc = np.array (arc)
	tree = cKDTree (pts)
	out = []
	for k, p in enumerate (polys):
		p = list (p)
		for at_end in (False, True):
			q = np.asarray (p[-1] if at_end else p[0])
			d, j = tree.query (q, k=16, distance_upper_bound=gap)
			best = None
			for dd, jj in zip (np.atleast_1d (d), np.atleast_1d (j)):
				if not np.isfinite (dd) or dd < 0.05: continue
				if ids[jj] == k and abs (arc[jj] - (total[k] if at_end else 0.0)) < 4 * gap: continue   # (its own line: only far along it)
				best = pts[jj]; break
			if best is not None:
				if at_end: p.append (best)
				else: p.insert (0, best)
		out.append (np.array (p))
	return out

def extract (pal, src, out_svg):
	img = np.asarray (Image.open (src).convert ("RGB"), np.float32) / 255.0
	h, w = img.shape[:2]
	# (lines first: the materials are classified without them)
	rel, clean = line_features (pal, img)
	label, conf, big = classify (pal, clean, UP)
	big = np.asarray (Image.fromarray ((img * 255).astype (np.uint8)).resize ((w * UP, h * UP), Image.LANCZOS), np.float32) / 255.0
	mats = pal["materials"]
	label = panel_vote (pal, label, conf, classify.dist, ndimage.zoom (rel, UP, order=1) > 0.2)
	label = soft_regions_out (pal, clean, label)
	svg = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h)]
	svg.append ('<!-- traced from %s by texvector.py; coordinates in its texels -->' % os.path.basename (src))
	# material regions: a path per material (even-odd: holes are rings)
	svg.append ('<g id="regions">')
	tol = pal.get ("region_tolerance", 0.3)
	for k, m in enumerate (mats):
		mask = np.pad ((label == k).astype (float), 1)
		if m.get ("pattern"):   # (the pattern's stripes are inside the region)
			mask = ndimage.binary_closing (mask > 0.5, iterations=int (UP * m["pattern"].get ("period", 4) * 0.6)).astype (float)
		mask = ndimage.binary_opening (mask > 0.5, iterations=1).astype (float)   # (specks out)
		rings = []
		for c in measure.find_contours (mask, 0.5):
			c = (c - 1) / UP
			if len (c) < 4: continue
			area = 0.5 * abs (np.dot (c[:, 1], np.roll (c[:, 0], 1)) - np.dot (c[:, 0], np.roll (c[:, 1], 1)))
			if area < 1.5: continue
			c = measure.approximate_polygon (c, tolerance=tol)
			rings.append ("M" + " L".join ("%.2f,%.2f" % (x, y) for y, x in c[:-1]) + " Z")
		col = "#%02x%02x%02x" % tuple (m["colors"][0])
		svg.append ('<path class="material" data-material="%s" fill="%s" fill-rule="evenodd" d="%s"/>' % (m["name"], col, " ".join (rings)))
	svg.append ('</g>')
	# panel seams: the skeleton of the line features at four times the
	# resolution, traced (spurs off), joined across gaps, carried on to the
	# lines they end at, straightened
	relb = ndimage.zoom (rel, UP, order=1)
	sk = morphology.skeletonize (relb > 0.2)
	deg = ndimage.convolve (sk.astype (int), np.ones ((3, 3), int), mode="constant") - 1
	polys = []
	for poly in trace_skeleton (sk):
		if len (poly) < 2: continue
		d0, d1 = deg[poly[0]], deg[poly[-1]]
		if (d0 == 1) != (d1 == 1) and len (poly) < pal.get ("spur", 1.5) * UP: continue   # (spur off a junction)
		polys.append (np.array ([(x / UP, y / UP) for y, x in poly]))   # ((x, y) from here on)
	# (no scribbles: soot, details of decals; and nothing inside the decals,
	# which show their own lines)
	unk = ndimage.binary_dilation (conf < 0.5, iterations=UP)
	def clean_line (p, limit):
		tt, L = turning (p)
		iy, ix = np.clip ((p[:, 1] * UP).astype (int), 0, unk.shape[0] - 1), np.clip ((p[:, 0] * UP).astype (int), 0, unk.shape[1] - 1)
		return tt / max (L, 1e-6) < limit and unk[iy, ix].mean () < 0.5
	polys = [p for p in polys if clean_line (p, pal.get ("scribble", 0.6))]
	gap = pal.get ("join_gap", 2.0)
	polys = join_lines ([measure.approximate_polygon (p, 0.15) for p in polys], gap, long_gap=pal.get ("join_long", 6.0), corner=pal.get ("join_corner", 3.0))
	polys = snap_ends (polys, gap)
	polys = [p for p in polys if clean_line (p, pal.get ("scribble", 0.6) * 0.5)]
	polys = close_outlines (polys, pal.get ("close_reach", 20.0))
	polys, panels = fit_panels (polys, pal.get ("panel_radius", 2.0), cover=pal.get ("panel_cover", 0.6))
	svg.append ('<g id="lines" stroke="#000" fill="none" stroke-width="%g">' % pal.get ("line_width", 0.5))
	n = 0
	for p in polys:
		p = straighten (p[:, ::-1], pal.get ("straighten", 0.6))[:, ::-1]
		if np.linalg.norm (np.diff (p, axis=0), axis=1).sum () < pal.get ("line_min_length", 5.0): continue
		svg.append ('<polyline points="%s"/>' % " ".join ("%.2f,%.2f" % (x, y) for x, y in p))
		n += 1
	for p in panels:   # (fitted: as they are)
		svg.append ('<polyline class="panel" points="%s"/>' % " ".join ("%.2f,%.2f" % (x, y) for x, y in p))
		n += 1
	svg.append ('</g>')
	# what no material describes: decals, lettering (image patches, from
	# the enlarged original; each a candidate for redrawing)
	svg.append ('<g id="decals">')
	unk = ndimage.binary_dilation (conf < 0.5, iterations=UP)
	lab_, nd = ndimage.label (unk)
	nd_kept = 0
	for k, sl in enumerate (ndimage.find_objects (lab_)):
		if sl is None: continue
		ys, xs = sl
		if (ys.stop - ys.start) * (xs.stop - xs.start) < (2 * UP) ** 2: continue
		m = (lab_[sl] == k + 1)
		a = ndimage.gaussian_filter (m.astype (float), UP * 0.5)
		patch = np.concatenate ([big[sl], np.clip (a * 1.5, 0, 1)[..., None]], -1)
		buf = io.BytesIO ()
		Image.fromarray ((patch * 255 + 0.5).astype (np.uint8), "RGBA").save (buf, "PNG")
		svg.append ('<image x="%.2f" y="%.2f" width="%.2f" height="%.2f" xlink:href="data:image/png;base64,%s"/>' % (
			xs.start / UP, ys.start / UP, (xs.stop - xs.start) / UP, (ys.stop - ys.start) / UP, base64.b64encode (buf.getvalue ()).decode ()))
		nd_kept += 1
	svg.append ('</g>')
	svg.append ('</svg>')
	open (out_svg, "w").write ("\n".join (svg) + "\n")
	print ("%s: %d materials, %d panel lines, %d decals" % (out_svg, len (mats), n, nd_kept))

# ----------------------------------------------------------------------
# reading the SVG (as written above, or edited: paths with absolute or
# relative moves, lines and curves)

def parse_path (d):
	"""rings (lists of (x, y)) of an SVG path"""
	toks = re.findall (r"[MmLlHhVvCcZzQqSsTt]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d)
	rings, ring, cmd, i = [], [], None, 0
	cx = cy = sx = sy = 0.0
	def num ():
		nonlocal i
		v = float (toks[i]); i += 1; return v
	while i < len (toks):
		t = toks[i]
		if t.isalpha (): cmd = t; i += 1
		if cmd in "Zz":
			if ring: rings.append (ring)
			ring = []; cx, cy = sx, sy
			continue
		rel = cmd.islower ()
		c = cmd.upper ()
		if c == "M":
			x, y = num (), num ()
			if rel: x += cx; y += cy
			if ring: rings.append (ring)
			ring = [(x, y)]; cx, cy = sx, sy = x, y
			cmd = "l" if rel else "L"
		elif c == "L":
			x, y = num (), num ()
			if rel: x += cx; y += cy
			ring.append ((x, y)); cx, cy = x, y
		elif c == "H":
			x = num () + (cx if rel else 0); ring.append ((x, cy)); cx = x
		elif c == "V":
			y = num () + (cy if rel else 0); ring.append ((cx, y)); cy = y
		elif c in "CSQT":
			n = {"C": 3, "S": 2, "Q": 2, "T": 1}[c]
			pts = [(num (), num ()) for _ in range (n)]
			if rel: pts = [(x + cx, y + cy) for x, y in pts]
			p0 = (cx, cy)
			for s in range (1, 9):   # (sampled; the control points as given)
				u = s / 8
				if n == 3:
					x = (1 - u) ** 3 * p0[0] + 3 * (1 - u) ** 2 * u * pts[0][0] + 3 * (1 - u) * u * u * pts[1][0] + u ** 3 * pts[2][0]
					y = (1 - u) ** 3 * p0[1] + 3 * (1 - u) ** 2 * u * pts[0][1] + 3 * (1 - u) * u * u * pts[1][1] + u ** 3 * pts[2][1]
				elif n == 2:
					x = (1 - u) ** 2 * p0[0] + 2 * (1 - u) * u * pts[0][0] + u * u * pts[1][0]
					y = (1 - u) ** 2 * p0[1] + 2 * (1 - u) * u * pts[0][1] + u * u * pts[1][1]
				else:
					x, y = p0[0] + u * (pts[0][0] - p0[0]), p0[1] + u * (pts[0][1] - p0[1])
				ring.append ((x, y))
			cx, cy = pts[-1]
		else:
			i += 1
	if ring: rings.append (ring)
	return rings

def read_svg (path):
	import xml.etree.ElementTree as ET
	root = ET.parse (path).getroot ()
	ns = "{http://www.w3.org/2000/svg}"
	out = {"regions": [], "lines": [], "decals": [], "w": float (root.get ("viewBox").split ()[2]), "h": float (root.get ("viewBox").split ()[3])}
	def groups (gid):
		return [g for g in root.iter (ns + "g") if g.get ("id") == gid]
	for g in groups ("regions"):
		for p in g.iter (ns + "path"):
			out["regions"].append ((p.get ("data-material"), parse_path (p.get ("d", ""))))
	for g in groups ("lines"):
		gw = float (g.get ("stroke-width", "0.5"))
		for p in g:
			w = float (p.get ("stroke-width", gw))
			if p.tag == ns + "polyline" or p.tag == ns + "line":
				if p.tag == ns + "line":
					pts = [(float (p.get ("x1")), float (p.get ("y1"))), (float (p.get ("x2")), float (p.get ("y2")))]
				else:
					v = [float (x) for x in re.findall (r"[-+]?(?:\d+\.?\d*|\.\d+)", p.get ("points"))]
					pts = list (zip (v[0::2], v[1::2]))
				out["lines"].append ((pts, w))
			elif p.tag == ns + "path":
				for r in parse_path (p.get ("d", "")): out["lines"].append ((r, w))
	for g in groups ("decals"):
		for im in g.iter (ns + "image"):
			href = im.get ("{http://www.w3.org/1999/xlink}href") or im.get ("href")
			data = Image.open (io.BytesIO (base64.b64decode (href.split (",", 1)[1]))) if href.startswith ("data:") else Image.open (os.path.join (os.path.dirname (path), href))
			out["decals"].append ((float (im.get ("x")), float (im.get ("y")), float (im.get ("width")), float (im.get ("height")), data.convert ("RGBA")))
	return out

# ----------------------------------------------------------------------
# rendering

def fill_rings (rings, W, H, s, ss=2):
	"""antialiased even-odd coverage of the rings (coordinates times s)"""
	acc = np.zeros ((H * ss, W * ss), bool)
	for r in rings:
		if len (r) < 3: continue
		r = np.array (r) * s * ss
		rr, cc = draw.polygon (r[:, 1], r[:, 0], acc.shape)
		m = np.zeros_like (acc); m[rr, cc] = True
		acc ^= m
	return acc.reshape (H, ss, W, ss).mean ((1, 3))

def stroke_lines (lines, W, H, s, ss=4):
	"""antialiased coverage of the polylines"""
	im = Image.new ("L", (W * ss, H * ss), 0)
	d = ImageDraw.Draw (im)
	for pts, w in lines:
		if len (pts) < 2: continue
		q = [(x * s * ss, y * s * ss) for x, y in pts]
		lw = max (1, int (round (w * s * ss)))
		d.line (q, fill=255, width=lw, joint="curve")
		for x, y in (q[0], q[-1]):   # (round caps)
			d.ellipse ((x - lw / 2, y - lw / 2, x + lw / 2, y + lw / 2), fill=255)
	a = np.asarray (im, np.float32) / 255.0
	return a.reshape (H, ss, W, ss).mean ((1, 3))

def fasteners (lines, mask_of, W, H, s, spacing, offset, radius):
	"""raised dots along both sides of panel lines (where mask_of says so)"""
	hgt = np.zeros ((H, W), np.float32)
	r = radius * s
	k = max (1, int (math.ceil (r * 2.5)))
	yy, xx = np.mgrid[-k:k + 1, -k:k + 1]
	for pts, w in lines:
		for (x0, y0), (x1, y1) in zip (pts[:-1], pts[1:]):
			L = math.hypot (x1 - x0, y1 - y0)
			if L < spacing: continue
			ux, uy = (x1 - x0) / L, (y1 - y0) / L
			for t in np.arange (spacing * 0.5, L, spacing):
				for side in (-1, 1):
					px, py = (x0 + ux * t - uy * offset * side) * s, (y0 + uy * t + ux * offset * side) * s
					ix, iy = int (round (px)), int (round (py))
					if not (k <= ix < W - k and k <= iy < H - k) or not mask_of (ix, iy): continue
					dot = np.exp (-((xx + ix - px) ** 2 + (yy + iy - py) ** 2) / (r * r))
					hgt[iy - k:iy + k + 1, ix - k:ix + k + 1] = np.maximum (hgt[iy - k:iy + k + 1, ix - k:ix + k + 1], dot)
	return hgt

def chips (pal, edist, cluster, W, H, s):
	"""worn paint: chips with straight, crisp edges (Voronoi cells), more of
	them nearer an edge (edist: original texels to the nearest seam or
	material border), clustered (by the noise given). Returns the cover of
	the chips and of their bare-metal cores."""
	from scipy.spatial import cKDTree
	c = pal.get ("chip_size", 0.6) * s   # (cell size, target pixels)
	rng = np.random.default_rng (7)
	gy, gx = np.mgrid[0:H:c, 0:W:c]
	sites = np.stack ([gy.ravel (), gx.ravel ()], -1) + rng.random ((gy.size, 2)) * c
	iy, ix = np.clip (sites[:, 0].astype (int), 0, H - 1), np.clip (sites[:, 1].astype (int), 0, W - 1)
	reach = pal.get ("wear_reach", 0.8)
	# (worn in patches: where the clustering noise is high, none elsewhere)
	p = pal.get ("wear", 0.3) * np.exp (-edist[iy, ix] / reach) * np.clip ((cluster[iy, ix] - 0.15) * 4, 0, 2)
	active = (rng.random (len (sites)) < p) & (edist[iy, ix] < 3 * reach)
	core = rng.random (len (sites)) < 0.35
	act = np.flatnonzero (active)
	wear = np.zeros ((H, W), np.float32); bare = np.zeros ((H, W), np.float32)
	if not len (act): return wear, bare
	# (only around active cells: the nearest two sites of each pixel there)
	near = np.zeros ((H, W), bool)
	r = int (math.ceil (c * 1.5))
	for y, x in sites[act].astype (int):
		near[max (0, y - r):y + r + 1, max (0, x - r):x + r + 1] = True
	py, px = np.nonzero (near)
	q = np.stack ([py + 0.5, px + 0.5], -1)
	d, j = cKDTree (sites).query (q, k=2)
	s1, s2 = sites[j[:, 0]], sites[j[:, 1]]
	# (distance to the border of the two cells, in pixels: antialiased)
	db = (d[:, 1] ** 2 - d[:, 0] ** 2) / (2 * np.maximum (np.linalg.norm (s2 - s1, axis=1), 1e-6))
	a1, a2 = active[j[:, 0]], active[j[:, 1]]
	cov = np.where (a1, np.where (a2, 1.0, np.clip (db + 0.5, 0, 1)), np.where (a2, np.clip (0.5 - db, 0, 1), 0.0))
	wear[py, px] = cov
	bare[py, px] = np.where (a1 & core[j[:, 0]], np.clip ((0.35 * c - d[:, 0]) + 0.5, 0, 1), 0.0)
	return wear, bare

FONT_DIR = "/usr/share/fonts/truetype/dejavu"

def draw_symbol (sym, W, H, s, ss=4):
	"""an overlay symbol (premultiplied RGBA at the target resolution): its
	rectangle and colours in the original's texels"""
	from PIL import ImageFont
	x, y, w, h = sym["rect"] if "rect" in sym else (0, 0, 0, 0)
	pad = 2
	if sym["type"] == "polyline":
		xs = [p[0] for p in sym["points"]]; ys = [p[1] for p in sym["points"]]
		x, y = min (xs) - 3, min (ys) - 3; w, h = max (xs) - x + 3, max (ys) - y + 3
	X0, Y0 = int (math.floor (x * s)) - pad, int (math.floor (y * s)) - pad
	X1, Y1 = int (math.ceil ((x + w) * s)) + pad, int (math.ceil ((y + h) * s)) + pad
	pw, ph = (X1 - X0) * ss, (Y1 - Y0) * ss
	im = Image.new ("RGBA", (pw, ph), (0, 0, 0, 0))
	d = ImageDraw.Draw (im)
	k = s * ss
	px = lambda u, v: ((u * s - X0) * ss, (v * s - Y0) * ss)
	col = lambda c: tuple (int (v) for v in c) + (255,)
	if sym.get ("bg"):
		r = sym.get ("radius", 0.0) * k
		d.rounded_rectangle ([px (x, y), px (x + w, y + h)], radius=r, fill=col (sym["bg"]))
	if sym["type"] == "trefoil":
		# (the radiation symbol: a disc of radius r, three blades 60 degrees
		# wide from 1.5 r to 5 r, one pointing down)
		cx, cy = px (x + w / 2, y + h / 2)
		R = min (w, h) * k * sym.get ("scale", 0.46)
		r = R / 5.0
		d.ellipse ([cx - r, cy - r, cx + r, cy + r], fill=col (sym["fg"]))
		for a0 in (90, 210, 330):   # (image y down: 90 is down)
			d.pieslice ([cx - R, cy - R, cx + R, cy + R], a0 - 30, a0 + 30, fill=col (sym["fg"]))
		ri = 1.5 * r
		d.ellipse ([cx - ri, cy - ri, cx + ri, cy + ri], fill=col (sym["bg"]))
		d.ellipse ([cx - r, cy - r, cx + r, cy + r], fill=col (sym["fg"]))
	elif sym["type"] == "disc":
		d.ellipse ([px (x, y), px (x + w, y + h)], fill=col (sym["fg"]))
		if sym.get ("ring"):
			d.ellipse ([px (x, y), px (x + w, y + h)], outline=col (sym["ring"]), width=max (1, int (sym.get ("ring_width", 0.4) * k)))
	elif sym["type"] == "text":
		font = ImageFont.truetype (os.path.join (FONT_DIR, sym.get ("font", "DejaVuSansCondensed-Bold.ttf")), 200)
		t = Image.new ("L", (4000, 400), 0)
		ImageDraw.Draw (t).text ((20, 20), sym["text"], font=font, fill=255)
		t = t.crop (t.getbbox ())
		rot = sym.get ("rotate", 0)
		if rot: t = t.rotate (rot, expand=True)
		fill = sym.get ("fill", 0.8)
		bw, bh = w * k * fill, h * k * fill
		f = min (bw / t.width, bh / t.height) if sym.get ("keep_aspect", True) else None
		tw, th = (int (t.width * f), int (t.height * f)) if f else (int (bw), int (bh))
		t = t.resize ((max (1, tw), max (1, th)), Image.LANCZOS)
		cx, cy = px (x + w / 2, y + h / 2)
		layer = Image.new ("RGBA", t.size, col (sym["fg"]))
		im.paste (layer, (int (cx - tw / 2), int (cy - th / 2)), t)
	elif sym["type"] == "polyline":
		pts = [px (u, v) for u, v in sym["points"]]
		lw = max (1, int (round (sym.get ("width", 1.0) * k)))
		if sym.get ("dash"):
			# (dashes of the given lengths along the line, original texels)
			on, off = sym["dash"][0] * k, sym["dash"][1] * k
			t, pen = sym.get ("dash_offset", 0.0) * k, True
			for (ax, ay), (bx, by) in zip (pts[:-1], pts[1:]):
				L = math.hypot (bx - ax, by - ay)
				u = 0.0
				while u < L:
					step = min ((on if pen else off) - t, L - u)
					if pen:
						p0 = (ax + (bx - ax) * u / L, ay + (by - ay) * u / L)
						p1 = (ax + (bx - ax) * (u + step) / L, ay + (by - ay) * (u + step) / L)
						d.line ([p0, p1], fill=col (sym["fg"]), width=lw)
					u += step; t += step
					if t >= (on if pen else off) - 1e-6: t, pen = 0.0, not pen
		else:
			d.line (pts, fill=col (sym["fg"]), width=lw, joint="curve")
		if sym.get ("arrow"):
			(ax, ay), (bx, by) = pts[-2], pts[-1]
			L = math.hypot (bx - ax, by - ay) or 1
			ux, uy = (bx - ax) / L, (by - ay) / L
			hl = sym.get ("head", 2.5) * k
			tip = (bx + ux * hl * 0.6, by + uy * hl * 0.6)
			d.polygon ([tip, (bx - ux * hl * 0.4 - uy * hl * 0.55, by - uy * hl * 0.4 + ux * hl * 0.55),
				(bx - ux * hl * 0.4 + uy * hl * 0.55, by - uy * hl * 0.4 - ux * hl * 0.55)], fill=col (sym["fg"]))
	a = np.asarray (im, np.float32) / 255.0
	a = a.reshape (ph // ss, ss, pw // ss, ss, 4).mean ((1, 3))
	return X0, Y0, a

def _overlaps (r, q):
	return r[0] < q[0] + q[2] and q[0] < r[0] + r[2] and r[1] < q[1] + q[3] and q[1] < r[1] + r[3]

def render (pal, src, svg_path, size, matmaps=None, overlay=None):
	sv = read_svg (svg_path)
	ov = json.load (open (overlay)) if overlay else {"symbols": [], "suppress": []}
	cover = [sym["rect"] for sym in ov["symbols"] if "rect" in sym] + ov.get ("suppress", [])
	for sym in ov["symbols"]:
		if sym["type"] == "polyline":
			xs = [p[0] for p in sym["points"]]; ys = [p[1] for p in sym["points"]]
			cover.append ([min (xs) - 1, min (ys) - 1, max (xs) - min (xs) + 2, max (ys) - min (ys) + 2])
	sv["decals"] = [dc for dc in sv["decals"] if not any (_overlaps (dc[:4], c) for c in cover)]
	# (seams traced from what the symbols replace: gone with it)
	def inside (x, y): return any (c[0] <= x <= c[0] + c[2] and c[1] <= y <= c[1] + c[3] for c in cover)
	def covered (pts):
		smp = [(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t) for (x0, y0), (x1, y1) in zip (pts[:-1], pts[1:]) for t in (0.0, 0.25, 0.5, 0.75)]
		return sum (inside (x, y) for x, y in smp) > 0.3 * len (smp)
	sv["lines"] = [l for l in sv["lines"] if not covered (l[0])]
	img = np.asarray (Image.open (src).convert ("RGB"), np.float32) / 255.0
	h, w = img.shape[:2]
	W, H = size, int (round (size * h / w))
	s = W / sv["w"]
	mats = pal["materials"]
	names = [m["name"] for m in mats]
	# regions: each material's coverage, drawn in palette order
	cov = np.zeros ((H, W, len (mats)), np.float32)
	for name, rings in sv["regions"]:
		if name in names: cov[..., names.index (name)] = np.maximum (cov[..., names.index (name)], fill_rings (rings, W, H, s))
	# (painted in order, materials with "z" over the others: the later
	# covers the earlier)
	order = sorted (range (len (mats)), key=lambda k: (mats[k].get ("z", 0), k))
	weights = np.zeros_like (cov)
	free = np.ones ((H, W), np.float32)
	for k in reversed (order):
		weights[..., k] = cov[..., k] * free
		free = free * (1.0 - cov[..., k])
	weights[..., order[0]] += np.clip (free, 0, 1)   # (no region: the first material)
	# (overlay symbols with "clear": their rectangle repainted with a
	# material first, e.g. traced remains of the lettering they replace)
	symbox = np.zeros ((H, W), bool)
	for sym in ov["symbols"]:
		if "rect" not in sym: continue
		x, y, w_, h_ = sym["rect"]
		X0, Y0, X1, Y1 = int (x * s), int (y * s), int (math.ceil ((x + w_) * s)), int (math.ceil ((y + h_) * s))
		symbox[max (0, Y0 - int (s)):Y1 + int (s), max (0, X0 - int (s)):X1 + int (s)] = True
		if sym.get ("clear") in names:
			m_ = int (round (sym.get ("clear_margin", 0) * s))
			sl = (slice (max (0, Y0 - m_), Y1 + m_), slice (max (0, X0 - m_), X1 + m_))
			weights[sl] = 0.0
			weights[sl + (names.index (sym["clear"]),)] = 1.0
	# (region fixes: a polygon given to a material, where it has one of the
	# materials listed: shading the tracing took for another material)
	for sym in ov["symbols"]:
		if sym["type"] != "fill": continue
		r = fill_rings ([sym["points"]], W, H, s)[..., None]
		src_k = [names.index (n) for n in sym.get ("from", names)]
		k = names.index (sym["material"])
		moved = (weights[..., src_k].sum (-1, keepdims=True)) * r
		for j in src_k: weights[..., j:j + 1] *= 1 - r
		weights[..., k:k + 1] += moved
	ov["symbols"] = [sym for sym in ov["symbols"] if sym["type"] != "fill"]
	for sym in ov["symbols"]:
		if sym["type"] == "polyline" and sym.get ("clear") in names:
			r = stroke_lines ([(sym["points"], max (sym.get ("width", 1.0), sym.get ("head", 0) * 1.1) + 2 * sym.get ("clear_margin", 0.5))], W, H, s)[..., None]
			k = names.index (sym["clear"])
			weights *= 1 - r; weights[..., k] += r[..., 0]
			symbox |= r[..., 0] > 0
	label = weights.argmax (-1)
	base = np.array ([np.array (m["colors"][0], np.float32) / 255.0 for m in mats])
	wear_col = np.array ([np.array (m.get ("wear", m["colors"][0]), np.float32) / 255.0 for m in mats])
	alb = tr.srgb_to_lin (np.einsum ("hwk,kc->hwc", weights, base))
	# patterns: stripes of a second colour (hazard markings)
	yy, xx = np.mgrid[0:H, 0:W].astype (np.float32) / s
	for k, m in enumerate (mats):
		pt = m.get ("pattern")
		if not pt or not weights[..., k].any (): continue
		a = math.radians (pt.get ("angle", 45))
		ph = (xx * math.cos (a) + yy * math.sin (a)) / pt.get ("period", 4.0)
		f = ph - np.floor (ph)
		duty = pt.get ("duty", 0.45)
		aa = 1.5 / (pt.get ("period", 4.0) * s)   # (antialiased edges)
		stripe = np.clip ((duty - f) / aa + 0.5, 0, 1) * np.clip (f / aa + 0.5, 0, 1)
		stripe[symbox] = 0.0   # (not around overlay symbols: their own colours)
		c = tr.srgb_to_lin (np.array (pt["color"], np.float32) / 255.0)
		wk = (weights[..., k] * stripe)[..., None]
		alb = alb * (1 - wk) + c * wk
	wear_lin = tr.srgb_to_lin (np.einsum ("hwk,kc->hwc", weights, wear_col))
	# shading: the original's brightness relative to its material's colour
	# (dirt, soot, baked shading), without its panel lines; where the
	# original shows something else (decals, markings redrawn as symbols)
	# the shading around it carried in. Smoothed over about a texel: the
	# original's blockiness and compression noise gone.
	_, clean = line_features (pal, img)
	_, conf0, _ = classify (pal, clean, 1)
	lab0 = np.asarray (Image.fromarray (label.astype (np.uint8)).resize ((w, h), Image.NEAREST))
	ratio0 = np.clip (lum (clean) / np.maximum (lum (base)[lab0], 1e-3), 0.1, 2.5)
	valid = (conf0 >= 0.5) & ~np.asarray (Image.fromarray (symbox.astype (np.uint8) * 255).resize ((w, h), Image.BILINEAR)).astype (bool)
	sg = pal.get ("fill_blur", 2.0)
	filled = ndimage.gaussian_filter (ratio0 * valid, sg) / np.maximum (ndimage.gaussian_filter (valid.astype (np.float32), sg), 1e-3)
	ratio0 = np.where (valid, ratio0, np.where (ndimage.gaussian_filter (valid.astype (np.float32), sg) > 0.02, filled, 1.0))
	ratio0 = ndimage.gaussian_filter (ratio0, pal.get ("shade_blur", 1.0))
	shade = np.asarray (Image.fromarray (ratio0.astype (np.float32), "F").resize ((W, H), Image.BICUBIC))
	shade = np.clip (1.0 + (shade - 1.0) * np.where (shade < 1.0, pal.get ("grime", 1.0), 1.0), 0.08, 2.5)
	grime = np.clip (1.0 - shade, 0, 1)
	n_fine = tr.noise ((H, W), 2.5 * s / 4, 11, 2)
	n_mid = tr.noise ((H, W), 10.0 * s / 4, 12, 3)
	n_big = tr.noise ((H, W), 60.0 * s / 4, 13, 3)
	# panel seams: grooves of one width, the paint in them a shade darker
	lines = sv["lines"]
	groove = stroke_lines (lines, W, H, s) * ~symbox   # (none through symbols)
	seam_shade = np.array ([m.get ("seam", pal.get ("seam_shade", 0.72)) for m in mats], np.float32)[label]
	dist = ndimage.distance_transform_edt (groove < 0.5) / s   # (original texels from a seam)
	halo = np.exp (-dist / 0.5) * (1.0 - groove) * pal.get ("seam_grime", 0.1)
	# worn paint: chips along the seams (panel edges)
	wear, bare = chips (pal, dist, n_big, W, H, s)
	wear = wear * np.array ([m.get ("wears", True) for m in mats], np.float32)[label]
	bare = bare * wear
	alb = alb * shade[..., None] * (1.0 + 0.03 * n_big[..., None])
	bare_col = tr.srgb_to_lin (np.array (pal.get ("bare", [168, 170, 172]), np.float32) / 255.0)
	under = wear_lin * (1 - bare[..., None]) + bare_col * bare[..., None]
	alb = alb * (1.0 - wear[..., None]) + under * shade[..., None] * wear[..., None]
	alb = alb * (1.0 - (1.0 - seam_shade[..., None]) * groove[..., None]) * (1.0 - halo[..., None])
	# decals: the patches as they are (enlarged), over everything
	alb_s = tr.lin_to_srgb (alb)
	for x, y, pw, ph, im in sv["decals"]:
		x0, y0 = int (round (x * s)), int (round (y * s))
		tw, th = max (1, int (round (pw * s))), max (1, int (round (ph * s)))
		p = np.asarray (im.resize ((tw, th), Image.LANCZOS), np.float32) / 255.0
		x1, y1 = min (W, x0 + tw), min (H, y0 + th)
		p = p[:y1 - y0, :x1 - x0]
		a = p[..., 3:4]
		alb_s[y0:y1, x0:x1] = alb_s[y0:y1, x0:x1] * (1 - a) + p[..., :3] * a
	# overlay symbols, drawn anew at this resolution: flat paint, under the
	# same broad shading as the surface around them (no chips, no specks)
	for sym in ov["symbols"]:
		X0, Y0, a = draw_symbol (sym, W, H, s)
		x0, y0, x1, y1 = max (0, X0), max (0, Y0), min (W, X0 + a.shape[1]), min (H, Y0 + a.shape[0])
		if x1 <= x0 or y1 <= y0: continue
		p = a[y0 - Y0:y1 - Y0, x0 - X0:x1 - X0]
		al = p[..., 3:4]
		rgb = np.where (al > 1e-4, p[..., :3] / np.maximum (al, 1e-4), 0)
		sh = shade[y0:y1, x0:x1, None]
		alb_s[y0:y1, x0:x1] = alb_s[y0:y1, x0:x1] * (1 - al) + tr.lin_to_srgb (tr.srgb_to_lin (rgb) * np.clip (sh, 0, 1.1)) * al
		wear[y0:y1, x0:x1] *= 1 - al[..., 0]
	# height: grooves, fasteners, surface grain
	kind = np.array ([{"paint": 1, "metal": 2, "rough": 3}.get (m.get ("detail", "none"), 0) for m in mats])[label]
	if matmaps is not None: kind = np.where (matmaps[1] > 0.5, 2, np.where (kind == 2, 0, kind))
	streak = ndimage.gaussian_filter (np.random.default_rng (3).standard_normal ((H, W)).astype (np.float32), (0.6, 18.0))
	streak /= streak.std () + 1e-6
	height = -groove * pal.get ("groove_depth", 1.0)
	fast_mats = np.array ([m.get ("fasteners", False) for m in mats])
	if pal.get ("fastener_spacing"):
		fm = fast_mats[label]
		height += 0.35 * fasteners (lines, lambda ix, iy: fm[iy, ix], W, H, s, pal["fastener_spacing"], pal.get ("fastener_offset", 1.2), pal.get ("fastener_radius", 0.22))
	height += np.where (kind == 1, 0.012 * n_fine, 0.0)
	height += np.where (kind == 2, 0.01 * streak, 0.0)
	height += np.where (kind == 3, 0.04 * n_fine + 0.03 * n_mid, 0.0)
	height -= 0.12 * wear + 0.05 * bare   # (chipped paint: a little lower)
	k = pal.get ("bump", 1.5) * 4.0 / s
	gy, gx = np.gradient (height)
	nrm = np.stack ([-gx * k * s, -gy * k * s, np.ones_like (height)], -1)
	nrm /= np.linalg.norm (nrm, axis=-1, keepdims=True)
	# roughness, metalness
	if matmaps is not None:
		sm, me, _ = matmaps
		rough = 1.0 - sm
		metal = me
	else:
		rough = np.array ([m["rough"] for m in mats], np.float32)[label]
		metal = np.array ([m["metal"] for m in mats], np.float32)[label]
	rough = np.clip (rough + 0.3 * grime + 0.2 * groove + 0.1 * wear - 0.2 * bare, 0.04, 1.0)
	metal = np.clip (np.maximum (metal * (1.0 - 0.6 * grime), bare), 0, 1)
	return np.clip (alb_s, 0, 1), nrm * 0.5 + 0.5, 1.0 - rough, metal

if __name__ == "__main__":
	ap = argparse.ArgumentParser ()
	ap.add_argument ("cmd", choices=["extract", "render"])
	ap.add_argument ("palette"); ap.add_argument ("src"); ap.add_argument ("a"); ap.add_argument ("b", nargs="?")
	ap.add_argument ("--size", type=int, default=2048)
	ap.add_argument ("--mesh"); ap.add_argument ("--texname"); ap.add_argument ("--matcfg"); ap.add_argument ("--meshname")
	ap.add_argument ("--overlay", help="symbols drawn over the texture (JSON)")
	a = ap.parse_args ()
	pal = json.load (open (a.palette))
	if a.cmd == "extract":
		extract (pal, a.src, a.a)
	else:
		mm = None
		if a.mesh:
			im = Image.open (a.src)
			H = int (round (a.size * im.size[1] / im.size[0]))
			cfg = tr.material_config (a.matcfg, a.meshname) if a.matcfg else {}
			mm = tr.material_maps (a.mesh, a.texname, cfg, a.size, H)
		alb, nrm, smooth, metal = render (pal, a.src, a.a, a.size, mm, a.overlay)
		u8 = lambda x: Image.fromarray ((np.clip (x, 0, 1) * 255 + 0.5).astype (np.uint8))
		u8 (alb).save (a.b + ".png"); u8 (nrm).save (a.b + "_norm.png")
		u8 (smooth).save (a.b + "_rghn.png"); u8 (metal).save (a.b + "_metal.png")
		print ("%s: %d x %d" % (a.b, alb.shape[1], alb.shape[0]))
