#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Generic launch complexes for the Earth bases without structures of
their own (a pad only): meshes of a kit, and the objects that place them
around each base's pad in its configuration file.

    basekit.py meshes <Meshes dir>
        writes the kit's meshes (Meshes/BaseKit/*.msh)
    basekit.py sampler <base cfg dir> <out.lua>
        a Lua script (run with Orbiter, headless) that logs the elevation
        about each base in the kit list: lines
        ELEVGRID <base> <row j> <81 elevations 50 m apart, west to east>
        for rows j -40 (south) to 40 (north)
    basekit.py layout <grid log> <base cfg dir>... [--meshes <Meshes dir>] [--vk <Config/VK/Bases>]
        writes each base's complex into its configuration (between the
        markers ";; BaseKit" and ";; BaseKit end", replacing an earlier
        one), its ground (roads, aprons) as a mesh of its own, and its
        floodlights for the Vulkan client

Each Earth base on land gets at least one vertical launch pad, hover pads
and a runway; what it lacks is added, on free land (clear of its own
objects), without water, as flat as can be found within 4 km. The layout
is deterministic (from the base name and the terrain):
- a launch pad complex: the pad with its service tower, lightning masts,
  water tower, propellant farm, apron and floodlights; for bases that are
  only a pad marker, about that pad (pad 1), with the base's campus
  (assembly building, processing hangars, offices, a radome, a row of
  hover pads) 0.9 to 1.5 km from it, a blockhouse between them (no room
  for it: the hover pads alone);
- a runway of 3 km (2.4 or 1.8 km where that does not fit) with its lights, a
  taxiway and a hangar;
- roads between them.

Positions are planned east and north of the base [m]; Orbiter's base frame
(configuration files, the Vulkan client's lights) is x south, y up, z east
(BF below); objects are set on the local ground by Orbiter. Meshes: clockwise front faces seen from outside (the
right-handed cross product of the first two edges along the outward
normal), +z their front.
"""

import math, os, re, sys, glob, zlib

# Every Earth base on land gets at least one vertical launch pad (a pad
# with a service tower), hover pads and a runway: what it lacks is added
# (bases with nothing but a pad marker at their origin: the whole kit, the
# pad complex about that pad, a campus). Left out: sea and air launch sites,
# and bases that have all of it.
SKIP_BASES = ["Barent_Sea", "Gran_canaria", "San_Marco", "Canaveral"]
# runways where none fits near the pad: the base's airfield (lng, lat [deg] of
# its middle, bearing [deg], length [m])
KNOWN_RUNWAYS = {"Wallops_Island": (-75.4664, 37.9402, 40.0, 2600.0)}

GRID_N, GRID_STEP = 80, 50.0  # the sampled grid: (2N+1)^2 points, STEP apart [m]

# ======================================================================
# Meshes

class Mat:
	def __init__ (self, name, diffuse, specular=(0.1, 0.1, 0.1), power=10.0, emissive=(0, 0, 0)):
		self.name, self.diffuse, self.specular, self.power, self.emissive = name, diffuse, specular, power, emissive

MATS = {
	"steel_red": Mat ("steel_red", (0.60, 0.17, 0.10), (0.3, 0.3, 0.3), 20),
	"steel_grey": Mat ("steel_grey", (0.52, 0.54, 0.56), (0.3, 0.3, 0.3), 20),
	"white": Mat ("white", (0.86, 0.86, 0.85), (0.3, 0.3, 0.3), 30),
	"panel": Mat ("panel", (0.80, 0.80, 0.78)),
	"panel_blue": Mat ("panel_blue", (0.42, 0.52, 0.64)),
	"door": Mat ("door", (0.34, 0.36, 0.38), (0.2, 0.2, 0.2), 20),
	"roof": Mat ("roof", (0.48, 0.48, 0.47)),
	"concrete": Mat ("concrete", (0.66, 0.65, 0.62)),
	"glass": Mat ("glass", (0.12, 0.15, 0.18), (0.8, 0.8, 0.8), 60),
	"asphalt": Mat ("asphalt", (0.16, 0.16, 0.16)),
	"apron": Mat ("apron", (0.62, 0.61, 0.58)),
	"yellow": Mat ("yellow", (0.80, 0.62, 0.10)),
}
TEXS = { # texture: tile size [m]
	"Concrete.dds": 20.0,
	"Wall01.dds": 12.0,
}

class Group:
	def __init__ (self, mat, tex=None, label=None):
		self.mat, self.tex, self.label = mat, tex, label
		self.vtx, self.tri = [], []

class MeshBuilder:
	"""Groups by (material, texture); faces with outward normals"""
	def __init__ (self):
		self.groups = {}

	def group (self, mat, tex=None):
		k = (mat, tex)
		if k not in self.groups: self.groups[k] = Group (mat, tex)
		return self.groups[k]

	def poly (self, pts, n, mat, tex=None, uv=None):
		"""a planar convex polygon facing out along n (normal used as is);
		uv: texture coordinates per point (default: metres in the plane / tile)"""
		g = self.group (mat, tex)
		a, b, c = pts[0], pts[1], pts[2]
		e1 = [b[i] - a[i] for i in range (3)]; e2 = [c[i] - a[i] for i in range (3)]
		cr = (e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0])
		if sum (cr[i] * n[i] for i in range (3)) < 0:
			pts = pts[::-1]
			if uv: uv = uv[::-1]
		l = math.sqrt (sum (x * x for x in n)) or 1.0
		nn = tuple (x / l for x in n)
		if uv is None:
			ts = TEXS.get (tex, 10.0)
			# (axes in the plane: horizontal and up, or x and z for horizontal faces)
			if abs (nn[1]) > 0.7: ax, ay = (1, 0, 0), (0, 0, 1)
			else:
				ax = (-nn[2], 0, nn[0]); la = math.hypot (ax[0], ax[2]) or 1.0
				ax = (ax[0] / la, 0, ax[2] / la); ay = (0, 1, 0)
			uv = [(sum (p[i] * ax[i] for i in range (3)) / ts, -sum (p[i] * ay[i] for i in range (3)) / ts) for p in pts]
		base = len (g.vtx)
		for p, t in zip (pts, uv): g.vtx.append ((p[0], p[1], p[2], nn[0], nn[1], nn[2], t[0], t[1]))
		for i in range (1, len (pts) - 1): g.tri.append ((base, base + i, base + i + 1))

	def box (self, x0, x1, y0, y1, z0, z1, mat, tex=None, top=None, bottom=False, sides=None):
		"""a box; top: material of its top (None: mat; False: none); sides: materials (+x, -x, +z, -z)"""
		s = sides or (mat, mat, mat, mat)
		self.poly ([(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)], (1, 0, 0), s[0], tex)
		self.poly ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], (-1, 0, 0), s[1], tex)
		self.poly ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1), s[2], tex)
		self.poly ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1), s[3], tex)
		if top is not False:
			self.poly ([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0), top or mat)
		if bottom:
			self.poly ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0), mat)

	def beam (self, p, q, w, mat):
		"""a square beam w [m] across from p to q"""
		d = [q[i] - p[i] for i in range (3)]
		L = math.sqrt (sum (x * x for x in d))
		if L < 1e-6: return
		d = [x / L for x in d]
		ref = (0, 1, 0) if abs (d[1]) < 0.9 else (1, 0, 0)
		u = (ref[1]*d[2] - ref[2]*d[1], ref[2]*d[0] - ref[0]*d[2], ref[0]*d[1] - ref[1]*d[0])
		lu = math.sqrt (sum (x * x for x in u)); u = [x / lu for x in u]
		v = (d[1]*u[2] - d[2]*u[1], d[2]*u[0] - d[0]*u[2], d[0]*u[1] - d[1]*u[0])
		h = 0.5 * w
		corners = [(h, h), (-h, h), (-h, -h), (h, -h)]
		ring = lambda c, o: tuple (o[i] + u[i] * c[0] + v[i] * c[1] for i in range (3))
		for k in range (4):
			a, b = corners[k], corners[(k + 1) % 4]
			n = tuple (u[i] * (a[0] + b[0]) + v[i] * (a[1] + b[1]) for i in range (3))
			self.poly ([ring (a, p), ring (b, p), ring (b, q), ring (a, q)], n, mat, uv=[(0, 0), (1, 0), (1, 1), (0, 1)])

	def cylinder (self, cx, cz, r, y0, y1, mat, n=16, top=True, r1=None, cap=None):
		"""a vertical cylinder (cone if r1 is given: radius at the top)"""
		r1 = r if r1 is None else r1
		for k in range (n):
			a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
			am = 0.5 * (a0 + a1)
			slope = (r - r1) / max (y1 - y0, 1e-6)
			nrm = (math.sin (am), slope, math.cos (am))
			pts = [(cx + r * math.sin (a0), y0, cz + r * math.cos (a0)), (cx + r * math.sin (a1), y0, cz + r * math.cos (a1)),
				(cx + r1 * math.sin (a1), y1, cz + r1 * math.cos (a1))]
			if r1 > 1e-6: pts.append ((cx + r1 * math.sin (a0), y1, cz + r1 * math.cos (a0)))
			self.poly (pts, nrm, mat, uv=[(k / n, 1), ((k + 1) / n, 1), ((k + 1) / n, 0), (k / n, 0)][:len (pts)])
		if top and r1 > 1e-6:
			self.poly ([(cx + r1 * math.sin (2 * math.pi * k / n), y1, cz + r1 * math.cos (2 * math.pi * k / n)) for k in range (n)], (0, 1, 0), cap or mat)

	def hcylinder (self, cx, cy, cz, r, L, mat, n=12):
		"""a horizontal cylinder along x, closed"""
		for k in range (n):
			a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
			am = 0.5 * (a0 + a1)
			y = lambda a: cy + r * math.cos (a); z = lambda a: cz + r * math.sin (a)
			self.poly ([(cx - L / 2, y (a0), z (a0)), (cx + L / 2, y (a0), z (a0)), (cx + L / 2, y (a1), z (a1)), (cx - L / 2, y (a1), z (a1))],
				(0, math.cos (am), math.sin (am)), mat, uv=[(0, k / n), (1, k / n), (1, (k + 1) / n), (0, (k + 1) / n)])
		for s in (-1, 1):
			self.poly ([(cx + s * L / 2, cy + r * math.cos (2 * math.pi * k / n), cz + r * math.sin (2 * math.pi * k / n)) for k in range (n)], (s, 0, 0), mat)

	def sphere (self, cx, cy, cz, r, mat, nlat=8, nlng=16, lat0=-90.0):
		"""a sphere (from latitude lat0 up: domes)"""
		for i in range (nlat):
			b0 = math.radians (lat0 + (90.0 - lat0) * i / nlat); b1 = math.radians (lat0 + (90.0 - lat0) * (i + 1) / nlat)
			for k in range (nlng):
				a0, a1 = 2 * math.pi * k / nlng, 2 * math.pi * (k + 1) / nlng
				P = lambda a, b: (cx + r * math.cos (b) * math.sin (a), cy + r * math.sin (b), cz + r * math.cos (b) * math.cos (a))
				bm, am = 0.5 * (b0 + b1), 0.5 * (a0 + a1)
				nrm = (math.cos (bm) * math.sin (am), math.sin (bm), math.cos (bm) * math.cos (am))
				pts = [P (a0, b0), P (a1, b0), P (a1, b1), P (a0, b1)]
				if i == nlat - 1 and lat0 + (90.0 - lat0) * (i + 1) / nlat >= 89.999: pts = pts[:3]
				elif i == 0 and lat0 <= -89.999: pts = [pts[0], pts[2], pts[3]]
				self.poly (pts, nrm, mat, uv=[(0, 0)] * len (pts))

	def write (self, path, label=None):
		mats = sorted ({g.mat for g in self.groups.values()})
		texs = sorted ({g.tex for g in self.groups.values() if g.tex})
		out = ["MSHX1", "GROUPS %d" % len (self.groups)]
		for (m, t), g in sorted (self.groups.items(), key=lambda kv: (kv[0][0], kv[0][1] or "")):
			if len (g.vtx) > 65535: raise ValueError ("%s: group too large" % path)
			out.append ("LABEL %s" % (m if not t else "%s_%s" % (m, t.split ('.')[0])))
			out.append ("MATERIAL %d" % (mats.index (m) + 1))
			out.append ("TEXTURE %d" % (texs.index (t) + 1 if t else 0))
			out.append ("GEOM %d %d" % (len (g.vtx), len (g.tri)))
			for v in g.vtx: out.append (" ".join (_f (x, 3 if i < 6 else 4) for i, x in enumerate (v)))
			for t3 in g.tri: out.append ("%d %d %d" % t3)
		out.append ("MATERIALS %d" % len (mats))
		out += mats
		for m in mats:
			M = MATS[m]
			out.append ("MATERIAL %s" % m)
			out.append ("%s 1" % " ".join (_f (x, 3) for x in M.diffuse))
			out.append ("%s 1" % " ".join (_f (x, 3) for x in M.diffuse))
			out.append ("%s 1 %s" % (" ".join (_f (x, 3) for x in M.specular), _f (M.power, 1)))
			out.append ("%s 1" % " ".join (_f (x, 3) for x in M.emissive))
		out.append ("TEXTURES %d" % len (texs))
		out += texs
		os.makedirs (os.path.dirname (path), exist_ok=True)
		with open (path, "w", newline="\n") as f: f.write ("\n".join (out) + "\n")

def _f (x, d):
	s = ("%.*f" % (d, x)).rstrip ("0").rstrip (".")
	return "0" if s in ("-0", "", "-") else s

# ----------------------------------------------------------------------
# The kit (each mesh: origin on the ground at its centre, +z its front)

def lattice (B, x0, x1, z0, z1, y0, y1, step, mat, w=0.6, faces=(True, True, True, True)):
	"""a square lattice tower: corner columns, girts every step [m], X braces"""
	cs = [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
	for (x, z) in cs: B.beam ((x, y0, z), (x, y1, z), w * 1.4, mat)
	n = max (1, int (round ((y1 - y0) / step)))
	for i in range (n + 1):
		y = y0 + (y1 - y0) * i / n
		for k in range (4):
			a, b = cs[k], cs[(k + 1) % 4]
			B.beam ((a[0], y, a[1]), (b[0], y, b[1]), w, mat)
	for i in range (n):
		ya, yb = y0 + (y1 - y0) * i / n, y0 + (y1 - y0) * (i + 1) / n
		for k in range (4):
			if not faces[k]: continue
			a, b = cs[k], cs[(k + 1) % 4]
			B.beam ((a[0], ya, a[1]), (b[0], yb, b[1]), w * 0.7, mat)
			B.beam ((b[0], ya, b[1]), (a[0], yb, a[1]), w * 0.7, mat)

def kit_tower (colour):
	"""service tower (umbilical tower) beside a pad: lattice, platforms,
	two swing arms towards +z (the pad), a crane on top, a lift shaft"""
	B = MeshBuilder ()
	m = "steel_" + colour
	lattice (B, -5, 5, -5, 5, 0, 77, 7, m)
	for y in range (14, 78, 14):
		B.box (-5.5, 5.5, y - 0.4, y, -5.5, 5.5, "steel_grey", bottom=True)
	B.box (-1.5, 1.5, 0, 80, -9, -5, "panel", top="roof") # (lift shaft at the back)
	for y in (42, 63):
		B.box (-1.2, 1.2, y, y + 2.4, 5, 19, m, bottom=True)        # arm
		B.box (-2.0, 2.0, y - 0.5, y + 3.0, 17.5, 20, "white", bottom=True) # its hood
	B.box (-0.8, 0.8, 77, 90, -0.8, 0.8, m)                     # crane mast
	B.box (-0.7, 0.7, 88, 89.6, -8, 18, m, bottom=True)          # jib
	B.cylinder (0, 0, 0.2, 90, 96, "white", n=6)                # (lightning rod)
	return B

def kit_mast ():
	"""lightning mast: a tapering pole with a fibreglass top"""
	B = MeshBuilder ()
	B.cylinder (0, 0, 1.2, 0, 100, "steel_grey", n=10, r1=0.5)
	B.cylinder (0, 0, 0.35, 100, 115, "white", n=8, r1=0.1)
	B.box (-1.8, 1.8, 0, 1.0, -1.8, 1.8, "concrete")
	return B

def kit_watertower ():
	"""deluge water tank on legs"""
	B = MeshBuilder ()
	for (x, z) in ((-5, -5), (5, -5), (5, 5), (-5, 5)):
		B.beam ((x * 1.2, 0, z * 1.2), (x * 0.8, 45, z * 0.8), 1.0, "steel_grey")
	lattice (B, -5.5, 5.5, -5.5, 5.5, 0, 45, 15, "steel_grey", w=0.4)
	B.cylinder (0, 0, 9, 45, 58, "white", n=20, top=False)
	B.cylinder (0, 0, 9, 58, 62, "white", n=20, r1=1.0)
	B.cylinder (0, 0, 0.6, 0, 45, "steel_grey", n=6)
	return B

def kit_spheres ():
	"""propellant storage: two spheres on legs, a pump house"""
	B = MeshBuilder ()
	for cx in (-14, 14):
		for k in range (8):
			a = 2 * math.pi * k / 8
			B.beam ((cx + 9 * math.sin (a), 0, 9 * math.cos (a)), (cx + 8 * math.sin (a), 11, 8 * math.cos (a)), 0.7, "steel_grey")
		B.sphere (cx, 13.5, 0, 10, "white", nlat=10, nlng=20)
	B.box (-4, 4, 0, 5, 14, 22, "panel", top="roof")
	B.beam ((-14, 4, 0), (14, 4, 0), 0.8, "steel_grey")
	B.beam ((0, 4, 0), (0, 4, 14), 0.8, "steel_grey")
	return B

def kit_tanks ():
	"""fuel farm: three horizontal tanks on saddles"""
	B = MeshBuilder ()
	for k in range (3):
		z = (k - 1) * 9.0
		B.hcylinder (0, 4.5, z, 3.2, 26, "white", n=14)
		for x in (-9, 0, 9): B.box (x - 0.6, x + 0.6, 0, 2.0, z - 2.4, z + 2.4, "concrete")
	B.box (-15, 15, 0, 0.8, -15, -14, "concrete") # (bund walls)
	B.box (-15, 15, 0, 0.8, 14, 15, "concrete")
	B.box (-15, -14, 0, 0.8, -14, 14, "concrete")
	B.box (14, 15, 0, 0.8, -14, 14, "concrete")
	return B

def kit_assembly (w=90, d=60, h=80):
	"""vertical assembly building: high bay with tall doors in front, a low annex"""
	B = MeshBuilder ()
	B.box (-w / 2, w / 2, 0, h, -d / 2, d / 2, "panel", top="roof")
	# doors: two tall bays on the front (+z) and back
	for s in (1, -1):
		for x in (-w / 4, w / 4):
			B.poly ([(x - 9, 0, s * (d / 2 + 0.3)), (x + 9, 0, s * (d / 2 + 0.3)), (x + 9, h - 8, s * (d / 2 + 0.3)), (x - 9, h - 8, s * (d / 2 + 0.3))],
				(0, 0, s), "door", uv=[(0, 0)] * 4)
	# a band of blue panels under the roof
	for (n, pts) in (((1, 0, 0), [(w / 2 + 0.2, h - 10, -d / 2), (w / 2 + 0.2, h - 10, d / 2), (w / 2 + 0.2, h - 4, d / 2), (w / 2 + 0.2, h - 4, -d / 2)]),
	                 ((-1, 0, 0), [(-w / 2 - 0.2, h - 10, -d / 2), (-w / 2 - 0.2, h - 10, d / 2), (-w / 2 - 0.2, h - 4, d / 2), (-w / 2 - 0.2, h - 4, -d / 2)])):
		B.poly (pts, n, "panel_blue", uv=[(0, 0)] * 4)
	B.box (w / 2, w / 2 + 40, 0, 18, -d / 2, d / 2 - 10, "panel", tex=None, top="roof")
	B.box (w / 2 + 2, w / 2 + 38, 18, 18.6, -d / 2 + 2, d / 2 - 12, "roof", top="roof")
	return B

def kit_hangar (w=48, d=40, h=22):
	"""processing facility: a hangar with a high door, a lower office wing"""
	B = MeshBuilder ()
	B.box (-w / 2, w / 2, 0, h, -d / 2, d / 2, "panel", top="roof")
	B.poly ([(-14, 0, d / 2 + 0.3), (14, 0, d / 2 + 0.3), (14, h - 3, d / 2 + 0.3), (-14, h - 3, d / 2 + 0.3)], (0, 0, 1), "door", uv=[(0, 0)] * 4)
	B.box (-w / 2, w / 2, 0, 8, -d / 2 - 12, -d / 2, "white", tex="Wall01.dds", top="roof")
	return B

def kit_office (w=40, d=16, h=11):
	"""an office building"""
	B = MeshBuilder ()
	B.box (-w / 2, w / 2, 0, h, -d / 2, d / 2, "white", tex="Wall01.dds", top="roof")
	B.box (-4, 4, h, h + 3, -3, 3, "panel", top="roof")
	return B

def kit_control ():
	"""blockhouse: a low bunker with a mound, periscopes, an antenna mast"""
	B = MeshBuilder ()
	B.box (-20, 20, 0, 6, -12, 12, "concrete", tex="Concrete.dds")
	# earth-covered roof: a shallow dome of concrete
	for k in range (4):
		B.box (-20 + 3 * k, 20 - 3 * k, 6 + 1.2 * k, 7.2 + 1.2 * k, -12 + 2 * k, 12 - 2 * k, "concrete", tex="Concrete.dds")
	B.cylinder (0, 0, 0.25, 0, 30, "steel_grey", n=6)
	B.box (-3, 3, 0, 3, 12, 16, "concrete", tex="Concrete.dds") # (entrance)
	return B

def kit_radome ():
	"""tracking station: a radome on its building, a dish beside it"""
	B = MeshBuilder ()
	B.box (-8, 8, 0, 9, -8, 8, "white", tex="Wall01.dds", top="roof")
	B.sphere (0, 13, 0, 7.5, "white", nlat=8, nlng=20, lat0=-30.0)
	B.cylinder (16, 0, 1.2, 0, 6, "steel_grey", n=8)
	B.cylinder (16, 0, 5.0, 6.5, 8.5, "white", n=16, r1=0.3, top=False) # (dish, upturned)
	return B

KIT = {
	"tower_red": lambda: kit_tower ("red"),
	"tower_grey": lambda: kit_tower ("grey"),
	"mast": kit_mast,
	"watertower": kit_watertower,
	"spheres": kit_spheres,
	"tanks": kit_tanks,
	"assembly": lambda: kit_assembly (),
	"assembly_small": lambda: kit_assembly (60, 44, 56),
	"hangar": lambda: kit_hangar (),
	"office": lambda: kit_office (),
	"control": kit_control,
	"radome": kit_radome,
}

def cmd_meshes (meshdir):
	for name, fn in KIT.items():
		fn ().write (os.path.join (meshdir, "BaseKit", name + ".msh"))
	print ("%d meshes in %s" % (len (KIT), os.path.join (meshdir, "BaseKit")))

# ======================================================================
# Terrain sampler and layout

def read_cfg (path):
	return read_cfg_text (open (path, encoding="latin-1").read ())

def read_cfg_text (t):
	m = re.search (r"Location\s*=\s*([-+\d.]+)\s+([-+\d.]+)", t)
	return t, float (m.group (1)), float (m.group (2))

def kit_bases (cfgdir):
	return [os.path.basename (f)[:-4] for f in sorted (glob.glob (os.path.join (cfgdir, "*.cfg")))
		if os.path.basename (f)[:-4] not in SKIP_BASES]

def cmd_sampler (cfgdir, out):
	lua = ['-- (made by Utils/BaseKit/basekit.py sampler)', 'local E = oapi.get_objhandle("Earth")', 'local R = oapi.get_size(E)', 'local bases = {']
	for b in kit_bases (cfgdir):
		_, lng, lat = read_cfg (os.path.join (cfgdir, b + ".cfg"))
		lua.append ('\t{"%s", %.6f, %.6f},' % (b, lng, lat))
	lua += ['}'] + LUA_AT + ['for _, b in ipairs(bases) do', '\tlocal lng0, lat0 = math.rad(b[2]), math.rad(b[3])',
		'\tfor j = -%d, %d do' % (GRID_N, GRID_N), '\t\tlocal t = {}', '\t\tfor i = -%d, %d do' % (GRID_N, GRID_N),
		'\t\t\tlocal x, z = i * %g, j * %g' % (GRID_STEP, GRID_STEP),
		'\t\t\tt[#t+1] = string.format("%.1f", oapi.surface_elevation(E, at(lng0, lat0, R, x, z)))',
		'\t\tend', '\t\toapi.write_log("ELEVGRID " .. b[1] .. " " .. j .. " " .. table.concat(t, " "))', '\tend', 'end', 'oapi.exit(0)']
	with open (out, "w") as f: f.write ("\n".join (lua) + "\n")

# Lua: longitude and latitude of the point (x east, z north) [m] of the base's
# tangent plane (as Base::Rel_EquPos: exact also near the poles)
LUA_AT = ['local function at(lng0, lat0, R, x, z)',
	'\tlocal bx, bz = -z, x',
	'\tlocal sn, cn, st, ct = math.sin(lng0), math.cos(lng0), math.sin(lat0), math.cos(lat0)',
	'\tlocal px = R * ct * cn + bx * st * cn - bz * sn',
	'\tlocal py = R * st - bx * ct',
	'\tlocal pz = R * ct * sn + bx * st * sn + bz * cn',
	'\treturn math.atan2(pz, px), math.atan2(py, math.sqrt(px * px + pz * pz))',
	'end']

class Terrain:
	def __init__ (self, rows):
		self.g = rows # rows[j][i], j south to north, i west to east
	def at (self, x, z):
		"""elevation at (x east, z north) [m], bilinear"""
		u, v = x / GRID_STEP + GRID_N, z / GRID_STEP + GRID_N
		u = min (max (u, 0.0), 2 * GRID_N - 1e-6); v = min (max (v, 0.0), 2 * GRID_N - 1e-6)
		i, j = int (u), int (v); fu, fv = u - i, v - j
		g = self.g
		return (g[j][i] * (1 - fu) + g[j][i+1] * fu) * (1 - fv) + (g[j+1][i] * (1 - fu) + g[j+1][i+1] * fu) * fv
	def footprint (self, x, z, b, w, d, step=20.0):
		"""elevations over a rectangle w x d [m] about (x, z), turned to bearing b [deg]"""
		s, c = math.sin (math.radians (b)), math.cos (math.radians (b))
		e = []
		nx, nz = max (1, int (w / step)), max (1, int (d / step))
		for i in range (nx + 1):
			for k in range (nz + 1):
				lx, lz = -w / 2 + w * i / nx, -d / 2 + d * k / nz
				e.append (self.at (x + lx * c + lz * s, z - lx * s + lz * c))
		return e
	def water (self, x, z):
		return abs (self.at (x, z)) < 0.3

GRADE_BLEND = 50.0 # (as TerrainDetail's GRADE_BLEND)

class GradedTerrain (Terrain):
	"""the terrain with graded areas (east, north, length, width, bearing,
	plane): levelled to the data's mean over them (9 x 5 points), or the
	plane fitted to them, blended back over GRADE_BLEND (as TerrainDetail)"""
	def __init__ (self, T, grades):
		Terrain.__init__ (self, T.g)
		self.areas = []
		sums = {}
		for (x, z, ln, wd, bb, plane, group) in grades:
			s, c = math.sin (math.radians (bb)), math.cos (math.radians (bb))
			al, ac = (s, c), (c, -s) # (along: east, north; across: to the right)
			hl, hw = 0.5 * ln, 0.5 * wd
			se = sse = tse = ss = tt = 0.0
			for i in range (9):
				for k in range (5):
					sv, tv = hl * (2.0 * i / 8 - 1.0), hw * (2.0 * k / 4 - 1.0)
					e = T.at (x + al[0] * sv + ac[0] * tv, z + al[1] * sv + ac[1] * tv)
					se += e; sse += sv * e; tse += tv * e; ss += sv * sv; tt += tv * tv
			g0 = se / 45.0
			gl = sse / ss if plane and ss > 0 else 0.0
			gw = tse / tt if plane and tt > 0 else 0.0
			self.areas.append ([x, z, al, ac, hl, hw, g0, gl, gw, group])
			if group: sums[group] = (sums.get (group, (0.0, 0))[0] + se, sums.get (group, (0.0, 0))[1] + 45)
		for a in self.areas: # (a group: one level, the mean over all of it)
			if a[9]: a[6], a[7], a[8] = sums[a[9]][0] / sums[a[9]][1], 0.0, 0.0
		self.areas = [tuple (a[:9]) for a in self.areas]
	def inside (self, x, z):
		lim = GRID_N * GRID_STEP
		return abs (x) < lim and abs (z) < lim
	def at (self, x, z):
		h = Terrain.at (self, x, z)
		w, g = 0.0, h
		for (cx, cz, al, ac, hl, hw, g0, gl, gw) in self.areas:
			ox, oz = x - cx, z - cz
			sv, tv = ox * al[0] + oz * al[1], ox * ac[0] + oz * ac[1]
			ds, dt = max (0.0, abs (sv) - hl), max (0.0, abs (tv) - hw)
			d = math.hypot (ds, dt)
			if d >= GRADE_BLEND: continue
			u = 1.0 - d / GRADE_BLEND
			wg = u * u * (3 - 2 * u)
			if wg > w: w, g = wg, g0 + gl * max (-hl, min (hl, sv)) + gw * max (-hw, min (hw, tv))
		return h * (1.0 - w) + g * w

def read_grid (path):
	grids = {}
	for l in open (path):
		p = l.split ()
		k = p.index ("ELEVGRID") if "ELEVGRID" in p else -1
		if k < 0: continue
		name, j = p[k + 1], int (p[k + 2])
		grids.setdefault (name, {})[j] = [float (x) for x in p[k + 3:]]
	return {n: Terrain ([rows[j] for j in range (-GRID_N, GRID_N + 1)]) for n, rows in grids.items()}

def BF (e, n):
	"""(east, north) -> base frame (x south, z east)"""
	return (-n, e)

def BROT (bearing):
	"""ROT [deg] of a mesh whose front (+z) faces bearing [deg, clockwise from north]
	(Orbiter turns x' = x cos r - z sin r, z' = x sin r + z cos r)"""
	return ((90.0 - bearing + 180.0) % 360.0) - 180.0

class Rnd:
	def __init__ (self, s): self.s = zlib.crc32 (s.encode ()) or 1
	def __call__ (self, a=0.0, b=1.0):
		self.s = (self.s * 1103515245 + 12345) & 0x7fffffff
		return a + (b - a) * self.s / 0x7fffffff

class Layout:
	"""objects (base frame), ground strips, floodlights of a base"""
	def __init__ (self, name, terrain):
		self.name, self.T = name, terrain
		self.objs, self.ground, self.floods = [], [], []
		self.foot = [] # (the objects' footprints: w, d)
		self.pads, self.runways, self.keep = [], [], [] # new pads (e, n, bearing), runways (end, end, width), areas taken (e, n, r)

	def place (self, mesh, x, z, bearing, w, d, shadow=True):
		"""a kit mesh at (x, z), its front (+z) towards bearing [deg]; sunk to the
		lowest ground under its footprint w x d [m]"""
		e = self.T.footprint (x, z, bearing, w, d)
		y = min (e) - self.T.at (x, z) - 0.3
		self.objs.append ((mesh, x, y, z, bearing, shadow))
		self.foot.append ((w, d))

	def grades (self):
		"""graded ground (Base::Grades): aprons level, runways on a plane;
		(east, north, length, width, bearing, plane)"""
		out = []
		for g in self.ground:
			if g[0] not in ("rect", "grade"): continue
			_, (x, z), (w, d), bb, mat = g
			out.append ([x, z, d, w, bb, 0, 0])
		# (aprons touching each other: one group, one level)
		def corners (a):
			s, c = math.sin (math.radians (a[4])), math.cos (math.radians (a[4]))
			return [(a[0] + s * l + c * w, a[1] + c * l - s * w) for l in (-a[2] / 2, a[2] / 2) for w in (-a[3] / 2, a[3] / 2)]
		def near (a, b):
			s, c = math.sin (math.radians (b[4])), math.cos (math.radians (b[4]))
			for (x, z) in corners (a):
				ox, oz = x - b[0], z - b[1]
				if abs (ox * s + oz * c) <= b[2] / 2 + 5 and abs (ox * c - oz * s) <= b[3] / 2 + 5: return True
			return False
		grp = list (range (len (out)))
		def find (i):
			while grp[i] != i: i = grp[i]
			return i
		for i in range (len (out)):
			for k in range (i + 1, len (out)):
				if near (out[i], out[k]) or near (out[k], out[i]): grp[find (k)] = find (i)
		roots = sorted ({find (i) for i in range (len (out))})
		for i in range (len (out)): out[i][6] = roots.index (find (i)) + 1
		for (a, b, w) in self.runways:
			dx, dz = b[0] - a[0], b[1] - a[1]
			out.append ([0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1]), math.hypot (dx, dz) + 120.0, w + 40.0, math.degrees (math.atan2 (dx, dz)) % 360.0, 1, 0])
		return [tuple (o) for o in out]

	def regrade (self):
		"""the objects' heights over the graded ground (as the core grades it:
		TerrainDetail), not the data's"""
		G = GradedTerrain (self.T, self.grades ())
		for k, (mesh, x, y, z, bearing, shadow) in enumerate (self.objs):
			w, d = self.foot[k]
			if not G.inside (x, z): continue
			e = G.footprint (x, z, bearing, w, d)
			self.objs[k] = (mesh, x, min (e) - G.at (x, z) - 0.3, z, bearing, shadow)

	def strip (self, pts, width, mat):
		"""a ground strip (road) along points (x, z)"""
		self.ground.append (("strip", pts, width, mat))

	def grade (self, x, z, bearing, w, d):
		"""graded ground only (no mesh): levelled with the aprons it touches"""
		self.ground.append (("grade", (x, z), (w, d), bearing, None))

	def rect (self, x, z, bearing, w, d, mat):
		self.ground.append (("rect", (x, z), (w, d), bearing, mat))

def existing (cfgtext):
	"""a base's own objects (outside the kit's block): points they occupy
	(east, north) and what it has (launch structures, pads, runways)"""
	t = cfgtext.replace ("\r\n", "\n")
	if BEGIN in t: t = t[:t.index (BEGIN)] + t[t.index (END) + len (END):]
	body = t[t.upper ().find ("BEGIN_OBJECTLIST"):]
	pts, has = [], {"launch": False, "pads": 0, "runway": False}
	objs = [] # (type, its first position)
	obj = None
	for l in body.split ("\n"):
		w = l.split (";")[0].split ()
		if not w: continue
		k = w[0].upper ()
		if k in ("POS", "END1", "END2") and len (w) >= 4:
			x, z = float (w[1]), float (w[3])
			pts.append ((z, -x))
			if obj and objs and objs[-1][1] is None: objs[-1] = (objs[-1][0], (x, z))
		elif k == "FILE" and len (w) > 1:
			if re.search (r"KLC39|tower_", w[1], re.I): has["launch"] = True
		elif k in ("END", "END_OBJECTLIST", "BEGIN_OBJECTLIST"): obj = None
		elif not l[:1].isspace ():
			obj = k
			objs.append ((k, None))
			if k.startswith ("LPAD"): has["pads"] += 1
			if k == "RUNWAY": has["runway"] = True
	# (only a pad marker at the origin)
	has["bare"] = bool (objs) and all (t.startswith ("LPAD") and (p is None or abs (p[0]) + abs (p[1]) < 1.0) for t, p in objs)
	# (the runways: their lines, a point every 200 m)
	ends = re.findall (r"^RUNWAY\s*$\s*END1\s+([-\d.]+)\s+[-\d.]+\s+([-\d.]+)\s*$\s*END2\s+([-\d.]+)\s+[-\d.]+\s+([-\d.]+)", body, re.M)
	for (x1, z1, x2, z2) in ends:
		a, b = (float (z1), -float (x1)), (float (z2), -float (x2))
		n = max (1, int (math.hypot (b[0] - a[0], b[1] - a[1]) / 200))
		pts += [(a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n) for i in range (n + 1)]
	return pts, has

def clear_of (pts, x, z, r):
	return all ((p[0] - x) ** 2 + (p[1] - z) ** 2 > r * r for p in pts)

def pad_complex (L, r, cx, cz, b0):
	"""a launch pad complex about a pad at (cx, cz), turned to bearing b0"""
	s, c = math.sin (math.radians (b0)), math.cos (math.radians (b0))
	P = lambda lx, lz: (cx + lx * c + lz * s, cz - lx * s + lz * c) # pad frame (x right, z ahead) -> base
	tower = "tower_red" if r () < 0.6 else "tower_grey"
	L.place (tower, *P (0, -52), b0, 12, 30)       # (behind the pad, arms towards it)
	for (lx, lz) in ((-62, -62), (62, -62), (-62, 62), (62, 62)):
		L.place ("mast", *P (lx, lz), b0, 4, 4)
	L.place ("watertower", *P (-150, -40), b0, 22, 22)
	L.place ("spheres", *P (170, -30), b0 + 90, 50, 30)
	L.place ("tanks", *P (160, 60), b0 + 90, 32, 32)
	for (lx, lz, w, d) in ((0, -70, 180, 60), (0, 70, 180, 60), (-70, 0, 40, 80), (70, 0, 40, 80)):
		L.rect (*P (lx, lz), b0, w, d, "apron")
	L.grade (cx, cz, b0, 100, 80) # (the pad between the aprons: one level with them)
	for (lx, lz) in ((0, -100), (0, 100), (-100, 0), (100, 0)):
		px, pz = P (lx, lz)
		L.floods.append ((px, 25, pz, cx - px, 0.15 * math.hypot (lx, lz), cz - pz))
	L.keep.append ((cx, cz, 260.0))

def flat_site (T, occ, w, d, r_occ, rmin, rmax, bearings, step=250.0):
	"""the flattest dry site for a w x d [m] footprint clear of occ by r_occ,
	rmin to rmax [m] from the origin: (score, x, z, bearing) or None"""
	best = None
	lim = GRID_N * GRID_STEP
	n = int (rmax / step)
	for i in range (-n, n + 1):
		for k in range (-n, n + 1):
			x, z = i * step, k * step
			rr = math.hypot (x, z)
			if rr < rmin or rr > rmax: continue
			for bb in bearings:
				sb, cb = abs (math.sin (math.radians (bb))), abs (math.cos (math.radians (bb)))
				ext_x, ext_z = 0.5 * (w * cb + d * sb), 0.5 * (w * sb + d * cb)
				if abs (x) + ext_x > lim or abs (z) + ext_z > lim: continue
				# (clear of the base's objects: points along its long axis)
				sa, ca = math.sin (math.radians (bb)), math.cos (math.radians (bb))
				m = max (1, int (d / 200))
				if not all (clear_of (occ, x + sa * d * (q / m - 0.5), z + ca * d * (q / m - 0.5), r_occ + 0.5 * w) for q in range (m + 1)):
					continue
				e = T.footprint (x, z, bb, w, d, 50.0)
				if any (abs (v) < 0.3 for v in e): continue
				score = max (e) - min (e) + 0.001 * rr
				if best is None or score < best[0]: best = (score, x, z, bb)
	return best

def runway (L, r, cx, cz, bb, length):
	"""a runway centred at (cx, cz) along bearing bb, its lights, a hangar and apron beside it"""
	sa, ca = math.sin (math.radians (bb)), math.cos (math.radians (bb))
	e1, e2 = (cx - sa * length / 2, cz - ca * length / 2), (cx + sa * length / 2, cz + ca * length / 2)
	L.runways.append ((e1, e2, 60.0))
	# (beside it, at its middle: an apron, a hangar, the taxiway)
	px, pz = ca, -sa # (to its right)
	ax, az = cx + px * 140, cz + pz * 140
	L.rect (ax, az, bb, 160, 80, "apron")
	L.place ("hangar", ax + px * 70, az + pz * 70, bb + 270, 50, 60)
	L.strip ([(cx + px * 30, cz + pz * 30), (ax - px * 40, az - pz * 40)], 22.0, "asphalt")
	L.keep.append ((cx, cz, 0.5 * length + 150))
	return (ax, az)

def plan (name, T, cfgtext):
	r = Rnd (name)
	L = Layout (name, T)
	occ, has = existing (cfgtext)
	occ = list (occ)
	links = [] # points to join by roads
	camp = None
	if has["bare"]:
		# the pad (the base's pad 1, at its origin): its complex
		b0 = round (r (0, 360) / 15) * 15
		pad_complex (L, r, 0.0, 0.0, b0)
		links.append ((0.0, 0.0))
		# the campus: the flattest land without water 0.9 to 1.5 km away
		best = None
		for k in range (24):
			bb = 15.0 * k
			for dist in (900, 1200, 1500):
				cx, cz = dist * math.sin (math.radians (bb)), dist * math.cos (math.radians (bb))
				e = T.footprint (cx, cz, bb, 560, 320, 25.0)
				if any (abs (v) < 0.3 for v in e): continue
				road = [T.at (cx * f, cz * f) for f in [i / 20 for i in range (21)]]
				if sum (1 for v in road if abs (v) < 0.3) > 1: continue
				score = (max (e) - min (e)) + 0.002 * dist + 0.2 * max (abs (road[i + 1] - road[i]) for i in range (20))
				if best is None or score < best[0]: best = (score, bb, dist, cx, cz)
		if best:
			_, bb, dist, cx, cz = best
			camp = campus (L, r, cx, cz, bb)
			links.append (camp)
		else: # (no room for a campus: hover pads alone)
			occ1 = occ + [(k[0], k[1]) for k in L.keep]
			site = flat_site (T, occ1, 120, 300, 150, 300, 3000, [0, 45, 90, 135], 100.0)
			if site:
				_, x, z, bb = site
				sa, ca = math.sin (math.radians (bb)), math.cos (math.radians (bb))
				for k in (-1, 0, 1):
					L.pads.append ((x + sa * 95 * k, z + ca * 95 * k, bb))
				L.rect (x, z, bb, 110, 300, "apron")
				L.keep.append ((x, z, 200.0))
				links.append ((x, z))
	elif not has["launch"]:
		# a launch pad of its own, clear of the base's objects
		site = flat_site (T, occ, 420, 420, 300, 600, 3600, [0, 45, 90, 135])
		if site:
			_, x, z, bb = site
			L.pads.append ((x, z, bb))
			pad_complex (L, r, x, z, bb)
			links.append ((x, z))
	if not has["runway"]:
		occ2 = occ + [(k[0], k[1]) for k in L.keep] + [p for g in L.ground if g[0] == "strip" for p in g[1]]
		occ2 += [(o[1], o[3]) for o in L.objs]
		site = None
		for length in (3000, 2400, 1800):
			site = flat_site (T, occ2, 300, length, 120, 0, 3900 - length / 2, range (0, 180, 10))
			if site: break
		if site:
			_, x, z, bb = site
			links.append (runway (L, r, x, z, bb, length))
		elif name in KNOWN_RUNWAYS:
			lng, lat, bb, length = KNOWN_RUNWAYS[name]
			_, lng0, lat0 = read_cfg_text (cfgtext)
			R = 6371010.0
			x = math.radians (lng - lng0) * R * math.cos (math.radians (lat0))
			z = math.radians (lat - lat0) * R
			links.append (runway (L, r, x, z, bb, length))
	# roads: each new part to the nearest earlier one (or the base's origin)
	for i in range (1, len (links)):
		a = min (links[:i], key=lambda p: (p[0] - links[i][0]) ** 2 + (p[1] - links[i][1]) ** 2)
		b = links[i]
		n = max (1, int (math.hypot (b[0] - a[0], b[1] - a[1]) / 100))
		L.strip ([(a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n) for k in range (n + 1)], 10.0, "asphalt")
	return L

def campus (L, r, cx, cz, bb):
	"""the base's campus at (cx, cz), its front (-z) towards the pad (bearing bb + 180)"""
	sb, cb = math.sin (math.radians (bb)), math.cos (math.radians (bb))
	C = lambda lx, lz: (cx + lx * cb + lz * sb, cz - lx * sb + lz * cb) # campus frame (z: away from the pad)
	face = bb + 180.0 # (buildings facing the pad)
	big = "assembly" if r () < 0.6 else "assembly_small"
	ax, az = C (-90, 40)
	L.place (big, ax, az, face, 150, 70)
	L.rect (ax, az, face, 170, 100, "apron")
	for (lx, lz) in ((60, 60), (120, 60)):
		L.place ("hangar", *C (lx, lz), face, 50, 60)
		L.rect (*C (lx, lz - 32), face, 52, 20, "apron")
	for k, (lx, lz) in enumerate (((40, 140), (90, 140), (140, 140))):
		if k == 2 and r () < 0.4: continue
		L.place ("office", *C (lx, lz), face, 42, 18)
	L.place ("radome", *C (-170, 150), face, 34, 18)
	# hover pads: a row beside the hangars
	for k in range (3):
		px, pz = C (220 + 85 * k, 40)
		L.pads.append ((px, pz, face))
	L.rect (*C (305, 40), face, 270, 100, "apron")
	# blockhouse: on the way, some 400 m from the pad
	bx, bz = 400 * sb, 400 * cb
	L.place ("control", bx + 60 * cb, bz - 60 * sb, bb + 180, 44, 32)
	L.strip ([(bx, bz), (bx + 60 * cb, bz - 60 * sb)], 6.0, "asphalt")
	L.strip ([C (-200, 0), C (400, 0)], 10.0, "asphalt")
	L.strip ([C (90, 0), C (90, 170)], 8.0, "asphalt")
	L.strip ([C (-90, 0), C (-90, 5)], 8.0, "asphalt")
	L.strip ([C (-170, 0), C (-170, 140)], 6.0, "asphalt")
	L.rect (*C (90, 100), face, 60, 25, "asphalt") # (car park)
	L.keep.append ((cx, cz, 350.0))
	return (cx, cz)

def ground_mesh (L, path):
	"""roads and aprons as a mesh (base frame; wrapped onto the ground by Orbiter)"""
	B = MeshBuilder ()
	SEG = 20.0
	for g in L.ground:
		if g[0] == "strip":
			_, pts, w, mat = g
			for (a, b) in zip (pts[:-1], pts[1:]):
				dx, dz = b[0] - a[0], b[1] - a[1]
				l = math.hypot (dx, dz)
				if l < 1e-3: continue
				n = max (1, int (math.ceil (l / SEG)))
				px, pz = -dz / l * w / 2, dx / l * w / 2
				for k in range (n):
					p0 = (a[0] + dx * k / n, a[1] + dz * k / n); p1 = (a[0] + dx * (k + 1) / n, a[1] + dz * (k + 1) / n)
					q = [(p0[0] - px, p0[1] - pz), (p1[0] - px, p1[1] - pz), (p1[0] + px, p1[1] + pz), (p0[0] + px, p0[1] + pz)]
					B.poly ([(BF (*e)[0], 0.05, BF (*e)[1]) for e in q], (0, 1, 0), mat, tex="Concrete.dds")
		elif g[0] == "rect":
			_, (x, z), (w, d), bb, mat = g
			s, c = math.sin (math.radians (bb)), math.cos (math.radians (bb))
			nx, nz = max (1, int (math.ceil (w / SEG))), max (1, int (math.ceil (d / SEG)))
			for i in range (nx):
				for k in range (nz):
					q = [(-w / 2 + w * ii / nx, -d / 2 + d * kk / nz) for (ii, kk) in ((i, k), (i + 1, k), (i + 1, k + 1), (i, k + 1))]
					e = [(x + lx * c + lz * s, z - lx * s + lz * c) for (lx, lz) in q]
					B.poly ([(BF (*p)[0], 0.03, BF (*p)[1]) for p in e], (0, 1, 0), mat, tex="Concrete.dds")
	B.write (path)

BEGIN, END = ";; BaseKit: generic launch complex (Utils/BaseKit/basekit.py)", ";; BaseKit end"

GBEGIN, GEND = ";; BaseKit grade: graded ground of the kit (Utils/BaseKit/basekit.py)", ";; BaseKit grade end"

def grade_block (L):
	"""BEGIN_GRADE lines (Base::Grades): x z length width bearing plane group, base frame"""
	g = L.grades ()
	if not g: return []
	out = [GBEGIN, "BEGIN_GRADE", "; x z (base frame: x south, z east) length width [m] bearing [deg] plane group"]
	for (e, n, ln, wd, bb, plane, group) in g:
		x, z = BF (e, n)
		out.append ("%.1f %.1f %.1f %.1f %.1f %d %d" % (x, z, ln, wd, bb % 360.0, plane, group))
	return out + ["END_GRADE", GEND]

def cfg_block (L):
	out = [BEGIN, "MESH", "\tFILE BaseKit\\%s_ground" % L.name, "\tPOS 0 0 0", "\tWRAPTOSURFACE", "\tUNDERSHADOWS", "\tOWNMATERIAL", "END"]
	for (e, n, bearing) in L.pads:
		x, z = BF (e, n)
		out += ["LPAD2", "\tPOS %.1f 0 %.1f" % (x, z), "\tSCALE 1", "\tROT %.1f" % BROT (bearing), "\tTEX Lpad02", "END"]
	for (a, b, w) in L.runways:
		(x1, z1), (x2, z2) = BF (*a), BF (*b)
		rl = math.hypot (x2 - x1, z2 - z1)
		segs = [(0.018, 0.25, 0.5, 1, 0.875), (0.088, 0, 0.25, 0.5, 0), (0.018, 0.25, 0.5, 0.75, 0.625), (0.088, 0, 0.25, 0.5, 0),
			(0.576, 0, 0.25, 1, 0.5), (0.088, 0, 0.25, 0.5, 0), (0.02, 0.25, 0.5, 0.625, 0.75), (0.088, 0, 0.25, 0, 0.5),
			(0.02, 0.25, 0.5, 0.875, 1)]
		out += ["RUNWAY", "\tEND1 %.1f 0 %.1f" % (x1, z1), "\tEND2 %.1f 0 %.1f" % (x2, z2), "\tWIDTH %g" % w, "\tNRWSEG %d" % len (segs)]
		for k, sg in enumerate (segs): # (subdivided: vertices at most 40 m apart, on the ground)
			out.append ("\tRWSEG%d %d \t%g \t%g \t%g \t%g \t%g" % ((k + 1, max (1, int (math.ceil (sg[0] * rl / 40.0)))) + sg))
		out += ["\tRWTEX Runway2", "END",
			"RUNWAYLIGHTS", "\tEND1 %.1f 0 %.1f" % (x1, z1), "\tEND2 %.1f 0 %.1f" % (x2, z2), "\tWIDTH %g" % w, "\tCOUNT1 40", "END"]
	for (mesh, e, y, n, bearing, shadow) in L.objs:
		x, z = BF (e, n)
		out += ["MESH", "\tFILE BaseKit\\%s" % mesh, "\tPOS %.1f %.1f %.1f" % (x, y, z), "\tROT %.1f" % BROT (bearing), "\tOWNMATERIAL"]
		if shadow: out.append ("\tSHADOW")
		out.append ("END")
	out.append (END)
	return out

def update_cfg (path, L):
	t = open (path, encoding="latin-1", newline="").read ()
	nl = "\r\n" if "\r\n" in t else "\n"
	lines = t.replace ("\r\n", "\n").split ("\n")
	if BEGIN in lines:
		i0, i1 = lines.index (BEGIN), lines.index (END)
		del lines[i0:i1 + 1]
	k = next (i for i, l in enumerate (lines) if l.strip ().upper () == "END_OBJECTLIST")
	lines[k:k] = cfg_block (L)
	# the graded ground (outside the object list)
	if GBEGIN in lines:
		i0, i1 = lines.index (GBEGIN), lines.index (GEND)
		del lines[i0:i1 + 1]
	gr = grade_block (L)
	if gr:
		k = next (i for i, l in enumerate (lines) if l.strip ().upper () == "BEGIN_OBJECTLIST")
		while k > 0 and lines[k - 1].strip ().startswith (";"): k -= 1 # (before the list's comment)
		lines[k:k] = gr + [""]
	# the pad drawn (it was a marker only)
	t2 = nl.join (lines)
	t2 = re.sub (r"(LPAD2\s*[\r\n]+\s*POS 0 0 0\s*[\r\n]+\s*SCALE) 0 0 0", r"\1 1", t2)
	with open (path, "w", encoding="latin-1", newline="") as f: f.write (t2)

def vk_name (title):
	"""the file of a base's lights (Config/VK/Bases): its name's bytes as in
	its configuration file (read as latin-1), not re-encoded (UTF-8 names)"""
	return os.fsdecode ((title.replace (" ", "_") + ".cfg").encode ("latin-1"))

def update_vk (path, L, title):
	"""the base's floodlights (Config/VK/Bases). Their colour r g b is the
	lamp's brightness: its glow's, and as a light r g b x 0.01 of sunlight
	(the Vulkan client's FLOOD_IRRADIANCE, VObjects.cpp) out to a fifth of the
	range (80 m), the inverse square beyond: 6 lights the pad 100 m away with
	~2 % of sunlight, a floodlit stadium"""
	lines = ["; Lights of %s for the Vulkan client (base frame [m]: x south, y up, z east)" % title,
		"; FLOODLIGHT x y z  dx dy dz  range[m]  r g b  umbra penumbra [deg]",
		"; r g b: the lamp's brightness (its glow; about 6 for a xenon flood). As a light it gives r g b x 0.01",
		"; of sunlight (VObjects.cpp FLOOD_IRRADIANCE) out to a fifth of the range, the inverse square beyond",
		"; (made by Utils/BaseKit/basekit.py: floodlights about the pad)"]
	for (e, y, n, de, dy, dn) in L.floods:
		(x, z), (dx, dz) = BF (e, n), BF (de, dn)
		l = math.sqrt (dx * dx + dy * dy + dz * dz)
		lines.append ("FLOODLIGHT %.0f %.0f %.0f  %.3f %.3f %.3f  400  6 5.6 5  30 50" % (x, y, z, dx / l, dy / l, dz / l))
	with open (path, "w", newline="\n") as f: f.write ("\n".join (lines) + "\n")

def cmd_layout (gridfile, cfgdirs, meshdir=None, vkdir=None):
	grids = read_grid (gridfile)
	for b in kit_bases (cfgdirs[0]):
		if b not in grids: print ("%s: no terrain grid, skipped" % b); continue
		cfgtext = open (os.path.join (cfgdirs[0], b + ".cfg"), encoding="latin-1").read ()
		L = plan (b, grids[b], cfgtext)
		if not (L.objs or L.pads or L.runways): print ("%-15s nothing to add" % b); continue
		L.regrade ()
		for d in cfgdirs:
			update_cfg (os.path.join (d, b + ".cfg"), L)
		if meshdir: ground_mesh (L, os.path.join (meshdir, "BaseKit", b + "_ground.msh"))
		if vkdir:
			t, _, _ = read_cfg (os.path.join (cfgdirs[0], b + ".cfg"))
			title = re.search (r"Name\s*=\s*(.+)", t).group (1).strip ()
			update_vk (os.path.join (vkdir, vk_name (title)), L, title)
		print ("%-15s %2d objects, %d new pads, %d runways" % (b, len (L.objs), len (L.pads), len (L.runways)))

if __name__ == "__main__":
	a = sys.argv[1:]
	if not a: print (__doc__); sys.exit (1)
	opt = lambda k: a[a.index (k) + 1] if k in a else None
	if a[0] == "meshes": cmd_meshes (a[1])
	elif a[0] == "sampler": cmd_sampler (a[1], a[2])
	elif a[0] == "layout":
		dirs = [x for x in a[2:] if not x.startswith ("--") and x not in (opt ("--meshes"), opt ("--vk"))]
		cmd_layout (a[1], dirs, opt ("--meshes"), opt ("--vk"))
	else: print (__doc__); sys.exit (1)
