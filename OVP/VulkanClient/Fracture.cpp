// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fracture.h"
#include "VesselAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <unordered_map>

namespace fracture {

namespace {
	struct Key {
		int64_t x, y, z;
		bool operator== (const Key &k) const { return x == k.x && y == k.y && z == k.z; }
	};
	struct KeyHash {
		size_t operator() (const Key &k) const {
			uint64_t h = (uint64_t)k.x * 0x9E3779B97F4A7C15ull;
			h ^= (uint64_t)k.y * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
			h ^= (uint64_t)k.z * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
			return (size_t)h;
		}
	};
	struct Edge {
		int kept = 0, lost = 0;
		size_t tri = 0; uint32_t a = 0, b = 0; // (a kept triangle with it, and its corners in that triangle's order)
	};
	double Hash01 (uint32_t a, uint32_t seed)
	{
		uint32_t h = a * 2654435761u ^ seed * 2246822519u;
		h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
		return (h & 0xffffff) / (double)0x1000000;
	}
	// a wall reaches inward to a skin within this times its depth (else
	// inward leads out of the vessel)
	const double WALL_REACH = 40.0;

	/// The triangles in cells of a grid, for rays from points of the skin
	class Grid {
	public:
		Grid (const std::vector<Tri> &tris, double cell): tris(tris), cell(cell), stamp(tris.size(), 0)
		{
			for (size_t t = 0; t < tris.size(); t++) {
				Key lo = KeyOf (tris[t].p[0]), hi = lo;
				for (int j = 1; j < 3; j++) {
					Key k = KeyOf (tris[t].p[j]);
					lo = { std::min (lo.x, k.x), std::min (lo.y, k.y), std::min (lo.z, k.z) };
					hi = { std::max (hi.x, k.x), std::max (hi.y, k.y), std::max (hi.z, k.z) };
				}
				if ((hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1) > 4096) { big.push_back (t); continue; }
				for (int64_t x = lo.x; x <= hi.x; x++)
					for (int64_t y = lo.y; y <= hi.y; y++)
						for (int64_t z = lo.z; z <= hi.z; z++) cells[{ x, y, z }].push_back (t);
			}
		}
		/// Distance along a ray (|d| = 1) from o to the nearest triangle
		/// beyond eps (either side), maxDist if none within it
		double RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double eps)
		{
			double best = maxDist;
			ray++;
			for (size_t t : big) Test (tris[t], o, d, eps, best);
			// (along the ray, the cells about each point a cell apart: when
			// done with one, all triangles the ray meets before it are tested)
			int steps = (int)std::ceil (maxDist / cell);
			for (int k = 0; k <= steps && best > (k - 1) * cell; k++) {
				Key c = KeyOf (o + d * (k * cell));
				for (int64_t x = c.x - 1; x <= c.x + 1; x++)
					for (int64_t y = c.y - 1; y <= c.y + 1; y++)
						for (int64_t z = c.z - 1; z <= c.z + 1; z++) {
							auto it = cells.find ({ x, y, z });
							if (it != cells.end())
								for (size_t t : it->second)
									if (stamp[t] != ray) stamp[t] = ray, Test (tris[t], o, d, eps, best);
						}
			}
			return best;
		}
	private:
		Key KeyOf (const VECTOR3 &p) const
		{
			return { (int64_t)std::floor (p.x / cell), (int64_t)std::floor (p.y / cell), (int64_t)std::floor (p.z / cell) };
		}
		static void Test (const Tri &tr, const VECTOR3 &o, const VECTOR3 &d, double eps, double &best)
		{
			// (Moeller-Trumbore)
			VECTOR3 e1 = tr.p[1] - tr.p[0], e2 = tr.p[2] - tr.p[0];
			VECTOR3 p = crossp (d, e2);
			double det = dotp (e1, p);
			if (std::fabs (det) < 1e-12) return;
			double inv = 1.0 / det;
			VECTOR3 q = o - tr.p[0];
			double u = dotp (q, p) * inv;
			if (u < 0.0 || u > 1.0) return;
			VECTOR3 qe = crossp (q, e1);
			double v = dotp (d, qe) * inv;
			if (v < 0.0 || u + v > 1.0) return;
			double t = dotp (e2, qe) * inv;
			if (t > eps && t < best) best = t;
		}
		const std::vector<Tri> &tris;
		double cell;
		std::unordered_map<Key, std::vector<size_t>, KeyHash> cells;
		std::vector<size_t> big; // (triangles over many cells: tested by every ray)
		std::vector<uint32_t> stamp; // (the last ray each was tested by)
		uint32_t ray = 0;
	};

	NTVERTEX Vertex (const VECTOR3 &p, const VECTOR3 &n)
	{
		NTVERTEX v{};
		v.x = (float)p.x, v.y = (float)p.y, v.z = (float)p.z;
		v.nx = (float)n.x, v.ny = (float)n.y, v.nz = (float)n.z;
		return v;
	}

	VECTOR3 Pos (const NTVERTEX &v) { return _V(v.x, v.y, v.z); }

	/// From a to b (t: 0 to 1): position, normal, texture coordinates
	NTVERTEX Lerp (const NTVERTEX &a, const NTVERTEX &b, double t)
	{
		NTVERTEX v{};
		VECTOR3 p = Pos (a) + (Pos (b) - Pos (a)) * t;
		VECTOR3 n = _V(a.nx, a.ny, a.nz) + (_V(b.nx, b.ny, b.nz) - _V(a.nx, a.ny, a.nz)) * t;
		double l = length (n);
		if (l > 1e-9) n /= l;
		v.x = (float)p.x, v.y = (float)p.y, v.z = (float)p.z;
		v.nx = (float)n.x, v.ny = (float)n.y, v.nz = (float)n.z;
		v.tu = (float)(a.tu + (b.tu - a.tu) * t);
		v.tv = (float)(a.tv + (b.tv - a.tv) * t);
		return v;
	}

	/// Smooth noise (-1 to 1) with features about 1 apart
	double Noise (const VECTOR3 &p, uint32_t seed)
	{
		double fx = std::floor (p.x), fy = std::floor (p.y), fz = std::floor (p.z);
		int64_t x = (int64_t)fx, y = (int64_t)fy, z = (int64_t)fz;
		double u[3] = { p.x - fx, p.y - fy, p.z - fz };
		for (double &w : u) w = w * w * (3.0 - 2.0 * w);
		auto at = [&] (int i, int j, int k) {
			uint32_t h = (uint32_t)(x + i) * 73856093u ^ (uint32_t)(y + j) * 19349663u ^ (uint32_t)(z + k) * 83492791u;
			return 2.0 * Hash01 (h, seed) - 1.0;
		};
		double r = 0.0;
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 2; k++)
					r += (i ? u[0] : 1.0 - u[0]) * (j ? u[1] : 1.0 - u[1]) * (k ? u[2] : 1.0 - u[2]) * at (i, j, k);
		return r;
	}

	// the torn surface: the parts' boundaries displaced by noise of two
	// scales (amplitude, wavelength, in the structure's cells), followed to
	// this fraction of a cell
	const double TEAR[2][2] = { { 0.6, 2.5 }, { 0.2, 0.6 } };
	const double CUT_RES = 0.5;
}

bool CutTriangles ()
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv ("ORBITER_VK_FRACTURE");
		on = (e && !strcmp (e, "walls")) ? 0 : 1;
	}
	return on != 0;
}

Field::Field (const VESSEL *v, double size, uint32_t seed): v(v), seed(seed)
{
	cell = std::clamp (size / 25.0, 0.05, 2.0); // (as the structure's: Src/Orbiter/Structure.cpp)
	h = CUT_RES * cell;
}

int Field::PartAt (const VECTOR3 &p) const
{
	VECTOR3 d{};
	for (int o = 0; o < 2; o++) {
		double a = TEAR[o][0] * cell, f = 1.0 / (TEAR[o][1] * cell);
		VECTOR3 q = p * f;
		d += _V(Noise (q, seed + 7 * o + 1), Noise (q, seed + 7 * o + 2), Noise (q, seed + 7 * o + 3)) * a;
	}
	return v->GetPartAt (p + d);
}

int Cut (const NTVERTEX *v, const VECTOR3 &ofs, const std::function<bool (const VECTOR3 &)> &side, double h,
	std::vector<NTVERTEX> &keep, std::vector<NTVERTEX> &lose)
{
	double emax = std::max ({ length (Pos (v[1]) - Pos (v[0])), length (Pos (v[2]) - Pos (v[1])), length (Pos (v[0]) - Pos (v[2])) });
	int n = 1; // (a power of 2: the grid is divided in halves)
	while (n < emax / h && n < 128) n *= 2;
	// the corners of a grid over it, and their sides
	auto at = [n] (int i, int j) { return i * (n + 1) + j; };
	std::vector<NTVERTEX> gv ((n + 1) * (n + 1));
	std::vector<char> gs (gv.size());
	bool any[2] = { false, false };
	for (int i = 0; i <= n; i++)
		for (int j = 0; i + j <= n; j++) {
			NTVERTEX &x = gv[at (i, j)];
			double u = (double)i / n, w = (double)j / n;
			x = i + j == 0 ? v[0] : Lerp (v[0], Lerp (v[1], v[2], w / (u + w)), u + w);
			gs[at (i, j)] = side (Pos (x) + ofs) ? 1 : 0;
			any[gs[at (i, j)]] = true;
		}
	if (!any[0]) return 0;
	if (!any[1]) return 1;
	// where the side changes between two corners (the same whichever
	// triangle the edge is in: from the first of them by position)
	auto crossing = [&] (const NTVERTEX &a, bool sa, const NTVERTEX &b) {
		bool swap = std::tie (b.x, b.y, b.z) < std::tie (a.x, a.y, a.z);
		const NTVERTEX &p0 = swap ? b : a, &p1 = swap ? a : b;
		bool s0 = swap ? !sa : sa;
		double lo = 0.0, hi = 1.0;
		for (int it = 0; it < 16; it++) {
			double mid = 0.5 * (lo + hi);
			if (side (Pos (p0) + (Pos (p1) - Pos (p0)) * mid + ofs) == s0) lo = mid;
			else hi = mid;
		}
		return Lerp (p0, p1, 0.5 * (lo + hi));
	};
	auto emit = [&] (const NTVERTEX &a, const NTVERTEX &b, const NTVERTEX &c, bool s) {
		std::vector<NTVERTEX> &o = s ? keep : lose;
		o.push_back (a), o.push_back (b), o.push_back (c);
	};
	auto tri = [&] (int a, int b, int c) { // (in the triangle's winding)
		if (gs[a] == gs[b] && gs[b] == gs[c]) { emit (gv[a], gv[b], gv[c], gs[a]); return; }
		// the corner alone on its side first
		int q[3] = { a, b, c };
		int k = gs[a] != gs[b] && gs[a] != gs[c] ? 0 : gs[b] != gs[a] && gs[b] != gs[c] ? 1 : 2;
		int A = q[k], B = q[(k + 1) % 3], C = q[(k + 2) % 3];
		NTVERTEX ab = crossing (gv[A], gs[A], gv[B]), ac = crossing (gv[A], gs[A], gv[C]);
		emit (gv[A], ab, ac, gs[A]);
		emit (ab, gv[B], gv[C], gs[B]);
		emit (ab, gv[C], ac, gs[B]);
	};
	// in halves where the side changes within (whole where it does not:
	// fewer triangles away from the cut)
	std::function<void (int, int, int, bool)> part = [&] (int i, int j, int s, bool up) {
		// (up: corners (i,j) (i+s,j) (i,j+s); down: (i+s,j) (i+s,j+s) (i,j+s))
		int a = up ? at (i, j) : at (i + s, j), b = up ? at (i + s, j) : at (i + s, j + s), c = at (i, j + s);
		if (s == 1) { tri (a, b, c); return; }
		bool same = true;
		for (int x = 0; x <= s && same; x++)
			for (int y = 0; y <= s && same; y++)
				if (up ? x + y <= s : x + y >= s) same = gs[at (i + x, j + y)] == gs[a];
		if (same) { emit (gv[a], gv[b], gv[c], gs[a]); return; }
		int h2 = s / 2;
		if (up) {
			part (i, j, h2, true), part (i + h2, j, h2, true), part (i, j + h2, h2, true), part (i, j, h2, false);
		} else {
			part (i + h2, j, h2, false), part (i + h2, j + h2, h2, false), part (i, j + h2, h2, false), part (i + h2, j + h2, h2, true);
		}
	};
	part (0, 0, n, true);
	return 2;
}

double WallDepth (double vesselSize)
{
	return std::clamp (0.012 * vesselSize, 0.04, 0.4);
}

void Walls (const std::vector<Tri> &tris, double depth, double weld, uint32_t seed,
	std::vector<std::vector<NTVERTEX>> &out)
{
	// weld the corners (meshes repeat vertices along seams of normals and
	// texture coordinates, and groups do not share vertices)
	std::unordered_map<Key, uint32_t, KeyHash> ids;
	std::vector<VECTOR3> pos;
	std::vector<uint32_t> corner (tris.size() * 3);
	for (size_t t = 0; t < tris.size(); t++)
		for (int j = 0; j < 3; j++) {
			const VECTOR3 &p = tris[t].p[j];
			Key k{ (int64_t)std::floor (p.x / weld + 0.5), (int64_t)std::floor (p.y / weld + 0.5), (int64_t)std::floor (p.z / weld + 0.5) };
			auto it = ids.emplace (k, (uint32_t)pos.size());
			if (it.second) pos.push_back (p);
			corner[t * 3 + j] = it.first->second;
		}
	// the edges, and the triangles on each side
	struct Use { uint32_t a, b; size_t tri; }; // (an edge of a triangle, in its order)
	std::vector<Use> uses;
	for (size_t t = 0; t < tris.size(); t++)
		for (int j = 0; j < 3; j++) {
			uint32_t a = corner[t * 3 + j], b = corner[t * 3 + (j + 1) % 3];
			if (a != b) uses.push_back ({ a, b, t });
		}
	auto keyOf = [] (uint32_t a, uint32_t b) { return (uint64_t)std::min (a, b) << 32 | std::max (a, b); };
	std::unordered_map<uint64_t, Edge> edges;
	auto count = [&] () {
		edges.clear ();
		for (const Use &u : uses) {
			Edge &e = edges[keyOf (u.a, u.b)];
			if (tris[u.tri].kept) e.kept++, e.tri = u.tri, e.a = u.a, e.b = u.b;
			else e.lost++;
		}
	};
	count ();
	// edges of one triangle only: other triangles' corners on them (cut
	// finer there; T-junctions) divide them into the pieces those share
	{
		std::vector<size_t> single;
		for (size_t i = 0; i < uses.size(); i++) {
			const Edge &e = edges[keyOf (uses[i].a, uses[i].b)];
			if (e.kept + e.lost == 1) single.push_back (i);
		}
		std::vector<uint32_t> ends;
		double len = 0.0;
		for (size_t i : single) {
			ends.push_back (uses[i].a), ends.push_back (uses[i].b);
			len += length (pos[uses[i].b] - pos[uses[i].a]);
		}
		std::sort (ends.begin(), ends.end());
		ends.erase (std::unique (ends.begin(), ends.end()), ends.end());
		if (!single.empty()) {
			double cell = std::max (len / single.size(), 100.0 * weld);
			auto cellOf = [cell] (const VECTOR3 &p) {
				return Key{ (int64_t)std::floor (p.x / cell), (int64_t)std::floor (p.y / cell), (int64_t)std::floor (p.z / cell) };
			};
			std::unordered_map<Key, std::vector<uint32_t>, KeyHash> grid;
			for (uint32_t id : ends) grid[cellOf (pos[id])].push_back (id);
			std::vector<Use> more;
			std::vector<uint8_t> split (uses.size(), 0);
			std::vector<std::pair<double, uint32_t>> on;
			for (size_t i : single) {
				const Use &u = uses[i];
				VECTOR3 a = pos[u.a], d = pos[u.b] - a;
				double l = length (d);
				if (l < 2.0 * weld) continue;
				on.clear ();
				int steps = (int)std::ceil (l / cell);
				for (int k = 0; k <= steps; k++) {
					Key c = cellOf (a + d * ((double)k / steps));
					for (int64_t x = c.x - 1; x <= c.x + 1; x++)
						for (int64_t y = c.y - 1; y <= c.y + 1; y++)
							for (int64_t z = c.z - 1; z <= c.z + 1; z++) {
								auto it = grid.find ({ x, y, z });
								if (it == grid.end()) continue;
								for (uint32_t id : it->second) {
									if (id == u.a || id == u.b) continue;
									double t = dotp (pos[id] - a, d) / (l * l);
									if (t <= 0.0 || t >= 1.0) continue;
									if (length (a + d * t - pos[id]) < weld) on.push_back ({ t, id });
								}
							}
				}
				if (on.empty()) continue;
				std::sort (on.begin(), on.end());
				on.erase (std::unique (on.begin(), on.end()), on.end()); // (found from more than one cell)
				split[i] = 1;
				uint32_t prev = u.a;
				for (auto &o : on) { more.push_back ({ prev, o.second, u.tri }); prev = o.second; }
				more.push_back ({ prev, u.b, u.tri });
			}
			if (!more.empty()) {
				std::vector<Use> kept;
				for (size_t i = 0; i < uses.size(); i++)
					if (!split[i]) kept.push_back (uses[i]);
				kept.insert (kept.end(), more.begin(), more.end());
				uses.swap (kept);
				count ();
			}
		}
	}
	// the cut edges
	struct Cut { size_t tri; uint32_t a, b; };
	std::vector<Cut> cuts;
	for (auto &it : edges) {
		const Edge &e = it.second;
		if (e.kept == 1 && (e.lost > 0 || tris[e.tri].open)) cuts.push_back ({ e.tri, e.a, e.b });
	}
	if (cuts.empty()) return;
	std::sort (cuts.begin(), cuts.end(), [] (const Cut &a, const Cut &b) {
		return std::tie (a.tri, a.a, a.b) < std::tie (b.tri, b.a, b.b); }); // (the same walls each time)
	// inward at each corner on the cut: against the normals of the skin there
	std::unordered_map<uint32_t, VECTOR3> inward;
	for (const Cut &c : cuts)
		for (uint32_t id : { c.a, c.b })
			inward[id] += tris[c.tri].n;
	for (auto &it : inward) {
		double l = length (it.second);
		it.second = l > 1e-9 ? it.second * (-1.0 / l) : _V(0, 0, 0);
	}
	// how deep at each corner: the depth varies along the cut (torn, not
	// sawn), at most to near the middle between this skin and the one
	// facing it inside; none where inward leads out of the vessel (the
	// skin's normals do not point out there, or a thin plate)
	std::unordered_map<uint32_t, double> deep;
	double reach = WALL_REACH * depth;
	Grid grid (tris, std::max (reach / 16.0, 4.0 * weld));
	for (auto &it : inward) {
		uint32_t id = it.first;
		const VECTOR3 &d = it.second;
		double t = grid.RayHit (pos[id], d, reach, weld);
		deep[id] = t < reach ? std::min (depth * (0.55 + 0.9 * Hash01 (id, seed)), 0.45 * t) : 0.0;
	}
	auto bottom = [&] (uint32_t id) { return pos[id] + inward[id] * deep[id]; };
	for (const Cut &c : cuts) {
		const Tri &t = tris[c.tri];
		uint32_t ia = c.a, ib = c.b;
		VECTOR3 a = pos[ia], b = pos[ib], o = (t.p[0] + t.p[1] + t.p[2]) / 3.0;
		if (deep[ia] <= 0.0 || deep[ib] <= 0.0) continue;
		VECTOR3 a1 = bottom (ia), b1 = bottom (ib);
		// facing out of the cut (away from the rest of its triangle)
		VECTOR3 w = crossp (t.n, b - a);
		double l = length (w);
		if (l < 1e-12) continue;
		w /= l;
		if (dotp (w, o - a) > 0.0) w = -w;
		if (t.src >= out.size()) out.resize (t.src + 1);
		std::vector<NTVERTEX> &v = out[t.src];
		// both sides (seen from outside, and from inside through the gap)
		for (int s = 0; s < 2; s++) {
			VECTOR3 n = s ? -w : w;
			VECTOR3 q[4] = { a, b, b1, a1 };
			int order[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 0, 2, 1, 0, 3, 2 } };
			for (int k : order[s]) v.push_back (Vertex (q[k], n));
		}
	}
}

VkMesh::Part WallPart (bool interior)
{
	VkMesh::Part p;
	p.mat.diffuse  = interior ? COLOUR4{ 0.03f, 0.028f, 0.026f, 1.0f } : COLOUR4{ 0.055f, 0.05f, 0.045f, 1.0f };
	p.mat.ambient  = p.mat.diffuse;
	p.mat.specular = { 0.02f, 0.02f, 0.02f, 1.0f };
	p.mat.emissive = { 0.0f, 0.0f, 0.0f, 1.0f };
	p.pbr = PBRParams::FromMaterial (p.mat);
	p.pbr.roughness = 0.95f;
	p.pbr.metalness = 0.0f;
	return p;
}

void AddInside (const NTVERTEX &a, const NTVERTEX &b, const NTVERTEX &c, double ofs, std::vector<NTVERTEX> &out)
{
	for (const NTVERTEX *v : { &a, &c, &b }) {
		NTVERTEX w = *v;
		w.nx = -w.nx, w.ny = -w.ny, w.nz = -w.nz;
		float l = std::sqrt (w.nx * w.nx + w.ny * w.ny + w.nz * w.nz);
		if (l > 0.0f) { // (a little inward: not on the outside, for depth and shadow rays)
			float f = (float)ofs / l;
			w.x += w.nx * f, w.y += w.ny * f, w.z += w.nz * f;
		}
		w.tu = w.tv = 0.0f;
		out.push_back (w);
	}
}

void AddParts (const std::vector<NTVERTEX> &vtx, std::vector<VkMesh::Part> &parts, bool interior)
{
	AddParts (vtx, parts, WallPart (interior));
}

void AddParts (const std::vector<NTVERTEX> &vtx, std::vector<VkMesh::Part> &parts, const VkMesh::Part &proto)
{
	const size_t MAXV = 65535 / 3 * 3;
	for (size_t i = 0; i < vtx.size(); i += MAXV) {
		VkMesh::Part p = proto;
		size_t n = std::min (MAXV, vtx.size() - i);
		p.vtx.assign (vtx.begin() + i, vtx.begin() + i + n);
		p.idx.resize (n);
		for (size_t k = 0; k < n; k++) p.idx[k] = (uint16_t)k;
		parts.push_back (std::move (p));
	}
}

} // namespace fracture
