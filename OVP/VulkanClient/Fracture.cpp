// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Fracture.h"
#include <algorithm>
#include <cmath>
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
		size_t tri = 0; int side = 0; // (a kept triangle with it, and which of its edges)
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

	/// The triangles in cells of a grid (cell: the farthest a ray goes),
	/// for rays from points of the skin
	class Grid {
	public:
		Grid (const std::vector<Tri> &tris, double cell): tris(tris), cell(cell)
		{
			for (size_t t = 0; t < tris.size(); t++) {
				Key lo = KeyOf (tris[t].p[0]), hi = lo;
				for (int j = 1; j < 3; j++) {
					Key k = KeyOf (tris[t].p[j]);
					lo = { std::min (lo.x, k.x), std::min (lo.y, k.y), std::min (lo.z, k.z) };
					hi = { std::max (hi.x, k.x), std::max (hi.y, k.y), std::max (hi.z, k.z) };
				}
				if ((hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1) > 64) { big.push_back (t); continue; }
				for (int64_t x = lo.x; x <= hi.x; x++)
					for (int64_t y = lo.y; y <= hi.y; y++)
						for (int64_t z = lo.z; z <= hi.z; z++) cells[{ x, y, z }].push_back (t);
			}
		}
		/// Distance along a ray (|d| = 1) from o to the nearest triangle
		/// beyond eps (either side), maxDist (<= cell) if none within it
		double RayHit (const VECTOR3 &o, const VECTOR3 &d, double maxDist, double eps) const
		{
			double best = maxDist;
			Key k = KeyOf (o);
			for (int64_t x = k.x - 1; x <= k.x + 1; x++)
				for (int64_t y = k.y - 1; y <= k.y + 1; y++)
					for (int64_t z = k.z - 1; z <= k.z + 1; z++) {
						auto it = cells.find ({ x, y, z });
						if (it != cells.end())
							for (size_t t : it->second) Test (tris[t], o, d, eps, best);
					}
			for (size_t t : big) Test (tris[t], o, d, eps, best);
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
	};

	NTVERTEX Vertex (const VECTOR3 &p, const VECTOR3 &n)
	{
		NTVERTEX v{};
		v.x = (float)p.x, v.y = (float)p.y, v.z = (float)p.z;
		v.nx = (float)n.x, v.ny = (float)n.y, v.nz = (float)n.z;
		return v;
	}
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
	std::unordered_map<uint64_t, Edge> edges;
	for (size_t t = 0; t < tris.size(); t++)
		for (int j = 0; j < 3; j++) {
			uint32_t a = corner[t * 3 + j], b = corner[t * 3 + (j + 1) % 3];
			if (a == b) continue;
			Edge &e = edges[(uint64_t)std::min (a, b) << 32 | std::max (a, b)];
			if (tris[t].kept) e.kept++, e.tri = t, e.side = j;
			else e.lost++;
		}
	// the cut edges
	struct Cut { size_t tri; int side; };
	std::vector<Cut> cuts;
	for (auto &it : edges) {
		const Edge &e = it.second;
		if (e.kept == 1 && (e.lost > 0 || tris[e.tri].open)) cuts.push_back ({ e.tri, e.side });
	}
	if (cuts.empty()) return;
	std::sort (cuts.begin(), cuts.end(), [] (const Cut &a, const Cut &b) {
		return a.tri != b.tri ? a.tri < b.tri : a.side < b.side; }); // (the same walls each time)
	// inward at each corner on the cut: against the normals of the skin there
	std::unordered_map<uint32_t, VECTOR3> inward;
	for (const Cut &c : cuts)
		for (int j : { c.side, (c.side + 1) % 3 })
			inward[corner[c.tri * 3 + j]] += tris[c.tri].n;
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
	Grid grid (tris, reach);
	for (auto &it : inward) {
		uint32_t id = it.first;
		const VECTOR3 &d = it.second;
		double t = grid.RayHit (pos[id], d, reach, weld);
		deep[id] = t < reach ? std::min (depth * (0.55 + 0.9 * Hash01 (id, seed)), 0.45 * t) : 0.0;
	}
	auto bottom = [&] (uint32_t id) { return pos[id] + inward[id] * deep[id]; };
	for (const Cut &c : cuts) {
		const Tri &t = tris[c.tri];
		uint32_t ia = corner[c.tri * 3 + c.side], ib = corner[c.tri * 3 + (c.side + 1) % 3];
		VECTOR3 a = t.p[c.side], b = t.p[(c.side + 1) % 3], o = t.p[(c.side + 2) % 3];
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
	const size_t MAXV = 65535 / 3 * 3;
	for (size_t i = 0; i < vtx.size(); i += MAXV) {
		VkMesh::Part p = WallPart (interior);
		size_t n = std::min (MAXV, vtx.size() - i);
		p.vtx.assign (vtx.begin() + i, vtx.begin() + i + n);
		p.idx.resize (n);
		for (size_t k = 0; k < n; k++) p.idx[k] = (uint16_t)k;
		parts.push_back (std::move (p));
	}
}

} // namespace fracture
