"""Minimal FITS reading: the header cards and data location of each HDU.

Enough to locate the image and table extensions of the dust map files
(local or remote by HTTP range requests) without astropy.
"""

import urllib.request

BLOCK = 2880


class Hdu:
    def __init__(self, offset, header, data_offset, data_bytes):
        self.offset = offset            # of the header [bytes]
        self.header = header            # keyword -> value (str)
        self.data_offset = data_offset  # [bytes]
        self.data_bytes = data_bytes    # without padding

    @property
    def name(self):
        return self.header.get('EXTNAME', '')

    def shape(self):
        n = int(self.header.get('NAXIS', 0))
        # FITS axes are fastest first: numpy order is reversed
        return tuple(int(self.header[f'NAXIS{i}']) for i in range(n, 0, -1))


def _value(card):
    v = card[10:]
    if v.lstrip().startswith("'"):
        s = v.lstrip()[1:]
        return s[:s.find("'")].rstrip()
    return v.split('/')[0].strip()


class Reader:
    """Read byte ranges from a local file or URL"""

    def __init__(self, src):
        self.src = src
        self.remote = src.startswith('http://') or src.startswith('https://')

    def read(self, offset, n):
        if self.remote:
            req = urllib.request.Request(self.src, headers={'Range': f'bytes={offset}-{offset + n - 1}'})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        with open(self.src, 'rb') as f:
            f.seek(offset)
            return f.read(n)


def walk(src):
    """List the HDUs of a FITS file (path or URL)"""
    rd = Reader(src)
    hdus = []
    off = 0
    while True:
        header = {}
        pos = off
        done = False
        while not done:
            try:
                blk = rd.read(pos, BLOCK)
            except urllib.error.HTTPError as e:
                if e.code == 416:  # past the end
                    return hdus
                raise
            if len(blk) < BLOCK:
                return hdus
            pos += BLOCK
            for i in range(36):
                card = blk[i * 80:(i + 1) * 80].decode('ascii', 'replace')
                key = card[:8].strip()
                if key == 'END':
                    done = True
                    break
                if card[8:10] == '= ':
                    header[key] = _value(card)
        nax = int(header.get('NAXIS', 0))
        n = 0
        if nax:
            n = abs(int(header['BITPIX'])) // 8
            for i in range(nax):
                n *= int(header[f'NAXIS{i + 1}'])
            n += int(header.get('PCOUNT', 0))
        hdus.append(Hdu(off, header, pos, n))
        off = pos + (n + BLOCK - 1) // BLOCK * BLOCK
