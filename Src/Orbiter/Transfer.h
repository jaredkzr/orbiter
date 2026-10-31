// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Transfer planning: transfers of a vessel from its orbit about one body
// to another body (directly, or by gravity assists: see Routes.h), either a body with the same parent (Earth to Mars,
// Ganymede to Callisto: an ejection hyperbola from the parking orbit, then
// a conic about the parent) or a moon of the vessel's reference body
// (Earth to the Moon: a conic from the parking orbit).
//
// The search is patched-conic: Lambert's problem over departure dates and
// times of flight (porkchop plot, launch windows over the next years), the
// ejection burn on the parking orbit from the departure asymptote. The
// burns are then refined with the numerical trajectory prediction
// (trajectory::Sandbox: full gravity) until the trajectory passes the
// target at the chosen periapsis (B-plane targeting), and become manoeuvre
// nodes: ejection, mid-course corrections (refined just before they are
// due), capture at the arrival periapsis.
//
// Routes with flybys (the advanced mode) are optimised with the patched
// conics of Routes.h, then targeted in full gravity encounter by encounter:
// the ejection (or the first deep-space manoeuvre) at the first flyby's
// B-plane point, a correction after each flyby (or that leg's deep-space
// manoeuvre) at the next one, powered flybys as nodes at the periapsis.
//
// The computations run on a worker thread; Planner::Update applies their
// results on the simulation thread.
// =======================================================================

#ifndef __TRANSFER_H
#define __TRANSFER_H

#include "Vecmat.h"
#include "Maneuver.h"
#include "Conics.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CelestialBody;
class Vessel;
class Elements;
namespace trajectory { class Sandbox; struct Prediction; }

namespace transfer {

	// ===================================================================
	// Settings and results

	enum Kind {
		KIND_NONE,
		KIND_SIBLING, ///< to a body with the same parent: ejection, then a conic about the parent
		KIND_MOON     ///< to a moon of the vessel's reference body
	};
	enum Objective {
		OBJ_TOTAL,    ///< least ejection + capture velocity change
		OBJ_EJECTION  ///< least ejection velocity change (flyby, aerocapture)
	};
	enum ArrivalPlane {
		ARR_EQUATORIAL, ///< arrival in the target's equatorial plane, prograde
		ARR_FREE        ///< as the trajectory arrives (only the periapsis altitude is targeted)
	};
	enum Role {
		ROLE_EJECTION, ROLE_MCC, ROLE_CAPTURE,
		ROLE_FLYBY,  ///< powered flyby: a burn at the flyby's periapsis
		ROLE_DSM     ///< deep-space manoeuvre (targets the next encounter)
	};

	struct Settings {
		const CelestialBody *target = nullptr;
		int objective = OBJ_TOTAL;
		int arrival = ARR_EQUATORIAL;
		double capAlt = 0.0;    ///< periapsis altitude at the target [m] (0: default for the target)
		double capApo = 0.0;    ///< apoapsis altitude of the capture orbit [m] (<= capAlt: circular)
		double mcc[2] = { 0.1, 0.8 }; ///< mid-course corrections at these fractions of the flight
		bool useMcc[2] = { true, true };
		bool autoCorrect = true;      ///< refine each correction shortly before it is due
		// launch from the surface
		double parkAlt = 0.0;         ///< parking orbit altitude [m] (0: default for the body)
		double ascentTime = 600.0;    ///< lift-off to orbit insertion [s]
		double minCoast = 1200.0;     ///< least coast in the parking orbit before the ejection [s]
		// gravity assists (the advanced mode)
		bool assists = false;         ///< routes with flybys (else direct transfers only)
		std::vector<const CelestialBody*> flybys; ///< the route's flyby bodies, in order
		std::vector<double> flybyAlt; ///< least periapsis altitude of each flyby [m] (0: default)
		bool dsm = false;             ///< one deep-space manoeuvre per leg (else powered flybys)
		int maxRevs = 0;              ///< full revolutions per leg (more choice; but a leg of many months can pass a planet again)
		double maxTof = 0.0;          ///< longest flight [days] (0: no limit)
		double routeSpan = 0.0;       ///< departures within this time from now [days] (0: from the bodies' periods)
		bool UsesRoute () const { return assists && !flybys.empty(); }
		double FlybyAlt (size_t i) const { return i < flybyAlt.size() ? flybyAlt[i] : 0.0; }
	};

	/// A flyby of a route: planned (patched conics), or as predicted
	struct RouteFlyby {
		const CelestialBody *body = nullptr;
		double mjd = 0.0;             ///< periapsis [MJD]
		double alt = 0.0;             ///< periapsis altitude [m]
		double minAlt = 0.0;          ///< least altitude allowed [m]
		double turn = 0.0;            ///< turn of the excess velocity [rad]
		double vinfIn = 0.0, vinfOut = 0.0; ///< excess speeds [m/s]
		double dv = 0.0;              ///< powered flyby: burn at the periapsis [m/s]
		double dvTurn = 0.0;          ///< the rest of a turn the body can't give (a burn far out) [m/s]
		Vector vinfInVec, vinfOutVec; ///< (global orientation)
	};
	struct RouteLeg {
		double t0 = 0.0, t1 = 0.0;    ///< [MJD]
		int revs = 0;                 ///< full revolutions about the central body
		Vector r0, v0;                ///< the conic at t0, relative to the central body (global orientation)
		double dsmMjd = 0.0;          ///< deep-space manoeuvre (0: none)
		double dsmDv = 0.0;           ///< [m/s]
		Vector dsmDvVec, dsmPos;      ///< (global orientation; relative to the central body)
		const CelestialBody *near = nullptr; ///< a body passed close by between the encounters (null: none)
		double nearDist = 0.0;        ///< its distance [m]
		double nearMjd = 0.0;
	};
	/// A route with flybys (patched conics)
	struct Route {
		bool valid = false;
		bool dsm = false;
		std::vector<const CelestialBody*> bodies; ///< the flyby bodies, then the target
		double dep = 0.0, tof = 0.0;  ///< [MJD], [days]
		double dvTotal = 0.0;         ///< [m/s] as counted by the objective
		double dvEject = 0.0, dvFlyby = 0.0, dvDsm = 0.0, dvCapture = 0.0; ///< [m/s]
		double vinfDep = 0.0, vinfArr = 0.0;
		std::vector<RouteFlyby> flybys;
		std::vector<RouteLeg> legs;
		std::vector<double> x;        ///< the optimiser's variables
	};
	/// A route found by Suggest
	struct Suggestion {
		std::vector<const CelestialBody*> flybys;
		double dv = 0.0;              ///< [m/s]
		double dep = 0.0, tof = 0.0;  ///< [MJD], [days]
		bool dsm = false;
		std::vector<double> x;        ///< the optimiser's variables (a start for the route's search)
	};

	/// A launch from the surface into a parking orbit for a transfer
	struct LaunchOpportunity {
		double launchMjd = 0.0;       ///< lift-off
		double azimuth = 0.0;         ///< launch heading from north, clockwise [rad]
		double incl = 0.0;            ///< parking orbit inclination to the equator [rad]
		double lan = 0.0;             ///< its ascending node (equatorial frame, from the body's prime meridian at lift-off) [rad]
		double insMjd = 0.0;          ///< orbit insertion
		Vector rIns, vIns;            ///< insertion state relative to the departure body (global orientation)
		double burnMjd = 0.0;         ///< ejection
		Vector burnDv;                ///< node frame [m/s]
		Vector burnPos;               ///< relative to the departure body
		double coast = 0.0;           ///< insertion to ejection [s]
		double dvEject = 0.0;         ///< [m/s]
		double dvPenalty = 0.0;       ///< ascent velocity change above a launch due east [m/s]
		double dvCapture = 0.0, dvTotal = 0.0; ///< [m/s] (total: penalty + ejection + capture by the objective)
		double tof = 0.0;             ///< [days] (the arrival of the selected transfer kept)
	};
	struct LaunchPlan {
		bool landed = false;          ///< the vessel is on the surface (else: no launch)
		double lat = 0.0, lng = 0.0;  ///< launch site [rad]
		double parkAlt = 0.0;         ///< parking orbit altitude used [m]
		double vAscent = 0.0;         ///< ideal ascent velocity change (orbital speed - rotation, launch due east) [m/s]
		std::vector<LaunchOpportunity> opp; ///< by lift-off time
		int sel = -1;                 ///< the chosen one
	};

	/// One transfer (patched conics)
	struct Solution {
		bool valid = false;
		double dep = 0.0, tof = 0.0;      ///< departure [MJD], time of flight [days]
		double dvEject = 0.0;             ///< [m/s] ejection from the parking orbit (coplanar estimate in searches)
		double dvCapture = 0.0;           ///< [m/s] capture into the target orbit
		double dvTotal = 0.0;             ///< [m/s] as counted by the objective
		double c3 = 0.0;                  ///< [m^2/s^2] departure energy (KIND_SIBLING)
		double vinfDep = 0.0, vinfArr = 0.0; ///< [m/s] hyperbolic excess speeds
		double decl = 0.0;                ///< [rad] departure asymptote out of the parking orbit plane
		Vector r1, v1, r2, v2;            ///< transfer conic ends, relative to its central body (global orientation)
		double mu = 0.0;                  ///< of the central body of the transfer conic
		Vector vinfDepVec, vinfArrVec;    ///< (global orientation)
		// the ejection burn on the parking orbit
		bool burnValid = false;
		double burnMjd = 0.0;
		Vector burnDv;                    ///< node frame (prograde, normal, radial) [m/s]
		Vector burnPos;                   ///< position relative to the departure body
		double burnDvMag = 0.0;           ///< [m/s] (with the plane change it needs)
	};

	/// Porkchop plot: the objective over departure dates and times of flight
	struct Grid {
		int nx = 0, ny = 0;
		double dep0 = 0, dep1 = 0;        ///< [MJD]
		double tof0 = 0, tof1 = 0;        ///< [days]
		std::vector<float> total, eject, capture; ///< [m/s] (j*nx+i; NaN: no solution)
		float zmin = 0.0f;
	};

	/// Launch windows: the least objective for each departure date
	struct Curve {
		double dep0 = 0, dep1 = 0;
		std::vector<float> dv, tof;       ///< per sample [m/s], [days]
		std::vector<double> windows;      ///< departure dates of the local minima (best first)
	};

	/// The plan: the burns of the transfer and the predicted arrival. Ejection
	/// and capture are manoeuvre nodes of the vessel; a correction becomes a
	/// node when it is computed, shortly before it is due (if it is needed).
	struct PlanNode {
		int role;
		double mjd;        ///< time [MJD]
		Vector dv;         ///< node frame [m/s]
		bool isNode;       ///< is a manoeuvre node of the vessel (corrections: once computed)
		bool done;         ///< computed (corrections), or flown (passed)
		int aim = -1;      ///< routes: the encounter it targets (index into the route's bodies)
	};
	struct Plan {
		bool valid = false;
		std::vector<PlanNode> nodes;
		double arrMjd = 0.0;              ///< predicted periapsis at the target [MJD]
		double periAlt = 0.0;             ///< predicted periapsis altitude [m]
		double periTarget = 0.0;          ///< targeted periapsis altitude [m]
		double incl = 0.0;                ///< predicted inclination at arrival (equatorial) [rad]
		double inclMin = 0.0;             ///< least inclination the arrival asymptote allows [rad]
		double bt = 0, br = 0;            ///< predicted B-plane point [m]
		double btT = 0, brT = 0;          ///< targeted B-plane point [m]
		bool arrives = false;             ///< the prediction enters the target's sphere of influence
		double missDist = 0.0;            ///< closest approach to the target's centre otherwise [m]
		bool armed = false;
		bool ejRetargeted = false;        ///< the ejection was targeted again shortly before it
		// routes
		std::vector<RouteFlyby> flybys;   ///< the flybys as predicted (periapsis time, altitude, burns)
		std::vector<double> flybyTarget;  ///< targeted periapsis altitude of each [m]
		std::vector<const CelestialBody*> route; ///< the route's bodies (flybys, then the target; empty: direct)
	};

	struct Status {
		enum Job { IDLE, SEARCH, REFINE, CORRECT, SUGGEST } job = IDLE;
		float progress = 0.0f;            ///< 0..1
		std::string message;              ///< last result or error
		unsigned int serial = 0;          ///< incremented with each new result
	};

	// ===================================================================

	struct Bodies; // (snapshot of the bodies involved, for the worker)

	class Planner {
	public:
		explicit Planner (Vessel *v);
		~Planner ();

		Vessel *GetVessel () const { return vessel; }
		Settings settings; // (simulation thread)

		/// Bodies the vessel can transfer to from its current reference body
		std::vector<const CelestialBody*> Targets () const;
		Kind KindOf (const CelestialBody *target) const;
		/// Default periapsis altitude at a target [m]
		static double DefaultCaptureAlt (const CelestialBody *target);

		/// Start a search: the launch windows over the next years, and the
		/// porkchop plot of the first good window. false if no transfer is possible.
		/// With a route (Settings::UsesRoute): the optimum of the route, and
		/// the best velocity change by departure date near it.
		bool Search ();
		/// Search routes to the target with up to 3 flybys (the best few, and
		/// the direct transfer for comparison)
		bool Suggest ();
		std::vector<Suggestion> Suggestions () const;
		/// Take a suggested route (its flybys) and search it from the suggestion
		bool UseSuggestion (int i);
		/// Bodies a route can fly by (with the target's parent; empty: none)
		std::vector<const CelestialBody*> FlybyBodies () const;
		/// Default least flyby altitude at a body [m]
		static double DefaultFlybyAlt (const CelestialBody *body);
		/// Porkchop plot around a departure date [MJD]
		bool SearchAround (double dep);
		/// Choose a transfer of the porkchop plot (or the optimum: dep < 0);
		/// routes: the departure date (the curve's nearest sample)
		void Select (double dep, double tof);
		/// Make the plan's manoeuvre nodes (replacing the vessel's nodes):
		/// the ejection refined with the numerical prediction, corrections,
		/// capture. Asynchronous; arm: fly them with the burn autopilot.
		bool MakeNodes (bool arm);
		/// Refine the next correction (or add one now if none is ahead); or
		/// target the ejection again (from the orbit as flown: a long coast in
		/// a low orbit drifts from its prediction)
		bool Correct (bool ejection = false);
		/// Fly the plan's nodes with the burn autopilot
		void Arm (bool on);
		/// Remove the plan's nodes
		void Clear ();
		/// Stop the running job
		void Abort ();

		/// Each frame (simulation thread): applies the worker's results,
		/// refines due corrections
		void Update ();

		// results (copies)
		std::shared_ptr<const Grid> Porkchop () const;
		std::shared_ptr<const Curve> Windows () const;
		Solution Best () const;
		/// The chosen transfer; routes: its departure (the ejection) and arrival
		Solution Selected () const;
		/// The chosen route (invalid: a direct transfer)
		Route GetRoute () const;
		LaunchPlan Launch () const;
		/// Choose a launch opportunity (index into LaunchPlan::opp)
		void SelectLaunch (int i);
		/// Compute the launch opportunities again (after changing the launch settings)
		void UpdateLaunch ();
		/// Default parking orbit altitude at a body [m]
		static double DefaultParkAlt (const CelestialBody *body);
		Plan GetPlan () const;
		Status GetStatus () const;
		Kind CurrentKind () const;
		const CelestialBody *Departure () const;   ///< departure body of the current search
		const CelestialBody *Central () const;     ///< central body of the transfer conic

		/// Velocity change the vessel's main engines can give with its
		/// propellant [m/s] (simulation thread)
		double AvailableDv () const;

	private:
		std::shared_ptr<Bodies> Snapshot (const CelestialBody *target) const;
		void Start (Status::Job job, std::function<void()> work);
		void Join ();
		int FindNode (double mjd) const;  // index of the vessel's node at this time (-1: none)
		static void Launches (const Bodies &b, const Solution &s, LaunchPlan &lp);
		bool RouteMode () const;          // the last search was of a route

		// worker jobs
		void DoSearch (std::shared_ptr<Bodies> b, double around);
		void DoRefine (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, Solution sol, Settings set, bool arm);
		void DoCorrect (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, std::vector<maneuver::Node> nodes,
			int refineIndex, double mccMjd, int planIndex, Settings set, bool arm);
		void DoRoute (std::shared_ptr<Bodies> b, Settings set, std::vector<double> seed);
		void DoSuggest (std::shared_ptr<Bodies> b, Settings set);
		void DoRefineRoute (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, Route r, Solution sol, Settings set, bool arm);
		void DoCorrectRoute (std::shared_ptr<Bodies> b, std::shared_ptr<trajectory::Sandbox> sb, std::vector<maneuver::Node> nodes,
			int refineIndex, double mjd, int planIndex, Settings set, bool arm);

		Vessel *vessel;
		std::thread worker;
		std::atomic<bool> abort{false};
		std::atomic<bool> busy{false};

		mutable std::mutex mtx; // guards the following
		std::shared_ptr<const Grid> grid;
		std::shared_ptr<const Curve> curve;
		Solution best, selected;
		Route route;                             // the chosen route (routes)
		std::vector<std::vector<double>> curveX; // the route's variables at each sample of curve
		std::vector<Suggestion> suggestions;
		std::vector<double> routeSeed;           // a start for the next route search (simulation thread)
		LaunchPlan launch;
		Plan plan;
		Status status;
		std::shared_ptr<Bodies> bodies;          // of the last search
		bool planPending = false;                // worker made a plan for the simulation thread
		Plan pendingPlan;
		bool pendingCorrection = false;          // worker refined a correction (pendingPlan holds it)
		int pendingPlanIndex = -1;

		// simulation thread
		double lastPlanCheck = 0.0;
	};

} // namespace transfer

#endif // !__TRANSFER_H
