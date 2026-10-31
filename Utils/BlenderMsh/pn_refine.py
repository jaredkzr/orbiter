# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# A mesh made smoother by its own vertex normals: each triangle becomes a
# curved patch (point-normal triangles: Vlachos et al., "Curved PN
# Triangles", 2001) tessellated into n x n triangles. Faithful to the
# original: the patch passes through the triangle's corners with the normals
# given there, so surfaces round off the way their shading already showed.
# - Edges whose triangles disagree about the normals at its ends (hard
#   edges: creases, the rims of nozzles, wing edges) stay straight: the
#   surfaces either side meet without cracks.
# - Smooth edges curve the same way for every triangle that shares them
#   (the curve depends only on the edge's ends and normals), also between
#   groups.
# - Texture coordinates are interpolated linearly: the textures' layouts,
#   and skins made for them, still fit.
# Usage:
#   python3 Utils/BlenderMsh/pn_refine.py <in.msh> <out.msh> [--level n] [--groups a,b,...] [--skip a,b,...]

import os, sys, math, argparse
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import orbiter_msh as om

def _sub (a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _add (a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def _mul (a, s): return (a[0] * s, a[1] * s, a[2] * s)
def _dot (a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _unit (a):
	l = math.sqrt (_dot (a, a))
	return _mul (a, 1.0 / l) if l > 1e-12 else a

def _key (p): return (round (p[0], 4), round (p[1], 4), round (p[2], 4))

def hard_edges (m, groups, cos_tol=0.999):
	"""edges (pairs of position keys) whose triangles disagree about the
	normals at the ends, or that have only one triangle with another
	triangle folding sharply onto it"""
	uses = {}
	for gi in groups:
		g = m.groups[gi]
		for t in g.tri:
			if len (set (t)) < 3: continue
			for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
				va, vb = g.vtx[a], g.vtx[b]
				ka, kb = _key (va), _key (vb)
				e = (ka, kb) if ka < kb else (kb, ka)
				na, nb = (va[3:6], vb[3:6]) if ka < kb else (vb[3:6], va[3:6])
				uses.setdefault (e, []).append ((na, nb))
	hard = set ()
	for e, ns in uses.items ():
		n0a, n0b = ns[0]
		for na, nb in ns[1:]:
			if _dot (na, n0a) < cos_tol or _dot (nb, n0b) < cos_tol:
				hard.add (e); break
	return hard

def refine_group (g, n, hard):
	"""the group's triangles as PN patches, n x n triangles each"""
	out = om.Group ()
	for f in ("label", "mat", "tex", "zbias", "texwrap", "flag", "static", "comment"):
		setattr (out, f, getattr (g, f))
	index = {}
	def vertex (p, nrm, uv):
		nrm = _unit (nrm)
		k = (round (p[0], 6), round (p[1], 6), round (p[2], 6), round (nrm[0], 4), round (nrm[1], 4), round (nrm[2], 4), round (uv[0], 6), round (uv[1], 6))
		i = index.get (k)
		if i is None:
			i = index[k] = len (out.vtx)
			out.vtx.append ((p[0], p[1], p[2], nrm[0], nrm[1], nrm[2], uv[0], uv[1]))
		return i
	for t in g.tri:
		if len (set (t)) < 3 or max (t) >= len (g.vtx): continue
		V = [g.vtx[i] for i in t]
		P = [v[:3] for v in V]
		N = [_unit (v[3:6]) if v[3] is not None else None for v in V]
		if None in N or _dot (_sub (P[1], P[0]), _sub (P[1], P[0])) < 1e-12:
			# (no normals: copied as it is)
			out.tri.append (tuple (vertex (v[:3], v[3:6] if v[3] is not None else (0, 1, 0), v[6:8]) for v in V))
			continue
		# edge control points (straight on hard edges)
		def edge (i, j):
			ki, kj = _key (P[i]), _key (P[j])
			e = (ki, kj) if ki < kj else (kj, ki)
			if e in hard:
				return _add (P[i], _mul (_sub (P[j], P[i]), 1 / 3)), _add (P[i], _mul (_sub (P[j], P[i]), 2 / 3))
			w_ij = _dot (_sub (P[j], P[i]), N[i])
			w_ji = _dot (_sub (P[i], P[j]), N[j])
			b_ij = _mul (_sub (_add (_mul (P[i], 2), P[j]), _mul (N[i], w_ij)), 1 / 3)
			b_ji = _mul (_sub (_add (_mul (P[j], 2), P[i]), _mul (N[j], w_ji)), 1 / 3)
			return b_ij, b_ji
		b210, b120 = edge (0, 1)
		b021, b012 = edge (1, 2)
		b102, b201 = edge (2, 0)
		E = _mul (_add (_add (_add (b210, b120), _add (b021, b012)), _add (b102, b201)), 1 / 6)
		Vc = _mul (_add (_add (P[0], P[1]), P[2]), 1 / 3)
		b111 = _add (E, _mul (_sub (E, Vc), 0.5))
		def point (u, v, w):   # (w: weight of corner 0, u: corner 1, v: corner 2)
			p = _mul (P[0], w ** 3)
			p = _add (p, _mul (P[1], u ** 3)); p = _add (p, _mul (P[2], v ** 3))
			p = _add (p, _mul (b210, 3 * w * w * u)); p = _add (p, _mul (b120, 3 * w * u * u))
			p = _add (p, _mul (b201, 3 * w * w * v)); p = _add (p, _mul (b021, 3 * u * u * v))
			p = _add (p, _mul (b102, 3 * w * v * v)); p = _add (p, _mul (b012, 3 * u * v * v))
			p = _add (p, _mul (b111, 6 * w * u * v))
			nrm = _add (_add (_mul (N[0], w), _mul (N[1], u)), _mul (N[2], v))
			uv = (V[0][6] * w + V[1][6] * u + V[2][6] * v, V[0][7] * w + V[1][7] * u + V[2][7] * v)
			return vertex (p, nrm, uv)
		grid = {}
		for i in range (n + 1):
			for j in range (n + 1 - i):
				grid[(i, j)] = point (i / n, j / n, 1.0 - (i + j) / n)
		for i in range (n):
			for j in range (n - i):
				out.tri.append ((grid[(i, j)], grid[(i + 1, j)], grid[(i, j + 1)]))
				if i + j < n - 1:
					out.tri.append ((grid[(i + 1, j)], grid[(i + 1, j + 1)], grid[(i, j + 1)]))
	return out

def refine (m, n, only=None, skip=()):
	sel = [k for k, g in enumerate (m.groups) if (only is None or g.label in only or str (k) in only) and g.label not in skip and str (k) not in skip]
	hard = hard_edges (m, range (len (m.groups)))
	for k in sel: m.groups[k] = refine_group (m.groups[k], n, hard)
	return m

if __name__ == "__main__":
	ap = argparse.ArgumentParser ()
	ap.add_argument ("src"); ap.add_argument ("dst")
	ap.add_argument ("--level", type=int, default=3)
	ap.add_argument ("--groups"); ap.add_argument ("--skip", default="")
	a = ap.parse_args ()
	m = om.read_msh (a.src)
	before = sum (len (g.tri) for g in m.groups)
	refine (m, a.level, set (a.groups.split (",")) if a.groups else None, set (filter (None, a.skip.split (","))))
	om.write_msh (a.dst, m)
	print ("%s: %d -> %d triangles" % (a.dst, before, sum (len (g.tri) for g in m.groups)))
