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
from mathutils import Vector

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
DECK_PANEL = KitMat ('deck_panel', 'kit_panel', (0.66, 0.67, 0.68))  # (the aerostat's deck: honeycomb composite)
CELL_FILM = KitMat ('cell_film', 'kit_panel', (0.74, 0.79, 0.86))  # (the aerostat's helium cells: metallised film)
DECK_PLATE = KitMat ('deck_plate', 'kit_steel', (0.72, 0.72, 0.74))
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
AE_CORE = AE_A * math.cos (math.radians (50.0))  # (the air-filled habitat core's radius, 87 m - an envelope ring: helium cells about it)
AE_CELLS = 16     # (the ring's helium cells)
AE_LOBE = 0.03    # (their bulge between the load tapes, a fraction of the radius: 4 m at the rim)
AE_KEEL_R = 80.0  # (the keel's radius)
AE_KEEL = -81.0   # (the keel under the core: the town's lit gallery, the gondola's mount)
DECK_W = (-0.14, -0.11, -0.08, -0.05)  # (heights: grating, walkways, hub and terminal plates, pad plates)

def ae_pads ():
	"""the deck's pads (offworld.py aerostat_layout: 42 m out at bearings 0, 120, 240), base frame (x south, z east)"""
	return [(-42 * math.cos (math.radians (b)), 42 * math.sin (math.radians (b)), math.radians (b)) for b in (0.0, 120.0, 240.0)]

def deck_quad (c, u, v, hu, hv, y):
	"""a plate (up-facing) centred on c (x, z), half sizes hu along u and hv along v (unit x, z directions)"""
	bm = kl._bm ()
	vs = [bm.verts.new ((c[0] + su * hu * u[0] + sv * hv * v[0], y, c[1] + su * hu * u[1] + sv * hv * v[1])) for (su, sv) in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
	bm.faces.new (vs)
	for f in bm.faces:
		if f.normal.y < 0: f.normal_flip ()
	return bm

def annulus (r0, r1, y, n=96, up=True):
	bm = kl._bm ()
	ri = [bm.verts.new ((r0 * math.sin (2 * math.pi * k / n), y, r0 * math.cos (2 * math.pi * k / n))) for k in range (n)]
	ro = [bm.verts.new ((r1 * math.sin (2 * math.pi * k / n), y, r1 * math.cos (2 * math.pi * k / n))) for k in range (n)]
	for k in range (n): bm.faces.new ((ri[k], ri[(k + 1) % n], ro[(k + 1) % n], ro[k]))
	for f in bm.faces:
		if (f.normal.y < 0) == up: f.normal_flip ()
	return bm

def a_aerostat ():
	"""a floating habitat: a lens-shaped envelope - a ring of 16 helium cells
	(silvered film, lobed between load tapes, solar arrays on every other one)
	about an air-filled core that holds the town (a collar where they meet, its
	window ring about the deck, its lit keel gallery below) - an open deck
	(girders under composite panels, plates for pads, grating walkways) on
	struts, rotors about its rim, a gondola below.

	Lift at 55 km (CO2 0.92 kg/m3): the helium ring (1.3 million m3) about
	1,100 t, the air core (1.6 million m3) about 500 t - 1,600 t against about
	1,150 t of deck, envelope, struts and town."""
	from mathutils import Vector
	A = Asset ('space_aerostat')
	env = sphere (1.0, (0, 0, 0), 96, 36)
	for v in env.verts: v.co = (v.co.x, -v.co.z, v.co.y)  # (its poles up and down: its meridians the cells' tapes)
	v0 = next (v for v in env.verts if math.hypot (v.co.x, v.co.z) > 0.5)
	a0 = math.atan2 (v0.co.x, v0.co.z)  # (a meridian of the envelope's: the load tapes on its meridians)
	def lobe (a, rr):
		f = ((a - a0) * AE_CELLS / (2 * math.pi)) % 1.0
		w = min (1.0, max (0.0, (rr - AE_CORE) / 16.0))
		return AE_LOBE * math.sin (math.pi * f) * w * w * (3 - 2 * w)
	for v in env.verts:
		x, y, z = v.co.x * AE_A, v.co.y * AE_C, v.co.z * AE_A
		s = 1 + lobe (math.atan2 (x, z), math.hypot (x, z))
		v.co = (x * s, AE_Y + y * s, z * s)
	cells = env.copy ()  # (split at the 50-degree ring: the core inside, the cells outside)
	rr = lambda f: math.hypot (f.calc_center_median ().x, f.calc_center_median ().z)
	bmesh.ops.delete (env, geom=[f for f in env.faces if rr (f) > AE_CORE], context='FACES')
	bmesh.ops.delete (cells, geom=[f for f in cells.faces if rr (f) <= AE_CORE], context='FACES')
	A.add (env, ENVELOPE)
	A.add (cells, CELL_FILM)
	P = lambda a, b, s=1.0: (s * AE_A * math.cos (b) * math.sin (a), AE_Y + s * AE_C * math.sin (b), s * AE_A * math.cos (b) * math.cos (a))
	tapes = kl._bm ()  # (the load tapes between the cells, over the rim from collar to collar)
	for c in range (AE_CELLS):
		a = a0 + 2 * math.pi * c / AE_CELLS
		t = (math.cos (a), 0.0, -math.sin (a))
		rows = []
		for k in range (-10, 11):
			p = P (a, math.pi * k / 36, 1.002)
			rows.append ((tapes.verts.new ((p[0] - 0.5 * t[0], p[1], p[2] - 0.5 * t[2])), tapes.verts.new ((p[0] + 0.5 * t[0], p[1], p[2] + 0.5 * t[2]))))
		for (l0, r0), (l1, r1) in zip (rows, rows[1:]):
			f = tapes.faces.new ((l0, r0, r1, l1))
			m = f.calc_center_median ()
			if f.normal.dot (Vector ((m.x, (m.y - AE_Y) * (AE_A / AE_C) ** 2, m.z))) < 0: f.normal_flip ()
	A.add (tapes, HULL_DARK)
	bc = math.radians (50.0)  # (the collars: where the cells meet the core, above and below)
	collar = []
	for sb in (1, -1):
		ring = [P (a0 + 2 * math.pi * k / 96, sb * bc) for k in range (97)]
		collar += [rod_between (ring[k], ring[k + 1], 0.9, 8) for k in range (96)]
	A.add (merge (*collar), HULL_DARK)
	win = kl._bm ()  # (the core's window ring about the deck: the town under it)
	n = 96
	rows = [[win.verts.new (P (a0 + 2 * math.pi * k / n, b, 1.004)) for k in range (n)] for b in (0.875, 0.925)]
	for k in range (n):
		f = win.faces.new ((rows[0][k], rows[0][(k + 1) % n], rows[1][(k + 1) % n], rows[1][k]))
		if f.normal.y < 0: f.normal_flip ()
	A.add (win, GLASS, uv='cyl', axis=(0, 1, 0), centre=(0, AE_Y, 0))
	for c in range (0, AE_CELLS, 2):  # (solar arrays on every other cell's shoulder)
		for i in range (4):
			f0 = 0.06 + 0.225 * i
			a0p, a1p = a0 + 2 * math.pi * (c + f0) / AE_CELLS, a0 + 2 * math.pi * (c + f0 + 0.2) / AE_CELLS
			for (b0, b1) in ((0.42, 0.62), (0.66, 0.84)):
				pnl = kl._bm ()
				Q = lambda a, b: P (a, b, 1.012 + lobe (a, AE_A * math.cos (b)))
				vs = [pnl.verts.new (Q (a0p, b0)), pnl.verts.new (Q (a1p, b0)), pnl.verts.new (Q (a1p, b1)), pnl.verts.new (Q (a0p, b1))]
				pnl.faces.new (vs)
				bmesh.ops.recalc_face_normals (pnl, faces=pnl.faces[:])
				if pnl.faces[0].normal.y < 0: pnl.faces[0].normal_flip ()
				A.add (pnl, SOLAR)
	# (the core's keel: the town's gallery, its windows looking out under the cells)
	A.add (cylinder (AE_KEEL_R, 10.0, 96, (0, AE_KEEL, 0), caps=(True, False)), HULL, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	for (y0, h) in ((AE_KEEL + 1.0, 1.5), (AE_KEEL + 3.4, 1.5)):
		A.add (cylinder (AE_KEEL_R + 0.05, h, 96, (0, y0, 0), caps=(False, False)), GLASS, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	# (the deck - its top just under the deck's ground (BEGIN_DECKS, y = 0) that vessels stand on:
	# light composite panels over radial and ring girders (their tops 0.3 m down), steel plates
	# under the pads, the hub and the terminal, grating walkways between them; its edge marking)
	yg, yw, yh, yp = DECK_W
	A.add (annulus (13.8, AE_DECK - 2.4, yg), DECK_PANEL)
	A.add (annulus (13.8, AE_DECK - 2.4, yg - 0.02, up=False), HULL_DARK)
	A.add (cylinder (14.0, 1.6 + yh, 48, (0, -1.6, 0)), DECK_PLATE)
	for (x, z, b) in ae_pads ():
		u = (-math.cos (b), math.sin (b))  # (out along the pad's bearing)
		v = (math.sin (b), math.cos (b))
		A.add (deck_quad ((x, z), u, v, 20.0, 20.0, yp), DECK_PLATE)
		A.add (deck_quad ((x * 0.5, z * 0.5), u, v, 13.0, 3.0, yw), GRATING)
	tb = math.radians (60.0)  # (the terminal: offworld.py aerostat_layout, 55 m out)
	u, v = (-math.cos (tb), math.sin (tb)), (math.sin (tb), math.cos (tb))
	A.add (deck_quad ((55 * u[0], 55 * u[1]), u, v, 11.0, 28.0, yh), DECK_PLATE)
	A.add (deck_quad ((29 * u[0], 29 * u[1]), u, v, 16.0, 3.0, yw), GRATING)
	A.add (annulus (AE_DECK - 2.5, AE_DECK - 0.3, -0.03), HAZARD)
	gird = []
	for k in range (36):  # (radial girders)
		a = 2 * math.pi * (k + 0.5) / 36
		gird.append (box (0.6, 1.3, AE_DECK - 0.6 - 14.0, ((14.0 + (AE_DECK - 0.6)) / 2 * math.sin (a), -1.6, (14.0 + (AE_DECK - 0.6)) / 2 * math.cos (a)), rot_y=math.degrees (a), bottom=False))
	for (rr, n) in ((24.0, 36), (35.0, 48), (52.0, 72), (68.0, 96)):  # (ring girders)
		for k in range (n):
			a = 2 * math.pi * k / n
			L = 2 * rr * math.sin (math.pi / n) + 0.1
			gird.append (box (L, 1.3, 0.5, (rr * math.cos (math.pi / n) * math.sin (a), -1.6, rr * math.cos (math.pi / n) * math.cos (a)), rot_y=math.degrees (a), bottom=False))
	A.add (merge (*gird), STEEL)
	fascia = []
	for k in range (96):  # (the edge girder)
		a = 2 * math.pi * k / 96
		rr = AE_DECK - 0.3
		fascia.append (box (2 * rr * math.sin (math.pi / 96) + 0.05, 1.54, 0.6, (rr * math.cos (math.pi / 96) * math.sin (a), -1.6, rr * math.cos (math.pi / 96) * math.cos (a)), rot_y=math.degrees (a)))
	A.add (merge (*fascia), HULL_DARK)
	# (a safety net about the deck, sloping out and down from under its edge; its edge lights)
	n = 96
	net = kl._bm ()
	ni = [net.verts.new ((AE_DECK * math.sin (2 * math.pi * k / n), -0.6, AE_DECK * math.cos (2 * math.pi * k / n))) for k in range (n)]
	no = [net.verts.new (((AE_DECK + 5.0) * math.sin (2 * math.pi * k / n), -1.6, (AE_DECK + 5.0) * math.cos (2 * math.pi * k / n))) for k in range (n)]
	for k in range (n): net.faces.new ((ni[k], ni[(k + 1) % n], no[(k + 1) % n], no[k]))
	back = net.copy (); bmesh.ops.reverse_faces (back, faces=back.faces[:])
	A.add (merge (net, back), GRATING, role='detail')
	for k in range (24):
		a = 2 * math.pi * (k + 0.5) / 24
		A.add (rod_between ((AE_DECK * math.sin (a), -0.6, AE_DECK * math.cos (a)), ((AE_DECK + 5.0) * math.sin (a), -1.6, (AE_DECK + 5.0) * math.cos (a)), 0.06, 4), STEEL, role='detail')
	for k in range (36):
		a = 2 * math.pi * k / 36
		A.light (((AE_DECK - 0.8) * math.sin (a), 0.15, (AE_DECK - 0.8) * math.cos (a)), (0.35, 0.55, 1.0), 0.5)
	struts = []
	for k in range (24):
		a = 2 * math.pi * k / 24
		for rr in (35.0, 68.0):
			y = AE_Y + AE_C * math.sqrt (max (0.0, 1 - (rr / AE_A) ** 2))
			struts.append (rod_between ((rr * math.sin (a), -1.6, rr * math.cos (a)), (rr * math.sin (a), y - 0.5, rr * math.cos (a)), 0.45, 8))
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
	yb = AE_KEEL - 25.0
	module (A, 0, 0, 30.0, 4.0, yb, HULL, windows=True)
	module (A, 0, 0, 30.0, 4.0, yb, HULL, along_x=True, windows=True)
	for (cx, cz) in ((-10, -10), (10, -10), (-10, 10), (10, 10)):
		A.add (rod_between ((cx * 0.4, yb + 8, cz * 0.4), (cx, AE_KEEL, cz), 0.15, 6), STEEL, role='detail')
	A.add (rod_between ((0, yb, 0), (0, yb - 114, 0), 0.2, 6), STEEL, role='detail')
	# collision: under the deck only (the deck itself is the base's ground: BEGIN_DECKS)
	proxy_box (A, 2 * AE_A * 0.95, 2 * AE_C * 0.9, 2 * AE_A * 0.95, (0, AE_Y - AE_C * 0.9, 0))
	proxy_box (A, 2 * AE_KEEL_R * 0.9, 5.0, 2 * AE_KEEL_R * 0.9, (0, AE_KEEL, 0))
	proxy_box (A, 34, 12, 34, (0, yb - 2, 0))
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

# ----------------------------------------------------------------------
# trains (TRAIN1, TRAIN2: drawn and moved by the Vulkan client, VBase::ReadTrains;
# +z along the track)

TRAIN_H = 12.0  # (the hangrail's rail height the portal is made for: the client scales it to the track's)
CAR = KitMat ('car', 'kit_panel', (0.93, 0.93, 0.92))
CAR_STRIPE = KitMat ('car_stripe', 'kit_panel', (0.16, 0.36, 0.72))
CAR_DARK = KitMat ('car_dark', 'kit_panel', (0.22, 0.23, 0.25))
HEAD = KitMat ('headlamp', None, (1.0, 0.97, 0.9), emissive=(1.0, 0.95, 0.85))
TAIL = KitMat ('taillamp', None, (1.0, 0.1, 0.06), emissive=(1.0, 0.08, 0.04))
CONCRETE = KitMat ('guideway', 'kit_concrete', (0.82, 0.82, 0.80))

def a_hangrail_girder ():
	"""a portal of the hangrail (TRAIN2), its rail height TRAIN_H: tapered box
	legs on footings 18 m apart, a crossbeam, hangers down to the two rail
	beams (x +-2.5, their tops at TRAIN_H + 1.2)"""
	A = Asset ('train_hangrail_girder')
	H = TRAIN_H
	top = H + 4.0
	for s in (-1, 1):
		A.add (box (3.2, 0.6, 3.2, (s * 9.0, 0, 0)), CONCRETE)
		# (the leg: from 1.3 x 1.3 at the foot to 0.9 x 0.9 under the crossbeam, leaning in)
		bm = kl._bm ()
		lo, hi = [(s * 9.0, 0.6), (s * 6.4, top - 1.6)], (0.65, 0.45)
		vs = []
		for (x, y), w in zip (lo, hi):
			vs.append ([bm.verts.new ((x + dx * w, y, dz * w)) for dx, dz in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
		for i in range (4):
			j = (i + 1) % 4
			bm.faces.new ((vs[0][i], vs[0][j], vs[1][j], vs[1][i]))
		bm.faces.new (vs[1])
		bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
		A.add (bm, STEEL)
		A.add (box (0.5, 0.05, 3.0, (s * 9.0, 0.6, 0)), HAZARD, role='detail')
	A.add (box (15.0, 1.6, 1.4, (0, top - 1.6, 0)), STEEL)                 # (the crossbeam)
	for s in (-1, 1):
		A.add (box (0.35, top - 1.6 - (H + 1.2), 0.35, (s * 2.5, H + 1.2, 0)), STEEL)  # (hangers)
		A.add (box (2.0, 0.5, 1.0, (s * 2.5, H + 1.2, 0)), CAR_DARK)          # (the beams' saddles)
	A.add (box (12.0, 0.08, 1.0, (0, top, 0)), GRATING, role='detail')       # (a walkway over it)
	A.add (sphere (0.18, (0, top + 0.3, 0), 8, 6), KitMat ('beacon', None, RED, emissive=RED), role='detail')
	A.light ((0, top + 0.3, 0), RED, 0.8)
	return A

def a_hangrail_beam ():
	"""a span of the hangrail's two rail beams (x +-2.5) from z 0 to 150 (the
	client stretches it to the span): steel box beams, their slots below (the
	cars' bogies run inside), a walkway between"""
	A = Asset ('train_hangrail_beam')
	L, H = 150.0, 0.0  # (the client puts it at the rail's height)
	for s in (-1, 1):
		x = s * 2.5
		A.add (box (1.1, 1.0, L, (x, 0.2, L / 2)), STEEL)                        # (the box)
		A.add (box (0.35, 0.2, L, (x - 0.375, 0.0, L / 2)), STEEL)               # (its lips, the slot between)
		A.add (box (0.35, 0.2, L, (x + 0.375, 0.0, L / 2)), STEEL)
		A.add (box (0.4, 0.02, L, (x, 0.18, L / 2)), CAR_DARK, role='detail')    # (the slot's dark)
	A.add (box (3.6, 0.06, L, (0, 1.0, L / 2)), GRATING, role='detail')
	for k in range (16):  # (cross ties)
		A.add (box (5.0, 0.25, 0.25, (0, 0.95, (k + 0.5) * L / 16)), STEEL, role='detail')
	return A

def car_body (A, L, w, y0, h, nose):
	"""a car's body along z (L long, w wide, from y0 up h), rounded ends of
	length nose: its shell, a window band, a livery stripe, lamps"""
	# (the cross-section: a rounded rectangle; the ends: tapered)
	n = 8
	def ring (z, sc):
		pts = []
		for k in range (4 * n):
			a = 2 * math.pi * (k + 0.5) / (4 * n)
			cx, cy = math.cos (a), math.sin (a)
			# (superellipse: square-ish, corners rounded)
			ex = math.copysign (abs (cx) ** 0.35, cx); ey = math.copysign (abs (cy) ** 0.35, cy)
			pts.append ((ex * w / 2 * sc, y0 + h / 2 + ey * h / 2 * (0.6 + 0.4 * sc), z))
		return pts
	zs = [(-L / 2, 0.45), (-L / 2 + nose * 0.35, 0.82), (-L / 2 + nose, 1.0), (L / 2 - nose, 1.0), (L / 2 - nose * 0.35, 0.82), (L / 2, 0.45)]
	bm = kl._bm ()
	rings = [[bm.verts.new (p) for p in ring (z, sc)] for z, sc in zs]
	for r0, r1 in zip (rings, rings[1:]):
		for k in range (4 * n):
			j = (k + 1) % (4 * n)
			bm.faces.new ((r0[k], r0[j], r1[j], r1[k]))
	bm.faces.new (rings[0][::-1]); bm.faces.new (rings[-1])
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	A.add (bm, CAR)
	zl = L / 2 - nose
	# (windows both sides, a band; the stripe under them; the ends' windscreens)
	for s in (-1, 1):
		A.add (box (0.04, h * 0.32, 2 * zl - 1.0, (s * (w / 2 + 0.01), y0 + h * 0.5, 0)), GLASS)
		A.add (box (0.04, h * 0.08, 2 * zl + nose * 0.6, (s * (w / 2 + 0.015), y0 + h * 0.38, 0)), CAR_STRIPE)
		A.add (box (w * 0.7, h * 0.3, 0.04, (0, y0 + h * 0.48, s * (L / 2 - nose * 0.12))), GLASS)
		for dx in (-1, 1):
			A.add (box (0.3, 0.18, 0.06, (dx * w * 0.32, y0 + h * 0.2, s * (L / 2 - nose * 0.05))), HEAD if s > 0 else TAIL, role='detail')
	# (doors: dark frames, two each side)
	for s in (-1, 1):
		for dz in (-zl * 0.45, zl * 0.45):
			A.add (box (0.03, h * 0.72, 1.3, (s * (w / 2 + 0.02), y0 + h * 0.1, dz)), CAR_DARK, role='detail')

def a_hangrail_car ():
	"""a hangrail car (TRAIN2): its origin at its rail (the beam's slot), the
	body hanging 1 m under it, 12 m x 4 m x 3 m; its bogie in the slot"""
	A = Asset ('train_hangrail_car')
	car_body (A, 12.0, 4.0, -4.0, 3.0, 1.6)
	A.add (box (0.3, 0.95, 1.2, (0, -1.0, -3.0)), STEEL)   # (hanger arms, up through the slot)
	A.add (box (0.3, 0.95, 1.2, (0, -1.0, 3.0)), STEEL)
	A.add (box (0.35, 0.5, 9.0, (0, 0.05, 0)), CAR_DARK)   # (the bogie, inside the beam)
	A.add (box (2.6, 0.3, 9.5, (0, -1.15, 0)), CAR_DARK)   # (the roof's equipment)
	A.light ((0, -2.0, 6.05), WHITE, 0.6); A.light ((0, -2.0, -6.05), RED, 0.5)
	return A

def a_monorail_beam ():
	"""a span of the ground monorail's guideway (TRAIN1) from z 0 to 100 (the
	client stretches it): a concrete beam 1.6 m x 1.2 m on low piers"""
	A = Asset ('train_monorail_beam')
	L = 100.0
	A.add (box (1.6, 0.9, L, (0, 0.3, L / 2)), CONCRETE)
	for k in range (5):
		A.add (box (2.4, 0.35, 1.2, (0, 0.0, (k + 0.5) * L / 5)), CONCRETE)
	A.add (box (0.25, 0.04, L, (0, 1.2, L / 2)), STEEL, role='detail')   # (the power rail)
	return A

def a_monorail_car ():
	"""a monorail car (TRAIN1) straddling its guideway: 20 m x 3.2 m, its
	floor over the beam (1.2 m), skirts down its sides"""
	A = Asset ('train_monorail_car')
	car_body (A, 20.0, 3.2, 1.4, 3.2, 2.4)
	for s in (-1, 1):
		A.add (box (0.25, 1.1, 16.0, (s * 1.05, 0.35, 0)), CAR_DARK)   # (skirts over the guideway's sides)
	A.light ((0, 2.0, 10.05), WHITE, 0.7); A.light ((0, 2.0, -10.05), RED, 0.5)
	return A

# ----------------------------------------------------------------------
# Earth (basekit.py: the launch complexes of the Earth bases; each keeps
# the footprint, origin and front (+z) of the mesh it replaces)

CLAD = KitMat ('cladding', 'kit_cladding', (0.86, 0.87, 0.86))
CLAD_BLUE = KitMat ('cladding_blue', 'kit_cladding', (0.24, 0.40, 0.66))
CLAD_DARK = KitMat ('cladding_dark', 'kit_cladding', (0.40, 0.42, 0.44))
DOOR = KitMat ('door', 'kit_cladding', (0.70, 0.72, 0.73))
ROOFING = KitMat ('roofing', 'kit_concrete', (0.52, 0.53, 0.54))
PAINT_WHITE = KitMat ('paint_white', 'kit_panel', (0.95, 0.95, 0.94))
STEEL_RED = KitMat ('steel_red', 'kit_steel', (0.78, 0.24, 0.16))
STEEL_GREY = KitMat ('steel_grey', 'kit_steel', (0.62, 0.64, 0.66))
FIBRE = KitMat ('fibreglass', 'kit_panel', (0.95, 0.93, 0.86))

def lattice (A, x0, x1, z0, z1, y0, y1, step, mat, w=0.6, faces=(True, True, True, True), role='detail'):
	"""a square lattice tower: corner columns, girts every step, X braces on each face"""
	cs = [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
	parts = []
	for (x, z) in cs: parts.append (box (w, y1 - y0, w, (x, y0, z)))
	n = max (1, int (round ((y1 - y0) / step)))
	for i in range (n + 1):
		y = y0 + (y1 - y0) * i / n
		for k in range (4):
			a, b = cs[k], cs[(k + 1) % 4]
			parts.append (rod_between ((a[0], y, a[1]), (b[0], y, b[1]), w * 0.32))
	for i in range (n):
		ya, yb = y0 + (y1 - y0) * i / n, y0 + (y1 - y0) * (i + 1) / n
		for k in range (4):
			if not faces[k]: continue
			a, b = cs[k], cs[(k + 1) % 4]
			parts.append (rod_between ((a[0], ya, a[1]), (b[0], yb, b[1]), w * 0.22))
			parts.append (rod_between ((b[0], ya, b[1]), (a[0], yb, a[1]), w * 0.22))
	A.add (merge (*parts), mat, role=role)

def shed (A, x, z, w, d, h, wall=CLAD, roof=ROOFING, parapet=0.6, rot_y=0.0):
	"""a flat-roofed box of a building: clad walls, a roof inset in its parapet"""
	bm = box (w, h, d, (x, 0, z), rot_y, bottom=False)
	for f in [f for f in bm.faces if f.normal.y > 0.9]: bm.faces.remove (f)  # (its top: the roof's)
	A.add (bm, wall)
	A.add (box (w, 0.05, d, (x, h - 0.05, z), rot_y), roof)
	# (the parapet: a low wall about the roof, capped)
	c, sn = math.cos (math.radians (rot_y)), math.sin (math.radians (rot_y))
	for (px, pz, sx, sz) in ((0, d / 2 - 0.15, w, 0.3), (0, -d / 2 + 0.15, w, 0.3), (w / 2 - 0.15, 0, 0.3, d), (-w / 2 + 0.15, 0, 0.3, d)):
		A.add (box (sx, parapet, sz, (x + c * px + sn * pz, h, z - sn * px + c * pz), rot_y), wall, role='detail')
		A.add (box (sx + 0.1, 0.08, sz + 0.1, (x + c * px + sn * pz, h + parapet, z - sn * px + c * pz), rot_y), STEEL_GREY, role='detail')

def window_band (A, x0, x1, y, h, z, side=1, mat=None):
	"""a band of windows on a face at z (side: +1 facing +z, -1 facing -z)"""
	A.add (box (x1 - x0, h, 0.06, ((x0 + x1) / 2, y, z + side * 0.03)), mat or GLASS)

def window_band_x (A, z0, z1, y, h, x, side=1):
	A.add (box (0.06, h, z1 - z0, (x + side * 0.03, y, (z0 + z1) / 2)), GLASS)

def a_tower (colour):
	"""service tower beside a pad: a lattice 10 m square to 77 m, platforms every
	14 m, two swing arms towards +z (the pad), a lift shaft at the back, a hammerhead
	crane, a lightning rod; its collision: the tower's and the shaft's boxes"""
	A = Asset ('tower_' + colour)
	m = STEEL_RED if colour == 'red' else STEEL_GREY
	A.add (box (12.0, 0.6, 12.0, (0, -0.3, 0)), CONCRETE)
	lattice (A, -5, 5, -5, 5, 0, 77, 7, m, w=0.7, role='drawn')
	for y in range (14, 78, 14):
		A.add (box (11.0, 0.35, 11.0, (0, y - 0.35, 0)), GRATING, role='detail')
		A.add (box (11.0, 0.25, 0.25, (0, y - 0.6, 5.4)), m, role='detail')
		A.add (box (11.0, 0.25, 0.25, (0, y - 0.6, -5.4)), m, role='detail')
		handrail (A, (-5.4, y, 5.4), (5.4, y, 5.4))
	A.add (box (3.0, 80.0, 4.0, (0, 0, -7.0)), CLAD)                       # (the lift shaft)
	A.add (box (3.2, 0.3, 4.2, (0, 80.0, -7.0)), STEEL_GREY, role='detail')
	for y in (42.0, 63.0):
		A.add (truss ((0, y + 1.2, 5.0), (0, y + 1.2, 17.5), 2.4, 2.4, 1.6, 0.08), m, role='detail')
		A.add (box (2.6, 0.2, 12.5, (0, y, 11.25)), GRATING, role='detail')
		A.add (box (4.0, 3.5, 2.5, (0, y - 0.5, 18.75)), PAINT_WHITE)     # (its hood)
	A.add (box (1.6, 13.0, 1.6, (0, 77, 0)), m)                            # (the crane: mast, jib, cab)
	A.add (box (1.4, 1.6, 26.0, (0, 88, 5.0)), m)
	A.add (box (2.2, 2.0, 2.4, (0, 86, -2.0)), PAINT_WHITE)
	A.add (cylinder (0.12, 6.0, 6, (0, 90, 0)), STEEL_GREY, role='detail')
	A.add (sphere (0.25, (0, 96.2, 0), 8, 6), KitMat ('beacon', None, RED, emissive=RED), role='detail')
	A.light ((0, 96.3, 0), RED, 1.4)
	for y in (40.0, 77.5):
		for (x, z) in ((-5.3, -5.3), (5.3, 5.3)): A.light ((x, y, z), RED, 1.0)
	proxy_box (A, 11, 77, 11, (0, 0, 0)); proxy_box (A, 3, 80, 4, (0, 0, -7)); proxy_box (A, 4, 4, 15, (0, 41.5, 12)); proxy_box (A, 4, 4, 15, (0, 62.5, 12))
	return A

def a_earth_mast ():
	"""lightning mast: a tapering steel pole to 100 m, a fibreglass top to 115,
	its footing; obstruction lights"""
	A = Asset ('mast')
	A.add (box (3.6, 1.0, 3.6, (0, 0, 0)), CONCRETE)
	A.add (cylinder (1.2, 100.0, 16, (0, 1.0, 0), r2=0.5), STEEL_GREY, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (0.35, 14.0, 10, (0, 101.0, 0), r2=0.1), FIBRE, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	for y in (35.0, 70.0):
		A.add (cylinder (1.5 - 0.007 * y, 0.3, 16, (0, y, 0)), STEEL_GREY, role='detail')
	for y in (50.0, 100.5):
		A.light ((0, y, 0), RED, 1.2)
	A.add (sphere (0.25, (0, 100.6, 0), 8, 6), KitMat ('beacon', None, RED, emissive=RED), role='detail')
	return A

def a_watertower ():
	"""deluge water tower: four raked legs, braced, a tank of 9 m radius at 45 m with
	a conical roof, a catwalk, the riser"""
	A = Asset ('watertower')
	legs = []
	for (x, z) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
		A.add (box (2.0, 1.0, 2.0, (x * 6.6, 0, z * 6.6)), CONCRETE)
		legs.append (rod_between ((x * 6.6, 1.0, z * 6.6), (x * 4.5, 45.0, z * 4.5), 0.55, 8))
	A.add (merge (*legs), STEEL_GREY)
	for i in range (3):  # (girts and braces between the legs)
		ya, yb = 1.0 + 44.0 * i / 3, 1.0 + 44.0 * (i + 1) / 3
		ra, rb = 6.6 - 2.1 * i / 3, 6.6 - 2.1 * (i + 1) / 3
		for k in range (4):
			a = [(-1, -1), (1, -1), (1, 1), (-1, 1)][k]; b = [(-1, -1), (1, -1), (1, 1), (-1, 1)][(k + 1) % 4]
			A.add (rod_between ((a[0] * rb, yb, a[1] * rb), (b[0] * rb, yb, b[1] * rb), 0.18), STEEL_GREY, role='detail')
			A.add (rod_between ((a[0] * ra, ya, a[1] * ra), (b[0] * rb, yb, b[1] * rb), 0.12), STEEL_GREY, role='detail')
	A.add (cylinder (0.6, 45.0, 10, (0, 0, 0)), STEEL_GREY)                     # (the riser)
	A.add (cylinder (9.0, 13.0, 40, (0, 45.0, 0)), PAINT_WHITE, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (9.0, 4.0, 40, (0, 58.0, 0), r2=1.0), PAINT_WHITE)
	A.add (cylinder (9.9, 0.2, 40, (0, 45.0, 0)), GRATING, role='detail')
	A.add (sphere (0.25, (0, 62.2, 0), 8, 6), KitMat ('beacon', None, RED, emissive=RED), role='detail')
	A.light ((0, 62.3, 0), RED, 1.2)
	proxy_box (A, 13, 45, 13, (0, 0, 0)); proxy_box (A, 17, 17, 17, (0, 45, 0))
	return A

def a_spheres ():
	"""propellant storage: two spheres of 10 m on eight legs each, a pump house
	towards +z, pipes"""
	A = Asset ('spheres')
	for cx in (-14.0, 14.0):
		A.add (sphere (10.0, (cx, 13.5, 0), 32, 16), PAINT_WHITE, uv='cyl', axis=(0, 1, 0), centre=(cx, 0, 0))
		legs = [rod_between ((cx + 8.6 * math.sin (2 * math.pi * k / 8), 0, 8.6 * math.cos (2 * math.pi * k / 8)),
			(cx + 8.0 * math.sin (2 * math.pi * k / 8), 11.0, 8.0 * math.cos (2 * math.pi * k / 8)), 0.35, 8) for k in range (8)]
		A.add (merge (*legs), STEEL_GREY)
		A.add (cylinder (9.0, 0.4, 32, (cx, 0, 0)), CONCRETE)
		A.add (cylinder (0.3, 1.2, 8, (cx, 23.5, 0)), STEEL_GREY, role='detail')
	shed (A, 0, 18.0, 8.0, 8.0, 5.0)
	A.add (box (2.4, 2.6, 0.08, (0, 0, 22.04)), DOOR, role='detail')
	for y in (2.0, 3.0):
		A.add (rod_between ((-14, y, 0), (14, y, 0), 0.3, 8), STEEL_GREY, role='detail')
		A.add (rod_between ((0, y, 0), (0, y, 14.0), 0.3, 8), STEEL_GREY, role='detail')
	A.add (box (1.0, 1.6, 1.0, (0, 0, 0)), STEEL_GREY, role='detail')
	proxy_box (A, 20, 23.5, 20, (-14, 0, 0)); proxy_box (A, 20, 23.5, 20, (14, 0, 0)); proxy_box (A, 8, 5, 8, (0, 0, 18))
	return A

def a_tanks ():
	"""fuel farm: three horizontal tanks of 3.2 m radius on saddles, within a bund"""
	A = Asset ('tanks')
	for k in range (3):
		z = (k - 1) * 9.0
		A.add (cylinder (3.2, 26.0, 28, (-13.0, 4.5, z), axis='x'), PAINT_WHITE, uv='cyl', axis=(1, 0, 0), centre=(0, 4.5, z))
		for x in (-9.0, 0.0, 9.0): A.add (box (1.2, 2.2, 4.8, (x, 0, z)), CONCRETE)
		A.add (box (1.2, 1.0, 0.8, (11.0, 7.6, z)), STEEL_GREY, role='detail')            # (a manway)
		A.add (rod_between ((13.0, 2.0, z), (14.0, 2.0, z), 0.2, 6), STEEL_GREY, role='detail')
	A.add (rod_between ((14.0, 2.0, -9.0), (14.0, 2.0, 9.0), 0.25, 8), STEEL_GREY, role='detail')
	for (sx, sz, x, z) in ((30, 1, 0, -14.5), (30, 1, 0, 14.5), (1, 28, -14.5, 0), (1, 28, 14.5, 0)):
		A.add (box (sx, 0.8, sz, (x, 0, z)), CONCRETE)
	A.add (box (28, 0.05, 28, (0, 0.02, 0)), CONCRETE, role='detail')
	proxy_box (A, 26, 7.7, 22, (0, 0, 0))
	return A

def a_assembly (name, w, d, h):
	"""vertical assembly building: a high bay w x d x h, two tall doors on its
	front (+z) and back, a blue band under the roof, a low annex (40 m to +x, 18 m
	high) with an office's windows, roof plant, obstruction lights"""
	A = Asset (name)
	shed (A, 0, 0, w, d, h, wall=CLAD)
	for side in (-1, 1):
		A.add (box (0.06, 6.0, d, (side * (w / 2 + 0.03), h - 10.0, 0)), CLAD_BLUE)   # (the band, the sides)
		A.add (box (w, 6.0, 0.06, (0, h - 10.0, side * (d / 2 + 0.03))), CLAD_BLUE)   # (and the ends)
		for x in (-w / 4, w / 4):  # (the doors: segments, a frame)
			dz = side * (d / 2 + 0.05)
			A.add (box (18.0, h - 8.0, 0.1, (x, 0, dz)), DOOR)
			for k in range (1, 6):
				A.add (box (18.0, 0.25, 0.14, (x, (h - 8.0) * k / 6, dz)), CLAD_DARK, role='detail')
			A.add (box (19.6, 0.8, 0.4, (x, h - 8.0, dz)), STEEL_GREY, role='detail')
			A.add (box (0.8, h - 8.0, 0.4, (x - 9.4, 0, dz)), STEEL_GREY, role='detail')
			A.add (box (0.8, h - 8.0, 0.4, (x + 9.4, 0, dz)), STEEL_GREY, role='detail')
			A.light ((x - 9.6, h - 7.0, dz + side * 0.6), WHITE, 0.8); A.light ((x + 9.6, h - 7.0, dz + side * 0.6), WHITE, 0.8)
	# the annex: offices, workshops (from the high bay to +x 40 m, back from its front by 10 m)
	ax0, ax1, az0, az1 = w / 2, w / 2 + 40.0, -d / 2, d / 2 - 10.0
	shed (A, (ax0 + ax1) / 2, (az0 + az1) / 2, ax1 - ax0, az1 - az0, 18.0, wall=PAINT_WHITE)
	for y in (3.0, 7.0, 11.0, 15.0):
		window_band (A, ax0 + 2.0, ax1 - 2.0, y, 1.6, az1, side=1)
		window_band (A, ax0 + 2.0, ax1 - 2.0, y, 1.6, az0, side=-1)
		window_band_x (A, az0 + 2.0, az1 - 2.0, y, 1.6, ax1, side=1)
	A.add (box (4.0, 3.0, 0.1, (ax0 + 20.0, 0, az1 + 0.05)), GLASS)                  # (its entrance)
	A.light ((ax0 + 20.0, 3.5, az1 + 0.6), WHITE, 0.6)
	for (x, z) in ((-w / 4, -d / 4), (w / 6, d / 6)):  # (roof plant)
		A.add (box (8.0, 2.5, 5.0, (x, h, z)), CLAD_DARK, role='detail')
	for (x, z) in ((-w / 2 + 0.5, -d / 2 + 0.5), (w / 2 - 0.5, -d / 2 + 0.5), (-w / 2 + 0.5, d / 2 - 0.5), (w / 2 - 0.5, d / 2 - 0.5)):
		A.light ((x, h + 0.6, z), RED, 1.2)
	proxy_box (A, w, h, d, (0, 0, 0)); proxy_box (A, ax1 - ax0, 18.0, az1 - az0, ((ax0 + ax1) / 2, 0, (az0 + az1) / 2))
	return A

def a_earth_hangar ():
	"""processing facility: a hangar 48 x 40 m, its low-pitched roof to 22 m, a
	high door on its front (+z); an office wing at its back"""
	A = Asset ('hangar')
	w, d, h = 48.0, 40.0, 20.0
	A.add (box (w, h, d, (0, 0, 0)), CLAD)
	# (the roof: two slopes to a ridge along z, 2 m up)
	bm = kl._bm ()
	v = [bm.verts.new (p) for p in ((-w / 2, h, -d / 2), (0, h + 2.0, -d / 2), (w / 2, h, -d / 2), (-w / 2, h, d / 2), (0, h + 2.0, d / 2), (w / 2, h, d / 2))]
	bm.faces.new ((v[0], v[1], v[4], v[3])); bm.faces.new ((v[1], v[2], v[5], v[4]))
	bm.faces.new ((v[2], v[1], v[0])); bm.faces.new ((v[3], v[4], v[5]))
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	A.add (bm, CLAD_DARK)
	A.add (box (28.0, 17.0, 0.12, (0, 0, d / 2 + 0.06)), DOOR)
	for k in range (1, 7): A.add (box (0.2, 17.0, 0.16, (-14.0 + 4.0 * k, 0, d / 2 + 0.08)), CLAD_DARK, role='detail')
	A.add (box (29.6, 0.8, 0.4, (0, 17.0, d / 2 + 0.2)), STEEL_GREY, role='detail')
	A.add (box (0.06, 1.2, 32.0, (w / 2 + 0.03, h - 3.0, 0)), GLASS); A.add (box (0.06, 1.2, 32.0, (-w / 2 - 0.03, h - 3.0, 0)), GLASS)
	A.light ((-14.5, 18.0, d / 2 + 0.5), WHITE, 0.8); A.light ((14.5, 18.0, d / 2 + 0.5), WHITE, 0.8)
	shed (A, 0, -d / 2 - 6.0, w, 12.0, 8.0, wall=PAINT_WHITE)
	for y in (1.2, 4.8):
		window_band (A, -w / 2 + 2, w / 2 - 2, y, 1.5, -d / 2 - 12.0, side=-1)
	A.add (box (3.0, 2.6, 0.1, (10.0, 0, -d / 2 - 12.05)), GLASS)
	proxy_box (A, w, h + 2.0, d, (0, 0, 0)); proxy_box (A, w, 8.0, 12.0, (0, 0, -d / 2 - 6.0))
	return A

def a_office ():
	"""an office building 40 x 16 m, three floors of ribbon windows, plant on its roof"""
	A = Asset ('office')
	w, d, h = 40.0, 16.0, 11.0
	shed (A, 0, 0, w, d, h, wall=PAINT_WHITE)
	for y in (1.0, 4.6, 8.2):
		for side in (-1, 1):
			window_band (A, -w / 2 + 1.5, w / 2 - 1.5, y, 1.8, side * d / 2, side=side)
		window_band_x (A, -d / 2 + 1.5, d / 2 - 1.5, y, 1.8, w / 2, side=1)
		window_band_x (A, -d / 2 + 1.5, d / 2 - 1.5, y, 1.8, -w / 2, side=-1)
	A.add (box (5.0, 2.8, 0.12, (0, 0, d / 2 + 0.06)), GLASS)        # (the entrance)
	A.add (box (6.0, 0.3, 1.2, (0, 3.0, d / 2 - 0.6)), STEEL_GREY, role='detail')
	A.light ((0, 3.4, d / 2 + 0.4), WHITE, 0.6)
	A.add (box (8.0, 3.0, 6.0, (0, h, 0)), CLAD_DARK)                # (plant)
	proxy_box (A, w, h + 3.0, d, (0, 0, 0))
	return A

def a_control ():
	"""blockhouse: a concrete bunker 40 x 24 m under a mound, its entrance at +z,
	periscopes, an antenna mast"""
	A = Asset ('control')
	A.add (box (40.0, 6.0, 24.0, (0, 0, 0)), CONCRETE)
	bm = kl._bm ()  # (the mound: a truncated pyramid over the roof)
	lo = [bm.verts.new (p) for p in ((-20, 6, -12), (20, 6, -12), (20, 6, 12), (-20, 6, 12))]
	hi = [bm.verts.new (p) for p in ((-11, 10.8, -6), (11, 10.8, -6), (11, 10.8, 6), (-11, 10.8, 6))]
	for i in range (4):
		j = (i + 1) % 4
		bm.faces.new ((lo[i], lo[j], hi[j], hi[i]))
	bm.faces.new (hi)
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	A.add (bm, CONCRETE)
	A.add (box (6.0, 3.0, 4.0, (0, 0, 14.0)), CONCRETE)             # (the entrance)
	A.add (box (2.2, 2.4, 0.1, (0, 0, 16.0)), DOOR)
	A.light ((0, 2.8, 16.3), WHITE, 0.6)
	for x in (-6.0, 0.0, 6.0):
		A.add (cylinder (0.35, 1.6, 10, (x, 10.8, 3.0)), STEEL_GREY, role='detail')
	A.add (truss ((12.0, 10.8, 0), (12.0, 30.0, 0), 0.5, 0.5, 1.0, 0.04), STEEL_GREY)
	A.light ((12.0, 30.2, 0), RED, 1.0)
	proxy_box (A, 40, 10.8, 24, (0, 0, 0)); proxy_box (A, 6, 3, 4, (0, 0, 14))
	return A

def dish (r, depth, n=24, rings=6):
	"""a paraboloid dish of radius r, depth deep, opening +y, its vertex at the origin"""
	bm = kl._bm ()
	centre = bm.verts.new ((0, 0, 0))
	prev = None
	rows = []
	for i in range (1, rings + 1):
		rr = r * i / rings
		y = depth * (rr / r) ** 2
		rows.append ([bm.verts.new ((rr * math.cos (2 * math.pi * k / n), y, rr * math.sin (2 * math.pi * k / n))) for k in range (n)])
	for k in range (n):
		bm.faces.new ((centre, rows[0][k], rows[0][(k + 1) % n]))
	for a, b in zip (rows, rows[1:]):
		for k in range (n):
			bm.faces.new ((a[k], b[k], b[(k + 1) % n], a[(k + 1) % n]))
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces[:])
	# (two-sided: a back face too)
	back = bm.copy ()
	bmesh.ops.reverse_faces (back, faces=back.faces[:])
	return merge (bm, back)

def a_radome ():
	"""tracking station: a radome on its building (16 x 16 x 9 m), a dish on its
	pedestal beside it (+x)"""
	A = Asset ('radome')
	shed (A, 0, 0, 16.0, 16.0, 9.0, wall=PAINT_WHITE)
	window_band (A, -6.0, 6.0, 3.0, 1.4, 8.0, side=1)
	A.add (box (2.2, 2.4, 0.1, (4.0, 0, 8.05)), DOOR)
	A.add (sphere (7.5, (0, 13.0, 0), 32, 16, cut=-3.75), FIBRE, uv='cyl', axis=(0, 1, 0), centre=(0, 0, 0))
	A.add (cylinder (7.6, 0.6, 32, (0, 9.0, 0)), STEEL_GREY, role='detail')
	A.add (cylinder (1.2, 6.0, 12, (16.0, 0, 0)), PAINT_WHITE)
	A.add (box (2.4, 1.0, 2.4, (16.0, 6.0, 0)), STEEL_GREY)
	bm = dish (5.0, 1.4)
	kl.transform (bm, kl.Matrix.Rotation (math.radians (-30), 4, 'Z'))  # (looking up, away from the building)
	kl.transform (bm, kl.Matrix.Translation (kl.Vector ((16.0, 7.5, 0))))
	A.add (bm, PAINT_WHITE)
	# (its feed: up the dish's axis, tilted 30 degrees towards +x)
	A.add (rod_between ((16.0, 7.5, 0), (16.0 + 4.0 * math.sin (math.radians (30)), 7.5 + 4.0 * math.cos (math.radians (30)), 0), 0.08), STEEL_GREY, role='detail')
	A.light ((0, 20.6, 0), RED, 1.0)
	proxy_box (A, 16, 9, 16, (0, 0, 0)); proxy_box (A, 15, 11.5, 15, (0, 9, 0)); proxy_box (A, 10, 10, 10, (16, 2.5, 0))
	return A

# ----------------------------------------------------------------------
# Cape Canaveral: launch complex 39 in its modern form (the base's frame,
# the pads' meshes unturned: x south, y up, z east; y = 0 the ground at the
# pad's origin (Canaveral.cfg BAREEARTH_KEEP holds it level about the pads),
# the hardstand 16 m up on its mound, the flame trench north-south under it)

# (the pad's surfaces - hardstand, the mound's flanks and the ramp paved with concrete slope panels as at the
# real pads, the crawler's lanes - and the launcher platform: painted once, as a whole, on a sheet of their
# own (capesurf.py cape_pad) and mapped onto it from above: nothing tiles)
import capesurf
PAD_SHEET = KitMat ('cape_pad', 'cape_pad', (1, 1, 1), tile=1.0)
PAD_SHEET_DARK = KitMat ('cape_pad_dark', 'cape_pad', (0.40, 0.40, 0.41), tile=1.0)   # (the platform's underside, its holes' walls)
REFRACTORY = KitMat ('refractory', 'kit_concrete', (0.46, 0.45, 0.44))
TOWER_STEEL = KitMat ('tower_steel', 'kit_steel', (0.70, 0.71, 0.72))

PAD_H = 16.0                        # (the hardstand)
PAD_X, PAD_Z = (-95.0, 95.0), (-56.0, 64.0)   # (its extent)
SLOPE = 2.0                         # (the mound's sides: run per rise)
TR_Z = (-5.1, 12.9)                 # (the flame trench's sides)
TR_OPEN = (-25.0, 22.0)             # (its opening under the vehicle)
TR_X0 = -135.0                      # (its exit, north of the mound)
RAMP_X1, RAMP_W = 400.0, 44.0       # (the crawlerway's ramp: its foot, its width)
LANES = (3.9 - 13.7, 3.9 + 13.7)    # (the crawler's lanes: their centres, 12 m wide)
# (the trench's floor: x, y - the main flame deflector rising south to under the
# vehicle, the trench closed south of it (the shuttle's second deflector filled in))
TR_FLOOR = ((TR_X0, 0.05), (-57.4, 0.05), (-21.9, 2.0), (-13.2, 5.0), (3.9, 14.0), (TR_OPEN[1], 14.0))
# (the shuttle's mobile launcher platform: deck, holes, the boosters' skirts as
# Atlantis stands on it: "Atlantis satellite Launch", ALT 47.361 - 18.962)
MLP_X, MLP_Z, MLP_Y = (-30.5, 19.5), (-17.12, 24.88), (22.7, 30.3)
MLP_HOLES = ((-6.85, 5.0, -5.28, 0.62), (-6.85, 5.0, 7.12, 13.03), (7.5, 17.4, -0.78, 8.53))
SKIRT_Y = 28.39
assert (PAD_H, PAD_X, PAD_Z, SLOPE, TR_Z, TR_OPEN, RAMP_X1, RAMP_W, LANES, MLP_X, MLP_Z, MLP_Y, MLP_HOLES) == (capesurf.PAD_H, capesurf.PAD_X,
	capesurf.PAD_Z, capesurf.SLOPE, capesurf.TR_Z, capesurf.TR_OPEN, capesurf.RAMP_X1, capesurf.RAMP_W, capesurf.LANES, capesurf.MLP_X,
	capesurf.MLP_Z, capesurf.MLP_Y, capesurf.MLP_HOLES) and (TR_X0, TR_FLOOR) == (capesurf.TR_X0, capesurf.TR_FLOOR), 'capesurf.py paints another pad'

def sheet_part (A, bm, sheet, name, ua, ub, mat, role='drawn'):
	"""a part mapped onto a region of one of the Cape's sheets (capesurf.py REGIONS): ua, ub the
	directions (unit, Orbiter's frame) of the region's a and b - a pad from above: x, z; a wall:
	along it, up"""
	r, rect = capesurf.region (sheet, name)
	ua, ub = Vector (ua), Vector (ub)
	return A.add (bm, mat, role=role, uv='decal', axis=(tuple (ua * r.a0 + ub * r.b0), tuple (ua * (r.a1 - r.a0)), tuple (ub * (r.b1 - r.b0)), rect))

def pad_part (A, bm, name, role='drawn'):
	return sheet_part (A, bm, 'cape_pad', name, (1, 0, 0), (0, 0, 1), PAD_SHEET, role)

def quad (bm, p, up=None):
	"""a face of the points p; up: a direction its normal must face (else as given)"""
	f = bm.faces.new ([bm.verts.new (q) for q in p])
	f.normal_update ()
	if up is not None and f.normal.dot (Vector (up)) < 0: f.normal_flip ()
	return f

def floor_y (x):
	pts = TR_FLOOR
	if x <= pts[0][0]: return pts[0][1]
	for (x0, y0), (x1, y1) in zip (pts, pts[1:]):
		if x <= x1: return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
	return pts[-1][1]

def ramp_y (x):
	"""the ramp's surface (and the hardstand's) along x"""
	return PAD_H if x <= PAD_X[1] else max (0.0, PAD_H * (RAMP_X1 - x) / (RAMP_X1 - PAD_X[1]))

def _split (a, b, n): return [a + (b - a) * k / n for k in range (n + 1)]

def hardstand (A):
	"""the hardstand: slabs of about 8 m (the trench's opening whole slabs left out), their joints
	where the sheet paints them (capesurf.hardstand_joints)"""
	xs, zs = capesurf.hardstand_joints ()
	H = PAD_H
	bm = kl._bm ()
	for (xa, xb) in zip (xs, xs[1:]):
		for (za, zb) in zip (zs, zs[1:]):
			xm, zm = (xa + xb) / 2, (za + zb) / 2
			if TR_OPEN[0] < xm < TR_OPEN[1] and TR_Z[0] < zm < TR_Z[1]: continue
			quad (bm, [(xa, H, za), (xb, H, za), (xb, H, zb), (xa, H, zb)], up=(0, 1, 0))
	pad_part (A, bm, 'mound')

def pad_mound (A, lanes_from=30.0):
	"""the mound: hardstand (the trench open under the vehicle), sides paved with concrete,
	the trench's walls and its exit north, the ramp up from the south with the
	crawler's gravel lanes - hardstand, sides, ramp and lanes all one painting (capesurf.py)"""
	(x0, x1), (z0, z1) = PAD_X, PAD_Z
	H, s = PAD_H, SLOPE
	hardstand (A)
	# (the sides: a frustum, its north face cut by the trench's exit)
	X0, X1, Z0, Z1 = x0 - s * H, x1 + s * H, z0 - s * H, z1 + s * H
	# (the sheet's regions: the mound's - hardstand and flanks, the ramp to CUT -, the ramp's beyond, the lanes' on the mound)
	CUT = capesurf.RAMP_CUT
	mound, ramp, lanes = kl._bm (), kl._bm (), [kl._bm (), kl._bm ()]
	def side (faces, up):
		for f in faces: quad (mound if max (p[0] for p in f) <= CUT + 1e-6 else ramp, f, up=up)
	side ([[(x0, H, z0), (X0, 0, Z0), (X0, 0, TR_Z[0]), (x0, H, TR_Z[0])], [(x0, H, TR_Z[1]), (X0, 0, TR_Z[1]), (X0, 0, Z1), (x0, H, z1)]], (-1, 1, 0))
	side ([[(x1, H, z0), (X1, 0, Z0), (X1, 0, Z1), (x1, H, z1)]], (1, 1, 0))
	side ([[(x0, H, z0), (x1, H, z0), (X1, 0, Z0), (X0, 0, Z0)]], (0, 1, -1))
	side ([[(x0, H, z1), (x1, H, z1), (X1, 0, Z1), (X0, 0, Z1)]], (0, 1, 1))
	# (the ramp: its sides down to the ground, the paving between and beside the lanes; cut where the regions meet)
	rz0, rz1 = 3.9 - RAMP_W / 2, 3.9 + RAMP_W / 2
	xs = sorted (set ([x1 + (RAMP_X1 - x1) * k / 8 for k in range (9)] + [CUT]))
	side ([[(xa, ramp_y (xa), rz0), (xb, ramp_y (xb), rz0), (xb, 0, rz0 - s * ramp_y (xb)), (xa, 0, rz0 - s * ramp_y (xa))] for (xa, xb) in zip (xs, xs[1:])], (0, 1, -1))
	side ([[(xa, ramp_y (xa), rz1), (xb, ramp_y (xb), rz1), (xb, 0, rz1 + s * ramp_y (xb)), (xa, 0, rz1 + s * ramp_y (xa))] for (xa, xb) in zip (xs, xs[1:])], (0, 1, 1))
	edges = [rz0, LANES[0] - 6, LANES[0] + 6, LANES[1] - 6, LANES[1] + 6, rz1]
	for (xa, xb) in zip (xs, xs[1:]):
		ya, yb = ramp_y (xa), ramp_y (xb)
		on_mound = xb <= CUT + 1e-6
		for i, (za, zb) in enumerate (zip (edges, edges[1:])):
			quad ((lanes[i // 2] if on_mound else ramp) if i % 2 else (mound if on_mound else ramp), [(xa, ya, za), (xb, yb, za), (xb, yb, zb), (xa, ya, zb)], up=(0, 1, 0))
	for k, z in enumerate (LANES):  # (on the hardstand to the vehicle; past the ramp's foot)
		quad (lanes[k], [(lanes_from, H + 0.03, z - 6), (x1, H + 0.03, z - 6), (x1, H + 0.03, z + 6), (lanes_from, H + 0.03, z + 6)], up=(0, 1, 0))
		quad (ramp, [(RAMP_X1, 0.04, z - 6), (RAMP_X1 + 60, 0.04, z - 6), (RAMP_X1 + 60, 0.04, z + 6), (RAMP_X1, 0.04, z + 6)], up=(0, 1, 0))
		pad_part (A, lanes[k], 'lane%d' % k)
	pad_part (A, mound, 'mound'); pad_part (A, ramp, 'ramp')
	# (the trench's walls: from its floor to the hardstand, or to the mound's side north of it - their sides painted
	# (capesurf.py trench_w, trench_e), the end and the tunnel's roof refractory)
	walls, side_w, side_e = kl._bm (), kl._bm (), kl._bm ()
	wx = sorted (set ([TR_X0, X0, x0, TR_OPEN[0], TR_OPEN[1]] + [p[0] for p in TR_FLOOR]))
	topy = lambda x: H if x >= x0 else max (0.0, H - (x0 - x) / s)
	for (xa, xb) in zip (wx, wx[1:]):
		if xb > TR_OPEN[1] + 1e-6: break
		for z, sgn in ((TR_Z[0], 1), (TR_Z[1], -1)):
			if topy (xa) - floor_y (xa) < 0.01 and topy (xb) - floor_y (xb) < 0.01: continue
			quad (side_w if sgn > 0 else side_e, [(xa, floor_y (xa), z), (xb, floor_y (xb), z), (xb, topy (xb), z), (xa, topy (xa), z)], up=(0, 0, sgn))
	quad (walls, [(TR_OPEN[1], floor_y (TR_OPEN[1]), TR_Z[0]), (TR_OPEN[1], H, TR_Z[0]), (TR_OPEN[1], H, TR_Z[1]), (TR_OPEN[1], floor_y (TR_OPEN[1]), TR_Z[1])], up=(-1, 0, 0))
	quad (walls, [(x0, H - 0.4, TR_Z[0]), (TR_OPEN[0], H - 0.4, TR_Z[0]), (TR_OPEN[0], H - 0.4, TR_Z[1]), (x0, H - 0.4, TR_Z[1])], up=(0, -1, 0))  # (the tunnel's roof)
	quad (walls, [(TR_OPEN[0], H - 0.4, TR_Z[0]), (TR_OPEN[0], H, TR_Z[0]), (TR_OPEN[0], H, TR_Z[1]), (TR_OPEN[0], H - 0.4, TR_Z[1])], up=(1, 0, 0))
	A.add (walls, REFRACTORY)
	sheet_part (A, side_w, 'cape_pad', 'trench_w', (1, 0, 0), (0, 1, 0), PAD_SHEET)
	sheet_part (A, side_e, 'cape_pad', 'trench_e', (1, 0, 0), (0, 1, 0), PAD_SHEET)
	# (hazard edging about the trench's opening)
	hz = kl._bm ()
	for (a, b, c, d) in ((TR_OPEN[0] - 0.8, TR_OPEN[1] + 0.8, TR_Z[0] - 0.8, TR_Z[0]), (TR_OPEN[0] - 0.8, TR_OPEN[1] + 0.8, TR_Z[1], TR_Z[1] + 0.8),
			(TR_OPEN[0] - 0.8, TR_OPEN[0], TR_Z[0], TR_Z[1]), (TR_OPEN[1], TR_OPEN[1] + 0.8, TR_Z[0], TR_Z[1])):
		quad (hz, [(a, H + 0.04, c), (b, H + 0.04, c), (b, H + 0.04, d), (a, H + 0.04, d)], up=(0, 1, 0))
	A.add (hz, HAZARD, role='detail')
	# collision: the hardstand, its sides, the ramp (proxies: the drawn mound, simple as it is, collides)

def a_pad_ground ():
	"""the flame trench's floor (KLC39Bground: both pads), the main flame deflector rising south: painted
	from above (capesurf.py trench_floor)"""
	A = Asset ('KLC39Bground')
	fl = kl._bm ()
	for (xa, ya), (xb, yb) in zip (TR_FLOOR, TR_FLOOR[1:]):
		quad (fl, [(xa, ya, TR_Z[0]), (xb, yb, TR_Z[0]), (xb, yb, TR_Z[1]), (xa, ya, TR_Z[1])], up=(0, 1, 0))
	pad_part (A, fl, 'trench_floor')
	return A

def pipe (A, pts, r, mat=STEEL_GREY, role='detail', n=8):
	A.add (merge (*[rod_between (a, b, r, n) for a, b in zip (pts, pts[1:])]), mat, role=role)

def wire (A, a, b, sag, r=0.12, n=16):
	"""a sagging wire (a parabola) from a to b"""
	pts = []
	for k in range (n + 1):
		t = k / n
		pts.append ((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t - 4 * sag * t * (1 - t), a[2] + (b[2] - a[2]) * t))
	pipe (A, pts, r, STEEL_GREY, n=4)

def tapered_lattice (A, cx, cz, y0, y1, w0, w1, levels, mat, col=0.5):
	"""a four-legged lattice tower tapering from w0 to w1 (half widths), girts and X braces"""
	hw = lambda y: w0 + (w1 - w0) * (y - y0) / (y1 - y0)
	cs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
	legs, br = [], []
	for (sx, sz) in cs: legs.append (rod_between ((cx + sx * w0, y0, cz + sz * w0), (cx + sx * w1, y1, cz + sz * w1), col, 6))
	for i in range (levels + 1):
		y = y0 + (y1 - y0) * i / levels
		for k in range (4):
			a, b = cs[k], cs[(k + 1) % 4]
			br.append (rod_between ((cx + a[0] * hw (y), y, cz + a[1] * hw (y)), (cx + b[0] * hw (y), y, cz + b[1] * hw (y)), col * 0.45))
	for i in range (levels):
		ya, yb = y0 + (y1 - y0) * i / levels, y0 + (y1 - y0) * (i + 1) / levels
		for k in range (4):
			a, b = cs[k], cs[(k + 1) % 4]
			br.append (rod_between ((cx + a[0] * hw (ya), ya, cz + a[1] * hw (ya)), (cx + b[0] * hw (yb), yb, cz + b[1] * hw (yb)), col * 0.3))
			br.append (rod_between ((cx + b[0] * hw (ya), ya, cz + b[1] * hw (ya)), (cx + a[0] * hw (yb), yb, cz + a[1] * hw (yb)), col * 0.3))
	A.add (merge (*legs), mat)
	A.add (merge (*br), mat, role='detail')

def lightning_tower (A, cx, cz):
	"""a lightning protection tower: a lattice of 150 m, a fibreglass mast to 180 m, aviation lights;
	returns its top"""
	for (sx, sz) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
		A.add (box (3.0, 1.2, 3.0, (cx + sx * 7.0, 0, cz + sz * 7.0)), CONCRETE)
	tapered_lattice (A, cx, cz, 1.2, 150.0, 7.0, 1.6, 25, TOWER_STEEL, col=0.45)
	A.add (cylinder (2.0, 1.0, 16, (cx, 150.0, cz)), TOWER_STEEL)
	A.add (cylinder (0.9, 30.0, 12, (cx, 151.0, cz), r2=0.35), FIBRE)
	for y in (60.0, 120.0, 181.2):
		A.light ((cx, y, cz), RED, 1.6)
	proxy_box (A, 15, 150, 15, (cx, 0, cz))
	return (cx, 180.5, cz)

def lathe (prof, cx, cz, n=40):
	"""a surface of revolution about the vertical through (cx, cz): prof the (radius, y) of its
	outline from bottom to top (a radius 0 closes it to a point)"""
	bm = kl._bm ()
	rings = []
	for (r, y) in prof:
		rings.append ([bm.verts.new ((cx + r * math.sin (2 * math.pi * k / n), y, cz + r * math.cos (2 * math.pi * k / n))) for k in range (n)])
	for ra, rb in zip (rings, rings[1:]):
		for k in range (n):
			vs = [ra[k], ra[(k + 1) % n], rb[(k + 1) % n], rb[k]]
			if len (set (v.co.to_tuple (5) for v in vs)) < 3: continue
			f = bm.faces.new (vs)
			f.smooth = True
	bmesh.ops.remove_doubles (bm, verts=bm.verts, dist=1e-4)
	bmesh.ops.recalc_face_normals (bm, faces=bm.faces)
	return bm

def pad_watertower (A, cx, cz):
	"""the sound suppression water tower as at 39A and 39B (88 m, 1.1 million litres): a
	capsule tank 75 m up on a standpipe of 4 m, a concrete plinth at its foot; six raked
	legs about it, ring girts and X braces in six panels, struts in to the pipe; returns its foot"""
	T0, T1, RT = 75.0, 88.0, 6.8      # (the tank: its bottom, its top, its radius)
	NL, R0, R1 = 6, 11.5, 6.4         # (the legs: at the ground, under the tank)
	A.add (box (9.0, 1.0, 9.0, (cx, 0, cz)), CONCRETE)
	A.add (box (7.0, 6.0, 7.0, (cx, 1.0, cz)), CONCRETE)
	A.add (cylinder (2.0, T0 - 6.5, 24, (cx, 7.0, cz)), PAINT_WHITE, uv='cyl', axis=(0, 1, 0), centre=(cx, 0, cz))
	# (the tank: elliptical heads 3 m deep, a cylinder between)
	hd = 3.0
	prof = [(RT * math.sin (math.pi / 2 * k / 6), T0 + hd * (1 - math.cos (math.pi / 2 * k / 6))) for k in range (7)]
	prof += [(RT * math.cos (math.pi / 2 * k / 6), T1 - hd + hd * math.sin (math.pi / 2 * k / 6)) for k in range (7)]
	A.add (lathe (prof, cx, cz), PAINT_WHITE, uv='cyl', axis=(0, 1, 0), centre=(cx, 0, cz))
	A.add (cylinder (0.9, 0.8, 16, (cx, T1 - 0.1, cz)), STEEL_GREY, role='detail')             # (the roof hatch, its vent)
	rail = [rod_between ((cx + 1.8 * math.sin (2 * math.pi * k / 12), T1 + 0.9, cz + 1.8 * math.cos (2 * math.pi * k / 12)),
		(cx + 1.8 * math.sin (2 * math.pi * (k + 1) / 12), T1 + 0.9, cz + 1.8 * math.cos (2 * math.pi * (k + 1) / 12)), 0.04, 4) for k in range (12)]
	A.add (merge (*rail), STEEL_GREY, role='detail')
	# (the legs: footings, the legs raked in to the tank's bottom; girts, X braces, struts to the pipe)
	ang = [2 * math.pi * (k + 0.5) / NL for k in range (NL)]
	at = lambda k, y: (cx + (R0 + (R1 - R0) * y / T0) * math.sin (ang[k % NL]), y, cz + (R0 + (R1 - R0) * y / T0) * math.cos (ang[k % NL]))
	legs, br, gi = [], [], []
	for k in range (NL):
		x, _, z = at (k, 0.0)
		A.add (box (2.2, 1.0, 2.2, (x, 0, z)), CONCRETE)
		legs.append (rod_between (at (k, 1.0), at (k, T0 + 1.0), 0.42, 8))
	levels = [1.0 + (T0 - 1.0) * i / 6 for i in range (7)]
	for i, y in enumerate (levels):
		if i == 0: continue
		for k in range (NL):
			gi.append (rod_between (at (k, y), at (k + 1, y), 0.2, 6))
			p = at (k, y)
			gi.append (rod_between (p, (cx + (p[0] - cx) * 2.0 / math.hypot (p[0] - cx, p[2] - cz), y, cz + (p[2] - cz) * 2.0 / math.hypot (p[0] - cx, p[2] - cz)), 0.15, 6))
		A.add (cylinder (2.25, 0.5, 24, (cx, y - 0.25, cz)), STEEL_GREY, role='detail')            # (the pipe's collars)
	for ya, yb in zip (levels, levels[1:]):
		for k in range (NL):
			br.append (rod_between (at (k, ya), at (k + 1, yb), 0.09, 4))
			br.append (rod_between (at (k + 1, ya), at (k, yb), 0.09, 4))
	A.add (merge (*legs), PAINT_WHITE)
	A.add (merge (*gi), PAINT_WHITE, role='detail')
	A.add (merge (*br), PAINT_WHITE, role='detail')
	A.light ((cx, T1 + 1.6, cz), RED, 1.4)
	proxy_box (A, 2 * R0, T0, 2 * R0, (cx, 0, cz)); proxy_box (A, 2 * RT, T1 - T0 + 1, 2 * RT, (cx, T0, cz))
	return (cx, 1.0, cz)

def pad_sphere (A, cx, cz, r=10.5):
	"""a cryogenic propellant sphere (liquid oxygen, liquid hydrogen) on its legs, a vent"""
	A.add (sphere (r, (cx, r + 4.0, cz), 32, 16), PAINT_WHITE, uv='cyl', axis=(0, 1, 0), centre=(cx, 0, cz))
	legs = [rod_between ((cx + (r - 0.8) * math.sin (2 * math.pi * k / 10), 0, cz + (r - 0.8) * math.cos (2 * math.pi * k / 10)),
		(cx + (r - 1.2) * math.sin (2 * math.pi * k / 10), r + 3.0, cz + (r - 1.2) * math.cos (2 * math.pi * k / 10)), 0.4, 8) for k in range (10)]
	A.add (merge (*legs), STEEL_GREY)
	A.add (cylinder (r, 0.5, 32, (cx, 0, cz)), CONCRETE)
	A.add (cylinder (0.35, 3.0, 8, (cx, 2 * r + 3.5, cz)), STEEL_GREY, role='detail')
	A.light ((cx, 2 * r + 6.8, cz), RED, 1.0)
	proxy_box (A, 2 * r * 0.95, 2 * r + 4, 2 * r * 0.95, (cx, 0, cz))

def pad_services (A, west):
	"""what both pads have about the mound: the water tower and its pipes to the
	trench, the liquid oxygen sphere (west) and liquid hydrogen sphere (east), their lines"""
	wt = pad_watertower (A, -40.0, west * 215.0)
	zw = west * 215.0
	# (the deluge mains: from the tower along the ground, up the mound's side, to the trench's edge)
	edge = PAD_Z[0] if west < 0 else PAD_Z[1]
	for dz in (-1.6, 1.6):
		pipe (A, [(-40.0 + dz, 1.0, zw - west * 3.5), (-40.0 + dz, 1.0, edge + west * (SLOPE * PAD_H + 4)), (-40.0 + dz, PAD_H + 1.0, edge + west * 2.0),
			(-40.0 + dz, PAD_H + 1.0, TR_Z[0] - 2.0 if west < 0 else TR_Z[1] + 2.0)], 0.9, STEEL_GREY, role='drawn')
	pad_sphere (A, -95.0, -150.0)   # (LOX)
	pad_sphere (A, -95.0, 160.0, 11.0)   # (LH2)
	for (cz, end) in ((-150.0, PAD_Z[0]), (160.0, PAD_Z[1])):
		sg = 1 if cz > 0 else -1
		pipe (A, [(-95.0, 1.5, cz - sg * 12), (-95.0, 1.5, end + sg * (SLOPE * PAD_H + 3)), (-60.0, PAD_H + 1.2, end + sg * 1.5), (-20.0, PAD_H + 1.2, end + sg * 1.5)], 0.6, PAINT_WHITE)

def a_mlp ():
	"""the shuttle's mobile launcher platform, parked on a pad's mount pedestals: its
	deck (the boosters' and the main engines' exhaust holes), the boosters' hold-down
	posts, the tail service masts, rainbirds"""
	A = Asset ('MLP')
	(x0, x1), (z0, z1), (y0, y1) = MLP_X, MLP_Z, MLP_Y
	xs = sorted (set ([x0, x1] + [h[0] for h in MLP_HOLES] + [h[1] for h in MLP_HOLES]))
	zs = sorted (set ([z0, z1] + [h[2] for h in MLP_HOLES] + [h[3] for h in MLP_HOLES]))
	hole = lambda x, z: any (h[0] < x < h[1] and h[2] < z < h[3] for h in MLP_HOLES)
	deck, under, hw = kl._bm (), kl._bm (), {'x': kl._bm (), 'z': kl._bm ()}
	for (xa, xb) in zip (xs, xs[1:]):
		for (za, zb) in zip (zs, zs[1:]):
			if hole ((xa + xb) / 2, (za + zb) / 2): continue
			quad (deck, [(xa, y1, za), (xb, y1, za), (xb, y1, zb), (xa, y1, zb)], up=(0, 1, 0))
			quad (under, [(xa, y0, za), (xb, y0, za), (xb, y0, zb), (xa, y0, zb)], up=(0, -1, 0))
			proxy_box (A, xb - xa, y1 - y0, zb - za, ((xa + xb) / 2, y0, (za + zb) / 2))
	# (its sides, each its own painting on the pad's sheet: a along it, b down from the deck)
	for (a, b, c, d, n, rg, ua) in ((x0, x1, z0, z0, (0, 0, -1), 'mlp_w', (1, 0, 0)), (x0, x1, z1, z1, (0, 0, 1), 'mlp_e', (1, 0, 0)),
			(x0, x0, z0, z1, (-1, 0, 0), 'mlp_n', (0, 0, 1)), (x1, x1, z0, z1, (1, 0, 0), 'mlp_s', (0, 0, 1))):
		wl = kl._bm ()
		quad (wl, [(a, y0, c), (b, y0, d), (b, y1, d), (a, y1, c)], up=n)
		sheet_part (A, wl, 'cape_pad', rg, ua, (0, 1, 0), PAD_SHEET)
	for (a, b, c, d) in MLP_HOLES:  # (the holes' walls, facing in: a side's plates, sooted)
		quad (hw['x'], [(a, y0, c), (b, y0, c), (b, y1, c), (a, y1, c)], up=(0, 0, 1))
		quad (hw['x'], [(a, y0, d), (b, y0, d), (b, y1, d), (a, y1, d)], up=(0, 0, -1))
		quad (hw['z'], [(a, y0, c), (a, y0, d), (a, y1, d), (a, y1, c)], up=(1, 0, 0))
		quad (hw['z'], [(b, y0, c), (b, y0, d), (b, y1, d), (b, y1, c)], up=(-1, 0, 0))
	sheet_part (A, hw['x'], 'cape_pad', 'mlp_w', (1, 0, 0), (0, 1, 0), PAD_SHEET_DARK)
	sheet_part (A, hw['z'], 'cape_pad', 'mlp_n', (0, 0, 1), (0, 1, 0), PAD_SHEET_DARK)
	sheet_part (A, deck, 'cape_pad', 'mlp_deck', (0, 0, 1), (1, 0, 0), PAD_SHEET)
	sheet_part (A, under, 'cape_pad', 'mlp_deck', (0, 0, 1), (1, 0, 0), PAD_SHEET_DARK)
	# (the pad's mount pedestals it stands on)
	for x in (-29.3, -11.0, 17.5):
		for z in (-15.9, 23.7):
			A.add (box (3.2, y0 - PAD_H, 3.2, (x, PAD_H, z)), CONCRETE)
			proxy_box (A, 3.2, y0 - PAD_H, 3.2, (x, PAD_H, z))
	# (hold-down posts: four a booster, at the holes' sides, the skirts on them)
	for (a, b, c, d) in MLP_HOLES[:2]:
		xc, zc = (a + b) / 2, (c + d) / 2
		for sx in (-1, 1):
			for sz in (-1, 1):
				A.add (box (0.9, SKIRT_Y - 26.6, 0.75, (xc + sx * 1.7, 26.6, zc + sz * (d - c - 0.75) / 2)), STEEL_RED, role='detail')
	# (hazard edging about the holes)
	hz = kl._bm ()
	for (a, b, c, d) in MLP_HOLES:
		for (p, q, r_, s_) in ((a - 0.6, b + 0.6, c - 0.6, c), (a - 0.6, b + 0.6, d, d + 0.6), (a - 0.6, a, c, d), (b, b + 0.6, c, d)):
			quad (hz, [(p, y1 + 0.02, r_), (q, y1 + 0.02, r_), (q, y1 + 0.02, s_), (p, y1 + 0.02, s_)], up=(0, 1, 0))
	A.add (hz, HAZARD, role='detail')
	# (the tail service masts either side of the main engines' hole)
	for zc in (-2.6, 11.0):
		A.add (box (4.6, 9.4, 2.7, (12.3, y1, zc)), PAINT_WHITE)
		A.add (box (4.8, 0.3, 2.9, (12.3, y1 + 9.4, zc)), STEEL_GREY)
		proxy_box (A, 4.6, 9.4, 2.7, (12.3, y1, zc))
	for (x, z) in ((-12.0, -10.0), (-12.0, 18.0), (0.0, -11.5), (0.0, 19.5), (11.0, -11.5), (11.0, 19.5)):  # (rainbirds)
		pipe (A, [(x, y1, z), (x, y1 + 3.2, z), (x + 0.0, y1 + 3.7, z + (1.2 if z < 4 else -1.2))], 0.35, STEEL_GREY)
	# (railings about the deck)
	rails = []
	for (a, b) in (((x0, z0), (x1, z0)), ((x1, z0), (x1, z1)), ((x1, z1), (x0, z1)), ((x0, z1), (x0, z0))):
		rails.append (rod_between ((a[0], y1 + 1.1, a[1]), (b[0], y1 + 1.1, b[1]), 0.04, 4))
		L = math.hypot (b[0] - a[0], b[1] - a[1]); n = int (L / 2.5)
		for k in range (n + 1):
			p = (a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n)
			rails.append (rod_between ((p[0], y1, p[1]), (p[0], y1 + 1.1, p[1]), 0.03, 4))
	A.add (merge (*rails), STEEL_GREY, role='detail')
	for (x, z) in ((x0, z0), (x0, z1), (x1, z0), (x1, z1)):
		A.light ((x, y1 + 1.4, z), WHITE, 1.0)
	return A

def a_klc39b ():
	"""pad 39B as NASA rebuilt it for SLS (from 2011): a clean hardstand on its mound,
	the trench with the new main flame deflector (KLC39Bground), no service
	structures; three lightning towers of 180 m carrying a catenary above the
	pad; the water tower, the propellant spheres"""
	A = Asset ('KLC39B')
	pad_mound (A)
	tops = [lightning_tower (A, -150.0, -125.0), lightning_tower (A, -140.0, 140.0), lightning_tower (A, 150.0, -95.0)]
	for i in range (3):
		a, b = tops[i], tops[(i + 1) % 3]
		wire (A, a, b, 28.0)
	for t in tops:  # (down-leads, out and away from the pad)
		d = math.hypot (t[0], t[2])
		wire (A, t, (t[0] * (d + 120) / d, 0.5, t[2] * (d + 120) / d), 6.0)
	pad_services (A, west=-1)
	return A

def a_klc39a ():
	"""pad 39A as it is now (from 2014): the shuttle's fixed service structure kept,
	a crew access arm on it, a lightning mast and catenary on top; the
	transporter-erector lowered on its rails up the ramp; the water tower, the
	propellant spheres"""
	A = Asset ('KLC39A')
	pad_mound (A, lanes_from=PAD_X[1] - 10.0)
	fx, fz, fw = 2.0, -26.0, 12.0
	y0, y1 = PAD_H, PAD_H + 81.0
	lattice (A, fx - fw / 2, fx + fw / 2, fz - fw / 2, fz + fw / 2, y0, y1, 6.0, STEEL_GREY, w=0.8, role='drawn')
	A.add (box (4.0, y1 - y0 + 4.0, 4.0, (fx - fw / 2 - 2.2, y0, fz)), PAINT_WHITE)  # (the lift's shaft)
	for lv in range (1, 14):  # (floors)
		A.add (box (fw, 0.3, fw, (fx, y0 + 6.0 * lv, fz)), GRATING, role='detail')
	for lv in (2, 7, 11):  # (enclosed rooms)
		A.add (box (fw - 1.0, 4.5, fw - 1.0, (fx, y0 + 6.0 * lv + 0.3, fz)), PAINT_WHITE)
	A.add (box (fw + 1.0, 1.0, fw + 1.0, (fx, y1, fz)), STEEL_GREY)
	# (the crew access arm, swung to the vehicle over the trench, its white room)
	ya = y0 + 59.0
	A.add (box (3.0, 3.2, 22.0, (fx, ya, fz + fw / 2 + 11.0)), PAINT_WHITE)
	A.add (box (4.2, 4.2, 3.5, (fx, ya - 0.5, fz + fw / 2 + 22.0)), PAINT_WHITE)
	A.add (box (3.2, 0.8, 18.0, (fx, ya + 0.6, fz + fw / 2 + 11.0)), KitMat ('arm_windows', 'kit_glass', (1, 1, 1), power=60), role='detail')
	proxy_box (A, fw, y1 - y0, fw, (fx, y0, fz)); proxy_box (A, 3.0, 3.2, 22.0, (fx, ya, fz + fw / 2 + 11.0))
	# (the lightning mast and its catenary, north and south to the ground)
	A.add (cylinder (1.0, 24.0, 12, (fx, y1 + 1.0, fz), r2=0.35), FIBRE)
	top = (fx, y1 + 25.0, fz)
	A.light ((top[0], top[1] + 0.4, top[2]), RED, 1.6)
	A.light ((fx + fw / 2, y1 + 1.5, fz + fw / 2), RED, 1.2)
	for gx in (-320.0, 330.0):
		wire (A, top, (gx, 0.5, fz), 18.0)
		A.add (box (2.0, 1.0, 2.0, (gx, 0, fz)), CONCRETE)
	# (the transporter-erector, lowered, on its rails up the ramp)
	tz = 3.9
	for dz in (-6.0, 6.0):
		rails = kl._bm ()
		xs = [PAD_X[1] - 70.0 + 10.0 * k for k in range (int ((RAMP_X1 + 40 - (PAD_X[1] - 70.0)) / 10.0) + 1)]
		for (xa, xb) in zip (xs, xs[1:]):
			ya_, yb_ = ramp_y (xa) + 0.25, ramp_y (xb) + 0.25
			quad (rails, [(xa, ya_, tz + dz - 0.4), (xb, yb_, tz + dz - 0.4), (xb, yb_, tz + dz + 0.4), (xa, ya_, tz + dz + 0.4)], up=(0, 1, 0))
		A.add (rails, STEEL_GREY, role='detail')
	te0, te1 = 26.0, 92.0
	for dz in (-2.6, 2.6):
		A.add (truss ((te0, PAD_H + 3.6, tz + dz), (te1, PAD_H + 3.6, tz + dz), 1.6, 3.6, 3.0, 0.22), PAINT_WHITE, role='detail')
	for k in range (12):
		x = te0 + (te1 - te0) * k / 11
		A.add (rod_between ((x, PAD_H + 5.3, tz - 2.6), (x, PAD_H + 5.3, tz + 2.6), 0.2, 4), PAINT_WHITE, role='detail')
	for x in (te0 + 4.0, te1 - 6.0):
		A.add (box (9.0, 1.8, 3.0, (x, PAD_H, tz - 6.0)), STEEL_GREY); A.add (box (9.0, 1.8, 3.0, (x, PAD_H, tz + 6.0)), STEEL_GREY)
	A.add (box (12.0, 7.0, 12.0, (te0 - 8.0, PAD_H, tz)), STEEL_GREY)  # (its launch mount's base, beside the trench)
	proxy_box (A, te1 - te0, 7.4, 6.8, ((te0 + te1) / 2, PAD_H, tz))
	pad_services (A, west=-1)
	return A

# (the Vehicle Assembly Building and the Launch Control Center, at their real
# size where Meshes/Vab.msh had them (Canaveral.cfg: POS 0 0 0, ROT 10))
VAB_HB = (-240.6, -106.6, 20.4, 178.4, 160.0)    # (the high bay: x0, x1, z0, z1, height)
VAB_LB = (-106.6, -22.6, 31.9, 166.9, 64.0)      # (the low bay, south of it)
VAB_DOORS = (-204.1, -142.6)                     # (the high bays' doors, east and west: their centres along x)
LB_BLOCKS = capesurf.LB_BLOCKS                   # (the low bay's blocks west to east: z0, z1, height)
VAB_CLAD = KitMat ('vab_clad', 'kit_cladding', (0.88, 0.89, 0.90))
VAB_DARK = KitMat ('vab_dark', 'kit_cladding', (0.46, 0.47, 0.49))
VAB_DOOR = KitMat ('vab_door', 'kit_cladding', (0.70, 0.71, 0.73))
DECALS = KitMat ('cape_decals', 'cape_decals', (1, 1, 1), tile=64.0)
LCC_O, LCC_U, LCC_LWH = (-72.1, 201.0), (0.7432, 0.6691), (109.1, 40.9, 23.5)
# (the building's faces - the high bay's four, the low bay's three, the Launch Control Center's - painted as wholes: capesurf.py cape_vab)
VAB_SHEET = KitMat ('cape_vab', 'cape_vab', (1, 1, 1), tile=1.0)
assert (VAB_HB, VAB_LB, VAB_DOORS, LCC_O, LCC_U, LCC_LWH) == (capesurf.VAB_HB, capesurf.VAB_LB, capesurf.VAB_DOORS, capesurf.LCC_O,
	capesurf.LCC_U, capesurf.LCC_LWH), 'capesurf.py paints another building'

def vab_part (A, bm, name, axis):
	"""a face's part on the building's sheet: axis 'x': the face spans z; 'z': it spans x"""
	return sheet_part (A, bm, 'cape_vab', name, (0, 0, 1) if axis == 'x' else (1, 0, 0), (0, 1, 0), VAB_SHEET)

def wall (bm, axis, c, a0, a1, y0, y1, holes, n):
	"""a wall in the plane axis = c (axis 'x': it spans z = a; 'z': it spans x = a), y0..y1,
	less the holes (a0, a1, y0, y1); n: its outward normal"""
	As = sorted (set ([a0, a1] + [h[0] for h in holes] + [h[1] for h in holes]))
	Ys = sorted (set ([y0, y1] + [h[2] for h in holes] + [h[3] for h in holes]))
	P = (lambda a, y: (c, y, a)) if axis == 'x' else (lambda a, y: (a, y, c))
	for (aa, ab) in zip (As, As[1:]):
		for (ya, yb) in zip (Ys, Ys[1:]):
			am, ym = (aa + ab) / 2, (ya + yb) / 2
			if not (a0 <= am <= a1 and y0 <= ym <= y1): continue
			if any (h[0] < am < h[1] and h[2] < ym < h[3] for h in holes): continue
			quad (bm, [P (aa, ya), P (ab, ya), P (ab, yb), P (aa, yb)], up=n)

def door_recess (A, axis, c, n, holes, depth, joints_y=(), joints_a=(), face=None):
	"""door leaves set back in the holes, their reveals (dark), the leaves' joints; face: the
	building's sheet's region the wall is on (the leaves are painted in its holes)"""
	back, rev, jn = kl._bm (), kl._bm (), []
	s = n[0] if axis == 'x' else n[2]
	cb = c - s * depth
	P = (lambda a, y, cc: (cc, y, a)) if axis == 'x' else (lambda a, y, cc: (a, y, cc))
	na = (0, 0, 1) if axis == 'x' else (1, 0, 0)
	for (ha, hb, hy0, hy1) in holes:
		quad (back, [P (ha, hy0, cb), P (hb, hy0, cb), P (hb, hy1, cb), P (ha, hy1, cb)], up=n)
		quad (rev, [P (ha, hy0, c), P (ha, hy1, c), P (ha, hy1, cb), P (ha, hy0, cb)], up=na)
		quad (rev, [P (hb, hy0, c), P (hb, hy1, c), P (hb, hy1, cb), P (hb, hy0, cb)], up=tuple (-q for q in na))
		quad (rev, [P (ha, hy1, c), P (hb, hy1, c), P (hb, hy1, cb), P (ha, hy1, cb)], up=(0, -1, 0))
		cj = cb + s * 0.06
		for y in joints_y:
			if hy0 < y < hy1: jn.append ((P (ha, y, cj), P (hb, y, cj)))
		for a in joints_a:
			if ha < a < hb: jn.append ((P (a, hy0, cj), P (a, hy1, cj)))
	if face: vab_part (A, back, face, axis)
	else: A.add (back, VAB_DOOR)
	A.add (rev, VAB_DARK)
	if jn: A.add (merge (*[rod_between (p, q, 0.25, 4) for p, q in jn]), VAB_DARK, role='detail')

def decal (A, o, eu, ev, rect, n):
	"""a decal on a wall: the rectangle o + s eu + t ev (s, t in 0..1), the sheet's rect onto it"""
	bm = kl._bm ()
	O, U, V = Vector (o), Vector (eu), Vector (ev)
	quad (bm, [tuple (O), tuple (O + U), tuple (O + U + V), tuple (O + V)], up=n)
	A.add (bm, DECALS, uv='decal', axis=(o, eu, ev, rect))

def a_vab ():
	"""the Vehicle Assembly Building: its high bay (160 m) with the four high bays'
	doors (east and west: 139 m, an inverted T, leaves set back), the transfer
	aisle's door north; the low bay (64 m) south; the flag and the NASA insignia
	on the south face (capetex.py); the utility annexes; the Launch Control
	Center by its south-east corner, its firing rooms' windows leaning out
	towards the pads, a bridge to the low bay"""
	A = Asset ('vab')
	x0, x1, z0, z1, H = VAB_HB
	holes = []
	for xc in VAB_DOORS:
		holes += [(xc - 23.0, xc + 23.0, 0.0, 35.0), (xc - 11.5, xc + 11.5, 35.0, 139.0)]
	jy = [35.0 + 104.0 * k / 7 for k in range (1, 7)]
	ja = [xc + d for xc in VAB_DOORS for d in (-11.5, 0.0, 11.5)]
	for (zc, sg) in ((z0, -1), (z1, 1)):  # (west, east: the one painting, the bays' paint and the doors' guides in it)
		wl = kl._bm ()
		wall (wl, 'z', zc, x0, x1, 0.0, H, holes, (0, 0, sg))
		vab_part (A, wl, 'e', 'z')
		door_recess (A, 'z', zc, (0, 0, sg), holes, 1.5, jy, ja, face='e')
	nh = [(99.4 - 12.5, 99.4 + 12.5, 0.0, 62.0)]  # (the transfer aisle's door, north)
	wl = kl._bm ()
	wall (wl, 'x', x0, z0, z1, 0.0, H, nh, (-1, 0, 0))
	vab_part (A, wl, 'n', 'x')
	door_recess (A, 'x', x0, (-1, 0, 0), nh, 1.2, [62.0 * k / 5 for k in range (1, 5)], face='n')
	wl = kl._bm ()
	for (za, zb, h) in ((z0, VAB_LB[2], 0.0),) + LB_BLOCKS + ((VAB_LB[3], z1, 0.0),):  # (above the low bay's blocks)
		quad (wl, [(x1, h, za), (x1, H, za), (x1, H, zb), (x1, h, zb)], up=(1, 0, 0))
	vab_part (A, wl, 's', 'x')
	roof = kl._bm ()   # (the high bay's and the low bay's: one painting in plan (capesurf.py roof))
	quad (roof, [(x0, H, z0), (x1, H, z0), (x1, H, z1), (x0, H, z1)], up=(0, 1, 0))
	for (px, pz, sx, sz) in ((x0 + 0.3, (z0 + z1) / 2, 0.6, z1 - z0), (x1 - 0.3, (z0 + z1) / 2, 0.6, z1 - z0), ((x0 + x1) / 2, z0 + 0.3, x1 - x0, 0.6), ((x0 + x1) / 2, z1 - 0.3, x1 - x0, 0.6)):
		A.add (box (sx, 1.2, sz, (px, H, pz)), VAB_CLAD, role='detail')
	for (bx, bz, sx, sz), h in zip (capesurf.VAB_PLANT, (5.0, 4.0, 6.0, 4.5)):  # (the plant: the roof's painting has its grime about it)
		A.add (box (sx, h, sz, (bx, H, bz)), VAB_CLAD)
	pipe (A, [(-166.0, H, 106.0), (-166.0, H + 12.0, 106.0)], 0.15, STEEL_GREY)  # (the flagpole)
	for (x, z) in ((x0 + 1, z0 + 1), (x0 + 1, z1 - 1), (x1 - 1, z0 + 1), (x1 - 1, z1 - 1)):
		A.light ((x, H + 1.8, z), RED, 1.6)
	# (the low bay: stepped - its middle block over the transfer aisle, 64 m, the door in it; wings either
	# side stepping down (LB_BLOCKS); their south faces one painting, their sides another, their roofs the plan's)
	lx0, lx1, lz0, lz1, LH = VAB_LB
	lb, ls = kl._bm (), kl._bm ()
	sd = [((lz0 + lz1) / 2 - 15.0, (lz0 + lz1) / 2 + 15.0, 0.0, 24.0)]
	for (za, zb, h) in LB_BLOCKS:
		wall (ls, 'x', lx1, za, zb, 0.0, h, sd if h == LH else [], (1, 0, 0))
		quad (roof, [(lx0, h, za), (lx1, h, za), (lx1, h, zb), (lx0, h, zb)], up=(0, 1, 0))
	vab_part (A, ls, 'ls', 'x')
	door_recess (A, 'x', lx1, (1, 0, 0), sd, 1.0, [8.0, 16.0], face='ls')
	hs = [0.0] + [b[2] for b in LB_BLOCKS] + [0.0]
	for k, (za, zb, h) in enumerate (LB_BLOCKS):  # (the sides: where a block stands over its neighbour)
		if h > hs[k]: wall (lb, 'z', za, lx0, lx1, hs[k], h, [], (0, 0, -1))
		if h > hs[k + 2]: wall (lb, 'z', zb, lx0, lx1, hs[k + 2], h, [], (0, 0, 1))
	vab_part (A, lb, 'le', 'z')
	sheet_part (A, roof, 'cape_vab', 'roof', (1, 0, 0), (0, 0, 1), VAB_SHEET)
	for (gz, h, sg) in ((lz0, LB_BLOCKS[0][2], -1), (lz1, LB_BLOCKS[-1][2], 1)):  # (window bands: the wings' offices)
		for y in (6.0, 14.0, 22.0, 30.0):
			if y + 1.8 > h - 2.0: continue
			bm = kl._bm ()
			quad (bm, [(lx0 + 6, y, gz + sg * 0.06), (lx1 - 6, y, gz + sg * 0.06), (lx1 - 6, y + 1.8, gz + sg * 0.06), (lx0 + 6, y + 1.8, gz + sg * 0.06)], up=(0, 0, sg))
			A.add (bm, GLASS)
	c = ((lx0 + lx1) / 2, LB_BLOCKS[-1][2], (LB_BLOCKS[-1][0] + LB_BLOCKS[-1][1]) / 2 - 3.0)   # (the east wing's penthouse)
	A.add (box (24.0, 4.5, 14.0, c), VAB_CLAD)
	# (the flag and the insignia on the high bay's south face, above the low bay: west, east)
	import capetex
	fu0, fv0, fu1, fv1 = capetex.DECALS['flag']; nu0, nv0, nu1, nv1 = capetex.DECALS['nasa']
	decal (A, (x1 + 0.08, 136.0, 26.0), (0, 0, 33.5), (0, -63.7, 0), (fu0, fv0, fu1, fv1), (1, 0, 0))
	# (the insignia, 40.2 m across and 33.5 m up, its centre where it was: its decal cut to the
	# sphere and the vector's ends out past it (capetex.insignia_patches), the wall between its own)
	iw, ih = capetex.INSIGNIA_SIZE
	o, eu, ev = (x1 + 0.08, 107.9 + ih / 2, z1 - 24.75 - iw / 2), (0, 0, iw), (0, -ih, 0)
	bm = kl._bm ()
	for poly in capetex.insignia_patches ():
		quad (bm, [(o[0], o[1] - t * ih, o[2] + s * iw) for (s, t) in poly], up=(1, 0, 0))
	A.add (bm, DECALS, uv='decal', axis=(o, eu, ev, (nu0, nv0, nu1, nv1)))
	proxy_box (A, x1 - x0, H, z1 - z0, ((x0 + x1) / 2, 0, (z0 + z1) / 2))
	for (za, zb, h) in LB_BLOCKS:
		proxy_box (A, lx1 - lx0, h, zb - za, ((lx0 + lx1) / 2, 0, (za + zb) / 2))
	# (the utility annex west, the cooling plant south-west with its fans)
	shed (A, -91.7, -39.8, 30.0, 77.0, 9.0, wall=VAB_CLAD)
	shed (A, -21.6, -40.0, 17.0, 50.0, 9.0, wall=VAB_CLAD)
	for z in (-22.0, -33.5, -45.7, -57.4):
		A.add (cylinder (4.6, 3.0, 24, (-21.0, 9.0, z)), VAB_CLAD)
		A.add (cylinder (4.0, 0.1, 24, (-21.0, 12.0, z)), GRATING, role='detail')
	proxy_box (A, 30, 9, 77, (-91.7, 0, -39.8)); proxy_box (A, 17, 12, 50, (-21.6, 0, -40.0))
	lcc (A)
	return A

def lcc (A):
	"""the Launch Control Center: four storeys, window bands; the firing rooms' windows
	(the third and fourth storeys of its north-east face, towards the pads) leaning out"""
	(ox, oz), (ux, uz), (L, W, H) = LCC_O, LCC_U, LCC_LWH
	vx, vz = -uz, ux
	P = lambda a, b, y: (ox + a * ux + b * vx, y, oz + a * uz + b * vz)
	lean = capesurf.LCC_LEAN
	sw, ne, en, gl, rf = kl._bm (), kl._bm (), kl._bm (), kl._bm (), kl._bm ()
	quad (sw, [P (0, 0, 0), P (L, 0, 0), P (L, 0, H), P (0, 0, H)], up=(-vx, 0, -vz))
	quad (ne, [P (0, W, 0), P (L, W, 0), P (L, W, 12.8), P (0, W, 12.8)], up=(vx, 0, vz))
	quad (gl, [P (0, W, 12.8), P (L, W, 12.8), P (L, W + lean, H - 1.2), P (0, W + lean, H - 1.2)], up=(vx, 0.7, vz))
	quad (ne, [P (0, W + lean, H - 1.2), P (L, W + lean, H - 1.2), P (L, W + lean, H), P (0, W + lean, H)], up=(vx, 0, vz))
	for (a, sg) in ((0.0, -1), (L, 1)):
		quad (en, [P (a, 0, 0), P (a, W, 0), P (a, W, 12.8), P (a, W + lean, H - 1.2), P (a, W + lean, H), P (a, 0, H)], up=(sg * ux, 0, sg * uz))
	quad (rf, [P (0, 0, H), P (L, 0, H), P (L, W + lean, H), P (0, W + lean, H)], up=(0, 1, 0))
	# (its faces on the building's sheet: along its length, across it)
	sheet_part (A, sw, 'cape_vab', 'lcc_sw', (ux, 0, uz), (0, 1, 0), VAB_SHEET)
	sheet_part (A, ne, 'cape_vab', 'lcc_ne', (ux, 0, uz), (0, 1, 0), VAB_SHEET)
	sheet_part (A, en, 'cape_vab', 'lcc_end', (vx, 0, vz), (0, 1, 0), VAB_SHEET)
	A.add (gl, GLASS)
	sheet_part (A, rf, 'cape_vab', 'lcc_roof', (ux, 0, uz), (vx, 0, vz), VAB_SHEET)
	bands = kl._bm ()
	for y in (2.2, 7.8, 13.4, 19.0):  # (window bands: the south-west face, the ends)
		quad (bands, [P (3, -0.06, y), P (L - 3, -0.06, y), P (L - 3, -0.06, y + 1.8), P (3, -0.06, y + 1.8)], up=(-vx, 0, -vz))
		for (a, sg) in ((-0.06, -1), (L + 0.06, 1)):
			quad (bands, [P (a, 3, y), P (a, W - 3, y), P (a, W - 3, y + 1.8), P (a, 3, y + 1.8)], up=(sg * ux, 0, sg * uz))
	for y in (2.2, 7.8):  # (and below the firing rooms on the north-east)
		quad (bands, [P (3, W + 0.06, y), P (L - 3, W + 0.06, y), P (L - 3, W + 0.06, y + 1.8), P (3, W + 0.06, y + 1.8)], up=(vx, 0, vz))
	A.add (bands, GLASS)
	rot = -math.degrees (math.atan2 (uz, ux))
	for (a, b, sx, sz), h in zip (capesurf.LCC_PLANT, (3.5, 4.0, 3.0)):
		c = P (a, b, H); A.add (box (sx, h, sz, (c[0], H, c[2]), rot), VAB_CLAD)
	c = P (70.0, 30.0, H); pipe (A, [(c[0], H, c[2]), (c[0], H + 14.0, c[2])], 0.2, STEEL_GREY); A.light ((c[0], H + 14.4, c[2]), RED, 1.0)
	c = P (L / 2, -4.0, 0); A.add (box (16.0, 0.4, 6.0, (c[0], 4.2, c[2]), rot), VAB_DARK)  # (the entrance's canopy)
	c = P (L / 2, W / 2 + lean / 2, 0); proxy_box (A, L, H, W + lean, (c[0], 0, c[2]), rot)
	# (the bridge from the low bay's east face to the LCC's end, the third storey)
	a, b = Vector ((-92.0, 10.0, VAB_LB[3])), Vector (P (0.0, W / 2, 10.0))
	d = b - a; Lb = d.length; rb = -math.degrees (math.atan2 (d.z, d.x)) + 90.0
	m = (a + b) / 2
	A.add (box (5.0, 4.5, Lb, (m.x, 10.0, m.z), rb), VAB_CLAD)
	A.add (box (5.1, 1.2, Lb - 1.0, (m.x, 11.6, m.z), rb), GLASS, role='detail')
	for t in (0.33, 0.67):
		p = a + d * t; A.add (box (2.0, 10.0, 2.0, (p.x, 0, p.z)), CONCRETE)

KITS = {
	'earth': {'tower_red': lambda: a_tower ('red'), 'tower_grey': lambda: a_tower ('grey'), 'mast': a_earth_mast,
		'watertower': a_watertower, 'spheres': a_spheres, 'tanks': a_tanks,
		'assembly': lambda: a_assembly ('assembly', 90.0, 60.0, 80.0), 'assembly_small': lambda: a_assembly ('assembly_small', 60.0, 44.0, 56.0),
		'hangar': a_earth_hangar, 'office': a_office, 'control': a_control, 'radome': a_radome},
	'trains': {'train_hangrail_girder': a_hangrail_girder, 'train_hangrail_beam': a_hangrail_beam,
		'train_hangrail_car': a_hangrail_car, 'train_monorail_beam': a_monorail_beam, 'train_monorail_car': a_monorail_car},
	'offworld': dict ([(('space_hab_' + b.lower ()), (lambda b=b: a_hab (b))) for b in BODIES]
		+ [(('space_padberm_' + b.lower ()), (lambda b=b: a_padberm (b))) for b in BODIES]
		+ [(('space_isru_' + b.lower ()), (lambda b=b: a_isru (b))) for b in BODIES]
		+ [('space_dome', a_dome), ('space_greenhouse', a_greenhouse), ('space_solar', a_solar),
		   ('space_radiator', a_radiator), ('space_reactor', a_reactor), ('space_comms', a_comms),
		   ('space_garage', a_garage), ('space_massdriver', a_massdriver), ('space_solar_tower', a_solar_tower),
		   ('space_radioarray', a_radioarray), ('space_aerostat', a_aerostat), ('space_aerostat_terminal', a_aerostat_terminal)]),
	'cape': {'KLC39B': a_klc39b, 'KLC39A': a_klc39a, 'KLC39Bground': a_pad_ground, 'MLP': a_mlp, 'vab': a_vab},
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
				os.path.join (ROOT, 'Utils', 'BlenderMsh', 'msh_check.py'), path, '--textures', os.path.join (ROOT, 'Textures')]
				+ ([] if kit in ('trains', 'cape') else ['--bounds-json', os.path.join (HERE, 'footprints.json')]),  # (trains, the Cape: new, nothing to keep)
				capture_output=True, text=True)
			print (r.stdout.rstrip ())
			if r.returncode: failed.append (name)
	json.dump (lights, open (lights_path, 'w'), indent=1, sort_keys=True)
	if failed: raise SystemExit ('footprint/mesh checks failed: ' + ', '.join (failed))

if __name__ == '__main__':
	main ()
