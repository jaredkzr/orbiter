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

def keepouts (P, half_w):
	"""where roads of half width half_w must not go: about each pad (and its
	berm), grown by the road's half width; overlapping ones as one (their hull)"""
	ks = []
	for outline, (px, pz), berm in P:
		if berm:
			r = (BERM_R + half_w + CLEAR) / math.cos (math.pi / 16)  # (16-gon about the circle)
			ks.append ([(px + r * math.cos (2 * math.pi * k / 16), pz + r * math.sin (2 * math.pi * k / 16)) for k in range (16)])
		else:
			g = half_w + CLEAR
			# (the outline grown: each corner moved out along its bisector)
			n = len (outline)
			grown = []
			for i in range (n):
				a, b, c = outline[i - 1], outline[i], outline[(i + 1) % n]
				def nrm (p, q):
					l = math.hypot (q[0] - p[0], q[1] - p[1])
					return ((q[1] - p[1]) / l, -(q[0] - p[0]) / l)
				n1, n2 = nrm (a, b), nrm (b, c)
				m = (n1[0] + n2[0], n1[1] + n2[1])
				k = g / max (0.2, (1 + n1[0] * n2[0] + n1[1] * n2[1]))
				grown.append ((b[0] + m[0] * k, b[1] + m[1] * k))
			ks.append (grown)
	# (merged while any overlap)
	merged = True
	while merged:
		merged = False
		for i in range (len (ks)):
			for j in range (i + 1, len (ks)):
				if overlaps (ks[i], ks[j]):
					ks[i] = hull (ks[i] + ks[j]); del ks[j]; merged = True
					break
			if merged: break
	return [ccw (k) for k in ks]

# ======================================================================
# Roads

def route (pts, K):
	"""a road (a polyline) around the keep-outs K: where it crosses one, along
	its edge the shorter way; where it starts or ends in one, from or to its
	edge. Polylines (a part wholly in one: left out)"""
	lines, cur = [], []
	a, i, guard = tuple (pts[0]), 0, 0
	while i < len (pts) - 1 and guard < 10000:
		guard += 1
		b = tuple (pts[i + 1])
		hit = None
		for P in K:
			r = seg_poly (a, b, P)
			if r and (hit is None or r[0] < hit[1][0]): hit = (P, r)
		if not hit:
			if not cur: cur = [a]
			cur.append (b); a = b; i += 1
			continue
		P, (t0, t1, e0, e1) = hit
		at = lambda t: (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
		E, X = at (t0), at (t1)
		if e0 < 0:  # (from inside: on from where it leaves)
			if len (cur) >= 2: lines.append (cur)
			cur = []
			if e1 < 0: a = b; i += 1
			else: a = X
			continue
		if not cur: cur = [a]
		if e1 < 0:  # (into it: up to its edge)
			cur.append (E); lines.append (cur); cur = []
			a = b; i += 1
			continue
		n = len (P)
		fwd = [P[(e0 + 1 + k) % n] for k in range ((e1 - e0) % n)]
		bwd = [P[(e0 - k) % n] for k in range ((e0 - e1) % n)]
		L = lambda path: sum (math.hypot (q[0] - p[0], q[1] - p[1]) for p, q in zip ([E] + path, path + [X]))
		cur += [E] + (fwd if L (fwd) <= L (bwd) else bwd) + [X]
		a = X
	if len (cur) >= 2: lines.append (cur)
	out = []
	for line in lines:  # (repeated and collinear points out)
		clean = []
		for p in line:
			if clean and math.hypot (p[0] - clean[-1][0], p[1] - clean[-1][1]) < 0.05: continue
			clean.append (p)
		k = 1
		while k < len (clean) - 1:
			p, q, r = clean[k - 1], clean[k], clean[k + 1]
			if abs ((q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])) < 1e-3 * math.hypot (r[0] - p[0], r[1] - p[1]): del clean[k]
			else: k += 1
		if len (clean) >= 2 and sum (math.hypot (q[0] - p[0], q[1] - p[1]) for p, q in zip (clean, clean[1:])) > 1.0: out.append (clean)
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

def build (name, desc, P, path):
	"""the ground mesh from its description, clear of the pads P"""
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
	roads = []
	for s in desc['strips']:
		hw = s['w'] / 2
		for pts in route ([tuple (p) for p in s['pts']], keepouts (P, hw)):
			strip (pts, hw, s)
			roads.append ((pts, s))
	for p in desc['polys']:
		for Q in clear_of_pads ([tuple (q) for q in p['pts']]):
			poly (Q, p['y'], p['mat'], p['tex'])
	B.write (path)
	return roads

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
	roads = build (name, desc, pads (cfgpath), os.path.join (meshdir, 'BaseKit', name + '_ground.msh'))
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
	if not a or a[0] not in ('extract', 'build', 'check'): print (__doc__); sys.exit (1)
	cfgs = base_cfgs ()
	names = a[1:] or sorted (cfgs)
	for n in names:
		if n not in cfgs: print ('%s: no base with its ground' % n); continue
		if a[0] == 'extract':
			d = extract (n)
			write_desc (n, d)
			print ('%-16s %3d strips, %4d polygons' % (n, len (d['strips']), len (d['polys'])))
		elif a[0] == 'build':
			dp = os.path.join (DESC_DIR, n + '.json')
			if not os.path.exists (dp): print ('%s: no description (ground.py extract)' % n); continue
			desc = json.load (open (dp))
			roads = build (n, desc, pads (cfgs[n][0]), os.path.join (MESH_DIR, n + '_ground.msh'))
			nl, nf = write_lights (cfgs[n][0], roads, desc)
			print ('%-16s built, %d triangles on pads; %d lights, %d floodlights' % (n, check (n, cfgs[n]), nl, nf))
		else:
			print ('%-16s %d triangles on pads' % (n, check (n, cfgs[n])))

if __name__ == '__main__':
	main ()
