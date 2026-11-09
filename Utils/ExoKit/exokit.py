#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License
"""ExoKit: the exoplanets of Orbiter's neighbouring systems.

Reads the NASA Exoplanet Archive's Planetary Systems Composite Parameters
(pscomppars.csv, cached here; --fetch downloads it again) and writes a
planet configuration file for each planet of the systems in Config/Systems,
plus the PlanetN lines of the system files.

Measured values are used as they are. What is not known is filled in and
marked "assumed" in the files:
  - the orbits' orientation on the sky (node), and the inclination of
    planets found by radial velocity (60 deg, the median of random
    orientations; their masses are M sin i / sin 60)
  - radii of planets found by radial velocity (rocky: R = M^0.27)
  - tidal locking (the M dwarfs' planets)
  - the surfaces, atmospheres and looks (SurfaceClass and so on)
"""

import argparse
import csv
import math
import os
import re
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname (os.path.abspath (__file__))
ROOT = os.path.normpath (os.path.join (HERE, '..', '..'))
CSV = os.path.join (HERE, 'pscomppars.csv')

COLUMNS = ('pl_name,hostname,pl_orbper,pl_orbsmax,pl_orbeccen,pl_orbincl,pl_orblper,pl_orbtper,pl_tranmid,'
	'pl_bmasse,pl_bmassprov,pl_rade,pl_eqt,pl_insol,st_mass,st_rad,st_teff,ra,dec,sy_dist,disc_year,'
	'discoverymethod,pl_orbper_reflink,pl_bmasse_reflink')

ME = 5.9722e24   # [kg]
RE = 6.3710e6    # [m]

# the archive's hosts: system file, the star's body (its Name), planet files' directory
HOSTS = {
	'Proxima Cen':    ('AlphaCentauri',  'Proxima',        'AlphaCentauri'),
	"Barnard's star": ('Barnard',        'Barnard',        'Barnard'),
	'eps Eri':        ('EpsilonEridani', 'EpsilonEridani', 'EpsilonEridani'),
	'TRAPPIST-1':     ('TRAPPIST-1',     'TRAPPIST-1',     'TRAPPIST-1'),
}

# Assumed: the planets' surfaces and looks. class, colours (low, high),
# relief, mountains, sea, craters, open water [deg], atmosphere (bar or 0;
# N2, or (bar, CO2 mole fraction) for an N2/CO2 mix).
# JWST: TRAPPIST-1 b and c are dark bare rock (Greene et al. 2023, Zieba
# et al. 2023), d has no Earth-like atmosphere (Piaulet-Ghorayeb et al. 2025).
# Open water needs an atmosphere: below water's triple point (611 Pa) it
# boils off and freezes. TRAPPIST-1 f (equilibrium 218 K) keeps a sea about
# the substellar point only under a thick greenhouse atmosphere, so its
# eyeball sea comes with an assumed 2 bar of N2 and CO2.
LOOKS = {
	'TRAPPIST-1 b': ('Rock',  (0.09, 0.085, 0.08), (0.17, 0.155, 0.14), 1500, 900, 0.0, 0.7, 0, 0),
	'TRAPPIST-1 c': ('Rock',  (0.15, 0.13, 0.12), (0.27, 0.24, 0.21), 2000, 1200, 0.0, 0.6, 0, 0),
	'TRAPPIST-1 d': ('Rock',  (0.24, 0.16, 0.11), (0.40, 0.30, 0.21), 2500, 1500, 0.0, 0.4, 0, 0),
	'TRAPPIST-1 e': ('Ocean', (0.22, 0.19, 0.15), (0.38, 0.35, 0.31), 3000, 2500, 0.65, 0.1, 38, 1.0),
	'TRAPPIST-1 f': ('Ice',   (0.70, 0.74, 0.80), (0.88, 0.90, 0.93), 1200, 600, 0.0, 0.3, 14, (2.0, 0.5)),
	'TRAPPIST-1 g': ('Ice',   (0.74, 0.78, 0.84), (0.90, 0.92, 0.95), 1000, 500, 0.0, 0.3, 0, 0),
	'TRAPPIST-1 h': ('Ice',   (0.62, 0.58, 0.54), (0.82, 0.79, 0.75), 900, 400, 0.0, 0.5, 0, 0),
	'Proxima Cen b': ('Ocean', (0.24, 0.20, 0.16), (0.40, 0.36, 0.30), 3500, 3000, 0.55, 0.1, 32, 1.0),
	'Proxima Cen d': ('Rock', (0.13, 0.12, 0.11), (0.29, 0.27, 0.24), 1800, 1000, 0.0, 0.9, 0, 0),
	'Barnard b':     ('Rock', (0.16, 0.15, 0.14), (0.30, 0.28, 0.26), 1600, 900, 0.0, 0.8, 0, 0),
	'Barnard c':     ('Rock', (0.20, 0.15, 0.11), (0.34, 0.27, 0.20), 2000, 1200, 0.0, 0.6, 0, 0),
	'Barnard d':     ('Rock', (0.10, 0.08, 0.07), (0.22, 0.17, 0.14), 1400, 800, 0.0, 0.7, 0, 0),
	'Barnard e':     ('Rock', (0.18, 0.17, 0.16), (0.33, 0.31, 0.29), 1800, 1100, 0.0, 0.9, 0, 0),
	'eps Eri b':     ('Gas',  (0.62, 0.58, 0.52), (0.86, 0.84, 0.79), 0, 0, 0.0, 0.0, 0, 0),
}

def fetch ():
	hosts = ','.join ("'" + h.replace ("'", "''") + "'" for h in HOSTS)
	q = 'select %s from pscomppars where hostname in (%s)' % (COLUMNS, hosts)
	url = 'https://exoplanetarchive.ipac.caltech.edu/TAP/sync?' + urllib.parse.urlencode ({'query': q, 'format': 'csv'})
	data = urllib.request.urlopen (url, timeout=120).read ()
	with open (CSV, 'wb') as f: f.write (data)
	print ('fetched %s' % CSV)

def num (s, default=None):
	try: return float (s)
	except (TypeError, ValueError): return default

def ref (s):
	s = re.sub ('<[^>]*>', '', s or '')
	return (s.replace ('&aacute;', 'a').replace ('&ntilde;', 'n').replace ('&uuml;', 'u').replace ('&eacute;', 'e')).strip ()

def planet_name (pl):
	# Orbiter's names: the host's body name and the letter
	host = pl['hostname']
	letter = pl['pl_name'].split ()[-1]
	return {'Proxima Cen': 'Proxima', "Barnard's star": 'Barnard', 'eps Eri': 'Epsilon Eridani',
		'TRAPPIST-1': 'TRAPPIST-1'}[host] + ' ' + letter

def file_stem (name):
	return re.sub ('[^A-Za-z0-9-]', '', name)

def seed_of (name):
	h = 2166136261
	for ch in name.encode ():
		h = ((h ^ ch) * 16777619) & 0x7fffffff
	return h

def write_planet (pl, outdir):
	name = planet_name (pl)
	look = LOOKS[pl['pl_name']]
	cls, clow, chigh, relief, mountains, sea, craters, openw, atm = look
	atm, co2 = atm if isinstance (atm, tuple) else (atm, 0.0)
	assert not (openw and not atm and cls != 'Ocean'), name + ': open water needs an atmosphere'
	P = num (pl['pl_orbper'])
	e = num (pl['pl_orbeccen'], 0.0)
	w = num (pl['pl_orblper'], 90.0)
	incl = num (pl['pl_orbincl'])
	notes = []
	rv = pl['discoverymethod'] != 'Transit'
	if incl is None:
		incl = 60.0
		notes.append ('i assumed (60 deg: the median of random orientations)')
	node = 0.0
	notes.append ('node assumed')
	m = num (pl['pl_bmasse'])
	if pl['pl_bmassprov'] == 'Msini':
		m /= math.sin (math.radians (incl))
		notes.append ('mass M sin i / sin i (i assumed)')
	r = num (pl['pl_rade'])
	if rv and cls != 'Gas':
		r = m ** 0.27
		notes.append ('radius assumed (rocky: M^0.27)')
	elif rv:
		notes.append ('radius from the archive (a mass-radius relation)')
	# epoch: a transit (or inferior conjunction), else a periastron passage
	if num (pl['pl_tranmid']) is not None:
		T, epoch = num (pl['pl_tranmid']), 'Conjunction'
	else:
		T, epoch = num (pl['pl_orbtper']), 'Periapsis'
	locked = P < 60.0
	gas = cls == 'Gas'
	lines = []
	add = lines.append
	add ('; === %s ===' % name)
	add ('; NASA Exoplanet Archive, Planetary Systems Composite Parameters (Utils/ExoKit):')
	add (';   orbit: %s; mass: %s' % (ref (pl['pl_orbper_reflink']), ref (pl['pl_bmasse_reflink'])))
	add (';   %s' % '; '.join (notes))
	add ('; Equilibrium temperature %s K, insolation %s x Earth\'s' % (pl['pl_eqt'] or '?', pl['pl_insol'] or '?'))
	add ('Name = %s' % name)
	add ('Primary = %s' % HOSTS[pl['hostname']][1])
	add ('Mass = %.6g            ; %.4g Earth masses' % (m * ME, m))
	add ('Size = %.6g            ; %.4g Earth radii' % (r * RE, r))
	add ('; orbit as seen from the Sun: period [d], epoch [JD], e, i, node, w [deg]')
	add ('SkyOrbit = %.9g %.8f %.5f %.4f %.1f %.2f' % (P, T, e, incl, node, w))
	add ('SkyOrbitEpoch = %s' % epoch)
	add ('EllipticOrbit = TRUE')
	if locked:
		add ('TidalLock = TRUE        ; assumed: a close orbit about an M dwarf')
		add ('SidRotPeriod = %.6g' % (P * 86400.0))
	else:
		add ('SidRotPeriod = 36000    ; assumed')
		add ('Obliquity = 0.05        ; assumed')
	if atm > 0.0:
		p0 = atm * 101325.0
		T0 = max (num (pl['pl_eqt'], 250.0), 200.0) + 20.0
		# the mix's gas constant, and its gamma from the heat capacities
		# (cv/R: N2 2.5, CO2 3.45 near 250 K)
		Rgas = 8314.46 / (28.01 * (1.0 - co2) + 44.01 * co2)
		gamma = 1.0 + 1.0 / (2.5 * (1.0 - co2) + 3.45 * co2)
		if co2 > 0.0:
			add ('; atmosphere: assumed (N2 and %.0f%% CO2, %.3g bar: a greenhouse that keeps the open sea liquid)' % (100.0 * co2, atm))
		else:
			add ('; atmosphere: assumed (N2, %.3g bar)' % atm)
		add ('AtmPressure0 = %.6g' % p0)
		add ('AtmDensity0 = %.4g' % (p0 / (Rgas * T0)))
		add ('AtmGasConstant = %.4g' % Rgas)
		add ('AtmGamma = %.3g' % gamma)
		add ('AtmAltLimit = 200e3')
		add ('AtmColor0 = 0.42 0.63 0.92')
		add ('AtmHazeColor = 0.57 0.74 1.0')
		add ('; clouds: procedural (assumed; a tidally locked world\'s deck over the substellar point)')
		add ('CloudFormat = 2')
		add ('CloudAlt = 6e3')
		add ('MinCloudResolution = 1')
		add ('MaxCloudResolution = 9')
		add ('CloudShadowDepth = 0.75')
	if gas:
		add ('; atmosphere and decks: assumed (Jupiter-like, colder)')
		add ('AtmPressure0 = 1e5')
		add ('AtmDensity0 = 0.29')
		add ('AtmGasConstant = 3615')
		add ('AtmGamma = 1.4')
		add ('AtmAltLimit = 2500e3')
		add ('AtmColor0 = 0.36 0.38 0.42')
		add ('AtmHazeColor = 0.20 0.18 0.16')
		add ('GasGiant = TRUE')
		add ('GGTemp1 = 140')
		add ('GGTropopauseTemp = 90')
		add ('GGJets = -90 0 -60 20 -45 -20 -30 60 -15 -40 0 100 15 -40 30 60 45 -20 60 20 90 0')
		add ('TileFormat = 2')
		add ('MaxPatchResolution = 8')
	else:
		add ('TileFormat = 2')
		add ('MaxPatchResolution = 18')
		add ('ElevationResolution = 1')
		emax = 2.5 * relief + 1.5 * mountains + 1000.0
		add ('MinElevation = %d' % -int (2.5 * relief * (1.5 if sea > 0 else 1.0) + 6000.0))
		add ('MaxElevation = %d' % int (emax))
	add ('; surface: procedural (assumed)')
	add ('SurfaceClass = %s' % cls)
	add ('SurfaceSeed = %d' % seed_of (name))
	add ('SurfaceColours = %.3f %.3f %.3f  %.3f %.3f %.3f' % (clow + chigh))
	if not gas:
		add ('SurfaceRelief = %g' % relief)
		add ('SurfaceMountains = %g' % mountains)
		add ('SurfaceSea = %g' % sea)
		add ('SurfaceCraters = %g' % craters)
		if openw: add ('SurfaceOpenWater = %g' % openw)
		if cls == 'Ocean':
			add ('TerrainDetailSea = TRUE')
			add ('TerrainDetailCraters = 0')
		elif cls == 'Rock':
			add ('TerrainDetailCraters = 1.5')
			add ('TerrainDetailPebbles = 1.2')
		elif cls == 'Ice':
			add ('TerrainDetailCraters = 0.6')
	path = os.path.join (outdir, file_stem (name) + '.cfg')
	with open (path, 'w', newline='\n') as f: f.write ('\n'.join (lines) + '\n')
	return name, path

def update_system (sysfile, entries):
	# replace the PlanetN lines (and their comment) of a system file
	with open (sysfile) as f: lines = [l.rstrip ('\n') for l in f]
	lines = [l for l in lines if not re.match (r'\s*Planet\d+\s*=', l) and not l.startswith ('; planets (Utils/ExoKit')]
	while lines and not lines[-1].strip (): lines.pop ()
	lines.append ('; planets (Utils/ExoKit)')
	for i, rel in enumerate (entries):
		lines.append ('Planet%d = %s' % (i + 1, rel))
	with open (sysfile, 'w', newline='\n') as f: f.write ('\n'.join (lines) + '\n')

def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--fetch', action='store_true', help='download the archive table again')
	args = ap.parse_args ()
	if args.fetch: fetch ()
	with open (CSV, newline='') as f: rows = list (csv.DictReader (f))
	bysys = {}
	for pl in sorted (rows, key=lambda r: (r['hostname'], float (r['pl_orbper']))):
		if pl['pl_name'] not in LOOKS:
			print ('skipped %s (no look)' % pl['pl_name'])
			continue
		sysname, _, sub = HOSTS[pl['hostname']]
		outdir = os.path.join (ROOT, 'Config', 'Systems', sub)
		name, path = write_planet (pl, outdir)
		bysys.setdefault (sysname, []).append ('Systems/%s/%s' % (sub, os.path.splitext (os.path.basename (path))[0]))
		print ('%-20s %s' % (name, os.path.relpath (path, ROOT)))
	for sysname, entries in bysys.items ():
		update_system (os.path.join (ROOT, 'Config', 'Systems', sysname + '.cfg'), entries)

if __name__ == '__main__':
	main ()
