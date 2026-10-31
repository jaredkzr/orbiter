// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Conic sections for transfer planning: Lambert's problem, departure on a
// hyperbola from a point, the B-plane, flybys (depends only on the vector
// maths)
// =======================================================================

#ifndef __CONICS_H
#define __CONICS_H

#include "Vecmat.h"

namespace transfer {

	// ===================================================================
	// Conics

	/// Lambert's problem (Izzo's algorithm, no full revolutions): the
	/// velocities at r1 and r2 of the conic about a body of gravitational
	/// parameter mu that takes tof [s] from r1 to r2, moving in the sense of
	/// the angular momentum direction nref (or against it if retro).
	/// Returns false if there is no solution (r1, r2 collinear, tof <= 0).
	bool Lambert (const Vector &r1, const Vector &r2, double tof, double mu, const Vector &nref, bool retro,
		Vector &v1, Vector &v2);

	/// Lambert's problem with revs full revolutions about the body. With
	/// revs > 0 there are two solutions if tof is long enough: branch 0
	/// (left: the higher-energy, faster-moving one at short tof) and branch 1
	/// (right). false if this one doesn't exist.
	bool Lambert (const Vector &r1, const Vector &r2, double tof, double mu, const Vector &nref, bool retro,
		int revs, int branch, Vector &v1, Vector &v2);

	/// Velocity at position r on the hyperbola (about a body of gravitational
	/// parameter mu) whose outgoing asymptotic velocity is vinf: the
	/// departure from a point
	Vector DepartureVelocity (const Vector &r, const Vector &vinf, double mu);

	/// Impact parameter vector of a hyperbola with position r and velocity
	/// v: B = S x h / vinf (S: incoming asymptote). Also returns S and vinf.
	Vector BVector (const Vector &r, const Vector &v, double mu, Vector &S, double &vinf);

	// ===================================================================
	// Flybys (patched conics: the excess velocities relative to the body)

	/// Turn angle of the excess velocity on a hyperbola of periapsis
	/// radius rp and excess speed vinf about a body of parameter mu
	double FlybyTurn (double vinf, double rp, double mu);
	/// Periapsis radius of the hyperbola that turns the excess velocity by
	/// this angle [rad]
	double FlybyPeriapsis (double vinf, double turn, double mu);
	/// Powered flyby from excess velocity vin to vout: the periapsis radius
	/// rp at which the incoming and outgoing hyperbola halves turn by the
	/// angle between them, and the velocity change at the periapsis
	/// (dvPeri) for the change of speed. If rp is below rpMin, the flyby is
	/// at rpMin and the rest of the turn costs dvTurn (a burn far out).
	/// Returns dvPeri + dvTurn.
	double PoweredFlyby (const Vector &vin, const Vector &vout, double mu, double rpMin, double &rp, double &dvPeri, double &dvTurn);
	/// Unpowered flyby: outgoing excess velocity for incoming vin, periapsis
	/// radius rp and B-plane angle beta, measured about vin from the
	/// direction vin x vref (vref: usually the body's velocity)
	Vector FlybyOut (const Vector &vin, double mu, double rp, double beta, const Vector &vref);

} // namespace transfer

#endif // !__CONICS_H
