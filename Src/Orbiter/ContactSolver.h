// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Contacts of rigid bodies, resolved with impulses once per time step
// (after the bodies were propagated without contact forces): touchdown
// points against the planet's surface, and vessels against each other.
// Each contact is a constraint along its normal, softened by a stiffness
// and damping where given (an implicit spring-damper, stable for any
// stiffness and time step), with Coulomb friction along two tangents.
// All vectors are in a common frame with the axes of the global frame.
// =======================================================================

#ifndef __CONTACTSOLVER_H
#define __CONTACTSOLVER_H

#include "Vecmat.h"
#include <vector>

namespace contact {

	/// A rigid body taking part in contacts
	struct Body {
		double mass;  ///< [kg]
		Vector pmi;   ///< principal moments of inertia per unit mass (body frame)
		Matrix R;     ///< body frame -> common frame
		Vector v;     ///< velocity of the centre of gravity
		Vector w;     ///< angular velocity
		Vector vp, wp; ///< (result) pseudo-velocities that remove penetration of rigid contacts: they move
		               ///< the body over the step, but are not kept (no energy gained by the correction)
	};

	/// A point of contact of body a with body b, or with a fixed surface
	/// (b < 0: the ground, or a vessel at rest on it), or about to be within
	/// the step
	struct Point {
		int a, b;           ///< bodies (b < 0: fixed surface)
		Vector ra, rb;      ///< the point relative to the bodies' centres of gravity
		Vector n;           ///< normal (from b towards a)
		Vector t1, t2;      ///< tangents (t1: rolling direction of a wheel)
		Vector vs;          ///< velocity of the fixed surface at the point (b < 0)
		double C;           ///< separation at the start of the step (< 0: penetration) [m]
		double k, c;        ///< stiffness [N/m], damping [N s/m] (k = 0: rigid)
		double mu1, mu2;    ///< friction coefficients along t1, t2
		double *warm;       ///< impulses of the previous step (normal, t1, t2), for warm starting (or null)

		// (solver)
		double Kn, K1, K2;  // inverse effective mass along n, t1, t2
		double gamma, bias; // softness, velocity bias of the normal constraint
		double pbias, lp;   // rigid contacts: bias of the position correction, its accumulated pseudo-impulse
		double ln, l1, l2;  // accumulated impulses
	};

	/// Resolve the contacts over a time step h [s]: the bodies' velocities are
	/// changed by the contact impulses. P, L (optional, one per body): total
	/// impulse on each body and its moment about the centre of gravity.
	void Solve (std::vector<Point> &pts, std::vector<Body> &bodies, double h,
		std::vector<Vector> *P = nullptr, std::vector<Vector> *L = nullptr);

	/// Tangent directions for a normal n (t1 along d where possible)
	void Tangents (const Vector &n, const Vector &d, Vector &t1, Vector &t2);

} // namespace contact

#endif // !__CONTACTSOLVER_H
