// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Ground contact of a rigid body: touchdown points against the planet's
// surface, resolved with impulses once per time step (after the body was
// propagated without contact forces). Each point is a constraint along the
// local terrain normal, softened by the point's stiffness and damping (an
// implicit spring-damper, stable for any stiffness and time step), with
// Coulomb friction along the rolling and lateral directions.
// =======================================================================

#ifndef __GROUNDCONTACT_H
#define __GROUNDCONTACT_H

#include "Vecmat.h"
#include <vector>

namespace contact {

	/// A touchdown point in contact (or about to be) with the ground. All
	/// vectors are in the frame of the body being resolved.
	struct Point {
		Vector r;           ///< position relative to the centre of gravity
		Vector n;           ///< terrain normal (out of the ground)
		Vector t1, t2;      ///< tangents: longitudinal (rolling), lateral
		Vector vg;          ///< velocity of the ground at the point
		double C;           ///< distance above the ground at the start of the step (< 0: penetration) [m]
		double k, c;        ///< stiffness [N/m], damping [N s/m] (k = 0: rigid)
		double mu1, mu2;    ///< friction coefficients along t1, t2
		double *warm;       ///< accumulated impulses of the previous step (normal, t1, t2), for warm starting

		// (solver)
		double Kn, K1, K2;  // inverse effective mass along n, t1, t2
		double gamma, bias; // softness, velocity bias of the normal constraint
		double ln, l1, l2;  // accumulated impulses
	};

	/// The body: mass properties and velocities (body frame)
	struct Body {
		double mass;
		Vector pmi;   ///< principal moments of inertia per unit mass
		Vector v;     ///< velocity of the centre of gravity
		Vector w;     ///< angular velocity
	};

	/// Resolve the contacts over a time step h [s]: the body's velocities
	/// are changed by the contact impulses. Returns the total impulse P and
	/// its moment L (about the centre of gravity), body frame.
	void Solve (std::vector<Point> &pts, Body &b, double h, Vector &P, Vector &L);

} // namespace contact

#endif // !__GROUNDCONTACT_H
