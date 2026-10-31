"""HEALPix NESTED pixel indexing (Gorski et al. 2005), vectorised with numpy.

Only what the dust map needs: direction -> pixel and pixel -> centre.
"""

import numpy as np

_JRLL = np.array([2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4])
_JPLL = np.array([1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7])


def _spread(v):
    """Spread the bits of v to the even bit positions"""
    v = v.astype(np.int64)
    v = (v | (v << 16)) & 0x0000FFFF0000FFFF
    v = (v | (v << 8)) & 0x00FF00FF00FF00FF
    v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0F
    v = (v | (v << 2)) & 0x3333333333333333
    v = (v | (v << 1)) & 0x5555555555555555
    return v


def _compress(v):
    """Inverse of _spread: gather the even bits of v"""
    v = v & 0x5555555555555555
    v = (v | (v >> 1)) & 0x3333333333333333
    v = (v | (v >> 2)) & 0x0F0F0F0F0F0F0F0F
    v = (v | (v >> 4)) & 0x00FF00FF00FF00FF
    v = (v | (v >> 8)) & 0x0000FFFF0000FFFF
    v = (v | (v >> 16)) & 0x00000000FFFFFFFF
    return v


def vec2pix_nest(nside, x, y, z):
    """Pixel containing direction (x, y, z) (need not be normalised)"""
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    z = np.asarray(z, np.float64)
    r = np.sqrt(x * x + y * y + z * z)
    r = np.where(r > 0, r, 1.0)
    zc = z / r
    za = np.abs(zc)
    phi = np.arctan2(y, x)
    phi = np.where(phi < 0, phi + 2 * np.pi, phi)
    tt = phi * (2 / np.pi)  # [0, 4)
    tt = np.where(tt >= 4, tt - 4, tt)

    face = np.empty(x.shape, np.int64)
    ix = np.empty(x.shape, np.int64)
    iy = np.empty(x.shape, np.int64)

    eq = za <= 2.0 / 3.0
    # equatorial belt
    t1 = nside * (0.5 + tt[eq])
    t2 = nside * (zc[eq] * 0.75)
    jp = (t1 - t2).astype(np.int64)  # ascending edge line
    jm = (t1 + t2).astype(np.int64)  # descending edge line
    ifp = jp // nside
    ifm = jm // nside
    f = np.where(ifp == ifm, ifp | 4, np.where(ifp < ifm, ifp, ifm + 8))
    face[eq] = f
    ix[eq] = jm & (nside - 1)
    iy[eq] = nside - (jp & (nside - 1)) - 1
    # polar caps
    po = ~eq
    ntt = np.minimum(tt[po].astype(np.int64), 3)
    tp = tt[po] - ntt
    tmp = nside * np.sqrt(3 * (1 - za[po]))
    jp = np.minimum((tp * tmp).astype(np.int64), nside - 1)
    jm = np.minimum(((1.0 - tp) * tmp).astype(np.int64), nside - 1)
    north = zc[po] >= 0
    face[po] = np.where(north, ntt, ntt + 8)
    ix[po] = np.where(north, nside - jm - 1, jp)
    iy[po] = np.where(north, nside - jp - 1, jm)

    return face * nside * nside + (_spread(ix) | (_spread(iy) << 1))


def pix2vec_nest(nside, pix):
    """Unit vector to the centre of each pixel"""
    pix = np.asarray(pix, np.int64)
    npface = nside * nside
    face = pix // npface
    ipf = pix % npface
    ix = _compress(ipf)
    iy = _compress(ipf >> 1)
    jr = _JRLL[face] * nside - ix - iy - 1
    nr = np.where(jr < nside, jr, np.where(jr > 3 * nside, 4 * nside - jr, nside))
    z = np.where(jr < nside, 1 - nr * nr / (3.0 * npface),
                 np.where(jr > 3 * nside, nr * nr / (3.0 * npface) - 1,
                          (2 * nside - jr) * 2.0 / (3.0 * nside)))
    kshift = np.where((jr >= nside) & (jr <= 3 * nside), (jr - nside) & 1, 0)
    jp = (_JPLL[face] * nr + ix - iy + 1 + kshift) // 2
    jp = np.where(jp > 4 * nside, jp - 4 * nside, jp)
    jp = np.where(jp < 1, jp + 4 * nside, jp)
    phi = (jp - (kshift + 1) * 0.5) * (np.pi / 2 / nr)
    s = np.sqrt(np.maximum(0.0, 1 - z * z))
    return np.cos(phi) * s, np.sin(phi) * s, z
