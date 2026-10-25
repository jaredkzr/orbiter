// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ConvexGeometry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace convex {

	// ===================================================================
	// Polytope

	int Polytope::Support (const Vector &d) const
	{
		int best = 0;
		double bmax = -1e300;
		for (size_t i = 0; i < v.size(); i++) {
			double e = dotp (v[i], d);
			if (e > bmax) bmax = e, best = (int)i;
		}
		return best;
	}

	void Polytope::Translate (const Vector &ofs)
	{
		for (Vector &p : v) p += ofs;
		for (Face &fc : f) fc.d += dotp (fc.n, ofs);
		centre += ofs;
	}

	Vector Placed::Support (const Vector &d) const
	{
		return pos + mul (R, p->v[p->Support (tmul (R, d))]);
	}

	// ===================================================================
	// Convex hull (incremental)

	namespace {
		struct Tri {
			int a, b, c;
			Vector n;
			double d;
			bool alive;
		};

		Tri MakeTri (const std::vector<Vector> &p, int a, int b, int c)
		{
			Tri t{a, b, c, crossp (p[b] - p[a], p[c] - p[a]), 0.0, true};
			double l = t.n.length();
			if (l > 0.0) t.n /= l;
			t.d = dotp (t.n, p[a]);
			return t;
		}

		// points without (near) duplicates
		std::vector<Vector> Unique (const std::vector<Vector> &pts, double q)
		{
			std::set<std::tuple<long long, long long, long long>> seen;
			std::vector<Vector> out;
			for (const Vector &p : pts) {
				auto key = std::make_tuple ((long long)std::floor (p.x / q), (long long)std::floor (p.y / q), (long long)std::floor (p.z / q));
				if (seen.insert (key).second) out.push_back (p);
			}
			return out;
		}

		// a unit vector perpendicular to d
		Vector Perp (const Vector &d)
		{
			Vector a (fabs (d.x) < 0.6 ? Vector(1,0,0) : fabs (d.y) < 0.6 ? Vector(0,1,0) : Vector(0,0,1));
			return crossp (d, a).unit();
		}

		const double THIN = 1e-3; // [m] thickness given to flat point sets

		// directions over the sphere: an icosahedron subdivided n times
		std::vector<Vector> Sphere (int n)
		{
			const double t = (1.0 + std::sqrt (5.0)) / 2.0;
			std::vector<Vector> v = {
				{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
				{0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1} };
			std::vector<std::array<int,3>> f = {
				{0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11}, {1,5,9}, {5,11,4}, {11,10,2}, {10,7,6}, {7,1,8},
				{3,9,4}, {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9}, {4,9,5}, {2,4,11}, {6,2,10}, {8,6,7}, {9,8,1} };
			for (auto &p : v) p.unify();
			for (int k = 0; k < n; k++) {
				std::map<std::pair<int,int>, int> mid;
				auto midpoint = [&](int a, int b) {
					auto key = std::make_pair (std::min (a, b), std::max (a, b));
					auto it = mid.find (key);
					if (it != mid.end()) return it->second;
					v.push_back ((v[a] + v[b]).unit());
					return mid[key] = (int)v.size() - 1;
				};
				std::vector<std::array<int,3>> f2;
				for (auto &tri : f) {
					int a = midpoint (tri[0], tri[1]), b = midpoint (tri[1], tri[2]), c = midpoint (tri[2], tri[0]);
					f2.push_back ({tri[0], a, c}); f2.push_back ({tri[1], b, a});
					f2.push_back ({tri[2], c, b}); f2.push_back ({a, b, c});
				}
				f = f2;
			}
			return v;
		}

		// large point sets: their extreme points along many directions (the
		// hull loses little: a few thousandths of its size)
		const size_t MAX_POINTS = 1000;
		std::vector<Vector> Extremes (const std::vector<Vector> &p)
		{
			static const std::vector<Vector> dirs = Sphere (3); // (642)
			std::vector<int> idx;
			for (const Vector &d : dirs) {
				int best = 0;
				double bmax = -1e300;
				for (size_t i = 0; i < p.size(); i++) {
					double e = dotp (d, p[i]);
					if (e > bmax) bmax = e, best = (int)i;
				}
				idx.push_back (best);
			}
			std::sort (idx.begin(), idx.end());
			idx.erase (std::unique (idx.begin(), idx.end()), idx.end());
			std::vector<Vector> out;
			for (int i : idx) out.push_back (p[i]);
			return out;
		}
	}

	bool Build (const std::vector<Vector> &input, Polytope &P)
	{
		P = Polytope();
		if (input.empty()) return false;

		Vector lo (1e300, 1e300, 1e300), hi (-1e300, -1e300, -1e300);
		for (const Vector &p : input)
			for (int k = 0; k < 3; k++) lo.data[k] = std::min (lo.data[k], p.data[k]), hi.data[k] = std::max (hi.data[k], p.data[k]);
		double ext = std::max ((hi - lo).length(), THIN);
		double eps = 1e-7 * ext;
		std::vector<Vector> p (Unique (input, 1e-6 * ext));
		if (p.size() > MAX_POINTS) p = Extremes (p);

		// initial tetrahedron (degenerate sets get a little extent)
		int i0 = 0;
		for (size_t i = 1; i < p.size(); i++) if (p[i].x < p[i0].x) i0 = (int)i;
		int i1 = i0;
		double dm = 0.0;
		for (size_t i = 0; i < p.size(); i++) { double d = p[i].dist (p[i0]); if (d > dm) dm = d, i1 = (int)i; }
		if (dm < 0.5*THIN) { // (a point: a small box)
			Vector c (p[i0]);
			p.clear();
			for (int k = 0; k < 8; k++)
				p.push_back (c + Vector ((k & 1) ? THIN : -THIN, (k & 2) ? THIN : -THIN, (k & 4) ? THIN : -THIN) * 0.5);
			return Build (p, P);
		}
		Vector axis ((p[i1] - p[i0]).unit());
		int i2 = i0;
		dm = 0.0;
		for (size_t i = 0; i < p.size(); i++) {
			Vector r (p[i] - p[i0]);
			double d = (r - axis * dotp (r, axis)).length();
			if (d > dm) dm = d, i2 = (int)i;
		}
		if (dm < 0.5*THIN) { // (on a line: widened)
			Vector u (Perp (axis)), w (crossp (axis, u));
			std::vector<Vector> q;
			for (const Vector &x : p)
				for (int k = 0; k < 4; k++) q.push_back (x + (k & 1 ? u : w) * ((k & 2) ? 0.5*THIN : -0.5*THIN));
			return Build (q, P);
		}
		Vector nrm (crossp (p[i1] - p[i0], p[i2] - p[i0]).unit());
		int i3 = i0;
		dm = 0.0;
		for (size_t i = 0; i < p.size(); i++) {
			double d = fabs (dotp (p[i] - p[i0], nrm));
			if (d > dm) dm = d, i3 = (int)i;
		}
		if (dm < 0.5*THIN) { // (flat: a thin slab)
			std::vector<Vector> q;
			for (const Vector &x : p) q.push_back (x + nrm * (0.5*THIN)), q.push_back (x - nrm * (0.5*THIN));
			return Build (q, P);
		}

		std::vector<Tri> tri;
		Vector inner ((p[i0] + p[i1] + p[i2] + p[i3]) * 0.25);
		auto add = [&](int a, int b, int c) {
			Tri t = MakeTri (p, a, b, c);
			if (dotp (t.n, inner) > t.d) t = MakeTri (p, a, c, b); // (outward)
			tri.push_back (t);
		};
		add (i0, i1, i2); add (i0, i1, i3); add (i0, i2, i3); add (i1, i2, i3);

		// each directed edge belongs to one triangle; the triangle across an
		// edge (a,b) is the one with (b,a)
		std::map<std::pair<int,int>, int> owner;
		auto link = [&](int t) {
			owner[{tri[t].a, tri[t].b}] = t; owner[{tri[t].b, tri[t].c}] = t; owner[{tri[t].c, tri[t].a}] = t;
		};
		auto unlink = [&](int t) {
			owner.erase ({tri[t].a, tri[t].b}); owner.erase ({tri[t].b, tri[t].c}); owner.erase ({tri[t].c, tri[t].a});
		};
		for (int t = 0; t < 4; t++) link (t);

		std::vector<int> stack, visible;
		std::vector<std::pair<int,int>> horizon;
		std::vector<char> mark;
		for (size_t i = 0; i < p.size(); i++) {
			int ip = (int)i;
			if (ip == i0 || ip == i1 || ip == i2 || ip == i3) continue;
			// the triangle the point is furthest above
			int seed = -1;
			double dmax = eps;
			for (size_t t = 0; t < tri.size(); t++) {
				if (!tri[t].alive) continue;
				double dd = dotp (tri[t].n, p[i]) - tri[t].d;
				if (dd > dmax) dmax = dd, seed = (int)t;
			}
			if (seed < 0) continue; // (inside)
			// the visible region: connected triangles the point is above
			mark.assign (tri.size(), 0);
			visible.clear(); horizon.clear(); stack.clear();
			stack.push_back (seed);
			mark[seed] = 1;
			while (!stack.empty()) {
				int t = stack.back(); stack.pop_back();
				visible.push_back (t);
				int e[3][2] = { {tri[t].a, tri[t].b}, {tri[t].b, tri[t].c}, {tri[t].c, tri[t].a} };
				for (auto &ed : e) {
					auto it = owner.find ({ed[1], ed[0]});
					int nb = (it == owner.end() ? -1 : it->second);
					if (nb >= 0 && mark[nb] == 1) continue;
					if (nb >= 0 && !mark[nb] && dotp (tri[nb].n, p[i]) - tri[nb].d > eps) {
						mark[nb] = 1;
						stack.push_back (nb);
					} else {
						horizon.push_back ({ed[0], ed[1]});
					}
				}
			}
			// (a horizon edge seen from a triangle that became visible later is
			// not on the horizon)
			std::vector<std::pair<int,int>> h;
			for (const auto &ed : horizon) {
				auto it = owner.find ({ed.second, ed.first});
				if (it != owner.end() && mark[it->second] == 1) continue;
				h.push_back (ed);
			}
			for (int t : visible) { unlink (t); tri[t].alive = false; }
			for (const auto &ed : h) {
				tri.push_back (MakeTri (p, ed.first, ed.second, ip));
				link ((int)tri.size() - 1);
			}
			if (tri.size() > 8 * p.size() + 64) { // (drop removed triangles now and then)
				std::vector<Tri> alive;
				for (const Tri &t : tri) if (t.alive) alive.push_back (t);
				tri = alive;
				owner.clear();
				for (size_t t = 0; t < tri.size(); t++) link ((int)t);
			}
		}
		tri.erase (std::remove_if (tri.begin(), tri.end(), [](const Tri &t) { return !t.alive; }), tri.end());

		// faces: coplanar triangles merged (on a convex hull, all triangles in
		// one plane form one face), their boundary as a loop
		std::vector<bool> used (tri.size(), false);
		std::map<int,int> remap;
		auto vid = [&](int i) {
			auto it = remap.find (i);
			if (it != remap.end()) return it->second;
			int k = (int)P.v.size();
			P.v.push_back (p[i]);
			return remap[i] = k;
		};
		for (size_t i = 0; i < tri.size(); i++) {
			if (used[i]) continue;
			std::map<int,int> next; // boundary edges a -> b
			std::set<std::pair<int,int>> group;
			for (size_t j = i; j < tri.size(); j++) {
				if (used[j]) continue;
				if (dotp (tri[i].n, tri[j].n) < 1.0 - 1e-8 || fabs (tri[i].d - tri[j].d) > 1e4 * eps) continue;
				used[j] = true;
				group.insert ({tri[j].a, tri[j].b}); group.insert ({tri[j].b, tri[j].c}); group.insert ({tri[j].c, tri[j].a});
			}
			for (const auto &e : group)
				if (!group.count ({e.second, e.first})) next[e.first] = e.second;
			if (next.empty()) continue;
			Face fc;
			fc.n = tri[i].n;
			int start = next.begin()->first, k = start;
			for (size_t guard = 0; guard <= next.size(); guard++) {
				fc.idx.push_back (vid (k));
				auto it = next.find (k);
				if (it == next.end()) break;
				k = it->second;
				if (k == start) break;
			}
			fc.d = dotp (fc.n, P.v[fc.idx[0]]);
			P.f.push_back (fc);
		}

		// bounding sphere, volume
		lo.Set (1e300, 1e300, 1e300), hi.Set (-1e300, -1e300, -1e300);
		for (const Vector &x : P.v)
			for (int k = 0; k < 3; k++) lo.data[k] = std::min (lo.data[k], x.data[k]), hi.data[k] = std::max (hi.data[k], x.data[k]);
		P.centre = (lo + hi) * 0.5;
		P.radius = 0.0;
		for (const Vector &x : P.v) P.radius = std::max (P.radius, x.dist (P.centre));
		P.volume = 0.0;
		for (const Tri &t : tri)
			P.volume += dotp (p[t.a] - P.centre, crossp (p[t.b] - P.centre, p[t.c] - P.centre)) / 6.0;
		return true;
	}

	// ===================================================================
	// GJK: distance of the polytopes' cores

	namespace {
		struct SV { Vector w, a, b; }; // support point of A - B, and of A and B

		SV SupportAB (const Placed &A, const Placed &B, const Vector &d)
		{
			SV s;
			s.a = A.Support (d);
			s.b = B.Support (-d);
			s.w = s.a - s.b;
			return s;
		}

		// closest point of a simplex to the origin: reduces the simplex to
		// the points that span it, with their weights
		Vector Closest (std::vector<SV> &S, std::vector<double> &lam)
		{
			if (S.size() == 1) { lam = {1.0}; return S[0].w; }
			if (S.size() == 2) {
				Vector a (S[0].w), b (S[1].w), ab (b - a);
				double t = -dotp (a, ab) / std::max (dotp (ab, ab), 1e-300);
				if (t <= 0.0) { S = {S[0]}; lam = {1.0}; return a; }
				if (t >= 1.0) { S = {S[1]}; lam = {1.0}; return b; }
				lam = {1.0 - t, t};
				return a + ab * t;
			}
			if (S.size() == 3) {
				// (Ericson, closest point on a triangle, for the origin)
				Vector a (S[0].w), b (S[1].w), c (S[2].w);
				Vector ab (b - a), ac (c - a), ap (-a);
				double d1 = dotp (ab, ap), d2 = dotp (ac, ap);
				if (d1 <= 0 && d2 <= 0) { S = {S[0]}; lam = {1.0}; return a; }
				Vector bp (-b);
				double d3 = dotp (ab, bp), d4 = dotp (ac, bp);
				if (d3 >= 0 && d4 <= d3) { S = {S[1]}; lam = {1.0}; return b; }
				double vc = d1*d4 - d3*d2;
				if (vc <= 0 && d1 >= 0 && d3 <= 0) {
					double v = d1 / (d1 - d3);
					S = {S[0], S[1]}; lam = {1.0 - v, v};
					return a + ab * v;
				}
				Vector cp (-c);
				double d5 = dotp (ab, cp), d6 = dotp (ac, cp);
				if (d6 >= 0 && d5 <= d6) { S = {S[2]}; lam = {1.0}; return c; }
				double vb = d5*d2 - d1*d6;
				if (vb <= 0 && d2 >= 0 && d6 <= 0) {
					double w = d2 / (d2 - d6);
					S = {S[0], S[2]}; lam = {1.0 - w, w};
					return a + ac * w;
				}
				double va = d3*d6 - d5*d4;
				if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
					double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
					S = {S[1], S[2]}; lam = {1.0 - w, w};
					return b + (c - b) * w;
				}
				double den = 1.0 / (va + vb + vc);
				double v = vb * den, w = vc * den;
				lam = {1.0 - v - w, v, w};
				return a + ab * v + ac * w;
			}
			// tetrahedron: the origin inside, or the closest of its faces
			static const int F[4][4] = { {0,1,2,3}, {0,1,3,2}, {0,2,3,1}, {1,2,3,0} };
			bool inside = true;
			for (const auto &fc : F) {
				Vector n (crossp (S[fc[1]].w - S[fc[0]].w, S[fc[2]].w - S[fc[0]].w));
				double so = dotp (n, -S[fc[0]].w), sd = dotp (n, S[fc[3]].w - S[fc[0]].w);
				if (so * sd < 0.0) { inside = false; break; }
			}
			if (inside) { lam.assign (4, 0.25); return Vector (0,0,0); }
			double best = 1e300;
			std::vector<SV> bestS;
			std::vector<double> bestL;
			Vector bestV;
			for (const auto &fc : F) {
				std::vector<SV> T = {S[fc[0]], S[fc[1]], S[fc[2]]};
				std::vector<double> l;
				Vector v (Closest (T, l));
				double d = dotp (v, v);
				if (d < best) best = d, bestS = T, bestL = l, bestV = v;
			}
			S = bestS; lam = bestL;
			return bestV;
		}

		// returns false if the cores overlap (S is then the final simplex)
		bool GJK (const Placed &A, const Placed &B, Vector &pa, Vector &pb, std::vector<SV> &S)
		{
			Vector v (A.pos + mul (A.R, A.p->centre) - B.pos - mul (B.R, B.p->centre));
			if (v.length() < 1e-12) v.Set (1, 0, 0);
			S.clear();
			std::vector<double> lam;
			double scale = A.p->radius + B.p->radius + 1.0;
			for (int it = 0; it < 64; it++) {
				SV s = SupportAB (A, B, -v);
				double vv = dotp (v, v);
				if (!S.empty() && vv - dotp (v, s.w) <= 1e-10 * vv + 1e-14 * scale * scale) break; // (converged)
				bool dup = false;
				for (const SV &q : S) if (q.w.dist2 (s.w) < 1e-20 * scale * scale) dup = true;
				if (dup) break;
				S.push_back (s);
				v = Closest (S, lam);
				if (S.size() == 4 || dotp (v, v) < 1e-18 * scale * scale) return false; // (origin inside)
			}
			if (S.empty()) return false;
			Vector qa, qb;
			for (size_t i = 0; i < S.size(); i++) qa += S[i].a * lam[i], qb += S[i].b * lam[i];
			pa = qa, pb = qb;
			return true;
		}

		// EPA: depth and direction of the cores' penetration (n: of A - B's
		// boundary closest to the origin, i.e. A moves by -n*depth to separate)
		double EPA (const Placed &A, const Placed &B, std::vector<SV> S, Vector &n, Vector &pa, Vector &pb)
		{
			double scale = A.p->radius + B.p->radius + 1.0;
			// a tetrahedron around the origin
			static const Vector dirs[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
			auto independent = [&](const SV &s) {
				if (S.empty()) return true;
				if (S.size() == 1) return s.w.dist (S[0].w) > 1e-9 * scale;
				if (S.size() == 2) return crossp (S[1].w - S[0].w, s.w - S[0].w).length() > 1e-9 * scale * scale;
				return fabs (dotp (crossp (S[1].w - S[0].w, S[2].w - S[0].w), s.w - S[0].w)) > 1e-9 * scale * scale * scale;
			};
			if (S.size() > 4) S.resize (4);
			for (int k = 0; S.size() < 4 && k < 6; k++) {
				Vector d (dirs[k]);
				if (S.size() == 3) {
					Vector nn (crossp (S[1].w - S[0].w, S[2].w - S[0].w));
					d = (k & 1) ? -nn : nn;
				} else if (S.size() == 2) {
					Vector e (S[1].w - S[0].w), u (Perp (e.unit()));
					d = (k & 1) ? -u : u;
					if (k >= 2) d = (k & 1) ? -crossp (e.unit(), u) : crossp (e.unit(), u);
				}
				SV s = SupportAB (A, B, d);
				if (independent (s)) S.push_back (s);
			}
			if (S.size() < 4) { // (degenerate: touching)
				n = Vector (1,0,0);
				pa = S.empty() ? A.pos : S[0].a, pb = S.empty() ? B.pos : S[0].b;
				return 0.0;
			}

			struct EF { int a, b, c; Vector n; double d; };
			std::vector<SV> V (S);
			std::vector<EF> F;
			Vector inner ((V[0].w + V[1].w + V[2].w + V[3].w) * 0.25);
			auto face = [&](int a, int b, int c) {
				EF f{a, b, c, crossp (V[b].w - V[a].w, V[c].w - V[a].w), 0.0};
				double l = f.n.length();
				if (l > 0.0) f.n /= l;
				f.d = dotp (f.n, V[a].w);
				if (dotp (f.n, inner) > f.d) { std::swap (f.b, f.c); f.n = -f.n; f.d = -f.d; }
				return f;
			};
			F = { face (0,1,2), face (0,1,3), face (0,2,3), face (1,2,3) };

			EF best = F[0];
			for (int it = 0; it < 64; it++) {
				int bi = 0;
				for (size_t i = 1; i < F.size(); i++) if (F[i].d < F[bi].d) bi = (int)i;
				best = F[bi];
				SV s = SupportAB (A, B, best.n);
				if (dotp (s.w, best.n) - best.d < 1e-7 * scale) break; // (converged)
				int iv = (int)V.size();
				V.push_back (s);
				std::map<std::pair<int,int>, int> edges;
				std::vector<EF> keep;
				for (const EF &f : F) {
					if (dotp (f.n, s.w) - f.d > 0.0) { edges[{f.a, f.b}]++; edges[{f.b, f.c}]++; edges[{f.c, f.a}]++; }
					else keep.push_back (f);
				}
				for (const auto &e : edges)
					if (!edges.count ({e.first.second, e.first.first})) {
						EF f{e.first.first, e.first.second, iv, crossp (V[e.first.second].w - V[e.first.first].w, V[iv].w - V[e.first.first].w), 0.0};
						double l = f.n.length();
						if (l > 0.0) f.n /= l;
						f.d = dotp (f.n, V[f.a].w);
						keep.push_back (f);
					}
				if (keep.empty()) break;
				F = keep;
			}
			// the origin's projection on the face, in barycentric coordinates
			Vector q (best.n * best.d), a (V[best.a].w), b (V[best.b].w), c (V[best.c].w);
			Vector v0 (b - a), v1 (c - a), v2 (q - a);
			double d00 = dotp (v0,v0), d01 = dotp (v0,v1), d11 = dotp (v1,v1), d20 = dotp (v2,v0), d21 = dotp (v2,v1);
			double den = d00*d11 - d01*d01;
			double l1 = den ? (d11*d20 - d01*d21) / den : 0.0, l2 = den ? (d00*d21 - d01*d20) / den : 0.0, l0 = 1.0 - l1 - l2;
			pa = V[best.a].a * l0 + V[best.b].a * l1 + V[best.c].a * l2;
			pb = V[best.a].b * l0 + V[best.b].b * l1 + V[best.c].b * l2;
			n = best.n;
			return best.d;
		}
	}

	double Distance (const Placed &A, const Placed &B, Vector &pa, Vector &pb, Vector &n)
	{
		std::vector<SV> S;
		Vector qa, qb;
		if (GJK (A, B, qa, qb, S)) {
			Vector v (qa - qb);
			double d = v.length();
			if (d > 1e-12) {
				n = v / d;
				pa = qa - n * A.skin, pb = qb + n * B.skin;
				return d - A.skin - B.skin;
			}
		}
		Vector ne;
		double depth = EPA (A, B, S, ne, qa, qb);
		n = -ne;
		pa = qa - n * A.skin, pb = qb + n * B.skin;
		return -depth - A.skin - B.skin;
	}

	// ===================================================================
	// Points of contact

	namespace {
		// the face of a placed polytope most aligned with d (common frame);
		// returns its alignment
		double BestFace (const Placed &P, const Vector &d, int &face)
		{
			Vector dl (tmul (P.R, d));
			double best = -2.0;
			face = 0;
			for (size_t i = 0; i < P.p->f.size(); i++) {
				double a = dotp (P.p->f[i].n, dl);
				if (a > best) best = a, face = (int)i;
			}
			return best;
		}

		std::vector<Vector> FacePoly (const Placed &P, int face)
		{
			std::vector<Vector> poly;
			for (int i : P.p->f[face].idx) poly.push_back (P.pos + mul (P.R, P.p->v[i]));
			return poly;
		}

		// polygon clipped to the side of a plane where dotp (n, x - o) >= 0
		std::vector<Vector> Clip (const std::vector<Vector> &poly, const Vector &n, const Vector &o)
		{
			std::vector<Vector> out;
			for (size_t i = 0; i < poly.size(); i++) {
				const Vector &a = poly[i], &b = poly[(i+1) % poly.size()];
				double da = dotp (n, a - o), db = dotp (n, b - o);
				if (da >= 0.0) out.push_back (a);
				if ((da >= 0.0) != (db >= 0.0)) out.push_back (a + (b - a) * (da / (da - db)));
			}
			return out;
		}
	}

	bool Collide (const Placed &A, const Placed &B, double margin, Manifold &m)
	{
		Vector pa, pb, n;
		double sep = Distance (A, B, pa, pb, n);
		m.npt = 0;
		if (sep > margin) return false;
		m.n = n;

		// reference face (most aligned with the normal) and incident face
		int fa, fb;
		double alB = BestFace (B, n, fb), alA = BestFace (A, -n, fa);
		bool refB = alB >= alA;
		const Placed &Ref = refB ? B : A, &Inc = refB ? A : B;
		int fr = refB ? fb : fa, fi = refB ? fa : fb;
		std::vector<Vector> cand;
		std::vector<double> csep;
		if (std::max (alA, alB) > 0.95) { // (a face lies against the other body: within 18 degrees of the normal)
			std::vector<Vector> ref (FacePoly (Ref, fr)), inc (FacePoly (Inc, fi));
			Vector nr (mul (Ref.R, Ref.p->f[fr].n));
			for (size_t i = 0; i < ref.size() && !inc.empty(); i++) {
				Vector e (ref[(i+1) % ref.size()] - ref[i]);
				inc = Clip (inc, crossp (nr, e).unit(), ref[i]);
			}
			double skins = A.skin + B.skin;
			for (const Vector &q : inc) {
				double h = dotp (q - ref[0], nr);   // (the incident core above the reference core)
				double s = h - skins;
				if (s > margin) continue;
				Vector qr (q - nr * h);            // (on the reference core)
				// (halfway between the surfaces)
				cand.push_back ((q - nr * Inc.skin + qr + nr * Ref.skin) * 0.5);
				csep.push_back (s);
			}
		}
		if (cand.empty()) { // (edges, corners: the closest points)
			m.pt[0] = (pa + pb) * 0.5;
			m.sep[0] = sep;
			m.npt = 1;
			return true;
		}

		// at most four: the deepest, the furthest from it, and those that
		// span the largest area with them
		std::vector<int> pick;
		int i0 = 0;
		for (size_t i = 1; i < cand.size(); i++) if (csep[i] < csep[i0]) i0 = (int)i;
		pick.push_back (i0);
		while (pick.size() < 4 && pick.size() < cand.size()) {
			int bi = -1;
			double bs = -1.0;
			for (size_t i = 0; i < cand.size(); i++) {
				if (std::find (pick.begin(), pick.end(), (int)i) != pick.end()) continue;
				double s;
				if (pick.size() == 1) s = cand[i].dist2 (cand[pick[0]]);
				else if (pick.size() == 2) s = crossp (cand[pick[1]] - cand[pick[0]], cand[i] - cand[pick[0]]).length();
				else {
					s = 1e300;
					for (int k = 0; k < 3; k++) // (smallest triangle it makes with an edge: largest is best)
						s = std::min (s, crossp (cand[pick[(k+1)%3]] - cand[pick[k]], cand[i] - cand[pick[k]]).length());
				}
				if (s > bs) bs = s, bi = (int)i;
			}
			if (bi < 0 || bs <= 1e-12) break;
			pick.push_back (bi);
		}
		for (int i : pick) {
			m.pt[m.npt] = cand[i];
			m.sep[m.npt] = csep[i];
			m.npt++;
		}
		return true;
	}

} // namespace convex
