// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Rotating black holes: the Kerr spacetime in Kerr-Schild coordinates.
//
// Kerr-Schild coordinates (t, x, y, z) are Cartesian far from the hole and
// regular across the horizon: the metric is flat space plus a null term,
// g = eta + f l l, with f = 2 M r^3 / (r^4 + a^2 z^2) and the null covector
// l = (1, (r x + a y) / (r^2 + a^2), (r y - a x) / (r^2 + a^2), z / r)
// (signature -+++), r the spheroidal radius: (x^2 + y^2) / (r^2 + a^2) +
// z^2 / r^2 = 1. These are the ingoing coordinates: t = t_BL + int 2Mr/Delta
// dr, phi = phi_BL + int a/Delta dr (Boyer-Lindquist t, phi). The spin is
// along +z. Units: geometric (G = c = 1), any unit of length (M and a in
// it): Hole::FromSI gives metres.
//
// Free motion (light and test particles) follows geodesics, integrated in
// Hamiltonian form, H = g^mn p_m p_n / 2, in the covariant momentum p_m
// (p_t = -E and the axial p_phi = x p_y - y p_x conserved, and H: -1/2 per
// unit mass for particles with the affine parameter their proper time, 0
// for light). Depends only on the C++ library (unit tests).
// =======================================================================

#ifndef __KERR_H
#define __KERR_H

#include <functional>

namespace kerr {

	/// A geodesic's state: position x^m (t, x, y, z), covariant momentum p_m
	struct State {
		double x[4] = {0, 0, 0, 0};
		double p[4] = {0, 0, 0, 0};
	};

	class Hole {
	public:
		double M = 1.0; ///< mass [length]: G m / c^2
		double a = 0.0; ///< spin [length]: J / (m c), |a| <= M; along +z

		Hole () = default;
		Hole (double M, double a): M(M), a(a) {}
		/// A hole of mass m [kg] and dimensionless spin chi = a / M, in metres
		static Hole FromSI (double m, double chi);

		// ---------------------------------------------------------------
		// Radii (spheroidal r; equatorial for orbits)

		/// spheroidal radius r of a point (x, y, z)
		double R (const double *x3) const;
		double Horizon () const;      ///< outer event horizon r+
		double InnerHorizon () const; ///< inner (Cauchy) horizon r-
		/// outer boundary of the ergosphere at polar angle theta (cos theta)
		double Ergosphere (double cosTheta) const;
		/// innermost stable circular orbit, equatorial, prograde (with the
		/// spin) or retrograde (Bardeen, Press & Teukolsky 1972)
		double ISCO (bool prograde = true) const;
		/// circular photon orbit in the equatorial plane
		double PhotonOrbit (bool prograde = true) const;
		/// impact parameter L/E of that orbit (signed: > 0 with the spin):
		/// equatorial light passing with less (prograde) is captured
		double PhotonImpact (bool prograde = true) const;
		/// The flux from a thin disc's face (Novikov-Thorne, Page & Thorne
		/// 1974) at r (equatorial, prograde, the ISCO its inner edge), in the
		/// disc's frame: F = 3 Mdot / (8 pi M^2) x DiscFluxShape (r); 0
		/// within the ISCO
		double DiscFluxShape (double r) const;
		/// equatorial circular orbit at r: energy and axial angular momentum
		/// per unit mass, angular velocity dphi/dt (false if none: within
		/// the photon orbit)
		bool CircularOrbit (double r, bool prograde, double &E, double &L, double &Omega) const;

		// ---------------------------------------------------------------
		// The metric

		/// metric g_mn at a point (x, y, z)
		void Metric (const double *x3, double g[4][4]) const;
		/// inverse metric g^mn
		void InverseMetric (const double *x3, double g[4][4]) const;
		/// f and the null covector l_m (l^m = (-1, l1, l2, l3))
		void NullForm (const double *x3, double &f, double l[4]) const;
		/// g^mn p_m p_n (-mu^2 for a particle of mass mu, 0 for light)
		double Norm (const double *x3, const double *p) const;
		/// raise / lower an index at a point
		void Raise (const double *x3, const double *p, double *v) const;
		void Lower (const double *x3, const double *v, double *p) const;
		/// g_mn v^m w^n
		double Dot (const double *x3, const double *v, const double *w) const;

		// ---------------------------------------------------------------
		// Geodesics

		/// d(x, p)/dlambda (Hamilton's equations); y = (x[4], p[4])
		void Derivs (const double *y, double *dy) const;
		/// energy, axial angular momentum, Carter constant (Q = 0 for
		/// equatorial motion; mu^2 from the norm)
		double Energy (const State &s) const { return -s.p[0]; }
		double AxialMomentum (const State &s) const { return s.x[1] * s.p[2] - s.x[2] * s.p[1]; }
		double Carter (const State &s) const;

		/// a state from Boyer-Lindquist coordinates: position (r, theta,
		/// phi) at t = 0, covariant momentum (p_t, p_r, p_theta, p_phi)
		State FromBoyerLindquist (double r, double theta, double phi, const double *pBL) const;

		// ---------------------------------------------------------------
		// Observers

		/// orthonormal frame of an observer of 4-velocity u (contravariant,
		/// normalised) at a point: e[0] = u, e[1..3] from the coordinate
		/// axes x, y, z (Gram-Schmidt); contravariant e[a][m]
		void Tetrad (const double *x3, const double *u, double e[4][4]) const;
		/// 4-velocity of the observer at rest in the coordinates (along d/dt;
		/// false within the ergosphere: none there)
		bool StaticObserver (const double *x3, double *u) const;
		/// 4-velocity of the observer normal to the slices t = const
		/// (u_m = -N dt, N = 1 / sqrt (1 + f)): defined everywhere, falls
		/// in across the horizon, at rest far away
		void NormalObserver (const double *x3, double *u) const;
		/// 4-velocity of a particle moving at coordinate velocity v
		/// (dx^i/dt), false if not timelike
		bool CoordinateVelocity (const double *x3, const double *v3, double *u) const;
		/// frame of an observer of 4-velocity u, boosted (pure boost) from
		/// the normal observer's frame (its axes the coordinate axes,
		/// Gram-Schmidt): far from the hole, the coordinate axes carried
		/// over by a pure boost (as Relativity.h's proper accelerations)
		void BoostFrame (const double *x3, const double *u, double e[4][4]) const;
		/// Christoffel symbols G[a][b][c] = Gamma^a_bc at a point (numerical
		/// derivatives of the metric)
		void Christoffel (const double *x3, double G[4][4][4]) const;
		/// tidal tensor in the frame e (e[0] the observer's 4-velocity,
		/// e[1..3] its axes): the acceleration of a free particle at
		/// separation xi (on the axes) relative to the observer is T xi;
		/// T_ij = -R_{i0j0} [1/length^2]. Outside the horizon of a hole not
		/// turning, at rest: 2M/r^3 along r, -M/r^3 across.
		void Tidal (const double *x3, const double e[4][4], double T[3][3]) const;
		/// frequency of light of momentum p seen by an observer of 4-velocity
		/// u: -p_m u^m (the ratio of two observers': the shift between them)
		static double Frequency (const double *p, const double *u) { return -(p[0] * u[0] + p[1] * u[1] + p[2] * u[2] + p[3] * u[3]); }
	};

	// ===================================================================
	// Integration: adaptive Runge-Kutta (Dormand-Prince 5(4))

	struct Integrator {
		double tol = 1e-10;   ///< error per step, relative to the scale of each component
		double hmax = 0.0;    ///< largest step (0: none)
		double hmin = 1e-12;  ///< smallest (relative to r): stops below
		/// step limit as a fraction of r (keeps steps from jumping past the hole)
		double rFrac = 0.1;
		int maxSteps = 1000000;
		/// stop when this returns true (checked after each step; Fly: the
		/// step shortened to end just past where it turns true)
		std::function<bool (const State &)> stop;
		int steps = 0;        ///< taken by the last call
	};

	enum Result { GEO_DONE, GEO_STOPPED, GEO_STEPS, GEO_STUCK };

	/// Integrate a geodesic over affine parameter dlambda (sign: direction);
	/// h: the step to try first (updated; 0: from r)
	Result Integrate (const Hole &hole, State &s, double dlambda, Integrator &opt, double &h);

	/// A particle's proper acceleration (felt on board) at coordinate
	/// time t, position x (x, y, z), 4-velocity u: its components alpha on
	/// the axes of the boost frame (BoostFrame)
	typedef std::function<void (double t, const double *x3, const double *u, double *alpha)> Thrust;

	/// Fly a particle (p: its covariant 4-velocity u_m, norm -1) over
	/// coordinate time dt under a proper acceleration (thrust: may be
	/// null): proper time elapsed in dtau. The step h is in coordinate
	/// time (updated; 0: from r). Stops (GEO_STOPPED) when opt.stop says.
	Result Fly (const Hole &hole, State &s, double dt, Integrator &opt, double &h, const Thrust &thrust, double &dtau);

} // namespace kerr

#endif // !__KERR_H
