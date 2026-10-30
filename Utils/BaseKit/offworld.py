#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Off-world bases: a kit of near-future surface installations, placed
about each base's own objects by the base's size (outpost, station,
colony) and its body (soil colour; vacuum: habitats under regolith berms
against radiation, solar power; Mars: the same, dust-coloured; Titan:
insulated, no solar power; Europa: on legs on the ice, nuclear power;
Mercury: sunshades over the habitats).

    offworld.py meshes <Meshes dir>
        writes the kit's meshes (Meshes/BaseKit/space_*.msh)
    offworld.py sampler <out.lua>
        a Lua script (run with Orbiter, headless) logging the elevation
        about each base in BASES (as basekit.py sampler; lines
        ELEVGRID <base> <row> ...)
    offworld.py skeleton
        writes the files of the new bases (BASES: new) where they are not yet
    offworld.py layout <grid log> [--meshes <Meshes dir>] [--vk <Config/VK/Bases>]
        writes each base's installations into its configuration files (the
        BaseKit block of basekit.py, replacing an earlier one), its ground
        and corridors as meshes of its own, floodlights for the Vulkan client
    offworld.py aerostat [--vk <Config/VK/Bases>]
        writes the floating base on Venus (AEROSTAT: Altitude, its deck)

Positions are planned east and north of the base [m]; written in Orbiter's
base frame (x south, z east), as basekit.py.
"""

import math, os, re, sys

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import basekit as bk
from basekit import MeshBuilder, MATS, Mat, BF, BROT, Rnd, Layout

# base: (body, size, configuration files (relative to the repository), new)
# new: a base made here (offworld.py skeleton writes its file: the header, a
# beacon and pad 1 at its origin): (beacon id, frequency [MHz], pads, extras)
M, S = "Src/Celbody/", "Src/Celbody/SPICE/Config/"
BASES = {
	"Brighton": ("Moon", "colony", [M + "Moon/Config/Moon/Base/Brighton.cfg", S + "Moon/Base/Brighton.cfg"], None),
	"Olympus": ("Mars", "station", [M + "Vsop87/Mars/Config/Mars/Base/Olympus.cfg", S + "Mars/Base/Olympus.cfg"], None),
	# (the Moon's south pole: the ridge between Shackleton and de Gerlache, lit most of the year)
	"Shackleton": ("Moon", "station", [M + "Moon/Config/Moon/Base/Shackleton.cfg", S + "Moon/Base/Shackleton.cfg"],
		("SHK", 115.30, 3, {"loc": (-137.31, -89.45)})),
	# (the far side: Daedalus crater, shielded from the Earth's radio noise)
	"Daedalus": ("Moon", "outpost", [M + "Moon/Config/Moon/Base/Daedalus.cfg", S + "Moon/Base/Daedalus.cfg"],
		("DDL", 113.70, 2, {"loc": (178.9133, -5.8918), "radio": True})),
	# (Jezero crater, where Perseverance landed)
	"Jezero": ("Mars", "station", [M + "Vsop87/Mars/Config/Mars/Base/Jezero.cfg", S + "Mars/Base/Jezero.cfg"],
		("JEZ", 115.80, 3, {"loc": (77.4393, 18.4392)})),
	# (the north polar cap, Planum Boreum: water ice)
	"Boreum": ("Mars", "outpost", [M + "Vsop87/Mars/Config/Mars/Base/Boreum.cfg", S + "Mars/Base/Boreum.cfg"],
		("BOR", 112.90, 2, {"loc": (-19.9272, 84.0059)})),
	# (the dunes by Selk crater, Dragonfly's destination)
	"Selk": ("Titan", "outpost", [M + "Satsat/Titan/Config/Titan/Base/Selk.cfg", S + "Titan/Base/Selk.cfg"],
		("SLK", 114.60, 2, {"loc": (161.00, 6.50)})),
	# (Conamara Chaos: young ice over the ocean)
	"Conamara": ("Europa", "outpost", [M + "Galsat/Europa/Config/Europa/Base/Conamara.cfg", S + "Europa/Base/Conamara.cfg"],
		("CNM", 113.40, 2, {"loc": (86.70, 9.70)})),
	# (Valhalla: outside Jupiter's radiation belts; NASA's HOPE study)
	"Valhalla": ("Callisto", "station", [M + "Galsat/Callisto/Config/Callisto/Base/Valhalla.cfg", S + "Callisto/Base/Valhalla.cfg"],
		("VHL", 116.20, 3, {"loc": (-56.00, 14.70)})),
	# (Occator crater, its bright salts; Ceres is in the SPICE solar system only: no elevation data, a flat grid)
	"Occator": ("Ceres", "outpost", [S + "Ceres/Base/Occator.cfg"],
		("OCC", 112.30, 2, {"loc": (-120.67, 19.82), "flat": True})),
	# (Prokofiev crater by Mercury's north pole: ice in its shadows, the sun at the horizon)
	"Prokofiev": ("Mercury", "outpost", [M + "Vsop87/Mercury/Config/Mercury/Base/Prokofiev.cfg", S + "Mercury/Base/Prokofiev.cfg"],
		("PRK", 114.10, 2, {"loc": (-62.90, 85.75)})),
}

# the floating base: Venus, 55 km up (about 0.5 bar and 30 C), over Aphrodite Terra
AEROSTAT = ("Aphrodite", "Venus", 105.0, -5.0, 55000.0, "APH", 115.10,
	[M + "Vsop87/Venus/Config/Venus/Base/Aphrodite.cfg", S + "Venus/Base/Aphrodite.cfg"])

# bodies: soil colour, and what their bases are like
BODIES = {
	"Moon":     dict (soil=(0.40, 0.39, 0.37), berms=True,  solar=True,  legs=False, shades=False, insul=False),
	"Mars":     dict (soil=(0.52, 0.32, 0.20), berms=True,  solar=True,  legs=False, shades=False, insul=False),
	"Mercury":  dict (soil=(0.34, 0.33, 0.32), berms=True,  solar=True,  legs=False, shades=True,  insul=False),
	"Titan":    dict (soil=(0.42, 0.34, 0.22), berms=False, solar=False, legs=False, shades=False, insul=True),
	"Europa":   dict (soil=(0.78, 0.75, 0.70), berms=False, solar=False, legs=True,  shades=False, insul=False),
	"Callisto": dict (soil=(0.28, 0.26, 0.24), berms=True,  solar=False, legs=False, shades=False, insul=False),
	"Ceres":    dict (soil=(0.22, 0.21, 0.20), berms=True,  solar=True,  legs=False, shades=False, insul=False),
}

MATS.update ({
	"hull": Mat ("hull", (0.84, 0.84, 0.82), (0.4, 0.4, 0.4), 30),
	"hull_dark": Mat ("hull_dark", (0.30, 0.31, 0.33), (0.3, 0.3, 0.3), 20),
	"insulation": Mat ("insulation", (0.80, 0.52, 0.22)),
	"window": Mat ("window", (0.10, 0.10, 0.10), (0.6, 0.6, 0.6), 60, emissive=(0.55, 0.42, 0.25)),
	"growlight": Mat ("growlight", (0.34, 0.50, 0.34), (0.15, 0.15, 0.15), 20, emissive=(0.05, 0.03, 0.06)), # (glass over plants, a faint glow of the grow lights)
	"solar": Mat ("solar", (0.06, 0.08, 0.16), (0.7, 0.7, 0.8), 80),
	"radiator": Mat ("radiator", (0.92, 0.92, 0.92), (0.2, 0.2, 0.2), 10),
	"gold": Mat ("gold", (0.75, 0.58, 0.22), (0.8, 0.7, 0.4), 60),
	"rail": Mat ("rail", (0.45, 0.46, 0.48), (0.5, 0.5, 0.5), 40),
	"envelope": Mat ("envelope", (0.88, 0.86, 0.80), (0.25, 0.25, 0.25), 15),
	"deck": Mat ("deck", (0.32, 0.32, 0.33), (0.1, 0.1, 0.1), 10),
	"deck_edge": Mat ("deck_edge", (0.85, 0.62, 0.12)),
})
for b, p in BODIES.items():
	MATS["soil_" + b.lower ()] = Mat ("soil_" + b.lower (), p["soil"])
	MATS["track_" + b.lower ()] = Mat ("track_" + b.lower (), tuple (0.7 * c for c in p["soil"])) # (graded, compacted)

# ======================================================================
# Meshes (origin on the ground at the centre, +z the front)

def hab (B, x, z, L=14.0, r=3.2, y=1.6, mat="hull", along_x=False, windows=True):
	"""a cylindrical module lying on cradles, its axis along z (or x)"""
	n = 14
	for k in range (n):
		a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
		am = 0.5 * (a0 + a1)
		P = lambda a, s: ((x + s, y + r + r * math.cos (a), z + r * math.sin (a)) if along_x else (x + r * math.sin (a), y + r + r * math.cos (a), z + s))
		N = (0, math.cos (am), math.sin (am)) if along_x else (math.sin (am), math.cos (am), 0)
		B.poly ([P (a0, -L / 2), P (a1, -L / 2), P (a1, L / 2), P (a0, L / 2)], N, mat, uv=[(0, 0)] * 4)
	# end caps (domed a little), window strips on the sides
	for s in (-1, 1):
		ring = [((x + s * L / 2, y + r + r * math.cos (2 * math.pi * k / n), z + r * math.sin (2 * math.pi * k / n)) if along_x else
			(x + r * math.sin (2 * math.pi * k / n), y + r + r * math.cos (2 * math.pi * k / n), z + s * L / 2)) for k in range (n)]
		B.poly (ring, (s, 0, 0) if along_x else (0, 0, s), "hull_dark")
	if windows:
		for sd in (-1, 1):
			for i in range (3):
				c = -L / 3 + i * L / 3
				if along_x:
					B.poly ([(x + c - 1.2, y + r + 0.4, z + sd * (r + 0.02)), (x + c + 1.2, y + r + 0.4, z + sd * (r + 0.02)),
						(x + c + 1.2, y + r + 1.2, z + sd * (r + 0.02)), (x + c - 1.2, y + r + 1.2, z + sd * (r + 0.02))], (0, 0, sd), "window", uv=[(0, 0)] * 4)
				else:
					B.poly ([(x + sd * (r + 0.02), y + r + 0.4, z + c - 1.2), (x + sd * (r + 0.02), y + r + 0.4, z + c + 1.2),
						(x + sd * (r + 0.02), y + r + 1.2, z + c + 1.2), (x + sd * (r + 0.02), y + r + 1.2, z + c - 1.2)], (sd, 0, 0), "window", uv=[(0, 0)] * 4)
	for s in (-0.35, 0.35): # (cradles)
		if along_x: B.box (x + s * L - 0.4, x + s * L + 0.4, 0, y + 0.8, z - r * 0.8, z + r * 0.8, "hull_dark")
		else: B.box (x - r * 0.8, x + r * 0.8, 0, y + 0.8, z + s * L - 0.4, z + s * L + 0.4, "hull_dark")

def mound (B, x, z, w, d, h, mat, n=10):
	"""a regolith mound: a half ellipsoid w x d x h"""
	for i in range (n // 2):
		b0, b1 = math.pi / 2 * i / (n // 2), math.pi / 2 * (i + 1) / (n // 2)
		for k in range (n):
			a0, a1 = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
			P = lambda a, b: (x + 0.5 * w * math.cos (b) * math.sin (a), h * math.sin (b), z + 0.5 * d * math.cos (b) * math.cos (a))
			am, bm = 0.5 * (a0 + a1), 0.5 * (b0 + b1)
			N = (math.cos (bm) * math.sin (am) / w, math.sin (bm) / max (h, 1e-3), math.cos (bm) * math.cos (am) / d)
			pts = [P (a0, b0), P (a1, b0), P (a1, b1), P (a0, b1)]
			if i == n // 2 - 1: pts = pts[:3]
			B.poly (pts, N, mat, uv=[(0, 0)] * len (pts))

def m_hab (body):
	"""habitat: two modules side by side and a node, under a regolith berm
	(vacuum), on legs (ice), or insulated"""
	p = BODIES[body]
	B = MeshBuilder ()
	mat = "insulation" if p["insul"] else "hull"
	y = 3.0 if p["legs"] else 0.6
	hab (B, -4.5, 0, mat=mat, y=y, windows=not p["berms"])
	hab (B, 4.5, 0, mat=mat, y=y, windows=not p["berms"])
	B.cylinder (0, 9.5, 3.0, y, y + 5.5, "hull_dark", n=12)                  # (node / airlock at the front)
	B.box (-1.0, 1.0, 0, y + 2.4, 12.4, 13.4, "hull_dark")                    # (its hatch)
	if p["legs"]:
		for (lx, lz) in ((-7, -5), (7, -5), (-7, 5), (7, 5)): B.beam ((lx, 0, lz), (lx * 0.8, y, lz * 0.8), 0.5, "hull_dark")
	if p["berms"]:
		mound (B, 0, -1, 24, 22, 9.5, "soil_" + body.lower (), n=14)
	if p["shades"]:
		for (lx, lz) in ((-12, -10), (12, -10), (-12, 12), (12, 12)): B.beam ((lx, 0, lz), (lx, 13, lz), 0.4, "hull_dark")
		B.box (-14, 14, 13, 13.3, -12, 14, "radiator", bottom=True)
	return B

def m_dome ():
	"""colony dome: a ring wall with windows, a ribbed hemisphere"""
	B = MeshBuilder ()
	R, H = 22.0, 5.0
	B.cylinder (0, 0, R, 0, H, "hull", n=28, top=False)
	n = 28
	for k in range (n):
		if k % 4 != 0: continue
		a0, a1 = 2 * math.pi * (k + 0.5) / n, 2 * math.pi * (k + 2.5) / n
		B.poly ([(R * 1.002 * math.sin (a0), 1.5, R * 1.002 * math.cos (a0)), (R * 1.002 * math.sin (a1), 1.5, R * 1.002 * math.cos (a1)),
			(R * 1.002 * math.sin (a1), 3.5, R * 1.002 * math.cos (a1)), (R * 1.002 * math.sin (a0), 3.5, R * 1.002 * math.cos (a0))],
			(math.sin (0.5 * (a0 + a1)), 0, math.cos (0.5 * (a0 + a1))), "window", uv=[(0, 0)] * 4)
	B.sphere (0, H, 0, R, "hull", nlat=8, nlng=28, lat0=0.0)
	for k in range (0, n, 2): # (ribs)
		a = 2 * math.pi * k / n
		for i in range (8):
			b0, b1 = math.pi / 2 * i / 8, math.pi / 2 * (i + 1) / 8
			B.beam ((1.003 * R * math.cos (b0) * math.sin (a), H + 1.003 * R * math.sin (b0), 1.003 * R * math.cos (b0) * math.cos (a)),
				(1.003 * R * math.cos (b1) * math.sin (a), H + 1.003 * R * math.sin (b1), 1.003 * R * math.cos (b1) * math.cos (a)), 0.35, "hull_dark")
	B.box (-2.5, 2.5, 0, 3.2, R - 1, R + 5, "hull_dark")  # (entrance)
	return B

def m_greenhouse ():
	"""greenhouse: a long half-cylinder lit by grow lights, ribbed"""
	B = MeshBuilder ()
	r, L, n = 6.0, 40.0, 12
	for k in range (n):
		a0, a1 = math.pi * k / n - math.pi / 2, math.pi * (k + 1) / n - math.pi / 2
		am = 0.5 * (a0 + a1)
		B.poly ([(r * math.sin (a0), 1 + r * math.cos (a0), -L / 2), (r * math.sin (a1), 1 + r * math.cos (a1), -L / 2),
			(r * math.sin (a1), 1 + r * math.cos (a1), L / 2), (r * math.sin (a0), 1 + r * math.cos (a0), L / 2)],
			(math.sin (am), math.cos (am), 0), "growlight", uv=[(0, 0)] * 4)
	B.box (-r, r, 0, 1, -L / 2, L / 2, "hull_dark")
	for s in (-1, 1):
		B.poly ([(r * math.sin (math.pi * k / n - math.pi / 2), 1 + r * math.cos (math.pi * k / n - math.pi / 2), s * L / 2) for k in range (n + 1)], (0, 0, s), "hull")
	for i in range (9):
		zz = -L / 2 + L * i / 8
		for k in range (n):
			a0, a1 = math.pi * k / n - math.pi / 2, math.pi * (k + 1) / n - math.pi / 2
			B.beam ((1.01 * r * math.sin (a0), 1 + 1.01 * r * math.cos (a0), zz), (1.01 * r * math.sin (a1), 1 + 1.01 * r * math.cos (a1), zz), 0.25, "hull")
	return B

def m_solar ():
	"""solar field: two rows of tilted arrays on posts"""
	B = MeshBuilder ()
	for row in (-6, 6):
		for k in range (6):
			x = -25 + 10 * k
			B.beam ((x, 0, row), (x, 2.2, row), 0.3, "hull_dark")
			t = math.radians (30)
			B.poly ([(x - 4.5, 2.2 - 1.5 * math.sin (t), row - 1.5 * math.cos (t)), (x + 4.5, 2.2 - 1.5 * math.sin (t), row - 1.5 * math.cos (t)),
				(x + 4.5, 2.2 + 1.5 * math.sin (t), row + 1.5 * math.cos (t)), (x - 4.5, 2.2 + 1.5 * math.sin (t), row + 1.5 * math.cos (t))],
				(0, math.cos (t), -math.sin (t)), "solar", uv=[(0, 0)] * 4)
			B.poly ([(x - 4.5, 2.19 - 1.5 * math.sin (t), row - 1.5 * math.cos (t)), (x + 4.5, 2.19 - 1.5 * math.sin (t), row - 1.5 * math.cos (t)),
				(x + 4.5, 2.19 + 1.5 * math.sin (t), row + 1.5 * math.cos (t)), (x - 4.5, 2.19 + 1.5 * math.sin (t), row + 1.5 * math.cos (t))],
				(0, -math.cos (t), math.sin (t)), "hull_dark", uv=[(0, 0)] * 4)
	return B

def m_radiator ():
	"""radiator wing: two upright panels on a frame"""
	B = MeshBuilder ()
	for s in (-1, 1):
		B.box (s * 6 - 0.1, s * 6 + 0.1, 1, 9, -7, 7, "radiator")
		B.beam ((s * 6, 0, -7), (s * 6, 9, -7), 0.3, "hull_dark")
		B.beam ((s * 6, 0, 7), (s * 6, 9, 7), 0.3, "hull_dark")
	B.box (-6, 6, 0, 1.2, -1, 1, "hull_dark")
	return B

def m_reactor ():
	"""fission power plant: a core with a conical radiator, behind a berm (to the -z side)"""
	B = MeshBuilder ()
	B.cylinder (0, 0, 1.6, 0, 3.0, "hull_dark", n=12)
	B.cylinder (0, 0, 1.2, 3.0, 4.0, "gold", n=12)
	for k in range (8):
		a = 2 * math.pi * k / 8
		ca, sa = math.cos (a), math.sin (a)
		pts = [(0.9 * sa, 4.0, 0.9 * ca), (5.5 * sa, 4.0, 5.5 * ca), (1.2 * sa, 13.0, 1.2 * ca), (0.4 * sa, 13.0, 0.4 * ca)]
		B.poly (pts, (ca, 0, -sa), "radiator", uv=[(0, 0)] * 4)
		B.poly (pts, (-ca, 0, sa), "radiator", uv=[(0, 0)] * 4)
	return B

def m_comms ():
	"""communications mast with a dish"""
	B = MeshBuilder ()
	bk.lattice (B, -1.2, 1.2, -1.2, 1.2, 0, 28, 7, "hull_dark", w=0.25)
	B.cylinder (0, 0, 0.3, 28, 34, "hull", n=6)
	B.cylinder (0, 2.2, 3.0, 24, 25.5, "hull", n=16, r1=0.3, top=False)
	B.box (-3, 3, 0, 3, 3, 9, "hull", top="hull_dark")
	return B

def m_isru (body):
	"""in-situ resource plant: processing hall, storage tanks, a hopper"""
	B = MeshBuilder ()
	B.box (-12, 12, 0, 8, -8, 8, "hull", top="hull_dark")
	for k in range (3):
		B.sphere (-8 + 8 * k, 6.5, 16, 4.0, "hull", nlat=6, nlng=14)
		B.cylinder (-8 + 8 * k, 16, 1.0, 0, 3.5, "hull_dark", n=8)
	B.cylinder (16, 0, 4, 6, 12, "hull_dark", n=12, r1=6)   # (hopper)
	for (lx, lz) in ((12.5, -3), (19.5, -3), (12.5, 3), (19.5, 3)): B.beam ((lx, 0, lz), (lx, 6, lz), 0.4, "hull_dark")
	B.beam ((16, 9, 0), (34, 0.5, 0), 1.0, "hull_dark")    # (conveyor)
	mound (B, 34, 0, 16, 14, 4, "soil_" + body.lower ())     # (regolith heap)
	return B

def m_garage ():
	"""rover garage: a half-cylinder hangar with a door"""
	B = MeshBuilder ()
	r, L, n = 8.0, 24.0, 10
	for k in range (n):
		a0, a1 = math.pi * k / n - math.pi / 2, math.pi * (k + 1) / n - math.pi / 2
		am = 0.5 * (a0 + a1)
		B.poly ([(r * math.sin (a0), r * math.cos (a0), -L / 2), (r * math.sin (a1), r * math.cos (a1), -L / 2),
			(r * math.sin (a1), r * math.cos (a1), L / 2), (r * math.sin (a0), r * math.cos (a0), L / 2)], (math.sin (am), math.cos (am), 0), "hull", uv=[(0, 0)] * 4)
	for s in (-1, 1):
		B.poly ([(r * math.sin (math.pi * k / n - math.pi / 2), r * math.cos (math.pi * k / n - math.pi / 2), s * L / 2) for k in range (n + 1)], (0, 0, s), "hull_dark" if s > 0 else "hull")
	B.poly ([(-5, 0, L / 2 + 0.05), (5, 0, L / 2 + 0.05), (5, 5.5, L / 2 + 0.05), (-5, 5.5, L / 2 + 0.05)], (0, 0, 1), "door", uv=[(0, 0)] * 4)
	return B

def m_padberm (body):
	"""blast berm about a landing pad: a ring wall (r 48 m) with an opening to +z"""
	B = MeshBuilder ()
	n, R, h = 24, 48.0, 3.5
	mat = "soil_" + body.lower ()
	for k in range (n):
		if k in (0, n - 1): continue # (the opening)
		a0, a1 = 2 * math.pi * (k - 0.5) / n, 2 * math.pi * (k + 0.5) / n
		P = lambda a, rr, y: (rr * math.sin (a), y, rr * math.cos (a))
		am = 2 * math.pi * k / n
		B.poly ([P (a0, R + 4, 0), P (a1, R + 4, 0), P (a1, R + 1, h), P (a0, R + 1, h)], (math.sin (am), 1, math.cos (am)), mat, uv=[(0, 0)] * 4)
		B.poly ([P (a0, R - 2, 0), P (a1, R - 2, 0), P (a1, R + 1, h), P (a0, R + 1, h)], (-math.sin (am), 1, -math.cos (am)), mat, uv=[(0, 0)] * 4)
	return B

def m_massdriver ():
	"""mass driver: a 1.6 km track on pylons along +z from a loading hall at its start"""
	B = MeshBuilder ()
	L, h = 1600.0, 5.0
	B.box (-14, 14, 0, 12, -40, -10, "hull", top="hull_dark")   # (loading hall)
	for k in range (41):
		z = -10 + (L) * k / 40
		B.beam ((-1.6, 0, z), (-1.0, h, z), 0.6, "hull_dark")
		B.beam ((1.6, 0, z), (1.0, h, z), 0.6, "hull_dark")
	for s in (-1, 1):
		B.box (s * 1.2 - 0.25, s * 1.2 + 0.25, h, h + 0.6, -10, L - 10, "rail", bottom=True)
	for k in range (0, 80, 2): # (drive coils)
		z = -10 + L * (k + 0.5) / 80
		B.box (-1.6, 1.6, h - 0.3, h + 1.6, z - 1.0, z + 1.0, "gold", bottom=True)
	return B

def m_solar_tower ():
	"""polar solar tower: a mast with a tall vertical array (the sun at the
	horizon), turning with it"""
	B = MeshBuilder ()
	bk.lattice (B, -0.8, 0.8, -0.8, 0.8, 0, 32, 8, "hull_dark", w=0.2)
	for s, mat in ((1, "solar"), (-1, "hull_dark")):
		B.poly ([(-5, 12, s * 1.2), (5, 12, s * 1.2), (5, 32, s * 1.2), (-5, 32, s * 1.2)], (0, 0, s), mat, uv=[(0, 0)] * 4)
	B.box (-5, 5, 21.8, 22.2, -1.2, 1.2, "hull_dark")
	B.box (-2, 2, 0, 1.2, -2, 2, "hull_dark")
	return B

def m_radioarray ():
	"""low-frequency radio telescope: a 200 m field of crossed dipoles, a receiver hut"""
	B = MeshBuilder ()
	for i in range (9):
		for k in range (9):
			x, z = -100 + 25 * i, -100 + 25 * k
			B.beam ((x, 0, z), (x, 1.2, z), 0.12, "hull_dark")
			B.box (x - 2.5, x + 2.5, 1.2, 1.32, z - 0.08, z + 0.08, "gold")
			B.box (x - 0.08, x + 0.08, 1.2, 1.32, z - 2.5, z + 2.5, "gold")
	B.box (-4, 4, 0, 3, 108, 114, "hull", top="hull_dark")
	return B

# the aerostat (base frame: origin on its deck, at the deck's centre)
AE_A, AE_C, AE_Y = 135.0, 38.0, -42.0 # envelope: radius, half height, centre [m]
AE_DECK = 75.0                         # deck radius [m]

def m_aerostat ():
	"""a floating habitat: a lens-shaped envelope of breathable air (a lifting
	gas in Venus's CO2), windows about its rim (the town inside), solar
	arrays on its shoulders, a landing deck on top, rotors for station
	keeping, a gondola below. All of it under the deck's surface (y < 0)"""
	B = MeshBuilder ()
	P = lambda a, b: (AE_A * math.cos (b) * math.sin (a), AE_Y + AE_C * math.sin (b), AE_A * math.cos (b) * math.cos (a))
	N = lambda a, b: (math.cos (b) * math.sin (a) / AE_A, math.sin (b) / AE_C, math.cos (b) * math.cos (a) / AE_A)
	nlat, nlng = 18, 64
	for i in range (nlat):
		b0, b1 = -math.pi / 2 + math.pi * i / nlat, -math.pi / 2 + math.pi * (i + 1) / nlat
		for k in range (nlng):
			a0, a1 = 2 * math.pi * k / nlng, 2 * math.pi * (k + 1) / nlng
			bm, am = 0.5 * (b0 + b1), 0.5 * (a0 + a1)
			pts = [P (a0, b0), P (a1, b0), P (a1, b1), P (a0, b1)]
			if i == 0: pts = [pts[0], pts[2], pts[3]]
			elif i == nlat - 1: pts = pts[:3]
			B.poly (pts, N (am, bm), "envelope", uv=[(0, 0)] * len (pts))
	# (overlays a little outside the envelope)
	Q = lambda a, b, s=1.004: tuple (AE_Y * (1 - s) * (j == 1) + s * c for j, c in enumerate (P (a, b)))
	for k in range (nlng):
		a0, a1 = 2 * math.pi * (k + 0.1) / nlng, 2 * math.pi * (k + 0.9) / nlng
		am = 2 * math.pi * (k + 0.5) / nlng
		# windows: two bands about the rim
		for (b0, b1) in ((-0.10, -0.03), (0.02, 0.09)):
			B.poly ([Q (a0, b0), Q (a1, b0), Q (a1, b1), Q (a0, b1)], N (am, 0.5 * (b0 + b1)), "window", uv=[(0, 0)] * 4)
		# solar arrays on the shoulders, outside the deck
		if k % 8 != 0:
			for (b0, b1) in ((0.42, 0.62), (0.66, 0.86)):
				B.poly ([Q (a0, b0), Q (a1, b0), Q (a1, b1), Q (a0, b1)], N (am, 0.5 * (b0 + b1)), "solar", uv=[(0, 0)] * 4)
	# the deck: a slab, its edge marked; on struts over the envelope
	B.cylinder (0, 0, AE_DECK, -2.0, -0.05, "hull_dark", n=48, cap="deck")
	for k in range (48):
		a0, a1 = 2 * math.pi * k / 48, 2 * math.pi * (k + 1) / 48
		if k % 2: continue
		B.poly ([(r * math.sin (a), -0.04, r * math.cos (a)) for (r, a) in ((AE_DECK - 2.5, a0), (AE_DECK - 0.3, a0), (AE_DECK - 0.3, a1), (AE_DECK - 2.5, a1))],
			(0, 1, 0), "deck_edge", uv=[(0, 0)] * 4)
	for k in range (24):
		a = 2 * math.pi * k / 24
		for rr in (35.0, 68.0):
			y = AE_Y + AE_C * math.sqrt (max (0.0, 1 - (rr / AE_A) ** 2))
			B.beam ((rr * math.sin (a), -2.0, rr * math.cos (a)), (rr * math.sin (a), y - 0.5, rr * math.cos (a)), 1.0, "hull_dark")
	# rotors on pylons about the rim (their thrust along the rim)
	for k in range (6):
		a = 2 * math.pi * (k + 0.5) / 6
		ca, sa = math.cos (a), math.sin (a)
		c = ((AE_A + 14) * sa, AE_Y, (AE_A + 14) * ca)
		B.beam ((AE_A * 0.97 * sa, AE_Y, AE_A * 0.97 * ca), c, 1.4, "hull_dark")
		t = (ca, 0, -sa) # (along the rim)
		B.beam ((c[0] - 6 * t[0], c[1], c[2] - 6 * t[2]), (c[0] + 6 * t[0], c[1], c[2] + 6 * t[2]), 3.0, "hull")
		hub = (c[0] + 6.5 * t[0], c[1], c[2] + 6.5 * t[2])
		for j in range (3):
			ph = 2 * math.pi * j / 3 + 0.3
			d = (math.cos (ph) * sa, math.sin (ph), math.cos (ph) * ca) # (in the plane across the rim)
			B.beam (hub, (hub[0] + 9 * d[0], hub[1] + 9 * d[1], hub[2] + 9 * d[2]), 0.8, "hull_dark")
	# the gondola under the envelope: two modules across, on cables
	yb = AE_Y - AE_C
	hab (B, 0, 0, L=30, r=4.0, y=yb - 26, mat="hull")
	hab (B, 0, 0, L=30, r=4.0, y=yb - 26, mat="hull", along_x=True)
	for (cx, cz) in ((-10, -10), (10, -10), (-10, 10), (10, 10)):
		B.beam ((cx * 0.4, yb - 18, cz * 0.4), (cx, yb + 0.6, cz), 0.3, "hull_dark")
	B.beam ((0, yb - 26, 0), (0, yb - 140, 0), 0.4, "hull_dark") # (the sonde line, down into the clouds)
	return B

def m_aerostat_terminal ():
	"""the deck's terminal: a control tower with a lit cab, a hangar for small craft, a mast"""
	B = MeshBuilder ()
	B.box (-14, 14, 0, 9, -9, 9, "hull", top="hull_dark")              # (hangar, its door to +z)
	B.poly ([(-11, 0, 9.05), (11, 0, 9.05), (11, 7.5, 9.05), (-11, 7.5, 9.05)], (0, 0, 1), "door", uv=[(0, 0)] * 4)
	B.cylinder (20, -2, 3.5, 0, 16, "hull", n=12)                      # (tower)
	B.cylinder (20, -2, 5.0, 16, 19.5, "window", n=12, top=False)
	B.cylinder (20, -2, 5.5, 19.5, 20.5, "hull_dark", n=12)
	bk.lattice (B, 18.8, 21.2, -3.2, -0.8, 20.5, 32, 3, "hull_dark", w=0.15)
	B.cylinder (20, -2, 1.6, 30, 30.6, "hull", n=10, r1=0.2, top=False)
	return B

def aerostat_layout ():
	"""the aerostat's objects and lights: (cfg lines of its object list, floodlights (east, y, north, de, dy, dn))"""
	pads, floods = [], []
	for k in range (3):
		b = 120.0 * k
		e, n = 38 * math.sin (math.radians (b)), 38 * math.cos (math.radians (b))
		pads.append ((e, n, b))
		floods.append ((e * 1.75, 12.0, n * 1.75, -e, -12.0 * 1.8, -n))
	# (pads first: their numbers)
	lines = []
	for k, (e, n, b) in enumerate (pads):
		x, z = BF (e, n)
		lines += ["LPAD2", "\tPOS %.1f 0 %.1f" % (x, z), "\tSCALE 1", "\tROT %.1f" % BROT (b), "\tTEX Lpad02", "\tNAV %.2f" % (130.10 + 0.2 * k), "END"]
	lines += ["MESH", "\tFILE BaseKit\\space_aerostat", "\tPOS 0 0 0", "\tOWNMATERIAL", "END"]
	tb = 60.0 # (the terminal between pads 1 and 2, at the deck's edge, its door to the centre)
	e, n = 55 * math.sin (math.radians (tb)), 55 * math.cos (math.radians (tb))
	x, z = BF (e, n)
	lines += ["MESH", "\tFILE BaseKit\\space_aerostat_terminal", "\tPOS %.1f 0 %.1f" % (x, z), "\tROT %.1f" % BROT (tb + 180), "\tOWNMATERIAL", "\tSHADOW", "END"]
	return lines, floods

def cmd_aerostat (root=".", vkdir=None):
	name, body, lng, lat, alt, vor, freq, cfgs = AEROSTAT
	objs, floods = aerostat_layout ()
	text = ["BASE-V2.0", "Name = %s" % name, "Location = %.2f %+.2f" % (lng, lat), "Size = 300",
		";; (made by Utils/BaseKit/offworld.py aerostat)",
		"; a floating base: its deck %g km up" % (alt / 1000), "Altitude = %.0f" % alt, "",
		"; decks: the ground for vessels over them (x z radius height [m], base frame: x south, z east)",
		"BEGIN_DECKS", "0 0 %.0f 0" % AE_DECK, "END_DECKS", "",
		"BEGIN_NAVBEACON", "VOR %s %.2f %+.2f %.2f 500" % (vor, lng, lat, freq), "END_NAVBEACON", "",
		"; === List of visuals ===", "BEGIN_OBJECTLIST"] + objs + ["END_OBJECTLIST", ""]
	for c in cfgs:
		path = os.path.join (root, c)
		os.makedirs (os.path.dirname (path), exist_ok=True)
		with open (path, "w", newline="\n") as f: f.write ("\n".join (text))
	if vkdir:
		L = Layout (name, None)
		L.floods = floods
		bk.update_vk (os.path.join (vkdir, name + ".cfg"), L, name)
	print ("%s: %d lines" % (name, len (text)))

def mesh_list ():
	m = {"space_dome": m_dome, "space_greenhouse": m_greenhouse, "space_solar": m_solar, "space_radiator": m_radiator,
		"space_reactor": m_reactor, "space_comms": m_comms, "space_garage": m_garage, "space_massdriver": m_massdriver,
		"space_solar_tower": m_solar_tower, "space_radioarray": m_radioarray, "space_aerostat": m_aerostat, "space_aerostat_terminal": m_aerostat_terminal}
	for b in BODIES:
		l = b.lower ()
		m["space_hab_" + l] = (lambda b=b: m_hab (b))
		m["space_isru_" + l] = (lambda b=b: m_isru (b))
		m["space_padberm_" + l] = (lambda b=b: m_padberm (b))
	return m

def cmd_meshes (meshdir):
	ml = mesh_list ()
	for name, fn in ml.items ():
		fn ().write (os.path.join (meshdir, "BaseKit", name + ".msh"))
	print ("%d meshes in %s" % (len (ml), os.path.join (meshdir, "BaseKit")))

# ======================================================================
# Sampler and layout

def cmd_sampler (out, root="."):
	lua = ['-- (made by Utils/BaseKit/offworld.py sampler)', 'local bases = {']
	for b, (body, size, cfgs, new) in BASES.items ():
		if new and new[3].get ("flat"): continue
		_, lng, lat = bk.read_cfg (os.path.join (root, cfgs[0]))
		lua.append ('\t{"%s", "%s", %.6f, %.6f},' % (b, body, lng, lat))
	lua += ['}'] + [l.replace ("function at", "function bk_at") for l in bk.LUA_AT] + ['for _, b in ipairs(bases) do', '\tlocal P = oapi.get_objhandle(b[2])', '\tlocal R = oapi.get_size(P)',
		'\tlocal lng0, lat0 = math.rad(b[3]), math.rad(b[4])',
		'\tfor j = -%d, %d do' % (bk.GRID_N, bk.GRID_N), '\t\tlocal t = {}', '\t\tfor i = -%d, %d do' % (bk.GRID_N, bk.GRID_N),
		'\t\t\tlocal x, z = i * %g, j * %g' % (bk.GRID_STEP, bk.GRID_STEP),
		'\t\t\tt[#t+1] = string.format("%.1f", oapi.surface_elevation(P, bk_at(lng0, lat0, R, x, z)))',
		'\t\tend', '\t\toapi.write_log("ELEVGRID " .. b[1] .. " " .. j .. " " .. table.concat(t, " "))', '\tend', 'end', 'oapi.exit(0)']
	with open (out, "w") as f: f.write ("\n".join (lua) + "\n")

def flat_site (T, occ, w, d, r_occ, rmin, rmax, bearings, step=100.0):
	"""as basekit.flat_site, without water (airless and icy bodies)"""
	best = None
	lim = bk.GRID_N * bk.GRID_STEP
	n = int (rmax / step)
	for i in range (-n, n + 1):
		for k in range (-n, n + 1):
			x, z = i * step, k * step
			rr = math.hypot (x, z)
			if rr < rmin or rr > rmax: continue
			for bb in bearings:
				sb, cb = abs (math.sin (math.radians (bb))), abs (math.cos (math.radians (bb)))
				if abs (x) + 0.5 * (w * cb + d * sb) > lim or abs (z) + 0.5 * (w * sb + d * cb) > lim: continue
				sa, ca = math.sin (math.radians (bb)), math.cos (math.radians (bb))
				m = max (1, int (d / 100))
				if not all (bk.clear_of (occ, x + sa * d * (q / m - 0.5), z + ca * d * (q / m - 0.5), r_occ + 0.5 * w) for q in range (m + 1)):
					continue
				e = T.footprint (x, z, bb, w, d, 25.0)
				score = max (e) - min (e) + 0.002 * rr
				if best is None or score < best[0]: best = (score, x, z, bb)
	return best

def plan (name, body, size, T, cfgtext, new=None):
	r = Rnd (name)
	p = BODIES[body]
	l = body.lower ()
	L = Layout (name, T)
	occ, has = bk.existing (cfgtext)
	occ = list (occ)
	taken = lambda: occ + [(o[1], o[3]) for o in L.objs] + [(k[0], k[1]) for k in L.keep]
	links = []
	_, _, lat = bk.read_cfg_text (cfgtext)
	opts = new[3] if new else {}
	# a new base: more pads by its first, in a row
	pads = [(x, z) for (k, x, z) in pads_of (cfgtext)]
	if new:
		for k in range (new[2] - 1):
			site = flat_site (T, taken (), 60, 60, 110, 120, 700, [0], step=50.0)
			if not site: break
			_, px, pz, _ = site
			L.pads.append ((px, pz, 0.0))
			L.keep.append ((px, pz, 60.0))
			pads.append ((px, pz))
			links.append ((px, pz))
	# blast berms about the landing pads
	for (x, z) in pads:
		L.place ("space_padberm_" + l, x, z, round (r (0, 360) / 30) * 30, 100, 100, shadow=False)
	L.floods += [(x + 30, 15, z, -0.8, -0.6, 0) for (x, z) in pads]

	n_hab = {"outpost": 2, "station": 4, "colony": 6}[size]
	# the habitat cluster: modules about a hub, joined by corridors
	site = flat_site (T, taken (), 160, 160, 60, 250, 1500, [0, 45, 90, 135])
	if site:
		_, hx, hz, hb = site
		s, c = math.sin (math.radians (hb)), math.cos (math.radians (hb))
		H = lambda lx, lz: (hx + lx * c + lz * s, hz - lx * s + lz * c)
		L.place ("space_hab_" + l, hx, hz, hb, 30, 30)
		hubs = [(hx, hz)]
		for k in range (n_hab - 1):
			a = 2 * math.pi * k / (n_hab - 1)
			px, pz = H (55 * math.sin (a), 55 * math.cos (a))
			L.place ("space_hab_" + l, px, pz, hb + math.degrees (a) + 180, 30, 30)
			add_tube (L, [(hx, hz), (px, pz)])
			hubs.append ((px, pz))
		gx, gz = H (0, -95)
		L.place ("space_garage", gx, gz, hb + 180, 20, 30)
		add_tube (L, [(hx, hz), (gx, gz)])
		L.keep.append ((hx, hz, 130.0))
		links.append ((hx, hz))
		L.floods += [(px, 12, pz, 0, -1, 0) for (px, pz) in hubs[:3]]

	# colony: domes and greenhouses about the habitats
	if size in ("station", "colony"):
		n_gh = 2 if size == "station" else 4
		site = flat_site (T, taken (), 200, 140, 60, 250, 1800, [0, 45, 90, 135])
		if site:
			_, gx, gz, gb = site
			s, c = math.sin (math.radians (gb)), math.cos (math.radians (gb))
			G = lambda lx, lz: (gx + lx * c + lz * s, gz - lx * s + lz * c)
			for k in range (n_gh):
				L.place ("space_greenhouse", *G (-60 + 120 * k / max (1, n_gh - 1), 0), gb, 14, 42)
			L.keep.append ((gx, gz, 120.0))
			links.append ((gx, gz))
	if size == "colony":
		for k in range (3):
			site = flat_site (T, taken (), 60, 60, 40, 200, 1500, [0])
			if not site: break
			_, dx, dz, _ = site
			L.place ("space_dome", dx, dz, round (r (0, 360) / 15) * 15, 50, 50)
			L.keep.append ((dx, dz, 45.0))
			links.append ((dx, dz))

	# power: solar fields, or a reactor; radiators; communications
	n_sol = {"outpost": 2, "station": 4, "colony": 6}[size] if p["solar"] else 0
	polar = abs (lat) > 70.0 # (the sun at the horizon: vertical arrays on towers, on the heights)
	for k in range (n_sol * (3 if polar else 1)):
		if polar:
			site = flat_site (T, taken (), 12, 12, 30, 200, 2000, [0])
			if not site: break
			_, sx, sz, _ = site
			L.place ("space_solar_tower", sx, sz, 0, 12, 12)
			L.keep.append ((sx, sz, 20.0))
			continue
		site = flat_site (T, taken (), 60, 30, 30, 300, 2000, [0, 90])
		if not site: break
		_, sx, sz, sb = site
		L.place ("space_solar", sx, sz, sb, 60, 30, shadow=False)
		L.keep.append ((sx, sz, 40.0))
	if opts.get ("radio"):
		site = flat_site (T, taken (), 230, 230, 60, 500, 3000, [0, 45])
		if site:
			_, ox, oz, ob = site
			L.place ("space_radioarray", ox, oz, ob, 210, 210, shadow=False)
			L.keep.append ((ox, oz, 160.0))
			links.append ((ox, oz))
	if not p["solar"] or size != "outpost":
		site = flat_site (T, taken (), 30, 30, 200, 900, 2500, [0])
		if site:
			_, rx, rz, _ = site
			L.place ("space_reactor", rx, rz, 0, 14, 14)
			L.keep.append ((rx, rz, 150.0))
			links.append ((rx, rz))
	for k in range ({"outpost": 1, "station": 2, "colony": 3}[size]):
		site = flat_site (T, taken (), 20, 20, 30, 150, 1200, [0, 90])
		if not site: break
		_, ax, az, ab = site
		L.place ("space_radiator", ax, az, ab, 16, 16)
		L.keep.append ((ax, az, 20.0))
	site = flat_site (T, taken (), 20, 20, 50, 200, 1500, [0])
	if site:
		_, cx, cz, _ = site
		L.place ("space_comms", cx, cz, round (r (0, 360) / 30) * 30, 12, 12)
		L.keep.append ((cx, cz, 20.0))
	# resources
	if size != "outpost":
		site = flat_site (T, taken (), 70, 40, 50, 400, 2500, [0, 45, 90, 135])
		if site:
			_, ix, iz, ib = site
			L.place ("space_isru_" + l, ix, iz, ib, 60, 40)
			L.keep.append ((ix, iz, 50.0))
			links.append ((ix, iz))
	if size == "colony":
		site = flat_site (T, taken (), 40, 1680, 40, 600, 3400, range (0, 180, 15), step=200.0)
		if site:
			_, mx, mz, mb = site
			sa, ca = math.sin (math.radians (mb)), math.cos (math.radians (mb))
			# (its start: the loading hall end, 790 m back)
			L.place ("space_massdriver", mx - sa * 790, mz - ca * 790, mb, 30, 30)
			L.keep.append ((mx, mz, 850.0))
			links.append ((mx - sa * 790, mz - ca * 790))
	# roads (graded tracks) from the base's origin to each part
	for (x, z) in links:
		n = max (1, int (math.hypot (x, z) / 100))
		L.strip ([(x * k / n, z * k / n) for k in range (n + 1)], 8.0, "track_" + l)
	return L

def pads_of (cfgtext):
	"""the base's own landing pads: (kind, east, north)"""
	t = cfgtext.replace ("\r\n", "\n")
	if bk.BEGIN in t: t = t[:t.index (bk.BEGIN)] + t[t.index (bk.END) + len (bk.END):]
	out = []
	for m in re.finditer (r"^(LPAD\w*)\s*$\s*POS\s+([-\d.]+)\s+[-\d.]+\s+([-\d.]+)", t, re.M):
		out.append ((m.group (1), float (m.group (3)), -float (m.group (2))))
	return out

def add_tube (L, pts):
	"""a pressurized corridor along pts (east, north)"""
	L.ground.append (("tube", pts))

def ground_and_tubes (L, path_ground, path_tubes, body):
	"""tracks (flat, on the ground) and corridors (tubes on low supports)"""
	G = MeshBuilder ()
	T = MeshBuilder ()
	for g in L.ground:
		if g[0] == "strip":
			_, pts, w, mat = g
			for (a, b) in zip (pts[:-1], pts[1:]):
				dx, dz = b[0] - a[0], b[1] - a[1]
				l = math.hypot (dx, dz)
				if l < 1e-3: continue
				n = max (1, int (math.ceil (l / 20.0)))
				px, pz = -dz / l * w / 2, dx / l * w / 2
				for k in range (n):
					p0 = (a[0] + dx * k / n, a[1] + dz * k / n); p1 = (a[0] + dx * (k + 1) / n, a[1] + dz * (k + 1) / n)
					q = [(p0[0] - px, p0[1] - pz), (p1[0] - px, p1[1] - pz), (p1[0] + px, p1[1] + pz), (p0[0] + px, p0[1] + pz)]
					G.poly ([(BF (*e)[0], 0.05, BF (*e)[1]) for e in q], (0, 1, 0), mat, uv=[(0, 0)] * 4)
		elif g[0] == "tube":
			pts = g[1]
			a, b = pts[0], pts[-1]
			dx, dz = b[0] - a[0], b[1] - a[1]
			l = math.hypot (dx, dz)
			if l < 30: continue
			ux, uz = dx / l, dz / l
			# (from the edge of one module's berm to the next's; in pieces of 10 m)
			s0, s1 = 14.0, l - 14.0
			n = max (1, int ((s1 - s0) / 10))
			for k in range (n):
				pa = (a[0] + ux * (s0 + (s1 - s0) * k / n), a[1] + uz * (s0 + (s1 - s0) * k / n))
				pb = (a[0] + ux * (s0 + (s1 - s0) * (k + 1) / n), a[1] + uz * (s0 + (s1 - s0) * (k + 1) / n))
				A, Bp = BF (*pa), BF (*pb)
				T.beam ((A[0], 2.2, A[1]), (Bp[0], 2.2, Bp[1]), 2.6, "hull")
				T.box (A[0] - 0.4, A[0] + 0.4, 0, 1.0, A[1] - 0.4, A[1] + 0.4, "hull_dark")
	G.write (path_ground)
	T.write (path_tubes)
	return bool (T.groups)

def cfg_block (L, has_tubes):
	out = bk.cfg_block (L)
	if has_tubes:
		out[-1:-1] = ["MESH", "\tFILE BaseKit\\%s_tubes" % L.name, "\tPOS 0 0 0", "\tWRAPTOSURFACE", "\tOWNMATERIAL", "\tSHADOW", "END"]
	return out

def cmd_skeleton (root="."):
	"""the files of the new bases (where they are not yet): header, beacon, pad 1"""
	for b, (body, size, cfgs, new) in BASES.items ():
		if not new: continue
		vor, freq, npad, opts = new
		lng, lat = opts["loc"]
		text = ["BASE-V2.0", "Name = %s" % b, "Location = %.4f %+.4f" % (lng, lat), "Size = 500",
			";; (made by Utils/BaseKit/offworld.py skeleton; its installations: the BaseKit block)", "",
			"BEGIN_NAVBEACON", "VOR %s %.4f %+.4f %.2f 500" % (vor, lng, lat, freq), "END_NAVBEACON", "",
			"; === List of visuals ===", "BEGIN_OBJECTLIST", "LPAD2", "\tPOS 0 0 0", "\tSCALE 1", "\tTEX Lpad02", "\tNAV %.2f" % (freq + 15.0), "END",
			"END_OBJECTLIST", ""]
		for c in cfgs:
			path = os.path.join (root, c)
			if os.path.exists (path): continue
			os.makedirs (os.path.dirname (path), exist_ok=True)
			with open (path, "w", newline="\n") as f: f.write ("\n".join (text))
			print ("wrote", path)

def cmd_layout (gridfile, root=".", meshdir=None, vkdir=None):
	grids = bk.read_grid (gridfile)
	for b, (body, size, cfgs, new) in BASES.items ():
		if new and new[3].get ("flat"): grids[b] = bk.Terrain ([[0.0] * (2 * bk.GRID_N + 1)] * (2 * bk.GRID_N + 1))
		if b not in grids: print ("%s: no terrain grid, skipped" % b); continue
		cfgtext = open (os.path.join (root, cfgs[0]), encoding="latin-1").read ()
		L = plan (b, body, size, grids[b], cfgtext, new)
		has_tubes = ground_and_tubes (L, os.path.join (meshdir, "BaseKit", b + "_ground.msh"), os.path.join (meshdir, "BaseKit", b + "_tubes.msh"), body) if meshdir else False
		block = cfg_block (L, has_tubes)
		for c in cfgs:
			path = os.path.join (root, c)
			t = open (path, encoding="latin-1", newline="").read ()
			nl = "\r\n" if "\r\n" in t else "\n"
			lines = t.replace ("\r\n", "\n").split ("\n")
			if bk.BEGIN in lines:
				i0, i1 = lines.index (bk.BEGIN), lines.index (bk.END)
				del lines[i0:i1 + 1]
			k = next (i for i, x in enumerate (lines) if x.strip ().upper () == "END_OBJECTLIST")
			lines[k:k] = block
			with open (path, "w", encoding="latin-1", newline="") as f: f.write (nl.join (lines))
		if vkdir:
			title = re.search (r"Name\s*=\s*(.+)", cfgtext).group (1).strip ()
			bk.update_vk (os.path.join (vkdir, bk.vk_name (title)), L, title)
		print ("%-10s %-8s %-7s %2d objects" % (b, body, size, len (L.objs)))

if __name__ == "__main__":
	a = sys.argv[1:]
	if not a: print (__doc__); sys.exit (1)
	opt = lambda k: a[a.index (k) + 1] if k in a else None
	if a[0] == "meshes": cmd_meshes (a[1])
	elif a[0] == "sampler": cmd_sampler (a[1])
	elif a[0] == "layout": cmd_layout (a[1], ".", opt ("--meshes"), opt ("--vk"))
	elif a[0] == "skeleton": cmd_skeleton ()
	elif a[0] == "aerostat": cmd_aerostat (".", opt ("--vk"))
	else: print (__doc__); sys.exit (1)
