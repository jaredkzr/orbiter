# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""The base kits' meshes, built in Blender (in the background):

    blender -b --python Utils/BaseKit/blender_kit.py -- <offworld|earth|cape|trains> <asset|all> [--out ROOT] [--check]

Each asset keeps the file name, origin (on the ground at its centre), front
(+z) and footprint of the mesh it replaces (Utils/BaseKit/footprints.json:
--check runs Utils/BlenderMsh/msh_check.py --bounds-json on it), so the
bases' layouts (basekit.py, offworld.py) stand as they are. Textures: the
kits' tiling materials (kittex.py), in world units. Collision: proxy shapes
where the drawn ones are detailed (kitlib).

Writes Meshes/BaseKit/<asset>.msh, and the assets' lights into
Utils/BaseKit/kit_lights.json (for the layouts' LIGHT lines).
"""

import json, math, os, subprocess, sys
import bmesh

HERE = os.path.dirname (os.path.abspath (__file__))
sys.path.insert (0, HERE)
import kitlib as kl
from kitlib import KitMat, Asset, box, cylinder, sphere, half_cylinder, merge, place, rod_between, truss

ROOT = kl.ROOT

# ----------------------------------------------------------------------
# bodies (offworld.py BODIES: their soil colour and what their bases are like)

BODIES = {
	"Moon":     dict (soil=(0.40, 0.39, 0.37), berms=True,  solar=True,  legs=False, shades=False, insul=False),
	"Mars":     dict (soil=(0.52, 0.32, 0.20), berms=True,  solar=True,  legs=False, shades=False, insul=False),
	"Mercury":  dict (soil=(0.34, 0.33, 0.32), berms=True,  solar=True,  legs=False, shades=True,  insul=False),
	"Titan":    dict (soil=(0.42, 0.34, 0.22), berms=False, solar=False, legs=False, shades=False, insul=True),
	"Europa":   dict (soil=(0.78, 0.75, 0.70), berms=False, solar=False, legs=True,  shades=False, insul=False),
	"Callisto": dict (soil=(0.28, 0.26, 0.24), berms=True,  solar=False, legs=False, shades=False, insul=False),
	"Ceres":    dict (soil=(0.22, 0.21, 0.20), berms=True,  solar=True,  legs=False, shades=False, insul=False),
}

# kit materials
HULL = KitMat ('hull', 'kit_panel', (0.92, 0.92, 0.90))
HULL_DARK = KitMat ('hull_dark', 'kit_panel', (0.42, 0.43, 0.45))
INSUL = KitMat ('insulation', 'kit_panel', (0.86, 0.56, 0.24))
MLI = KitMat ('mli', 'kit_mli')
STEEL = KitMat ('steel', 'kit_steel')
GRATING = KitMat ('grating', 'kit_grating')
GLASS = KitMat ('glass', 'kit_glass', power=60)
HAZARD = KitMat ('hazard', 'kit_hazard')
SOLAR = KitMat ('solar', 'kit_solar', power=60)
RADIATOR = KitMat ('radiator', 'kit_radiator')
LAMP = KitMat ('lamp', None, (1.0, 0.95, 0.85), emissive=(1.0, 0.9, 0.7))

def soil (body):
	return KitMat ('soil_' + body.lower (), 'kit_regolith', tuple (min (1.0, c / 0.62 * 0.95) for c in BODIES[body]['soil']))

RED, WHITE = (1.0, 0.12, 0.08), (1.0, 0.9, 0.7)

# ----------------------------------------------------------------------
# parts shared by the assets

def berm (A, body, w, d, h, z0=0.0, n=40, rings=14, lumps=0.25, seed=1):
	"""a regolith berm over modules: half an ellipsoid w x d x h, lumpy, a
	sandbag ring about its foot"""
	import random
	rnd = random.Random (seed)
	bm = sphere (1.0, segments=n, rings=rings, cut=0.0)
	for v in bm.verts:
		x, y, z = v.co
		k = 1.0 + lumps / max (h, 1) * (math.sin (7.1 * x + 3.0 * z + seed) * 0.5 + math.sin (11.3 * z - 5.2 * x) * 0.5) * y
		v.co = (x * w / 2 * k, y * h * k, z * d / 2 * k + z0)
	A.add (bm, soil (body))
	ring = cylinder (1.0, 0.8, n=n, caps=(False, True))
	for v in ring.verts: v.co = (v.co.x * (w / 2 + 0.4), v.co.y, v.co.z * (d / 2 + 0.4) + z0)
	A.add (ring, soil (body))

def hatch (A, pos, rot_y=0.0, w=1.6, h=2.2, frame=0.2):
	"""a door in a frame, hazard-striped sill, a lamp over it"""
	x, y, z = pos
	A.add (box (w + 2 * frame, h + frame, 0.25, (x, y, z), rot_y), STEEL)
	ox, oz = math.sin (math.radians (rot_y)) * 0.08, math.cos (math.radians (rot_y)) * 0.08
	A.add (box (w, h, 0.12, (x + ox, y, z + oz), rot_y), HULL_DARK)  # (the door, standing out of the frame a little)
	A.add (box (w + 2 * frame, 0.12, 0.4, (x, y, z), rot_y), HAZARD)
	lx = x + math.sin (math.radians (rot_y)) * 0.2; lz = z + math.cos (math.radians (rot_y)) * 0.2
	A.add (box (0.5, 0.12, 0.25, (lx, y + h + frame + 0.1, lz), rot_y), LAMP, role='detail')
	A.light ((lx, y + h + frame + 0.3, lz), WHITE, 1.0)

def handrail (A, p0, p1, h=1.0, posts=4):
	from mathutils import Vector
	p0, p1 = Vector (p0), Vector (p1)
	parts = [rod_between (p0 + Vector ((0, h, 0)), p1 + Vector ((0, h, 0)), 0.03, 6)]
	for k in range (posts + 1):
		p = p0 + (p1 - p0) * (k / posts)
		parts.append (rod_between (p, p + Vector ((0, h, 0)), 0.03, 6))
	A.add (merge (*parts), STEEL, role='detail')

def mast (A, pos, h, light=True):
	x, y, z = pos
	A.add (truss ((x, y, z), (x, y + h, z), 0.45, 0.45, 0.9, 0.035), STEEL, role='detail')
	A.add (cylinder (0.5, 0.08, 16, (x, y + h, z)), STEEL, role='detail')
	A.add (cylinder (0.06, 1.2, 6, (x, y + h, z)), STEEL, role='detail')
	if light:
		A.add (sphere (0.12, (x, y + h + 1.25, z), 8, 6), KitMat ('beacon', None, RED, emissive=RED), role='detail')
		A.light ((x, y + h + 1.3, z), RED, 1.2)

def module (A, x, z, L, r, y, mat, along_x=False, windows=False, mli_bands=True):
	"""a cylindrical module on cradles (axis along z, or x), domed ends"""
	axis = 'x' if along_x else 'z'
	c = (x - L / 2, y + r, z) if along_x else (x, y + r, z - L / 2)
	A.add (cylinder (r, L, 32, c, axis=axis, caps=(False, False)), mat, uv='cyl',
		axis=(1, 0, 0) if along_x else (0, 0, 1), centre=(x, y + r, z))
	for s in (-1, 1):
		e = (x + s * L / 2, y + r, z) if along_x else (x, y + r, z + s * L / 2)
		cap = sphere (r, (0, 0, 0), 32, 16, cut=0.0)
		for v in cap.verts: v.co = (v.co.x, v.co.y * 1.0 / r, v.co.z)  # (a shallow dome 1 m deep, up)
		from mathutils import Matrix
		# (turned to point out of the module's end: up to +-x about z, up to +-z about x)
		rot = Matrix.Rotation (math.radians (-90 * s), 4, 'Z') if along_x else Matrix.Rotation (math.radians (90 * s), 4, 'X')
		kl.transform (cap, Matrix.Translation (e) @ rot)
		A.add (cap, HULL_DARK)
	if mli_bands:
		for f in (-0.33, 0.0, 0.33):
			bc = (x + f * L - 0.3, y + r, z) if along_x else (x, y + r, z + f * L - 0.3)
			A.add (cylinder (r + 0.03, 0.6, 32, bc, axis=axis, caps=(False, False)), MLI, uv='cyl',
				axis=(1, 0, 0) if along_x else (0, 0, 1), centre=(x, y + r, z))
	for s in (-0.35, 0.35):
		cx, cz = (x + s * L, z) if along_x else (x, z + s * L)
		A.add (box (0.8 if along_x else r * 1.6, y + r * 0.4, r * 1.6 if along_x else 0.8, (cx, 0, cz)), STEEL)
	if windows:
		for sd in (-1, 1):
			for i in range (3):
				cc = -L / 3 + i * L / 3
				p = (x + cc, y + r + 0.8, z + sd * (r + 0.02)) if along_x else (x + sd * (r + 0.02), y + r + 0.8, z + cc)
				A.add (box (2.4 if along_x else 0.1, 0.9, 0.1 if along_x else 2.4, p), GLASS)

# ----------------------------------------------------------------------
# off-world kit (offworld.py's meshes, rebuilt)

def a_hab (body):
	"""habitat: two modules side by side and a node, under a regolith berm
	(vacuum), on legs (ice), or insulated"""
	p = BODIES[body]
	A = Asset ('space_hab_' + body.lower ())
	mat = INSUL if p['insul'] else HULL
	y = 3.0 if p['legs'] else 0.6
	if not p['berms']:  # (under a berm: not seen)
		module (A, -4.5, 0, 12.0, 3.2, y, mat, windows=True, mli_bands=not p['insul'])
		module (A, 4.5, 0, 12.0, 3.2, y, mat, windows=True, mli_bands=not p['insul'])
	# the node: an airlock at the front, its hatch, a vestibule and steps
	A.add (cylinder (3.0, 5.5, 32, (0, y, 9.5)), HULL_DARK if not p['insul'] else INSUL, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 9.5))
	A.add (cylinder (3.08, 0.5, 32, (0, y + 4.2, 9.5), caps=(False, False)), MLI, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 9.5))
	A.add (cylinder (3.2, 0.25, 32, (0, y + 5.5, 9.5)), STEEL)
	A.add (box (3.0, y + 0.1, 1.6, (0, 0, 12.5)), STEEL)  # (the vestibule's floor)
	A.add (box (3.0, 0.02, 1.6, (0, y + 0.1, 12.5)), GRATING, role='detail')
	hatch (A, (0, y + 0.1, 12.48))
	handrail (A, (-1.45, y + 0.1, 11.7), (-1.45, y + 0.1, 13.3), posts=2)
	handrail (A, (1.45, y + 0.1, 11.7), (1.45, y + 0.1, 13.3), posts=2)
	A.add (place (cylinder (0.25, 1.8, 12, (0, 0, 0), axis='z'), (2.0, y + 4.8, 8.0)), STEEL, role='detail')  # (a vent)
	if p['legs']:
		for (lx, lz) in ((-7, -5), (7, -5), (-7, 5), (7, 5)):
			A.add (rod_between ((lx, 0, lz), (lx * 0.8, y, lz * 0.8), 0.25, 8), STEEL)
	if p['berms']:
		berm (A, body, 23.4, 22.6, 10.2, z0=-0.6, seed=sum (map (ord, body)))
	mast (A, (-6.5, 9.0 if p['berms'] else y + 6.4, -6.0), 3.0)
	if p['shades']:
		for (lx, lz) in ((-12, -10), (12, -10), (-12, 12), (12, 12)):
			A.add (rod_between ((lx, 0, lz), (lx, 13, lz), 0.25, 8), STEEL)
		A.add (box (28, 0.3, 26, (0, 13, 1)), RADIATOR)
	# collision: the berm (or the modules) and the node
	if p['berms']: A.add (box (21, 8.5, 19, (0, 0, -1)), None, role='proxy')
	else: A.add (box (16, y + 6.4, 14, (0, 0, 0)), None, role='proxy')
	A.add (box (6.2, y + 5.8, 6.2, (0, 0, 9.5)), None, role='proxy')
	return A

GROW = KitMat ('growglass', 'kit_growglass', power=60)
ENVELOPE = KitMat ('envelope', 'kit_panel', (0.95, 0.93, 0.86))
DECK = KitMat ('deck', 'kit_concrete', (0.55, 0.55, 0.57))
GOLD = KitMat ('gold', 'kit_mli', (1.0, 1.0, 1.0))

def proxy_box (A, sx, sy, sz, pos, rot_y=0.0):
	A.add (box (sx, sy, sz, pos, rot_y), None, role='proxy')

def vault (A, r, L, n, mat, pos=(0, 0, 0), y0=0.0, axis_x=False, ribs=0, rib_mat=None, ends=True, uvmode='cyl'):
	"""a barrel vault of radius r, length L along z (or x), on a plinth y0 high"""
	x, y, z = pos
	bm = half_cylinder (r, L, n, (x, y + y0, z), rot_y=90 if axis_x else 0, ends=ends)
	A.add (bm, mat, uv=uvmode, axis=(1, 0, 0) if axis_x else (0, 0, 1), centre=(x, y + y0, z))
	if y0 > 0: A.add (box (2 * r if not axis_x else L, y0, L if not axis_x else 2 * r, (x, y, z)), HULL_DARK)
	if ribs:
		for i in range (ribs + 1):
			zz = -L / 2 + L * i / ribs
			rb = half_cylinder (r + 0.12, 0.3, n, (0, 0, 0), ends=False)
			place (rb, (x + (zz if axis_x else 0), y + y0, z + (0 if axis_x else zz)), 90 if axis_x else 0)
			A.add (rb, rib_mat or STEEL, role='detail')

def a_padberm (body):
	"""blast berm about a landing pad: a ring wall (r 48 m), its opening to +z;
	beacons at the opening"""
	A = Asset ('space_padberm_' + body.lower ())
	n, R, h = 64, 48.0, 3.5
	import bmesh as _b
	bm = kl._bm ()
	gap = math.radians (7.5)  # (the opening: +-7.5 deg about +z)
	prof = [(R + 4.0, 0.0), (R + 2.2, 2.2), (R + 1.0, h), (R + 0.2, h - 0.2), (R - 1.0, 1.8), (R - 2.0, 0.0)]
	rows = []
	for k in range (n + 1):
		a = gap + (2 * math.pi - 2 * gap) * k / n
		wob = 1.0 + 0.04 * math.sin (5.3 * a + len (body)) + 0.03 * math.sin (13.1 * a)
		rows.append ([bm.verts.new ((rr * math.sin (a), yy * wob, rr * math.cos (a))) for rr, yy in prof])
	for k in range (n):
		for j in range (len (prof) - 1):
			bm.faces.new ((rows[k][j], rows[k + 1][j], rows[k + 1][j + 1], rows[k][j + 1]))
	for row in (rows[0], rows[-1]):  # (the ends closed)
		f = bm.faces.new (row)
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	A.add (bm, soil (body))
	for s in (-1, 1):  # (beacons and hazard posts at the opening)
		a = s * gap
		px, pz = (R + 1.0) * math.sin (a), (R + 1.0) * math.cos (a)
		A.add (box (0.6, 1.4, 0.6, (px, h - 0.2, pz)), HAZARD, role='detail')
		A.add (sphere (0.2, (px, h + 1.35, pz), 8, 6), KitMat ('beacon_blue', None, (0.3, 0.5, 1.0), emissive=(0.3, 0.5, 1.0)), role='detail')
		A.light ((px, h + 1.5, pz), (0.3, 0.5, 1.0), 1.0)
	for k in range (16):  # (collision: segments of the ring)
		a = gap + (2 * math.pi - 2 * gap) * (k + 0.5) / 16
		seg = (2 * math.pi - 2 * gap) / 16 * R
		proxy_box (A, 5.0, h, seg + 1.0, ((R + 1.0) * math.sin (a), 0, (R + 1.0) * math.cos (a)), math.degrees (a))
	return A

def a_dome ():
	"""colony dome: a ring wall with windows, a panelled hemisphere with a
	skylight and ribs, an entrance vestibule"""
	A = Asset ('space_dome')
	R, H = 22.0, 5.0
	A.add (cylinder (R, 1.5, 48, (0, 0, 0), caps=(False, False)), HULL_DARK, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (R, 2.0, 48, (0, 1.5, 0), caps=(False, False)), GLASS, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (R, 1.5, 48, (0, 3.5, 0), caps=(False, False)), HULL, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (R + 0.06, 0.4, 48, (0, 4.6, 0), caps=(False, False)), MLI, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	dome = sphere (R, (0, H, 0), 48, 24, cut=0.0)
	for f in [f for f in dome.faces if f.calc_center_median ().y > H + R * 0.93]: dome.faces.remove (f)
	A.add (dome, HULL)
	sky = sphere (R + 0.02, (0, H, 0), 48, 24, cut=R * 0.92)
	A.add (sky, GLASS)
	for k in range (16):  # (ribs)
		a = 2 * math.pi * k / 16
		pts = [(R * 1.01 * math.cos (b) * math.sin (a), H + R * 1.01 * math.sin (b), R * 1.01 * math.cos (b) * math.cos (a)) for b in [math.pi / 2 * i / 10 for i in range (10)]]
		A.add (merge (*[rod_between (pts[i], pts[i + 1], 0.22, 6) for i in range (len (pts) - 1)]), STEEL, role='detail')
	A.add (box (5.0, 3.6, 6.0, (0, 0, R + 2.0)), HULL_DARK)  # (the entrance)
	hatch (A, (0, 0.3, R + 5.02))
	A.add (box (5.6, 0.3, 6.6, (0, 0, R + 2.0)), STEEL)
	return A

def a_greenhouse ():
	"""greenhouse: a long glazed vault lit by grow lights, ribbed, its ends panelled"""
	A = Asset ('space_greenhouse')
	r, L = 6.0, 40.0
	vault (A, r, L, 24, GROW, y0=1.0, ribs=10, ends=False)
	for s in (-1, 1):
		end = kl._bm ()
		vs = [end.verts.new ((r * math.cos (math.pi * k / 24), 1.0 + r * math.sin (math.pi * k / 24), s * L / 2)) for k in range (25)]
		end.faces.new (vs if s > 0 else vs[::-1])
		A.add (end, HULL)
	hatch (A, (0, 1.0, L / 2 - 0.1))
	for x in (-r - 0.6, r + 0.6):  # (a duct along each side)
		A.add (cylinder (0.35, L, 10, (x, 0.6, -L / 2), axis='z'), STEEL, role='detail')
	proxy_box (A, 2 * r, r + 1.0, L, (0, 0, 0))
	return A

def a_solar ():
	"""solar field: two rows of tilted arrays on posts"""
	A = Asset ('space_solar')
	t = math.radians (30)
	for row in (-6, 6):
		for k in range (6):
			x = -25 + 10 * k
			from mathutils import Matrix, Vector
			pnl = box (9.0, 0.08, 3.0)
			kl.transform (pnl, Matrix.Translation (Vector ((x, 2.2, row))) @ Matrix.Rotation (-t, 4, 'X') @ Matrix.Translation (Vector ((0, -0.04, 0))))
			A.add (pnl, SOLAR)
			frm = box (9.2, 0.12, 0.1)
			for dz in (-1.5, 1.5):
				f2 = frm.copy ()
				kl.transform (f2, Matrix.Translation (Vector ((x, 2.2, row))) @ Matrix.Rotation (-t, 4, 'X') @ Matrix.Translation (Vector ((0, -0.1, dz))))
				A.add (f2, STEEL, role='detail')
			A.add (cylinder (0.12, 2.2, 8, (x, 0, row)), STEEL, role='detail')
			A.add (rod_between ((x - 1.5, 0, row), (x, 1.6, row), 0.06, 6), STEEL, role='detail')
			A.add (rod_between ((x + 1.5, 0, row), (x, 1.6, row), 0.06, 6), STEEL, role='detail')
		A.add (box (58, 0.3, 0.4, (-0, 0, row + 2.0)), STEEL, role='detail')  # (cable tray)
	for row in (-6, 6): proxy_box (A, 58, 3.0, 3.0, (0, 0, row))
	return A

def a_radiator ():
	"""radiator wing: two upright panels on a frame, fed by pipes"""
	A = Asset ('space_radiator')
	for s in (-1, 1):
		A.add (box (0.2, 8.0, 14.0, (s * 6, 1.0, 0)), RADIATOR)
		for dz in (-7, 7):
			A.add (box (0.35, 9.0, 0.35, (s * 6, 0, dz)), STEEL)
		A.add (box (0.35, 0.35, 14.0, (s * 6, 9.0, 0)), STEEL)
		A.add (cylinder (0.18, 12.0, 10, (s * 6 - 6 * s, 1.6, 0), axis='x', rot_y=0), STEEL, role='detail')
	A.add (box (12.0, 1.2, 2.0, (0, 0, 0)), HULL_DARK)
	A.add (cylinder (0.25, 12.0, 10, (-6, 0.9, 0), axis='x'), STEEL, role='detail')
	return A

def a_reactor ():
	"""fission power plant: a shielded core, its conical radiator fins"""
	A = Asset ('space_reactor')
	A.add (cylinder (1.6, 3.0, 24, (0, 0, 0)), HULL_DARK, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (1.25, 1.0, 24, (0, 3.0, 0)), GOLD, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (0.5, 9.5, 12, (0, 4.0, 0)), STEEL, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	for k in range (8):
		a = 2 * math.pi * k / 8
		ca, sa = math.cos (a), math.sin (a)
		fin = kl._bm ()
		pts = [(0.5 * sa, 4.0, 0.5 * ca), (5.5 * sa, 4.0, 5.5 * ca), (1.2 * sa, 13.0, 1.2 * ca), (0.5 * sa, 13.0, 0.5 * ca)]
		vs = [fin.verts.new (p) for p in pts]
		off = (0.06 * ca, 0, -0.06 * sa)
		vs2 = [fin.verts.new ((p[0] + off[0], p[1], p[2] + off[2])) for p in pts]
		fin.faces.new (vs); fin.faces.new (vs2[::-1])
		for i in range (4): fin.faces.new ((vs[i], vs2[i], vs2[(i + 1) % 4], vs[(i + 1) % 4]))
		bmesh.ops.recalc_face_normals (fin, faces=fin.faces[:])
		A.add (fin, RADIATOR)
		A.add (rod_between ((1.5 * sa, 3.0, 1.5 * ca), (5.4 * sa, 4.0, 5.4 * ca), 0.1, 6), STEEL, role='detail')
	A.add (cylinder (6.0, 0.4, 32, (0, 0, 0)), STEEL)
	for k in range (4):  # (hazard markers about it)
		a = math.pi / 4 + math.pi / 2 * k
		A.add (box (0.4, 1.2, 0.4, (7.5 * math.sin (a), 0, 7.5 * math.cos (a))), HAZARD, role='detail')
	A.light ((0, 13.3, 0), RED, 1.0)
	proxy_box (A, 3.4, 4.0, 3.4, (0, 0, 0)); proxy_box (A, 11.0, 9.0, 11.0, (0, 4.0, 0))
	return A

def a_comms ():
	"""communications mast: a lattice tower, a dish, whips, an equipment shelter"""
	A = Asset ('space_comms')
	A.add (truss ((0, 0, 0), (0, 28, 0), 2.4, 2.4, 2.0, 0.08), STEEL)
	A.add (cylinder (0.3, 6.0, 10, (0, 28, 0)), HULL, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (box (2.8, 0.2, 2.8, (0, 28, 0)), GRATING, role='detail')
	# the dish: a paraboloid 6 m across on the tower's face, looking up 30 degrees
	from mathutils import Matrix, Vector
	dish = kl._bm ()
	nr, na = 6, 24
	rings = [[dish.verts.new ((3.0 * i / nr * math.cos (2 * math.pi * k / na), 3.0 * i / nr * math.sin (2 * math.pi * k / na),
		0.6 * (i / nr) ** 2)) for k in range (na)] for i in range (1, nr + 1)]
	ctr = dish.verts.new ((0, 0, 0))
	for k in range (na): dish.faces.new ((ctr, rings[0][k], rings[0][(k + 1) % na]))
	for i in range (nr - 1):
		for k in range (na): dish.faces.new ((rings[i][k], rings[i + 1][k], rings[i + 1][(k + 1) % na], rings[i][(k + 1) % na]))
	back = [dish.verts.new ((v.co.x, v.co.y, v.co.z - 0.08)) for v in rings[-1]]
	for k in range (na): dish.faces.new ((rings[-1][k], back[k], back[(k + 1) % na], rings[-1][(k + 1) % na]))
	dish.faces.new (back[::-1])
	bmesh.ops.recalc_face_normals (dish, faces=dish.faces[:])
	kl.transform (dish, Matrix.Translation (Vector ((0, 24.5, 2.2))) @ Matrix.Rotation (math.radians (-60), 4, 'X'))
	A.add (dish, HULL)
	A.add (rod_between ((0, 24.5, 2.2), (0, 26.5, 4.0), 0.06, 6), STEEL, role='detail')
	for dx in (-1.0, 1.0): A.add (cylinder (0.04, 4.0, 6, (dx, 34, 0)), STEEL, role='detail')
	A.add (box (6.0, 3.0, 6.0, (0, 0, 6.0)), HULL)  # (the shelter)
	A.add (box (6.2, 0.25, 6.2, (0, 3.0, 6.0)), HULL_DARK)
	hatch (A, (0, 0, 9.02), w=1.2, h=2.0)
	mast (A, (0, 34, 0), 0.01, light=True)
	proxy_box (A, 2.6, 34.0, 2.6, (0, 0, 0)); proxy_box (A, 6.0, 3.2, 6.0, (0, 0, 6.0))
	return A

def a_isru (body):
	"""in-situ resource plant: a processing hall, storage spheres, a hopper,
	a conveyor up from a regolith heap"""
	A = Asset ('space_isru_' + body.lower ())
	A.add (box (24, 8, 16, (0, 0, 0)), HULL)
	A.add (box (24.4, 0.4, 16.4, (0, 8, 0)), HULL_DARK)
	for x in (-8, 0, 8):  # (roof plant)
		A.add (box (3, 1.5, 3, (x, 8.4, -3)), STEEL)
	A.add (box (8, 6, 0.3, (0, 0, 8.05)), HULL_DARK)  # (the hall's door)
	A.add (box (8.6, 0.12, 0.6, (0, 0, 8.3)), HAZARD)
	A.light ((0, 6.6, 8.4), WHITE, 1.2)
	for k in range (3):
		x = -8 + 8 * k
		A.add (sphere (4.0, (x, 6.5, 16), 24, 12), HULL)
		for (lx, lz) in ((-2.5, -2.5), (2.5, -2.5), (-2.5, 2.5), (2.5, 2.5)):
			A.add (rod_between ((x + lx, 0, 16 + lz), (x + lx * 0.6, 4.5, 16 + lz * 0.6), 0.18, 8), STEEL, role='detail')
		A.add (cylinder (0.3, 6.5, 8, (x, 0, 12)), STEEL, role='detail')
	A.add (cylinder (6.0, 6.0, 24, (16, 6, 0), r2=4.0), HULL_DARK)  # (the hopper, wide end up)
	from mathutils import Matrix
	for (lx, lz) in ((12.5, -3), (19.5, -3), (12.5, 3), (19.5, 3)):
		A.add (box (0.5, 6.0, 0.5, (lx, 0, lz)), STEEL)
	A.add (truss ((16, 9, 0), (34, 0.8, 0), 1.6, 1.0, 2.0, 0.07), STEEL, role='detail')
	A.add (box (1.2, 0.2, 20.0, (25, 4.6, 0), rot_y=90), STEEL, role='detail')
	heap = sphere (1.0, (0, 0, 0), 24, 8, cut=0.0)
	for v in heap.verts: v.co = (34 + v.co.x * 8, v.co.y * 4 * (1 + 0.1 * math.sin (5 * v.co.x)), v.co.z * 7)
	A.add (heap, soil (body))
	proxy_box (A, 24, 8.5, 16, (0, 0, 0)); proxy_box (A, 22, 10.5, 9, (0, 0, 16)); proxy_box (A, 12, 12, 12, (16, 0, 0))
	return A

def a_garage ():
	"""rover garage: a ribbed vault, a door with a lit frame"""
	A = Asset ('space_garage')
	r, L = 8.0, 24.0
	vault (A, r, L, 20, KitMat ('garage', 'kit_steel', (0.86, 0.86, 0.84)), ribs=6, ends=False)
	for s in (-1, 1):
		end = kl._bm ()
		vs = [end.verts.new ((r * math.cos (math.pi * k / 20), r * math.sin (math.pi * k / 20), s * L / 2)) for k in range (21)]
		end.faces.new (vs if s > 0 else vs[::-1])
		A.add (end, HULL_DARK if s > 0 else HULL)
	A.add (box (10.0, 5.5, 0.2, (0, 0, L / 2 + 0.05)), KitMat ('door', 'kit_steel', (0.45, 0.46, 0.48)))
	A.add (box (11.0, 0.12, 0.3, (0, 0, L / 2 + 0.15)), HAZARD)
	A.add (box (1.0, 0.25, 0.3, (0, 6.0, L / 2 + 0.15)), LAMP, role='detail')
	A.light ((0, 6.3, L / 2 + 0.5), WHITE, 1.2)
	proxy_box (A, 2 * r, r, L, (0, 0, 0))
	return A

def a_massdriver ():
	"""mass driver: a 1.6 km track on pylons along +z from a loading hall"""
	A = Asset ('space_massdriver')
	L, h = 1600.0, 5.0
	A.add (box (28, 12, 30, (0, 0, -25)), HULL)
	A.add (box (28.4, 0.4, 30.4, (0, 12, -25)), HULL_DARK)
	A.add (box (6, 6, 0.3, (0, 5, -9.9)), HULL_DARK)
	for k in range (41):
		z = -10 + L * k / 40
		A.add (rod_between ((-1.6, 0, z), (-1.0, h, z), 0.3, 8), STEEL)
		A.add (rod_between ((1.6, 0, z), (1.0, h, z), 0.3, 8), STEEL)
		A.add (box (2.6, 0.4, 0.6, (0, h - 0.4, z)), STEEL)
	for s in (-1, 1):
		A.add (box (0.5, 0.6, L, (s * 1.2, h, L / 2 - 10)), KitMat ('rail', 'kit_steel', (0.6, 0.6, 0.62)))
	for k in range (0, 80, 2):
		z = -10 + L * (k + 0.5) / 80
		A.add (box (3.2, 1.9, 2.0, (0, h - 0.3, z)), GOLD)
	A.light ((0, 12.6, -25), RED, 1.2)
	proxy_box (A, 28, 12, 30, (0, 0, -25))
	return A

def a_solar_tower ():
	"""polar solar tower: a lattice mast with a tall vertical array (the sun at the horizon)"""
	A = Asset ('space_solar_tower')
	A.add (truss ((0, 0, 0), (0, 32, 0), 1.6, 1.6, 1.6, 0.06), STEEL, role='detail')
	for s, mat in ((1, SOLAR), (-1, STEEL)):
		A.add (box (10.0, 20.0, 0.06, (0, 12, s * 1.2)), mat)
	A.add (box (10.0, 0.4, 2.4, (0, 21.8, 0)), STEEL)
	A.add (box (4.0, 1.2, 4.0, (0, 0, 0)), HULL_DARK)
	A.light ((0, 32.3, 0), RED, 1.0)
	proxy_box (A, 10.0, 32.0, 2.6, (0, 0, 0))
	return A

def a_radioarray ():
	"""low-frequency radio telescope: a 200 m field of crossed dipoles, a receiver hut"""
	A = Asset ('space_radioarray')
	parts_post, parts_dip = [], []
	for i in range (9):
		for k in range (9):
			x, z = -100 + 25 * i, -100 + 25 * k
			parts_post.append (cylinder (0.06, 1.2, 6, (x, 0, z)))
			parts_dip.append (box (5.0, 0.1, 0.1, (x, 1.2, z)))
			parts_dip.append (box (0.1, 0.1, 5.0, (x, 1.26, z)))
	A.add (merge (*parts_post), STEEL)
	A.add (merge (*parts_dip), GOLD)
	A.add (box (8, 3, 6, (0, 0, 111)), HULL)
	A.add (box (8.4, 0.25, 6.4, (0, 3, 111)), HULL_DARK)
	hatch (A, (0, 0, 108 - 0.02), rot_y=180, w=1.2, h=2.0)
	proxy_box (A, 8, 3, 6, (0, 0, 111))
	return A

# the aerostat (offworld.py: AE_A, AE_C, AE_Y, AE_DECK)
AE_A, AE_C, AE_Y, AE_DECK = 135.0, 38.0, -42.0, 75.0

def a_aerostat ():
	"""a floating habitat: a lens-shaped envelope (gores, window bands of the
	town inside, solar arrays on its shoulders), a landing deck on struts,
	rotors about its rim, a gondola below"""
	A = Asset ('space_aerostat')
	env = sphere (1.0, (0, 0, 0), 96, 36)
	for v in env.verts: v.co = (v.co.x * AE_A, AE_Y + v.co.y * AE_C, v.co.z * AE_A)
	A.add (env, ENVELOPE)
	for (b0, b1) in ((-0.10, -0.03), (0.02, 0.09)):  # (window bands: the town inside)
		band = kl._bm ()
		n = 96
		r0 = [band.verts.new ((AE_A * 1.004 * math.cos (b0) * math.sin (2 * math.pi * k / n), AE_Y + AE_C * 1.004 * math.sin (b0), AE_A * 1.004 * math.cos (b0) * math.cos (2 * math.pi * k / n))) for k in range (n)]
		r1 = [band.verts.new ((AE_A * 1.004 * math.cos (b1) * math.sin (2 * math.pi * k / n), AE_Y + AE_C * 1.004 * math.sin (b1), AE_A * 1.004 * math.cos (b1) * math.cos (2 * math.pi * k / n))) for k in range (n)]
		for k in range (n): band.faces.new ((r0[k], r0[(k + 1) % n], r1[(k + 1) % n], r1[k]))
		bmesh.ops.recalc_face_normals (band, faces=band.faces[:])
		A.add (band, GLASS, uv='cyl', axis=(0, 1, 0), centre=(0, AE_Y, 0))
	for k in range (64):  # (solar arrays on the shoulders)
		if k % 8 == 0: continue
		a0, a1 = 2 * math.pi * (k + 0.1) / 64, 2 * math.pi * (k + 0.9) / 64
		for (b0, b1) in ((0.42, 0.62), (0.66, 0.86)):
			pnl = kl._bm ()
			P = lambda a, b: (AE_A * 1.006 * math.cos (b) * math.sin (a), AE_Y + AE_C * 1.006 * math.sin (b), AE_A * 1.006 * math.cos (b) * math.cos (a))
			vs = [pnl.verts.new (P (a0, b0)), pnl.verts.new (P (a1, b0)), pnl.verts.new (P (a1, b1)), pnl.verts.new (P (a0, b1))]
			pnl.faces.new (vs)
			bmesh.ops.recalc_face_normals (pnl, faces=pnl.faces[:])
			if pnl.faces[0].normal.y < 0: pnl.faces[0].normal_flip ()
			A.add (pnl, SOLAR)
	# (the deck: its top 5 cm under the deck's ground (BEGIN_DECKS, y = 0) - vessels stand on that,
	# not on the slab's hull; its edge marking a little above it)
	A.add (cylinder (AE_DECK, 1.95, 96, (0, -2.0, 0)), DECK)
	edge = kl._bm ()
	n = 96
	ri = [edge.verts.new (((AE_DECK - 2.5) * math.sin (2 * math.pi * k / n), -0.03, (AE_DECK - 2.5) * math.cos (2 * math.pi * k / n))) for k in range (n)]
	ro = [edge.verts.new (((AE_DECK - 0.3) * math.sin (2 * math.pi * k / n), -0.03, (AE_DECK - 0.3) * math.cos (2 * math.pi * k / n))) for k in range (n)]
	for k in range (n): edge.faces.new ((ri[k], ri[(k + 1) % n], ro[(k + 1) % n], ro[k]))
	bmesh.ops.recalc_face_normals (edge, faces=edge.faces[:])
	for f in edge.faces:
		if f.normal.y < 0: f.normal_flip ()
	A.add (edge, HAZARD)
	struts = []
	for k in range (24):
		a = 2 * math.pi * k / 24
		for rr in (35.0, 68.0):
			y = AE_Y + AE_C * math.sqrt (max (0.0, 1 - (rr / AE_A) ** 2))
			struts.append (rod_between ((rr * math.sin (a), -2.0, rr * math.cos (a)), (rr * math.sin (a), y - 0.5, rr * math.cos (a)), 0.5, 8))
	A.add (merge (*struts), STEEL)
	for k in range (6):  # (rotors on pylons about the rim)
		a = 2 * math.pi * (k + 0.5) / 6
		ca, sa = math.cos (a), math.sin (a)
		c = ((AE_A + 14) * sa, AE_Y, (AE_A + 14) * ca)
		A.add (rod_between ((AE_A * 0.97 * sa, AE_Y, AE_A * 0.97 * ca), c, 0.7, 10), STEEL)
		t = (ca, 0, -sa)
		A.add (rod_between ((c[0] - 6 * t[0], c[1], c[2] - 6 * t[2]), (c[0] + 6 * t[0], c[1], c[2] + 6 * t[2]), 1.5, 16), HULL)
		hub = (c[0] + 6.5 * t[0], c[1], c[2] + 6.5 * t[2])
		for j in range (3):
			ph = 2 * math.pi * j / 3 + 0.3
			d = (math.cos (ph) * sa, math.sin (ph), math.cos (ph) * ca)
			A.add (rod_between (hub, (hub[0] + 9 * d[0], hub[1] + 9 * d[1], hub[2] + 9 * d[2]), 0.4, 6), HULL_DARK)
		A.light ((c[0], c[1] + 2.0, c[2]), RED, 1.5)
	yb = AE_Y - AE_C
	module (A, 0, 0, 30.0, 4.0, yb - 26, HULL, windows=True)
	module (A, 0, 0, 30.0, 4.0, yb - 26, HULL, along_x=True, windows=True)
	for (cx, cz) in ((-10, -10), (10, -10), (-10, 10), (10, 10)):
		A.add (rod_between ((cx * 0.4, yb - 18, cz * 0.4), (cx, yb + 0.6, cz), 0.15, 6), STEEL, role='detail')
	A.add (rod_between ((0, yb - 26, 0), (0, yb - 140, 0), 0.2, 6), STEEL, role='detail')
	# collision: under the deck only (the deck itself is the base's ground: BEGIN_DECKS)
	proxy_box (A, 2 * AE_A * 0.95, 2 * AE_C * 0.9, 2 * AE_A * 0.95, (0, AE_Y - AE_C * 0.9, 0))
	proxy_box (A, 34, 12, 34, (0, yb - 28, 0))
	return A

def a_aerostat_terminal ():
	"""the deck's terminal: a hangar for small craft, a control tower with a lit cab, a mast"""
	A = Asset ('space_aerostat_terminal')
	A.add (box (28, 9, 18, (0, 0, 0)), HULL)
	A.add (box (28.4, 0.4, 18.4, (0, 9, 0)), HULL_DARK)
	A.add (box (22, 7.5, 0.1, (0, 0, 9.0)), KitMat ('door', 'kit_steel', (0.45, 0.46, 0.48)))
	A.add (box (23, 0.12, 0.2, (0, 0, 9.05)), HAZARD)
	A.add (cylinder (3.5, 16, 24, (20, 0, -2)), HULL, uv='cyl', axis=(0, 1, 0), centre=(20, 0, -2))
	A.add (cylinder (5.0, 3.5, 24, (20, 16, -2), caps=(True, False)), GLASS, uv='cyl', axis=(0, 1, 0), centre=(20, 0, -2))
	A.add (cylinder (5.5, 1.0, 24, (20, 19.5, -2)), HULL_DARK)
	A.add (truss ((20, 20.5, -2), (20, 32, -2), 1.6, 1.6, 1.5, 0.05), STEEL)
	A.light ((20, 30.3, -2), RED, 1.2)
	A.light ((0, 8.0, 9.6), WHITE, 1.2)
	proxy_box (A, 28, 9, 18, (0, 0, 0)); proxy_box (A, 11, 20.5, 11, (20, 0, -2))
	return A

KITS = {
	'offworld': dict ([(('space_hab_' + b.lower ()), (lambda b=b: a_hab (b))) for b in BODIES]
		+ [(('space_padberm_' + b.lower ()), (lambda b=b: a_padberm (b))) for b in BODIES]
		+ [(('space_isru_' + b.lower ()), (lambda b=b: a_isru (b))) for b in BODIES]
		+ [('space_dome', a_dome), ('space_greenhouse', a_greenhouse), ('space_solar', a_solar),
		   ('space_radiator', a_radiator), ('space_reactor', a_reactor), ('space_comms', a_comms),
		   ('space_garage', a_garage), ('space_massdriver', a_massdriver), ('space_solar_tower', a_solar_tower),
		   ('space_radioarray', a_radioarray), ('space_aerostat', a_aerostat), ('space_aerostat_terminal', a_aerostat_terminal)]),
}

# ----------------------------------------------------------------------

def main ():
	args = sys.argv[sys.argv.index ('--') + 1:] if '--' in sys.argv else []
	if len (args) < 2: raise SystemExit (__doc__)
	kit, which = args[0], args[1]
	root = args[args.index ('--out') + 1] if '--out' in args else ROOT
	check = '--check' in args
	lights_path = os.path.join (HERE, 'kit_lights.json')
	lights = json.load (open (lights_path)) if os.path.exists (lights_path) else {}
	failed = []
	for name, build in KITS[kit].items ():
		if which != 'all' and which != name: continue
		A = build ()
		path = os.path.join (root, 'Meshes', 'BaseKit', name + '.msh')
		ntri = kl.write (A, path)
		lights[name] = [[list (p), list (c), s] for p, c, s in A.lights]
		print ('%s: %d triangles, %d lights' % (name, ntri, len (A.lights)), flush=True)
		if check:
			r = subprocess.run ([sys.executable if not sys.executable.endswith ('blender') else 'python3',
				os.path.join (ROOT, 'Utils', 'BlenderMsh', 'msh_check.py'), path,
				'--bounds-json', os.path.join (HERE, 'footprints.json'), '--textures', os.path.join (ROOT, 'Textures')],
				capture_output=True, text=True)
			print (r.stdout.rstrip ())
			if r.returncode: failed.append (name)
	json.dump (lights, open (lights_path, 'w'), indent=1, sort_keys=True)
	if failed: raise SystemExit ('footprint/mesh checks failed: ' + ', '.join (failed))

if __name__ == '__main__':
	main ()
