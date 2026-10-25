// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Collision shapes of vessels: a set of convex hulls, one for each group
// of the vessel's external meshes (or as given in Config/Collision/
// <class>.cfg). Each hull is bounded by planes in a fixed set of
// directions (a discrete-orientation polytope) through the group's
// extreme vertices, which also serve as its test points: a vessel touches
// another where a test point of one is inside (or within a margin of) a
// hull of the other.
// =======================================================================

#ifndef __COLLISIONHULL_H
#define __COLLISIONHULL_H

#include "Vecmat.h"
#include <string>
#include <vector>

class Mesh;

namespace collision {

	/// A convex hull (in the frame of its vessel)
	struct Hull {
		std::vector<Vector> pts; ///< test points (extreme vertices)
		std::vector<double> d;   ///< plane distances along the directions (Dirs): inside where dotp (Dirs()[i], p) <= d[i]
		Vector centre;           ///< bounding sphere
		double radius;

		/// Signed distance of p (vessel frame) from the hull: the largest
		/// distance outside any of its planes (< 0: inside); face: that plane
		double Distance (const Vector &p, int &face) const;
	};

	/// A hull placed in the global frame
	struct HullRef {
		const Hull *hull;
		Matrix R;   ///< hull's frame -> global
		Vector pos; ///< position of the hull's frame (global)
	};

	/// A free docking port (global frame)
	struct Port {
		Vector pos, dir; ///< position, approach direction (out of the vessel)
	};

	/// The hull of a set of points (with a skin of thickness 'skin' added)
	Hull MakeHull (const std::vector<Vector> &pts, double skin);

	/// Plane directions of the hulls
	const std::vector<Vector> &Dirs ();

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
