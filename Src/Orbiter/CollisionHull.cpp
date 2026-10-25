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
			hulls.push_back (MakeHull (pts));
		}
		LOGOUT ("Collision hulls of mesh %s: %d", key.c_str(), (int)hulls.size());
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
