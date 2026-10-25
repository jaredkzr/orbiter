// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Manoeuvre nodes: planned impulsive velocity changes of a vessel, given
// in the orbital frame of the reference body at the node (prograde, orbit
// normal, radial outward), and the burn that executes them.
// =======================================================================

#ifndef __MANEUVER_H
#define __MANEUVER_H

#include "Vecmat.h"

namespace maneuver {

	struct Node {
		double mjd;            ///< time of the (impulsive) velocity change
		Vector dv;             ///< [m/s] x: prograde, y: orbit normal, z: radial outward
		bool autoburn = false; ///< flown by the burn autopilot
	};

	/// Unit vectors of the orbital frame for position r and velocity v
	/// relative to the reference body (global frame orientation). The
	/// normal is that of Orbiter's normal navigation mode (crossp (v, r) in
	/// Orbiter's left-handed frame); radial is outward, orthogonal to prograde.
	void Frame (const Vector &r, const Vector &v, Vector &pro, Vector &nml, Vector &rad);

	/// Velocity change of a node as a global vector
	Vector ToGlobal (const Vector &dv, const Vector &r, const Vector &v);

	/// Global velocity change in the node frame
	Vector FromGlobal (const Vector &dvg, const Vector &r, const Vector &v);

	/// Burn duration [s] for a velocity change dv [m/s] with thrust F [N],
	/// initial mass m [kg] and exhaust velocity ve [m/s] (rocket equation)
	double BurnTime (double dv, double F, double m, double ve);

	/// Two-body (Kepler) propagation of position r0 and velocity v0 relative
	/// to a body with gravitational parameter mu by dt (any sign; any
	/// conic), with universal variables. Returns false if it did not converge.
	bool Kepler (const Vector &r0, const Vector &v0, double mu, double dt, Vector &r, Vector &v);

} // namespace maneuver

#endif // !__MANEUVER_H
