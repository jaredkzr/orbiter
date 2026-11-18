#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Tiling materials of the base kits (Utils/BaseKit/blender_kit.py): each a
seamless square of a surface at its real size, mapped by the kit's meshes in
world units (MATERIALS: the tile's size [m]), with the maps the Vulkan client
reads (VkTexture.cpp LoadMaps): _norm (tangent-space normals, BC5),
_rghn (smoothness in green), _metal (metalness in green), _night (emission
lit at night only: windows, lamps).

The colours are neutral where a mesh tints them by its material's diffuse
colour (regolith: the body's soil; panels: their paint).

    kittex.py [--only NAME] [--preview DIR] [--size N]

Writes Textures/BaseKit/<name>.dds and its maps.
"""

import argparse, math, os, sys
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
ROOT = os.path.normpath (os.path.join (HERE, '..', '..'))
sys.path.insert (0, os.path.join (ROOT, 'Utils', 'runway'))
try: import make_pavement as mp  # (noise, normals, mips, BC1/BC5 encoders, DDS writer)
except ImportError: mp = None    # (inside Blender: MATERIALS only)

OUT = os.path.join (ROOT, 'Textures', 'BaseKit')

# name: tile size [m] (the kit's meshes map a tile over this many metres)
MATERIALS = {
	'kit_panel': 4.0,      # insulated hull panels, white (tinted by the material): seams, fasteners, wear
	'kit_mli': 2.0,        # multi-layer insulation: quilted foil, crinkled
	'kit_regolith': 4.0,   # sintered regolith, sandbags (neutral: tinted to the soil)
	'kit_steel': 2.0,      # painted structural steel: edges worn
	'kit_grating': 1.0,    # walkway grating
	'kit_solar': 2.0,      # solar cells, busbars
	'kit_radiator': 2.0,   # radiator panels: fluid tubes under white coating
	'kit_glass': 4.0,      # windows: dark glass, frames; lit rooms at night (_night)
	'kit_growglass': 2.0,  # greenhouse glazing: green-tinged panes in white frames; grow lights at night
	'kit_hazard': 1.0,     # hazard stripes, yellow and black
	'kit_concrete': 4.0,   # cast concrete: formwork panels, tie holes
	'kit_cladding': 2.0,   # trapezoidal steel siding (Earth's sheds and halls): ribs, a lap, streaks (tinted)
}


def tile_noise (N, scale_m, T, rng):
	"""periodic noise, features about scale_m [m] on a tile T [m] of N px"""
	return mp.noise (N, N, (scale_m / T * N, scale_m / T * N), rng)


def seams (N, T, pitch_u, pitch_v, width, offset_rows=False):
	"""coverage of seams (grooves) of panels pitch_u x pitch_v [m], width [m];
	offset_rows: every other row shifted by half a panel"""
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N))
	u = np.ones ((N, 1)) * c[None, :]
	if offset_rows:
		row = np.floor (v / pitch_v)
		u = u + (row % 2) * 0.5 * pitch_u
	du_ = np.abs (((u / pitch_u) + 0.5) % 1.0 - 0.5) * pitch_u
	dv_ = np.abs (((v / pitch_v) + 0.5) % 1.0 - 0.5) * pitch_v
	cov = np.maximum (np.clip ((width / 2 - du_) / du + 0.5, 0, 1), np.clip ((width / 2 - dv_) / du + 0.5, 0, 1))
	return cov, u, v


def write (name, rgb, smooth, height, T, metal=None, night=None, preview=None):
	N = rgb.shape[0]
	os.makedirs (OUT, exist_ok=True)
	base = os.path.join (OUT, name)
	mp.write_dds (base + '.dds', np.clip (rgb, 0, 1), srgb=True)
	nm = mp.normals (height, T / N)
	mp.write_dds (base + '_norm.dds', nm, srgb=False, fmt='ATI2')
	q = smooth.reshape (N // 4, 4, N // 4, 4).mean (axis=(1, 3))
	z = np.zeros_like (q)
	mp.write_dds (base + '_rghn.dds', np.stack ([z, np.clip (q, 0, 1), z], axis=2), srgb=False)
	if metal is not None:
		q = metal.reshape (N // 4, 4, N // 4, 4).mean (axis=(1, 3))
		mp.write_dds (base + '_metal.dds', np.stack ([z, np.clip (q, 0, 1), z], axis=2), srgb=False)
	if night is not None:
		h = night.reshape (N // 2, 2, N // 2, 2, 3).mean (axis=(1, 3))
		mp.write_dds (base + '_night.dds', np.clip (h, 0, 1), srgb=True)
	if preview:
		from PIL import Image
		os.makedirs (preview, exist_ok=True)
		Image.fromarray ((np.clip (rgb, 0, 1) * 255 + 0.5).astype (np.uint8)).save (os.path.join (preview, name + '.png'))
		if night is not None:
			Image.fromarray ((np.clip (night, 0, 1) * 255 + 0.5).astype (np.uint8)).save (os.path.join (preview, name + '_night.png'))
	print ('%s: %d px for %.1f m (%.1f mm)%s%s' % (name, N, T, T / N * 1000, ', metal' if metal is not None else '',
		', night' if night is not None else ''))


# ----------------------------------------------------------------------
# the materials

def m_panel (N, T, rng):
	cov, u, v = seams (N, T, 2.0, 1.0, 0.012)
	pnl = np.floor (u / 2.0) * 7 + np.floor (v / 1.0) * 13
	shade = (np.sin (pnl * 12.9898) * 43758.5453) % 1.0  # (each panel a shade apart)
	big, mid, fine = tile_noise (N, 1.5, T, rng), tile_noise (N, 0.2, T, rng), tile_noise (N, 0.01, T, rng)
	rgb = np.full ((N, N, 3), 0.88) * (1 + 0.025 * (shade - 0.5) + 0.02 * big + 0.012 * mid + 0.015 * fine)[..., None]
	# fasteners along the seams, every 0.25 m
	du = T / N
	fu = np.abs (((u / 0.25) + 0.5) % 1.0 - 0.5) * 0.25
	fv = np.abs (((v / 1.0) + 0.5) % 1.0 - 0.5) * 1.0
	fast = np.clip ((0.006 - np.hypot (fu, fv - 0.03)) / du + 0.5, 0, 1)
	# dust and grime gathered low on each panel
	grime = np.clip (tile_noise (N, 0.6, T, rng) * 0.5 + 0.3 * (1 - (v % 1.0)), 0, 1)
	rgb *= (1 - 0.06 * grime)[..., None]
	rgb *= (1 - 0.55 * cov)[..., None]
	rgb *= (1 - 0.25 * fast)[..., None]
	height = -0.003 * cov + 0.0006 * fast + 0.0002 * fine + 0.0008 * big
	smooth = 0.55 - 0.15 * grime - 0.3 * cov
	return rgb, smooth, height, None, None


def m_mli (N, T, rng):
	# quilted foil: stitch lines every 0.5 m, crinkles between
	cov, u, v = seams (N, T, 0.5, 0.5, 0.01)
	cr1, cr2 = tile_noise (N, 0.03, T, rng), tile_noise (N, 0.012, T, rng)
	pillow = (np.sin (math.pi * (u % 0.5) / 0.5) * np.sin (math.pi * (v % 0.5) / 0.5))
	gold = np.array ([0.83, 0.62, 0.26])
	rgb = gold[None, None, :] * (1 + 0.06 * cr1 + 0.05 * cr2)[..., None]
	rgb *= (1 - 0.35 * cov)[..., None]
	height = 0.004 * pillow + 0.0012 * cr1 + 0.0005 * cr2 - 0.002 * cov
	smooth = 0.7 + 0.1 * cr2
	metal = 0.9 - 0.5 * cov
	return rgb, smooth, height, metal, None


def m_regolith (N, T, rng):
	# sandbags of sintered regolith, 0.8 x 0.4 m, rows offset
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N))
	u = np.ones ((N, 1)) * c[None, :] + (np.floor (v / 0.4) % 2) * 0.4
	lu, lv = (u % 0.8) / 0.8, (v % 0.4) / 0.4
	bag = np.clip (np.sin (math.pi * lu), 0, 1) ** 0.35 * np.clip (np.sin (math.pi * lv), 0, 1) ** 0.5
	idx = np.floor (u / 0.8) * 17 + np.floor (v / 0.4) * 31
	shade = (np.sin (idx * 12.9898) * 43758.5453) % 1.0
	grain, mid = tile_noise (N, 0.008, T, rng), tile_noise (N, 0.15, T, rng)
	rgb = np.full ((N, N, 3), 0.62) * (0.9 + 0.1 * bag + 0.05 * (shade - 0.5) + 0.04 * mid + 0.06 * grain)[..., None]
	rgb *= (0.55 + 0.45 * np.clip (bag * 3, 0, 1))[..., None]  # (the gaps between bags, shadowed)
	height = 0.06 * bag + 0.002 * grain + 0.004 * mid
	smooth = 0.05 + 0.03 * grain
	return rgb, smooth, height, None, None


def m_steel (N, T, rng):
	cov, u, v = seams (N, T, 1.0, 2.0, 0.006)
	big, fine = tile_noise (N, 0.5, T, rng), tile_noise (N, 0.006, T, rng)
	wear = np.clip ((tile_noise (N, 0.02, T, rng) - 1.8) * 2, 0, 1) * np.clip (tile_noise (N, 0.3, T, rng), 0, 1)
	paint = np.array ([0.72, 0.73, 0.74])
	rgb = paint[None, None, :] * (1 + 0.03 * big + 0.02 * fine)[..., None]
	rgb = rgb * (1 - wear[..., None]) + np.array ([0.42, 0.42, 0.43])[None, None, :] * wear[..., None]
	rgb *= (1 - 0.4 * cov)[..., None]
	height = 0.0004 * fine + 0.001 * big - 0.002 * cov - 0.0003 * wear
	smooth = 0.42 + 0.25 * wear
	metal = 0.15 + 0.8 * wear
	return rgb, smooth, height, metal, None


def m_grating (N, T, rng):
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N)); u = np.ones ((N, 1)) * c[None, :]
	bar = np.clip ((0.003 - np.abs (((u / 0.03) + 0.5) % 1 - 0.5) * 0.03) / du + 0.5, 0, 1)
	cross = np.clip ((0.002 - np.abs (((v / 0.1) + 0.5) % 1 - 0.5) * 0.1) / du + 0.5, 0, 1)
	metal_ = np.maximum (bar, cross)
	rgb = np.where (metal_[..., None] > 0.5, np.array ([0.55, 0.56, 0.57]), np.array ([0.05, 0.05, 0.05]))
	rgb = rgb * (1 + 0.04 * tile_noise (N, 0.2, T, rng))[..., None]
	height = 0.02 * metal_
	smooth = 0.3 + 0.3 * metal_
	return rgb, smooth, height, 0.8 * metal_, None


def m_solar (N, T, rng):
	# cells 0.156 m (12 x 12 a tile), busbars, a frame every 1 m
	du = T / N
	cell, cu, cv = seams (N, T, 2.0 / 12, 2.0 / 12, 0.004)
	frame, _, _ = seams (N, T, 1.0, 2.0, 0.02)
	bus = np.clip ((0.0008 - np.abs (((cu / (2.0 / 36)) + 0.5) % 1 - 0.5) * (2.0 / 36)) / du + 0.5, 0, 1)
	sheen = tile_noise (N, 0.5, T, rng)
	rgb = np.array ([0.04, 0.06, 0.14])[None, None, :] * (1 + 0.15 * sheen)[..., None]
	rgb = rgb * (1 - cell[..., None]) + np.array ([0.55, 0.56, 0.58]) * cell[..., None] * 0.6
	rgb = rgb * (1 - bus[..., None]) + np.array ([0.7, 0.7, 0.72]) * bus[..., None]
	rgb = rgb * (1 - frame[..., None]) + np.array ([0.62, 0.63, 0.65]) * frame[..., None]
	height = 0.002 * frame - 0.0005 * cell
	smooth = 0.85 - 0.4 * np.maximum (cell, frame)
	metal = np.maximum (bus, frame) * 0.9
	return rgb, smooth, height, metal, None


def m_radiator (N, T, rng):
	du = T / N
	c = (np.arange (N) + 0.5) * du
	u = np.ones ((N, 1)) * c[None, :]
	tube = np.clip (1 - np.abs (((u / 0.1) + 0.5) % 1 - 0.5) * 0.1 / 0.012, 0, 1) ** 2
	cov, _, _ = seams (N, T, 2.0, 1.0, 0.01)
	rgb = np.full ((N, N, 3), 0.9) * (1 - 0.06 * tube - 0.3 * cov + 0.02 * tile_noise (N, 0.4, T, rng))[..., None]
	height = 0.006 * tube - 0.002 * cov
	smooth = 0.35 + 0.1 * tube
	return rgb, smooth, height, None, None


def m_glass (N, T, rng):
	# a window band 4 m wide: 4 panes of 1 m in frames; behind them at night,
	# lit rooms (warm, cool, dark) in panes of their own
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N)); u = np.ones ((N, 1)) * c[None, :]
	fr = 0.05
	fu = np.abs (((u / 1.0) + 0.5) % 1 - 0.5) * 1.0
	fv = np.abs (((v / 1.0) + 0.5) % 1 - 0.5) * 1.0
	frame = np.maximum (np.clip ((fr - fu) / du + 0.5, 0, 1), np.clip ((fr - fv) / du + 0.5, 0, 1))
	refl = tile_noise (N, 1.2, T, rng)
	rgb = np.array ([0.07, 0.09, 0.11])[None, None, :] * (1 + 0.3 * refl)[..., None]
	rgb = rgb * (1 - frame[..., None]) + np.array ([0.6, 0.61, 0.62]) * frame[..., None]
	smooth = 0.95 - 0.55 * frame
	metal = 0.8 * frame
	height = 0.01 * frame
	# night: each pane lit or not, warm or cool, a little uneven
	pane = np.floor (u) * 7 + np.floor (v) * 13
	h = (np.sin (pane * 12.9898) * 43758.5453) % 1.0
	h2 = (np.sin (pane * 78.233) * 12345.678) % 1.0
	lit = (h < 0.62).astype (float)
	warm, cool = np.array ([1.0, 0.78, 0.48]), np.array ([0.80, 0.88, 1.0])
	col = np.where ((h2 < 0.7)[..., None], warm, cool)
	inner = np.clip ((np.minimum (fu, fv) - fr) / 0.15, 0, 1)
	night = col * (lit * (0.55 + 0.45 * inner) * (0.8 + 0.2 * tile_noise (N, 0.3, T, rng)))[..., None] * (1 - frame[..., None]) * 0.9
	return rgb, smooth, height, metal, night


def m_growglass (N, T, rng):
	# panes 1 x 0.5 m in white frames, plants' green behind; at night the grow lights (magenta-white)
	du = T / N
	cov, u, v = seams (N, T, 1.0, 0.5, 0.04)
	leaves = tile_noise (N, 0.05, T, rng)
	rgb = np.array ([0.16, 0.26, 0.15])[None, None, :] * (1 + 0.35 * leaves)[..., None]
	rgb = rgb * (1 - cov[..., None]) + np.array ([0.85, 0.86, 0.85]) * cov[..., None]
	smooth = 0.92 - 0.5 * cov
	height = 0.012 * cov
	glow = np.array ([1.0, 0.55, 0.95]) * 0.75
	night = glow[None, None, :] * (0.75 + 0.25 * np.clip (leaves, -1, 1))[..., None] * (1 - cov[..., None])
	return rgb, smooth, height, None, night


def m_hazard (N, T, rng):
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N)); u = np.ones ((N, 1)) * c[None, :]
	d = ((u + v) / 0.25) % 2.0  # (stripes 0.177 m at 45 deg, two a period of 0.25 m along u: tiles at 1 m)
	y = np.clip ((np.minimum (d, 2 - d) - 0.5) / (du / 0.25) + 0.5, 0, 1)
	wear = np.clip ((tile_noise (N, 0.01, T, rng) - 1.6) * 2, 0, 1) * 0.6
	rgb = np.where (y[..., None] > 0.5, np.array ([0.88, 0.66, 0.08]), np.array ([0.04, 0.04, 0.04]))
	rgb = rgb * (1 - 0.5 * wear[..., None]) + 0.3 * wear[..., None]
	height = 0.0003 * tile_noise (N, 0.01, T, rng)
	smooth = 0.4 - 0.2 * wear
	return rgb, smooth, height, None, None


def m_concrete (N, T, rng):
	# formwork panels 2.4 x 1.2 m, tie holes at their corners
	cov, u, v = seams (N, T, 2.0, 1.0, 0.006)
	du = T / N
	tu = np.abs (((u / 0.5) + 0.5) % 1 - 0.5) * 0.5
	tv = np.abs (((v / 0.5) + 0.5) % 1 - 0.5) * 0.5
	tie = np.clip ((0.012 - np.hypot (tu, tv)) / du + 0.5, 0, 1)
	big, mid, fine = tile_noise (N, 1.0, T, rng), tile_noise (N, 0.1, T, rng), tile_noise (N, 0.004, T, rng)
	stain = np.clip (tile_noise (N, 0.4, T, rng) - 0.8, 0, None) * 0.15
	rgb = np.array ([0.68, 0.67, 0.64])[None, None, :] * (1 + 0.03 * big + 0.03 * mid + 0.04 * fine - stain)[..., None]
	rgb *= (1 - 0.3 * cov - 0.5 * tie)[..., None]
	height = 0.0004 * fine + 0.0008 * mid - 0.002 * cov - 0.008 * tie
	smooth = 0.12 + 0.04 * mid
	return rgb, smooth, height, None, None


def m_cladding (N, T, rng):
	# trapezoidal sheet: ribs every 0.25 m (crests 0.07 m, 0.03 m proud), a horizontal lap
	# every 2 m (a tile), rain streaks down from the laps
	du = T / N
	c = (np.arange (N) + 0.5) * du
	v = c[:, None] * np.ones ((1, N)); u = np.ones ((N, 1)) * c[None, :]
	p = np.abs ((u % 0.25) - 0.125)
	rib = np.clip ((0.06 - p) / 0.02, 0, 1)                    # (1 on the crests, ramps on the webs)
	web = np.clip (1 - np.abs (p - 0.05) / 0.012, 0, 1)        # (the webs: shaded)
	lap = np.clip ((0.008 - np.abs (((v / 2.0) + 0.5) % 1 - 0.5) * 2.0) / du + 0.5, 0, 1)
	streak = np.repeat (tile_noise (N, 0.08, T, rng)[:1, :], N, axis=0)
	streak = np.clip (streak - 0.6, 0, None) * np.clip (1 - (v % 2.0) / 1.4, 0, 1)
	big, fine = tile_noise (N, 1.2, T, rng), tile_noise (N, 0.01, T, rng)
	rgb = np.full ((N, N, 3), 0.86) * (1 + 0.02 * big + 0.01 * fine - 0.10 * web - 0.12 * streak - 0.35 * lap)[..., None]
	height = 0.03 * rib - 0.002 * lap + 0.0002 * fine
	smooth = 0.5 - 0.15 * streak - 0.2 * lap
	return rgb, smooth, height, None, None


BUILD = {'kit_panel': m_panel, 'kit_mli': m_mli, 'kit_regolith': m_regolith, 'kit_steel': m_steel,
	'kit_grating': m_grating, 'kit_solar': m_solar, 'kit_radiator': m_radiator, 'kit_glass': m_glass,
	'kit_growglass': m_growglass,
	'kit_hazard': m_hazard, 'kit_concrete': m_concrete, 'kit_cladding': m_cladding}


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--only')
	ap.add_argument ('--preview')
	ap.add_argument ('--size', type=int, default=1024)
	a = ap.parse_args ()
	for k, (name, T) in enumerate (MATERIALS.items ()):
		if a.only and a.only != name: continue
		rgb, smooth, height, metal, night = BUILD[name] (a.size, T, np.random.default_rng (100 + k))
		write (name, rgb, smooth, height, T, metal, night, a.preview)


if __name__ == '__main__':
	main ()
