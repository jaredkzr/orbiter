// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Approximate convex decomposition of a triangle mesh (for collision
// shapes): the mesh is split into parts whose convex hulls lie close to
// the part's surface. A part whose hull sticks out further than the
// tolerance from its triangles (it is concave: a bay, a fork, an L) is
// split by the plane that best reduces that, recursively. Works on open
// meshes too (distances to the surface, no volumes). Depends only on
// Vecmat and ConvexGeometry.
// =======================================================================

#ifndef __CONVEXDECOMPOSITION_H
#define __CONVEXDECOMPOSITION_H

#include "ConvexGeometry.h"
#include <vector>

namespace convex {

	struct Triangle {
		Vector a, b, c;
	};

	struct DecomposeParams {
		double tolerance = 0.1;  ///< [m] how far a part's hull may stick out from its surface ...
		double relTolerance = 0.02; ///< ... or this fraction of the mesh's size, if more
		int maxPieces = 16;      ///< parts at most (the worst are split first)
		double minExtent = 0.02; ///< [m] parts smaller than this are dropped
	};

	/// Convex parts of a mesh
	std::vector<Polytope> Decompose (const std::vector<Triangle> &tris, const DecomposeParams &prm);

	/// How far the hull of a set of triangles sticks out from them: the
	/// largest distance of points on the hull's surface from the triangles
	double Concavity (const std::vector<Triangle> &tris, const Polytope &hull, Vector *deepest = nullptr);

} // namespace convex

#endif // !__CONVEXDECOMPOSITION_H
