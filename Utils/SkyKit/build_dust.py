#!/usr/bin/env python3
"""Resample the Edenhofer et al. (2024) 3D dust map into nested Cartesian grids.

  build_dust.py [--cache DIR] [--out FILE] [--samples N]

The map gives dust density on HEALPix spheres (nside 256, nested) at
logarithmically spaced distances from the Sun. The sky renderer needs it
in a form a GPU can sample from any point: three nested grids centred on
the Sun in heliocentric Galactic coordinates (x toward the Galactic
centre, y toward l = 90 deg, z toward the north Galactic pole) [pc]:

  level 0:  2 pc cells, |x|,|y| < 256 pc,  |z| < 160 pc
  level 1:  4 pc cells, |x|,|y| < 640 pc,  |z| < 256 pc
  level 2:  8 pc cells, |x|,|y| < 2048 pc, |z| < 400 pc

Each voxel holds the mean extinction density A_V per pc over its volume
(stratified sub-samples), quantised logarithmically to 8 bits. Inside
1.1 kpc the 1.25 kpc map is used; beyond it, blended over to the 2 kpc
map, which ends at 1.99 kpc (zero beyond: the renderer switches to its
dust model there). Inside the first shell (69 pc) the map only gives the
integrated extinction per direction, spread evenly along the radius.

Output layout (little-endian):
  char[4] 'ODST', uint32 version (1), uint32 nlevel,
  float   log10 density of code 1, float log10 density of code 255,
  float   radius of map coverage [pc],
  per level: float cell [pc], uint32 nx, ny, nz, float origin x, y, z [pc]
             (corner of voxel 0)
  per level: uint8 codes, x fastest (nx*ny*nz)
"""

import argparse
import multiprocessing as mp
import os
import struct
import sys
import time

import numpy as np

from skykit import healpix

NSIDE = 256
NPIX = 12 * NSIDE * NSIDE
AV_PER_E = 2.8  # A_V per unit E of Zhang, Green & Rix (2023) (Edenhofer et al. 2024)
LEVELS = [  # cell, half extents
    (2.0, (256, 256, 160)),
    (4.0, (640, 640, 256)),
    (8.0, (2048, 2048, 400)),
]
LOG_MIN, LOG_MAX = -7.5, 0.0
BLEND = (1100.0, 1240.0)  # 1.25 kpc map -> 2 kpc map

_maps = {}


class DustSphere:
    def __init__(self, cache, tag, nshell):
        d = os.path.join(cache, 'edenhofer')
        raw = os.path.join(d, f'{tag}_mean.bin')
        native = os.path.join(d, f'{tag}_mean.f32')
        if not os.path.exists(native):
            a = np.memmap(raw, '>f4', 'r', shape=(nshell, NPIX))
            o = np.memmap(native, '<f4', 'w+', shape=(nshell, NPIX))
            for i in range(0, nshell, 32):
                o[i:i + 32] = a[i:i + 32]
            o.flush()
            del o
        self.mean = np.memmap(native, '<f4', 'r', shape=(nshell, NPIX))
        self.centers = np.fromfile(os.path.join(d, f'{tag}_centers.bin'), '>f4').astype(np.float64)
        self.bounds = np.fromfile(os.path.join(d, f'{tag}_bounds.bin'), '>f4').astype(np.float64)
        self.inner = np.fromfile(os.path.join(d, f'{tag}_inner.bin'), '>f4').astype(np.float64)
        assert len(self.centers) == nshell
        self.r0 = self.bounds[0]
        self.lnq = np.log(self.centers[1] / self.centers[0])

    def density(self, r, pix):
        """Dust density [E/pc] at radius r [pc] in pixel pix"""
        out = np.zeros(r.shape)
        inner = r < self.r0
        out[inner] = self.inner[pix[inner]] / self.r0
        sel = (r >= self.r0) & (r <= self.bounds[-1])
        f = np.log(r[sel] / self.centers[0]) / self.lnq
        f = np.clip(f, 0, len(self.centers) - 1.000001)
        i0 = f.astype(np.int64)
        w = f - i0
        p = pix[sel]
        out[sel] = (1 - w) * self.mean[i0, p] + w * self.mean[i0 + 1, p]
        return out


def _init(cache):
    _maps['near'] = DustSphere(cache, 'r1250', 516)
    _maps['far'] = DustSphere(cache, 'r2000', 654)


def density(x, y, z):
    r = np.sqrt(x * x + y * y + z * z)
    pix = healpix.vec2pix_nest(NSIDE, x, y, z)
    near, far = _maps['near'], _maps['far']
    w = np.clip((r - BLEND[0]) / (BLEND[1] - BLEND[0]), 0, 1)
    out = np.zeros(r.shape)
    a = w < 1
    out[a] += (1 - w[a]) * near.density(r[a], pix[a])
    b = w > 0
    out[b] += w[b] * far.density(r[b], pix[b])
    return out * AV_PER_E


def _slab(args):
    """Mean density of the voxels of one z layer"""
    cell, nx, ny, origin, k, ns, seed = args
    rng = np.random.default_rng(seed)
    gx, gy = np.meshgrid(np.arange(nx), np.arange(ny), indexing='xy')  # x fastest
    acc = np.zeros((ny, nx))
    n = 0
    for sx in range(ns):
        for sy in range(ns):
            for sz in range(ns):
                j = rng.random((3,) + gx.shape)
                x = origin[0] + (gx + (sx + j[0]) / ns) * cell
                y = origin[1] + (gy + (sy + j[1]) / ns) * cell
                z = np.full(gx.shape, origin[2] + (k + (sz + j[2][0, 0]) / ns) * cell)
                acc += density(x.ravel(), y.ravel(), z.ravel()).reshape(gx.shape)
                n += 1
    rho = acc / n
    code = np.zeros(rho.shape, np.uint8)
    pos = rho > 10 ** LOG_MIN
    lg = np.log10(rho[pos])
    code[pos] = np.clip(1 + np.rint(254 * (lg - LOG_MIN) / (LOG_MAX - LOG_MIN)), 1, 255).astype(np.uint8)
    return k, code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=os.path.expanduser('~/orbiter-data/skykit'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--samples', type=int, default=2, help='sub-samples per voxel axis')
    ap.add_argument('--jobs', type=int, default=max(1, os.cpu_count() - 2))
    a = ap.parse_args()

    _init(a.cache)
    coverage = float(_maps['far'].bounds[-1])
    levels = []
    for cell, half in LEVELS:
        dims = tuple(int(2 * h / cell) for h in half)
        origin = tuple(-float(h) for h in half)
        levels.append((cell, dims, origin))

    tmp = a.out + '.part'
    with open(tmp, 'wb') as f:
        f.write(b'ODST' + struct.pack('<II', 1, len(levels)))
        f.write(struct.pack('<fff', LOG_MIN, LOG_MAX, coverage))
        for cell, dims, origin in levels:
            f.write(struct.pack('<f3I3f', cell, *dims, *origin))
        with mp.Pool(a.jobs, _init, (a.cache,)) as pool:
            for li, (cell, (nx, ny, nz), origin) in enumerate(levels):
                t0 = time.time()
                grid = np.zeros((nz, ny, nx), np.uint8)
                tasks = [(cell, nx, ny, origin, k, a.samples, li * 100000 + k) for k in range(nz)]
                for k, code in pool.imap_unordered(_slab, tasks):
                    grid[k] = code
                f.write(grid.tobytes())
                nz_codes = grid[grid > 0]
                print(f'level {li}: {nx}x{ny}x{nz} cells of {cell} pc, {time.time() - t0:.0f} s, '
                      f'filled {nz_codes.size / grid.size:.3f}, median code {np.median(nz_codes):.0f}',
                      file=sys.stderr)
    os.replace(tmp, a.out)


if __name__ == '__main__':
    main()
