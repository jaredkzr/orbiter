# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Tools of the base kits' Blender builders (Utils/BaseKit/blender_kit.py;
run inside Blender: blender -b --python <script> -- <args>).

An asset is built from parts: bmesh shapes, each placed in Orbiter's frame
(x, y up, z; metres; the asset's origin on the ground at its centre, +z its
front) and given a kit material (kittex.py's tiling textures, a tint) and a
role:
  - drawn (the default): rendered, and its triangles are collision hulls
  - detail: rendered, left out of the collisions (MESHGRP_NOCOLLIDE: rails,
    lattice, antennas, pipes)
  - proxy: a collision shape only, never drawn (MESHGRP_COLLISION | hidden)
Export (write) merges the parts by (material, role) into mesh groups, maps
their texture in world units (planar by the face's normal, or cylindrical
about a part's axis), smooths normals across edges under an angle, and
writes the .msh (Utils/BlenderMsh/orbiter_msh.py's file format).

Lights (aviation reds, door lamps) are recorded per asset for the bases'
layouts (Config/VK/Bases LIGHT lines).
"""

import math, os, sys
import bpy, bmesh
from mathutils import Vector, Matrix

HERE = os.path.dirname (os.path.abspath (__file__))
ROOT = os.path.normpath (os.path.join (HERE, '..', '..'))
sys.path.insert (0, os.path.join (ROOT, 'Utils', 'BlenderMsh'))
sys.path.insert (0, HERE)
import orbiter_msh as om
import kittex

MESHGRP_NOCOLLIDE, MESHGRP_COLLISION, GRP_HIDDEN = 0x100, 0x200, 0x2

# kit materials: texture (Textures/BaseKit/<name>.dds; None: untextured), tint
# (diffuse; multiplies the texture), specular power (legacy; the maps set the
# PBR look), emissive
class KitMat:
	def __init__ (self, name, tex, tint=(1, 1, 1), power=20.0, emissive=(0, 0, 0), tile=None):
		self.name, self.tex, self.tint, self.power, self.emissive = name, tex, tint, power, emissive
		self.tile = tile or (kittex.MATERIALS[tex] if tex else 4.0)


class Part:
	def __init__ (self, bm, mat, role='drawn', uv='planar', axis=None, centre=None):
		self.bm, self.mat, self.role, self.uv = bm, mat, role, uv
		self.axis, self.centre = axis, centre  # (cylindrical mapping: axis direction and a point on it, Orbiter frame)


class Asset:
	"""an asset under construction: parts, lights"""
	def __init__ (self, name):
		self.name, self.parts, self.lights = name, [], []

	def add (self, bm, mat, role='drawn', uv='planar', axis=None, centre=None):
		self.parts.append (Part (bm, mat, role, uv, axis, centre))
		return bm

	def light (self, pos, col, size=1.0):
		self.lights.append ((tuple (pos), tuple (col), size))

	def triangles (self, role=None):
		return sum (len (p.bm.faces) for p in self.parts if role is None or p.role == role)


# ----------------------------------------------------------------------
# shapes (bmesh, in Orbiter's frame: x, y up, z)

def _bm ():
	return bmesh.new ()

def transform (bm, M):
	bmesh.ops.transform (bm, matrix=M, verts=bm.verts)
	return bm

def place (bm, pos=(0, 0, 0), rot_y=0.0, rot_x=0.0, rot_z=0.0):
	"""rotate (degrees: about z, then x, then y - y the vertical) and move"""
	M = Matrix.Translation (Vector (pos)) @ Matrix.Rotation (math.radians (rot_y), 4, 'Y') \
		@ Matrix.Rotation (math.radians (rot_x), 4, 'X') @ Matrix.Rotation (math.radians (rot_z), 4, 'Z')
	return transform (bm, M)

def box (sx, sy, sz, pos=(0, 0, 0), rot_y=0.0, bevel=0.0, bottom=True):
	"""a box sx x sy x sz (y up), its base centred at pos; bevel: its edges [m]"""
	bm = _bm ()
	bmesh.ops.create_cube (bm, size=1.0)
	transform (bm, Matrix.Diagonal (Vector ((sx, sy, sz, 1.0))))
	transform (bm, Matrix.Translation (Vector ((0, sy / 2, 0))))
	if not bottom:
		for f in [f for f in bm.faces if f.normal.y < -0.9]: bm.faces.remove (f)
	if bevel > 0:
		bmesh.ops.bevel (bm, geom=bm.edges[:], offset=bevel, segments=2, affect='EDGES', profile=0.5)
	return place (bm, pos, rot_y)

def cylinder (r, h, n=24, pos=(0, 0, 0), axis='y', caps=(True, True), r2=None, rot_y=0.0, smooth=True):
	"""a cylinder (cone with r2) of radius r, length h along an axis from pos;
	axis 'y' (up), 'x' or 'z'"""
	bm = _bm ()
	bmesh.ops.create_cone (bm, cap_ends=True, cap_tris=False, segments=n, radius1=r, radius2=r if r2 is None else r2, depth=h)
	# (created along Blender's z = Orbiter's... here the bmesh frame is Orbiter's: along z; turned to the axis)
	transform (bm, Matrix.Translation (Vector ((0, 0, h / 2))))
	if not caps[0]:
		for f in [f for f in bm.faces if f.normal.z < -0.9]: bm.faces.remove (f)
	if not caps[1]:
		for f in [f for f in bm.faces if f.normal.z > 0.9]: bm.faces.remove (f)
	if axis == 'y': transform (bm, Matrix.Rotation (math.radians (-90), 4, 'X'))
	elif axis == 'x': transform (bm, Matrix.Rotation (math.radians (90), 4, 'Y'))
	return place (bm, pos, rot_y)

def sphere (r, pos=(0, 0, 0), segments=32, rings=16, cut=None):
	"""a sphere; cut: keep only y >= cut [m] (relative to its centre: a dome)"""
	bm = _bm ()
	bmesh.ops.create_uvsphere (bm, u_segments=segments, v_segments=rings, radius=r)
	if cut is not None:
		bmesh.ops.bisect_plane (bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, cut, 0), plane_no=(0, 1, 0), clear_inner=True)
	return place (bm, pos)

def half_cylinder (r, length, n=16, pos=(0, 0, 0), rot_y=0.0, ends=True):
	"""a half cylinder (barrel vault) along z, its flat side on the ground"""
	bm = _bm ()
	verts_f, verts_b = [], []
	for i in range (n + 1):
		a = math.pi * i / n
		x, y = r * math.cos (a), r * math.sin (a)
		verts_f.append (bm.verts.new ((x, y, -length / 2)))
		verts_b.append (bm.verts.new ((x, y, length / 2)))
	for i in range (n):
		bm.faces.new ((verts_f[i], verts_f[i + 1], verts_b[i + 1], verts_b[i]))
	if ends:
		bm.faces.new (verts_f[::-1])
		bm.faces.new (verts_b)
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	return place (bm, pos, rot_y)

def merge (*bms):
	out = _bm ()
	for b in bms:
		me = bpy.data.meshes.new ('tmp')
		b.to_mesh (me)
		out.from_mesh (me)
		bpy.data.meshes.remove (me)
	return out

def ring_of (n, radius, make, start=0.0):
	"""make(i, x, z, angle_deg) for n points about a circle; their bmeshes merged"""
	parts = []
	for i in range (n):
		a = start + 360.0 * i / n
		parts.append (make (i, radius * math.sin (math.radians (a)), radius * math.cos (math.radians (a)), a))
	return merge (*parts)

def truss (p0, p1, w, d, step, rod=0.06):
	"""a lattice girder from p0 to p1 (a rectangular section w x d about the
	line, its chords and zig-zag diagonals as thin boxes): detail"""
	p0, p1 = Vector (p0), Vector (p1)
	L = (p1 - p0).length
	ax = (p1 - p0).normalized ()
	up = Vector ((0, 1, 0)) if abs (ax.y) < 0.9 else Vector ((1, 0, 0))
	s1 = ax.cross (up).normalized (); s2 = ax.cross (s1).normalized ()
	corners = [s1 * (w / 2) + s2 * (d / 2), s1 * (-w / 2) + s2 * (d / 2), s1 * (-w / 2) + s2 * (-d / 2), s1 * (w / 2) + s2 * (-d / 2)]
	bms = []
	for c in corners: bms.append (rod_between (p0 + c, p1 + c, rod))
	n = max (1, int (round (L / step)))
	for k in range (n):
		a, b = p0 + ax * (L * k / n), p0 + ax * (L * (k + 1) / n)
		for i in range (4):
			c0, c1 = corners[i], corners[(i + 1) % 4]
			bms.append (rod_between (a + (c0 if k % 2 == 0 else c1), b + (c1 if k % 2 == 0 else c0), rod * 0.7))
	return merge (*bms)

def rod_between (a, b, r, n=4):
	"""a thin prism of n sides (radius r) from a to b"""
	a, b = Vector (a), Vector (b)
	d = b - a
	L = d.length
	bm = cylinder (r, L, n=n, axis='z', smooth=False)
	z = Vector ((0, 0, 1))
	q = z.rotation_difference (d.normalized ())
	transform (bm, Matrix.Translation (a) @ q.to_matrix ().to_4x4 ())
	return bm


# ----------------------------------------------------------------------
# export

def _uv (p, n, part):
	if part.mat is None: return (0.0, 0.0)  # (a proxy)
	T = part.mat.tile
	if part.uv == 'cyl' and part.axis is not None:
		ax = Vector (part.axis).normalized (); c = Vector (part.centre)
		d = p - c
		h = d.dot (ax)
		rv = d - ax * h
		ref = Vector ((1, 0, 0)) if abs (ax.x) < 0.9 else Vector ((0, 0, 1))
		e1 = (ref - ax * ref.dot (ax)).normalized (); e2 = ax.cross (e1)
		if abs (n.dot (ax)) > 0.7:  # (the ends: planar)
			return (rv.dot (e1) / T, -rv.dot (e2) / T)
		a = math.atan2 (rv.dot (e2), rv.dot (e1))
		r = max (rv.length, 1e-6)
		# (the arc in metres, rounded to whole tiles about the circumference: no seam)
		circ = 2 * math.pi * r
		k = max (1, round (circ / T))
		return (a / (2 * math.pi) * k, -h / T)
	# planar: horizontal faces x, z; others along the horizontal and up
	if abs (n.y) > 0.7: return (p.x / T, p.z / T)
	t = Vector ((-n.z, 0, n.x)).normalized () if Vector ((n.x, 0, n.z)).length > 1e-6 else Vector ((1, 0, 0))
	return (p.dot (t) / T, -p.y / T)

def _part_triangles (part, smooth_deg):
	"""(vertices (x,y,z,nx,ny,nz,u,v), triangles) of a part, normals smoothed
	across edges whose faces meet under smooth_deg"""
	bm = part.bm.copy ()
	bmesh.ops.triangulate (bm, faces=bm.faces[:])
	bm.normal_update ()
	cos_lim = math.cos (math.radians (smooth_deg))
	vtx, tri, key = [], [], {}
	for f in bm.faces:
		fn = f.normal.copy ()
		ids = []
		for v in f.verts:
			acc = Vector ((0, 0, 0))
			for g in v.link_faces:
				if g.normal.dot (fn) >= cos_lim: acc += g.normal * g.calc_area ()
			n = acc.normalized () if acc.length > 1e-12 else fn
			uv = _uv (v.co, fn, part)
			k = (round (v.co.x, 4), round (v.co.y, 4), round (v.co.z, 4), round (n.x, 3), round (n.y, 3), round (n.z, 3), round (uv[0], 4), round (uv[1], 4))
			if k not in key:
				key[k] = len (vtx)
				vtx.append ((v.co.x, v.co.y, v.co.z, n.x, n.y, n.z, uv[0], uv[1]))
			ids.append (key[k])
		# (the parts are built in Orbiter's frame itself: bmesh's order - counter-clockwise
		# about the outward normal in right-handed terms - is Orbiter's clockwise front in its
		# left-handed one; no axis swap, so no reversal, unlike orbiter_msh's import)
		tri.append ((ids[0], ids[1], ids[2]))
	bm.free ()
	return vtx, tri

def write (asset, path, smooth_deg=35.0):
	"""the asset's .msh: groups by (material, role), in a stable order"""
	m = om.MeshFile ()
	mats, texs, groups = {}, {}, {}
	order = []
	for part in asset.parts:
		key = (part.mat.name if part.role != 'proxy' else '_proxy', part.role)
		if key not in groups:
			groups[key] = ([], [])
			order.append ((key, part))
		vtx, tri = _part_triangles (part, smooth_deg if part.role != 'proxy' else 0.0)
		gv, gt = groups[key]
		base = len (gv)
		gv.extend (vtx)
		gt.extend ((a + base, b + base, c + base) for a, b, c in tri)
	for (key, part) in order:
		name, role = key
		gv, gt = groups[key]
		if len (gv) > 65535: raise SystemExit ('%s: group %s %s: %d vertices' % (asset.name, name, role, len (gv)))
		g = om.Group ()
		g.label = ('%s_%s' % (name, role)) if role != 'drawn' else name
		g.vtx, g.tri = gv, gt
		if role == 'proxy':
			g.flag = MESHGRP_COLLISION | GRP_HIDDEN
			g.mat, g.tex = -1, -1
		else:
			g.flag = MESHGRP_NOCOLLIDE if role == 'detail' else 0
			km = part.mat
			if km.name not in mats:
				mt = om.Material (km.name)
				mt.diffuse = list (km.tint) + [1.0]
				mt.ambient = list (km.tint) + [1.0]
				mt.specular = [0.2, 0.2, 0.2, 1.0]
				mt.power = km.power
				mt.emissive = list (km.emissive) + [1.0]
				mats[km.name] = len (m.materials); m.materials.append (mt)
			g.mat = mats[km.name]
			if km.tex:
				tn = 'BaseKit\\%s.dds' % km.tex
				if tn not in texs: texs[tn] = len (m.textures); m.textures.append ((tn, ''))
				g.tex = texs[tn]
			else: g.tex = -1
		m.groups.append (g)
	os.makedirs (os.path.dirname (path), exist_ok=True)
	om.write_msh (path, m)
	return sum (len (g.tri) for g in m.groups if not (g.flag & GRP_HIDDEN))
