#!/usr/bin/env python3
"""Download the source data of the simulated sky into a cache directory.

  fetch.py [--cache DIR] [dust] [gaia] [hip] [teff] [harris]

dust: Edenhofer et al. (2024) 3D dust map, posterior mean only (the 1.25 kpc
      map and the 2 kpc map), raw big-endian float32 plus the shell radii.
gaia: Gaia DR3 sources brighter than G = 12.5 and those nearer than 25 pc
      (parallax > 40 mas) at any brightness, the Bailer-Jones et al. (2021)
      distances of the bright ones (GAVO's mirror) and the Hipparcos
      identifiers of the Gaia sources (ESA archive).
hip:  Hipparcos new reduction (astrometry, Hp, colours) and the original
      catalogue's V magnitudes, spectral types and multiplicity.
teff: Pecaut & Mamajek's table of dwarf colours and temperatures.
harris: Harris's catalogue of Milky Way globular clusters (2010 edition).

The Hipparcos tables come from the ESA Gaia archive (the CDS mirror is
often unreachable). Files already present are skipped.
"""

import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

from skykit import fits

ZENODO = 'https://zenodo.org/records/10658339/files/'
DUST_MAPS = {
    'r1250': 'mean_and_std_healpix.fits',
    'r2000': 'validation_with_less_data_but_2kpc_mean_and_std_healpix.fits',
}
ESA_TAP = 'https://gea.esac.esa.int/tap-server/tap'
# GAVO's mirror of the Gaia DR3 astrometry and photometry: the ESA archive
# limits anonymous queries to 60 s and 50k rows (sync)
GAVO_TAP = 'https://dc.g-vo.org/tap'
GAIA_GMAX = 12.5
NEARBY_PLX = 40  # [mas]
MAMAJEK = 'https://www.pas.rochester.edu/~emamajek/EEM_dwarf_UBVIJHK_colors_Teff.txt'
HARRIS = 'https://physics.mcmaster.ca/~harris/mwgc.dat'  # Milky Way globular clusters (Harris 1996, 2010 edition)
GAIA_COLUMNS = ('source_id, ra, dec, parallax, parallax_error, pmra, pmdec, radial_velocity, '
                'phot_g_mean_mag, phot_bp_mean_mag, phot_rp_mean_mag, ruwe')


def download(url, path, offset=None, size=None):
    if os.path.exists(path) and (size is None or os.path.getsize(path) == size):
        print('have', path)
        return
    tmp = path + '.part'
    headers = {}
    if offset is not None:
        headers['Range'] = f'bytes={offset}-{offset + size - 1}'
    print('fetch', url, '->', path, f'({size} bytes)' if size else '')
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=300) as r, open(tmp, 'wb') as f:
                while True:
                    buf = r.read(1 << 22)
                    if not buf:
                        break
                    f.write(buf)
            if size is not None and os.path.getsize(tmp) != size:
                raise IOError(f'short read {os.path.getsize(tmp)} of {size}')
            os.replace(tmp, path)
            return
        except Exception as e:
            print('  retry:', e)
            time.sleep(5 * (attempt + 1))
    sys.exit(f'failed: {url}')


def fetch_dust(cache):
    d = os.path.join(cache, 'edenhofer')
    os.makedirs(d, exist_ok=True)
    for tag, name in DUST_MAPS.items():
        url = ZENODO + name
        meta_path = os.path.join(d, tag + '.json')
        if os.path.exists(meta_path):
            meta = json.load(open(meta_path))
        else:
            hdus = fits.walk(url)
            meta = {'source': url}
            for h in hdus:
                nm = h.name.lower()
                if nm == 'mean':
                    meta['mean'] = {'offset': h.data_offset, 'bytes': h.data_bytes, 'shape': h.shape(),
                                    'nside': int(h.header['NSIDE']), 'ordering': h.header['ORDERING'],
                                    'unit': h.header.get('CUNIT', '')}
                elif nm == 'radial pixel centers':
                    meta['centers'] = {'offset': h.data_offset, 'bytes': h.data_bytes}
                elif nm == 'radial pixel boundaries':
                    meta['bounds'] = {'offset': h.data_offset, 'bytes': h.data_bytes}
                elif nm.startswith('mean of integrated inner'):
                    meta['inner'] = {'offset': h.data_offset, 'bytes': h.data_bytes,
                                     'radius': float(h.header['CTYPE'].split()[-2])}
            json.dump(meta, open(meta_path, 'w'), indent=1)
        for part in ('centers', 'bounds', 'inner', 'mean'):
            p = meta[part]
            download(url, os.path.join(d, f'{tag}_{part}.bin'), p['offset'], p['bytes'])


def tap_sync(tap, query, path):
    """Run a (small) ADQL query; save the CSV result"""
    if os.path.exists(path):
        print('have', path)
        return
    data = urllib.parse.urlencode({'REQUEST': 'doQuery', 'LANG': 'ADQL', 'FORMAT': 'csv',
                                   'QUERY': query}).encode()
    print('query', path)
    with urllib.request.urlopen(urllib.request.Request(tap + '/sync', data=data), timeout=600) as r:
        body = r.read()
    if b'QUERY_STATUS' in body[:2000] and b'ERROR' in body[:2000]:
        sys.exit(body[:2000].decode())
    with open(path + '.part', 'wb') as f:
        f.write(body)
    os.replace(path + '.part', path)


def tap_async(tap, query, path):
    """Run an ADQL query as an asynchronous job; save the CSV result"""
    if os.path.exists(path):
        print('have', path)
        return
    data = urllib.parse.urlencode({'REQUEST': 'doQuery', 'LANG': 'ADQL', 'FORMAT': 'csv',
                                   'PHASE': 'RUN', 'MAXREC': 20000000, 'QUERY': query}).encode()
    req = urllib.request.Request(tap + '/async', data=data)
    opener = urllib.request.build_opener(NoRedirect)
    with opener.open(req, timeout=120) as r:
        job = r.headers['Location']
    print('job', job)
    # (some services ignore PHASE=RUN on creation)
    urllib.request.urlopen(urllib.request.Request(job + '/phase', data=b'PHASE=RUN'), timeout=60).close()
    while True:
        with urllib.request.urlopen(job + '/phase', timeout=60) as r:
            phase = r.read().decode().strip()
        if phase == 'COMPLETED':
            break
        if phase in ('ERROR', 'ABORTED'):
            with urllib.request.urlopen(job + '/error', timeout=60) as r:
                sys.exit(f'{phase}: {r.read().decode()[:2000]}')
        time.sleep(10)
    download(job + '/results/result', path)
    with open(path, 'rb') as f:
        head = f.read(2000)
    if b'QUERY_STATUS' in head and b'OVERFLOW' in head:
        sys.exit(f'{path}: result truncated')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None

    def http_error_303(self, req, fp, code, msg, headers):
        return fp
    http_error_302 = http_error_303


def fetch_gaia(cache):
    d = os.path.join(cache, 'gaia')
    os.makedirs(d, exist_ok=True)
    tap_async(GAVO_TAP, f'SELECT {GAIA_COLUMNS} FROM gaia.dr3lite WHERE phot_g_mean_mag < {GAIA_GMAX}',
              os.path.join(d, 'dr3.csv'))
    # the nearby stars, also the faint ones (most red dwarfs within 25 pc)
    tap_sync(GAVO_TAP, f'SELECT {GAIA_COLUMNS} FROM gaia.dr3lite WHERE parallax > {NEARBY_PLX}',
             os.path.join(d, 'nearby.csv'))
    # distances: posterior percentiles, geometric and photogeometric
    tap_async(GAVO_TAP, 'SELECT d.source_id, d.r_lo_geo, d.r_med_geo, d.r_hi_geo, d.r_lo_photogeo, d.r_med_photogeo, '
              'd.r_hi_photogeo FROM gedr3dist.main AS d JOIN gaia.dr3lite AS g ON d.source_id = g.source_id '
              f'WHERE g.phot_g_mean_mag < {GAIA_GMAX}', os.path.join(d, 'dist.csv'))
    # Hipparcos identifiers of the Gaia sources (small: synchronous, in parts)
    for i in range(0, 120000, 30000):
        tap_sync(ESA_TAP, 'SELECT source_id, original_ext_source_id AS hip FROM gaiadr3.hipparcos2_best_neighbour '
                 f'WHERE original_ext_source_id >= {i} AND original_ext_source_id < {i + 30000}',
                 os.path.join(d, f'hip_neighbour_{i // 30000}.csv'))


def fetch_hip(cache):
    d = os.path.join(cache, 'hip')
    os.makedirs(d, exist_ok=True)
    # (the ESA archive's synchronous queries return at most 50k rows)
    for i in range(0, 120000, 30000):
        rng = f'WHERE hip >= {i} AND hip < {i + 30000}'
        tap_sync(ESA_TAP, 'SELECT hip, ra, dec, plx, e_plx, pm_ra, pm_de, hp_mag, b_v, v_i, sn '
                 f'FROM public.hipparcos_newreduction {rng}', os.path.join(d, f'hip2_{i // 30000}.csv'))
        tap_sync(ESA_TAP, 'SELECT hip, vmag, b_v, v_i, sptype, ccdm, nsys, ncomp, multflag, hd '
                 f'FROM public.hipparcos {rng}', os.path.join(d, f'hip1997_{i // 30000}.csv'))


def fetch_teff(cache):
    d = os.path.join(cache, 'mamajek')
    os.makedirs(d, exist_ok=True)
    download(MAMAJEK, os.path.join(d, 'eem.txt'))


def fetch_harris(cache):
    d = os.path.join(cache, 'harris')
    os.makedirs(d, exist_ok=True)
    download(HARRIS, os.path.join(d, 'mwgc.dat'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=os.path.expanduser('~/orbiter-data/skykit'))
    ap.add_argument('what', nargs='*', default=['dust', 'gaia', 'hip', 'teff', 'harris'])
    a = ap.parse_args()
    for w in a.what:
        {'dust': fetch_dust, 'gaia': fetch_gaia, 'hip': fetch_hip, 'teff': fetch_teff, 'harris': fetch_harris}[w](a.cache)


if __name__ == '__main__':
    main()
