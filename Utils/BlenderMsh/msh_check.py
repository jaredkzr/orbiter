# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Checks of a new Orbiter mesh, before it replaces a stock one:
#   python3 Utils/BlenderMsh/msh_check.py <new.msh> [--ref <old.msh>] [--code <vessel source dir>]
#                                         [--textures <texture dir>] [--budget <triangles>]
#                                         [--bounds-ref <old.msh> | --bounds-json <footprints.json>] [--tol <m>]
# - group labels the vessel code uses (meshc makes GRP_<label> constants of
#   them) are all there; with --ref, labels of the old mesh gone are listed
# - material and texture lists at least as long as the old ones (skins and
#   oapiSetMaterial refer to them by index)
# - groups under 65536 vertices (16-bit indices), triangles within budget
# - textures found, DDS, sizes powers of two; their maps (_norm, _rghn,
#   _metal, _emis, _night) listed
# - with --bounds-ref (or --bounds-json, a snapshot by mesh file name: base
#   meshes whose footprint layouts rely on): the footprint (x, z bounds) the
#   same within --tol (0.5 m), the height within the larger of tol and 10 %
#   (bounds over the drawn groups: hidden ones, collision proxies and detail
#   left out)
# Exit status 1 if anything is wrong.

import os, re, sys, struct, argparse
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import orbiter_msh as om

def code_labels (srcdir):
	"""GRP_<name> constants used by the sources (meshres*.h names)"""
	used = set ()
	for root, _, files in os.walk (srcdir):
		for f in files:
			if f.endswith ((".cpp", ".h")) and not f.startswith ("meshres"):
				with open (os.path.join (root, f), encoding="latin-1") as fh:
					used |= set (re.findall (r"\bGRP_(\w+)", fh.read ()))
	return used

def dds_info (path):
	with open (path, "rb") as f: h = f.read (128)
	if h[:4] != b"DDS ": return None
	hh, ww = struct.unpack ("<II", h[12:20])
	return ww, hh, h[84:88].decode ("latin-1").strip ("\0") or "raw"

def find (texdir, name):
	p = os.path.join (texdir, *name.replace ("\\", "/").split ("/"))
	if os.path.exists (p): return p
	cur = texdir
	for part in os.path.relpath (p, texdir).split (os.sep):
		hit = [e for e in os.listdir (cur) if e.lower () == part.lower ()] if os.path.isdir (cur) else []
		if not hit: return None
		cur = os.path.join (cur, hit[0])
	return cur

def bounds (m):
	"""x, y, z minima and maxima over the drawn groups (not hidden: user flag
	0x2), without detail (MESHGRP_NOCOLLIDE 0x100: masts, rails, antennas)"""
	lo, hi = [1e30] * 3, [-1e30] * 3
	for g in m.groups:
		if g.flag & 0x102: continue
		for v in g.vtx:
			for i in range (3): lo[i] = min (lo[i], v[i]); hi[i] = max (hi[i], v[i])
	return lo, hi

def check_bounds (m, ref, tol, errors, notes):
	(lo, hi), (rlo, rhi) = bounds (m), ref
	notes.append ("bounds x %.2f..%.2f z %.2f..%.2f height %.2f (reference x %.2f..%.2f z %.2f..%.2f height %.2f)" % (
		lo[0], hi[0], lo[2], hi[2], hi[1], rlo[0], rhi[0], rlo[2], rhi[2], rhi[1]))
	for i, ax in ((0, "x"), (2, "z")):
		if abs (lo[i] - rlo[i]) > tol or abs (hi[i] - rhi[i]) > tol:
			errors.append ("footprint %s %.2f..%.2f, reference %.2f..%.2f (tolerance %.2f m)" % (ax, lo[i], hi[i], rlo[i], rhi[i], tol))
	if abs (hi[1] - rhi[1]) > max (tol, 0.1 * abs (rhi[1])):
		errors.append ("height %.2f, reference %.2f" % (hi[1], rhi[1]))

def main ():
	ap = argparse.ArgumentParser ()
	ap.add_argument ("mesh")
	ap.add_argument ("--ref")
	ap.add_argument ("--code")
	ap.add_argument ("--textures")
	ap.add_argument ("--budget", type=int, default=0)
	ap.add_argument ("--bounds-ref")
	ap.add_argument ("--bounds-json")
	ap.add_argument ("--tol", type=float, default=0.5)
	a = ap.parse_args ()
	m = om.read_msh (a.mesh)
	errors, notes = [], []
	labels = {g.label for g in m.groups if g.label}
	ntri = sum (len (g.tri) for g in m.groups)
	notes.append ("%d groups, %d triangles, %d materials, %d textures" % (len (m.groups), ntri, len (m.materials), len (m.textures)))
	if a.budget and ntri > a.budget: errors.append ("%d triangles, budget %d" % (ntri, a.budget))
	for k, g in enumerate (m.groups):
		if len (g.vtx) > 65535: errors.append ("group %d (%s): %d vertices (16-bit indices)" % (k, g.label, len (g.vtx)))
		if not (-1 <= g.mat < len (m.materials)): errors.append ("group %d (%s): material %d of %d" % (k, g.label, g.mat + 1, len (m.materials)))
		if not (-1 <= g.tex < len (m.textures)): errors.append ("group %d (%s): texture %d of %d" % (k, g.label, g.tex + 1, len (m.textures)))
	if len (labels) != sum (1 for g in m.groups if g.label): errors.append ("labels not unique")
	if a.code:
		missing = sorted (l for l in code_labels (a.code) if l not in labels)
		# (constants of the other meshes of the vessel: VC, panels)
		if a.ref:
			ref_labels = {g.label for g in om.read_msh (a.ref).groups if g.label}
			missing = [l for l in missing if l in ref_labels]
		for l in missing: errors.append ("label %s used by the code (GRP_%s) is missing" % (l, l))
	if a.ref:
		r = om.read_msh (a.ref)
		gone = sorted ({g.label for g in r.groups if g.label} - labels)
		if gone: notes.append ("labels of the old mesh gone: " + ", ".join (gone))
		if len (m.materials) < len (r.materials): errors.append ("%d materials, the old mesh had %d (indices used by code)" % (len (m.materials), len (r.materials)))
		if len (m.textures) < len (r.textures): errors.append ("%d textures, the old mesh had %d (skins replace them by index)" % (len (m.textures), len (r.textures)))
		notes.append ("old mesh: %d groups, %d triangles" % (len (r.groups), sum (len (g.tri) for g in r.groups)))
	if a.bounds_ref:
		check_bounds (m, bounds (om.read_msh (a.bounds_ref)), a.tol, errors, notes)
	elif a.bounds_json:
		import json
		key = os.path.splitext (os.path.basename (a.mesh))[0]
		fp = json.load (open (a.bounds_json)).get (key)
		if fp is None: errors.append ("%s: no footprint in %s" % (key, a.bounds_json))
		else: check_bounds (m, (fp["lo"], fp["hi"]), a.tol, errors, notes)
	if a.textures:
		for name, flag in m.textures:
			if name == "0": continue
			p = find (a.textures, name)
			if not p: errors.append ("texture %s not found" % name); continue
			info = dds_info (p)
			if not info: errors.append ("texture %s: not DDS" % name); continue
			w, h, fmt = info
			if w & (w - 1) or h & (h - 1): errors.append ("texture %s: %d x %d, not powers of two" % (name, w, h))
			stem, ext = os.path.splitext (p)
			maps = [s for s in ("norm", "rghn", "metal", "emis", "night", "detail") if os.path.exists ("%s_%s%s" % (stem, s, ext))]
			notes.append ("texture %s: %d x %d %s%s" % (name, w, h, fmt, (", maps " + " ".join (maps)) if maps else ""))
	for n in notes: print ("  " + n)
	for e in errors: print ("ERROR " + e)
	return 1 if errors else 0

if __name__ == "__main__":
	sys.exit (main ())
