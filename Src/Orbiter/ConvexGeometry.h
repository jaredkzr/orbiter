// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Convex polytopes for collisions: building them from points (convex
// hull), and the contact between two of them: their distance (GJK), their
// penetration (EPA) and the points of contact (clipping their faces
// against each other). Each polytope may have a skin (a radius around
// it). Depends only on Vecmat.
// =======================================================================

#ifndef __CONVEXGEOMETRY_H
#define __CONVEXGEOMETRY_H

#include "Vecmat.h"
#include <vector>

namespace convex {

	/// A face: its plane (outward normal n; points x on it: dotp (n, x) = d)
	/// and its vertices, counter-clockwise around n
	struct Face {
		Vector n;
		double d;
		std::vector<int> idx;
	};

	/// A convex polytope
	struct Polytope {
		std::vector<Vector> v; ///< vertices
		std::vector<Face> f;   ///< faces
		Vector centre;         ///< bounding sphere
		double radius = 0.0;
		double volume = 0.0;

		/// vertex furthest along d
		int Support (const Vector &d) const;
		/// moved by ofs
		void Translate (const Vector &ofs);
	};

	/// Convex hull of a set of points. Flat or degenerate sets are given a
	/// minimal thickness (thin: 1 mm). False if there are no points.
	bool Build (const std::vector<Vector> &pts, Polytope &P);

	/// A polytope placed in a common frame (p' = R p + pos), with a skin
	struct Placed {
		const Polytope *p;
		Matrix R;
		Vector pos;
		double skin;
		Vector Support (const Vector &d) const; ///< (common frame, without the skin)
	};

	/// Contact between two placed polytopes A and B
	struct Manifold {
		Vector n;            ///< normal, from B towards A
		int npt = 0;
		Vector pt[4];        ///< points of contact (common frame)
		double sep[4];       ///< separation of the surfaces there along n (< 0: penetration)
	};

	/// Distance between the surfaces (skins included; < 0: penetration depth)
	/// and the closest points (or deepest, when penetrating) on A and B
	double Distance (const Placed &A, const Placed &B, Vector &pa, Vector &pb, Vector &n);

	/// Points of contact of A and B where their surfaces are closer than
	/// margin (or penetrate). False if they are further apart.
	bool Collide (const Placed &A, const Placed &B, double margin, Manifold &m);

} // namespace convex

#endif // !__CONVEXGEOMETRY_H
