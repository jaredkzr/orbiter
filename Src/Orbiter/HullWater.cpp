// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "HullWater.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace hydro {

namespace {

	const double MIN_CELL = 0.05;   // [m]
	const double PANEL_GRID = 12.0; // panels: the longest extent in this many patches (to start with)

	bool Inside (const convex::Polytope &P, const Vector &x, double tol)
	{
		if (x.dist2 (P.centre) > (P.radius + tol) * (P.radius + tol)) return false;
		for (const convex::Face &f : P.f)
			if (dotp (f.n, x) > f.d + tol) return false;
		return true;
	}

	// a unit normal's bin: the cube face of its largest component, 4 x 4 on it
	int NormalBin (const Vector &n)
	{
		double ax = fabs (n.x), ay = fabs (n.y), az = fabs (n.z);
		int face;
		double u, v, m;
		if (ax >= ay && ax >= az) face = n.x > 0 ? 0 : 1, m = ax, u = n.y, v = n.z;
		else if (ay >= az) face = n.y > 0 ? 2 : 3, m = ay, u = n.x, v = n.z;
		else face = n.z > 0 ? 4 : 5, m = az, u = n.x, v = n.y;
		int iu = std::clamp ((int)((u / m + 1.0) * 2.0), 0, 3), iv = std::clamp ((int)((v / m + 1.0) * 2.0), 0, 3);
		return face * 16 + iu * 4 + iv;
	}

	struct Sample { Vector p, n; double a; int part; };

}

Model Build (const std::vector<convex::Polytope> &hulls, const std::function<int(const Vector&)> &partAt)
{
	Model m;
	if (hulls.empty()) return m;
	Vector lo (1e30, 1e30, 1e30), hi (-1e30, -1e30, -1e30);
	for (const convex::Polytope &P : hulls)
		for (const Vector &v : P.v) {
			lo = Vector (std::min (lo.x, v.x), std::min (lo.y, v.y), std::min (lo.z, v.z));
			hi = Vector (std::max (hi.x, v.x), std::max (hi.y, v.y), std::max (hi.z, v.z));
			m.radius = std::max (m.radius, v.length());
		}
	Vector ext (hi - lo);
	m.length = std::max (ext.x, std::max (ext.y, ext.z));
	if (!(m.length > 0.0)) return m;
	auto part = [&](const Vector &p) { return partAt ? partAt (p) : -1; };

	// cells: the union in a grid (coarser until it fits); cells the hulls'
	// faces cut are sampled 8 x 8 x 8 for their filled share
	struct Box { Vector a, b; };
	std::vector<Box> hb;
	for (const convex::Polytope &P : hulls) {
		Box x { Vector (1e30, 1e30, 1e30), Vector (-1e30, -1e30, -1e30) };
		for (const Vector &v : P.v) {
			x.a = Vector (std::min (x.a.x, v.x), std::min (x.a.y, v.y), std::min (x.a.z, v.z));
			x.b = Vector (std::max (x.b.x, v.x), std::max (x.b.y, v.y), std::max (x.b.z, v.z));
		}
		hb.push_back (x);
	}
	double h = std::max (m.length / 64.0, MIN_CELL);
	std::vector<int> cand;
	std::vector<Cell> cells;
	for (int pass = 0; pass < 8; pass++) {
		int nx = (int)ceil (ext.x / h - 1e-9), ny = (int)ceil (ext.y / h - 1e-9), nz = (int)ceil (ext.z / h - 1e-9);
		nx = std::max (nx, 1), ny = std::max (ny, 1), nz = std::max (nz, 1);
		// (centred on the bodies' box)
		Vector o (lo + (ext - Vector (nx * h, ny * h, nz * h)) * 0.5);
		cells.clear();
		for (int i = 0; i < nx; i++) {
			double x0 = o.x + i * h, x1 = x0 + h;
			for (int j = 0; j < ny; j++) {
				double y0 = o.y + j * h, y1 = y0 + h;
				for (int k = 0; k < nz; k++) {
					double z0 = o.z + k * h, z1 = z0 + h;
					cand.clear();
					for (size_t q = 0; q < hulls.size(); q++)
						if (hb[q].a.x <= x1 && hb[q].b.x >= x0 && hb[q].a.y <= y1 && hb[q].b.y >= y0 && hb[q].a.z <= z1 && hb[q].b.z >= z0)
							cand.push_back ((int)q);
					if (cand.empty()) continue;
					auto in = [&](const Vector &p) {
						for (int q : cand) if (Inside (hulls[q], p, 0.0)) return true;
						return false;
					};
					// (the centre and the corners all in: full)
					Vector c (x0 + 0.5 * h, y0 + 0.5 * h, z0 + 0.5 * h);
					bool all = in (c);
					for (int q = 0; q < 8 && all; q++)
						all = in (Vector (q & 1 ? x1 : x0, q & 2 ? y1 : y0, q & 4 ? z1 : z0));
					if (all) { cells.push_back ({ c, 1.0, -1 }); continue; }
					const int S = 8; // (sub-samples per axis)
					int nin = 0;
					Vector cs;
					for (int a = 0; a < S; a++)
						for (int b = 0; b < S; b++)
							for (int d = 0; d < S; d++) {
								Vector p (x0 + (a + 0.5) / S * h, y0 + (b + 0.5) / S * h, z0 + (d + 0.5) / S * h);
								if (in (p)) nin++, cs += p;
							}
					if (nin) cells.push_back ({ cs / (double)nin, nin / (double)(S * S * S), -1 });
				}
			}
		}
		if (cells.size() <= (size_t)MAX_CELLS || pass == 7) break;
		h *= cbrt ((double)cells.size() / MAX_CELLS) * 1.03;
	}
	m.h = h;
	m.cells.swap (cells);
	double wsum = 0.0;
	Vector csum;
	for (Cell &c : m.cells) {
		c.part = part (c.c);
		wsum += c.w;
		csum += c.c * c.w;
	}
	// (thin bodies that no sample falls in: one cell at the hulls' centre, their volume)
	if (m.cells.empty()) {
		double vol = 0.0;
		Vector c;
		for (const convex::Polytope &P : hulls) vol += P.volume, c += P.centre * P.volume;
		if (vol > 0.0) c /= vol; else c = (lo + hi) * 0.5;
		m.h = cbrt (std::max (vol, 1e-6));
		m.cells.push_back ({ c, 1.0, part (c) });
		wsum = 1.0, csum = c;
	}
	m.volume = wsum * m.h * m.h * m.h;
	m.centroid = csum / wsum;

	// panels: the hulls' faces where they are the outside of the union
	std::vector<Sample> samples;
	const double s = std::max (h, MIN_CELL);
	for (size_t hi_ = 0; hi_ < hulls.size(); hi_++) {
		const convex::Polytope &P = hulls[hi_];
		for (const convex::Face &f : P.f) {
			if (f.idx.size() < 3) continue;
			const Vector &a = P.v[f.idx[0]];
			for (size_t t = 1; t + 1 < f.idx.size(); t++) {
				const Vector &b = P.v[f.idx[t]], &c = P.v[f.idx[t + 1]];
				double A = 0.5 * crossp (b - a, c - a).length();
				if (A <= 0.0) continue;
				double e = std::max ({ a.dist (b), b.dist (c), c.dist (a) });
				int n = std::clamp ((int)ceil (e / s), 1, 64);
				double sa = A / (n * n);
				Vector u ((b - a) / n), v ((c - a) / n);
				for (int i = 0; i < n; i++)
					for (int j = 0; j + i < n; j++) {
						// (the small triangles of the grid: upright, and inverted between them)
						for (int inv = 0; inv < 2; inv++) {
							if (inv && i + j + 1 >= n) continue;
							Vector p = inv ? a + u * (i + 2.0 / 3.0) + v * (j + 2.0 / 3.0) : a + u * (i + 1.0 / 3.0) + v * (j + 1.0 / 3.0);
							// (covered: just outside the face is inside another hull; or the
							// face lies on another's like it, of a lower index: counted once)
							Vector q (p + f.n * (0.25 * h)), qi (p - f.n * (0.01 * h));
							bool covered = false;
							for (size_t o = 0; o < hulls.size() && !covered; o++)
								if (o != hi_ && (Inside (hulls[o], q, 0.0) || (o < hi_ && Inside (hulls[o], qi, 0.0) && Inside (hulls[o], p, 1e-6 * h))))
									covered = true;
							if (!covered) samples.push_back ({ p, f.n, sa, -1 });
						}
					}
			}
		}
	}
	// (gathered into patches by place and direction)
	double P = std::max (m.length / PANEL_GRID, s);
	for (int pass = 0; pass < 10; pass++) {
		std::unordered_map<uint64_t, size_t> index;
		std::vector<Panel> pl;
		std::vector<Vector> nsum;
		for (const Sample &x : samples) {
			int ix = (int)floor ((x.p.x - lo.x) / P), iy = (int)floor ((x.p.y - lo.y) / P), iz = (int)floor ((x.p.z - lo.z) / P);
			uint64_t key = ((uint64_t)(ix & 0xfff) << 40) | ((uint64_t)(iy & 0xfff) << 28) | ((uint64_t)(iz & 0xfff) << 16) | (uint64_t)NormalBin (x.n);
			auto it = index.find (key);
			size_t k;
			if (it == index.end()) {
				k = pl.size();
				index[key] = k;
				pl.push_back ({ Vector(), Vector(), 0.0, 0.0, -1 });
				nsum.push_back (Vector());
			} else k = it->second;
			pl[k].c += x.p * x.a;
			pl[k].area += x.a;
			nsum[k] += x.n * x.a;
		}
		if (pl.size() > (size_t)MAX_PANELS && pass < 9) { P *= 1.3; continue; }
		m.area = 0.0;
		for (size_t k = 0; k < pl.size(); k++) {
			Panel &q = pl[k];
			q.c /= q.area;
			double nl = nsum[k].length();
			q.n = nl > 0.0 ? nsum[k] / nl : Vector (0, 1, 0);
			q.size = std::min (P, sqrt (q.area) * 1.5);
			q.part = part (q.c);
			m.area += q.area;
		}
		m.panels.swap (pl);
		break;
	}
	return m;
}

Model Sphere (const Vector &c, double r)
{
	Model m;
	r = std::max (r, 0.1);
	m.h = r * cbrt (4.0 * 3.14159265358979 / 3.0);
	m.cells.push_back ({ c, 1.0, -1 });
	m.volume = m.h * m.h * m.h;
	m.centroid = c;
	m.radius = c.length() + r;
	m.length = 2.0 * r;
	// (six panels about it)
	const Vector d[6] = { Vector (1,0,0), Vector (-1,0,0), Vector (0,1,0), Vector (0,-1,0), Vector (0,0,1), Vector (0,0,-1) };
	for (const Vector &n : d) m.panels.push_back ({ c + n * r, n, 4.0 * 3.14159265358979 * r * r / 6.0, r, -1 });
	m.area = 4.0 * 3.14159265358979 * r * r;
	return m;
}

double Submerged (const Model &m, const Vector &p0, const Vector &up, Vector *centroid)
{
	double vol = 0.0;
	Vector c;
	const double v = m.h * m.h * m.h;
	for (const Cell &x : m.cells) {
		double d = dotp (p0 - x.c, up); // (depth of the centre)
		double f = std::clamp (0.5 + d / m.h, 0.0, 1.0);
		if (f <= 0.0) continue;
		vol += f * x.w * v;
		c += x.c * (f * x.w * v);
	}
	if (centroid) *centroid = vol > 0.0 ? c / vol : m.centroid;
	return vol;
}

} // namespace hydro
