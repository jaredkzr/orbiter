// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Gravity-assist routes (patched conics): a departure, flybys of bodies
// with the same central body, and the arrival at a target. Two models:
//
// - MGA: the legs are Lambert conics between the bodies (with full
//   revolutions if allowed); a flyby whose excess velocities don't match
//   (speed, or a turn the body can't give above its least periapsis) is
//   powered: a burn at the periapsis. Variables: the departure date and
//   the time of each leg.
// - MGA-1DSM: each leg has one deep-space manoeuvre; flybys are unpowered.
//   Variables: departure date, departure excess velocity (speed and two
//   angles), then per leg the manoeuvre's time (fraction of the leg) and
//   the leg's time, and per flyby its periapsis and B-plane angle.
//
// The optimiser is a self-adaptive differential evolution with restarts,
// each followed by monotonic basin hopping (Nelder-Mead local searches). Depends only on the vector maths, Conics
// and the Kepler propagation (unit tests).
// =======================================================================

#ifndef __ROUTES_H
#define __ROUTES_H

#include "Vecmat.h"
#include <atomic>
#include <functional>
#include <vector>

namespace transfer {
namespace route {

	/// A body of the route: departure, flyby or target
	struct Body {
		double mu = 0.0;
		double radius = 0.0;
		double rpMin = 0.0;  ///< least flyby periapsis radius [m]
		/// > 0: a powered flyby's periapsis below rpMin costs this much per
		/// metre (the penalty of benchmarks) instead of a burn for the rest of the turn
		double rpSoft = 0.0;
		/// state relative to the central body at a time [MJD] (global orientation; m, m/s)
		std::function<bool (double mjd, Vector &r, Vector &v)> state;
	};

	enum Departure {
		DEP_VINF,  ///< cost: the departure excess speed (benchmarks)
		DEP_PARK,  ///< cost: the ejection from a circular parking orbit about the departure body
		DEP_ORBIT  ///< from the vessel's own orbit about the central body: cost the burn
	};
	enum Arrival {
		ARR_VINF,    ///< cost: the arrival excess speed (rendezvous)
		ARR_CAPTURE, ///< cost: capture into an orbit about the target
		ARR_NONE     ///< no cost (flyby, aerocapture)
	};

	struct Problem {
		double mu = 0.0;          ///< central body
		Vector nref;              ///< prograde sense about the central body
		int dep = DEP_PARK;
		Body depBody;             ///< DEP_VINF, DEP_PARK
		double parkR = 0.0;       ///< DEP_PARK: parking orbit radius
		Vector r0, v0;            ///< DEP_ORBIT: the vessel relative to the central body at mjd0
		double mjd0 = 0.0;
		std::vector<Body> enc;    ///< the flyby bodies, then the target
		int arr = ARR_CAPTURE;
		double capRp = 0.0, capRa = 0.0; ///< ARR_CAPTURE: orbit about the target (periapsis, apoapsis radius)
		double t0lo = 0.0, t0hi = 0.0;   ///< departure window [MJD]
		std::vector<double> tlo, thi;    ///< bounds of each leg's time [days]
		double maxTof = 0.0;      ///< longest flight [days] (0: no limit)
		int maxRevs = 0;          ///< full revolutions per leg
		bool dsm = false;         ///< MGA-1DSM
		double vinfMax = 6000.0;  ///< 1DSM: largest departure excess speed (DEP_ORBIT: burn) [m/s]
		double rpMaxFactor = 30.0;///< 1DSM: largest flyby periapsis (multiple of rpMin)
		/// bodies the legs are to keep clear of between the encounters (radius:
		/// the distance to keep): passes the patched conics don't see, which
		/// the full-gravity flight can't be targeted through
		std::vector<Body> avoid;
		int Legs () const { return (int)enc.size (); }
	};

	struct Leg {
		double t0 = 0.0, t1 = 0.0;   ///< [MJD]
		Vector r0, v0;               ///< the conic after the departure or flyby (relative to the central body)
		double dsmMjd = 0.0;         ///< deep-space manoeuvre (0: none)
		Vector dsmR, dsmDv;          ///< its position, velocity change (global orientation)
		Vector r1, v1;               ///< at the encounter
		int revs = 0;                ///< full revolutions of the Lambert conic
		int nearBody = -1;           ///< closest pass to a body of Problem::avoid (-1: none checked)
		double nearDist = 0.0;       ///< its distance as a fraction of the body's radius to keep
		double nearMjd = 0.0;
	};
	struct Encounter {
		double mjd = 0.0;
		Vector vinfIn, vinfOut;      ///< excess velocities (global orientation; out: flybys only)
		double rp = 0.0;             ///< flyby periapsis radius [m]
		double turn = 0.0;           ///< [rad]
		double dvPeri = 0.0;         ///< burn at the periapsis (powered flyby) [m/s]
		double dvTurn = 0.0;         ///< burn for the turn below the least periapsis [m/s]
		double beta = 0.0;           ///< B-plane angle (1DSM)
	};
	struct Result {
		bool valid = false;
		double dv = 1e30;            ///< the objective [m/s] (with penalties)
		double dvDep = 0.0, dvArr = 0.0, dvFlyby = 0.0, dvDsm = 0.0; ///< [m/s]
		Vector vinfDep;              ///< departure excess velocity (DEP_ORBIT: the burn)
		double dep = 0.0, tof = 0.0; ///< [MJD], [days]
		std::vector<Leg> legs;
		std::vector<Encounter> enc;
		std::vector<double> x;       ///< decision vector
	};

	int Dimension (const Problem &p);
	void Bounds (const Problem &p, std::vector<double> &lo, std::vector<double> &hi);
	/// The objective for a decision vector (and the route if res)
	double Evaluate (const Problem &p, const double *x, Result *res = nullptr);
	/// The legs' closest passes to the bodies of p.avoid (Leg::nearBody...),
	/// sampled finely; returns the least fraction of the distance to keep
	double CheckPasses (const Problem &p, Result &r, int samples = 400);
	/// A 1DSM decision vector close to an MGA route of the same bodies
	bool ToDsm (const Problem &dsm, const Result &mga, std::vector<double> &x);

	struct Options {
		unsigned int seed = 1;
		long evals = 200000;            ///< evaluation budget
		const std::atomic<bool> *abort = nullptr;
		std::function<void (float)> progress;
		std::vector<std::vector<double>> seeds; ///< starting points (decision vectors)
	};
	/// Global optimisation of the route
	Result Optimise (const Problem &p, const Options &o);

	/// Nelder-Mead from x (in the problem's bounds); returns the objective
	double Polish (const Problem &p, std::vector<double> &x, int evals, const std::atomic<bool> *abort = nullptr);

} // namespace route
} // namespace transfer

#endif // !__ROUTES_H
