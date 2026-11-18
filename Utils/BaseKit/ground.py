#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""The kit's ground of each base (roads, tracks, aprons: Meshes/BaseKit/
<Base>_ground.msh) from its description (Utils/BaseKit/ground/<Base>.json,
written by basekit.py and offworld.py layout), kept clear of the base's
landing pads: roads go around them (and around an off-world pad's blast
berm), aprons have holes where pads stand on them. (The pads lie under the
ground mesh, a few cm over the terrain: a road across a pad was drawn over
its markings, an apron hid the pads on it.)

    ground.py extract [Base ...]
        the descriptions from the ground meshes as they are (once: the
        layouts were made before the descriptions were kept)
    ground.py build [Base ...]
        the ground meshes from the descriptions and the bases' pads, and
        the lights along their roads and about their aprons (the Vulkan
        client's: Config/VK/Bases/<Name>.cfg, its groundlights block)
    ground.py check [Base ...]
        ground triangles on pads (and roads within the berms), per base
    ground.py berms [Base ...]
        the blast berms turned (their configuration files): their openings
        towards what reaches into them (a tunnel's arch), else towards the
        base's buildings

Description (base frame [m]: x south, z east, as the configuration files):
    {"strips": [{"pts": [[x, z], ...], "w": width, "mat": m, "tex": t|null, "y": h}],
     "polys":  [{"pts": [[x, z], ...] (convex), "mat": m, "tex": t|null, "y": h}]}
y: the height over the terrain (strips 0.05, aprons 0.03: roads over aprons).
"""

import glob, json, math, os, re, sys

HERE = os.path.dirname (os.path.abspath (__file__))
ROOT = os.path.normpath (os.path.join (HERE, '..', '..'))
sys.path.insert (0, HERE)
import basekit as bk
import offworld  # (its materials: soil_, track_)
from basekit import MeshBuilder

DESC_DIR = os.path.join (HERE, 'ground')
MESH_DIR = os.path.join (ROOT, 'Meshes', 'BaseKit')
CFG_GLOBS = ['Src/Celbody/*/Config/*/Base/*.cfg', 'Src/Celbody/*/*/Config/*/Base/*.cfg']
SEG = 20.0          # cells at most this long [m] (the mesh is wrapped onto the terrain at its vertices)
BERM_R = 56.0       # an off-world pad's blast berm: its outer foot [m] (blender_kit.py a_padberm: r 48 + 4, wobble)
CLEAR = 2.0         # roads' edges this far from pads (and berms) [m]
# the pads' outlines (Baseobj.cpp: Lpad01 its rim's foot, Lpad02/02a +-40 m)
OCTAGON = [(41.848, -17.334), (41.848, 17.334), (17.334, 41.848), (-17.334, 41.848),
	(-41.848, 17.334), (-41.848, -17.334), (-17.334, -41.848), (17.334, -41.848)]
SQUARE = [(40.0, -40.0), (40.0, 40.0), (-40.0, 40.0), (-40.0, -40.0)]

# ======================================================================
# Plane geometry (convex polygons: lists of (x, z), either orientation)

def area2 (P):
	return sum (P[i][0] * P[(i + 1) % len (P)][1] - P[(i + 1) % len (P)][0] * P[i][1] for i in range (len (P)))

def ccw (P):
	return P if area2 (P) > 0 else P[::-1]

def hull (pts):
	"""convex hull (counter-clockwise: x towards z)"""
	p = sorted (set ((round (a, 6), round (b, 6)) for a, b in pts))
	if len (p) < 3: return p
	cr = lambda o, a, b: (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
	lo, hi = [], []
	for q in p:
		while len (lo) >= 2 and cr (lo[-2], lo[-1], q) <= 0: lo.pop ()
		lo.append (q)
	for q in reversed (p):
		while len (hi) >= 2 and cr (hi[-2], hi[-1], q) <= 0: hi.pop ()
		hi.append (q)
	return lo[:-1] + hi[:-1]

def clip_half (P, a, b, keep_left):
	"""P clipped to the side of the line a->b (left: cross > 0)"""
	out = []
	side = lambda q: ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])) * (1 if keep_left else -1)
	for i in range (len (P)):
		p, q = P[i], P[(i + 1) % len (P)]
		sp, sq = side (p), side (q)
		if sp >= 0: out.append (p)
		if (sp >= 0) != (sq >= 0):
			t = sp / (sp - sq)
			out.append ((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
	return out

def subtract (A, B):
	"""A minus B (both convex): convex pieces"""
	B = ccw (B)
	pieces, rest = [], list (A)
	for i in range (len (B)):
		a, b = B[i], B[(i + 1) % len (B)]
		outside = clip_half (rest, a, b, False)
		if len (outside) >= 3 and abs (area2 (outside)) > 1e-3: pieces.append (outside)
		rest = clip_half (rest, a, b, True)
		if len (rest) < 3 or abs (area2 (rest)) < 1e-6: return pieces + ([] if len (rest) < 3 else [])
	return pieces  # (rest: inside B, dropped)

def overlaps (A, B):
	"""convex polygons with a common interior"""
	for P, Q in ((A, B), (B, A)):
		P = ccw (P)
		for i in range (len (P)):
			a, b = P[i], P[(i + 1) % len (P)]
			if all ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0]) <= 1e-6 for q in Q): return False
	return True

def seg_poly (a, b, P):
	"""the segment a->b through convex P (ccw): (t_in, t_out, edge in, edge out) or None"""
	t0, t1, e0, e1 = 0.0, 1.0, -1, -1
	d = (b[0] - a[0], b[1] - a[1])
	for i in range (len (P)):
		p, q = P[i], P[(i + 1) % len (P)]
		n = (q[1] - p[1], -(q[0] - p[0]))  # (outward, for ccw)
		num = n[0] * (a[0] - p[0]) + n[1] * (a[1] - p[1])
		den = n[0] * d[0] + n[1] * d[1]
		if abs (den) < 1e-12:
			if num > 0: return None
			continue
		t = -num / den
		if den < 0:
			if t > t0: t0, e0 = t, i
		else:
			if t < t1: t1, e1 = t, i
		if t0 > t1: return None
	if t1 - t0 < 1e-6: return None
	return t0, t1, e0, e1

def cells (P):
	"""a convex polygon in pieces of at most SEG across (on a SEG grid)"""
	xs, zs = [p[0] for p in P], [p[1] for p in P]
	if max (xs) - min (xs) <= SEG + 0.5 and max (zs) - min (zs) <= SEG + 0.5: return [P]
	out = []
	i0, i1 = int (math.floor (min (xs) / SEG)), int (math.ceil (max (xs) / SEG))
	k0, k1 = int (math.floor (min (zs) / SEG)), int (math.ceil (max (zs) / SEG))
	P = ccw (P)
	for i in range (i0, i1):
		for k in range (k0, k1):
			x0, x1, z0, z1 = i * SEG, (i + 1) * SEG, k * SEG, (k + 1) * SEG
			Q = P
			for (a, b) in (((x0, z0), (x1, z0)), ((x1, z0), (x1, z1)), ((x1, z1), (x0, z1)), ((x0, z1), (x0, z0))):
				Q = clip_half (Q, a, b, True)
				if len (Q) < 3: break
			if len (Q) >= 3 and abs (area2 (Q)) > 1e-3: out.append (Q)
	return out

# ======================================================================
# Pads

def base_cfgs ():
	"""ground mesh name -> its base's configuration files"""
	out = {}
	for g in CFG_GLOBS:
		for p in sorted (glob.glob (os.path.join (ROOT, g))):
			t = open (p, encoding='latin-1').read ()
			m = re.search (r'FILE\s+BaseKit\\(\S+)_ground\b', t)
			if m: out.setdefault (m.group (1), []).append (p)
	return out

def pads (cfgpath):
	"""the base's pads: (outline (ccw, base frame), keep-out of roads (ccw), berm)"""
	t = open (cfgpath, encoding='latin-1').read ().replace ('\r\n', '\n')
	berms = []
	for m in re.finditer (r'^MESH\s*\n(.*?)^END', t, re.M | re.S):
		if re.search (r'FILE\s+BaseKit\\space_padberm', m.group (1), re.I):
			p = re.search (r'POS\s+(\S+)\s+\S+\s+(\S+)', m.group (1))
			berms.append ((float (p.group (1)), float (p.group (2))))
	out = []
	for m in re.finditer (r'^(LPAD\w*)\s*\n(.*?)^END', t, re.M | re.S):
		body = m.group (2)
		pos = re.search (r'POS\s+(\S+)\s+\S+\s+(\S+)', body)
		if not pos: continue
		px, pz = float (pos.group (1)), float (pos.group (2))
		rot = re.search (r'ROT\s+(\S+)', body)
		sc = re.search (r'SCALE\s+(\S+)', body)
		a = math.radians (float (rot.group (1))) if rot else 0.0
		s = abs (float (sc.group (1))) if sc else 1.0
		if s < 1e-3: continue  # (a marker, not drawn)
		shape = OCTAGON if m.group (1).upper () == 'LPAD1' else SQUARE
		c, sn = math.cos (a), math.sin (a)
		outline = ccw ([(px + s * (c * x - sn * z), pz + s * (sn * x + c * z)) for (x, z) in shape])
		berm = any (math.hypot (bx - px, bz - pz) < 5.0 for bx, bz in berms)
		out.append ((outline, (px, pz), berm))
	return out

def grow (P, g):
	"""a convex polygon (ccw) grown by g: each corner moved out along its bisector"""
	n = len (P)
	out = []
	for i in range (n):
		a, b, c = P[i - 1], P[i], P[(i + 1) % n]
		def nrm (p, q):
			l = math.hypot (q[0] - p[0], q[1] - p[1]) or 1.0
			return ((q[1] - p[1]) / l, -(q[0] - p[0]) / l)
		n1, n2 = nrm (a, b), nrm (b, c)
		m = (n1[0] + n2[0], n1[1] + n2[1])
		k = g / max (0.2, (1 + n1[0] * n2[0] + n1[1] * n2[1]))
		out.append ((b[0] + m[0] * k, b[1] + m[1] * k))
	return out

def keepouts (P, half_w, clear=CLEAR):
	"""where roads of half width half_w must not go: about each pad (and its
	berm), grown by the road's half width and clear"""
	ks = []
	for outline, (px, pz), berm in P:
		if berm:
			r = (BERM_R + half_w + clear) / math.cos (math.pi / 16)  # (16-gon about the circle)
			ks.append (ccw ([(px + r * math.cos (2 * math.pi * k / 16), pz + r * math.sin (2 * math.pi * k / 16)) for k in range (16)]))
		else:
			ks.append (ccw (grow (outline, half_w + clear)))
	return ks

# ----------------------------------------------------------------------
# What else roads keep clear of: the base's buildings (its meshes - kit,
# legacy - by their footprints, its generic buildings), its corridors, its
# runways

LONG = 400.0  # (longer objects - a mass driver's rail - roads may cross)
_bounds = {}

def mesh_bounds (name):
	"""a mesh's footprint (x0, z0, x1, z1) in its frame: the kit's recorded one, else its vertices'"""
	key = name.lower ()
	if key in _bounds: return _bounds[key]
	fp = {k.lower (): v for k, v in json.load (open (os.path.join (HERE, 'footprints.json'))).items ()}
	b = None
	if key.split ('\\')[-1] in fp:
		v = fp[key.split ('\\')[-1]]
		b = (v['lo'][0], v['lo'][2], v['hi'][0], v['hi'][2])
	else:
		path = os.path.join (ROOT, 'Meshes', *name.split ('\\')) + '.msh'
		if os.path.exists (path):
			xs, zs = [], []
			for _, _, V, _ in read_msh (path):
				xs += [v[0] for v in V]; zs += [v[2] for v in V]
			if xs: b = (min (xs), min (zs), max (xs), max (zs))
	_bounds[key] = b
	return b

def place_rect (x0, z0, x1, z1, pos, rot, sc=(1.0, 1.0)):
	c, s = math.cos (math.radians (rot)), math.sin (math.radians (rot))
	pts = [(x * sc[0], z * sc[1]) for x, z in ((x0, z0), (x1, z0), (x1, z1), (x0, z1))]
	return ccw ([(pos[0] + c * x - s * z, pos[1] + s * x + c * z) for x, z in pts])

def corridors (path):
	"""the corridors of a tubes mesh: each a convex outline (its pieces - joined
	by their triangles, and where they meet, within 0.5 m)"""
	pts, grp = [], []
	def find (i):
		while grp[i] != i: grp[i] = grp[grp[i]]; i = grp[i]
		return i
	for _, _, V, T in read_msh (path):
		base = len (pts)
		pts += [(v[0], v[2]) for v in V]
		grp += list (range (base, base + len (V)))
		for t in T:
			for k in (1, 2): grp[find (base + t[k])] = find (base + t[0])
	cell = {}
	for i, (x, z) in enumerate (pts):
		k = (int (math.floor (x / 0.5)), int (math.floor (z / 0.5)))
		for dx in (-1, 0, 1):
			for dz in (-1, 0, 1):
				for j in cell.get ((k[0] + dx, k[1] + dz), []):
					if math.hypot (x - pts[j][0], z - pts[j][1]) < 0.5: grp[find (i)] = find (j)
		cell.setdefault (k, []).append (i)
	out = {}
	for i, p in enumerate (pts): out.setdefault (find (i), []).append (p)
	return [ccw (hull (v)) for v in out.values () if len (v) >= 3]

def buildings (cfgpath):
	"""the base's obstacles to roads: (outline (ccw, base frame), kind:
	'building', 'corridor', 'runway', what it is: a mesh's file, a generic building's type)"""
	t = open (cfgpath, encoding='latin-1').read ().replace ('\r\n', '\n')
	t = t[t.upper ().find ('BEGIN_OBJECTLIST'):]
	out = []
	num = lambda m, k, d=0.0: float (m.group (k)) if m and m.group (k) else d
	for m in re.finditer (r'^(MESH|BLOCK|HANGAR|HANGAR2|HANGAR3|TANK|RUNWAY)\s*\n(.*?)^END\s*$', t, re.M | re.S):
		kind, body = m.group (1), m.group (2)
		pos = re.search (r'POS\s+(\S+)\s+\S+\s+(\S+)', body)
		p = (float (pos.group (1)), float (pos.group (2))) if pos else (0.0, 0.0)
		rot = num (re.search (r'ROT\s+(\S+)', body), 1)
		sc = re.search (r'SCALE\s+(\S+)(?:\s+(\S+)\s+(\S+))?', body)
		if kind == 'RUNWAY':
			e1 = re.search (r'END1\s+(\S+)\s+\S+\s+(\S+)', body); e2 = re.search (r'END2\s+(\S+)\s+\S+\s+(\S+)', body)
			w = num (re.search (r'WIDTH\s+(\S+)', body), 1, 50.0) + 20.0  # (with its shoulders)
			a, b = (float (e1.group (1)), float (e1.group (2))), (float (e2.group (1)), float (e2.group (2)))
			L = math.hypot (b[0] - a[0], b[1] - a[1]); ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
			e = 60.0  # (its overruns)
			out.append ((ccw ([(a[0] - ux * e - uz * w / 2, a[1] - uz * e + ux * w / 2), (a[0] - ux * e + uz * w / 2, a[1] - uz * e - ux * w / 2),
				(b[0] + ux * e + uz * w / 2, b[1] + uz * e - ux * w / 2), (b[0] + ux * e - uz * w / 2, b[1] + uz * e + ux * w / 2)]), 'runway', 'RUNWAY'))
			continue
		if kind == 'MESH':
			f = re.search (r'FILE\s+(\S+)', body)
			if not f: continue
			name = f.group (1)
			low = name.lower ()
			if low.endswith ('_tubes'):  # (corridors: wrapped onto the ground, on their supports)
				path = os.path.join (ROOT, 'Meshes', *name.split ('\\')) + '.msh'
				if os.path.exists (path): out += [(c, 'corridor', name) for c in corridors (path)]
				continue
			if re.search (r'WRAPTOSURFACE', body, re.I): continue
			if low.endswith ('_ground') or 'padberm' in low: continue
			bd = mesh_bounds (name)
			if not bd: continue
			s3 = (num (sc, 1, 1.0), num (sc, 3, num (sc, 1, 1.0))) if sc else (1.0, 1.0)
			if max ((bd[2] - bd[0]) * s3[0], (bd[3] - bd[1]) * s3[1]) > LONG: continue
			out.append ((place_rect (bd[0], bd[1], bd[2], bd[3], p, rot, s3), 'building', name))
			continue
		if not sc: continue
		sx, sz = num (sc, 1), num (sc, 3, num (sc, 1))
		if kind == 'TANK':  # (SCALE: radius, height, radius)
			r = sx / math.cos (math.pi / 8)
			out.append ((ccw ([(p[0] + r * math.cos (math.pi / 4 * k + math.pi / 8), p[1] + r * math.sin (math.pi / 4 * k + math.pi / 8)) for k in range (8)]), 'building', kind))
		elif max (sx, sz) <= LONG or kind != 'BLOCK':
			out.append ((place_rect (-sx / 2, -sz / 2, sx / 2, sz / 2, p, rot), 'building', kind))
	return out

# ----------------------------------------------------------------------
# Roads: around what is in their way (the shortest way about the obstacles,
# grown by a road's half width and clear: a visibility graph of their
# corners), roads fanning out from a point joined into a network

def dist_in (p, P):
	"""how far p is inside convex P (ccw): its least distance to an edge's line (< 0: outside)"""
	d = 1e30
	for i in range (len (P)):
		a, b = P[i], P[(i + 1) % len (P)]
		l = math.hypot (b[0] - a[0], b[1] - a[1]) or 1.0
		d = min (d, ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / l)
	return d

def blocked (a, b, polys, eps=1e-3):
	"""does the segment a-b pass through any polygon's inside (along an edge: not)"""
	for P in polys:
		r = seg_poly (a, b, P)
		if not r: continue
		t = 0.5 * (r[0] + r[1])
		if dist_in ((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t), P) > eps: return True
	return False

def leave (a, b, polys):
	"""a moved along a-b out of the polygons it is in (None: b too is in them)"""
	for _ in range (20):
		inside = [P for P in polys if dist_in (a, P) > 1e-6]
		if not inside: return a
		t = 0.0
		for P in inside:
			r = seg_poly (a, b, P)
			if r: t = max (t, r[1])
		if t >= 1.0 - 1e-9: return None
		a = (a[0] + (b[0] - a[0]) * (t + 1e-6), a[1] + (b[1] - a[1]) * (t + 1e-6))
	return a

def shortest (a, b, polys):
	"""the shortest way from a to b about the polygons (their corners outside
	the others: a visibility graph); from and to their edges where a, b are inside"""
	a2 = leave (a, b, polys)
	if a2 is None: return None
	b2 = leave (b, a2, polys)
	if b2 is None: return None
	a, b = a2, b2
	if not blocked (a, b, polys): return [a, b]
	# (the polygons near the way only)
	m = 600.0  # (by their bounding boxes: a long runway's corners are far from where a road meets it)
	x0, x1, z0, z1 = min (a[0], b[0]) - m, max (a[0], b[0]) + m, min (a[1], b[1]) - m, max (a[1], b[1]) + m
	near = [P for P in polys if min (v[0] for v in P) < x1 and max (v[0] for v in P) > x0 and min (v[1] for v in P) < z1 and max (v[1] for v in P) > z0]
	nodes = [a, b] + [v for P in near for v in P if all (dist_in (v, Q) <= 1e-6 for Q in near)]
	import heapq
	dist, prev, done = {0: 0.0}, {}, set ()
	q = [(0.0, 0)]
	while q:
		d, i = heapq.heappop (q)
		if i in done: continue
		done.add (i)
		if i == 1: break
		for j in range (len (nodes)):
			if j in done: continue
			nd = d + math.hypot (nodes[j][0] - nodes[i][0], nodes[j][1] - nodes[i][1])
			if nd >= dist.get (j, 1e30): continue
			if blocked (nodes[i], nodes[j], near): continue
			dist[j], prev[j] = nd, i
			heapq.heappush (q, (nd, j))
	if 1 not in prev: return [a, b]  # (no way about: straight, as before)
	path, k = [nodes[1]], 1
	while k != 0: k = prev[k]; path.append (nodes[k])
	return path[::-1]

def clean (line):
	"""repeated and collinear points out; None if under a metre long"""
	out = []
	for p in line:
		if out and math.hypot (p[0] - out[-1][0], p[1] - out[-1][1]) < 0.05: continue
		out.append (p)
	k = 1
	while k < len (out) - 1:
		p, q, r = out[k - 1], out[k], out[k + 1]
		if abs ((q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])) < 1e-3 * math.hypot (r[0] - p[0], r[1] - p[1]): del out[k]
		else: k += 1
	if len (out) < 2 or sum (math.hypot (q[0] - p[0], q[1] - p[1]) for p, q in zip (out, out[1:])) < 1.0: return None
	return out

def nearest_on (lines, p):
	"""the point of the polylines nearest p"""
	best, bd = None, 1e30
	for line in lines:
		for a, b in zip (line, line[1:]):
			dx, dz = b[0] - a[0], b[1] - a[1]
			l2 = dx * dx + dz * dz or 1e-12
			t = max (0.0, min (1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2))
			q = (a[0] + dx * t, a[1] + dz * t)
			d = math.hypot (p[0] - q[0], p[1] - q[1])
			if d < bd: best, bd = q, d
	return best

def plan_roads (strips, P, B):
	"""the roads (polylines, their strips) clear of the pads P and the
	obstacles B (buildings, corridors, runways): a road through any, or
	ending in one, the shortest way about them (to their edge); roads from
	one point (a fan) a network - each from the nearest point of those laid"""
	out = []
	def obstacles (s):
		hw = s['w'] / 2
		taxi = s['w'] >= 20.0
		touch = keepouts (P, hw, 0.0) + [grow (o, hw) for o, k, _ in B if not (taxi and k == 'runway')]
		keep = keepouts (P, hw) + [grow (o, hw + CLEAR) for o, k, _ in B if not (taxi and k == 'runway')]
		return touch, keep
	# (fans: straight roads, of one kind, from a common end)
	groups = {}
	for s in strips:
		a = s['pts'][0]
		key = (round (a[0], 0), round (a[1], 0), s['mat'], s['w'])
		groups.setdefault (key, []).append (s)
	for key, g in groups.items ():
		root = tuple (g[0]['pts'][0])
		touch, keep = obstacles (g[0])
		if len (g) == 1:
			s = g[0]
			pts = [tuple (p) for p in s['pts']]
			if not any (blocked (p, q, touch) for p, q in zip (pts, pts[1:])) and not any (dist_in (q, T) > 1e-6 for q in (pts[0], pts[-1]) for T in touch):
				out.append ((pts, s)); continue  # (clear: as it was laid)
			path = shortest (pts[0], pts[-1], keep)
			if path and clean (path): out.append ((clean (path), s))
			continue
		net = []
		for s in sorted (g, key=lambda s: math.hypot (s['pts'][-1][0] - root[0], s['pts'][-1][1] - root[1])):
			e = tuple (s['pts'][-1])
			start = nearest_on (net, e) if net else root
			path = shortest (start, e, keep)
			if path and clean (path):
				net.append (clean (path)); out.append ((clean (path), s))
	return out

def inside_any (p, K):
	for P in K:
		if all ((P[(i + 1) % len (P)][0] - P[i][0]) * (p[1] - P[i][1]) - (P[(i + 1) % len (P)][1] - P[i][1]) * (p[0] - P[i][0]) > 0 for i in range (len (P))):
			return True
	return False

# ======================================================================
# Meshes

def read_msh (path):
	"""groups: (material name, texture name or None, vertices, triangles)"""
	L = open (path, errors='replace').read ().split ('\n')
	groups, i, cur = [], 0, {}
	mats, texs = [], []
	while i < len (L):
		w = L[i].split ()
		if not w: i += 1; continue
		k = w[0].upper ()
		if k == 'MATERIAL' and len (w) == 2 and w[1].isdigit (): cur['mat'] = int (w[1])
		elif k == 'TEXTURE' and len (w) == 2: cur['tex'] = int (w[1])
		elif k == 'GEOM':
			nv, nt = int (w[1]), int (w[2])
			v = [tuple (float (x) for x in L[i + 1 + j].split ()[:3]) for j in range (nv)]
			t = [tuple (int (x) for x in L[i + 1 + nv + j].split ()[:3]) for j in range (nt)]
			groups.append ((cur.get ('mat', 0), cur.get ('tex', 0), v, t)); cur = {}
			i += 1 + nv + nt; continue
		elif k == 'MATERIALS':
			n = int (w[1]); mats = [L[i + 1 + j].strip () for j in range (n)]; i += 1 + n; continue
		elif k == 'TEXTURES':
			n = int (w[1]); texs = [L[i + 1 + j].strip () for j in range (n)]; i += 1 + n; continue
		i += 1
	return [(mats[m - 1] if m else None, texs[t - 1] if t else None, v, tr) for (m, t, v, tr) in groups]

def extract (name):
	"""a description from a ground mesh: quads at road height (pieces of
	strips, their first edge along the road) and the rest (aprons)"""
	desc = {'strips': [], 'polys': []}
	for mat, tex, V, T in read_msh (os.path.join (MESH_DIR, name + '_ground.msh')):
		# (polygons: consecutive vertices, fans of triangles)
		polys, k = [], 0
		while k < len (T):
			b = T[k][0]
			n = 1
			while k + n < len (T) and T[k + n][0] == b: n += 1
			polys.append ([V[j] for j in range (b, b + n + 2)])
			k += n
		strip = None
		for P in polys:
			y = round (P[0][1], 3)
			if len (P) == 4 and y >= 0.045:
				p0 = ((P[0][0] + P[3][0]) / 2, (P[0][2] + P[3][2]) / 2)
				p1 = ((P[1][0] + P[2][0]) / 2, (P[1][2] + P[2][2]) / 2)
				w = round (math.hypot (P[0][0] - P[3][0], P[0][2] - P[3][2]), 2)
				if strip and strip['w'] == w and math.hypot (p0[0] - strip['pts'][-1][0], p0[1] - strip['pts'][-1][1]) < 0.05:
					strip['pts'].append (p1)
				else:
					strip = {'pts': [p0, p1], 'w': w, 'mat': mat, 'tex': tex, 'y': y}
					desc['strips'].append (strip)
			else:
				strip = None
				desc['polys'].append ({'pts': [(p[0], p[2]) for p in P], 'mat': mat, 'tex': tex, 'y': y})
	for s in desc['strips']:  # (straight runs: their ends)
		s['pts'] = [s['pts'][0]] + [p for a, p, c in zip (s['pts'], s['pts'][1:], s['pts'][2:])
			if abs ((p[0] - a[0]) * (c[1] - a[1]) - (p[1] - a[1]) * (c[0] - a[0])) > 1e-3 * math.hypot (c[0] - a[0], c[1] - a[1])] + [s['pts'][-1]]
	return desc

def round_desc (d):
	r = lambda P: [[round (x, 3), round (z, 3)] for x, z in P]
	return {'strips': [dict (s, pts=r (s['pts'])) for s in d['strips']], 'polys': [dict (p, pts=r (p['pts'])) for p in d['polys']]}

def write_desc (name, desc):
	os.makedirs (DESC_DIR, exist_ok=True)
	d = round_desc (desc)
	with open (os.path.join (DESC_DIR, name + '.json'), 'w', newline='\n') as f:
		f.write ('{"strips": [\n' + ',\n'.join (json.dumps (s) for s in d['strips']) + '],\n"polys": [\n'
			+ ',\n'.join (json.dumps (p) for p in d['polys']) + ']}\n')

def build (name, desc, P, path, obst=()):
	"""the ground mesh from its description, its roads clear of the pads P and the obstacles obst"""
	B = MeshBuilder ()
	outlines = [o for o, _, _ in P]
	def poly (Q, y, mat, tex):
		for C in cells (Q):
			pts = [(x, y, z) for (x, z) in ccw (C)]
			if tex: B.poly (pts, (0, 1, 0), mat, tex=tex)
			else: B.poly (pts, (0, 1, 0), mat, uv=[(0, 0)] * len (pts))
	def clear_of_pads (Q):
		pieces = [Q]
		for O in outlines:
			nxt = []
			for R in pieces:
				nxt += subtract (R, O) if overlaps (R, O) else [R]
			pieces = nxt
		return pieces
	def strip (pts, hw, s):
		for a, b in zip (pts[:-1], pts[1:]):
			dx, dz = b[0] - a[0], b[1] - a[1]
			l = math.hypot (dx, dz)
			if l < 0.05: continue
			n = max (1, int (math.ceil (l / SEG)))
			px, pz = -dz / l * hw, dx / l * hw
			for k in range (n):
				p0 = (a[0] + dx * k / n, a[1] + dz * k / n); p1 = (a[0] + dx * (k + 1) / n, a[1] + dz * (k + 1) / n)
				for Q in clear_of_pads ([(p0[0] - px, p0[1] - pz), (p1[0] - px, p1[1] - pz), (p1[0] + px, p1[1] + pz), (p0[0] + px, p0[1] + pz)]):
					poly (Q, s['y'], s['mat'], s['tex'])
		# (the bends: filled, a disc's piece)
		for c in pts[1:-1]:
			disc = [(c[0] + hw * math.cos (2 * math.pi * k / 12), c[1] + hw * math.sin (2 * math.pi * k / 12)) for k in range (12)]
			for Q in clear_of_pads (disc): poly (Q, s['y'], s['mat'], s['tex'])
	roads = plan_roads (desc['strips'], P, obst)
	for pts, s in roads: strip (pts, s['w'] / 2, s)
	for p in desc['polys']:
		for Q in clear_of_pads ([tuple (q) for q in p['pts']]):
			poly (Q, p['y'], p['mat'], p['tex'])
	B.write (path)
	return roads

# ======================================================================
# Blast berms: their opening (+z of the mesh) towards what reaches into them
# (a tunnel's arch), else towards the base's buildings about them

def dist_to (p, P):
	"""from p to convex P (0 inside)"""
	if dist_in (p, P) >= 0: return 0.0
	d = 1e30
	for a, b in zip (P, P[1:] + P[:1]):
		dx, dz = b[0] - a[0], b[1] - a[1]
		t = max (0.0, min (1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / (dx * dx + dz * dz or 1e-12)))
		d = min (d, math.hypot (p[0] - a[0] - dx * t, p[1] - a[1] - dz * t))
	return d

def orient_berms (cfgpath):
	"""the berms' ROT in a configuration file: their openings towards what
	reaches into them, else towards the buildings within 800 m (their middle);
	the number turned"""
	B = [(o, nm) for o, k, nm in buildings (cfgpath) if k == 'building']
	t = open (cfgpath, encoding='latin-1', newline='').read ()
	n = 0
	def fix (m):
		nonlocal n
		body = m.group (0)
		pos = re.search (r'POS\s+(\S+)\s+\S+\s+(\S+)', body)
		c = (float (pos.group (1)), float (pos.group (2)))
		close = [(dist_to (c, o), o) for o, _ in B if dist_to (c, o) < BERM_R + 12.0]
		if close:
			o = min (close)[1]
			# (the point of it nearest the pad)
			best = min (((a[0] + (b[0] - a[0]) * k / 20, a[1] + (b[1] - a[1]) * k / 20) for a, b in zip (o, o[1:] + o[:1]) for k in range (21)),
				key=lambda q: math.hypot (q[0] - c[0], q[1] - c[1]))
			tx, tz = best
		else:
			near = [o for o, _ in B if 0 < dist_to (c, o) < 800.0]
			if not near: return body
			cs = [(sum (v[0] for v in o) / len (o), sum (v[1] for v in o) / len (o)) for o in near]
			tx, tz = sum (q[0] for q in cs) / len (cs), sum (q[1] for q in cs) / len (cs)
		bearing = math.degrees (math.atan2 (tz - c[1], -(tx - c[0])))  # (east, north: x south, z east)
		rot = bk.BROT (bearing)
		new = re.sub (r'(ROT\s+)\S+', lambda r: r.group (1) + '%.1f' % rot, body, count=1) if re.search (r'ROT\s+\S+', body) else body.replace (pos.group (0), pos.group (0) + '\n\tROT %.1f' % rot)
		if new != body: n += 1
		return new
	t2 = re.sub (r'MESH\s*\r?\n\s*FILE\s+BaseKit\\space_padberm_\w+.*?\r?\nEND', fix, t, flags=re.S)
	if t2 != t: open (cfgpath, 'w', encoding='latin-1', newline='').write (t2)
	return n

# ======================================================================
# Lights (the Vulkan client's: Config/VK/Bases/<Name>.cfg, between
# "; BEGIN groundlights" and "; END groundlights")

LBEGIN, LEND = '; BEGIN groundlights', '; END groundlights'
TRACK_LIGHT = ((0.50, 0.70, 1.00), 0.40, 30.0, 0.3)   # (colour, size, step [m], height [m]): off-world tracks' edges
TAXI_LIGHT = ((0.25, 0.45, 1.00), 0.60, 30.0, 0.4)    # taxiways' edges (blue)
STREET_LIGHT = ((1.00, 0.70, 0.35), 0.90, 45.0, 9.0)  # roads: sodium lamps on poles, on one side
MAST = ((1.00, 0.90, 0.75), 1.20, 18.0)                # aprons' light masts: colour, size, height
MAST_FLOOD = '250  5 4.7 4.2  60 90'                   # (their floodlights: range, brightness, beam [deg])
# off-world: work lights on masts along the tracks about the base's core (a
# wash on the ground: the floodlights' light only reaches ~80 m), every
# WORK_STEP within WORK_REACH of the base's origin, not where floodlights are
WORK_STEP, WORK_REACH, WORK_H = 120.0, 500.0, 12.0
WORK_FLOOD = '300  5 4.8 4.4  110 160'

def work_lights (roads, floods):
	out = []
	for pts, s in roads:
		if not s['mat'].startswith ('track_'): continue
		off = s['w'] / 2 + 2.0
		L = sum (math.hypot (q[0] - p[0], q[1] - p[1]) for p, q in zip (pts, pts[1:]))
		t = WORK_STEP / 2
		while t < L:
			r = t
			for a, b in zip (pts, pts[1:]):
				l = math.hypot (b[0] - a[0], b[1] - a[1])
				if r <= l:
					ux, uz = (b[0] - a[0]) / l, (b[1] - a[1]) / l
					x, z = a[0] + ux * r - uz * off, a[1] + uz * r + ux * off
					if math.hypot (x, z) <= WORK_REACH and not any (math.hypot (x - fx, z - fz) < 60.0 for fx, fz in floods):
						out.append (('FLOODLIGHTH', x, WORK_H, z, (0.0, -1.0, 0.0), WORK_FLOOD))
						floods.append ((x, z))
					break
				r -= l
			t += WORK_STEP
	return out

def road_lights (roads):
	"""lights along the roads (base frame; h above the ground)"""
	out = []
	for pts, s in roads:
		hw = s['w'] / 2
		if s['mat'].startswith ('track_'): (col, size, step, h), sides = TRACK_LIGHT, (-1, 1)
		elif s['w'] >= 20.0: (col, size, step, h), sides = TAXI_LIGHT, (-1, 1)
		else: (col, size, step, h), sides = STREET_LIGHT, (1,)
		off = hw + (1.5 if h > 2 else 0.6)
		L = sum (math.hypot (q[0] - p[0], q[1] - p[1]) for p, q in zip (pts, pts[1:]))
		n = int (L / step)
		if n < 1: continue
		for k in range (n + 1):
			t = 0.5 * (L - n * step) + k * step  # (centred along the road)
			for a, b in zip (pts, pts[1:]):
				l = math.hypot (b[0] - a[0], b[1] - a[1])
				if t <= l + 1e-6:
					ux, uz = (b[0] - a[0]) / l, (b[1] - a[1]) / l
					x, z = a[0] + ux * t, a[1] + uz * t
					for sd in sides: out.append (('LIGHT', x - uz * off * sd, h, z + ux * off * sd, col, size))
					break
				t -= l
	return out

def apron_lights (desc, floods):
	"""masts at the corners of the aprons (touching ones as one): a lamp, and a
	floodlight on the apron (not where floodlights are already)"""
	polys = [p['pts'] for p in desc['polys'] if p['mat'] == 'apron']
	if not polys: return []
	grp = list (range (len (polys)))
	def find (i):
		while grp[i] != i: grp[i] = grp[grp[i]]; i = grp[i]
		return i
	# (touching: a vertex in common, within 1 m; on a 2 m grid)
	cell = {}
	for i, P in enumerate (polys):
		for (x, z) in P:
			for dx in (-1, 0, 1):
				for dz in (-1, 0, 1):
					k = (int (math.floor (x / 2)) + dx, int (math.floor (z / 2)) + dz)
					for j in cell.get (k, []):
						if any (math.hypot (x - a, z - b) < 1.0 for a, b in polys[j]): grp[find (i)] = find (j)
			cell.setdefault ((int (math.floor (x / 2)), int (math.floor (z / 2))), []).append (i)
	clusters = {}
	for i in range (len (polys)): clusters.setdefault (find (i), []).extend (polys[i])
	out = []
	for pts in clusters.values ():
		H = ccw (hull (pts))
		if len (H) < 3: continue
		cx, cz = sum (p[0] for p in H) / len (H), sum (p[1] for p in H) / len (H)
		# (corners: where the outline turns by 25 degrees and more)
		corners = []
		for i in range (len (H)):
			a, b, c = H[i - 1], H[i], H[(i + 1) % len (H)]
			t1, t2 = math.atan2 (b[1] - a[1], b[0] - a[0]), math.atan2 (c[1] - b[1], c[0] - b[0])
			turn = abs ((t2 - t1 + math.pi) % (2 * math.pi) - math.pi)
			if turn > math.radians (25): corners.append (b)
		for (x, z) in corners:
			d = math.hypot (x - cx, z - cz)
			if d < 1.0: continue
			mx, mz = x + (x - cx) / d * 3.0, z + (z - cz) / d * 3.0  # (3 m off the apron)
			col, size, h = MAST
			if any (math.hypot (mx - fx, mz - fz) < 60.0 for fx, fz in floods):
				out.append (('LIGHT', mx, h, mz, col, size))  # (a lamp only: a floodlight near)
				continue
			tx, tz = mx + 0.45 * (cx - mx), mz + 0.45 * (cz - mz)  # (aimed at the apron, about half way in)
			v = (tx - mx, -h, tz - mz); l = math.sqrt (sum (q * q for q in v))
			out.append (('FLOODLIGHTH', mx, h, mz, tuple (q / l for q in v)))
			floods.append ((mx, mz))
	return out

def vk_path (cfgpath):
	t = open (cfgpath, encoding='latin-1').read ()
	title = re.search (r'Name\s*=\s*(.+)', t).group (1).strip ()
	return os.path.join (ROOT, 'Config', 'VK', 'Bases', bk.vk_name (title)), title

def write_lights (cfgpath, roads, desc):
	path, title = vk_path (cfgpath)
	text = open (path, newline='').read () if os.path.exists (path) else \
		'; Lights of %s for the Vulkan client (base frame [m]: x south, y up, z east)\n' % title
	a = text.find (LBEGIN)
	if a >= 0:
		b = text.find ('\n', text.find (LEND, a)) + 1
		text = text[:a] + text[b:]
	floods = []
	for line in text.split ('\n'):
		w = line.split (';')[0].split ()
		if w and w[0].upper () in ('FLOODLIGHT', 'FLOODLIGHTH'): floods.append ((float (w[1]), float (w[3])))
	ls = road_lights (roads) + apron_lights (desc, floods) + work_lights (roads, floods)
	lines = [LBEGIN, '; (made by Utils/BaseKit/ground.py: lights along the roads (LIGHT x h z  r g b  size),',
		'; masts about the aprons, work lights along tracks about an off-world base\'s core',
		'; (FLOODLIGHTH x h z  dx dy dz  range  r g b  umbra penumbra: h above the ground))']
	for l in ls:
		if l[0] == 'LIGHT':
			_, x, h, z, c, sz = l
			lines.append ('LIGHT %.1f %.1f %.1f  %.2f %.2f %.2f  %.2f' % (x, h, z, c[0], c[1], c[2], sz))
		else:
			x, h, z, d = l[1:5]
			lines.append ('FLOODLIGHTH %.1f %.1f %.1f  %.3f %.3f %.3f  %s' % (x, h, z, d[0], d[1], d[2], l[5] if len (l) > 5 else MAST_FLOOD))
	lines.append (LEND)
	text = text.rstrip ('\n') + '\n' + '\n'.join (lines) + '\n'
	with open (path, 'w', newline='\n') as f: f.write (text)
	return sum (1 for l in ls if l[0] == 'LIGHT'), sum (1 for l in ls if l[0] != 'LIGHT')

# ======================================================================
# From a layout (basekit.py, offworld.py: Layout, planned east and north)

def describe (L, tex=None):
	"""a layout's ground: its strips (roads) and rectangles (aprons)"""
	desc = {'strips': [], 'polys': []}
	for g in L.ground:
		if g[0] == 'strip':
			_, pts, w, mat = g
			desc['strips'].append ({'pts': [bk.BF (*p) for p in (pts[0], pts[-1])] if len (pts) > 1 else [], 'w': w, 'mat': mat, 'tex': tex, 'y': 0.05})
		elif g[0] == 'rect':
			_, (x, z), (w, d), bb, mat = g
			s, c = math.sin (math.radians (bb)), math.cos (math.radians (bb))
			q = [(-w / 2, -d / 2), (w / 2, -d / 2), (w / 2, d / 2), (-w / 2, d / 2)]
			desc['polys'].append ({'pts': [bk.BF (x + lx * c + lz * s, z - lx * s + lz * c) for lx, lz in q], 'mat': mat, 'tex': tex, 'y': 0.03})
	desc['strips'] = [s for s in desc['strips'] if len (s['pts']) == 2]
	return desc

def make (name, L, cfgpath, meshdir, tex=None):
	"""a layout's ground: its description kept (Utils/BaseKit/ground), its mesh
	(<meshdir>/BaseKit/<name>_ground.msh) clear of the base's pads (as its
	configuration has them: after the layout's block is written), its lights"""
	desc = describe (L, tex)
	write_desc (name, desc)
	desc = json.load (open (os.path.join (DESC_DIR, name + '.json')))
	roads = build (name, desc, pads (cfgpath), os.path.join (meshdir, 'BaseKit', name + '_ground.msh'), buildings (cfgpath))
	write_lights (cfgpath, roads, desc)

# ======================================================================

def check (name, cfgs):
	P = pads (cfgs[0])
	tris = 0
	for mat, tex, V, T in read_msh (os.path.join (MESH_DIR, name + '_ground.msh')):
		for t in T:
			c = (sum (V[j][0] for j in t) / 3, sum (V[j][2] for j in t) / 3)
			if inside_any (c, [o for o, _, _ in P]): tris += 1
	return tris

def main ():
	a = sys.argv[1:]
	if not a or a[0] not in ('extract', 'build', 'check', 'berms'): print (__doc__); sys.exit (1)
	cfgs = base_cfgs ()
	names = a[1:] or sorted (cfgs)
	for n in names:
		if n not in cfgs: print ('%s: no base with its ground' % n); continue
		if a[0] == 'berms':
			print ('%-16s %s' % (n, ', '.join ('%d berms turned' % orient_berms (c) for c in cfgs[n])))
			continue
		if a[0] == 'extract':
			d = extract (n)
			write_desc (n, d)
			print ('%-16s %3d strips, %4d polygons' % (n, len (d['strips']), len (d['polys'])))
		elif a[0] == 'build':
			dp = os.path.join (DESC_DIR, n + '.json')
			if not os.path.exists (dp): print ('%s: no description (ground.py extract)' % n); continue
			desc = json.load (open (dp))
			roads = build (n, desc, pads (cfgs[n][0]), os.path.join (MESH_DIR, n + '_ground.msh'), buildings (cfgs[n][0]))
			nl, nf = write_lights (cfgs[n][0], roads, desc)
			print ('%-16s built, %d triangles on pads; %d lights, %d floodlights' % (n, check (n, cfgs[n]), nl, nf))
		else:
			print ('%-16s %d triangles on pads' % (n, check (n, cfgs[n])))

if __name__ == '__main__':
	main ()
