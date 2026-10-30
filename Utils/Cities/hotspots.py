#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License
"""
Cities of a planet from its night lights: the catalogue of city centres the
core's procedural cities (Src/Orbiter/Cities.h) grow around.

The night lights are the RGB of the planet's Mask tiles (Textures/<planet>/
Archive/Mask.tree; alpha: water). They are a halftone: built-up ground is
speckled with lit texels at about the same density in a city's core as in
its suburbs, so local brightness says where the ground is built up, not how
densely. A city's size is the lit area of its agglomeration; its centre is
the peak of the lit share smoothed over a few km.

    hotspots.py <Orbiter data root> <planet> <output> [--preview <png>]

Output lines: lat lng [deg], lit area [km2], peak (smoothed lit share 0-1),
metro (lit area about it [km2]), main centre of its metro (0/1).
"""

import argparse
import struct
import sys
import zlib

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

LEVEL = 10          # tile level read (global coverage on Earth: ~600 m/texel)
DOWN = 4            # texels per raster cell (2.4 km)
LIT_R = 16          # lit texel: red above this (0-255), and not blue
SIGMA_SHAPE = 1.0   # [cells] agglomerations: smoothed lit share ...
SHAPE_MIN = 0.12    # ... above this
SIGMA_CENTRE = 2.0  # [cells] centres: peaks of the lit share smoothed this much
PEAK_WINDOW = 7     # [cells] a centre is the highest point within this window
PEAK_MIN = 0.12     # ... and at least this high
AREA_MIN = 1.0      # [km2] smallest catalogued town (smaller ones: villages)
SIGMA_METRO = 6.0   # [cells] metro size: lit area around a centre, weighted this wide (15 km)
PRIMARY_RADIUS = 16 # [cells] a metro's main centre: the largest metro within 40 km


class Tree:
    """An Orbiter tile tree archive (ZTreeMgr)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        magic = self.f.read(4)
        if magic[:2] != b'TX':
            raise ValueError(path + ': not a tile tree')
        size, _flags, self.data_ofs = struct.unpack('<3I', self.f.read(12))
        self.data_len, = struct.unpack('<q', self.f.read(8))
        n, _r1, _r2, _r3, r4a, r4b = struct.unpack('<6I', self.f.read(24))
        self.root = (r4a, r4b)
        self.f.seek(size)
        raw = np.frombuffer(self.f.read(32 * n), dtype=np.dtype([('pos', '<i8'), ('size', '<u4'), ('child', '<u4', 4), ('pad', '<u4')]))
        self.pos = raw['pos'].astype(np.int64)
        self.child = raw['child']

    def idx(self, lvl, ilat, ilng):
        if lvl == 4:
            return int(self.root[ilng])
        p = self.idx(lvl - 1, ilat // 2, ilng // 2)
        if p == 0xffffffff:
            return p
        return int(self.child[p][((ilat & 1) << 1) + (ilng & 1)])

    def read(self, i):
        end = self.pos[i + 1] if i + 1 < len(self.pos) else self.data_len
        self.f.seek(self.data_ofs + int(self.pos[i]))
        return zlib.decompress(self.f.read(int(end - self.pos[i])))


def decode_dxt1(raw):
    """DXT1 DDS -> (lit bool, water bool) arrays: lit texels are lights
    (yellow to white, red at least LIT_R); the dim blue speckle on some
    deserts and salt flats is not."""
    h, w = struct.unpack('<2I', raw[12:20])
    d = np.frombuffer(raw[128:128 + w * h // 2], dtype=np.uint8).reshape(h // 4, w // 4, 8)
    c0 = d[..., 0].astype(np.uint32) | (d[..., 1].astype(np.uint32) << 8)
    c1 = d[..., 2].astype(np.uint32) | (d[..., 3].astype(np.uint32) << 8)
    bits = (d[..., 4].astype(np.uint32) | (d[..., 5].astype(np.uint32) << 8)
            | (d[..., 6].astype(np.uint32) << 16) | (d[..., 7].astype(np.uint32) << 24))
    four = c0 > c1
    lit = np.zeros((h, w), bool)
    water = np.zeros((h, w), bool)
    pals = []
    for shift, bits_, mx in ((11, 31, 31), (0, 31, 31)):  # red, blue
        a = ((c0 >> shift) & bits_) * 255.0 / mx
        b = ((c1 >> shift) & bits_) * 255.0 / mx
        pals.append(np.stack([a, b, np.where(four, (2 * a + b) / 3, (a + b) / 2), np.where(four, (a + 2 * b) / 3, 0.0)], -1))
    for k in range(16):
        s = (bits >> (2 * k)) & 3
        r = np.take_along_axis(pals[0], s[..., None], -1)[..., 0]
        b = np.take_along_axis(pals[1], s[..., None], -1)[..., 0]
        lit[k // 4::4, k % 4::4] = (r > LIT_R) & (r >= 0.6 * b)
        water[k // 4::4, k % 4::4] = (s == 3) & ~four
    return lit, water


def lit_raster(tree):
    """Global raster of the lit share (cells of DOWN x DOWN texels of level
    LEVEL). Where the tree has no tile of that level (Earth: poleward of
    56 deg) the finest one above it, scaled up."""
    nlat, nlng = 1 << (LEVEL - 4), 2 << (LEVEL - 4)
    cells = 512 // DOWN
    lit = np.zeros((nlat * cells, nlng * cells), np.float32)
    n = 0
    for ilat in range(nlat):
        for ilng in range(nlng):
            for up in range(0, LEVEL - 4):
                lvl = LEVEL - up
                i = tree.idx(lvl, ilat >> up, ilng >> up)
                if i != 0xffffffff:
                    break
            if i == 0xffffffff:
                continue
            raw = tree.read(i)
            if len(raw) < 128 or raw[84:88] != b'DXT1':
                continue
            on, water = decode_dxt1(raw)
            on = (on & ~water).astype(np.float32)
            if up:  # (the part of the coarser tile, scaled up)
                k = 512 >> up
                y0, x0 = (ilat & ((1 << up) - 1)) * k, (ilng & ((1 << up) - 1)) * k
                on = np.kron(on[y0:y0 + k, x0:x0 + k], np.ones((1 << up, 1 << up), np.float32))
            lit[ilat * cells:(ilat + 1) * cells, ilng * cells:(ilng + 1) * cells] = on.reshape(cells, DOWN, cells, DOWN).mean((1, 3))
            n += 1
        print(f'\rtiles: row {ilat + 1}/{nlat}, {n} read', end='', file=sys.stderr)
    print(file=sys.stderr)
    return lit


def smooth(a, sigma):
    # (wraps in longitude)
    return ndimage.gaussian_filter(a, sigma, mode=('nearest', 'wrap'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root')
    ap.add_argument('planet')
    ap.add_argument('output')
    ap.add_argument('--preview')
    arg = ap.parse_args()

    tree = Tree(f'{arg.root}/Textures/{arg.planet}/Archive/Mask.tree')
    lit = lit_raster(tree)
    H, W = lit.shape
    R = 6371.0 if arg.planet == 'Earth' else float(input('radius [km]: '))
    lat = 90.0 - (np.arange(H) + 0.5) * 180.0 / H
    cell_km2 = (np.pi * R / H) * (2 * np.pi * R / W) * np.cos(np.radians(lat))  # per row

    shape = smooth(lit, SIGMA_SHAPE) > SHAPE_MIN
    label, ncomp = ndimage.label(shape, structure=np.ones((3, 3)))
    # (labels across the 180 deg meridian: merged)
    for y in np.nonzero(label[:, 0] & label[:, -1])[0]:
        a, b = label[y, 0], label[y, -1]
        if a and b and a != b:
            label[label == b] = a
    area = ndimage.sum(lit * cell_km2[:, None], label, index=np.arange(ncomp + 1))

    S = smooth(lit, SIGMA_CENTRE)
    metro = smooth(lit * cell_km2[:, None], SIGMA_METRO) * (2 * np.pi * SIGMA_METRO ** 2)
    peak = (S == ndimage.maximum_filter(S, size=PEAK_WINDOW, mode=('nearest', 'wrap'))) & (S > PEAK_MIN) & (label > 0)
    py, px = np.nonzero(peak)
    print(f'{ncomp} agglomerations, {len(py)} centres', file=sys.stderr)

    # each agglomeration's lit area shared out among its centres: each lit
    # cell to the nearest centre of its agglomeration
    lab = label[py, px]
    out = []
    order = np.argsort(lab)
    py, px, lab = py[order], px[order], lab[order]
    starts = np.searchsorted(lab, np.arange(ncomp + 2))
    objs = ndimage.find_objects(label)
    for c in range(1, ncomp + 1):
        s0, s1 = starts[c], starts[c + 1]
        if s1 == s0:
            continue
        cy, cx = py[s0:s1], px[s0:s1]
        if s1 - s0 == 1:
            shares = [area[c]]
        else:
            sl = objs[c - 1]
            yy, xx = np.nonzero(label[sl] == c)
            yy = yy + sl[0].start
            xx = xx + sl[1].start
            w = lit[yy, xx] * cell_km2[yy]
            cl = np.cos(np.radians(lat[cy]))
            _, near = cKDTree(np.c_[cx * cl, cy]).query(np.c_[xx * np.cos(np.radians(lat[yy])), yy])
            shares = np.bincount(near, weights=w, minlength=len(cy))
        for y, x, a in zip(cy, cx, shares):
            if a < AREA_MIN:
                continue
            la = lat[y]
            lo = -180.0 + (x + 0.5) * 360.0 / W
            out.append([la, lo, a, S[y, x], metro[y, x], 0])
    # main centres: the largest metro within PRIMARY_RADIUS
    km = np.pi * R / H
    pts = np.array([[np.radians(o[0]), np.radians(o[1])] for o in out])
    xyz = np.c_[np.cos(pts[:, 0]) * np.cos(pts[:, 1]), np.cos(pts[:, 0]) * np.sin(pts[:, 1]), np.sin(pts[:, 0])] * R
    kd = cKDTree(xyz)
    for i, o in enumerate(out):
        near = kd.query_ball_point(xyz[i], PRIMARY_RADIUS * km)
        o[5] = int(all(out[j][4] <= o[4] for j in near))
    out.sort(key=lambda r: -r[4])
    with open(arg.output, 'w') as f:
        f.write(f'; city centres of {arg.planet} from its night lights (Utils/Cities/hotspots.py)\n')
        f.write('; lat lng [deg]  lit area [km2] (its share)  peak lit share  metro [km2] (lit area about it, 15 km)  main centre (1: largest metro within 40 km)\n')
        for la, lo, a, p, m, pr in out:
            f.write(f'{la:.4f} {lo:.4f} {a:.1f} {p:.3f} {m:.0f} {pr}\n')
    print(f'{len(out)} cities written; largest metros: ' + ', '.join(f'({o[0]:.2f},{o[1]:.2f}) {o[4]:.0f} km2' for o in out[:12]), file=sys.stderr)

    if arg.preview:
        from PIL import Image, ImageDraw
        img = Image.fromarray((np.clip(lit, 0, 1) * 255).astype(np.uint8)).convert('RGB')
        dr = ImageDraw.Draw(img)
        for la, lo, a, *_ in out:
            y = (90.0 - la) / 180.0 * H
            x = (lo + 180.0) / 360.0 * W
            r = max(1.0, a ** 0.25)
            dr.ellipse([x - r, y - r, x + r, y + r], outline=(255, 60, 30))
        img.save(arg.preview)


if __name__ == '__main__':
    main()
