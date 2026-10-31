# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Textures designed anew on the vessel itself, baked into a mesh's texture
# layout (texbake.py): nothing of the old texture is used. A design (JSON)
# describes the vessel in its own coordinates (metres; x right, y up, z
# forward):
#
#   "finishes":  named surface finishes: colour (sRGB), smoothness,
#                metalness, "grain" (paint, metal, tiles)
#   "zones":     [finish, condition] in order, the later over the earlier;
#                conditions are numpy expressions of x, y, z, ax (|x|),
#                nx, ny, nz (the normal), mat (material index), grp (group
#                index), label (group label: label == "Hatch1")
#   "seams":     families of panel seams: planes across an axis ("axis",
#                "at" or "start"/"step"/"end"), where a condition holds;
#                grooves of "width" metres, rivets along them ("rivets":
#                spacing)
#   "tiles":     a staggered grid of heat shield tiles where a condition
#                holds ("size", "gap", tone variation "vary")
#   "panels":    access panels: rounded rectangles on a view plane ("side",
#                "top", "bottom", "front"): centre, size, corner radius,
#                optional fill finish; "mirror" for both sides
#   "decals":    markings on a view plane: text, trefoil, arrow, stripes
#                (walkways, hazard bands), mirrored where asked, reading
#                the right way round on both sides
#   "soot":      streaks downstream (-z) of points: thrusters, vents
#   "grime":     strength of large, soft variations of the finish
#
#   out/assetvenv/bin/python Utils/BlenderMsh/texdesign.py <design.json> <msh> <texname> <out stem> [--size 2048]
#
# Writes <out>.png, <out>_norm.png, <out>_rghn.png (smoothness),
# <out>_metal.png, as texvector.py.

import os, sys, json, math, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import texbake, texrefine as tr
import orbiter_msh as om

FONT_DIR = "/usr/share/fonts/truetype/dejavu"

def noise3 (p, scale, seed):
	"""smooth value noise in -1..1 of 3D positions, features of about scale metres"""
	rng = np.random.default_rng (seed)
	lat = rng.standard_normal ((64, 64, 64)).astype (np.float32)
	q = p / scale + 32.0
	return ndimage.map_coordinates (lat, [q[..., 0] % 63, q[..., 1] % 63, q[..., 2] % 63], order=1, mode="wrap") * 0.7

def hash2 (i, j, seed=0):
	"""random value in 0..1 per integer cell"""
	h = (i.astype (np.int64) * 73856093) ^ (j.astype (np.int64) * 19349663) ^ (seed * 83492791)
	h = (h ^ (h >> 13)) * 1274126177
	return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0

def plane_coords (b, plane, sign, center):
	"""coordinates (metres) of texels on a view plane relative to a centre
	(given for the right side, x > 0, or the top's right half; mirrored to
	the other with sign -1): u to the viewer's right, v up, as the plane is
	seen from outside (axes left-handed: x right, y up, z forward); and
	where the surface faces the viewer"""
	x, y, z = b["pos"][..., 0], b["pos"][..., 1], b["pos"][..., 2]
	nx, ny, nz = b["nrm"][..., 0], b["nrm"][..., 1], b["nrm"][..., 2]
	ca, cb = center
	if plane == "side":   # (seen from +x: the nose to the right)
		return (z - ca) * sign, y - cb, (nx * sign > 0.35) & (x * sign > 0)
	if plane == "top":    # (seen from above, the nose up)
		return x - ca * sign, z - cb, (ny > 0.35) & (x * sign >= 0)
	if plane == "bottom": # (seen from below, the nose up)
		return -(x - ca * sign), z - cb, (ny < -0.35) & (x * sign >= 0)
	if plane == "front":
		return -(x - ca * sign), y - cb, (nz > 0.35) & (x * sign >= 0)
	if plane == "rear":
		return x - ca * sign, y - cb, (nz < -0.35) & (x * sign >= 0)
	raise ValueError (plane)

def symbol_image (d, px_per_m):
	"""an RGBA bitmap of a decal, its size in metres"""
	w, h = d["size"]
	W, H = max (8, int (w * px_per_m)), max (8, int (h * px_per_m))
	im = Image.new ("RGBA", (W, H), (0, 0, 0, 0))
	dr = ImageDraw.Draw (im)
	col = lambda c: tuple (int (v) for v in c) + (255,)
	t = d["type"]
	if d.get ("bg"):
		dr.rounded_rectangle ([0, 0, W - 1, H - 1], radius=int (d.get ("radius", 0) * px_per_m), fill=col (d["bg"]))
	if t == "text":
		font = ImageFont.truetype (os.path.join (FONT_DIR, d.get ("font", "DejaVuSansCondensed-Bold.ttf")), 400)
		tb = Image.new ("L", (6000, 600), 0)
		ImageDraw.Draw (tb).text ((20, 20), d["text"], font=font, fill=255)
		tb = tb.crop (tb.getbbox ())
		f = min (W * d.get ("fill", 0.9) / tb.width, H * d.get ("fill", 0.9) / tb.height)
		tb = tb.resize ((max (1, int (tb.width * f)), max (1, int (tb.height * f))), Image.LANCZOS)
		im.paste (Image.new ("RGBA", tb.size, col (d["fg"])), ((W - tb.width) // 2, (H - tb.height) // 2), tb)
	elif t == "trefoil":
		cx, cy = W / 2, H / 2
		R = min (W, H) * 0.46; r = R / 5
		dr.ellipse ([cx - r, cy - r, cx + r, cy + r], fill=col (d["fg"]))
		for a0 in (90, 210, 330):
			dr.pieslice ([cx - R, cy - R, cx + R, cy + R], a0 - 30, a0 + 30, fill=col (d["fg"]))
		dr.ellipse ([cx - 1.5 * r, cy - 1.5 * r, cx + 1.5 * r, cy + 1.5 * r], fill=col (d["bg"]))
		dr.ellipse ([cx - r, cy - r, cx + r, cy + r], fill=col (d["fg"]))
	elif t == "arrow":   # (pointing to +u)
		sh = H * 0.32
		hl = min (H * 0.95, W * 0.6)   # (the head)
		dr.rectangle ([0, H / 2 - sh / 2, W - hl, H / 2 + sh / 2], fill=col (d["fg"]))
		dr.polygon ([(W - 1, H / 2), (W - hl, 0), (W - hl, H - 1)], fill=col (d["fg"]))
	elif t == "stripes":   # (diagonal bands: hazard markings)
		per = d.get ("period", 0.1) * px_per_m
		for k in range (-int (H / per) - 2, int (W / per) + 2):
			x0 = k * per
			dr.polygon ([(x0, H), (x0 + per / 2, H), (x0 + per / 2 + H, 0), (x0 + H, 0)], fill=col (d["fg"]))
	elif t == "frame":   # (an outline: walkway edges)
		lw = max (1, int (d.get ("line", 0.03) * px_per_m))
		if d.get ("dash"):
			on, off = d["dash"][0] * px_per_m, d["dash"][1] * px_per_m
			for (x0, y0, x1, y1) in ((0, lw / 2, W, lw / 2), (0, H - lw / 2, W, H - lw / 2)):
				s = 0.0
				while s < x1 - x0:
					dr.line ([(x0 + s, y0), (x0 + min (s + on, x1 - x0), y1)], fill=col (d["fg"]), width=lw); s += on + off
		else:
			dr.rectangle ([lw / 2, lw / 2, W - lw / 2, H - lw / 2], outline=col (d["fg"]), width=lw)
	elif t == "rect":
		dr.rectangle ([0, 0, W - 1, H - 1], fill=col (d["fg"]))
	return np.asarray (im, np.float32) / 255.0

def round_box (u, v, cu, cv, hu, hv, r):
	"""signed distance to a rounded rectangle"""
	qu, qv = np.abs (u - cu) - (hu - r), np.abs (v - cv) - (hv - r)
	return np.hypot (np.maximum (qu, 0), np.maximum (qv, 0)) + np.minimum (np.maximum (qu, qv), 0) - r

def design (des, b, matcfg=None):
	pos, nrm, used = b["pos"], b["nrm"], b["used"]
	H, W = used.shape
	x, y, z = pos[..., 0], pos[..., 1], pos[..., 2]
	nx, ny, nz = nrm[..., 0], nrm[..., 1], nrm[..., 2]
	# metres per texel, here (for antialiasing): from the position map,
	# within islands
	gy, gx = np.gradient (pos, axis=(0, 1))
	px = np.maximum (np.linalg.norm (gx, axis=-1), np.linalg.norm (gy, axis=-1))
	med = b["scale"] if b["scale"] > 0 else float (np.median (px[used]))
	px = np.where ((px > 0.2 * med) & (px < 4 * med), px, med)
	px = ndimage.median_filter (px, 3)
	cov = lambda d, w: np.clip ((w / 2 - d) / px + 0.5, 0, 1)   # (antialiased band of width w around d = 0)
	m = b["msh"]
	grp = b["group"]
	matidx = np.array ([g.mat for g in m.groups] + [-1])[grp]
	labels = np.array ([g.label or "" for g in m.groups] + [""])[grp]
	env = dict (x=x, y=y, z=z, ax=np.abs (x), nx=nx, ny=ny, nz=nz, mat=matidx, grp=grp, label=labels, np=np, used=used)
	ev = lambda expr: np.broadcast_to (eval (expr, {"__builtins__": {}}, env), (H, W)) if expr else np.ones ((H, W), bool)
	# finishes, by zone
	fin = des["finishes"]
	names = list (fin)
	which = np.zeros ((H, W), np.int32)
	for name, cond in des["zones"]:
		which = np.where (ev (cond), names.index (name), which)
	col = np.array ([fin[n]["color"] for n in names], np.float32)[which] / 255.0
	smooth = np.array ([fin[n].get ("smooth", 0.3) for n in names], np.float32)[which]
	metal = np.array ([fin[n].get ("metal", 0.0) for n in names], np.float32)[which]
	grain = np.array ([fin[n].get ("grain", "paint") for n in names])[which]
	lin = tr.srgb_to_lin (col)
	height = np.zeros ((H, W), np.float32)
	# heat shield tiles: staggered rows across z, tone varying per tile
	for t in des.get ("tiles", []):
		w = ev (t.get ("where"))
		s = t.get ("size", 0.3); g = t.get ("gap", 0.008)
		row = np.floor (z / s)
		xs = x + 0.5 * s * (row % 2)
		col_ = np.floor (xs / s)
		du = np.abs (xs / s - np.round (xs / s)) * s / np.sqrt (np.maximum (1 - nx ** 2, 0.05))
		dv = np.abs (z / s - np.round (z / s)) * s / np.sqrt (np.maximum (1 - nz ** 2, 0.05))
		gapc = np.maximum (cov (du, g), cov (dv, g)) * w
		tone = 1.0 + t.get ("vary", 0.08) * (hash2 (row, col_) - 0.5) * 2
		lin = lin * np.where (w, tone, 1.0)[..., None]
		lin = lin * (1 - gapc[..., None] * (1 - t.get ("gap_shade", 0.4)))
		height -= gapc * t.get ("depth", 1.0)
	# panel seams
	seam = np.zeros ((H, W), np.float32)
	rivet_h = np.zeros ((H, W), np.float32)
	for f in des.get ("seams", []):
		w = ev (f.get ("where"))
		ax = "xyz".index (f["axis"])
		c, n = pos[..., ax], nrm[..., ax]
		if "at" in f:
			at = np.array (f["at"], np.float32)
			d = np.min (np.abs (c[..., None] - at), -1)
			near = at[np.argmin (np.abs (c[..., None] - at), -1)]
		else:
			st = f["step"]
			k = np.round ((c - f["start"]) / st)
			k = np.clip (k, 0, round ((f["end"] - f["start"]) / st))
			near = f["start"] + k * st
			d = np.abs (c - near)
		d = d / np.sqrt (np.maximum (1 - n ** 2, 0.05))   # (across the surface)
		width = f.get ("width", 0.012)
		seam = np.maximum (seam, cov (d, width) * w)
		if f.get ("rivets"):
			# (rows both sides of the seam, along the surface's other
			# directions)
			sp = f["rivets"]; off = f.get ("rivet_offset", 0.03); r = f.get ("rivet_radius", 0.006)
			other = [a for a in range (3) if a != ax]
			t_ = pos[..., other[0]] + pos[..., other[1]]
			dt = np.abs (t_ / sp - np.round (t_ / sp)) * sp
			for side in (-1, 1):
				dd = np.hypot (np.abs (d - off), dt)
				rivet_h = np.maximum (rivet_h, np.clip (1 - dd / r, 0, 1) * w * (np.sign (c - near) == side))
	# access panels
	for p in des.get ("panels", []):
		for sign in ((1, -1) if p.get ("mirror", True) else (p.get ("sign", 1),)):
			u, v, faces = plane_coords (b, p["plane"], sign, p["center"])
			w = faces & ev (p.get ("where"))
			cu = cv = 0.0; hu, hv = p["size"][0] / 2, p["size"][1] / 2
			sd = round_box (u, v, 0.0, 0.0, hu, hv, p.get ("radius", 0.05))
			if p.get ("fill"):
				inside = np.clip (0.5 - sd / px, 0, 1) * w
				fl = tr.srgb_to_lin (np.array (fin[p["fill"]]["color"], np.float32) / 255.0)
				lin = lin * (1 - inside[..., None]) + fl * inside[..., None]
				smooth = smooth * (1 - inside) + fin[p["fill"]].get ("smooth", 0.3) * inside
				metal = metal * (1 - inside) + fin[p["fill"]].get ("metal", 0.0) * inside
			seam = np.maximum (seam, cov (np.abs (sd), p.get ("width", 0.012)) * w)
			if p.get ("rivets"):
				sp = p["rivets"]
				per = np.arctan2 (v - cv, u - cu) * (hu + hv) / sp
				dt = np.abs (per - np.round (per)) * sp
				dd = np.hypot (np.abs (sd + p.get ("rivet_offset", 0.025)), dt)
				rivet_h = np.maximum (rivet_h, np.clip (1 - dd / 0.006, 0, 1) * w)
	# outlines of parts (doors, hatches, control surfaces): seams along the
	# groups' open edges, in 3D (they run on across texture islands)
	ol = des.get ("outlines")
	if ol:
		from scipy.spatial import cKDTree
		pts = []
		for g in m.groups:
			if not g.label or not any (g.label.startswith (p) or g.label.endswith (p) for p in ol["labels"]): continue
			key = lambda i: tuple (round (c, 4) for c in g.vtx[i][:3])
			cnt = {}
			for t in g.tri:
				for a_, b_ in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
					e = tuple (sorted ((key (a_), key (b_))))
					cnt[e] = cnt.get (e, 0) + 1
			for (p0, p1), c in cnt.items ():
				if c != 1: continue
				p0, p1 = np.array (p0), np.array (p1)
				k = max (2, int (np.linalg.norm (p1 - p0) / 0.004))
				pts.append (p0 + (p1 - p0) * np.linspace (0, 1, k)[:, None])
		if pts:
			tree = cKDTree (np.concatenate (pts))
			d, _ = tree.query (pos[used], distance_upper_bound=0.05)
			dd = np.full ((H, W), 1.0, np.float32); dd[used] = np.minimum (d, 1.0)
			seam = np.maximum (seam, cov (dd, ol.get ("width", 0.014)))
	lin = lin * (1 - seam[..., None] * (1 - des.get ("seam_shade", 0.6)))
	height -= seam * des.get ("seam_depth", 1.0)
	height += rivet_h * des.get ("rivet_height", 0.5)
	smooth = smooth - 0.1 * seam
	# weathering: soft variations, soot downstream of thrusters
	n_big = noise3 (pos, 1.5, 1) + 0.5 * noise3 (pos, 0.4, 2)
	lin = lin * (1 + des.get ("grime", 0.04) * n_big)[..., None]
	smooth = smooth * (1 + 0.15 * n_big)
	for s in des.get ("soot", []):
		sx, sy, sz = s["at"]
		for sign in ((1, -1) if s.get ("mirror", True) and abs (sx) > 1e-6 else (1,)):
			behind = (sz - z)
			lat = np.hypot (x - sx * sign, (y - sy) * 0.6)
			k = np.clip (behind / s.get ("length", 1.5), 0, 1)
			a = s.get ("strength", 0.35) * np.exp (-(lat / (s.get ("width", 0.12) * (1 + 2 * k))) ** 2) * (behind > -0.05) * np.exp (-behind / s.get ("length", 1.5))
			a *= 0.8 + 0.4 * noise3 (pos * np.array ([1, 1, 0.15]), 0.05, 7)
			lin = lin * (1 - np.clip (a, 0, 0.8))[..., None]
			smooth = smooth * (1 - 0.5 * np.clip (a, 0, 1))
	# surface grain in the height map
	n_fine = noise3 (pos, 0.01, 3)
	height += np.where (grain == "paint", 0.04 * n_fine, 0) + np.where (grain == "metal", 0.03 * noise3 (pos * np.array ([1, 1, 0.05]), 0.004, 4), 0) + np.where (grain == "rough", 0.12 * n_fine, 0)
	# decals: drawn on their plane, mirrored to the other side reading the
	# right way round
	alb = tr.lin_to_srgb (lin)
	for dcl in des.get ("decals", []):
		ppm = 2.0 / med
		img = symbol_image (dcl, ppm)
		hh, ww = img.shape[:2]
		for sign in ((1, -1) if dcl.get ("mirror", True) else (dcl.get ("sign", 1),)):
			du, dv, faces = plane_coords (b, dcl["plane"], sign, dcl["center"])
			w = faces & ev (dcl.get ("where"))
			a = math.radians (dcl.get ("rotate", 0) * (sign if dcl["plane"] != "side" else 1))
			lu = du * math.cos (a) + dv * math.sin (a)
			lv = -du * math.sin (a) + dv * math.cos (a)
			iu = (lu / dcl["size"][0] + 0.5) * ww - 0.5
			iv = (0.5 - lv / dcl["size"][1]) * hh - 0.5
			inside = w & (iu > -1) & (iu < ww) & (iv > -1) & (iv < hh)
			if not inside.any (): continue
			py, pxx = np.nonzero (inside)
			smp = np.stack ([ndimage.map_coordinates (img[..., c], [iv[py, pxx], iu[py, pxx]], order=1, mode="constant") for c in range (4)], -1)
			al = smp[:, 3:4]
			rgb = np.where (al > 1e-4, smp[:, :3] / np.maximum (al, 1e-4), 0)
			alb[py, pxx] = alb[py, pxx] * (1 - al) + rgb * al
			smooth[py, pxx] = smooth[py, pxx] * (1 - al[:, 0]) + dcl.get ("smooth", 0.35) * al[:, 0]
			metal[py, pxx] *= 1 - al[:, 0]
	# normal map from the height (millimetres) and the texel size
	k = des.get ("bump", 0.004) / px
	hy, hx = np.gradient (height)
	nm = np.stack ([-hx * k, -hy * k, np.ones_like (height)], -1)
	nm /= np.linalg.norm (nm, axis=-1, keepdims=True)
	return np.clip (alb, 0, 1), nm * 0.5 + 0.5, np.clip (smooth, 0, 1), np.clip (metal, 0, 1)

if __name__ == "__main__":
	ap = argparse.ArgumentParser ()
	ap.add_argument ("design"); ap.add_argument ("msh"); ap.add_argument ("texname"); ap.add_argument ("out")
	ap.add_argument ("--size", type=int, default=2048)
	a = ap.parse_args ()
	des = json.load (open (a.design))
	m = om.read_msh (a.msh)
	b = texbake.bake (m, a.texname, a.size, a.size)
	b["msh"] = m
	alb, nrm, smooth, metal = design (des, b)
	u8 = lambda x: Image.fromarray ((np.clip (x, 0, 1) * 255 + 0.5).astype (np.uint8))
	u8 (alb).save (a.out + ".png"); u8 (nrm).save (a.out + "_norm.png")
	u8 (smooth).save (a.out + "_rghn.png"); u8 (metal).save (a.out + "_metal.png")
	print ("%s: %d x %d, %.1f mm per texel" % (a.out, a.size, a.size, b["scale"] * 1000))
