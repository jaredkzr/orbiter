// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Structure.h"
#include "Orbiter.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

extern Orbiter *g_pOrbiter;

namespace structure {

	double SignedDistance (const convex::Polytope &h, const Vector &p)
	{
		double d = -1e30;
		for (const convex::Face &f : h.f) d = std::max (d, dotp (f.n, p) - f.d);
		if (h.f.empty()) d = (p - h.centre).length() - h.radius;
		return d;
	}

	// =======================================================================

	ClassConfig ReadClassConfig (const char *classname)
	{
		ClassConfig cfg;
		std::string name = std::string ("Damage/") + classname + ".cfg";
		std::ifstream ifs (g_pOrbiter->Cfg()->ConfigPathNoext (name.c_str()));
		if (!ifs.good()) return cfg;
		cfg.found = true;
		std::string line;
		while (std::getline (ifs, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			std::istringstream ss (line);
			std::string tok;
			if (!(ss >> tok)) continue;
			if (tok == "LOADFACTOR") ss >> cfg.loadFactor;
			else if (tok == "SAFETY") ss >> cfg.safety;
			else if (tok == "PARTS") ss >> cfg.nparts;
			else if (tok == "INDESTRUCTIBLE") cfg.indestructible = true;
			else if (tok == "PART") {
				// PART <name> BOX x0 y0 z0 x1 y1 z1 [LOADFACTOR g] [STRENGTH N]
				// PART <name> GROUPS m:g m:g ... [LOADFACTOR g] [STRENGTH N]
				ClassConfig::Named np;
				ss >> np.name;
				std::string kw;
				while (ss >> kw) {
					if (kw == "BOX") {
						Vector a, b;
						ss >> a.x >> a.y >> a.z >> b.x >> b.y >> b.z;
						np.box = true;
						for (int i = 0; i < 3; i++)
							np.lo.data[i] = std::min (a.data[i], b.data[i]), np.hi.data[i] = std::max (a.data[i], b.data[i]);
					} else if (kw == "GROUPS") {
						std::string g;
						while (ss >> g) {
							int m, k;
							if (sscanf (g.c_str(), "%d:%d", &m, &k) == 2) np.groups.push_back ({m, k});
							else { kw = g; break; }
						}
						if (kw == "LOADFACTOR") ss >> np.loadFactor;
						else if (kw == "STRENGTH") ss >> np.strength;
					} else if (kw == "LOADFACTOR") ss >> np.loadFactor;
					else if (kw == "STRENGTH") ss >> np.strength;
				}
				if (!np.name.empty() && (np.box || !np.groups.empty())) cfg.parts.push_back (np);
			}
		}
		return cfg;
	}

	// =======================================================================

	namespace {

		uint64_t Mix (uint64_t x)
		{
			// (splitmix64)
			x += 0x9e3779b97f4a7c15ull;
			x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
			x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
			return x ^ (x >> 31);
		}

		double Uniform (uint64_t seed, uint64_t i)
		{
			return (Mix (seed ^ Mix (i)) >> 11) * (1.0 / 9007199254740992.0);
		}

		// a name from where the part lies in the vessel's bounding box
		std::string Locate (const Vector &p, const Vector &lo, const Vector &hi)
		{
			auto side = [&](int i, const char *neg, const char *pos) -> std::string {
				double c = 0.5 * (lo.data[i] + hi.data[i]), e = 0.5 * (hi.data[i] - lo.data[i]);
				if (e <= 0.0) return "";
				double u = (p.data[i] - c) / e;
				return u < -0.33 ? neg : u > 0.33 ? pos : "";
			};
			std::string s;
			for (const std::string &w : { side (2, "aft", "forward"), side (0, "left", "right"), side (1, "lower", "upper") })
				if (!w.empty()) s += (s.empty() ? "" : " ") + w;
			return s.empty() ? "centre section" : s + " section";
		}
	}

	namespace {
		typedef std::tuple<int,int,int> Cell;
	}

	void Model::Build (const std::vector<convex::Polytope> &hulls, const std::vector<Triangle> &tris,
		double emass, double size, const ClassConfig &cfg, uint64_t seed)
	{
		parts.clear();
		hullPart.assign (hulls.size(), 0);
		triPart.assign (tris.size(), 0);
		cells.clear();
		cellPart.clear();
		indestructible = cfg.indestructible;
		const int nh = (int)hulls.size();

		// The surface is sampled into cells of a grid (symmetric about x = 0):
		// the cells are the elements clustered into parts. Each triangle
		// belongs to the cell of its centre; its area is shared by the cells
		// it covers, and cells next to each other are connected.
		cellSize = std::clamp (size / 25.0, 0.05, 2.0);
		const double cs = cellSize;
		auto cellOf = [cs](const Vector &x) {
			return Cell ((int)std::floor (x.x / cs), (int)std::floor (x.y / cs), (int)std::floor (x.z / cs));
		};
		std::map<Cell,int> cellIdx;
		std::vector<double> w;                  // area in each cell
		std::vector<Vector> wp;                 // area-weighted sum of positions
		std::vector<std::map<std::pair<int,int>,double>> grpArea; // area by mesh group
		auto cellAt = [&](const Cell &c) {
			auto it = cellIdx.find (c);
			if (it != cellIdx.end()) return it->second;
			int i = (int)cells.size();
			cellIdx[c] = i;
			cells.push_back (Vector ((std::get<0>(c) + 0.5) * cs, (std::get<1>(c) + 0.5) * cs, (std::get<2>(c) + 0.5) * cs));
			w.push_back (0.0), wp.push_back (Vector(0,0,0)), grpArea.emplace_back();
			return i;
		};
		std::vector<int> triCell (tris.size());
		double atot = 0.0;
		for (size_t t = 0; t < tris.size(); t++) {
			const Triangle &tr = tris[t];
			Vector c ((tr.a + tr.b + tr.c) / 3.0);
			double a = 0.5 * crossp (tr.b - tr.a, tr.c - tr.a).length();
			atot += a;
			triCell[t] = cellAt (cellOf (c));
			double emax = std::max ({(tr.b - tr.a).length(), (tr.c - tr.b).length(), (tr.a - tr.c).length()});
			int n = std::clamp ((int)std::ceil (emax / cs), 1, 64);
			int ns = (n + 1) * (n + 2) / 2;
			for (int i = 0; i <= n; i++)
				for (int j = 0; i + j <= n; j++) {
					double u = (double)i / n, v = (double)j / n;
					Vector x (tr.a + (tr.b - tr.a) * u + (tr.c - tr.a) * v);
					int ci = cellAt (cellOf (x));
					w[ci] += a / ns, wp[ci] += x * (a / ns);
					grpArea[ci][{tr.mesh, tr.group}] += a / ns;
				}
		}
		const int ne = (int)cells.size();
		if (!ne) { // (no triangles: a single part)
			Part core;
			core.name = "core";
			core.mass = core.subtreeMass = emass;
			core.loadFactor = cfg.loadFactor;
			core.radius = size;
			for (int h = 0; h < nh; h++) core.hulls.push_back (h);
			parts.push_back (core);
			return;
		}
		std::vector<Vector> p (ne);
		std::vector<double> wc (ne);
		double wtot = 0.0;
		for (int e = 0; e < ne; e++) {
			p[e] = w[e] > 0.0 ? wp[e] / w[e] : cells[e];
			wc[e] = std::max (w[e], 1e-9 * (atot + 1.0));
			wtot += wc[e];
		}
		std::vector<std::vector<int>> adj (ne);
		for (const auto &ci : cellIdx) {
			const Cell &c = ci.first;
			for (int dx = -1; dx <= 1; dx++)
				for (int dy = -1; dy <= 1; dy++)
					for (int dz = -1; dz <= 1; dz++) {
						if (!dx && !dy && !dz) continue;
						auto it = cellIdx.find (Cell (std::get<0>(c) + dx, std::get<1>(c) + dy, std::get<2>(c) + dz));
						if (it != cellIdx.end()) adj[ci.second].push_back (it->second);
					}
		}

		// named parts first
		std::vector<int> label (ne, -1);
		std::vector<bool> named;
		int nlabel = 0;
		for (const ClassConfig::Named &np : cfg.parts) {
			for (int e = 0; e < ne; e++) {
				if (label[e] >= 0) continue;
				bool in = false;
				if (np.box) {
					in = true;
					for (int i = 0; i < 3; i++)
						if (p[e].data[i] < np.lo.data[i] || p[e].data[i] > np.hi.data[i]) in = false;
				} else {
					double ain = 0.0, aall = 0.0;
					for (const auto &ga : grpArea[e]) {
						aall += ga.second;
						if (std::find (np.groups.begin(), np.groups.end(), ga.first) != np.groups.end()) ain += ga.second;
					}
					in = aall > 0.0 && ain > 0.5 * aall;
				}
				if (in) label[e] = nlabel;
			}
			named.push_back (true);
			nlabel++;
		}

		// mirror symmetry about the x = 0 plane (most vessels): mirrored cells
		// found among them, by area
		bool sym = false;
		{
			double am = 0.0;
			for (const auto &ci : cellIdx) {
				const Cell &c = ci.first;
				if (cellIdx.count (Cell (-std::get<0>(c) - 1, std::get<1>(c), std::get<2>(c)))) am += wc[ci.second];
			}
			sym = am > 0.8 * wtot;
		}

		// the others clustered (k-means, weighted by area; started from the
		// heaviest cell and then from those furthest from the centres so far).
		// Symmetric: clustered folded onto one side, then clusters away from
		// the plane split into a left and a right part.
		std::vector<int> rest;
		for (int e = 0; e < ne; e++) if (label[e] < 0) rest.push_back (e);
		int k = cfg.nparts > 0 ? cfg.nparts : std::clamp ((int)std::lround (size), 4, 32);
		if (sym) k = (k * 2 + 2) / 3; // (most clusters give two parts)
		k = std::min (k, (int)rest.size());
		std::vector<Vector> q (p);
		if (sym) for (Vector &x : q) x.x = std::abs (x.x);
		if (k > 0) {
			std::vector<Vector> c;
			int e0 = rest[0];
			for (int e : rest) if (wc[e] > wc[e0]) e0 = e;
			c.push_back (q[e0]);
			while ((int)c.size() < k) {
				int be = -1;
				double bd = -1.0;
				for (int e : rest) {
					double d = 1e30;
					for (const Vector &x : c) d = std::min (d, (q[e] - x).length2());
					d *= wc[e]; // (no tiny seeds far out)
					if (d > bd) bd = d, be = e;
				}
				if (be < 0 || bd <= 0.0) break;
				c.push_back (q[be]);
			}
			k = (int)c.size();
			std::vector<int> lab (ne, -1);
			for (int it = 0; it < 100; it++) {
				bool changed = false;
				for (int e : rest) {
					int best = 0;
					for (int j = 1; j < k; j++)
						if ((q[e] - c[j]).length2() < (q[e] - c[best]).length2()) best = j;
					if (lab[e] != best) lab[e] = best, changed = true;
				}
				if (!changed) break;
				std::vector<Vector> s (k);
				std::vector<double> sw (k, 0.0);
				for (int e : rest) s[lab[e]] += q[e] * wc[e], sw[lab[e]] += wc[e];
				for (int j = 0; j < k; j++) if (sw[j] > 0.0) c[j] = s[j] / sw[j];
			}
			// (split: the clusters which do not reach across the plane)
			std::vector<double> sw (k, 0.0), swp (k, 0.0);
			for (int e : rest) {
				sw[lab[e]] += wc[e];
				if (std::abs (cells[e].x) < cs) swp[lab[e]] += wc[e];
			}
			for (int j = 0; j < k; j++) {
				bool split = sym && swp[j] < 0.02 * sw[j];
				int l0 = nlabel;
				named.push_back (false), nlabel++;
				int l1 = l0;
				if (split) l1 = nlabel++, named.push_back (false);
				for (int e : rest)
					if (lab[e] == j) label[e] = (split && cells[e].x < 0.0) ? l1 : l0;
			}
		}

		// automatic parts made connected: pieces of a cluster apart from its
		// largest go to the part they touch most (or become parts of their own)
		for (int pass = 0; pass < 5; pass++) {
			bool changed = false;
			std::vector<int> comp (ne, -1);
			std::vector<std::vector<int>> comps;
			for (int e = 0; e < ne; e++) {
				if (comp[e] >= 0 || named[label[e]]) continue;
				std::vector<int> qq (1, e);
				comp[e] = (int)comps.size();
				for (size_t i = 0; i < qq.size(); i++)
					for (int j : adj[qq[i]])
						if (comp[j] < 0 && label[j] == label[e]) comp[j] = comp[e], qq.push_back (j);
				comps.push_back (qq);
			}
			std::vector<int> largest (nlabel, -1);
			std::vector<double> lw (nlabel, -1.0);
			for (size_t ci = 0; ci < comps.size(); ci++) {
				double sw = 0.0;
				for (int e : comps[ci]) sw += wc[e];
				int l = label[comps[ci][0]];
				if (sw > lw[l]) lw[l] = sw, largest[l] = (int)ci;
			}
			for (size_t ci = 0; ci < comps.size(); ci++) {
				int l = label[comps[ci][0]];
				if (largest[l] == (int)ci) continue;
				std::map<int,int> touch;
				for (int e : comps[ci]) for (int j : adj[e]) if (label[j] != l) touch[label[j]]++;
				int nl = -1, nt = 0;
				for (const auto &t : touch) if (t.second > nt) nt = t.second, nl = t.first;
				if (nl < 0) { nl = nlabel++; named.push_back (false); }
				for (int e : comps[ci]) label[e] = nl;
				changed = true;
			}
			if (!changed) break;
		}

		// small automatic parts merged into the part they touch most (or the nearest)
		{
			std::vector<double> la (nlabel, 0.0);
			std::vector<Vector> lp (nlabel);
			for (int e = 0; e < ne; e++) la[label[e]] += wc[e], lp[label[e]] += p[e] * wc[e];
			for (int l = 0; l < nlabel; l++) if (la[l] > 0.0) lp[l] /= la[l];
			for (int l = 0; l < nlabel; l++) {
				if (named[l] || la[l] <= 0.0 || la[l] >= 0.01 * wtot) continue;
				std::map<int,int> touch;
				for (int e = 0; e < ne; e++)
					if (label[e] == l) for (int j : adj[e]) if (label[j] != l) touch[label[j]]++;
				int nl = -1, nt = 0;
				for (const auto &t : touch) if (t.second > nt) nt = t.second, nl = t.first;
				if (nl < 0) {
					double bd = 1e30;
					for (int m = 0; m < nlabel; m++)
						if (m != l && la[m] > 0.0 && (lp[m] - lp[l]).length() < bd) bd = (lp[m] - lp[l]).length(), nl = m;
				}
				if (nl < 0) continue;
				for (int e = 0; e < ne; e++) if (label[e] == l) label[e] = nl;
				lp[nl] = (lp[nl] * la[nl] + lp[l] * la[l]) / (la[nl] + la[l]);
				la[nl] += la[l], la[l] = 0.0;
			}
		}

		// labels in use, numbered
		std::vector<int> idx (nlabel, -1);
		std::vector<int> labOf;
		for (int e = 0; e < ne; e++)
			if (idx[label[e]] < 0) idx[label[e]] = (int)labOf.size(), labOf.push_back (label[e]);
		const int np = (int)labOf.size();
		for (int e = 0; e < ne; e++) label[e] = idx[label[e]];

		// connections between the parts (number of neighbouring cells)
		std::vector<std::map<int,int>> link (np);
		for (int e = 0; e < ne; e++)
			for (int j : adj[e])
				if (label[j] != label[e]) link[label[e]][label[j]]++;

		// part areas and centres
		std::vector<double> la (np, 0.0);
		std::vector<Vector> lp (np);
		for (int e = 0; e < ne; e++) la[label[e]] += wc[e], lp[label[e]] += p[e] * wc[e];
		for (int l = 0; l < np; l++) if (la[l] > 0.0) lp[l] /= la[l];

		// the core part: at the vessel's origin (the cell nearest to it)
		int core = 0;
		{
			double bd = 1e30;
			for (int e = 0; e < ne; e++)
				if (p[e].length() < bd) bd = p[e].length(), core = label[e];
		}

		// tree: each part hangs from a part it touches that is one step
		// nearer the core (the one it touches most); parts touching none of
		// those hang from the nearest part (by centre) already in the tree
		std::vector<int> par (np, -1);
		std::vector<bool> in (np, false);
		in[core] = true;
		std::vector<int> front (1, core);
		int nin = 1;
		while (nin < np) {
			std::vector<int> next;
			std::map<int,std::pair<int,int>> best; // part -> (links, parent)
			for (int a : front)
				for (const auto &l : link[a]) {
					if (in[l.first]) continue;
					auto it = best.find (l.first);
					if (it == best.end() || l.second > it->second.first) best[l.first] = {l.second, a};
				}
			for (const auto &b : best) {
				par[b.first] = b.second.second, in[b.first] = true, nin++;
				next.push_back (b.first);
			}
			if (next.empty()) {
				int ba = -1, bb = -1;
				double bd = 1e30;
				for (int a = 0; a < np; a++) {
					if (!in[a]) continue;
					for (int b = 0; b < np; b++)
						if (!in[b] && (lp[a] - lp[b]).length() < bd) bd = (lp[a] - lp[b]).length(), ba = a, bb = b;
				}
				par[bb] = ba, in[bb] = true, nin++;
				next.push_back (bb);
			}
			front = next;
		}

		// numbered from the core outwards (parents before children)
		std::vector<std::vector<int>> ch (np);
		for (int l = 0; l < np; l++) if (par[l] >= 0) ch[par[l]].push_back (l);
		std::vector<int> order (1, core);
		for (size_t i = 0; i < order.size(); i++)
			for (int c : ch[order[i]]) order.push_back (c);
		std::vector<int> num (np);
		for (int i = 0; i < np; i++) num[order[i]] = i;

		cellPart.resize (ne);
		for (int e = 0; e < ne; e++) cellPart[e] = num[label[e]];
		for (size_t t = 0; t < tris.size(); t++) triPart[t] = cellPart[triCell[t]];

		// bounding box of the vessel (names)
		Vector lo (1e30, 1e30, 1e30), hi (-1e30, -1e30, -1e30);
		for (int e = 0; e < ne; e++)
			for (int i = 0; i < 3; i++) lo.data[i] = std::min (lo.data[i], p[e].data[i]), hi.data[i] = std::max (hi.data[i], p[e].data[i]);

		parts.resize (np);
		for (int i = 0; i < np; i++) {
			Part &pt = parts[i];
			int l = order[i];
			pt.parent = par[l] >= 0 ? num[par[l]] : -1;
			pt.area = la[l];
			pt.cg = lp[l];
			pt.mass = emass * la[l] / wtot;
			pt.loadFactor = cfg.loadFactor;
		}
		for (int e = 0; e < ne; e++) {
			Part &pt = parts[cellPart[e]];
			pt.radius = std::max (pt.radius, (cells[e] - pt.cg).length() + 0.87 * cs);
		}

		// the vessel's hulls: each to the part of the cell nearest its centre
		for (int h = 0; h < nh; h++) {
			hullPart[h] = PartAt (hulls[h].centre);
			parts[hullPart[h]].hulls.push_back (h);
		}

		// names and settings of the named parts
		std::vector<int> namedOf (np, -1);
		for (int l = 0; l < np; l++)
			if (labOf[l] < (int)cfg.parts.size()) namedOf[num[l]] = labOf[l];
		std::map<std::string,int> used;
		for (int i = 0; i < np; i++) {
			Part &pt = parts[i];
			if (namedOf[i] >= 0) {
				const ClassConfig::Named &nc = cfg.parts[namedOf[i]];
				pt.name = nc.name;
				if (nc.loadFactor > 0.0) pt.loadFactor = nc.loadFactor;
			} else {
				pt.name = i == 0 ? std::string ("core") : Locate (pt.cg, lo, hi);
			}
			int n = ++used[pt.name];
			if (n > 1) pt.name += " " + std::to_string (n);
		}

		// joints
		for (int i = np - 1; i >= 0; i--) {
			parts[i].subtreeMass += parts[i].mass;
			if (parts[i].parent >= 0) {
				parts[parts[i].parent].subtreeMass += parts[i].subtreeMass;
				parts[parts[i].parent].children.insert (parts[parts[i].parent].children.begin(), i);
			}
		}
		for (int i = 0; i < np; i++) {
			Part &pt = parts[i];
			if (pt.parent < 0) { pt.strength = 0.0; continue; }
			double v = 1.0 + STRENGTH_SPREAD * (2.0 * Uniform (seed, i) - 1.0);
			pt.strength = pt.loadFactor * cfg.safety * G0 * pt.subtreeMass * v;
			if (namedOf[i] >= 0 && cfg.parts[namedOf[i]].strength > 0.0) pt.strength = cfg.parts[namedOf[i]].strength;
		}
	}

	int Model::PartAt (const Vector &x) const
	{
		// (the part of the nearest cell)
		int best = -1;
		double bd = 1e30;
		for (size_t e = 0; e < cells.size(); e++) {
			double d = (cells[e] - x).length2();
			if (d < bd) bd = d, best = cellPart[e];
		}
		if (best < 0 && !parts.empty()) best = 0;
		return best;
	}

} // namespace structure
