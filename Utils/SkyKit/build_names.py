#!/usr/bin/env python3
"""Name the stars of the simulated sky: the trip targets of interstellar travel.

  build_names.py [--cache DIR] [--ref FILE] [--out FILE]

Names from the IAU Catalog of Star Names (IAU-CSN, fetch.py iau), then the
common names in curated_names.csv; each matched to its star in stars.bin by
Hipparcos number, else by direction (J2000) and brightness. Positions and
velocities come from build_stars.py's reference table (stars_ref.csv: full
precision, where stars.bin rounds positions to 0.0015 pc), so that a trip
arrives where the client draws the star close by.

Output (CSV): name, designation, constellation, index in stars.bin,
identifier, Hipparcos number, heliocentric Galactic position [pc] (epoch of
stars.bin) and velocity [km/s], M_V, effective temperature [K].
"""

import argparse
import csv
import math
import os
import re
import sys

import numpy as np

ICRS_TO_GAL = np.array([
    [-0.0548755604162154, -0.8734370902348850, -0.4838350155487132],
    [0.4941094278755837, -0.4448296299600112, 0.7469822444972189],
    [-0.8676661490190047, -0.1980763734312015, 0.4559837761750669]])
MATCH_ARCSEC = 60.0   # by direction: within this (J2000 to J2016 proper motions)
MATCH_MAG = 1.5       # and the catalogue's V within this of the IAU list's
CURATED_MAX_PC = 25.0 # curated names: nearby stars (a sanity check of the Hipparcos number)


def read_iau(path):
    # fixed columns for the names and the designation (they hold spaces),
    # the constellation by the header's column; the rest from the date back
    # (the notes after it are optional)
    rows = []
    con_at = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith('#Name'):
                con_at = line.index('Con')
            if line.startswith('#') or line.startswith('$') or not line.strip():
                continue
            line = line.rstrip('\n')
            name = line[0:18].strip()
            desig = line[36:49].strip()
            tok = line[49:].split()
            k = next((j for j, t in enumerate(tok) if re.fullmatch(r'\d{4}-\d{2}-\d{2}', t)), -1)
            if k < 6:
                continue
            mag, hip, ra, dec = tok[k - 6], tok[k - 4], tok[k - 2], tok[k - 1]
            con = line[con_at:con_at + 3].strip() if con_at else ''
            try:
                rows.append(dict(name=name, desig=desig, con=con if con != '_' else '',
                                 mag=float(mag) if mag != '_' else math.nan,
                                 hip=int(hip) if hip.isdigit() else 0,
                                 ra=float(ra), dec=float(dec)))
            except ValueError:
                print(f'IAU-CSN: unreadable line: {line[:60]}', file=sys.stderr)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=os.path.expanduser('~/orbiter-data/skykit'))
    ap.add_argument('--ref', help='stars_ref.csv (default: CACHE/out/stars_ref.csv)')
    ap.add_argument('--out', help='output (default: CACHE/out/stars_named.csv)')
    a = ap.parse_args()
    ref = a.ref or os.path.join(a.cache, 'out', 'stars_ref.csv')
    out = a.out or os.path.join(a.cache, 'out', 'stars_named.csv')
    here = os.path.dirname(os.path.abspath(__file__))

    r = np.genfromtxt(ref, delimiter=',', skip_header=1, names=True, dtype=None, encoding='ascii')
    ids = [int(x) for x in np.genfromtxt(ref, delimiter=',', skip_header=2, usecols=1, dtype=str)]  # (uint64: beyond int64)
    pos = np.stack([r['x'], r['y'], r['z']], axis=1)
    dist = np.linalg.norm(pos, axis=1)
    vmag = r['mv'] + 5.0 * np.log10(np.maximum(dist, 1e-6) / 10.0) + r['av']
    with np.errstate(invalid='ignore', divide='ignore'):
        u = pos / dist[:, None]
    byhip = {int(h): i for i, h in enumerate(r['hip']) if h > 0}

    named = {}  # row in ref -> (name, designation, constellation)
    iau = read_iau(os.path.join(a.cache, 'iau', 'IAU-CSN.txt'))
    miss = []
    for s in iau:
        i = byhip.get(s['hip'], -1)
        if i < 0:
            ra, dec = math.radians(s['ra']), math.radians(s['dec'])
            e = ICRS_TO_GAL @ np.array([math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec)])
            sep = np.degrees(np.arccos(np.clip(u @ e, -1, 1))) * 3600.0
            ok = (sep < MATCH_ARCSEC) & ~(np.abs(vmag - s['mag']) > MATCH_MAG)
            if ok.any():
                i = int(np.nonzero(ok)[0][np.argmin(sep[ok])])
        if i < 0:
            miss.append(s['name'])
            continue
        if i not in named:
            named[i] = (s['name'], s['desig'], s['con'])
    with open(os.path.join(here, 'curated_names.csv')) as f:
        for row in csv.DictReader(line for line in f if not line.startswith('#')):
            i = byhip.get(int(row['hip']), -1)
            if i < 0 or dist[i] > CURATED_MAX_PC:
                print(f"curated name {row['name']}: HIP {row['hip']} not found within {CURATED_MAX_PC} pc", file=sys.stderr)
                continue
            if i not in named:
                named[i] = (row['name'], f"HIP {row['hip']}", '')

    with open(out + '.part', 'w', encoding='utf-8') as f:
        f.write('# Named stars (build_names.py): IAU Catalog of Star Names (WGSN, CC BY 4.0) and curated_names.csv;\n')
        f.write('# positions heliocentric Galactic [pc] at the epoch of stars.bin, velocities [km/s]\n')
        f.write('name,designation,con,index,id,hip,x,y,z,vx,vy,vz,mv,teff\n')
        for i in sorted(named, key=lambda k: dist[k]):
            n, d, c = named[i]
            f.write('"%s","%s",%s,%d,%d,%d,%.15g,%.15g,%.15g,%.6g,%.6g,%.6g,%.4f,%.1f\n' % (
                n.replace('"', "'"), d, c, r['index'][i], ids[i], r['hip'][i], pos[i, 0], pos[i, 1], pos[i, 2],
                r['vx'][i], r['vy'][i], r['vz'][i], r['mv'][i], r['teff'][i]))
    os.replace(out + '.part', out)
    print(f'{out}: {len(named)} named stars; {len(miss)} IAU names unmatched (fainter than the catalogue\'s Hipparcos/nearby reference)')
    if miss:
        print('  unmatched: ' + ', '.join(miss[:40]) + (' ...' if len(miss) > 40 else ''))


if __name__ == '__main__':
    main()
