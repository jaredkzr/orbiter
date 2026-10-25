// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "CollisionHull.h"
#include "ConvexDecomposition.h"
#include "Orbiter.h"
#include "Config.h"
#include "Mesh.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

extern Orbiter *g_pOrbiter;

namespace collision {

	namespace {
		const double MIN_EXTENT = 0.1; // [m] groups smaller than this are left out
	}

	Hull MakeHull (const std::vector<Vector> &pts)
	{
		Hull h;
		convex::Build (pts, h);
		return h;
	}

	Hull Offset (const Hull &h, const Vector &ofs)
	{
		Hull o (h);
		o.Translate (ofs);
		return o;
	}

	namespace {
		const uint32_t CACHE_VERSION = 2; // (of the decomposition: cached results of other versions are made again)

		uint64_t Hash (uint64_t h, const void *data, size_t n)
		{
			// (FNV-1a)
			const unsigned char *p = (const unsigned char*)data;
			for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
			return h;
		}

		std::string CachePath (uint64_t h)
		{
			char cbuf[64];
			snprintf (cbuf, sizeof(cbuf), "%016llx.bin", (unsigned long long)h);
			return std::string ("Cache/Collision/") + cbuf;
		}

		// cached parts: for each, its vertices (the hulls are made again from them)
		bool LoadParts (const std::string &path, std::vector<Hull> &hulls)
		{
			std::ifstream ifs (path, std::ios::binary);
			if (!ifs) return false;
			uint32_t ver = 0, n = 0;
			ifs.read ((char*)&ver, 4); ifs.read ((char*)&n, 4);
			if (!ifs || ver != CACHE_VERSION) return false;
			std::vector<Hull> out;
			for (uint32_t i = 0; i < n; i++) {
				uint32_t nv = 0;
				ifs.read ((char*)&nv, 4);
				if (!ifs || nv > 100000) return false;
				std::vector<Vector> v (nv);
				for (Vector &x : v) ifs.read ((char*)x.data, 3*sizeof(double));
				if (!ifs) return false;
				out.push_back (MakeHull (v));
			}
			hulls = std::move (out);
			return true;
		}

		void SaveParts (const std::string &path, const std::vector<Hull> &hulls)
		{
			std::error_code ec;
			std::filesystem::create_directories ("Cache/Collision", ec);
			std::ofstream ofs (path, std::ios::binary);
			if (!ofs) return;
			uint32_t ver = CACHE_VERSION, n = (uint32_t)hulls.size();
			ofs.write ((const char*)&ver, 4); ofs.write ((const char*)&n, 4);
			for (const Hull &h : hulls) {
				uint32_t nv = (uint32_t)h.v.size();
				ofs.write ((const char*)&nv, 4);
				for (const Vector &x : h.v) ofs.write ((const char*)x.data, 3*sizeof(double));
			}
		}
	}

	const std::vector<Hull> &MeshHulls (const std::string &key, const Mesh &mesh)
	{
		static std::map<std::string, std::vector<Hull>> cache;
		auto it = cache.find (key);
		if (it != cache.end()) return it->second;
		std::vector<Hull> &hulls = cache[key];

		// the groups' triangles (hidden groups and tiny ones left out)
		std::vector<std::vector<convex::Triangle>> groups;
		uint64_t h = Hash (1469598103934665603ull, &CACHE_VERSION, sizeof(CACHE_VERSION));
		for (DWORD g = 0; g < mesh.nGroup(); g++) {
			const GroupSpec *grp = const_cast<Mesh&>(mesh).GetGroup (g);
			if (!grp || grp->nVtx < 3 || grp->nIdx < 3 || (grp->Flags & 0x2)) continue;
			Vector lo (1e30, 1e30, 1e30), hi (-1e30, -1e30, -1e30);
			for (DWORD k = 0; k < grp->nVtx; k++) {
				Vector p (grp->Vtx[k].x, grp->Vtx[k].y, grp->Vtx[k].z);
				for (int i = 0; i < 3; i++) lo.data[i] = std::min (lo.data[i], p.data[i]), hi.data[i] = std::max (hi.data[i], p.data[i]);
			}
			if ((hi - lo).length() < MIN_EXTENT) continue;
			std::vector<convex::Triangle> t;
			for (DWORD k = 0; k + 2 < grp->nIdx; k += 3) {
				const NTVERTEX *v[3] = { grp->Vtx + grp->Idx[k], grp->Vtx + grp->Idx[k+1], grp->Vtx + grp->Idx[k+2] };
				t.push_back ({Vector (v[0]->x, v[0]->y, v[0]->z), Vector (v[1]->x, v[1]->y, v[1]->z), Vector (v[2]->x, v[2]->y, v[2]->z)});
			}
			for (const convex::Triangle &tr : t) h = Hash (h, &tr, sizeof(tr));
			groups.push_back (std::move (t));
		}

		// decomposed into convex parts (cached on disk)
		std::string path (CachePath (h));
		if (LoadParts (path, hulls)) {
			LOGOUT ("Collision hulls of mesh %s: %d (cached)", key.c_str(), (int)hulls.size());
			return hulls;
		}
		convex::DecomposeParams prm;
		for (const auto &t : groups)
			for (convex::Polytope &p : convex::Decompose (t, prm))
				hulls.push_back (std::move (p));
		SaveParts (path, hulls);
		LOGOUT ("Collision hulls of mesh %s: %d from %d groups", key.c_str(), (int)hulls.size(), (int)groups.size());
		return hulls;
	}

	int DockCapture (const Port &a, const Port &b, double maxOffset, double *value, double *limit)
	{
		const CFG_PHYSICSPRM &prm = g_pOrbiter->Cfg()->CfgPhysicsPrm;
		auto refuse = [&](int reason, double v, double l) {
			if (value) *value = v;
			if (limit) *limit = l;
			return reason;
		};
		double speed = (a.vel - b.vel).length();
		if (speed > prm.DockSpeedLimit) return refuse (DOCKREFUSED_SPEED, speed, prm.DockSpeedLimit);
		double approach = acos (std::clamp (-dotp (a.dir, b.dir), -1.0, 1.0)); // (facing each other)
		if (approach > prm.DockAngleLimit) return refuse (DOCKREFUSED_APPROACH, approach, prm.DockAngleLimit);
		double roll = acos (std::clamp (dotp (a.rot, b.rot), -1.0, 1.0));      // (turned alike about the axis)
		if (roll > prm.DockAngleLimit) return refuse (DOCKREFUSED_ROLL, roll, prm.DockAngleLimit);
		Vector d (b.pos - a.pos);
		double offset = (d - a.dir * dotp (d, a.dir)).length();
		if (offset > maxOffset) return refuse (DOCKREFUSED_OFFSET, offset, maxOffset);
		return 0;
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
				if (inHull && pts.size() >= 1) hulls.push_back (MakeHull (pts));
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
