// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Special relativity for relativistic flight: Lorentz factor, rapidity,
// velocity composition and boosts, aberration and Doppler shift of light,
// motion under proper acceleration, and trips to moving targets by burns
// of constant proper acceleration (the ideal drive of the Relativistic
// Travel MFD). Depends only on the vector maths (unit tests).
//
// Conventions: SI units. Coordinate time t and positions are those of the
// frame the states are given in (Orbiter's global frame: the Solar
// System's). The proper velocity u = gamma v (dx/dtau) keeps states
// regular at any speed. A proper acceleration is the acceleration felt
// on board: a vector in the ship's rest frame, whose axes are those of the
// coordinate frame carried over by a pure boost.
// =======================================================================

#ifndef __RELATIVITY_H
#define __RELATIVITY_H

#include "Vecmat.h"
#include <functional>
#include <vector>

namespace relativity {

	const double C = 299792458.0; ///< speed of light [m/s]
	const double G0 = 9.80665;    ///< standard gravity [m/s^2]
	const double LY = C * 365.25 * 86400.0; ///< light year (Julian) [m]
	const double PC = 3.0856775814913673e16; ///< parsec [m]

	// ===================================================================
	// Speeds

	/// Lorentz factor of speed v [m/s]
	double Gamma (double v);
	/// Lorentz factor of proper velocity u = gamma v
	double GammaOfU (const Vector &u);
	/// gamma - 1 of proper velocity u, accurate at low speed
	double GammaMinus1OfU (const Vector &u);
	/// velocity of proper velocity u
	Vector VelocityOfU (const Vector &u);
	/// proper velocity of velocity v (|v| < c)
	Vector UOfVelocity (const Vector &v);
	/// rapidity of speed v: atanh (v/c)
	double Rapidity (double v);
	/// speed of rapidity eta
	double SpeedOfRapidity (double eta);

	/// Velocity composition: the velocity, in frame S, of an object moving
	/// at w in a frame S' that moves at v relative to S (S' axes those of S
	/// carried over by a pure boost)
	Vector AddVelocity (const Vector &v, const Vector &w);
	/// Lorentz boost of an event (t, x) of frame S into the frame moving at
	/// velocity v relative to S (pure boost, same origin at t = 0)
	void Boost (const Vector &v, double &t, Vector &x);

	// ===================================================================
	// Light

	/// Aberration: a source seen in direction n (unit vector towards it)
	/// by an observer at rest in S is seen in this direction by an observer
	/// moving at v (axes carried over by a pure boost)
	Vector Aberrate (const Vector &n, const Vector &v);
	/// Doppler factor: the frequency seen by an observer moving at v over
	/// that emitted by a source at rest in S, seen in direction n from S
	/// (> 1: blueshift). Specific intensity scales by D^3 (I_nu / nu^3 is
	/// invariant), a blackbody of temperature T is seen at D T.
	double Doppler (const Vector &n, const Vector &v);
	/// Light-time: where a body at p moving at v (S's positions and
	/// velocities, now) was when it emitted the light now reaching obs: in
	/// a circle about its parent (at pp, moving at vp; null: none, the body
	/// moving straight), the parent straight; returns the light's travel
	/// time [s], pe the emission's place
	double EmissionPos (const Vector &p, const Vector &v, const Vector *pp, const Vector *vp, const Vector &obs, Vector &pe);

	// ===================================================================
	// Gravity and clocks (weak fields: first post-Newtonian order)

	/// Coordinate acceleration dv/dt of a test particle at velocity v
	/// (relative to the sources, taken at rest) in a Newtonian field g of
	/// potential phi (< 0): g (1 + v^2/c^2 + 4 phi/c^2) - 4 (v.g) v / c^2
	/// (PPN beta = gamma = 1). Deflects light twice as much as Newton, a
	/// particle of speed v by 2GM/(b v^2) (1 + v^2/c^2).
	Vector PNAccel (const Vector &g, double phi, const Vector &v);
	/// Coordinate acceleration dv/dt at velocity v for proper acceleration
	/// alpha (along v: alpha/gamma^3; across: alpha/gamma^2)
	Vector CoordAccel (const Vector &alpha, const Vector &v);
	/// Proper acceleration giving coordinate acceleration a at velocity v
	/// (the inverse of CoordAccel: along v gamma^3 a, across gamma^2 a)
	Vector ProperOfCoordAccel (const Vector &a, const Vector &v);
	/// Rate du/dt of the proper velocity u under coordinate acceleration a
	/// (dv/dt: gravity) and proper acceleration alpha
	Vector URate (const Vector &u, const Vector &a, const Vector &alpha);
	/// Rate of a clock moving at speed^2 v2 in potential phi, less one:
	/// dtau/dt - 1 = sqrt (1 + 2 phi/c^2 - v2/c^2) - 1 (accurate when small)
	double ClockRateMinus1 (double v2, double phi);

	// ===================================================================
	// Motion

	/// A state: coordinate and proper time, position, proper velocity
	struct State {
		double t = 0.0;    ///< coordinate time [s]
		double tau = 0.0;  ///< proper time [s]
		Vector x;          ///< position [m]
		Vector u;          ///< proper velocity gamma v [m/s]
		Vector V () const { return VelocityOfU (u); }
		double Gamma () const { return GammaOfU (u); }
	};

	/// External forces in coordinate terms: d(gamma v)/dt (force per rest
	/// mass) at coordinate time t, position x, velocity v (gravity, R1)
	typedef std::function<Vector (double t, const Vector &x, const Vector &v)> Field;

	/// Coordinate rate du/dtau of the proper velocity for proper
	/// acceleration alpha (rest frame): alpha boosted to the coordinate frame
	Vector BoostedAcceleration (const Vector &alpha, const Vector &u);

	/// Advance a state by proper time dtau with constant proper
	/// acceleration alpha (and an external field), Runge-Kutta 4 in steps
	/// of at most a rapidity change of 0.01 (steps > 0: that many)
	void Propagate (State &s, const Vector &alpha, double dtau, const Field *field = nullptr, int steps = 0);

	/// Straight-line motion from rest under constant proper acceleration a
	/// for proper time tau: coordinate time, distance, speed (hyperbolic
	/// motion, exact)
	void Hyperbolic (double a, double tau, double &t, double &x, double &v);

	/// Flip-and-burn between two points at rest a distance d apart with
	/// proper acceleration a (accelerate to halfway, decelerate the rest):
	/// proper time, coordinate time, top speed
	void FlipAndBurn (double d, double a, double &tau, double &t, double &vmax);

	/// Accelerate, coast, decelerate between two points at rest a distance
	/// d apart, arriving after coordinate time t: the rapidity of the
	/// coast (0 if not possible: t shorter than the flip-and-burn's) and
	/// each burn's proper time
	double CoastRapidity (double d, double a, double t, double &tauBurn);

	// ===================================================================
	// Trips by burns of constant proper acceleration, no gravity

	enum TripMode {
		TRIP_RENDEZVOUS, ///< arrive at the target's position and velocity: two burns
		TRIP_FLYBY       ///< arrive at the target's position at any velocity: one burn
	};

	/// The state to arrive at, at coordinate time t (position, velocity)
	typedef std::function<void (double t, Vector &x, Vector &v)> TargetFunc;

	struct TripSpec {
		State start;
		double accel = G0;     ///< proper acceleration of the burns [m/s^2]
		int mode = TRIP_RENDEZVOUS;
		double tArrive = 0.0;  ///< coordinate time of arrival (0: as soon as possible; else the burns are shorter, with a coast between)
		TargetFunc target;
		const struct Trip *warm = nullptr; ///< (optional) an earlier solution of the trip, as the first guess
		double warmElapsed = 0.0;          ///< proper time from that solution's start to this start [s]
	};

	struct Burn {
		double tau0 = 0.0;   ///< start (proper time from the trip's start) [s]
		double dtau = 0.0;   ///< length (proper time) [s]
		Vector dir;          ///< direction of thrust (unit, rest frame)
	};

	struct Trip {
		bool ok = false;
		std::vector<Burn> burns;    ///< 2 (rendezvous) or 1 (flyby)
		double coastDt = 0.0;       ///< coordinate time of the coast between the burns (after the burn of a flyby)
		double tArrive = 0.0;       ///< coordinate time of arrival [s]
		double tauArrive = 0.0;     ///< proper time of the trip [s]
		double vmax = 0.0;          ///< top speed [m/s]
		double properDv = 0.0;      ///< the burns' total: accel x burn time [m/s] (rocket cost)
		double missX = 0.0, missV = 0.0; ///< residual at arrival [m], [m/s]
		int iterations = 0;
	};

	/// Solve a trip (Newton iterations from a guess by straight-line
	/// trips). Not ok if it doesn't converge or tArrive is too soon.
	Trip SolveTrip (const TripSpec &spec);
	/// The state on a trip at proper time tau from its start (clamped to
	/// the trip; coasting after the arrival)
	State TripState (const TripSpec &spec, const Trip &trip, double tau);

	// ===================================================================
	// Rocket cost

	struct RocketCost {
		double massRatio = 1.0; ///< initial over final mass
		double propellant = 0.0;///< [kg]
		double energy = 0.0;    ///< energy given to the exhaust [J]: (m0 - m1) c^2 (1 - sqrt (1 - w^2/c^2)), all of the propellant's rest energy for a photon rocket
	};
	/// The rocket that makes a trip of proper velocity change properDv
	/// (accel x burn time) with exhaust speed w (C: a photon rocket), dry
	/// mass m1: relativistic rocket equation, m0/m1 = exp (properDv / w)
	RocketCost Rocket (double properDv, double w, double m1);

} // namespace relativity

#endif // !__RELATIVITY_H
