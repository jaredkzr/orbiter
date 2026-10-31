#!/usr/bin/env python3
"""Build the globular cluster table of the simulated sky from Harris's
catalogue (1996, 2010 edition; fetch.py harris).

  build_clusters.py [--cache DIR] [--out FILE]

One row per cluster with a distance and a luminosity: name, heliocentric
Galactic position [pc], M_V (dereddened, the catalogue's), Plummer radius
[pc] (the projected half-light radius: a Plummer sphere's), tidal radius
[pc] (King: r_c 10^c), [Fe/H]. The client draws a cluster's unresolved
light as a Plummer sphere and generates its stars from it.
"""

import argparse
import math
import os

from skykit.harris import clusters
from skykit.galaxies import equatorial

ARCMIN = math.pi / (180 * 60)
DEFAULT_RH = 3.0    # [pc]: half-light radius where the catalogue has none (a typical one)
MAX_TIDAL = 150.0   # [pc]


def table(cache):
    """the clusters with distance and luminosity: list of dicts (as the file)"""
    out = []
    for c in clusters(os.path.join(cache, 'harris', 'mwgc.dat')):
        if math.isnan(c['dist']) or math.isnan(c['mv']):
            continue
        d = c['dist']
        a = c['rh'] * ARCMIN * d if not math.isnan(c['rh']) else DEFAULT_RH
        rt = c['rc'] * ARCMIN * d * 10 ** c['c'] if not (math.isnan(c['rc']) or math.isnan(c['c'])) else 10 * a
        p = equatorial(c['ra'], c['dec']) * d
        out.append(dict(name=c['name'], x=p[0], y=p[1], z=p[2], mv=c['mv'], a=a, rt=min(max(rt, 3 * a), MAX_TIDAL),
                        feh=c['feh'], dist=d, ra=c['ra'], dec=c['dec']))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=os.path.expanduser('~/orbiter-data/skykit'))
    ap.add_argument('--out', help='output (default: CACHE/out/globulars.csv)')
    a = ap.parse_args()
    out = a.out or os.path.join(a.cache, 'out', 'globulars.csv')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rows = table(a.cache)
    with open(out, 'w') as f:
        f.write('# Milky Way globular clusters (Harris 1996, 2010 edition: '
                'https://physics.mcmaster.ca/~harris/mwgc.dat), by Utils/SkyKit/build_clusters.py\n')
        f.write('# name, x, y, z [pc] (heliocentric Galactic), M_V, Plummer radius [pc], tidal radius [pc], [Fe/H]\n')
        for r in rows:
            feh = '' if math.isnan(r['feh']) else f"{r['feh']:.2f}"
            f.write(f"{r['name'].replace(',', ' ')},{r['x']:.1f},{r['y']:.1f},{r['z']:.1f},{r['mv']:.2f},"
                    f"{r['a']:.2f},{r['rt']:.1f},{feh}\n")
    print(f'{out}: {len(rows)} clusters')


if __name__ == '__main__':
    main()
