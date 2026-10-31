# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# A stock texture remade at a higher resolution, in the same layout (so that
# meshes and skins still fit), with maps for the Vulkan client's materials:
# - the image is enlarged and each texel is given to a material of a
#   palette (painted metal, bare metal, insulation, ...) by its colour: the
#   materials' boundaries come out sharp, the original's dirt and shading
#   stay (as the ratio of its brightness to the material's colour)
# - panel lines (thin dark lines of the original) become grooves of a
#   height map, which gives the normal map (<tex>_norm)
# - each material has its roughness (<tex>_rghn, stored as smoothness) and
#   metalness (<tex>_metal), and fine detail (orange peel of paint, the
#   grain of brushed metal) beyond what the original had
# Writes PNG images; ddsenc.py makes the DDS textures of them.
#   out/assetvenv/bin/python Utils/BlenderMsh/texrefine.py <palette.json> <in.dds|png> <out stem> [--scale 4]
# palette.json: {"materials": [{"name", "colors": [[r, g, b], ...], "rough", "metal", "detail": "paint|metal|rough|none",
#                "flatten": 0..1 (how far the colour is evened out)}], "lines": strength of panel lines (0..1)}

import os, sys, json, math, argparse
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import orbiter_msh as om

def material_config (path, mesh):
	"""per material: (smoothness, metalness) of a material configuration
	file (Config/GC/<vessel>.cfg) for the mesh"""
	out, cur, mat = {}, None, None
	for l in open (path, encoding="latin-1"):
		t = l.split (";")[0].split ()
		if not t: continue
		if t[0] == "MESH": cur = t[1].lower ().replace ("/", "\\"); mat = None
		elif t[0] == "MATERIAL": mat = int (t[1])
		elif cur == mesh.lower ().replace ("/", "\\") and mat is not None:
			if t[0] == "SMOOTHNESS": out.setdefault (mat, [None, None])[0] = float (t[1])
			elif t[0] == "METALNESS": out.setdefault (mat, [None, None])[1] = float (t[1])
	return out

def material_maps (msh, texname, cfg, W, H):
	"""smoothness and metalness of the texels the mesh's groups use (their
	materials': the mesh's own values, refined by the configuration), and
	where they are used; texels no group uses take their nearest used one's"""
	m = om.read_msh (msh)
	ti = [k for k, t in enumerate (m.textures) if t[0].lower ().replace ("/", "\\").endswith (texname.lower ())]
	idx = Image.new ("I", (W, H), 0)
	d = ImageDraw.Draw (idx)
	vals = {}
	for g in m.groups:
		if g.tex not in ti: continue
		mt = m.materials[g.mat] if 0 <= g.mat < len (m.materials) else None
		smooth = 1.0 - (min (1.0, math.sqrt (2.0 / (mt.power + 2.0))) if mt and mt.power > 0 else 1.0)
		metal = 0.0
		c = cfg.get (g.mat, [None, None])
		if c[0] is not None: smooth = c[0]
		if c[1] is not None: metal = c[1]
		vals[g.mat + 1] = (smooth, metal)
		for t in g.tri:
			if len (set (t)) < 3: continue
			uv = [(g.vtx[i][6], g.vtx[i][7]) for i in t]
			ox, oy = math.floor (min (u for u, v in uv)), math.floor (min (v for u, v in uv))
			d.polygon ([((u - ox) * W, (v - oy) * H) for u, v in uv], fill=g.mat + 1)
	ids = np.asarray (idx, np.int32)
	used = ids > 0
	if not used.any (): return None
	# (unused texels: the nearest used one's, for mip maps and filtering)
	_, (iy, ix) = ndimage.distance_transform_edt (~used, return_indices=True)
	ids = ids[iy, ix]
	sm = np.zeros ((H, W), np.float32); me = np.zeros ((H, W), np.float32)
	for k, (a, b) in vals.items (): sm[ids == k] = a; me[ids == k] = b
	return sm, me, used

def srgb_to_lin (c): return np.where (c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def lin_to_srgb (c): return np.where (c <= 0.0031308, c * 12.92, 1.055 * np.power (np.clip (c, 0, None), 1 / 2.4) - 0.055)

def to_lab (rgb):
	"""CIE Lab of sRGB colours in 0..1 (D65)"""
	l = srgb_to_lin (rgb)
	M = np.array ([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
	xyz = l @ M.T / np.array ([0.9505, 1.0, 1.089])
	f = np.where (xyz > 0.008856, np.cbrt (xyz), 7.787 * xyz + 16 / 116)
	return np.stack ([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)

def noise (shape, scale, seed, octaves=3):
	"""smooth value noise in -1..1 with features of about scale texels"""
	rng = np.random.default_rng (seed)
	out = np.zeros (shape, np.float32)
	amp, tot = 1.0, 0.0
	for o in range (octaves):
		s = max (scale / (2 ** o), 1.0)
		small = rng.standard_normal ((int (shape[0] / s) + 2, int (shape[1] / s) + 2)).astype (np.float32)
		big = ndimage.zoom (small, (shape[0] / small.shape[0] + 1e-6, shape[1] / small.shape[1] + 1e-6), order=3)[:shape[0], :shape[1]]
		out += amp * big; tot += amp; amp *= 0.5
	return out / tot

def refine (pal, src, scale, matmaps=None):
	img = np.asarray (Image.open (src).convert ("RGB"), np.float32) / 255.0
	h, w = img.shape[:2]
	H, W = h * scale, w * scale
	big = np.asarray (Image.fromarray ((img * 255).astype (np.uint8)).resize ((W, H), Image.LANCZOS), np.float32) / 255.0
	# (sharpened: an unsharp mask about an original texel wide)
	sh = pal.get ("sharpen", 0.0)
	if sh: big = np.clip (big + sh * (big - ndimage.gaussian_filter (big, (scale * 0.6, scale * 0.6, 0))), 0, 1)
	mats = pal["materials"]
	# (classified on the enlarged image smoothed a little: the boundaries
	# follow the original's, without its texel steps and compression noise)
	smooth = ndimage.gaussian_filter (big, (scale * 0.35, scale * 0.35, 0))
	lab = to_lab (smooth)
	dist = []
	for m in mats:
		cl = to_lab (np.array (m["colors"], np.float32) / 255.0)
		d = np.min (np.linalg.norm (lab[:, :, None, :] - cl[None, None, :, :], axis=-1), axis=-1)
		dist.append (d)
	dist = np.stack (dist, -1)
	label = dist.argmin (-1)
	conf = np.clip (1.0 - (dist.min (-1) - 10.0) / 25.0, 0.0, 1.0)  # (far from all: kept as it was)
	# albedo: the material's colour times the original's shading (its
	# brightness relative to the colour), evened out by "flatten"
	base = np.array ([np.array (m["colors"][0], np.float32) / 255.0 for m in mats])
	flat = np.array ([m.get ("flatten", 0.6) for m in mats], np.float32)
	lum = lambda c: (srgb_to_lin (c) * np.array ([0.2126, 0.7152, 0.0722])).sum (-1)
	bl, ml = lum (big), lum (base)[label]
	ratio = np.clip (bl / np.maximum (ml, 1e-3), 0.25, 1.6)
	ratio = 1.0 + (ratio - 1.0) * (1.0 - flat[label])
	clean = lin_to_srgb (srgb_to_lin (base[label]) * ratio[..., None])
	alb = clean * conf[..., None] + big * (1.0 - conf[..., None])
	# panel lines: thin features darker than their surroundings, at the
	# original's resolution, enlarged smoothly and made thinner
	L = lum (img)
	tophat = ndimage.grey_closing (L, size=(3, 3)) - L
	# (only long ones: specks of dirt would become dents)
	lab_, n_ = ndimage.label (tophat > 0.02, structure=np.ones ((3, 3)))
	if n_:
		size = ndimage.sum (np.ones_like (L), lab_, index=np.arange (1, n_ + 1))
		keep = np.concatenate ([[False], size >= pal.get ("line_min", 12)])
		tophat = tophat * keep[lab_]
	lines = ndimage.zoom (tophat, scale, order=3)
	lines = np.clip ((lines - 0.015) / 0.06, 0, 1) ** 1.5 * pal.get ("lines", 0.8)
	# fine detail per material
	rough = np.array ([m["rough"] for m in mats], np.float32)[label]
	metal = np.array ([m["metal"] for m in mats], np.float32)[label]
	kind = np.array ([{"paint": 1, "metal": 2, "rough": 3}.get (m.get ("detail", "none"), 0) for m in mats])[label]
	if matmaps is not None:   # (metal where the materials are)
		kind = np.where (matmaps[1] > 0.5, 2, np.where (kind == 2, 0, kind))
	n_fine = noise ((H, W), 3.0, 1, 2)
	n_mid = noise ((H, W), 24.0, 2, 3)
	streak = ndimage.gaussian_filter (np.random.default_rng (3).standard_normal ((H, W)).astype (np.float32), (0.6, 18.0))
	streak /= streak.std () + 1e-6
	height = -lines * 1.0
	height += np.where (kind == 1, 0.012 * n_fine, 0.0)                   # orange peel
	height += np.where (kind == 2, 0.01 * streak, 0.0)                    # brushed metal
	height += np.where (kind == 3, 0.04 * n_fine + 0.03 * n_mid, 0.0)     # insulation, rough surfaces
	alb = alb * (1.0 + 0.025 * n_mid[..., None] + 0.015 * n_fine[..., None])
	# (dirt, as darker than the material, is rougher)
	dirt = np.clip (1.0 - ratio, 0, 1)
	rough = np.clip (rough + 0.25 * dirt + 0.025 * n_mid + np.where (kind == 2, 0.04 * streak, 0.0), 0.05, 1.0)
	rough = rough * conf + 0.7 * (1.0 - conf)
	metal = metal * conf
	if matmaps is not None:
		# (the materials' own values, as their authors set them, with the
		# dirt and the grain of the palette's surfaces on top)
		sm, me, used = matmaps
		rough = np.clip (1.0 - sm + 0.25 * dirt + 0.025 * n_mid, 0.04, 1.0)
		metal = me
	# panel lines darken the colour a little (the grooves' shadow is the
	# normal map's) and are rougher
	alb = alb * (1.0 - 0.12 * lines[..., None])
	rough = np.clip (rough + 0.2 * lines, 0, 1)
	# normal map from the height (x: +u, y: +v; strength in texels)
	k = pal.get ("bump", 2.0)
	gy, gx = np.gradient (height)
	n = np.stack ([-gx * k, -gy * k, np.ones_like (height)], -1)
	n /= np.linalg.norm (n, axis=-1, keepdims=True)
	return np.clip (alb, 0, 1), n * 0.5 + 0.5, 1.0 - rough, metal, label

if __name__ == "__main__":
	ap = argparse.ArgumentParser ()
	ap.add_argument ("palette"); ap.add_argument ("src"); ap.add_argument ("out")
	ap.add_argument ("--scale", type=int, default=4)
	ap.add_argument ("--mesh", help="mesh whose materials give the roughness and metalness")
	ap.add_argument ("--matcfg", help="material configuration (Config/GC/<vessel>.cfg)")
	ap.add_argument ("--meshname", help="the mesh's name in the configuration, e.g. DG\\deltaglider_ns")
	ap.add_argument ("--texname", help="the texture's name in the mesh, e.g. DGMK4_1.dds")
	a = ap.parse_args ()
	pal = json.load (open (a.palette))
	mm = None
	if a.mesh:
		w, h = Image.open (a.src).size
		cfg = material_config (a.matcfg, a.meshname) if a.matcfg else {}
		mm = material_maps (a.mesh, a.texname, cfg, w * a.scale, h * a.scale)
	alb, nrm, smooth, metal, label = refine (pal, a.src, a.scale, mm)
	u8 = lambda x: Image.fromarray ((np.clip (x, 0, 1) * 255 + 0.5).astype (np.uint8))
	u8 (alb).save (a.out + ".png")
	u8 (nrm).save (a.out + "_norm.png")
	u8 (smooth).save (a.out + "_rghn.png")
	u8 (metal).save (a.out + "_metal.png")
	pal_cols = np.array ([np.array (m["colors"][0]) / 255.0 for m in pal["materials"]])
	u8 (pal_cols[label]).save (a.out + "_labels.png")
	print ("%s: %d x %d" % (a.out, alb.shape[1], alb.shape[0]))
