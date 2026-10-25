// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ConvexDecomposition.h"
#include <algorithm>
#include <cmath>

namespace convex {

	namespace {

		// ---------------------------------------------------------------
		// distances to a set of triangles (bounding volume hierarchy)

		Vector ClosestOnTriangle (const Vector &p, const Triangle &t)
		{
			// (Ericson, Real-Time Collision Detection 5.1.5)
			const Vector &a = t.a, &b = t.b, &c = t.c;
			Vector ab (b - a), ac (c - a), ap (p - a);
			double d1 = dotp (ab, ap), d2 = dotp (ac, ap);
			if (d1 <= 0 && d2 <= 0) return a;
			Vector bp (p - b);
			double d3 = dotp (ab, bp), d4 = dotp (ac, bp);
			if (d3 >= 0 && d4 <= d3) return b;
			double vc = d1*d4 - d3*d2;
			if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
			Vector cp (p - c);
			double d5 = dotp (ab, cp), d6 = dotp (ac, cp);
			if (d6 >= 0 && d5 <= d6) return c;
			double vb = d5*d2 - d1*d6;
			if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
			double va = d3*d6 - d5*d4;
			if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
			double den = 1.0 / (va + vb + vc);
			return a + ab * (vb * den) + ac * (vc * den);
		}

		struct BVH {
			struct Node { Vector lo, hi; int left = -1, right = -1, first = 0, count = 0; };
			std::vector<Node> nodes;
			std::vector<Triangle> tris;

			explicit BVH (const std::vector<Triangle> &t): tris (t)
			{
				if (!tris.empty()) Build (0, (int)tris.size());
			}

			int Build (int first, int count)
			{
				Node n;
				n.lo.Set (1e300, 1e300, 1e300), n.hi.Set (-1e300, -1e300, -1e300);
				for (int i = first; i < first + count; i++)
					for (const Vector *v : {&tris[i].a, &tris[i].b, &tris[i].c})
						for (int k = 0; k < 3; k++)
							n.lo.data[k] = std::min (n.lo.data[k], v->data[k]), n.hi.data[k] = std::max (n.hi.data[k], v->data[k]);
				int idx = (int)nodes.size();
				nodes.push_back (n);
				if (count <= 4) {
					nodes[idx].first = first, nodes[idx].count = count;
					return idx;
				}
				// (split at the median of the centroids along the longest axis)
				Vector ext (n.hi - n.lo);
				int ax = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
				int mid = first + count / 2;
				std::nth_element (tris.begin() + first, tris.begin() + mid, tris.begin() + first + count,
					[ax](const Triangle &p, const Triangle &q) {
						return p.a.data[ax] + p.b.data[ax] + p.c.data[ax] < q.a.data[ax] + q.b.data[ax] + q.c.data[ax]; });
				int l = Build (first, mid - first);
				int r = Build (mid, first + count - mid);
				nodes[idx].left = l, nodes[idx].right = r;
				return idx;
			}

			static double BoxDist2 (const Node &n, const Vector &p)
			{
				double d = 0.0;
				for (int k = 0; k < 3; k++) {
					double e = std::max (std::max (n.lo.data[k] - p.data[k], 0.0), p.data[k] - n.hi.data[k]);
					d += e*e;
				}
				return d;
			}

			// squared distance of p from the nearest triangle
			double Dist2 (const Vector &p) const
			{
				double best = 1e300;
				if (nodes.empty()) return best;
				int stack[128], sp = 0;
				stack[sp++] = 0;
				while (sp) {
					const Node &n = nodes[stack[--sp]];
					if (BoxDist2 (n, p) >= best) continue;
					if (n.left < 0) {
						for (int i = n.first; i < n.first + n.count; i++)
							best = std::min (best, ClosestOnTriangle (p, tris[i]).dist2 (p));
					} else if (sp < 126) {
						// (nearer child last: visited first)
						double dl = BoxDist2 (nodes[n.left], p), dr = BoxDist2 (nodes[n.right], p);
						if (dl < dr) stack[sp++] = n.right, stack[sp++] = n.left;
						else         stack[sp++] = n.left, stack[sp++] = n.right;
					}
				}
				return best;
			}
		};

		// ---------------------------------------------------------------

		std::vector<Vector> Vertices (const std::vector<Triangle> &tris)
		{
			std::vector<Vector> v;
			v.reserve (tris.size() * 3);
			for (const Triangle &t : tris) v.push_back (t.a), v.push_back (t.b), v.push_back (t.c);
			return v;
		}

		// a plane a part was cut along (normal along axis ax), and the part's
		// cross-section there (segments where its surface meets the plane)
		struct Plane {
			Vector n;
			double d;
			int ax;
			std::vector<std::pair<Vector,Vector>> seg;

			// a point on the plane: its distance from the cross-section (0 inside)
			double Outside (const Vector &p, double tol) const
			{
				int u = (ax + 1) % 3, v = (ax + 2) % 3;
				bool inside = false;
				double dmin = 1e300;
				for (const auto &s : seg) {
					const Vector &a = s.first, &b = s.second;
					// (even-odd test: a ray from p along +u)
					if ((a.data[v] > p.data[v]) != (b.data[v] > p.data[v])) {
						double x = a.data[u] + (p.data[v] - a.data[v]) / (b.data[v] - a.data[v]) * (b.data[u] - a.data[u]);
						if (x > p.data[u]) inside = !inside;
					}
					Vector ab (b - a);
					double t = std::clamp (dotp (p - a, ab) / std::max (dotp (ab, ab), 1e-300), 0.0, 1.0);
					dmin = std::min (dmin, (a + ab * t).dist (p));
				}
				return (inside || dmin < tol) ? 0.0 : dmin;
			}
		};

		struct Part {
			std::vector<Triangle> tris;
			std::vector<Plane> cuts; // planes the part was cut off along (its open sides)
			Polytope hull;
			double conc = 0.0;
			Vector deep;
			bool final = false;
		};

		double ConcavityCut (const std::vector<Triangle> &tris, const std::vector<Plane> &cuts, const Polytope &hull, Vector *deepest);

		Part MakePart (std::vector<Triangle> &&tris, const std::vector<Plane> &cuts)
		{
			Part p;
			p.tris = std::move (tris);
			p.cuts = cuts;
			Build (Vertices (p.tris), p.hull);
			p.conc = ConcavityCut (p.tris, p.cuts, p.hull, &p.deep);
			return p;
		}

		// a triangle clipped by a plane into the parts below (dotp (n, x) < d)
		// and above it
		void SplitTriangle (const Triangle &t, const Vector &n, double d, std::vector<Triangle> &below, std::vector<Triangle> &above,
			std::vector<std::pair<Vector,Vector>> &section)
		{
			const Vector *v[3] = { &t.a, &t.b, &t.c };
			double s[3];
			for (int k = 0; k < 3; k++) s[k] = dotp (n, *v[k]) - d;
			if (s[0] <= 0 && s[1] <= 0 && s[2] <= 0) { below.push_back (t); return; }
			if (s[0] >= 0 && s[1] >= 0 && s[2] >= 0) { above.push_back (t); return; }
			std::vector<Vector> lo, hi;
			for (int k = 0; k < 3; k++) {
				const Vector &a = *v[k], &b = *v[(k+1)%3];
				double sa = s[k], sb = s[(k+1)%3];
				if (sa <= 0) lo.push_back (a);
				if (sa >= 0) hi.push_back (a);
				if ((sa < 0 && sb > 0) || (sa > 0 && sb < 0)) {
					Vector x (a + (b - a) * (sa / (sa - sb)));
					lo.push_back (x), hi.push_back (x);
				}
			}
			// (the points on the plane: the triangle's part of the cross-section)
			std::vector<Vector> on;
			for (const Vector &x : lo) if (fabs (dotp (n, x) - d) < 1e-12 * (1.0 + fabs (d))) on.push_back (x);
			if (on.size() == 2) section.push_back ({on[0], on[1]});
			for (size_t k = 1; k + 1 < lo.size(); k++) below.push_back ({lo[0], lo[k], lo[k+1]});
			for (size_t k = 1; k + 1 < hi.size(); k++) above.push_back ({hi[0], hi[k], hi[k+1]});
		}

		const int MAX_SAMPLES = 400;
	}

	double Concavity (const std::vector<Triangle> &tris, const Polytope &hull, Vector *deepest)
	{
		return ConcavityCut (tris, {}, hull, deepest);
	}

	namespace {
	double ConcavityCut (const std::vector<Triangle> &tris, const std::vector<Plane> &cuts, const Polytope &hull, Vector *deepest)
	{
		if (tris.empty() || hull.f.empty()) return 0.0;
		BVH bvh (tris);
		// points on the hull's faces: a grid over each (fan) triangle, finer on
		// large faces
		double spacing = std::max (hull.radius / 12.0, 1e-3);
		std::vector<Vector> s;
		for (const Face &f : hull.f) {
			const Vector &v0 = hull.v[f.idx[0]];
			for (size_t i = 1; i + 1 < f.idx.size(); i++) {
				const Vector &v1 = hull.v[f.idx[i]], &v2 = hull.v[f.idx[i+1]];
				double l = std::max (std::max (v0.dist (v1), v1.dist (v2)), v2.dist (v0));
				int n = std::clamp ((int)std::ceil (l / spacing), 1, 8);
				for (int a = 0; a <= n; a++)
					for (int b = 0; a + b <= n; b++) {
						int c = n - a - b;
						if (a == n || b == n || c == n) continue; // (the vertices are on the surface)
						s.push_back ((v0 * a + v1 * b + v2 * c) / (double)n);
					}
			}
		}
		size_t stride = std::max<size_t> (1, s.size() / MAX_SAMPLES);
		double dmax = 0.0;
		for (size_t i = 0; i < s.size(); i += stride) {
			double d = bvh.Dist2 (s[i]);
			for (const Plane &c : cuts) { // (on an open side, where the part was cut off: within its cross-section)
				double e = dotp (c.n, s[i]) - c.d;
				if (fabs (e) < 1e-6 * (1.0 + hull.radius)) {
					double o = c.Outside (s[i] - c.n * e, spacing);
					d = std::min (d, o*o);
				}
			}
			if (d > dmax) {
				dmax = d;
				if (deepest) *deepest = s[i];
			}
		}
		return std::sqrt (dmax);
	}
	}

	std::vector<Polytope> Decompose (const std::vector<Triangle> &input, const DecomposeParams &prm)
	{
		std::vector<Polytope> out;
		if (input.empty()) return out;
		std::vector<Part> parts;
		parts.push_back (MakePart (std::vector<Triangle> (input), {}));
		double tol = std::max (prm.tolerance, prm.relTolerance * 2.0 * parts[0].hull.radius);

		while ((int)parts.size() < prm.maxPieces) {
			// the part that sticks out furthest
			int w = -1;
			for (size_t i = 0; i < parts.size(); i++)
				if (!parts[i].final && parts[i].conc > tol && (w < 0 || parts[i].conc > parts[w].conc)) w = (int)i;
			if (w < 0) break;
			Part &P = parts[w];

			// candidate planes: across each axis at a quarter, half and three
			// quarters of the part, and through its deepest point
			Vector lo (1e300, 1e300, 1e300), hi (-1e300, -1e300, -1e300);
			for (const Vector &v : P.hull.v)
				for (int k = 0; k < 3; k++) lo.data[k] = std::min (lo.data[k], v.data[k]), hi.data[k] = std::max (hi.data[k], v.data[k]);
			double bestScore = 1e300, bestVol = 1e300;
			Part bestL, bestR;
			bool found = false;
			for (int ax = 0; ax < 3; ax++) {
				double ext = hi.data[ax] - lo.data[ax];
				if (ext < prm.minExtent) continue;
				double cut[2] = { lo.data[ax] + 0.5*ext, P.deep.data[ax] };
				for (double c : cut) {
					if (c <= lo.data[ax] + 0.02*ext || c >= hi.data[ax] - 0.02*ext) continue;
					Vector n (0,0,0);
					n.data[ax] = 1.0;
					std::vector<Triangle> L, R;
					Plane cp{n, c, ax, {}};
					for (const Triangle &t : P.tris) SplitTriangle (t, n, c, L, R, cp.seg);
					if (L.empty() || R.empty()) continue;
					// (the earlier cross-sections, each side's part of them)
					std::vector<Plane> cl, cr;
					for (const Plane &q : P.cuts) {
						Plane ql{q.n, q.d, q.ax, {}}, qr{q.n, q.d, q.ax, {}};
						for (const auto &sg : q.seg) {
							double sa = sg.first.data[ax] - c, sb = sg.second.data[ax] - c;
							if (sa <= 0 && sb <= 0) ql.seg.push_back (sg);
							else if (sa >= 0 && sb >= 0) qr.seg.push_back (sg);
							else {
								Vector x (sg.first + (sg.second - sg.first) * (sa / (sa - sb)));
								if (sa < 0) ql.seg.push_back ({sg.first, x}), qr.seg.push_back ({x, sg.second});
								else        qr.seg.push_back ({sg.first, x}), ql.seg.push_back ({x, sg.second});
							}
						}
						cl.push_back (ql), cr.push_back (qr);
					}
					cl.push_back (cp), cr.push_back (cp);
					Part pl = MakePart (std::move (L), cl), pr = MakePart (std::move (R), cr);
					double score = std::max (pl.conc, pr.conc), vol = pl.hull.volume + pr.hull.volume;
					if (score < bestScore - 1e-9 || (score < bestScore + 1e-9 && vol < bestVol)) {
						bestScore = score, bestVol = vol;
						bestL = std::move (pl), bestR = std::move (pr);
						found = true;
					}
				}
			}
			if (!found) { P.final = true; continue; } // (cannot be split further)
			parts[w] = std::move (bestL);
			parts.push_back (std::move (bestR));
		}

		// merge neighbouring parts whose joint hull still fits (splitting at
		// the middle or the deepest point is not always where the shape
		// changes)
		auto box = [](const Part &p, Vector &lo, Vector &hi) {
			lo.Set (1e300, 1e300, 1e300), hi.Set (-1e300, -1e300, -1e300);
			for (const Vector &v : p.hull.v)
				for (int k = 0; k < 3; k++) lo.data[k] = std::min (lo.data[k], v.data[k]), hi.data[k] = std::max (hi.data[k], v.data[k]);
		};
		auto touch = [&](const Part &a, const Part &b) {
			Vector la, ha, lb, hb;
			box (a, la, ha), box (b, lb, hb);
			for (int k = 0; k < 3; k++)
				if (la.data[k] > hb.data[k] + tol || lb.data[k] > ha.data[k] + tol) return false;
			return true;
		};
		auto join = [](const Part &a, const Part &b) {
			std::vector<Triangle> t (a.tris);
			t.insert (t.end(), b.tris.begin(), b.tris.end());
			// (the cuts of either, but for those between them: inside the joint part)
			std::vector<Plane> cuts;
			for (const Plane &p : a.cuts) {
				bool shared = false;
				for (const Plane &q : b.cuts) if (q.ax == p.ax && q.d == p.d) shared = true;
				if (!shared) cuts.push_back (p);
			}
			for (const Plane &q : b.cuts) {
				bool shared = false;
				for (const Plane &p : a.cuts) if (q.ax == p.ax && q.d == p.d) shared = true;
				if (!shared) cuts.push_back (q);
			}
			return MakePart (std::move (t), cuts);
		};
		for (bool merged = true; merged && parts.size() > 1; ) {
			merged = false;
			double best = tol;
			int bi = -1, bj = -1;
			Part bp;
			for (size_t i = 0; i < parts.size(); i++)
				for (size_t j = i+1; j < parts.size(); j++) {
					if (!touch (parts[i], parts[j])) continue;
					Part m = join (parts[i], parts[j]);
					if (m.conc <= best) best = m.conc, bi = (int)i, bj = (int)j, bp = std::move (m);
				}
			if (bi >= 0) {
				parts[bi] = std::move (bp);
				parts.erase (parts.begin() + bj);
				merged = true;
			}
		}

		for (Part &p : parts) {
			if (p.hull.v.empty()) continue;
			if (2.0 * p.hull.radius < prm.minExtent) continue;
			out.push_back (std::move (p.hull));
		}
		return out;
	}

} // namespace convex
