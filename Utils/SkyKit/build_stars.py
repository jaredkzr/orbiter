#!/usr/bin/env python3
"""Build the 3D star catalogue of the simulated sky from Gaia DR3 and Hipparcos.

  build_stars.py [--cache DIR] [--out FILE] [--procs N]

Stars: Gaia DR3 brighter than G = 12.5 and those within 25 pc at any
brightness; Hipparcos for the stars too bright for Gaia (and for the
photometry of all stars brighter than V = 5.5). Each star gets:

- a position in space (heliocentric Galactic [pc], epoch J2016.0): its
  distance is drawn from the distance posterior (Bailer-Jones et al. 2021
  photogeometric percentiles, or the parallax likelihood where precise),
  not set to a point estimate: estimates line stars up in radial streaks
  when seen from elsewhere. The draw is seeded by the star's identifier, so
  rebuilds give the same catalogue.
- its intrinsic light: absolute visual magnitude M_V and effective
  temperature, dereddened with the extinction from the Sun that the client
  itself computes (skykit/galdust.py): seen from the Sun, the client shows
  each star at its observed magnitude and colour.
  Hot stars (O, B, Wolf-Rayet) of known Hipparcos spectral type take their
  temperature from the type instead, and their extinction from their colour
  excess (flag SPECTRAL): their colours hardly vary with temperature, and
  the dust map misses much of the dust close around them. (The client's
  nebulae are ionised by these stars.) Where their parallax says little,
  their distance follows from the absolute magnitude of the type. A type at
  odds with the parallax (a companion's) is not used.
- its space velocity (heliocentric Galactic [km/s]; no radial part where
  Gaia has no radial velocity).
- a stable identifier (the Gaia DR3 source_id, else 2^63 + the Hipparcos
  number; the Sun: 2^63) and its Hipparcos number where it has one.

The Sun is in the catalogue (flag SUN), at the origin.

Beside stars.bin, stars_ref.csv holds the Hipparcos stars and those within
25 pc in full precision (stars.bin rounds positions to 0.0015 pc), for
build_names.py.

The file format is in Orbitersdk/include/StarCatalogue.h. Stars are grouped
in cubic sectors of 100 pc (by sector, brightest first in each), their
positions stored relative to the sector's corner.
"""

import argparse
import math
import multiprocessing as mp
import os
import re
import struct
import sys
import time

import numpy as np
import pandas as pd
from scipy.special import ndtri

from skykit.galdust import GalDust
from skykit import galaxies
import build_clusters

from fetch import GAIA_GMAX, NEARBY_PLX

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

SECTOR = 100.0          # sector edge [pc]
SPT_PLX_SIGMA = 5.0     # a hot star's type is ignored where the distance it implies is this many sigma off the parallax
SPT_MV_SCATTER = 1.0    # [mag] scatter of the absolute magnitudes of a spectral type (in that sigma)
EPOCH = 2016.0          # [Julian year] of the positions
HIP_EPOCH = 1991.25
PLX_ZP = -0.017         # Gaia parallax zero point [mas] (Lindegren et al. 2021): add 0.017
E_BPRP = 0.432          # E(BP-RP) / A_V
E_BV = 1.0 / 3.1        # E(B-V) / A_V
HIP_PHOT_V = 5.5        # Hipparcos photometry for stars brighter than this
TEFF_SUN, MV_SUN = 5772.0, 4.83
KMS_PER_MASYR_PC = 4.740470446e-3  # 1 mas/yr at 1 pc [km/s]
ID_HIP = 1 << 63

# flags (as StarCatalogue.h)
F_SUN, F_HIP, F_NEARBY, F_DIST_UNCERTAIN, F_NO_COLOUR, F_NO_RV, F_MULTIPLE, F_SPECTRAL, F_EVOLVED, F_MEMBER = \
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512

# ICRS -> Galactic (Hipparcos introduction, vol. 1, sec. 1.5.3)
ICRS_TO_GAL = np.array([[-0.0548755604162154, -0.8734370902348850, -0.4838350155487132],
                        [+0.4941094278755837, -0.4448296299600112, +0.7469822444972189],
                        [-0.8676661490190047, -0.1980763734312015, +0.4559837761750669]])


# --- colour, temperature, magnitude relations of dwarfs (Pecaut & Mamajek 2013, table version 2022.04.16)
class Dwarfs:
    def __init__(self, path):
        rows = []
        inside = False
        for line in open(path):
            if line.startswith('#SpT'):
                if inside:
                    break
                inside = True
                continue
            if inside and line.strip():
                rows.append(line.split())
        def num(s):  # ('...': none; ':' marks an uncertain value)
            s = s.rstrip(':')
            return float(s) if s.strip('.') else math.nan
        t = np.array([[num(r[1]), num(r[8]), num(r[10]), num(r[11]), num(r[13]), num(r[7])] for r in rows])
        self.teff, bv, gv, bprp, mg, mv = t.T
        # (by spectral type: O and B, and A0 to join)
        sp = np.array([spt_index(r[0]) for r in rows])
        ok = ~np.isnan(sp) & (sp <= 20.0) & ~np.isnan(bv) & ~np.isnan(mv)
        self.spt = sp[ok], np.log10(self.teff[ok]), bv[ok], mv[ok]

        def mono(x, y):
            # the part of the sequence where x increases, from the hottest star
            ok = ~np.isnan(x) & ~np.isnan(y)
            x, y = x[ok], y[ok]
            keep = [0]
            for i in range(1, len(x)):
                if x[i] > x[keep[-1]]:
                    keep.append(i)
            return x[keep], y[keep]
        self.bprp_t = mono(bprp, np.log10(self.teff))
        self.bv_t = mono(bv, np.log10(self.teff))
        self.bprp_gv = mono(bprp, gv)
        self.bprp_bv = mono(bprp, bv)
        self.mg_bprp = mono(mg, bprp)

    def bprp_of_bv(self, bv):
        """BP - RP of B - V (hotter than the table's BP - RP: on the slope of ~1.2)"""
        x, y = self.bprp_bv
        return np.where(bv < x[0], y[0] + 1.2 * (bv - x[0]), np.interp(bv, x, y))

    def teff_bprp(self, c):
        # (hotter than the table's BP - RP: through B - V, on the slope of
        # BP - RP against B - V of late B stars, ~1.2)
        x, y = self.bprp_t
        bv0 = np.interp(x[0], *self.bprp_bv)
        hot = 10 ** _interp_extrap(bv0 + (c - x[0]) / 1.2, *self.bv_t, 4.65)
        return np.where(c < x[0], hot, 10 ** np.interp(c, x, y))

    def teff_bv(self, c):
        x, y = self.bv_t
        return 10 ** _interp_extrap(c, x, y, 4.6)

    def bprp_of_mg(self, mg):
        x, y = self.mg_bprp
        return np.interp(mg, x, y)

    def g_minus_v(self, c):
        """G - V of observed BP - RP: Riello et al. (2021) to 2.75, the dwarf
        sequence beyond (joined)"""
        cc = np.clip(c, -0.5, 2.75)
        r = -0.02704 + 0.01424 * cc - 0.2156 * cc ** 2 + 0.01426 * cc ** 3
        x, y = self.bprp_gv
        tail = np.interp(c, x, y) - np.interp(2.75, x, y)
        return np.where(c > 2.75, r + tail, r)


# red giants (approximate sequences, after Alonso et al. 1999 and van Belle
# 1999): cooler stars of a colour are hotter as giants than as dwarfs
GIANT_BV = ([0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9],
            [5200, 5000, 4800, 4600, 4450, 4300, 4150, 4000, 3800, 3600, 3450, 3350])
GIANT_BPRP = ([0.9, 1.0, 1.1, 1.2, 1.4, 1.6, 1.8, 2.0, 2.4, 3.0, 4.0],
              [5300, 5050, 4800, 4600, 4300, 4050, 3850, 3700, 3550, 3400, 3200])
GIANT_MV = (1.0, 2.5)  # giants brighter than the first, dwarfs fainter than the second


def giant_teff(teff_dwarf, colour, mv, seq):
    """the temperature of a star of the colour, a giant's where it is luminous"""
    x, y = seq
    tg = np.interp(colour, x, y)
    w = np.clip((GIANT_MV[1] - mv) / (GIANT_MV[1] - GIANT_MV[0]), 0, 1) * (colour > x[0])
    return teff_dwarf * (1 - w) + tg * w


# --- hot stars by spectral type: their colours hardly change with temperature,
# so reddening (which the dust map can underestimate near them) makes a
# hot star look cool; the spectral type gives the temperature and, with the
# observed colour, the star's own extinction
SPT_LETTERS = 'OBAFGKM'
SPT_RE = re.compile(r'^(W[CNRO]?|O|B)(\d+(?:\.\d+)?)?\s*(Ia|Iab|Ib|III|II|IV|V|I)?')
SPT_DEFAULT_SUB = {'O': 7.0, 'B': 5.0}
WR_TEFF = 50000.0
# supergiants are cooler than dwarfs of the type (approximate, after the
# calibrations of Martins et al. 2005 and Crowther et al. 2006), by
# spectral type index (O0 = 0, B0 = 10, A0 = 20); giants in between
SG_SCALE = ([3.0, 9.5, 10.0, 15.0, 19.0], [0.93, 0.90, 0.85, 0.87, 0.97])
CLASS_FRAC = {'V': 0.0, 'IV': 0.15, 'III': 0.4, 'II': 0.7, 'I': 1.0, 'Ib': 1.0, 'Iab': 1.0, 'Ia': 1.0}
# absolute magnitudes (for the stars whose parallax says little): dwarfs'
# from the table; brighter by luminosity class (rounded calibrations, e.g.
# Wegner 2006; Martins & Plez 2006); supergiants about -6.3 throughout
MV_CLASS_OFFSET = {'V': 0.0, 'IV': -0.4, 'III': -1.0}
MV_SUPERGIANT, MV_WR = -6.3, -4.5
# intrinsic colours of luminous hot stars are redder than dwarfs' (B - V,
# early B supergiants by ~0.06; e.g. Wegner 1994, Fitzgerald 1970)
BV_CLASS_OFFSET = {'V': 0.0, 'IV': 0.0, 'III': 0.02, 'II': 0.04, 'I': 0.06, 'Ib': 0.06, 'Iab': 0.06, 'Ia': 0.06}


def spt_index(spt):
    """O0 = 0, B0 = 10, A0 = 20, ... (nan: not a plain type)"""
    m = re.match(r'^([OBAFGKM])(\d+(?:\.\d+)?)', spt)
    return SPT_LETTERS.index(m.group(1)) * 10.0 + float(m.group(2)) if m else math.nan


def spectral(sptypes, dw):
    """effective temperature, intrinsic B - V and absolute magnitude M_V
    of O, B and Wolf-Rayet stars of the (Hipparcos) spectral types (nan for
    the others), and whether evolved (giants, supergiants, Wolf-Rayet)"""
    n = len(sptypes)
    teff, bv0, mv0 = np.full(n, np.nan), np.full(n, np.nan), np.full(n, np.nan)
    evolved = np.zeros(n, bool)
    sx, sl, sb, sm = dw.spt
    for i, t in enumerate(sptypes):
        if not isinstance(t, str):
            continue
        m = SPT_RE.match(t.strip())
        if not m or re.search(r'\+\s*[AFGKMC]', t):  # (not with a cooler companion: its colour)
            continue
        letter, sub, lc = m.group(1), m.group(2), m.group(3) or 'V'
        if letter.startswith('W'):
            teff[i], bv0[i], mv0[i], evolved[i] = WR_TEFF, -0.32, MV_WR, True
            continue
        evolved[i] = lc not in ('V', 'IV')
        x = SPT_LETTERS.index(letter) * 10.0 + (float(sub) if sub else SPT_DEFAULT_SUB[letter])
        f = 1.0 - (1.0 - np.interp(x, *SG_SCALE)) * CLASS_FRAC[lc]
        teff[i] = 10 ** np.interp(x, sx, sl) * f
        bv0[i] = np.interp(x, sx, sb) + BV_CLASS_OFFSET[lc] * (x < 20.0)
        mvd = np.interp(x, sx, sm)
        if lc in MV_CLASS_OFFSET:
            mv0[i] = mvd + MV_CLASS_OFFSET[lc]
        elif lc == 'II':
            mv0[i] = 0.5 * (mvd - 1.0 + MV_SUPERGIANT)
        else:
            mv0[i] = min(MV_SUPERGIANT, mvd)
    return teff, bv0, mv0, evolved


def _interp_extrap(c, x, y, ymax):
    """interpolate; linear extrapolation on the hot side (to ymax)"""
    s = (y[1] - y[0]) / (x[1] - x[0])
    v = np.interp(c, x, y)
    return np.where(c < x[0], np.minimum(y[0] + s * (c - x[0]), ymax), v)


# --- deterministic random numbers from identifiers
def _splitmix(x, salt):
    with np.errstate(over='ignore'):
        z = x.astype(np.uint64) + np.uint64(salt) * np.uint64(0x9E3779B97F4A7C15)
        z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        z = z ^ (z >> np.uint64(31))
    return z


def unit_normal(ids, salt):
    u = ((_splitmix(ids, salt) >> np.uint64(11)).astype(np.float64) + 0.5) / float(1 << 53)
    return ndtri(u)


# --- loading
def load_csvs(paths, **kw):
    return pd.concat([pd.read_csv(p, **kw) for p in paths], ignore_index=True)


def load_gaia(cache):
    d = os.path.join(cache, 'gaia')
    print('reading Gaia DR3 ...')
    g = pd.read_csv(os.path.join(d, 'dr3.csv'))
    near = pd.read_csv(os.path.join(d, 'nearby.csv'))
    near['nearby'] = True
    g['nearby'] = g.source_id.isin(near.source_id)
    g = pd.concat([g, near[~near.source_id.isin(g.source_id)]], ignore_index=True)
    g['nearby'] = g.nearby.fillna(False).astype(bool)
    dist = os.path.join(d, 'dist.csv')
    if os.path.exists(dist):
        bj = pd.read_csv(dist)
        g = g.merge(bj, on='source_id', how='left')
    else:
        print('  (no Bailer-Jones distances: parallaxes alone)')
        for c in ('r_lo_photogeo', 'r_med_photogeo', 'r_hi_photogeo', 'r_lo_geo', 'r_med_geo', 'r_hi_geo'):
            g[c] = np.nan
    nb = load_csvs([os.path.join(d, f'hip_neighbour_{i}.csv') for i in range(4)])
    nb = nb.drop_duplicates('hip').drop_duplicates('source_id')
    g = g.merge(nb, on='source_id', how='left')
    g = g[g.phot_g_mean_mag.notna()].reset_index(drop=True)
    print(f'  {len(g)} sources, {g.hip.notna().sum()} with a Hipparcos match, {g.nearby.sum()} within 25 pc')
    return g


def load_hip(cache):
    d = os.path.join(cache, 'hip')
    h2 = load_csvs([os.path.join(d, f'hip2_{i}.csv') for i in range(4)])
    h1 = load_csvs([os.path.join(d, f'hip1997_{i}.csv') for i in range(4)])
    h = h2.merge(h1[['hip', 'vmag', 'b_v', 'sptype', 'ccdm', 'multflag', 'nsys']], on='hip', how='left', suffixes=('', '_1997'))
    print(f'  {len(h)} Hipparcos stars')
    return h


# --- the merged list
def merge(g, h, dw):
    """One row per star: identifier, Hipparcos number, ra/dec at J2016 [deg],
    pm [mas/yr], rv [km/s], parallax and error [mas] or distance
    percentiles, V, colour (kind, value), flags."""
    hipset = h.set_index('hip')
    matched = g.hip.notna() & g.hip.isin(h.hip)
    gh = g[matched]
    hv = hipset.loc[gh.hip.astype(int), 'vmag'].to_numpy()
    # the Hipparcos row stands for a matched star that is bright or that Gaia
    # has no parallax or colour for
    use_hip = (hv < HIP_PHOT_V) | gh.parallax.isna().to_numpy() | gh.phot_bp_mean_mag.isna().to_numpy()
    drop_gaia = gh.index[use_hip]
    hip_from_gaia = set(gh.hip[use_hip].astype(int))
    gaia_rows = g.drop(index=drop_gaia)
    hip_rows = h[~h.hip.isin(set(gh.hip[~use_hip].astype(int)))]
    print(f'  Hipparcos rows: {len(hip_rows)} ({len(hip_from_gaia)} preferred to their Gaia match), Gaia rows: {len(gaia_rows)}')

    # the Gaia astrometry of the stars taken from Hipparcos, where good
    ga = gh[use_hip].set_index(gh.hip[use_hip].astype(int))
    ga = ga[(ga.parallax / ga.parallax_error > 10) & (ga.ruwe < 2.0)]

    out = {}
    # Gaia rows
    G = gaia_rows
    n = len(G)
    c = (G.phot_bp_mean_mag - G.phot_rp_mean_mag).to_numpy()
    out['id'] = G.source_id.to_numpy(np.uint64)
    out['hip'] = G.hip.fillna(0).to_numpy(np.int64)
    out['ra'], out['dec'] = G.ra.to_numpy(), G.dec.to_numpy()
    out['pmra'], out['pmdec'] = G.pmra.fillna(0).to_numpy(), G.pmdec.fillna(0).to_numpy()
    out['rv'] = G.radial_velocity.to_numpy()
    out['plx'], out['plx_err'] = (G.parallax - PLX_ZP).to_numpy(), G.parallax_error.to_numpy()
    for q in ('lo', 'med', 'hi'):
        out['r_' + q] = G[f'r_{q}_photogeo'].fillna(G[f'r_{q}_geo']).to_numpy()
    no_col = np.isnan(c)
    out['V'] = G.phot_g_mean_mag.to_numpy() - dw.g_minus_v(np.where(no_col, 1.0, c))
    out['colour'] = c
    out['colour_kind'] = np.zeros(n, np.int8)  # 0: BP-RP, 1: B-V
    out['gmag'] = G.phot_g_mean_mag.to_numpy()
    out['sptype'] = hipset.sptype.reindex(G.hip.fillna(0).astype(int)).to_numpy(object)
    flags = np.where(G.nearby, F_NEARBY, 0) | np.where(no_col, F_NO_COLOUR, 0) | np.where(G.radial_velocity.isna(), F_NO_RV, 0)
    flags |= np.where(G.ruwe > 1.4, F_MULTIPLE, 0)
    out['flags'] = flags.astype(np.uint16)

    # Hipparcos rows (positions brought to J2016)
    H = hip_rows.reset_index(drop=True)
    m = len(H)
    dt = EPOCH - HIP_EPOCH
    dec = H.dec.to_numpy() + H.pm_de.fillna(0).to_numpy() * dt / 3.6e6
    ra = H.ra.to_numpy() + H.pm_ra.fillna(0).to_numpy() * dt / 3.6e6 / np.cos(np.radians(H.dec.to_numpy()))
    hipn = H.hip.to_numpy()
    plx, plx_err = H.plx.to_numpy(np.float64), H.e_plx.to_numpy(np.float64)
    pmra, pmdec = H.pm_ra.fillna(0).to_numpy(), H.pm_de.fillna(0).to_numpy()
    gid = np.full(m, np.uint64(0))
    rv = np.full(m, np.nan)
    # (Gaia's astrometry where good)
    sel = np.isin(hipn, ga.index)
    if sel.any():
        gg = ga.loc[hipn[sel]]
        ra[sel], dec[sel] = gg.ra.to_numpy(), gg.dec.to_numpy()
        plx[sel], plx_err[sel] = (gg.parallax - PLX_ZP).to_numpy(), gg.parallax_error.to_numpy()
        pmra[sel], pmdec[sel] = gg.pmra.to_numpy(), gg.pmdec.to_numpy()
        rv[sel] = gg.radial_velocity.to_numpy()
    # (identifiers: the Gaia source where matched, else Hipparcos)
    hid = np.uint64(ID_HIP) + hipn.astype(np.uint64)
    gmatch = g[matched].set_index(g[matched].hip.astype(int)).source_id
    has_g = np.isin(hipn, gmatch.index)
    hid[has_g] = gmatch.loc[hipn[has_g]].to_numpy(np.uint64)
    bv = H.b_v.fillna(H.b_v_1997).to_numpy()
    V = H.vmag.to_numpy()
    V = np.where(np.isnan(V), H.hp_mag.to_numpy() - 0.3 * np.nan_to_num(bv, nan=0.5), V)
    hf = F_HIP | np.where(np.isnan(bv), F_NO_COLOUR, 0) | np.where(np.isnan(rv), F_NO_RV, 0)
    hf |= np.where(H.ccdm.notna() | H.multflag.notna(), F_MULTIPLE, 0) | np.where(plx > 40, F_NEARBY, 0)
    hip_part = dict(id=hid, hip=hipn.astype(np.int64), ra=ra, dec=dec, pmra=pmra, pmdec=pmdec, rv=rv,
                    plx=plx, plx_err=plx_err, r_lo=np.full(m, np.nan), r_med=np.full(m, np.nan),
                    r_hi=np.full(m, np.nan), V=V, colour=bv, colour_kind=np.ones(m, np.int8),
                    gmag=np.full(m, np.nan), sptype=H.sptype.to_numpy(object), flags=hf.astype(np.uint16))
    for k in out:
        out[k] = np.concatenate([out[k], hip_part[k]])
    # curated values for nearby stars
    cur = pd.read_csv(os.path.join(HERE, 'curated_nearby.csv'), comment='#')
    out['curated'] = np.zeros(len(out['id']), bool)
    for _, c in cur.iterrows():
        sel = ((out['hip'] == c.hip) & (c.hip > 0)) | ((out['id'] == np.uint64(c.source_id)) & (c.source_id > 0))
        out['plx'][sel], out['plx_err'][sel] = c.plx, c.plx_err
        out['flags'][sel] |= F_NEARBY
        out['curated'][sel] = True
    print(f'  {len(cur)} curated nearby stars')
    return out


# --- distances
def draw_distance(s):
    """[pc], drawn from each star's distance posterior; and whether it is
    uncertain (> 10%)"""
    n = len(s['id'])
    u = np.clip(unit_normal(s['id'], 1), -3, 3)
    plx, err = s['plx'], s['plx_err']
    snr = plx / err
    r = np.full(n, np.nan)
    # precise parallaxes: the likelihood (the posterior, practically)
    good = snr > 10
    r[good] = 1000.0 / (plx[good] + err[good] * u[good])
    # else the Bailer-Jones posterior: split log-normal through the percentiles
    bj = ~good & ~np.isnan(s['r_med'])
    lo, med, hi = s['r_lo'][bj], s['r_med'][bj], s['r_hi'][bj]
    ub = u[bj]
    r[bj] = med * np.where(ub > 0, (hi / med) ** ub, (med / lo) ** (-ub))
    # else the parallax with an exponentially decreasing space density prior
    # (L = 1.35 kpc), sampled numerically
    rest = np.isnan(r) & ~np.isnan(plx) & (err > 0)
    if rest.any():
        rg = np.geomspace(1.0, 3e4, 2000)
        idx = np.nonzero(rest)[0]
        uu = (_splitmix(s['id'][idx], 2) >> np.uint64(11)).astype(np.float64) / float(1 << 53)
        for i0 in range(0, len(idx), 20000):
            j = idx[i0:i0 + 20000]
            lp = (np.log(rg ** 3)[None, :] - rg[None, :] / 1350.0
                  - 0.5 * ((plx[j, None] - 1000.0 / rg[None, :]) / err[j, None]) ** 2)  # (r^3: dr in log r)
            p = np.exp(lp - lp.max(axis=1, keepdims=True))
            cdf = np.cumsum(p, axis=1)
            cdf /= cdf[:, -1:]
            k = (cdf < uu[i0:i0 + 20000, None]).sum(axis=1)
            r[j] = rg[np.minimum(k, len(rg) - 1)]
    # curated parallaxes (a system's components share theirs): at it, not
    # drawn (independent draws would split a binary along the line of sight)
    if 'curated' in s:
        cur = s['curated']
        r[cur] = 1000.0 / plx[cur]
    # no parallax: a red-clump giant's absolute magnitude (M_V = 0.8)
    none = np.isnan(r)
    r[none] = 10.0 ** ((s['V'][none] - 0.8) / 5.0 + 1.0)
    uncertain = ~good
    rel = np.ones(n)
    rel[bj] = (hi - lo) / (2.0 * med)
    uncertain &= ~(bj & (rel < 0.1))
    print(f'  distances: {good.sum()} from the parallax, {bj.sum()} Bailer-Jones, {rest.sum()} parallax + prior, '
          f'{none.sum()} photometric')
    return np.clip(r, 1.0, 1e5), uncertain


# --- members of the Magellanic Clouds and of globular clusters: their
# parallaxes say nothing (or little), so the posterior puts them in the
# foreground; their distance is their system's
def members(s, u, r, cfg, cache):
    """place the members in their systems (r: in place); which they are"""
    plx, err = s['plx'], s['plx_err']
    has = ~np.isnan(plx) & ~np.isnan(err)
    member = np.zeros(len(r), bool)
    for g in galaxies.load(cfg):
        m = g['members']
        if not m:
            continue
        c = g['centre']
        sel = (u @ (c / np.linalg.norm(c))) > math.cos(math.radians(m['radius']))
        sel &= np.hypot(s['pmra'] - m['pmra'], s['pmdec'] - m['pmdec']) < m['tol']
        sel &= has & (plx < 3.0 * err + 0.05)
        z = g['axes'][2]
        t = (c @ z) / (u[sel] @ z)  # (the line of sight through the disk's plane)
        r[sel] = np.clip(t + unit_normal(s['id'][sel], 4) * m['depth'], 0.5 * g['dist'], 1.5 * g['dist'])
        member |= sel
        print(f'  {sel.sum()} stars of the {g["name"]}')
    n = 0
    for cl in build_clusters.table(cache):
        c = np.array([cl['x'], cl['y'], cl['z']])
        d = cl['dist']
        sel = (u @ (c / d)) > math.cos(min(cl['rt'], 5.0 * cl['a']) / d)
        sel &= has & ~member & (np.abs(plx - 1000.0 / d) < 3.0 * err + 0.03)
        r[sel] = np.maximum(d + unit_normal(s['id'][sel], 5) * cl['a'], 0.5 * d)
        member |= sel
        n += sel.sum()
    print(f'  {n} stars of globular clusters')
    return member


def unit_vectors(ra, dec):
    a, d = np.radians(ra), np.radians(dec)
    eq = np.stack([np.cos(d) * np.cos(a), np.cos(d) * np.sin(a), np.sin(d)], axis=1)
    return eq @ ICRS_TO_GAL.T, a, d


def velocities(s, r, a, d):
    """heliocentric Galactic velocity [km/s]"""
    vt_a = s['pmra'] * r * KMS_PER_MASYR_PC
    vt_d = s['pmdec'] * r * KMS_PER_MASYR_PC
    vr = np.nan_to_num(s['rv'])
    rhat = np.stack([np.cos(d) * np.cos(a), np.cos(d) * np.sin(a), np.sin(d)], axis=1)
    phat = np.stack([-np.sin(a), np.cos(a), np.zeros_like(a)], axis=1)
    qhat = np.stack([-np.sin(d) * np.cos(a), -np.sin(d) * np.sin(a), np.cos(d)], axis=1)
    v = vr[:, None] * rhat + vt_a[:, None] * phat + vt_d[:, None] * qhat
    return v @ ICRS_TO_GAL.T


_dust = None


def _ext_init(cfg, dust):
    global _dust
    _dust = GalDust(cfg, dust)


def _ext_chunk(pos):
    return _dust.extinction(np.zeros(3), pos)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=os.path.expanduser('~/orbiter-data/skykit'))
    ap.add_argument('--cfg', default=os.path.join(ROOT, 'Config', 'CSphere', 'Sky', 'galaxy.cfg'))
    ap.add_argument('--galaxies', default=os.path.join(ROOT, 'Config', 'CSphere', 'Sky', 'galaxies.cfg'))
    ap.add_argument('--dust', help='dust.bin (default: CACHE/out/dust.bin)')
    ap.add_argument('--out', help='output (default: CACHE/out/stars.bin)')
    ap.add_argument('--procs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    dust = a.dust or os.path.join(a.cache, 'out', 'dust.bin')
    out = a.out or os.path.join(a.cache, 'out', 'stars.bin')
    t0 = time.time()

    dw = Dwarfs(os.path.join(a.cache, 'mamajek', 'eem.txt'))
    g = load_gaia(a.cache)
    h = load_hip(a.cache)
    s = merge(g, h, dw)
    del g, h
    n = len(s['id'])

    r, uncertain = draw_distance(s)
    u, ra, dec = unit_vectors(s['ra'], s['dec'])
    member = members(s, u, r, a.galaxies, a.cache)
    pos = u * r[:, None]
    vel = velocities(s, r, ra, dec)

    print(f'extinction from the Sun ({a.procs} processes) ...')
    chunks = np.array_split(pos, max(1, n // 50000))
    with mp.Pool(a.procs, _ext_init, (a.cfg, dust)) as pool:
        av = np.concatenate(pool.map(_ext_chunk, chunks))

    # intrinsic light
    mv = s['V'] - 5.0 * np.log10(r / 10.0) - av
    col = s['colour'].copy()
    gaia = s['colour_kind'] == 0
    # (no colour: that of a dwarf of the absolute magnitude)
    nc = np.isnan(col)
    mg = s['gmag'] - 5.0 * np.log10(r / 10.0) - 0.8 * av
    col[nc & gaia] = dw.bprp_of_mg(np.nan_to_num(mg[nc & gaia], nan=4.0))
    col[nc & gaia] += E_BPRP * av[nc & gaia]  # (as observed)
    col[nc & ~gaia] = 0.6 + E_BV * av[nc & ~gaia]
    c0 = np.where(gaia, col - E_BPRP * av, col - E_BV * av)
    teff = np.where(gaia, giant_teff(dw.teff_bprp(c0), c0, mv, GIANT_BPRP), giant_teff(dw.teff_bv(c0), c0, mv, GIANT_BV))
    teff = np.clip(teff, 1600.0, 45000.0)
    flags = s['flags'] | np.where(uncertain, F_DIST_UNCERTAIN, 0).astype(np.uint16)
    # hot stars of known spectral type: temperature from the type, and the
    # extinction from the Sun from the colour excess (from the Sun, the
    # client shows the star with it)
    ts, bv0, mv0, evolved = spectral(s['sptype'], dw)
    sp = ~np.isnan(ts) & ~np.isnan(s['colour'])
    c00 = np.where(gaia, dw.bprp_of_bv(np.nan_to_num(bv0, nan=0.0)), bv0)
    av_sp = np.maximum(s['colour'] - c00, 0.0) / np.where(gaia, E_BPRP, E_BV)
    # (not where the type is at odds with the parallax: the type of a
    # companion, its colour the brighter star's, e.g. Almach: B8V of a K
    # giant's colour, placed by it at 3.6 pc instead of 120)
    with np.errstate(invalid='ignore', divide='ignore'):
        r_type = 10.0 ** ((s['V'] - av_sp - mv0) / 5.0 + 1.0)
        sig = np.hypot(s['plx_err'], SPT_MV_SCATTER * 0.4605 * np.abs(s['plx'])) # (a magnitude's scatter: 46% in distance)
        odd = np.abs(1000.0 / r_type - s['plx']) > SPT_PLX_SIGMA * sig
    odd &= sp & (s['plx_err'] > 0)
    sp &= ~odd
    print(f'  {odd.sum()} spectral types at odds with their parallaxes: ignored')
    teff[sp] = ts[sp]
    mv[sp] += av[sp] - av_sp[sp]
    av[sp] = av_sp[sp]
    flags[sp] |= F_SPECTRAL
    flags[sp & evolved] |= F_EVOLVED
    flags[member] |= F_MEMBER
    # (where the parallax says little, the distance from the absolute
    # magnitude of the type: a poor parallax's draw can put a hot star kpc
    # away, as luminous as a cluster)
    spd = sp & uncertain & ~np.isnan(mv0) & ~member
    r_sp = 10.0 ** ((s['V'][spd] - av[spd] - mv0[spd]) / 5.0 + 1.0)
    pos[spd] = u[spd] * r_sp[:, None]
    r[spd] = r_sp
    mv[spd] = mv0[spd]
    vel = velocities(s, r, ra, dec)
    print(f'  {sp.sum()} hot stars with temperatures from their spectral types, {spd.sum()} of them placed by it')

    # the Sun
    ids = np.concatenate([[np.uint64(ID_HIP)], s['id']])
    hip = np.concatenate([[0], s['hip']])
    pos = np.vstack([[0, 0, 0], pos])
    vel = np.vstack([[0, 0, 0], vel])
    mv = np.concatenate([[MV_SUN], mv])
    teff = np.concatenate([[TEFF_SUN], teff])
    av = np.concatenate([[0.0], av])
    flags = np.concatenate([[F_SUN], flags]).astype(np.uint16)
    n += 1

    # sectors, brightest first in each
    sec = np.floor(pos / SECTOR).astype(np.int64)
    if np.abs(sec).max() >= 32767:
        sys.exit('star beyond the sector range')
    key = ((sec[:, 0] + 32768) << 32) | ((sec[:, 1] + 32768) << 16) | (sec[:, 2] + 32768)
    order = np.lexsort((mv, key))
    key, sec = key[order], sec[order]
    pos, vel, mv, teff, av, flags, ids, hip = (x[order] for x in (pos, vel, mv, teff, av, flags, ids, hip))
    first = np.concatenate([[0], np.nonzero(np.diff(key))[0] + 1])
    count = np.diff(np.concatenate([first, [n]]))
    q = np.clip(np.round((pos - sec * SECTOR) / SECTOR * 65536.0), 0, 65535).astype(np.uint16)
    hipidx = np.nonzero(hip > 0)[0]

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out + '.part', 'wb') as f:
        f.write(struct.pack('<4sIIII', b'OSTR', 1, n, len(first), len(hipidx)))
        f.write(struct.pack('<ffff', SECTOR, EPOCH, GAIA_GMAX, 1000.0 / NEARBY_PLX))
        f.write(struct.pack('<4I', 0, 0, 0, 0))
        st = np.zeros(len(first), dtype=[('i', '<i2', 3), ('pad', '<u2'), ('first', '<u4'), ('count', '<u4')])
        st['i'] = sec[first]
        st['first'], st['count'] = first, count
        f.write(st.tobytes())
        f.write(q.astype('<u2').tobytes())
        f.write(np.round(mv * 1000).clip(-32767, 32767).astype('<i2').tobytes())
        f.write(np.round(np.log10(teff) * 10000).astype('<u2').tobytes())
        f.write(np.round(av * 1000).clip(0, 65535).astype('<u2').tobytes())
        f.write(flags.astype('<u2').tobytes())
        f.write(np.clip(vel, -60000, 60000).astype('<f2').tobytes())  # (bad distances: absurd speeds)
        f.write(ids.astype('<u8').tobytes())
        hp = np.zeros(len(hipidx), dtype=[('index', '<u4'), ('hip', '<u4')])
        hp['index'], hp['hip'] = hipidx, hip[hipidx]
        f.write(hp.tobytes())
    os.replace(out + '.part', out)

    # the reference table: full precision (stars.bin rounds positions to
    # 0.0015 pc) for the Hipparcos stars and those within 25 pc, by index in
    # stars.bin: for build_names.py (named stars: trip targets, drawn as
    # suns close by)
    ref = os.path.splitext(out)[0] + '_ref.csv'
    sel = np.nonzero((hip > 0) | (np.linalg.norm(pos, axis=1) < 25.0) | (flags & F_SUN != 0))[0]
    with open(ref + '.part', 'w') as f:
        f.write('# stars.bin reference (build_stars.py): heliocentric Galactic position [pc] (epoch %.1f), velocity [km/s]\n' % EPOCH)
        f.write('index,id,hip,x,y,z,vx,vy,vz,mv,teff,av,flags\n')
        for i in sel:
            f.write('%d,%d,%d,%.15g,%.15g,%.15g,%.6g,%.6g,%.6g,%.4f,%.1f,%.4f,%d\n' % (
                i, ids[i], hip[i], pos[i, 0], pos[i, 1], pos[i, 2], vel[i, 0], vel[i, 1], vel[i, 2], mv[i], teff[i], av[i], flags[i]))
    os.replace(ref + '.part', ref)
    print(f'{ref}: {len(sel)} stars')

    vis = mv + 5 * np.log10(np.maximum(np.linalg.norm(pos, axis=1), 1e-6) / 10) + av
    print(f'{out}: {n} stars in {len(first)} sectors, {os.path.getsize(out) / 1e6:.0f} MB, {time.time() - t0:.0f} s')
    for lim in (6.5, 8, 9, 10, 11, 12):
        print(f'  V < {lim}: {(vis < lim).sum()}')


if __name__ == '__main__':
    main()
