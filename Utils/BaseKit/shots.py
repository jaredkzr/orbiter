#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""Screenshots of the showcase bases, for before/after comparisons of their
assets (the base-asset overhaul).

    shots.py scenes
        writes Scenarios/VkTest/base_<shot>.scn from SHOTS (a delta-glider
        landed on a pad, a ground camera looking at the base by day, by
        night, or at a close-up)
    shots.py render [--label L] [--only TEXT] [--build DIR] [--jobs N]
                    [--frame N] [--validate] [--env K=V ...]
        renders them (offscreen, the Vulkan client, fixed steps) into
        out/baseshots/<L>/<shot>.png; --validate: the validation layer on,
        its messages counted (out/baseshots/<L>/summary.txt); --env: extra
        environment, e.g. ORBITER_VK_BASE_BUILDINGS=0 for an A/B
    shots.py compare <label A> <label B> [--only TEXT]
        side-by-side sheets out/baseshots/compare_<A>_vs_<B>/<shot>.png

The times of day (MJD) were found from the sun's elevation at each base
(about 40 degrees by day; the lowest by night).
"""

import argparse, math, os, re, subprocess, sys, concurrent.futures

ROOT = os.path.normpath (os.path.join (os.path.dirname (os.path.abspath (__file__)), '..', '..'))
SCN_DIR = os.path.join (ROOT, 'Scenarios', 'VkTest')
OUT = os.path.join (ROOT, 'out', 'baseshots')
RADIUS = {'Earth': 6.37101e6, 'Moon': 1.738e6, 'Mars': 3.39e6, 'Venus': 6.0518e6}

# base: body, location (lng, lat [deg]), the pad the glider stands on, MJD by day and by night (None: none)
BASES = {
	'Brighton':   ('Moon', 'Brighton Beach', (-33.4375, 41.125), 1, 52012.50, 51995.50),
	'Shackleton': ('Moon', 'Shackleton', (-137.31, -89.45), 1, 51989.25, None),  # (the polar ridge: the sun at the horizon, always up)
	'Olympus':    ('Mars', 'Olympus', (-135.43, 12.74), 1, 51986.42, 51987.08),
	'Jezero':     ('Mars', 'Jezero', (77.4393, 18.4392), 1, 51985.08, 51986.48),
	'Canaveral':  ('Earth', 'Cape Canaveral', (-80.65165, 28.58438), 1, 51985.85, 51985.23),
	'Edwards':    ('Earth', 'Edwards', (-117.889999, 34.919998), 1, 51985.73, 51985.33),
	'Baikonur':   ('Earth', 'Baikonur', (63.330002, 45.919998), 1, 51985.28, 51985.83),
	'Jiuquan':    ('Earth', 'Jiuquan', (100.2915, 40.9581), 1, 51985.31, 51985.73),
	'Habana':     ('Earth', 'Habana', (-82.40, 23.00), 1, 51985.70, 51985.20),
	'Aphrodite':  ('Venus', 'Aphrodite', (105.00, -5.00), 1, 51983.25, 52040.0),  # (the floating base, 55 km up: its deck the ground)
}

# shot: base, camera (east, north [m] from the base's origin, height [m] above the ground), the point looked at
# (east, north, up [m]; None: the glider), field of view [deg], times ('day', 'night')
SHOTS = {
	'Brighton':        ('Brighton', (520, -560, 230), (-120, 120, 0), 55, ('day', 'night')),
	'Brighton_hab':    ('Brighton', (-330, 190, 18), (-400, 300, 4), 60, ('day', 'night')),
	'Brighton_habclose': ('Brighton', (-335, 226, 6), (-367.7, 255.5, 3), 55, ('day',)),
	'Brighton_hub':    ('Brighton', (75, -95, 22), (0, 0, 6), 60, ('day', 'night')),
	'Edwards_close':   ('Edwards', (300, 700, 40), (92, 945, 8), 55, ('day', 'night')),
	'Shackleton':      ('Shackleton', (450, -450, 200), (0, 0, 0), 55, ('day',)),
	'Olympus':         ('Olympus', (450, -450, 200), (0, 0, 0), 55, ('day', 'night')),
	'Jezero':          ('Jezero', (450, -450, 200), (0, 0, 0), 55, ('day', 'night')),
	'Canaveral':       ('Canaveral', (900, -900, 300), (0, 0, 40), 55, ('day', 'night')),
	'Canaveral_vab':   ('Canaveral', (380, -330, 25), (0, 0, 70), 60, ('day', 'night')),
	'Canaveral_vabeast': ('Canaveral', (330, 40, 30), (150, 170, 75), 60, ('day',)),   # (its east face: the high bays' doors)
	'Canaveral_vabwall': ('Canaveral', (150, -75, 12), (95, 20, 45), 60, ('day',)),      # (its siding, close)
	'Canaveral_39bramp': ('Canaveral', (3001 + 75, 4759 - 330, 28), (3001, 4759 - 140, 6), 60, ('day',)),  # (the crawlerway's ramp)
	'Canaveral_39b':   ('Canaveral', (3001 + 380, 4759 - 380, 40), (3001, 4759, 40), 60, ('day', 'night')),
	'Canaveral_39btower': ('Canaveral', (3001 - 215 - 95, 4759 + 40 - 135, 22), (3001 - 215, 4759 + 40, 48), 50, ('day',)),  # (its water tower)
	'Canaveral_39bpad': ('Canaveral', (3001 + 75, 4759 - 95, 38), (3003, 4757, 26), 60, ('day',)),
	'Canaveral_39a':   ('Canaveral', (4635 + 330, 2659 - 300, 45), (4635, 2659, 45), 60, ('day', 'night')),
	'Canaveral_39apad': ('Canaveral', (4635 + 95, 2659 - 40, 30), (4615, 2659, 40), 60, ('day',)),
	'Edwards':         ('Edwards', (800, 250, 250), (92, 945, 0), 55, ('day', 'night')),
	'Baikonur':        ('Baikonur', (-1300, 4300, 250), (-2000, 5000, 0), 55, ('day', 'night')),
	'Baikonur_close':  ('Baikonur', (-1700, 4600, 60), (-2050, 5080, 5), 55, ('day',)),
	'Jiuquan':         ('Jiuquan', (700, -700, 250), (0, 0, 0), 55, ('day', 'night')),
	# pads seen from above (roads and aprons about them)
	'Brighton_pad3':   ('Brighton', (-173, 30, 150), (-173, 100, 0), 60, ('day', 'night')),
	'Brighton_pad5':   ('Brighton', (173, 30, 150), (173, 100, 0), 60, ('day',)),
	'Jiuquan_pad1':    ('Jiuquan', (0, -70, 150), (0, 0, 0), 60, ('day',)),
	'Jiuquan_pad3':    ('Jiuquan', (-616, -1189, 150), (-616, -1119, 0), 60, ('day', 'night')),
	'Brighton_train':  ('Brighton', (-200, 330, 35), (-400, 700, 10), 60, ('day', 'night')),
	'Brighton_trainside': ('Brighton', (-626, 1165, 12), (-661, 1145, 8), 60, ('day',)),  # (a car there at frame 400)
	'Habana_mono':     ('Habana', (420, 420, 8), (380, 300, 1), 60, ('day',)),
	'Habana_train':    ('Habana', (700, 250, 45), (380, 0, 5), 60, ('day',)),
	# (off its deck the ground is the planet's, 55 km down: heights over that)
	'Aphrodite':       ('Aphrodite', (220, -220, 55090), (0, 0, 55000), 55, ('day', 'night')),
	'Aphrodite_deck':  ('Aphrodite', (60, -45, 6), (-20, 30, 4), 60, ('day',)),
	'Aphrodite_below': ('Aphrodite', (260, -200, 54940), (0, 0, 54930), 55, ('day',)),
	'Edwards_top':     ('Edwards', (92, 700, 350), (92, 945, 0), 60, ('night',)),
	'Jiuquan_roof':    ('Jiuquan', (-234 + 70, -1221 - 70, 95), (-234, -1221, 40), 55, ('day',)),
	'Jiuquan_campus':  ('Jiuquan', (-350, -1350, 60), (-616, -1119, 0), 60, ('day',)),
}


def shot_names (only=None):
	out = []
	for s, (b, cam, look, fov, times) in SHOTS.items ():
		for t in times:
			n = 'base_%s_%s' % (s, t)
			if not only or only.lower () in n.lower (): out.append (n)
	return out


def scene_text (shot, t):
	b, cam, look, fov, times = SHOTS[shot]
	body, name, (lng0, lat0), pad, mday, mnight = BASES[b]
	R = RADIUS[body]
	mjd = mday if t == 'day' else mnight
	lat = lat0 + math.degrees (cam[1] / R)
	lng = lng0 + math.degrees (cam[0] / (R * math.cos (math.radians (lat0))))
	if look:
		de, dn, du = look[0] - cam[0], look[1] - cam[1], look[2] - cam[2]
		# (the ground observer's direction (Camera.cpp, its frame x south, y up, z east):
		# phi from east towards north, theta up)
		phi = math.degrees (math.atan2 (dn, de))
		tht = math.degrees (math.atan2 (du, math.hypot (de, dn)))
		direction = '  GROUNDDIRECTION %.2f %.2f\n' % (phi, tht)
	else:
		direction = ''  # (locked on the glider)
	return '''BEGIN_DESC
%s base (%s): its assets by %s (Utils/BaseKit/shots.py)
END_DESC

BEGIN_ENVIRONMENT
  System Sol
  Date MJD %.4f
END_ENVIRONMENT

BEGIN_FOCUS
  Ship V
END_FOCUS

BEGIN_CAMERA
  TARGET V
  MODE Extern
  TRACKMODE Ground %s
  GROUNDLOCATION %.7f %.7f %.1f
%s  FOV %.2f
END_CAMERA

BEGIN_SHIPS
V:DeltaGlider
  STATUS Landed %s
  BASE %s:%d
  HEADING 0
  GEAR 1 1.0000
END
END_SHIPS
''' % (name, shot, 'night' if t == 'night' else 'day', mjd, body, lng, lat, cam[2], direction, fov, body, name, pad)


def cmd_scenes ():
	for s, v in SHOTS.items ():
		for t in v[4]:
			path = os.path.join (SCN_DIR, 'base_%s_%s.scn' % (s, t))
			open (path, 'w').write (scene_text (s, t))
			print (path)


def render_one (scn, build, outdir, frame, validate, extra_env):
	png = os.path.join (outdir, scn + '.png')
	log = os.path.join (outdir, scn + '.log')
	env = dict (os.environ, ORBITER_NO_SAVE='1', ORBITER_VK_OFFSCREEN='1', ORBITER_VK_SCREENSHOT=png,
		ORBITER_VK_SCREENSHOT_FRAME=str (frame), ORBITER_VK_SCREENSHOT_EXIT='1', ORBITER_VK_SCREENSHOT_NOUI='1',
		ORBITER_VK_FRAMETIME='0.016667')
	if validate: env['ORBITER_VK_VALIDATION'] = '1'
	env.update (extra_env)
	cmd = [os.path.join (build, 'Orbiter'), '-p', 'VulkanClient', '--fixedstep=0.016667',
		'--scenariox=' + os.path.join (SCN_DIR, scn + '.scn')]
	with open (log, 'w') as f:
		r = subprocess.run (cmd, cwd=build, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=600)
	txt = open (log, errors='replace').read ()
	vuid = len (re.findall (r'VUID-|Validation Error', txt))
	return scn, r.returncode, os.path.exists (png), vuid


def cmd_render (a):
	label = a.label
	outdir = os.path.join (OUT, label)
	os.makedirs (outdir, exist_ok=True)
	extra = dict (kv.split ('=', 1) for kv in (a.env or []))
	names = shot_names (a.only)
	rows = []
	with concurrent.futures.ThreadPoolExecutor (max_workers=a.jobs) as ex:
		futs = [ex.submit (render_one, n, a.build, outdir, a.frame, a.validate, extra) for n in names]
		for f in concurrent.futures.as_completed (futs):
			scn, rc, ok, vuid = f.result ()
			rows.append ('%-32s exit %d %s validation %d' % (scn, rc, 'png' if ok else 'NO PNG', vuid))
			print (rows[-1], flush=True)
	open (os.path.join (outdir, 'summary.txt'), 'w').write ('\n'.join (sorted (rows)) + '\n')


def cmd_compare (a):
	from PIL import Image, ImageDraw, ImageFont
	try: font = ImageFont.truetype ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 26)
	except OSError: font = ImageFont.load_default ()
	dst = os.path.join (OUT, 'compare_%s_vs_%s' % (a.a, a.b))
	os.makedirs (dst, exist_ok=True)
	for n in shot_names (a.only):
		pa, pb = os.path.join (OUT, a.a, n + '.png'), os.path.join (OUT, a.b, n + '.png')
		if not (os.path.exists (pa) and os.path.exists (pb)): continue
		ia, ib = Image.open (pa).convert ('RGB'), Image.open (pb).convert ('RGB')
		w, h = ia.width // 2, ia.height // 2
		sheet = Image.new ('RGB', (2 * w, h), 'black')
		sheet.paste (ia.resize ((w, h)), (0, 0)); sheet.paste (ib.resize ((w, h)), (w, 0))
		d = ImageDraw.Draw (sheet)
		for x, lab in ((0, a.a), (w, a.b)):
			d.rectangle ((x, 0, x + 12 + 15 * len (lab), 36), fill='black'); d.text ((x + 6, 4), lab, fill='white', font=font)
		sheet.save (os.path.join (dst, n + '.png'))
		print (os.path.join (dst, n + '.png'))


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	sub = ap.add_subparsers (dest='cmd', required=True)
	sub.add_parser ('scenes')
	r = sub.add_parser ('render')
	r.add_argument ('--label', default='current')
	r.add_argument ('--only')
	r.add_argument ('--build', default=os.path.join (ROOT, 'out', 'build', 'linux-x64-headless-release'))
	r.add_argument ('--jobs', type=int, default=3)
	r.add_argument ('--frame', type=int, default=400)
	r.add_argument ('--validate', action='store_true')
	r.add_argument ('--env', action='append')
	c = sub.add_parser ('compare')
	c.add_argument ('a'); c.add_argument ('b'); c.add_argument ('--only')
	a = ap.parse_args ()
	if a.cmd == 'scenes': cmd_scenes ()
	elif a.cmd == 'render': cmd_render (a)
	else: cmd_compare (a)


if __name__ == '__main__':
	main ()
