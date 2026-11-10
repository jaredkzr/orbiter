// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "HullWater.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>

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

	// cells: the union in a grid (coarser until it fits). A cell any hull
	// reaches into is a wall; the empty cells reached from outside the grid
	// (a flood fill through the cells no hull reaches) are the water's; the
	// other empty cells are sealed inside (a hollow hull's cabin): full.
	// Wall cells are sampled 8 x 8 x 8 for their filled share, and are full
	// where they border the sealed inside.
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
	int nx = 0, ny = 0, nz = 0;
	Vector o;
	std::vector<uint8_t> state; // 0 empty, 1 wall, 2 water (outside), 3 sealed
	std::vector<double> wv;     // filled share
	std::vector<Vector> cv;     // centre of the filled share
	auto idx = [&](int i, int j, int k) { return ((size_t)i * ny + j) * nz + k; };
	for (int pass = 0; pass < 8; pass++) {
		// (a cell of margin about the bodies: the flood starts there)
		nx = (int)ceil (ext.x / h - 1e-9) + 2, ny = (int)ceil (ext.y / h - 1e-9) + 2, nz = (int)ceil (ext.z / h - 1e-9) + 2;
		o = lo + (ext - Vector (nx * h, ny * h, nz * h)) * 0.5;
		state.assign ((size_t)nx * ny * nz, 0);
		wv.assign ((size_t)nx * ny * nz, 0.0);
		cv.assign ((size_t)nx * ny * nz, Vector (0, 0, 0));
		for (int i = 0; i < nx; i++) {
			double x0 = o.x + i * h, x1 = x0 + h;
			for (int j = 0; j < ny; j++) {
				double y0 = o.y + j * h, y1 = y0 + h;
				for (int k = 0; k < nz; k++) {
					double z0 = o.z + k * h, z1 = z0 + h;
					cand.clear();
					for (size_t q = 0; q < hulls.size(); q++) {
						if (!(hb[q].a.x <= x1 && hb[q].b.x >= x0 && hb[q].a.y <= y1 && hb[q].b.y >= y0 && hb[q].a.z <= z1 && hb[q].b.z >= z0)) continue;
						// (separated by one of the hull's face planes: the cell's corners all outside it)
						bool sep = false;
						for (const convex::Face &f : hulls[q].f) {
							double dmin = 1e30;
							for (int c = 0; c < 8; c++)
								dmin = std::min (dmin, dotp (f.n, Vector (c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0)));
							if (dmin > f.d) { sep = true; break; }
						}
						if (!sep) cand.push_back ((int)q);
					}
					if (cand.empty()) continue;
					size_t id = idx (i, j, k);
					state[id] = 1;
					auto in = [&](const Vector &p) {
						for (int q : cand) if (Inside (hulls[q], p, 0.0)) return true;
						return false;
					};
					Vector c (x0 + 0.5 * h, y0 + 0.5 * h, z0 + 0.5 * h);
					bool all = in (c);
					for (int q = 0; q < 8 && all; q++)
						all = in (Vector (q & 1 ? x1 : x0, q & 2 ? y1 : y0, q & 4 ? z1 : z0));
					if (all) { wv[id] = 1.0, cv[id] = c; continue; }
					const int S = 8; // (sub-samples per axis)
					int nin = 0;
					Vector cs;
					for (int a = 0; a < S; a++)
						for (int b = 0; b < S; b++)
							for (int d = 0; d < S; d++) {
								Vector p (x0 + (a + 0.5) / S * h, y0 + (b + 0.5) / S * h, z0 + (d + 0.5) / S * h);
								if (in (p)) nin++, cs += p;
							}
					wv[id] = nin / (double)(S * S * S);
					cv[id] = nin ? cs / (double)nin : c;
				}
			}
		}
		// mirror symmetry about x = 0 (most vessels: their hulls, a mesh's convex
		// decomposition, are not): both sides of each pair of cells take the
		// union of their walls and the larger filling
		m.symmetric = false;
		if (fabs (0.5 * (lo.x + hi.x)) < 0.25 * h) {
			double wt = 0.0, wm = 0.0;
			for (int i = 0; i < nx; i++)
				for (int j = 0; j < ny; j++)
					for (int k = 0; k < nz; k++) {
						double a = wv[idx (i, j, k)], b = wv[idx (nx - 1 - i, j, k)];
						wt += a, wm += std::min (a, b);
					}
			if (wt > 0.0 && wm > 0.8 * wt) {
				m.symmetric = true;
				for (int i = 0; i < nx / 2; i++)
					for (int j = 0; j < ny; j++)
						for (int k = 0; k < nz; k++) {
							size_t a = idx (i, j, k), b = idx (nx - 1 - i, j, k);
							uint8_t st = std::max (state[a], state[b]);
							state[a] = state[b] = st;
							bool fa = wv[a] >= wv[b];
							double w = fa ? wv[a] : wv[b];
							Vector c = fa ? cv[a] : cv[b];
							wv[a] = wv[b] = w;
							cv[a] = (c.x < 0.0) == (a < b) ? c : Vector (-c.x, c.y, c.z);
							cv[b] = Vector (-cv[a].x, cv[a].y, cv[a].z);
						}
			}
		}
		// the water's cells: flood from the margin through the empty cells
		std::vector<size_t> stack;
		for (int i = 0; i < nx; i++)
			for (int j = 0; j < ny; j++)
				for (int k = 0; k < nz; k++)
					if ((i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1) && state[idx (i, j, k)] == 0)
						state[idx (i, j, k)] = 2, stack.push_back (idx (i, j, k));
		while (!stack.empty()) {
			size_t id = stack.back(); stack.pop_back();
			int k = (int)(id % nz), j = (int)((id / nz) % ny), i = (int)(id / ((size_t)nz * ny));
			const int d[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
			for (auto &e : d) {
				int a = i + e[0], b = j + e[1], c = k + e[2];
				if (a < 0 || b < 0 || c < 0 || a >= nx || b >= ny || c >= nz) continue;
				size_t n2 = idx (a, b, c);
				if (state[n2] == 0) state[n2] = 2, stack.push_back (n2);
			}
		}
		cells.clear();
		for (int i = 0; i < nx; i++)
			for (int j = 0; j < ny; j++)
				for (int k = 0; k < nz; k++) {
					size_t id = idx (i, j, k);
					Vector c (o.x + (i + 0.5) * h, o.y + (j + 0.5) * h, o.z + (k + 0.5) * h);
					if (state[id] == 0) { state[id] = 3; cells.push_back ({ c, 1.0, -1 }); continue; } // (sealed)
					if (state[id] != 1) continue;
					// (a wall bordering the sealed inside: full)
					bool sealed = false;
					const int d[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
					for (auto &e : d) {
						int a = i + e[0], b = j + e[1], cc = k + e[2];
						if (a < 0 || b < 0 || cc < 0 || a >= nx || b >= ny || cc >= nz) continue;
						uint8_t st = state[idx (a, b, cc)];
						if (st == 0 || st == 3) { sealed = true; break; }
					}
					if (sealed) cells.push_back ({ c, 1.0, -1 });
					else if (wv[id] > 0.0) cells.push_back ({ cv[id], wv[id], -1 });
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
	// (whether a point is in the water's cells)
	auto outside = [&](const Vector &p) {
		int i = (int)floor ((p.x - o.x) / h), j = (int)floor ((p.y - o.y) / h), k = (int)floor ((p.z - o.z) / h);
		if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return true;
		return state[idx (i, j, k)] == 2;
	};

	// panels: the faces between the body's cells and the water's, the
	// outside of the union as the grid has it (symmetric where the body is:
	// the hulls of a mesh's convex decomposition are not). Each face's normal
	// is the smoothed gradient of the cells' filling (not the grid's axis),
	// its area the share of the surface it stands for (its axis's part of
	// the normal: the steps of a sloped surface add up to its area), its
	// place moved onto the surface by its cell's filling.
	auto fill = [&](int i, int j, int k) {
		if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return 0.0;
		size_t id = idx (i, j, k);
		uint8_t st = state[id];
		if (st == 2) return 0.0;
		if (st == 3) return 1.0;
		// (a wall bordering the sealed inside is full: as the cells)
		return std::max (wv[id], 0.0);
	};
	std::vector<Sample> samples;
	const double s = h;
	for (int i = 0; i < nx; i++)
		for (int j = 0; j < ny; j++)
			for (int k = 0; k < nz; k++) {
				size_t id = idx (i, j, k);
				if (state[id] == 2 || state[id] == 0) continue;
				// (the smoothed gradient of the filling about the cell: Sobel-like)
				Vector g;
				for (int a = -1; a <= 1; a++)
					for (int b = -1; b <= 1; b++)
						for (int c = -1; c <= 1; c++) {
							double f = fill (i + a, j + b, k + c);
							double wgt = (a == 0 ? 2.0 : 1.0) * (b == 0 ? 2.0 : 1.0) * (c == 0 ? 2.0 : 1.0);
							g += Vector (a, b, c) * (f * wgt);
						}
				double gl = g.length();
				const int d[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
				for (auto &e : d) {
					int a = i + e[0], b = j + e[1], c = k + e[2];
					bool water = a < 0 || b < 0 || c < 0 || a >= nx || b >= ny || c >= nz || state[idx (a, b, c)] == 2;
					if (!water) continue;
					Vector ax (e[0], e[1], e[2]);
					// (a thin plate, water on both sides of the cell: the gradient lies in
					// the plate, and the face's own axis is right; elsewhere the gradient,
					// the risers of the grid's steps on a sloped surface taking their small
					// share of it)
					int ra = i - e[0], rb = j - e[1], rc = k - e[2];
					bool thin = ra < 0 || rb < 0 || rc < 0 || ra >= nx || rb >= ny || rc >= nz || state[idx (ra, rb, rc)] == 2;
					Vector n = (thin || gl <= 1e-9) ? ax : g / -gl;
					if (dotp (n, ax) <= 0.02) continue;
					double area = h * h * dotp (n, ax);
					if (area <= 0.0) continue;
					// (onto the surface: from the filled share's centre, half its thickness out)
					double w = state[id] == 3 ? 1.0 : std::max (wv[id], 0.0);
					Vector base = state[id] == 3 || w >= 1.0 ? Vector (o.x + (i + 0.5) * h, o.y + (j + 0.5) * h, o.z + (k + 0.5) * h) : cv[id];
					Vector p (base + ax * (0.5 * h * std::max (w, 0.05)));
					samples.push_back ({ p, n, area, -1 });
				}
			}
	// (a symmetric body: what lies on its plane, half to either side)
	if (m.symmetric) {
		size_t n0 = samples.size();
		for (size_t q = 0; q < n0; q++)
			if (fabs (samples[q].p.x) < 1e-6 * h) {
				samples[q].a *= 0.5;
				Sample t = samples[q];
				t.p.x = -1e-9 * h;
				samples[q].p.x = 1e-9 * h;
				samples.push_back (t);
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
			// (a symmetric body: bins mirrored about x = 0, none across it)
			if (m.symmetric) ix = (x.p.x >= 0.0 ? 1 : -1) * (1 + (int)floor (fabs (x.p.x) / P)) + 2048;
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
		// (mirror pairs next to each other: the solver's sweeps, forward and
		// back in turn, then favour neither side)
		if (m.symmetric)
			std::stable_sort (pl.begin(), pl.end(), [](const Panel &a, const Panel &b) {
				double ax = std::round (fabs (a.c.x) * 100.0), bx = std::round (fabs (b.c.x) * 100.0);
				if (ax != bx) return ax < bx;
				if (a.c.y != b.c.y) return a.c.y < b.c.y;
				if (a.c.z != b.c.z) return a.c.z < b.c.z;
				return a.c.x < b.c.x;
			});
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
