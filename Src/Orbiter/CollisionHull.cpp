// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CollisionHull.h"
#include "Orbiter.h"
#include "Config.h"
#include "Mesh.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

extern Orbiter *g_pOrbiter;

namespace collision {

	namespace {
		const double MIN_EXTENT = 0.1; // [m] groups smaller than this are left out

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

		// directions for the test points (the extreme vertices along them)
		const std::vector<Vector> &PointDirs ()
		{
			static std::vector<Vector> dirs = Sphere (2); // (162)
			return dirs;
		}
	}

	const std::vector<Vector> &Dirs ()
	{
		static std::vector<Vector> dirs = Sphere (1); // (42)
		return dirs;
	}

	double Hull::Distance (const Vector &p, int &face) const
	{
		const std::vector<Vector> &D = Dirs();
		double dmax = -1e30;
		face = 0;
		for (size_t i = 0; i < D.size(); i++) {
			double e = dotp (D[i], p) - d[i];
			if (e > dmax) dmax = e, face = (int)i;
		}
		return dmax;
	}

	Hull MakeHull (const std::vector<Vector> &pts, double skin)
	{
		Hull h;
		const std::vector<Vector> &D = Dirs();
		h.d.assign (D.size(), -1e30);
		for (const Vector &p : pts)
			for (size_t i = 0; i < D.size(); i++)
				h.d[i] = std::max (h.d[i], dotp (D[i], p));
		for (double &d : h.d) d += skin;

		// test points: the extreme points along many directions
		std::vector<int> idx;
		for (const Vector &dir : PointDirs()) {
			int best = 0;
			double bmax = -1e30;
			for (size_t j = 0; j < pts.size(); j++) {
				double e = dotp (dir, pts[j]);
				if (e > bmax) bmax = e, best = (int)j;
			}
			idx.push_back (best);
		}
		std::sort (idx.begin(), idx.end());
		idx.erase (std::unique (idx.begin(), idx.end()), idx.end());
		for (int j : idx) h.pts.push_back (pts[j]);

		// bounding sphere (about the centre of the bounding box)
		Vector lo (1e30, 1e30, 1e30), hi (-1e30, -1e30, -1e30);
		for (const Vector &p : pts)
			for (int k = 0; k < 3; k++) lo.data[k] = std::min (lo.data[k], p.data[k]), hi.data[k] = std::max (hi.data[k], p.data[k]);
		h.centre = (lo + hi) * 0.5;
		h.radius = 0.0;
		for (const Vector &p : pts) h.radius = std::max (h.radius, p.dist (h.centre));
		h.radius += skin;
		return h;
	}

	Hull Offset (const Hull &h, const Vector &ofs)
	{
		Hull o (h);
		const std::vector<Vector> &D = Dirs();
		for (size_t i = 0; i < D.size(); i++) o.d[i] += dotp (D[i], ofs);
		for (Vector &p : o.pts) p += ofs;
		o.centre += ofs;
		return o;
	}

	const std::vector<Hull> &MeshHulls (const std::string &key, const Mesh &mesh)
	{
		static std::map<std::string, std::vector<Hull>> cache;
		auto it = cache.find (key);
		if (it != cache.end()) return it->second;

		std::vector<Hull> &hulls = cache[key];
		std::vector<Vector> pts;
		for (DWORD g = 0; g < mesh.nGroup(); g++) {
			const GroupSpec *grp = const_cast<Mesh&>(mesh).GetGroup (g);
			if (!grp || grp->nVtx < 3 || (grp->Flags & 0x2)) continue; // (hidden groups)
			pts.clear();
			for (DWORD k = 0; k < grp->nVtx; k++)
				pts.push_back (Vector (grp->Vtx[k].x, grp->Vtx[k].y, grp->Vtx[k].z));
			Vector lo (1e30, 1e30, 1e30), hi (-1e30, -1e30, -1e30);
			for (const Vector &p : pts)
				for (int i = 0; i < 3; i++) lo.data[i] = std::min (lo.data[i], p.data[i]), hi.data[i] = std::max (hi.data[i], p.data[i]);
			if ((hi - lo).length() < MIN_EXTENT) continue;
			hulls.push_back (MakeHull (pts, SKIN));
		}
		LOGOUT ("Collision hulls of mesh %s: %d", key.c_str(), (int)hulls.size());
		return hulls;
	}

	bool DockCapture (const Port &a, const Port &b, double maxOffset)
	{
		const CFG_PHYSICSPRM &prm = g_pOrbiter->Cfg()->CfgPhysicsPrm;
		if ((a.vel - b.vel).length() > prm.DockSpeedLimit) return false;
		double c = cos (prm.DockAngleLimit);
		if (-dotp (a.dir, b.dir) < c) return false;  // (facing each other)
		if (dotp (a.rot, b.rot) < c) return false;   // (turned alike about the axis)
		Vector d (b.pos - a.pos);
		return (d - a.dir * dotp (d, a.dir)).length() <= maxOffset;
	}

	bool ClassHulls (const char *classname, std::vector<Hull> &hulls)
	{
		std::string name = std::string ("Collision/") + classname + ".cfg";
		std::ifstream ifs (g_pOrbiter->Cfg()->ConfigPathNoext (name.c_str()));
		if (!ifs.good()) return false;
		hulls.clear();
		std::string line;
		std::vector<Vector> pts;
		bool inHull = false;
		while (std::getline (ifs, line)) {
			size_t c = line.find (';');
			if (c != std::string::npos) line.erase (c);
			std::istringstream ss (line);
			std::string tok;
			if (!(ss >> tok)) continue;
			if (tok == "NONE") { hulls.clear(); return true; }
			if (tok == "HULL") { inHull = true; pts.clear(); continue; }
			if (tok == "END") {
				if (inHull && pts.size() >= 1) hulls.push_back (MakeHull (pts, SKIN));
				inHull = false;
				continue;
			}
			if (inHull) {
				Vector p;
				std::istringstream sp (line);
				if (sp >> p.x >> p.y >> p.z) pts.push_back (p);
			}
		}
		return true;
	}

} // namespace collision
