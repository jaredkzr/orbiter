// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Trajectory prediction for the map view and manoeuvre planning: the
// path of a vessel (and of a target vessel) into the future, with its
// manoeuvre nodes applied as impulsive velocity changes. It is integrated
// numerically under the gravity of the Sun, the planets and their moons
// (with the zonal J2-J4 terms of nonspherical bodies) on a worker thread.
// The bodies' positions come from their ephemerides, sampled at future
// times and interpolated (Hermite), so that the prediction matches the
// simulation's own gravity model; atmospheric drag is not included.
//
// The path is split into segments by sphere of influence (Laplace
// sphere): each segment's points are relative to its reference body.
// =======================================================================

#ifndef __TRAJECTORY_H
#define __TRAJECTORY_H

#include "Vecmat.h"
#include "Maneuver.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Body;
class CelestialBody;
class Vessel;
class Elements;

namespace trajectory {

	struct Point {
		double t;      // simulation time
		Vector r, v;   // relative to the segment's reference body
	};

	struct Segment {
		const CelestialBody *ref; // reference body
		int node;                 // index of the node the segment starts at (-1: before the first)
		std::vector<Point> pts;
		Vector refG, parentG;     // global positions of ref and its parent at the segment's start
	};

	struct NodeState {
		double t;                 // simulation time
		const CelestialBody *ref;
		Vector r, v;              // state before the burn, relative to ref
		Vector dvg;               // the velocity change (global frame)
	};

	struct Event {
		enum Type { PERIAPSIS, APOAPSIS, SOI_ENTER, SOI_EXIT, IMPACT, CLOSEST };
		Type type;
		double t;
		const CelestialBody *ref; // reference body (CLOSEST: segment's reference)
		Vector r;                 // position relative to ref
		double value;             // altitude above ref (CLOSEST: distance to the target) [m]
		double speed;             // relative speed (CLOSEST) [m/s]
		int node;                 // the segment's node index
		int seg;                  // index of the segment it belongs to (in segs)
	};

	struct Prediction {
		double t0 = 0.0;          // simulation time of the vessel's state it starts from
		double t1 = 0.0;          // end of the prediction
		Vector rEnd, vEnd;        // the vessel's global state at t1
		std::vector<Segment> segs;
		std::vector<NodeState> nodes;
		std::vector<Event> events;
		std::vector<Segment> target; // path of the target vessel (if any; same segmentation rules)
		const Vessel *vessel = nullptr;
		const Body *targetBody = nullptr;
		bool complete = false;
		unsigned int serial = 0;  // incremented for each new prediction
	};

	class Predictor {
	public:
		Predictor ();
		~Predictor ();

		/// Called each frame on the simulation thread: starts a new
		/// prediction for vessel v (and target, a celestial body or vessel,
		/// may be null) when its inputs changed, or periodically while the
		/// vessel is under thrust.
		void Update (const Vessel *v, const Body *target);

		/// The latest finished prediction (null if none)
		std::shared_ptr<const Prediction> Latest () const;

		/// Discard the prediction and stop (e.g. on focus change)
		void Reset ();

		/// Longest prediction [days] (Orbiter.cfg: PredictionDays)
		double maxDays = 400.0;

		struct Input;
		struct Context;
		/// Snapshot of a vessel (its nodes, and target) and of the bodies
		/// for a prediction (simulation thread); null if the vessel isn't in flight
		static std::shared_ptr<Input> Snapshot (const Vessel *v, const Body *target, double days);
		/// A prediction (any thread); null if aborted
		static std::shared_ptr<Prediction> Compute (const Input &in, Context &ctx, const std::atomic<bool> &abort);

	private:
		void Run ();

		std::thread worker;
		mutable std::mutex mtx;
		std::condition_variable cv;
		std::shared_ptr<Input> pending;     // next request
		std::shared_ptr<const Prediction> latest;
		std::atomic<bool> quit{false};
		std::atomic<bool> abort{false};     // stop the current run (a newer request is pending)
		std::unique_ptr<Context> ctx;       // worker state (ephemeris caches)

		// change detection (simulation thread)
		const Vessel *lastVessel = nullptr;
		const Body *lastTarget = nullptr;
		unsigned int lastNodeVersion = ~0u;
		double lastRequestSys = -1e10;      // system time of the last request
		double lastRequestSim = -1e10;
		unsigned int serial = 0;
	};

	/// Predictions of a vessel's path with other manoeuvre nodes than its own,
	/// off the simulation thread (manoeuvre planning). The vessel and the
	/// bodies are taken at construction (simulation thread); Predict may then
	/// be called on any one thread.
	class Sandbox {
	public:
		Sandbox (const Vessel *v, const Body *target, double days);
		~Sandbox ();
		bool Valid () const { return in != nullptr; }
		/// Simulation time and MJD of the snapshot
		double T0 () const;
		double MJD0 () const { return mjd0; }
		/// Prediction with these nodes (replacing the vessel's), for up to days
		std::shared_ptr<const Prediction> Predict (const std::vector<maneuver::Node> &nodes, double days, const std::atomic<bool> *abort = nullptr);
		/// The body whose closest approaches are reported (one of the snapshot's bodies)
		void SetTarget (const CelestialBody *target);
		/// Move the snapshot to a later time [MJD] (before all the nodes of later
		/// predictions): the coast to it is integrated once, not with each prediction
		bool Advance (double mjd, const std::atomic<bool> *abort = nullptr);
	private:
		std::shared_ptr<Predictor::Input> in;
		std::unique_ptr<Predictor::Context> ctx;
		double mjd0 = 0.0;
	};

} // namespace trajectory

#endif // !__TRAJECTORY_H
