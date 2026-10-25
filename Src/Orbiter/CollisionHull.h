// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Collision shapes of vessels: a set of convex hulls (ConvexGeometry.h),
// one for each group of the vessel's external meshes (or as defined by the
// vessel, or given in Config/Collision/<class>.cfg), each with a skin of
// SKIN around it.
// =======================================================================

#ifndef __COLLISIONHULL_H
#define __COLLISIONHULL_H

#include "Vecmat.h"
#include "ConvexGeometry.h"
#include <string>
#include <vector>

class Mesh;

namespace collision {

	/// A convex hull (in the frame of its vessel)
	typedef convex::Polytope Hull;

	/// A hull placed in the global frame
	struct HullRef {
		const Hull *hull;
		Matrix R;   ///< hull's frame -> global
		Vector pos; ///< position of the hull's frame (global)
	};

	/// A free docking port (global frame)
	struct Port {
		Vector pos, dir, rot; ///< position, approach direction (out of the vessel), longitudinal alignment
		Vector vel;           ///< velocity of the port
	};

	/// Whether two docking ports can capture each other: relative speed and
	/// misalignment within the limits (Orbiter.cfg: DockingSpeedLimit,
	/// DockingAngleLimit); lateral offset: also within maxOffset of each
	/// other's axis. Returns 0 if they can, otherwise the reason
	/// (DOCKREFUSED_*), with the value that exceeded its limit.
	int DockCapture (const Port &a, const Port &b, double maxOffset, double *value = nullptr, double *limit = nullptr);

	const double SKIN = 0.05; ///< [m] thickness added around each hull (thin parts, e.g. panels)

	/// The convex hull of a set of points
	Hull MakeHull (const std::vector<Vector> &pts);

	/// Hulls of the groups of a mesh (mesh frame); cached by key (the mesh's
	/// file name)
	const std::vector<Hull> &MeshHulls (const std::string &key, const Mesh &mesh);

	/// Hulls of a vessel class from Config/Collision/<classname>.cfg, if it
	/// exists: false if there is none. A file with NONE gives no hulls (the
	/// vessel does not collide with other vessels).
	bool ClassHulls (const char *classname, std::vector<Hull> &hulls);

	/// A hull moved by ofs (e.g. a mesh's offset in its vessel)
	Hull Offset (const Hull &h, const Vector &ofs);

} // namespace collision

#endif // !__COLLISIONHULL_H
