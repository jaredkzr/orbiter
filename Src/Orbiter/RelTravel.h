// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Relativistic travel: the trip autopilot of the ideal drive (the
// Relativistic Travel MFD's). A trip to a celestial body or a vessel, to
// arrive at rest at an altitude above it, in a circular orbit, or passing
// it, by burns of constant proper acceleration (relativity::SolveTrip):
// as soon as possible, or arriving at a set time with a coast between the
// burns.
//
// Each step (before the forces) the trip is solved again from the
// vessel's state, the target followed along conics about its primaries:
// gravity and the attitude's lag are corrected as they arise. The drive
// pushes along the vessel's +z axis: the autopilot turns it towards the
// current burn's direction (attitude thrusters), less gravity (the
// planned trip is free of it), throttles it while the attitude is off,
// and cuts it off where a burn ends within a step. Time warp is lowered
// ahead of a burn's end.
// =======================================================================

#ifndef __RELTRAVEL_H
#define __RELTRAVEL_H

#include "Relativity.h"
#include <string>

class Vessel;
class Body;

namespace reltravel {

	const double MAX_ACCEL = 1e6 * relativity::G0; ///< largest proper acceleration of a trip [m/s^2] (the drive keeps a quarter more for the autopilot)

	enum Arrival {
		ARRIVE_REST,  ///< at rest relative to the target, at an altitude above it
		ARRIVE_ORBIT, ///< in a circular orbit at an altitude (celestial bodies)
		ARRIVE_FLYBY  ///< passing the target at an altitude, at speed
	};

	enum Phase {
		PH_IDLE,     ///< not engaged
		PH_ALIGN,    ///< turning to the first burn, the drive off
		PH_BURN1,    ///< accelerating
		PH_COAST,    ///< coasting (between the burns; a flyby after its burn)
		PH_BURN2,    ///< decelerating
		PH_ARRIVED,  ///< done (the drive off)
		PH_FAILED    ///< no trip found (the drive off)
	};
	const char *PhaseName (int phase);

	struct Settings {
		const Body *target = nullptr;
		int star = -1;               ///< else a named star (StarTargets.h; arrival: at rest or passing)
		std::string targetName;      ///< (until resolved, e.g. read from a scenario; a star: "star:<name>")
		int arrival = ARRIVE_REST;
		double altitude = 1e6;       ///< above the target's surface [m] (a star: the distance from it)
		double accel = 1000.0 * relativity::G0; ///< proper acceleration [m/s^2]
		bool fastest = true;         ///< as soon as possible (else: after tripTime)
		double tripTime = 86400.0;   ///< trip time when not fastest [s] (Solar System time)
	};

	struct Plan {
		bool ok = false;
		relativity::TripSpec spec;   ///< (from the state when planned; target function valid while the controller lives)
		relativity::Trip trip;
		double planned = 0.0;        ///< simulation time of the plan
	};

	class Controller {
	public:
		Controller (Vessel *v);
		Settings settings;

		/// The trip from the vessel's current state (for display; the
		/// autopilot plans its own each step)
		const Plan &Preview ();
		/// Engage or disengage the autopilot (false: refused, e.g. no target)
		bool Engage (bool on);
		bool Engaged () const { return phase != PH_IDLE && phase != PH_ARRIVED && phase != PH_FAILED; }
		/// Each step, before the forces (from Vessel::UpdateThrustForces)
		void Update ();

		int GetPhase () const { return phase; }
		const Plan &Current () const { return cur; } ///< the autopilot's plan of this step
		double AttitudeError () const { return attErr; }
		double ArriveT () const { return tArrive; }  ///< fixed arrival time when engaged not fastest (0: fastest)
		void SetArriveT (double t) { tArrive = t; }
		const std::string &Message () const { return message; }
		/// Time to the arrival from now, Solar System and the vessel's clock
		/// [s] (engaged: the autopilot's estimate; else the preview's)
		bool Remaining (double &tSolar, double &tShip) const;
		void SetPhase (int ph) { phase = ph; aligned = guided = stopped = false; }
		void Abort (const char *msg); ///< stop the trip (e.g. on an impact)

		/// The target (resolved): a body, else a named star
		bool HasTarget () const { return settings.target || settings.star >= 0; }
		std::string TargetName () const;
		/// The target's state at simulation time t (global frame)
		void TargetState (double t, Vector &x, Vector &v) const;
		void SetTarget (const Body *b);
		void SetStar (int i);          ///< (the arrival distance: the star's default)

		/// State of a body (or vessel) at simulation time t, extrapolated
		/// from now along conics about its primaries (relativistic vessels:
		/// straight)
		static void BodyState (const Body *b, double t, Vector &x, Vector &v);

		/// Resolve a target known by name (read from a scenario); false: none
		bool Resolve ();

	private:
		bool Solve (Plan &p, double tArr, const Plan *warm = nullptr);
		void Stop (int ph, const char *msg);
		double Point (const Vector &dir);              // the drive's axis towards dir: the attitude error
		bool IdealTurns () const;                      // turned by the ideal drive (no attitude thrusters, or steps too long for them)
		void Drive (const Vector &alpha, double tEnd); // the drive at alpha (throttled by the attitude error), cut off at tEnd
		void LimitWarp (double tEvent);
		void Guide ();                                 // after the first burn: guided to the arrival
		Vessel *vessel;
		int phase = PH_IDLE;
		bool aligned = false;
		bool guided = false;           // past the first burn
		bool stopped = false;          // braked to rest (a rendezvous)
		double tStop = 0.0;            // [s] time to rest, as of the last step
		double uRel = 0.0, aBrake = 0.0; // braking: proper speed relative to the arrival, the deceleration
		relativity::TargetFunc arrival; // the arrival state (fixed when guided)
		Vector lastAlpha;
		int fails = 0;
		int replans = 0;               // new approaches after stopping short
		double tArrive = 0.0;
		double attErr = 0.0;
		double driveSign = 1.0;        // the drive's end pointed: +z, or reversed (-z)
		double warpRestore = 0.0;      // the ship-time warp to return to after an event (0: none)
		double warpLastSet = 0.0;      // the ship-time warp as last left here
		Plan preview, cur;
		std::string message;
	};

} // namespace reltravel

#endif // !__RELTRAVEL_H
