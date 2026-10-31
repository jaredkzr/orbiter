"""Harris's catalogue of Milky Way globular clusters (1996, 2010 edition:
https://physics.mcmaster.ca/~harris/mwgc.dat), parsed.

clusters(path) -> list of dicts: name, ra, dec [deg], l, b [deg], dist
[pc], mv (M_V), feh, ebv, c (King concentration), rc, rh [arcmin]
(missing: nan). The catalogue's three parts list the clusters by their
identifier in fixed-width columns.
"""

import math
import re


def _part(lines, title):
    """the data rows of a part: from its column header to the next part"""
    start = next(i for i, l in enumerate(lines) if title in l)
    rows, inside = [], False
    for l in lines[start + 1:]:
        if re.match(r'\s+Part I', l):
            break
        if re.match(r'\s+ID\s', l):
            inside = True
            continue
        if inside and l.strip() and not l.startswith('_'):
            rows.append(l.rstrip('\n'))
    return rows


def _num(s):
    s = s.strip().rstrip('c').strip()
    try:
        return float(s)
    except ValueError:
        return math.nan


def clusters(path):
    lines = open(path, encoding='latin-1').readlines()
    out = {}
    for l in _part(lines, 'Part I:  Identifications'):
        cid = l[:12].strip()
        ra = l[25:38].split()
        dec = l[38:51].split()
        if len(ra) < 3 or len(dec) < 3:
            continue
        sgn = -1.0 if dec[0].startswith('-') else 1.0
        out[cid] = dict(id=cid, name=l[12:25].strip() or cid,
                        ra=15.0 * (float(ra[0]) + float(ra[1]) / 60 + float(ra[2]) / 3600),
                        dec=sgn * (abs(float(dec[0])) + float(dec[1]) / 60 + float(dec[2]) / 3600),
                        l=_num(l[51:60]), b=_num(l[60:68]), dist=_num(l[68:75]) * 1000.0)
    for l in _part(lines, 'Part II:  Metallicity'):
        cid = l[:12].strip()
        if cid in out:
            # (columns: [Fe/H] wt E(B-V) V_HB (m-M)V V_t M_V,t ...; blanks
            # where unknown: by position)
            out[cid].update(feh=_num(l[12:18]), ebv=_num(l[22:28]), mv=_num(l[46:54]))
    for l in _part(lines, 'Part III:  Velocities'):
        cid = l[:12].strip()
        if cid in out:
            out[cid].update(c=_num(l[48:56]), rc=_num(l[57:65]), rh=_num(l[65:72]))
    return list(out.values())
