"""The other galaxies of the simulated sky (Config/CSphere/Sky/galaxies.cfg):
their geometry, as the Vulkan client sets it up (SkyObjects.cpp).

Frame of a galaxy (heliocentric Galactic): its centre; the line of sight
n; on the sky at the centre, north N and east E; the major axis a = cos PA
N + sin PA E; the near side s = sin PA N - cos PA E (PA - 90 deg); the
disk's second axis b = cos i s - sin i n (toward the observer on the near
side); its normal z = a x b.
"""

import math

import numpy as np

ICRS_TO_GAL = np.array([[-0.0548755604162154, -0.8734370902348850, -0.4838350155487132],
                        [+0.4941094278755837, -0.4448296299600112, +0.7469822444972189],
                        [-0.8676661490190047, -0.1980763734312015, +0.4559837761750669]])


def equatorial(ra, dec):
    """Galactic unit vector of an ICRS direction [deg]"""
    a, d = math.radians(ra), math.radians(dec)
    return ICRS_TO_GAL @ np.array([math.cos(d) * math.cos(a), math.cos(d) * math.sin(a), math.sin(d)])


def load(path):
    """list of dicts: name, ra, dec, dist, mv, incl, pa, members (or None),
    centre, axes (a, b, z)"""
    out, g = [], None
    for line in open(path):
        f = line.split(';')[0].split()
        if not f:
            continue
        if f[0] == 'GALAXY':
            g = dict(name=f[1], ra=float(f[2]), dec=float(f[3]), dist=float(f[4]), mv=float(f[5]),
                     incl=float(f[6]), pa=float(f[7]), members=None)
        elif f[0] == 'MEMBERS' and g:
            g['members'] = dict(pmra=float(f[1]), pmdec=float(f[2]), tol=float(f[3]), radius=float(f[4]),
                                depth=float(f[5]))
        elif f[0] == 'END' and g:
            frame(g)
            out.append(g)
            g = None
    return out


def frame(g):
    a, d = math.radians(g['ra']), math.radians(g['dec'])
    n = equatorial(g['ra'], g['dec'])
    north = ICRS_TO_GAL @ np.array([-math.sin(d) * math.cos(a), -math.sin(d) * math.sin(a), math.cos(d)])
    east = ICRS_TO_GAL @ np.array([-math.sin(a), math.cos(a), 0.0])
    pa, i = math.radians(g['pa']), math.radians(g['incl'])
    ax = math.cos(pa) * north + math.sin(pa) * east
    near = math.sin(pa) * north - math.cos(pa) * east
    bx = math.cos(i) * near - math.sin(i) * n
    g['centre'] = n * g['dist']
    g['axes'] = (ax, bx, np.cross(ax, bx))
