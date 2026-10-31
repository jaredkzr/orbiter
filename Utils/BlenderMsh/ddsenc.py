# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Orbiter textures (DDS, with mip maps) from images (PNG, 8 or 16 bit),
# block-compressed with the rgbcx encoders of quicktex (Apache 2.0; a tool
# only, nothing of it is shipped). Run with the asset environment:
#   python3 -m venv out/assetvenv && out/assetvenv/bin/pip install quicktex numpy pillow
#   out/assetvenv/bin/python Utils/BlenderMsh/ddsenc.py <in.png> <out.dds> <kind>
# kinds (formats both clients read):
#   color   colour (sRGB): DXT1, or DXT5 if the image has alpha
#   gray    a value in all channels, e.g. <tex>_rghn (smoothness) and
#           <tex>_metal (metalness), which the clients read from green: DXT1
#   normal  tangent-space normals (x: +u, y: +v, as the clients take them;
#           a Blender bake has y up: flip with --flip-y): ATI2 (BC5)
# Mip maps are made in linear light (colour) or renormalised (normals).

import sys, struct, argparse
import numpy as np
from PIL import Image
import quicktex
from quicktex.s3tc import bc1, bc3, bc5

def load (path):
	"""float RGBA in 0..1, top row first"""
	im = Image.open (path)
	if im.mode in ("I;16", "I;16B", "I"):
		a = np.asarray (im, dtype=np.float32) / 65535.0
		a = np.stack ([a, a, a, np.ones_like (a)], -1)
	else:
		a = np.asarray (im.convert ("RGBA"), dtype=np.float32) / 255.0
	return a

def srgb_to_lin (c): return np.where (c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def lin_to_srgb (c): return np.where (c <= 0.0031308, c * 12.92, 1.055 * np.power (np.clip (c, 0, None), 1 / 2.4) - 0.055)

def mips (img, kind):
	"""the mip chain down to 4 x 4 (sizes: powers of two)"""
	h, w = img.shape[:2]
	if w & (w - 1) or h & (h - 1): raise ValueError ("size %d x %d: not powers of two" % (w, h))
	work = img.copy ()
	if kind == "color": work[..., :3] = srgb_to_lin (work[..., :3])
	if kind == "normal": work[..., :2] = work[..., :2] * 2.0 - 1.0
	levels = [work]
	while min (levels[-1].shape[:2]) > 4:
		p = levels[-1]
		q = 0.25 * (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2])
		if kind == "normal":
			# (renormalised: the average of unit vectors is shorter)
			z = np.sqrt (np.clip (1.0 - (p[..., 0] ** 2 + p[..., 1] ** 2), 0, 1))
			zq = 0.25 * (z[0::2, 0::2] + z[1::2, 0::2] + z[0::2, 1::2] + z[1::2, 1::2])
			l = np.sqrt (q[..., 0] ** 2 + q[..., 1] ** 2 + zq ** 2) + 1e-8
			q[..., 0] /= l; q[..., 1] /= l
		levels.append (q)
	out = []
	for lv in levels:
		e = lv.copy ()
		if kind == "color": e[..., :3] = lin_to_srgb (e[..., :3])
		if kind == "normal": e[..., :2] = e[..., :2] * 0.5 + 0.5
		out.append (np.clip (e, 0, 1))
	return out

def encode (levels, fmt):
	data = []
	for lv in levels:
		h, w = lv.shape[:2]
		raw = quicktex.RawTexture.frombytes (np.ascontiguousarray ((lv * 255.0 + 0.5).astype (np.uint8)).tobytes (), w, h)
		if fmt == "DXT1": enc = bc1.BC1Encoder (18, bc1.BC1Encoder.ColorMode.FourColor)
		elif fmt == "DXT5": enc = bc3.BC3Encoder (18)
		else: enc = bc5.BC5Encoder (0, 1)
		data.append (enc.encode (raw).tobytes ())
	return data

def write (path, w, h, fmt, data):
	DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000   # caps, height, width, pixel format, mip count, linear size
	hdr = struct.pack ("<4s7I44s2I4s5I5I", b"DDS ", 124, DDSD, h, w, len (data[0]), 0, len (data), b"\0" * 44,
		32, 0x4, fmt.encode (), 0, 0, 0, 0, 0,
		0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
	with open (path, "wb") as f:
		f.write (hdr)
		for d in data: f.write (d)

def convert (src, dst, kind, flip_y=False):
	img = load (src)
	if kind == "gray":
		g = img[..., 1:2] if img.shape[2] > 1 else img[..., :1]
		img = np.concatenate ([g, g, g, np.ones_like (g)], -1)
	if kind == "normal" and flip_y: img[..., 1] = 1.0 - img[..., 1]
	fmt = "ATI2" if kind == "normal" else "DXT5" if kind == "color" and (img[..., 3] < 0.996).any () else "DXT1"
	levels = mips (img, kind)
	write (dst, img.shape[1], img.shape[0], fmt, encode (levels, fmt))
	return fmt

if __name__ == "__main__":
	ap = argparse.ArgumentParser (description="image to Orbiter DDS texture")
	ap.add_argument ("src"); ap.add_argument ("dst")
	ap.add_argument ("kind", choices=["color", "gray", "normal"])
	ap.add_argument ("--flip-y", action="store_true", help="normal maps baked with y up (Blender)")
	a = ap.parse_args ()
	print ("%s: %s" % (a.dst, convert (a.src, a.dst, a.kind, a.flip_y)))
