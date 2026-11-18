#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License
"""
Pavement textures for base runways with markings (RUNWAY ... MARKINGS in a
base's file: Src/Orbiter/Baseobj.h): an atlas of VARIANTS x VARIANTS
different slabs, each 25 ft (7.62 m) square. The runway's texture
coordinates are in units of the atlas (u 0 on the centreline, v 0 at END1):
tiled as it is, its slabs repeat every 100 ft; the Vulkan client
(OVP/VulkanClient/Shaders/runway.glsl) picks a slab of the atlas for each
slab of the runway and turns or mirrors it (one of 8 ways), and draws the
markings, the slabs' shades, stains, rubber and the transverse grooves.

  RunwayConcrete: Portland cement concrete, sealed joints at the slabs'
                  edges, their edges curled
  RunwayAsphalt:  asphalt: aggregate (the slabs' edges not marked)
  PadPavers:      slabs of sintered regolith (landing pads off Earth), one
                  paver a slab, of nearly even tone (a cast surface, its
                  seams), neutral grey: the client tints them to the local
                  ground's colour (VObjects.cpp PAVER_MEAN: their mean, linear)

Writes Textures/<name>.dds (BC1, sRGB, full mip chain), <name>_rghn.dds
(smoothness in green, quarter size) and <name>_norm.dds (tangent-space
normals, BC5, half size: x along +u, y along +v).

Usage: make_pavement.py [--preview DIR] [--size N] [--only NAME]
"""

import argparse
import math
import os
import struct

import numpy as np
from PIL import Image

FT = 0.3048
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SLAB = 25 * FT  # [m] a slab (Runway::SLAB)
VARIANTS = 4    # slabs a side of the atlas (Runway::TEXSLABS; runway.glsl)

# colours (sRGB 0-1)
CONCRETE = np.array([0.70, 0.69, 0.66])
ASPHALT = np.array([0.33, 0.33, 0.32])
# smoothness (1 - roughness)
SM_CONCRETE, SM_ASPHALT, SM_JOINT = 0.12, 0.10, 0.30


# ----------------------------------------------------------------------
# noise (periodic over the tile: FFT-filtered white noise)

def noise(h, w, scale_px, rng, periodic=True):
    """Gaussian-filtered noise, unit standard deviation; features about
    scale_px (rows, cols) pixels; periodic over the tile (all tiles: the
    seams of the others are a faint low-frequency mismatch)"""
    periodic = True
    ph, pw = h, w
    n = rng.standard_normal((ph, pw)).astype(np.float32)
    fy = np.fft.fftfreq(ph)[:, None]
    fx = np.fft.rfftfreq(pw)[None, :]
    g = np.exp(-2 * (math.pi ** 2) * ((fy * scale_px[0]) ** 2 + (fx * scale_px[1]) ** 2))
    f = np.fft.irfft2(np.fft.rfft2(n) * g, s=(ph, pw)).astype(np.float32)
    if not periodic:
        oy, ox = int(scale_px[0] * 3), int(scale_px[1] * 3)
        f = f[oy:oy + h, ox:ox + w]
    f -= f.mean()  # (the white noise's mean survives the filter: a tile smaller than the scale would be offset)
    s = f.std()
    return f / s if s > 0 else f



# ----------------------------------------------------------------------
# DDS (BC1, BC5) writer

def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.maximum(c, 0), 1 / 2.4) - 0.055)


def mips(img, srgb):
    """the mip chain (box filter; colours averaged in linear light)"""
    levels = [img]
    cur = srgb_to_lin(img) if srgb else img
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        h, w = cur.shape[:2]
        h2, w2 = max(1, h // 2), max(1, w // 2)
        c = cur[:h2 * 2 if h > 1 else 1, :w2 * 2 if w > 1 else 1]
        if h > 1:
            c = 0.5 * (c[0::2] + c[1::2])
        if w > 1:
            c = 0.5 * (c[:, 0::2] + c[:, 1::2])
        cur = c.astype(np.float32)
        levels.append(lin_to_srgb(cur).astype(np.float32) if srgb else cur)
    return levels


def bc1_encode(img):
    """BC1 blocks of an RGB image (0-1), its sides padded to multiples of 4"""
    h, w = img.shape[:2]
    H, Wd = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (H, Wd) != (h, w):
        img = np.pad(img, ((0, H - h), (0, Wd - w), (0, 0)), mode='edge')
    out = bytearray()
    rows_per = 256
    for y0 in range(0, H, rows_per):
        sub = img[y0:y0 + rows_per]
        bh = sub.shape[0] // 4
        bw = Wd // 4
        blk = sub.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3) * 255.0
        mean = blk.mean(axis=1, keepdims=True)
        d = blk - mean
        cov = np.einsum('nki,nkj->nij', d, d)
        axis = np.ones((blk.shape[0], 3), np.float32) / math.sqrt(3)
        for _ in range(4):
            axis = np.einsum('nij,nj->ni', cov, axis)
            nrm = np.linalg.norm(axis, axis=1, keepdims=True)
            axis = np.where(nrm > 1e-6, axis / np.maximum(nrm, 1e-6), 1 / math.sqrt(3))
        proj = np.einsum('nki,ni->nk', d, axis)
        e0 = mean[:, 0] + axis * proj.max(axis=1, keepdims=True)
        e1 = mean[:, 0] + axis * proj.min(axis=1, keepdims=True)

        def to565(c):
            c = np.clip(np.round(c), 0, 255).astype(np.int32)
            return ((c[:, 0] * 31 + 127) // 255 << 11) | ((c[:, 1] * 63 + 127) // 255 << 5) | ((c[:, 2] * 31 + 127) // 255)

        def from565(v):
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], axis=1).astype(np.float32)
        c0, c1 = to565(e0), to565(e1)
        swap = c0 < c1
        c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
        p0, p1 = from565(c0), from565(c1)
        pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], axis=1)  # (n, 4, 3)
        dist = ((blk[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(axis=3)
        idx = dist.argmin(axis=2).astype(np.uint32)
        same = c0 == c1
        idx[same] = 0
        bits = np.zeros(blk.shape[0], np.uint32)
        for k in range(16):
            bits |= idx[:, k] << (2 * k)
        rec = np.zeros(blk.shape[0], dtype=[('c0', '<u2'), ('c1', '<u2'), ('b', '<u4')])
        rec['c0'], rec['c1'], rec['b'] = c0, c1, bits
        out += rec.tobytes()
    return bytes(out)


def bc4_blocks(ch):
    """BC4 blocks of one channel (n, 16) of 0-255 values: (n, 8) bytes"""
    v = ch.astype(np.float32)
    e0 = np.round(v.max(axis=1)).astype(np.int32)
    e1 = np.round(v.min(axis=1)).astype(np.int32)
    same = e0 == e1
    e0 = np.where(same & (e0 < 255), e0 + 1, e0)  # (e0 > e1: the 8-value mode)
    e1 = np.where(same & (e0 == e1), e1 - 1, e1)
    k = np.arange(8)
    w0 = np.array([7, 0, 6, 5, 4, 3, 2, 1], np.float32) / 7.0  # (index 0: e0, 1: e1, 2-7 between)
    pal = e0[:, None] * w0[None, :] + e1[:, None] * (1 - w0[None, :])
    idx = np.abs(v[:, :, None] - pal[:, None, :]).argmin(axis=2).astype(np.uint64)
    bits = np.zeros(v.shape[0], np.uint64)
    for i in range(16):
        bits |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((v.shape[0], 8), np.uint8)
    out[:, 0], out[:, 1] = e0, e1
    for b in range(6):
        out[:, 2 + b] = ((bits >> np.uint64(8 * b)) & np.uint64(255)).astype(np.uint8)
    return out


def bc5_encode(img):
    """BC5 blocks of a two-channel image (-1..1: normals' x and y)"""
    h, w = img.shape[:2]
    H, Wd = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (H, Wd) != (h, w):
        img = np.pad(img, ((0, H - h), (0, Wd - w), (0, 0)), mode='edge')
    v = np.clip((img * 0.5 + 0.5) * 255.0, 0, 255)
    blk = v.reshape(H // 4, 4, Wd // 4, 4, 2).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 2)
    return np.concatenate([bc4_blocks(blk[:, :, 0]), bc4_blocks(blk[:, :, 1])], axis=1).tobytes()


def write_dds(path, img, srgb=True, fmt='DXT1'):
    """a DDS file with the full mip chain: DXT1 (BC1: RGB, sRGB or linear) or
    ATI2 (BC5: two channels, -1..1, the mips averaged)"""
    levels = mips(img, srgb and fmt == 'DXT1')
    h, w = img.shape[:2]
    bb = 8 if fmt == 'DXT1' else 16
    hdr = struct.pack('<4sIIIIIII11I', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w,
                      ((w + 3) // 4) * ((h + 3) // 4) * bb, 0, len(levels), *([0] * 11))
    pf = struct.pack('<II4sIIIII', 32, 0x4, fmt.encode(), 0, 0, 0, 0, 0)
    caps = struct.pack('<IIIII', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(hdr + pf + caps)
        for lv in levels:
            f.write(bc1_encode(lv) if fmt == 'DXT1' else bc5_encode(lv))



# ----------------------------------------------------------------------
# the slabs

def interval_cover(c, half, a, b):
    """coverage of pixels centred at c (half-width half) by [a, b]"""
    return np.clip(np.minimum(c + half, b) - np.maximum(c - half, a), 0, None) / (2 * half)


def normals(height, du):
    """tangent-space normals from a periodic height field [m] (texel du [m])"""
    hgt = height.astype(np.float64)
    dhu = (np.roll(hgt, -1, axis=1) - np.roll(hgt, 1, axis=1)) / (2 * du)
    dhv = (np.roll(hgt, -1, axis=0) - np.roll(hgt, 1, axis=0)) / (2 * du)
    n = np.stack([-dhu, -dhv, np.ones_like(hgt)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n[..., :2].astype(np.float32)


def concrete(N, rng):
    du = SLAB / N
    big = noise(N, N, (1.5 / du, 1.5 / du), rng)
    mid = noise(N, N, (0.3 / du, 0.3 / du), rng)
    fine = noise(N, N, (0.02 / du, 0.02 / du), rng)
    # (low frequencies faint: tiled, they would repeat every slab; the shader varies the slabs)
    rgb = CONCRETE[None, None, :] * (1.0 + 0.004 * big + 0.012 * mid + 0.035 * fine)[..., None]
    # aggregate showing through: small darker and lighter specks
    speck = noise(N, N, (0.006 / du, 0.006 / du), rng)
    rgb *= (1.0 + 0.05 * np.clip(speck - 1.2, 0, None) - 0.06 * np.clip(-speck - 1.4, 0, None))[..., None]
    # joints at the slab's edges: 2.5 cm of dark sealant (half at each edge)
    c = (np.arange(N) + 0.5) * du
    jd = np.minimum(c, SLAB - c)
    cj1 = interval_cover(jd, du / 2, -0.0125, 0.0125)
    cj = np.maximum(cj1[:, None], cj1[None, :])
    rgb *= (1 - 0.55 * cj)[..., None]
    # relief: the slab's unevenness (curling at the edges), grain, the joints (the grooves: runway.glsl)
    curl = 0.0015 * (np.clip(1 - jd / 0.6, 0, 1) ** 2)
    curl2 = np.maximum(curl[:, None], curl[None, :])
    grain = noise(N, N, (0.01 / du, 0.01 / du), rng)
    height = curl2 + 0.00035 * grain - 0.006 * cj
    smooth = np.full((N, N), SM_CONCRETE, np.float32) * (1 + 0.15 * mid) * (1 - cj) + SM_JOINT * cj
    return np.clip(rgb, 0, 1), np.clip(smooth, 0, 1), height


def asphalt(N, rng):
    du = SLAB / N
    big = noise(N, N, (1.2 / du, 1.2 / du), rng)
    mid = noise(N, N, (0.15 / du, 0.15 / du), rng)
    agg = noise(N, N, (0.008 / du, 0.008 / du), rng)
    rgb = ASPHALT[None, None, :] * (1.0 + 0.004 * big + 0.015 * mid + 0.10 * np.tanh(agg))[..., None]
    # stones: lighter specks of aggregate
    st = np.clip(noise(N, N, (0.005 / du, 0.005 / du), rng) - 1.5, 0, None)
    rgb = rgb + (0.25 * st)[..., None] * 0.6
    height = 0.0012 * agg + 0.0004 * mid
    smooth = np.full((N, N), SM_ASPHALT, np.float32) * (1 + 0.2 * mid) + 0.1 * np.clip(st, 0, 1)
    return np.clip(rgb, 0, 1), np.clip(smooth, 0, 1), height


PAVERS = 1  # pavers a side of a slab (--pavers; 1: a slab each)
PAVER_SHADE = 0.012  # their shades' spread (--paver-shade: more, a checkerboard)


def pavers(N, rng):
    du = SLAB / N
    P = PAVERS
    n = N // P
    c = (np.arange(N) + 0.5) * du
    pc = SLAB / P
    e = np.minimum(c % pc, pc - c % pc)  # distance to the nearest joint [m]
    ed = np.minimum(e[:, None], e[None, :])
    # each paver its own shade (and a little hue), the joints sand-filled, darker
    shade = rng.normal(0, PAVER_SHADE, (P, P))
    hue = rng.normal(0, 0.0035, (P, P, 3))
    sh = np.repeat(np.repeat(shade, n, 0), n, 1)
    hu = np.repeat(np.repeat(hue, n, 0), n, 1)
    mid = noise(N, N, (0.08 / du, 0.08 / du), rng)
    fine = noise(N, N, (0.006 / du, 0.006 / du), rng)
    base = np.array([0.62, 0.61, 0.59])
    rgb = base[None, None, :] * (1.0 + sh + 0.03 * mid + 0.05 * fine)[..., None] + hu
    # glassy specks of the sintering
    gl = np.clip(noise(N, N, (0.004 / du, 0.004 / du), rng) - 1.8, 0, None)
    rgb += 0.15 * gl[..., None]
    joint = np.clip((0.008 - ed) / du + 0.5, 0, 1)  # (the joint's share of the texel: 1.6 cm wide)
    rgb *= (1 - 0.45 * joint)[..., None]
    # relief: chamfered edges (2 cm), each paver a little tilted, the surface's grain
    tilt = rng.normal(0, 0.002, (P, P, 2))
    lx = (c % pc) - pc / 2
    tx = np.repeat(np.repeat(tilt[..., 0], n, 0), n, 1) * lx[None, :]
    tz = np.repeat(np.repeat(tilt[..., 1], n, 0), n, 1) * lx[:, None]
    height = -0.006 * np.clip(1 - ed / 0.02, 0, 1) ** 2 + tx + tz + 0.0004 * fine
    smooth = np.full((N, N), 0.14, np.float32) + 0.2 * np.clip(gl, 0, 1) - 0.06 * joint
    return np.clip(rgb, 0, 1), np.clip(smooth, 0, 1), height


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--preview', help='write PNG previews into this directory')
    ap.add_argument('--only', help='one texture by name')
    ap.add_argument('--paver-shade', type=float, default=PAVER_SHADE, help='PadPavers: their shades\' spread (default %.3f)' % PAVER_SHADE)
    ap.add_argument('--pavers', type=int, default=PAVERS, help='PadPavers: pavers a side of a slab (default %d)' % PAVERS)
    ap.add_argument('--size', type=int, default=1024, help='texels a side of a slab (default 1024: 7.4 mm)')
    args = ap.parse_args()
    globals()['PAVERS'] = args.pavers
    globals()['PAVER_SHADE'] = args.paver_shade
    N, V = args.size, VARIANTS
    tex = os.path.join(ROOT, 'Textures')
    for name, fn, seed in (('RunwayConcrete', concrete, 1), ('RunwayAsphalt', asphalt, 2), ('PadPavers', pavers, 3)):
        if args.only and name != args.only:
            continue
        rgb = np.zeros((V * N, V * N, 3), np.float32)
        sm = np.zeros((V * N, V * N), np.float32)
        hgt = np.zeros((V * N, V * N), np.float32)
        for k in range(V * V):
            r, c = divmod(k, V)
            a, b, h = fn(N, np.random.default_rng(seed * 1000 + k))
            rgb[r * N:(r + 1) * N, c * N:(c + 1) * N] = a
            sm[r * N:(r + 1) * N, c * N:(c + 1) * N] = b
            hgt[r * N:(r + 1) * N, c * N:(c + 1) * N] = h
        nm = normals(hgt, SLAB / N)
        write_dds(os.path.join(tex, name + '.dds'), rgb, srgb=True)
        M = V * N
        q = sm.reshape(M // 4, 4, M // 4, 4).mean(axis=(1, 3))
        write_dds(os.path.join(tex, name + '_rghn.dds'), np.stack([np.zeros_like(q), q, np.zeros_like(q)], axis=2), srgb=False)
        nh = nm.reshape(M // 2, 2, M // 2, 2, 2).mean(axis=(1, 3))
        write_dds(os.path.join(tex, name + '_norm.dds'), nh, srgb=False, fmt='ATI2')
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            Image.fromarray((rgb[::4, ::4] * 255 + 0.5).astype(np.uint8)).save(os.path.join(args.preview, name + '.png'))
        print('%s: %d x %d slabs of %d x %d texels (%.1f mm); mean (linear) %.4f' % (name, V, V, N, N, SLAB / N * 1000, srgb_to_lin(rgb).mean()))


if __name__ == '__main__':
    main()
