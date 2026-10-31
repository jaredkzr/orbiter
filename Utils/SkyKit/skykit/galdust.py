"""Dust extinction of the simulated sky, as the Vulkan client computes it.

A mirror of GalDust / GalExtinction in OVP/VulkanClient/Shaders/galaxy.glsl:
the Edenhofer et al. (2024) map resampled into dust.bin near the Sun,
blended into the dust model of galaxy.cfg beyond. Catalogue stars are
dereddened with it, so that the client, applying the same extinction,
shows them from the Sun as observed.

Positions: heliocentric Galactic [pc], arrays of shape (..., 3).
"""

import math
import struct

import numpy as np
from scipy import ndimage

# samples along a line of sight (midpoint rule; as GAL_EXT_STEPS)
EXT_STEPS = 160


class GalDust:
    def __init__(self, cfg_path, dust_path):
        self._load_cfg(cfg_path)
        self._load_map(dust_path)

    # --- galaxy.cfg: the dust model beyond the map
    def _load_cfg(self, path):
        self.R0, self.zsun = 8150.0, 20.8
        self.edge, self.edge_scale = 13000.0, 1500.0
        self.growth = 0.042
        self.arms = []
        dust = [0.0009, 3000.0, 110.0, 3.0, 2500.0]
        self.map_blend = [1700.0, 1990.0]
        self.ext = [1.40, 1.12, 0.94, 0.80]
        for line in open(path):
            line = line.split(';')[0].split()
            if not line:
                continue
            k, v = line[0], line[1:]
            if k == 'R0':
                self.R0 = float(v[0])
            elif k == 'ZSUN':
                self.zsun = float(v[0])
            elif k == 'EDGE':
                self.edge, self.edge_scale = float(v[0]), float(v[1])
            elif k == 'ARM':
                bk, Rk, p1, p2, w, b0, b1, st = map(float, v[1:9])
                self.arms.append((math.radians(bk), math.log(Rk), math.tan(math.radians(p1)),
                                  math.tan(math.radians(p2)), w, math.radians(b0), math.radians(b1), st))
            elif k == 'ARM_WIDTH_GROWTH':
                self.growth = float(v[0]) * 1e-3
            elif k == 'DUST':
                dust = [float(x) for x in v[:5]]
            elif k == 'DUST_MAP':
                self.map_blend = [float(v[0]), float(v[1])]
            elif k == 'EXTINCTION':
                self.ext = [float(x) for x in v[:4]]
        hole = 1.0 - math.exp(-(self.R0 / dust[4]) ** 4) if dust[4] > 0 else 1.0
        self.dust_mid = dust[0] / hole
        self.dust_hR, self.dust_hz, self.dust_arm, self.dust_hole = dust[1], dust[2], dust[3], dust[4]
        self.dust_armN = 1.0 + self.dust_arm * float(self.arm_profile(np.array([self.R0]), np.array([0.0]))[0])

    def arm_profile(self, R, beta):
        """GalArms (young: all arms alike)"""
        FADE = 0.349
        best = np.zeros_like(R)
        for bk, lnRk, tp1, tp2, w0, b0, b1, st in self.arms:
            for k in (-1, 0, 1):
                be = beta + 2.0 * math.pi * k
                ok = (be >= b0 - FADE) & (be <= b1 + FADE)
                tp = np.where(be <= bk, tp1, tp2)
                Ra = np.exp(lnRk - (be - bk) * tp)
                ok &= (Ra >= 2500.0) & (Ra <= 18000.0)
                fade = np.clip((be - b0) / FADE + 1.0, 0, 1) * np.clip((b1 - be) / FADE + 1.0, 0, 1)
                fade *= _smoothstep(2500.0, 3500.0, Ra) * (1.0 - _smoothstep(16000.0, 18000.0, Ra))
                w = w0 + self.growth * (Ra - math.exp(lnRk))
                d = (R - Ra) / np.sqrt(1.0 + tp * tp)
                best = np.maximum(best, np.where(ok, fade * np.exp(-0.5 * d * d / (w * w)), 0.0))
        return best

    def model(self, h):
        """GalDustModel [A_V / pc]"""
        gx, gy, gz = h[..., 0] - self.R0, h[..., 1], h[..., 2] + self.zsun
        R = np.hypot(gx, gy)
        C = 0.3
        v = np.exp(C - np.sqrt(gz * gz / (self.dust_hz ** 2) + C * C))
        rad = np.exp(-(R - self.R0) / self.dust_hR)
        rad = np.where(R > self.edge, rad * np.exp(-(R - self.edge) / self.edge_scale), rad)
        if self.dust_hole > 0:
            rad = rad * (1.0 - np.exp(-(R / self.dust_hole) ** 4))
        arm = (1.0 + self.dust_arm * self.arm_profile(R, np.arctan2(gy, -gx))) / self.dust_armN
        return np.where(v < 1e-6, 0.0, self.dust_mid * rad * v * arm)

    # --- dust.bin: the map
    def _load_map(self, path):
        with open(path, 'rb') as f:
            magic, ver, nlev = struct.unpack('<4sII', f.read(12))
            self.log_min, self.log_max, coverage = struct.unpack('<3f', f.read(12))
            assert magic == b'ODST' and ver == 1
            lv = [struct.unpack('<f3I3f', f.read(28)) for _ in range(nlev)]
            self.levels = []
            for cell, nx, ny, nz, ox, oy, oz in lv:
                a = np.frombuffer(f.read(nx * ny * nz), np.uint8).reshape(nz, ny, nx).astype(np.float32)
                self.levels.append((cell, np.array([nx, ny, nz], float), np.array([ox, oy, oz]), a))
        self.map_blend[1] = min(self.map_blend[1], coverage)
        self.map_blend[0] = min(self.map_blend[0], self.map_blend[1] - 1.0)

    def map(self, h):
        """GalDustMap [A_V / pc]; < 0 outside all grids"""
        h = h.reshape(-1, 3)
        out = np.full(len(h), -1.0)
        todo = np.ones(len(h), bool)
        for cell, n, origin, a in self.levels:
            u = (h - origin) / (cell * n)
            inside = todo & np.all((u >= 0) & (u < 1), axis=1)
            if not inside.any():
                continue
            t = u[inside] * n - 0.5  # (texel centres; clamp to edge)
            c = ndimage.map_coordinates(a, [t[:, 2], t[:, 1], t[:, 0]], order=1, mode='nearest')
            out[inside] = np.where(c < 1.0, c * 3.16e-8,
                                   10.0 ** (self.log_min + (c - 1.0) * ((self.log_max - self.log_min) / 254.0)))
            todo &= ~inside
        return out

    def density(self, h):
        """GalDust [A_V / pc]"""
        shape = h.shape[:-1]
        h = h.reshape(-1, 3)
        r = np.linalg.norm(h, axis=1)
        m = np.full(len(h), -1.0)
        near = r < self.map_blend[1]
        if near.any():
            m[near] = self.map(h[near])
        out = np.empty(len(h))
        far = m < 0
        if far.any():
            out[far] = self.model(h[far])
        w = _smoothstep(self.map_blend[0], self.map_blend[1], r)
        blend = ~far & (w > 0)
        out[~far] = m[~far]
        if blend.any():
            out[blend] = m[blend] + (self.model(h[blend]) - m[blend]) * w[blend]
        return out.reshape(shape)

    def extinction(self, a, b, chunk=20000):
        """A_V [mag] along the straight paths from a to b (GalExtinction)"""
        a = np.broadcast_to(a, b.shape)
        out = np.empty(len(b))
        t = (np.arange(EXT_STEPS) + 0.5) / EXT_STEPS
        for i in range(0, len(b), chunk):
            pa, pb = a[i:i + chunk], b[i:i + chunk]
            d = pb - pa
            pts = pa[:, None, :] + d[:, None, :] * t[None, :, None]
            rho = self.density(pts.astype(np.float32).astype(np.float64))
            out[i:i + chunk] = rho.sum(axis=1) * np.linalg.norm(d, axis=1) / EXT_STEPS
        return out


def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)
