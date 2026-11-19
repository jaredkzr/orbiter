#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""The Cape's unique surface sheets: textures painted for one surface each and
mapped onto it once - nothing tiles, nothing repeats (blender_kit.py cape maps
its faces to REGIONS of the sheets, uv 'decal').

  cape_pad (4096 x 4096, 12 px/m: 8.3 cm a texel)
    a launch pad of complex 39 seen from above - one painting in the pad's
    own frame (x south, z east [m]), cut into the sheet's regions:
      mound    the hardstand and the mound's four flanks (x -127..127)
      ramp     the crawlerway's ramp, its sides, the lanes past its foot
      lane0/1  the crawler's lanes where they lie on the hardstand
    The hardstand: pale concrete in slabs (their joints in line with the
    flame trench's opening), pours of slightly different tone, weathering,
    spills, repairs, the blast's scorch about the trench. The flanks and the
    ramp's sides: the pads' cream slope paving - strips of panels down the
    fall line, courses across, runoff streaks from the top, seepage under the
    courses, mildew towards the foot, panels replaced since. The lanes: tan
    river rock, crushed pale where the crawler's treads run.
    Also the shuttle's mobile launcher platform (20 px/m): its deck (plates,
    soot and the boosters' pale deposit about the exhaust holes) and its four
    sides (plate seams, stiffeners, hatches, rust from the deck's edge).

  cape_vab (4096 x 2048, 9 px/m: 11 cm a texel)
    the Vehicle Assembly Building's faces as repainted (2020): white towers
    and pilasters, light grey bays; south and north the grey middle with its
    white frame about the transfer aisle's translucent panel; east and west
    the high bays' doors (seven leaves up each, the four of the base), and
    the low bay. Rain streaks, the siding's bays and courses faintly.

  cape_pad_detail, cape_vab_detail
    their grain close by (the client's "_detail" map, VkTexture.h MAP_DETAIL:
    a tile laid over the sheet 8 texels to one of the sheet's; albedo x (0.5 +
    rgb)): concrete's grain and pits; the siding's ribs, 0.32 m apart.

What they are painted after (photographs looked at, none used as texture):
Wikimedia Commons "Kennedy LC-39B, June 2019", "KSC SLC-39B September 2017",
"A view of the Vehicle Assembly Building (VAB) ... on June 22, 2020", "VAB
Exterior Photos (KSC-20220119-PH-CSH01 0016)", "Vehicle Assembly Building
(KSC-98PC-0998)" - NASA, public domain.

    capesurf.py [--only pad|vab] [--preview DIR]

Writes Textures/BaseKit/cape_pad*.dds, cape_vab*.dds.
"""

import argparse, math, os, sys
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
sys.path.insert (0, HERE)

# ----------------------------------------------------------------------
# what is painted: the pads' and the building's measures (blender_kit.py's: it checks they agree)

PAD_H = 16.0
PAD_X, PAD_Z = (-95.0, 95.0), (-56.0, 64.0)
SLOPE = 2.0
TR_Z, TR_OPEN = (-5.1, 12.9), (-25.0, 22.0)
RAMP_X1, RAMP_W, RAMP_C = 400.0, 44.0, 3.9
LANES, LANE_W = (3.9 - 13.7, 3.9 + 13.7), 12.0
RAMP_CUT = 127.0   # (the mound's region ends, the ramp's begins: blender_kit.py cuts the ramp's faces there)
MLP_X, MLP_Z, MLP_Y = (-30.5, 19.5), (-17.12, 24.88), (22.7, 30.3)
MLP_HOLES = ((-6.85, 5.0, -5.28, 0.62), (-6.85, 5.0, 7.12, 13.03), (7.5, 17.4, -0.78, 8.53))
VAB_HB = (-240.6, -106.6, 20.4, 178.4, 160.0)
VAB_LB = (-106.6, -22.6, 31.9, 166.9, 64.0)
VAB_DOORS = (-204.1, -142.6)
LCC_O, LCC_U, LCC_LWH, LCC_LEAN = (-72.1, 201.0), (0.7432, 0.6691), (109.1, 40.9, 23.5), 2.6

def hardstand_joints ():
	"""the hardstand's slabs: their joints along x, along z (the trench's opening whole slabs)"""
	sp = lambda a, b, n: [a + (b - a) * k / n for k in range (n + 1)]
	xs = sp (PAD_X[0], TR_OPEN[0], 9) + sp (TR_OPEN[0], TR_OPEN[1], 6)[1:] + sp (TR_OPEN[1], PAD_X[1], 9)[1:]
	zs = sp (PAD_Z[0], TR_Z[0], 6) + sp (TR_Z[0], TR_Z[1], 2)[1:] + sp (TR_Z[1], PAD_Z[1], 6)[1:]
	return xs, zs

# ----------------------------------------------------------------------
# the sheets' regions

class Region:
	"""a rectangle of a sheet [px] and what it shows: a from a0 (its left edge) to a1 (its
	right), b from b0 (its top) to b1 (its bottom) - metres, of the surface's own frame"""
	def __init__ (self, x, y, a0, a1, b0, b1, s):
		self.x, self.y, self.a0, self.a1, self.b0, self.b1 = x, y, a0, a1, b0, b1
		self.w, self.h = int (round (abs (a1 - a0) * s)), int (round (abs (b1 - b0) * s))

	def rect (self, size):
		"""(u0, v0, u1, v1) on a sheet of size (w, h) px"""
		return (self.x / size[0], self.y / size[1], (self.x + self.w) / size[0], (self.y + self.h) / size[1])

	def grid (self, g):
		"""a, b at the texels' centres, g texels of gutter about it (the edge's values)"""
		ca = np.clip (np.arange (-g, self.w + g) + 0.5, 0.5, self.w - 0.5) / self.w
		cb = np.clip (np.arange (-g, self.h + g) + 0.5, 0.5, self.h - 0.5) / self.h
		a = (self.a0 + ca * (self.a1 - self.a0)).astype (np.float32)
		b = (self.b0 + cb * (self.b1 - self.b0)).astype (np.float32)
		return np.broadcast_to (a[None, :], (len (b), len (a))), np.broadcast_to (b[:, None], (len (b), len (a)))

PAD_SIZE, PAD_S, MLP_S = (4096, 4096), 12.0, 20.0
VAB_SIZE, VAB_S = (4096, 2048), 9.0
_my0, _my1 = MLP_Y[1], MLP_Y[0]
_hx0, _hx1, _hz0, _hz1, _hh = VAB_HB
_lx0, _lx1, _lz0, _lz1, _lh = VAB_LB
_lu = LCC_O[0] * LCC_U[0] + LCC_O[1] * LCC_U[1]      # (the centre's corner along its length, across it)
_lv = -LCC_O[0] * LCC_U[1] + LCC_O[1] * LCC_U[0]
REGIONS = {
	'cape_pad': {
		# (plan: a = x, b = z)
		'mound': Region (0, 0, -127.5, 127.5, -88.5, 96.5, PAD_S),
		'ramp': Region (0, 2230, RAMP_CUT, 461.0, RAMP_C - 54.5, RAMP_C + 54.5, PAD_S),
		'lane0': Region (0, 3548, 29.5, 127.5, LANES[0] - 6.25, LANES[0] + 6.25, PAD_S),
		'lane1': Region (1186, 3548, 29.5, 127.5, LANES[1] - 6.25, LANES[1] + 6.25, PAD_S),
		# (the platform: its deck in plan, a = z, b = x; its sides, a along, b down from the deck)
		'mlp_deck': Region (3080, 0, MLP_Z[0], MLP_Z[1], MLP_X[0], MLP_X[1], MLP_S),
		'mlp_n': Region (3080, 1010, MLP_Z[0], MLP_Z[1], _my0, _my1, MLP_S),
		'mlp_s': Region (3080, 1172, MLP_Z[0], MLP_Z[1], _my0, _my1, MLP_S),
		'mlp_w': Region (3080, 1334, MLP_X[0], MLP_X[1], _my0, _my1, MLP_S),
		'mlp_e': Region (3080, 1496, MLP_X[0], MLP_X[1], _my0, _my1, MLP_S),
	},
	'cape_vab': {
		# (a along the face: z (south, north), x (east and west: the one painting); b the height, down from the top)
		's': Region (0, 0, _hz0, _hz1, _hh, 0.0, VAB_S),
		'n': Region (1432, 0, _hz0, _hz1, _hh, 0.0, VAB_S),
		'e': Region (2864, 0, _hx0, _hx1, _hh, 0.0, VAB_S),
		'ls': Region (0, 1450, _lz0, _lz1, _lh, 0.0, VAB_S),
		'le': Region (1225, 1450, _lx0, _lx1, _lh, 0.0, VAB_S),
		# (the Launch Control Center: a along its length (its south-west face, its north-east), across it (its ends: the one painting))
		'lcc_sw': Region (1991, 1450, _lu, _lu + LCC_LWH[0], LCC_LWH[2], 0.0, VAB_S),
		'lcc_ne': Region (1991, 1672, _lu, _lu + LCC_LWH[0], LCC_LWH[2], 0.0, VAB_S),
		'lcc_end': Region (2983, 1450, _lv, _lv + LCC_LWH[1] + LCC_LEAN, LCC_LWH[2], 0.0, VAB_S),
	},
}
SIZES = {'cape_pad': PAD_SIZE, 'cape_vab': VAB_SIZE}

def region (sheet, name):
	"""a region and its rectangle (u0, v0, u1, v1) on its sheet"""
	r = REGIONS[sheet][name]
	return r, r.rect (SIZES[sheet])

# ----------------------------------------------------------------------
# noise as a function of the place (the regions of a sheet agree where they meet)

def _hash (ix, iy, seed):
	h = (ix.astype (np.int64) * 374761393 + iy.astype (np.int64) * 668265263 + seed * 362437) & 0xffffffff
	h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
	h = h ^ (h >> 16)
	return (h & 0xffffff).astype (np.float32) / 16777216.0

def cellhash (i, j, seed):
	"""a value 0..1 per cell (i, j: arrays of whole numbers)"""
	return _hash (np.floor (i), np.floor (j), seed)

def vnoise (a, b, seed):
	"""value noise -1..1 on a lattice of unit cells"""
	ia, ib = np.floor (a), np.floor (b)
	fa, fb = (a - ia).astype (np.float32), (b - ib).astype (np.float32)
	fa = fa * fa * (3 - 2 * fa); fb = fb * fb * (3 - 2 * fb)
	h00, h10 = _hash (ia, ib, seed), _hash (ia + 1, ib, seed)
	h01, h11 = _hash (ia, ib + 1, seed), _hash (ia + 1, ib + 1, seed)
	v = (h00 + (h10 - h00) * fa) * (1 - fb) + (h01 + (h11 - h01) * fa) * fb
	return v * 2 - 1

def fbm (a, b, scale, seed, octaves=4, sb=None):
	"""octaves of it, the largest of the scale [m] (sb: its scale along b, else the same); about -1..1"""
	sb = sb or scale
	out, amp, tot = 0.0, 1.0, 0.0
	for k in range (octaves):
		f = 2.03 ** k
		out = out + amp * vnoise (a * (f / scale) + 17.3 * k, b * (f / sb) - 9.1 * k, seed + 31 * k)
		tot += amp; amp *= 0.5
	return out / tot * 1.6

def sstep (lo, hi, v):
	t = np.clip ((v - lo) / (hi - lo), 0, 1)
	return t * t * (3 - 2 * t)

def near (v, lines):
	"""the distance from v to the nearest of the sorted lines"""
	ln = np.asarray (lines, np.float32)
	k = np.clip (np.searchsorted (ln, v), 1, len (ln) - 1)
	return np.minimum (np.abs (v - ln[k - 1]), np.abs (ln[k] - v))

def line (dist, width, px):
	"""coverage of a line of the width [m] at the distance from its centre, texels of px [m]"""
	return np.clip ((width / 2 - dist) / px + 0.5, 0, 1)

def col (c): return np.array (c, np.float32)[None, None, :]

def mix (c0, c1, t): return c0 * (1 - t[..., None]) + c1 * t[..., None]

# ----------------------------------------------------------------------
# the pad

HARD, PAVE, PAVE_NEW = (0.800, 0.785, 0.742), (0.825, 0.775, 0.660), (0.845, 0.815, 0.725)
ROCK, CRUSHED = (0.700, 0.628, 0.497), (0.835, 0.785, 0.672)

def slope_paint (t, dp, htop, face, px, rng):
	"""slope paving: t along the slope's top [m], dp down it (in plan [m]), htop the top's height
	[m]; face: which slope (its own streaks). Luminance factor, how much mildew (0..1), how new"""
	P, C = 6.1, SLOPE * PAD_H / 3              # (the strips; the courses down the slope)
	it, jc = np.floor (t / P), np.floor (dp / C)
	h1, h2 = cellhash (it + 40 * face, jc, 71), cellhash (it + 40 * face, jc, 72)
	new = (h2 < 0.035).astype (np.float32)
	tone = 0.085 * (h1 - 0.5) - 0.07 * ((h2 > 0.90) & (h2 < 0.96)) + 0.012 * fbm (t, dp, 2.0, 73 + face, 3)
	f = np.clip (dp / np.maximum (SLOPE * htop, 0.5), 0, 1)       # (0 at the top, 1 at the foot)
	g = dp / C - jc                                               # (down a course)
	run = np.clip (vnoise (t / 0.55 + 3.1 * face, dp / 26.0, 74) * 1.3 + 0.15, 0, 1) * np.exp (-dp / 9.0)
	run = run * np.clip (vnoise (t / 7.0, 0.3 + face, 75) + 0.75, 0, 1)
	seep = np.clip (vnoise (t / 0.9 - 5.7 * face, jc * 3.3, 76) * 1.4 + 0.1, 0, 1) * np.exp (-g * 3.5) * (jc > 0)
	mil = sstep (0.45, 1.05, f + 0.22 * fbm (t, dp, 9.0, 77 + face, 4)) * (0.7 + 0.3 * np.clip (fbm (t, dp, 2.5, 78, 3) + 0.5, 0, 1))
	mil = mil * (1 - 0.8 * new)
	jd = np.minimum (np.abs ((t / P + 0.5) % 1.0 - 0.5) * P, np.abs ((dp / C + 0.5) % 1.0 - 0.5) * C)
	jn = line (jd, 0.05, px)
	lum = (1 + tone) * (1 - 0.20 * run * (1 - 0.6 * new)) * (1 - 0.14 * seep * (1 - 0.6 * new)) * (1 - 0.15 * mil)
	lum = lum * (1 - 0.06 * np.exp (-jd / 0.2)) * (1 - 0.42 * jn)
	return lum, mil, new

def lane_paint (X, c, k, px, rng):
	"""a crawlerway lane: c across it from its centre [m] (k: which lane)"""
	wob = 0.30 * vnoise (X / 37.0 + 11.0 * k, 0.5, 81) + 0.10 * vnoise (X / 9.0, 3.5 + k, 82)
	band = np.zeros (X.shape, np.float32)
	for uc in (-2.7, 2.7):
		band = np.maximum (band, np.clip ((1.45 - np.abs (c - uc - wob)) / 0.55, 0, 1))
	band = band * band * (3 - 2 * band)
	strength = np.clip (0.62 + 0.38 * fbm (X, c * 0 + 7.0 * k, 22.0, 83, 3), 0.15, 1.0)
	streak = vnoise (X / 45.0, c / 0.33 + 20 * k, 84) + 0.6 * vnoise (X / 14.0, c / 0.16 + 20 * k, 85)
	blot = fbm (X, c + 60.0 * k, 7.0, 86, 4)
	rib = np.sin (2 * math.pi * X / 0.52) * band
	edge = np.clip (1 - (LANE_W / 2 - np.abs (c)) / 1.1, 0, 1)
	grain = rng.standard_normal (X.shape).astype (np.float32)
	rgb = mix (col (ROCK), col (CRUSHED), band * strength)
	lum = (1 + 0.035 * streak + 0.055 * blot + 0.018 * rib + 0.055 * grain * (1 - 0.5 * band)) * (1 - 0.16 * edge * (0.6 + 0.4 * np.clip (blot + 0.5, 0, 1)))
	return rgb * lum[..., None], 0.20 - 0.08 * band

def pad_paint (X, Z, lanes, px, rng):
	"""the pad from above at x, z [m]: colour (sRGB), smoothness; lanes: the crawler's lanes on the ramp"""
	(x0, x1), (z0, z1), H = PAD_X, PAD_Z, PAD_H
	dx = np.maximum (np.maximum (x0 - X, X - x1), 0); dz = np.maximum (np.maximum (z0 - Z, Z - z1), 0)
	d = np.maximum (dx, dz)
	hf = H - d / SLOPE                                             # (the mound: the hardstand where d = 0)
	ry = np.where (X >= x1, np.clip (H * (RAMP_X1 - X) / (RAMP_X1 - x1), 0, H), -1e3)
	cz = np.abs (Z - RAMP_C) - RAMP_W / 2
	hr = np.where (cz <= 0, ry, ry - cz / SLOPE)
	onr = hr >= hf
	hard = (d <= 0) & ~onr
	rtop = onr & (cz <= 0)
	rside = onr & (cz > 0)
	mott = 0.030 * fbm (X, Z, 22.0, 11, 4) + 0.022 * fbm (X, Z, 4.0, 12, 4) + 0.02 * fbm (X, Z, 0.7, 13, 2)
	grain = rng.standard_normal (X.shape).astype (np.float32)

	# the hardstand
	xs, zs = hardstand_joints ()
	si, sj = np.searchsorted (np.asarray (xs, np.float32), X), np.searchsorted (np.asarray (zs, np.float32), Z)
	jd = np.minimum (near (X, xs), near (Z, zs))
	sh = cellhash (si, sj, 21)
	tone = 0.050 * (sh - 0.5) + 0.030 * vnoise (si / 2.6 + 0.3, sj / 1.7 + 0.6, 22)        # (slab by slab; stretches poured together)
	weath = np.clip (fbm (X, Z, 17.0, 23, 5) * 1.25 + 0.05, 0, 1)                          # (weathered dark over wide patches)
	weath = weath * weath * (0.75 + 0.5 * cellhash (si, sj, 24))
	spill = np.clip (fbm (X, Z, 2.6, 25, 4) - 0.50, 0, 1) * np.clip (fbm (X, Z, 30.0, 26, 2) + 0.4, 0, 1)
	drip = np.clip (fbm (X, Z, 0.5, 27, 2) - 0.80, 0, 1)
	track = np.clip (vnoise (X / 60.0, Z / 0.7, 28) - 0.25, 0, 1) * np.clip (vnoise (X / 45.0, Z / 14.0, 29) + 0.2, 0, 1)   # (vehicles' ways along the pad)
	# (the blast: soot about the trench's opening, rays of it outward)
	ex, ez = np.maximum (np.maximum (TR_OPEN[0] - X, X - TR_OPEN[1]), 0), np.maximum (np.maximum (TR_Z[0] - Z, Z - TR_Z[1]), 0)
	dt = np.hypot (ex, ez)
	ang = np.arctan2 (Z - (TR_Z[0] + TR_Z[1]) / 2, (X - (TR_OPEN[0] + TR_OPEN[1]) / 2) * 0.45)
	rays = 0.6 * vnoise (ang * 9.0, dt / 40.0, 30) + 0.4 * vnoise (ang * 31.0, dt / 25.0, 31)
	soot = np.exp (-dt / (6.5 + 4.5 * rays)) * (0.8 + 0.25 * fbm (X, Z, 3.0, 32, 3))
	soot = np.clip (soot, 0, 1)
	# (repairs: newer concrete, sawn in)
	patch = np.zeros (X.shape, np.float32); pedge = np.zeros (X.shape, np.float32)
	for (pa, pb, pw, pd) in ((-71.0, -38.0, 5.2, 3.4), (-52.0, 41.0, 3.1, 6.0), (41.0, -44.5, 7.5, 3.0), (63.5, 33.0, 4.0, 4.4),
			(-12.0, 37.0, 2.6, 5.2), (33.0, -24.0, 3.3, 2.4), (78.0, -8.0, 2.4, 6.6), (-84.0, 22.5, 5.0, 2.2), (9.0, -38.0, 6.2, 2.7)):
		q = np.minimum (np.minimum (X - pa, pa + pw - X), np.minimum (Z - pb, pb + pd - Z))
		patch = np.maximum (patch, np.clip (q / px + 0.5, 0, 1)); pedge = np.maximum (pedge, line (np.abs (q), 0.03, px))
	lum = (1 + tone + mott + 0.012 * grain) * (1 - 0.15 * weath) * (1 - 0.20 * spill - 0.15 * drip) * (1 - 0.07 * track)
	lum = lum * (1 + 0.075 * patch * (1 - 0.6 * soot)) * (1 - 0.25 * pedge) * (1 - 0.42 * soot)
	lum = lum * (1 - 0.08 * np.exp (-jd / 0.22) * (0.4 + 0.9 * cellhash (si, sj, 33))) * (1 - 0.50 * line (jd, 0.045, px))
	warm = np.clip (0.5 * weath + 0.8 * spill + 0.35 * soot, 0, 1)
	rgb_h = mix (col (HARD), col ((0.76, 0.70, 0.60)), warm) * lum[..., None]

	# its flanks, the ramp's sides
	fx = dx >= dz
	face = np.where (fx, np.where (X < 0, 0, 1), np.where (Z < 0, 2, 3))
	t = np.where (fx, Z, X)
	l1, m1, n1 = slope_paint (t, d, np.full (X.shape, H, np.float32), face, px, rng)
	# (the trench's exit north: scorched either side of it)
	l1 = l1 * (1 - 0.30 * (face == 0) * np.exp (-np.maximum (np.abs (Z - RAMP_C) - 9.0, 0) / 7.0) * (0.7 + 0.3 * fbm (X, Z, 3.0, 34, 2)))
	l2, m2, n2 = slope_paint (X, np.maximum (cz, 0), np.maximum (ry, 0.01), np.where (Z < RAMP_C, 4, 5), px, rng)
	ls, ms, ns = np.where (rside, l2, l1), np.where (rside, m2, m1), np.where (rside, n2, n1)
	ls = ls * (1 + mott + 0.012 * grain)
	rgb_s = mix (mix (col (PAVE), col (PAVE_NEW), ns), col ((0.67, 0.64, 0.56)), 0.45 * ms) * ls[..., None]

	# the ramp's top: paving between and beside the lanes, its joints across; the lanes
	jr = np.minimum (np.abs ((X / 6.1 + 0.5) % 1.0 - 0.5) * 6.1, np.minimum (np.abs (np.abs (Z - RAMP_C) - (13.7 - 6.0)), np.abs (np.abs (Z - RAMP_C) - (13.7 + 6.0))))
	rt = 0.07 * (cellhash (np.floor (X / 6.1), np.floor ((Z - RAMP_C) / 7.7), 41) - 0.5)
	tyre = np.clip (vnoise (X / 50.0, Z / 0.45, 42) - 0.1, 0, 1)
	wash = np.clip (fbm (X, Z, 6.0, 43, 4) + 0.1, 0, 1)                                    # (the lanes' fines washed onto it)
	lr = (1 + rt + mott + 0.012 * grain) * (1 - 0.09 * tyre) * (1 - 0.07 * np.exp (-jr / 0.2)) * (1 - 0.42 * line (jr, 0.05, px))
	rgb_r = mix (col ((0.790, 0.752, 0.662)), col ((0.74, 0.68, 0.56)), 0.55 * wash) * lr[..., None]
	smooth = np.full (X.shape, 0.12, np.float32)
	rgb = np.where (hard[..., None], rgb_h, np.where (rtop[..., None], rgb_r, rgb_s))
	if lanes:
		for k, zc in enumerate (LANES):
			on = rtop & (np.abs (Z - zc) <= LANE_W / 2)
			if not on.any (): continue
			lrgb, lsm = lane_paint (X, Z - zc, k, px, rng)
			rgb = np.where (on[..., None], lrgb, rgb); smooth = np.where (on, lsm, smooth)
	return rgb.astype (np.float32), smooth

# ----------------------------------------------------------------------
# the mobile launcher platform

MLP_GREY = (0.47, 0.48, 0.485)

def mlp_deck_paint (Zc, Xc, px, rng):
	"""its deck from above (a = z, b = x [m])"""
	X, Z = Xc, Zc
	i, j = np.floor ((X - MLP_X[0]) / 3.05), np.floor ((Z - MLP_Z[0]) / 2.44)
	sd = np.minimum (np.abs (((X - MLP_X[0]) / 3.05 + 0.5) % 1.0 - 0.5) * 3.05, np.abs (((Z - MLP_Z[0]) / 2.44 + 0.5) % 1.0 - 0.5) * 2.44)
	tone = 0.07 * (cellhash (i, j, 51) - 0.5) + 0.04 * fbm (X, Z, 6.0, 52, 4) + 0.03 * fbm (X, Z, 0.8, 53, 3)
	soot = np.zeros (X.shape, np.float32); pale = np.zeros (X.shape, np.float32)
	for n, (a, b, c, d) in enumerate (MLP_HOLES):
		ex, ez = np.maximum (np.maximum (a - X, X - b), 0), np.maximum (np.maximum (c - Z, Z - d), 0)
		dh = np.hypot (ex, ez)
		ang = np.arctan2 (Z - (c + d) / 2, X - (a + b) / 2)
		ray = 0.6 * vnoise (ang * 7.0 + 5 * n, dh / 9.0, 54) + 0.4 * vnoise (ang * 23.0 + 5 * n, dh / 5.0, 55)
		soot = np.maximum (soot, np.exp (-dh / (2.6 + 1.6 * ray)))
		if n < 2: pale = np.maximum (pale, np.clip (ray - 0.1, 0, 1) * np.exp (-np.abs (dh - 2.5) / 2.5))   # (the boosters' alumina)
	rust = np.clip (fbm (X, Z, 1.6, 56, 4) - 0.45, 0, 1) * 1.6
	wear = np.clip (fbm (X, Z, 4.5, 57, 4) + 0.15, 0, 1)                                    # (worn, walked on: lighter)
	lum = (1 + tone + 0.02 * rng.standard_normal (X.shape).astype (np.float32)) * (1 - 0.48 * soot) * (1 + 0.10 * wear) * (1 - 0.10 * np.exp (-sd / 0.12)) * (1 - 0.35 * line (sd, 0.03, px))
	rgb = mix (col (MLP_GREY), col ((0.47, 0.34, 0.25)), np.clip (0.55 * rust, 0, 0.7)) * lum[..., None]
	rgb = mix (rgb, col ((0.74, 0.72, 0.68)), 0.42 * pale)
	return rgb.astype (np.float32), np.full (X.shape, 0.30, np.float32) - 0.12 * soot

def mlp_side_paint (A, Y, side, px, rng):
	"""a side: A along it [m], Y its height [m] (MLP_Y: the bottom, the deck)"""
	y0, y1 = MLP_Y
	a = A + 100.0 * side
	sv = np.abs ((a / 3.05 + 0.5) % 1.0 - 0.5) * 3.05               # (stiffeners)
	sh = near (Y, (y0 + 0.9, y0 + 3.3, y0 + 5.7))                   # (plate seams)
	i, j = np.floor (a / 3.05), np.searchsorted (np.asarray ((y0 + 0.9, y0 + 3.3, y0 + 5.7), np.float32), Y)
	tone = 0.06 * (cellhash (i, j, 61) - 0.5) + 0.035 * fbm (a, Y, 5.0, 62, 4)
	rust = np.clip (vnoise (a / 0.22, Y / 9.0, 63) * 1.5 - 0.35, 0, 1) * np.clip (vnoise (a / 2.3, 0.5, 64) + 0.45, 0, 1)
	rust = rust * np.exp (-(y1 - Y) / 3.2) + 0.5 * np.clip (vnoise (a / 0.3, j * 2.7, 65) * 1.5 - 0.5, 0, 1) * np.exp (-np.abs (sh) / 0.9) * (Y < y0 + 5.7)
	# (hatches and ports: dark, a row of them, not every bay)
	hc = (i + 0.5) * 3.05
	hw = 0.55 + 0.5 * cellhash (i, i * 0, 66)
	has = cellhash (i, i * 0, 67) < 0.3
	hy = y0 + 3.9 + 1.2 * cellhash (i, i * 0, 68)
	hq = np.minimum (hw - np.abs (a - hc), 0.55 - np.abs (Y - hy))
	hatch = np.clip (hq / px + 0.5, 0, 1) * has
	girder = (Y < y0 + 0.9)
	lum = (1 + tone + 0.02 * rng.standard_normal (A.shape).astype (np.float32)) * (1 - 0.16 * np.exp (-sv / 0.10)) * (1 + 0.10 * np.exp (-np.abs (sv - 0.22) / 0.08))
	lum = lum * (1 - 0.30 * line (sh, 0.04, px)) * (1 - 0.12 * girder) * (1 - 0.10 * np.exp (-(Y - y0) / 0.6))
	rgb = mix (col ((0.52, 0.53, 0.535)), col ((0.50, 0.35, 0.24)), np.clip (0.75 * rust, 0, 0.8)) * lum[..., None]
	rgb = mix (rgb, col ((0.10, 0.10, 0.11)), 0.85 * hatch)
	return rgb.astype (np.float32), np.full (A.shape, 0.32, np.float32)

# ----------------------------------------------------------------------
# the Vehicle Assembly Building

VAB_WHITE, VAB_GREY, VAB_PANEL, VAB_LEAF = (0.885, 0.888, 0.882), (0.615, 0.628, 0.640), (0.600, 0.640, 0.585), (0.505, 0.515, 0.530)
VAB_BAY, VAB_COURSE = 11.58, 12.2    # (the frame's bays, 38 ft; the siding's courses)
LEAVES, LEAF_Y0, LEAF_Y1 = 7, 35.0, 139.0

def vab_weather (A, Y, top, seed, px):
	"""what the weather and the siding's make do to a face's paint: a luminance factor, and how dirty (0..1)"""
	st = np.clip (vnoise (A / 0.42, Y / 60.0, seed) * 1.25 - 0.20, 0, 1) * np.clip (vnoise (A / 4.0, Y / 110.0, seed + 1) + 0.55, 0, 1)
	st2 = np.clip (vnoise (A / 1.3, Y / 85.0, seed + 2) * 1.2 - 0.25, 0, 1)
	down = 0.45 + 0.55 * np.clip ((top - Y) / 50.0, 0, 1)                                   # (growing down from the roof's edge)
	dirt = np.clip (0.75 * st * down + 0.45 * st2 * down, 0, 1)
	bay, crs = np.floor (A / VAB_BAY), np.floor (Y / VAB_COURSE)
	tone = 0.016 * (cellhash (bay, crs, seed + 3) - 0.5) + 0.012 * fbm (A, Y, 28.0, seed + 4, 3)
	jd = np.minimum (np.abs ((A / VAB_BAY + 0.5) % 1.0 - 0.5) * VAB_BAY, np.abs ((Y / VAB_COURSE + 0.5) % 1.0 - 0.5) * VAB_COURSE)
	foot = np.exp (-Y / 2.2) * (0.6 + 0.4 * np.clip (vnoise (A / 1.7, 0.5, seed + 5) + 0.5, 0, 1))
	lum = (1 + tone) * (1 - 0.050 * line (jd, 0.10, px)) * (1 - 0.10 * foot) * (1 - 0.16 * line (top - 0.35 - Y, 0.7, px))
	return lum, dirt

def _frame (A, Y, ac, y1, half_grey, half_white, top_white, half_panel, top_panel, y0_panel):
	"""the grey middle of a face about ac, its white frame (an inverted U), the panel in it: masks"""
	c = np.abs (A - ac)
	grey = c < half_grey
	white = (c < half_white) & (Y < top_white)
	panel = (c < half_panel) & (Y < top_panel) & (Y >= y0_panel)
	return grey & ~white, panel

def vab_paint (face, A, Y, px, rng):
	"""a face of the building: A along it (z: s, n, ls; x: e, le [m]), Y the height [m]"""
	x0, x1, z0, z1, H = VAB_HB
	lx0, lx1, lz0, lz1, LH = VAB_LB
	if face.startswith ('lcc'): return lcc_paint (face, A, Y, px, rng)
	top = LH if face in ('ls', 'le') else H
	lum, dirt = vab_weather (A, Y, top, {'s': 100, 'n': 120, 'e': 140, 'ls': 160, 'le': 180}[face], px)
	rgb = np.broadcast_to (col (VAB_WHITE), A.shape + (3,)).copy ()
	shade = np.ones (A.shape, np.float32)
	if face in ('s', 'n', 'ls'):
		if face == 'ls': ac, g = (lz0 + lz1) / 2, _frame (A, Y, (lz0 + lz1) / 2, LH, 26.0, 20.0, 54.0, 15.0, 46.0, 24.0)
		else: ac, g = (z0 + z1) / 2, _frame (A, Y, (z0 + z1) / 2, H, 31.0, 21.0, 136.0, 12.5, 124.0, VAB_LB[4] if face == 's' else 62.0)
		grey, panel = g
		rgb[grey] = VAB_GREY
		# (the panel: translucent sheets in a grid of mullions)
		mu = np.minimum (np.abs (((A - ac) / 6.25 + 0.5) % 1.0 - 0.5) * 6.25, np.abs ((Y / 5.9 + 0.5) % 1.0 - 0.5) * 5.9)
		pt = 1 + 0.05 * (cellhash (np.floor ((A - ac) / 6.25 + 0.5), np.floor (Y / 5.9 + 0.5), 201) - 0.5) + 0.03 * fbm (A, Y, 9.0, 202, 3)
		rgb = np.where (panel[..., None], col (VAB_PANEL) * (pt * (1 - 0.16 * line (mu, 0.22, px)))[..., None], rgb)
		# (the door under it - the transfer aisle's north, the low bay's south: leaves one above another)
		hd, yd, n = (12.5, 62.0, 5) if face == 'n' else (15.0, 24.0, 3)
		if face != 's':
			door = (np.abs (A - ac) < hd) & (Y < yd)
			k = np.floor (Y / (yd / n)); yl = Y - k * (yd / n)
			lf = (1 + 0.05 * (cellhash (k, k * 0, 203) - 0.5)) * (1 - 0.30 * np.exp (-yl / 0.35)) * (1 + 0.10 * sstep (yd / n - 1.0, yd / n - 0.5, yl))
			lf = lf * (1 - 0.08 * np.exp (-np.abs (((A - ac) / 2.9 + 0.5) % 1.0 - 0.5) * 2.9 / 0.12))
			rgb = np.where (door[..., None], col (VAB_LEAF) * lf[..., None], rgb)
			shade = np.where ((np.abs (np.abs (A - ac) - hd - 0.6) < 0.6) & (Y < yd + 1.2) | (np.abs (A - ac) < hd + 1.2) & (np.abs (Y - yd - 0.6) < 0.6), 0.80, shade)
	elif face == 'e':
		for xc in VAB_DOORS:
			c = np.abs (A - xc)
			bay = c < 23.0
			rgb[bay] = VAB_GREY
			ledge = bay & (Y >= LEAF_Y0) & (Y < LEAF_Y0 + 4.5) & (c >= 11.5)
			rgb[ledge] = VAB_WHITE
			shade = np.where (bay & (c >= 11.5) & (Y < LEAF_Y0) & (Y > LEAF_Y0 - 1.0), 0.8, shade)
			# (the upper door: seven leaves, each a plane back from the one under it - its sill light, shadow under it)
			up = (c < 11.5) & (Y >= LEAF_Y0) & (Y < LEAF_Y1)
			hl = (LEAF_Y1 - LEAF_Y0) / LEAVES
			k = np.floor ((Y - LEAF_Y0) / hl); yl = (Y - LEAF_Y0) - k * hl
			kk = k + 10 * (xc > -170)
			lf = (1 + 0.06 * (cellhash (kk, kk * 0, 211) - 0.5) + 0.02 * fbm (A, Y, 5.0, 212, 3)) * (1 - 0.34 * np.exp (-yl / 0.45))
			lf = lf * (1 - 0.07 * np.exp (-np.abs (((A - xc) / 2.875 + 0.5) % 1.0 - 0.5) * 2.875 / 0.12))
			sill = sstep (hl - 1.9, hl - 1.5, yl)
			leaf = mix (col (VAB_LEAF) * lf[..., None], col ((0.80, 0.805, 0.80)), sill)
			rgb = np.where (up[..., None], leaf, rgb)
			# (the base's door: four leaves side by side)
			lo = (c < 23.0) & (Y < LEAF_Y0)
			q = np.floor ((A - xc) / 11.5)
			ll = (1 + 0.05 * (cellhash (q + 5 * (xc > -170), q * 0, 213) - 0.5) + 0.02 * fbm (A, Y, 5.0, 214, 3))
			ll = ll * (1 - 0.30 * line (np.abs (((A - xc) / 11.5 + 0.5) % 1.0 - 0.5) * 11.5, 0.35, px)) * (1 - 0.07 * line (np.abs ((Y / 5.0 + 0.5) % 1.0 - 0.5) * 5.0, 0.2, px))
			rgb = np.where (lo[..., None], col ((0.555, 0.565, 0.58)) * ll[..., None], rgb)
			# (the upper door's guides either side, its head)
			guide = ((c >= 11.5) & (c < 13.3) & (Y >= LEAF_Y0 + 4.5) & (Y < LEAF_Y1 + 2.5)) | ((c < 13.3) & (Y >= LEAF_Y1) & (Y < LEAF_Y1 + 2.5))
			shade = np.where (guide, 0.74, shade)
	elif face == 'le':
		band = (Y > 2.0) & (Y < 37.0) & (A > lx0 + 3.5) & (A < lx1 - 3.5)
		rgb[band] = (0.74, 0.75, 0.76)
		shade = np.where (band & (np.abs (((A - lx0) / 10.5 + 0.5) % 1.0 - 0.5) * 10.5 < 0.5), 1.12, shade)   # (pilasters)
	grainy = 0.006 * rng.standard_normal (A.shape).astype (np.float32)
	rgb = rgb * (lum * shade * (1 + grainy))[..., None]
	# (dirt: the paint greyed, a little brown)
	rgb = rgb * (1 - 0.06 * dirt[..., None]) * (1 - dirt[..., None] * col ((0.0, 0.006, 0.022)))
	return rgb.astype (np.float32), np.full (A.shape, 0.42, np.float32) - 0.12 * dirt

def lcc_paint (face, A, Y, px, rng):
	"""a face of the Launch Control Center: white precast panels, storey by storey, streaked under its window bands"""
	seed = {'lcc_sw': 220, 'lcc_ne': 240, 'lcc_end': 260}[face]
	Hh = LCC_LWH[2]
	a = A - (_lv if face == 'lcc_end' else _lu)
	P = 3.64
	jd = np.abs ((a / P + 0.5) % 1.0 - 0.5) * P
	st = np.floor (Y / 5.6)
	tone = 0.03 * (cellhash (np.floor (a / P), st, seed) - 0.5) + 0.012 * fbm (a, Y, 12.0, seed + 1, 3)
	below = np.minimum (((Y - 2.2) % 5.6), 5.6)                                    # (above a band's sill: 0 at it)
	under = 5.6 - below                                                            # (under the next band up)
	run = np.clip (vnoise (a / 0.35, st * 3.7, seed + 2) * 1.3 - 0.1, 0, 1) * np.exp (-(under - 3.8).clip (0, None) / 1.2) * (under > 3.8)
	roof = np.clip (vnoise (a / 0.5, 0.5, seed + 3) * 1.2 - 0.2, 0, 1) * np.exp (-(Hh - Y) / 2.5)
	dirt = np.clip (run + roof, 0, 1)
	lum = (1 + tone + 0.006 * rng.standard_normal (A.shape).astype (np.float32)) * (1 - 0.10 * line (jd, 0.10, px)) * (1 - 0.07 * line (np.abs ((Y / 5.6 + 0.5) % 1.0 - 0.5) * 5.6, 0.12, px))
	lum = lum * (1 - 0.12 * np.exp (-Y / 1.2)) * (1 - 0.14 * line (Hh - 0.3 - Y, 0.6, px))
	rgb = col ((0.875, 0.872, 0.858)) * (lum * (1 - 0.11 * dirt))[..., None] * (1 - dirt[..., None] * col ((0.0, 0.008, 0.028)))
	return rgb.astype (np.float32), np.full (A.shape, 0.25, np.float32)

# ----------------------------------------------------------------------
# the detail maps (tiles: periodic)

def detail_pad (N, mp, rng):
	T = N * (1.0 / PAD_S) / 8.0     # (the tile [m]: a sheet's texel is 8 of its)
	n = lambda sc: mp.noise (N, N, (sc / T * N, sc / T * N), rng)
	fine, mid, big = n (0.012), n (0.05), n (0.22)
	pit = np.clip (n (0.016) - 1.7, 0, 2.0)
	speck = np.clip (n (0.03) - 1.5, 0, 2.0)
	d = 0.035 * fine + 0.030 * mid + 0.018 * big - 0.09 * pit + 0.05 * speck
	d -= d.mean ()
	return np.repeat ((0.5 + d)[..., None], 3, axis=2).astype (np.float32)

def detail_vab (N, mp, rng):
	T = N * (1.0 / VAB_S) / 8.0     # (7.1 m)
	ribs = int (round (T / 0.32))
	u = (np.arange (N) + 0.5) / N * ribs
	p = np.abs (u % 1.0 - 0.5) * 0.32      # (from a rib's crest [m]: crests 0.07 m, webs to the pans)
	crest = np.clip ((0.045 - p) / 0.012, 0, 1); web = np.clip (1 - np.abs (p - 0.06) / 0.022, 0, 1)
	prof = 0.035 * crest - 0.13 * web
	d = np.repeat (prof[None, :], N, axis=0) + 0.010 * mp.noise (N, N, (0.02 / T * N, 0.02 / T * N), rng)
	d -= d.mean ()
	return np.repeat ((0.5 + d)[..., None], 3, axis=2).astype (np.float32)

# ----------------------------------------------------------------------

def paint_sheet (name, rng):
	W, Hh = SIZES[name]
	bg = HARD if name == 'cape_pad' else VAB_WHITE
	rgb = np.empty ((Hh, W, 3), np.float32); rgb[:] = bg
	smooth = np.full ((Hh, W), 0.15 if name == 'cape_pad' else 0.42, np.float32)
	G = 4
	for rn, r in REGIONS[name].items ():
		a, b = r.grid (G)
		if name == 'cape_pad':
			px = 1.0 / (MLP_S if rn.startswith ('mlp') else PAD_S)
			if rn in ('mound', 'ramp'): c, s = pad_paint (a, b, rn == 'ramp', px, rng)
			elif rn.startswith ('lane'): c, s = lane_paint (a, b - LANES[int (rn[-1])], int (rn[-1]), px, rng)
			elif rn == 'mlp_deck': c, s = mlp_deck_paint (a, b, px, rng)
			else: c, s = mlp_side_paint (a, b, 'nswe'.index (rn[-1]), px, rng)
		else:
			c, s = vab_paint (rn, a, b, 1.0 / VAB_S, rng)
		ya, xa = max (r.y - G, 0), max (r.x - G, 0)
		yb, xb = min (r.y + r.h + G, Hh), min (r.x + r.w + G, W)
		rgb[ya:yb, xa:xb] = c[ya - (r.y - G):yb - (r.y - G), xa - (r.x - G):xb - (r.x - G)]
		smooth[ya:yb, xa:xb] = s[ya - (r.y - G):yb - (r.y - G), xa - (r.x - G):xb - (r.x - G)]
		print ('  %s.%s: %d x %d px' % (name, rn, r.w, r.h), flush=True)
	return rgb, smooth


def write_sheet (name, rgb, smooth, detail, preview=None):
	"""the sheet (BC1, sRGB), its smoothness an eighth its size, its detail tile (BC1, linear)"""
	import kittex
	mp = kittex.mp
	base = os.path.join (kittex.OUT, name)
	os.makedirs (kittex.OUT, exist_ok=True)
	mp.write_dds (base + '.dds', np.clip (rgb, 0, 1), srgb=True)
	Hh, W = smooth.shape
	q = smooth.reshape (Hh // 8, 8, W // 8, 8).mean (axis=(1, 3))
	z = np.zeros_like (q)
	mp.write_dds (base + '_rghn.dds', np.stack ([z, np.clip (q, 0, 1), z], axis=2), srgb=False)
	mp.write_dds (base + '_detail.dds', np.clip (detail, 0, 1), srgb=False)
	if preview:
		from PIL import Image
		os.makedirs (preview, exist_ok=True)
		Image.fromarray ((np.clip (rgb, 0, 1) * 255 + 0.5).astype (np.uint8)).save (os.path.join (preview, name + '.png'))
		Image.fromarray ((np.clip (detail, 0, 1) * 255 + 0.5).astype (np.uint8)).save (os.path.join (preview, name + '_detail.png'))
	print ('%s: %d x %d px, detail %d px' % (name, W, Hh, detail.shape[0]), flush=True)


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--only', choices=('pad', 'vab'))
	ap.add_argument ('--preview')
	a = ap.parse_args ()
	import kittex
	for k, (name, dfun, dn) in enumerate ((('cape_pad', detail_pad, 1024), ('cape_vab', detail_vab, 512))):
		if a.only and 'cape_' + a.only != name: continue
		rng = np.random.default_rng (390 + k)
		rgb, smooth = paint_sheet (name, rng)
		write_sheet (name, rgb, smooth, dfun (dn, kittex.mp, rng), a.preview)


if __name__ == '__main__':
	main ()
