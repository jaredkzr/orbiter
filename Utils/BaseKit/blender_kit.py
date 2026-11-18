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
				+ ([] if kit == 'trains' else ['--bounds-json', os.path.join (HERE, 'footprints.json')]),  # (trains: new, nothing to keep)
				capture_output=True, text=True)
			print (r.stdout.rstrip ())
			if r.returncode: failed.append (name)
	json.dump (lights, open (lights_path, 'w'), indent=1, sort_keys=True)
	if failed: raise SystemExit ('footprint/mesh checks failed: ' + ', '.join (failed))

if __name__ == '__main__':
	main ()
