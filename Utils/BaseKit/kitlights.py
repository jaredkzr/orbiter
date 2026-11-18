#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""The base kits' lights (aviation reds on masts, lamps over doors: recorded
per asset by blender_kit.py in Utils/BaseKit/kit_lights.json) placed where
the bases' files put the assets (MESH ... FILE BaseKit\\<asset>, POS, ROT,
SCALE), as lines of the bases' Vulkan client files (Config/VK/Bases/<Name>.cfg,
between ; BEGIN kitlights and ; END kitlights):

    LIGHT x h z  r g b  size      base frame (x south, z east [m]); h above the ground
    LIGHTABS x y z  r g b  size   base frame (a floating base's: its objects about its origin)

    kitlights.py [--root DIR]
"""

import argparse, glob, json, math, os, re

HERE = os.path.dirname (os.path.abspath (__file__))
ROOT = os.path.normpath (os.path.join (HERE, '..', '..'))
CFG_GLOBS = ['Src/Celbody/*/Config/*/Base/*.cfg', 'Src/Celbody/*/*/Config/*/Base/*.cfg']

HEADER = '''; Lights of %s for the Vulkan client (base frame [m]: x south, y up, z east)
'''


def read_base (path):
	t = open (path, errors='replace').read ().replace ('\r\n', '\n')
	m = re.search (r'^\s*Name\s*=\s*(.+?)\s*$', t, re.M | re.I)
	if not m: return None
	name = m.group (1)
	floating = re.search (r'^\s*Altitude\s*=', t, re.M | re.I) is not None
	objs = []
	for b in re.finditer (r'^\s*MESH\s*\n(.*?)^\s*END\s*$', t, re.M | re.S):
		body = b.group (1)
		f = re.search (r'^\s*FILE\s+BaseKit[\\/](\S+)', body, re.M | re.I)
		if not f: continue
		pos = re.search (r'^\s*POS\s+(\S+)\s+(\S+)\s+(\S+)', body, re.M | re.I)
		rot = re.search (r'^\s*ROT\s+(\S+)', body, re.M | re.I)
		sc = re.search (r'^\s*SCALE\s+(\S+)(?:\s+(\S+)\s+(\S+))?', body, re.M | re.I)
		p = tuple (float (pos.group (i)) for i in (1, 2, 3)) if pos else (0.0, 0.0, 0.0)
		r = float (rot.group (1)) if rot else 0.0
		s = (1.0, 1.0, 1.0)
		if sc: s = (float (sc.group (1)),) * 3 if sc.group (2) is None else tuple (float (sc.group (i)) for i in (1, 2, 3))
		objs.append ((f.group (1).lower (), p, r, s))
	return name, floating, objs


def place (light, pos, rot_deg, sc):
	(x, y, z), col, size = light
	x, y, z = x * sc[0], y * sc[1], z * sc[2]
	a = math.radians (rot_deg)
	X = math.cos (a) * x - math.sin (a) * z  # (Mesh::Rotate ROTATE_Y)
	Z = math.sin (a) * x + math.cos (a) * z
	return (pos[0] + X, pos[1] + y, pos[2] + Z), col, size


def update (path, name, lines):
	text = open (path).read () if os.path.exists (path) else HEADER % name
	block = '; BEGIN kitlights\n; (made by Utils/BaseKit/kitlights.py: the kit\'s lights where the base puts its assets)\n' + ''.join (lines) + '; END kitlights\n'
	a = text.find ('; BEGIN kitlights')
	if a >= 0:
		b = text.find ('; END kitlights', a)
		b = text.find ('\n', b) + 1
		text = text[:a] + block + text[b:]
	else:
		text = text.rstrip ('\n') + '\n' + block
	os.makedirs (os.path.dirname (path), exist_ok=True)
	open (path, 'w').write (text)


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--root', default=ROOT)
	a = ap.parse_args ()
	kit = json.load (open (os.path.join (HERE, 'kit_lights.json')))
	kit = {k.lower (): v for k, v in kit.items ()}
	done = {}
	for g in CFG_GLOBS:
		for path in sorted (glob.glob (os.path.join (a.root, g))):
			if os.sep + 'SPICE' + os.sep in path: continue  # (the SPICE copies: the same bases)
			r = read_base (path)
			if not r: continue
			name, floating, objs = r
			lines = []
			for asset, pos, rot, sc in objs:
				for l in kit.get (asset, []):
					(x, y, z), col, size = place (l, pos, rot, sc)
					lines.append ('%s %.2f %.2f %.2f  %.2f %.2f %.2f  %.2f\n' % ('LIGHTABS' if floating else 'LIGHT', x, y, z, col[0], col[1], col[2], size))
			if lines or name in done:
				done[name] = len (lines)
				update (os.path.join (a.root, 'Config', 'VK', 'Bases', name.replace (' ', '_') + '.cfg'), name, lines)
	for n, k in sorted (done.items ()): print ('%s: %d lights' % (n, k))


if __name__ == '__main__':
	main ()
