#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License
#
# Derived ISS textures for the Vulkan client (Textures/VK/ISS):
# - issbso*_cells: the sun-facing cell side of the main solar array wings
#   (dark blue cells under cover glass, light interconnect lines, gold
#   edges), derived from the panel structure of the stock blanket textures
#   (which show the bronze Kapton rear side)
# - issrad_white: radiator panels in their white coating
# Usage: make_iss_textures.py <orbiter root>
import struct, sys, os
import numpy as np
from PIL import Image, ImageFilter

def write_dds(path, img):
    """uncompressed 32-bit BGRA DDS with a full mipmap chain"""
    w, h = img.size
    mips = [img]
    while mips[-1].size != (1, 1):
        a, b = mips[-1].size
        mips.append(mips[-1].resize((max(1, a // 2), max(1, b // 2)), Image.BOX))
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8, h, w, w * 4, 0, len(mips))
    pf = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
    caps = struct.pack('<IIII4x', 0x1000 | 0x400000 | 0x8, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(hdr + pf + caps)
        for m in mips:
            a = np.asarray(m.convert('RGBA'))
            f.write(a[:, :, [2, 1, 0, 3]].tobytes())

def cells(src):
    rgb = np.asarray(src.convert('RGB')).astype(np.float32)
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    mean = np.asarray(Image.fromarray(lum.astype(np.uint8)).filter(ImageFilter.GaussianBlur(3))).astype(np.float32)
    detail = lum - mean
    # gold edge stripes and frames: saturated yellow in the source
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    gold = (r > 170) & (g > 125) & (b < 120) & (r - b > 90)
    # without isolated specks (only stripes and frames)
    pad = np.pad(gold, 1).astype(np.int32)
    n = sum(pad[1 + dy:pad.shape[0] - 1 + dy, 1 + dx:pad.shape[1] - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1))
    gold &= n >= 4
    navy = np.array([16, 22, 48], np.float32)
    out = navy + np.clip(detail, -40, 60)[..., None] * np.array([0.9, 0.95, 1.2], np.float32)
    # cells vary a little in tint
    out *= (0.9 + 0.2 * (mean / max(mean.max(), 1)))[..., None]
    out = np.where(gold[..., None], rgb, out)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))

def white(src):
    rgb = np.asarray(src.convert('RGB')).astype(np.float32)
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    ref = np.percentile(lum, 75)
    v = np.clip(232.0 * lum / ref, 0, 245)
    return Image.fromarray(np.repeat(v[..., None], 3, axis=2).astype(np.uint8))

root = sys.argv[1]
src = os.path.join(root, 'Src/Vessel/ISS/Textures')
dst = os.path.join(root, 'Textures/VK/ISS')
os.makedirs(dst, exist_ok=True)
for n in ('issbso', 'issbso2'):
    write_dds(os.path.join(dst, n + '_cells.dds'), cells(Image.open(os.path.join(src, n + '.dds'))))
write_dds(os.path.join(dst, 'issrad_white.dds'), white(Image.open(os.path.join(src, 'issrad.dds'))))
