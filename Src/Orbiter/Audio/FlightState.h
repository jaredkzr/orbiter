// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The flight of the vessel in focus, as the sound follows it: what phase
// it is in (on the ground, flying, climbing to orbit, in orbit) and the
// moments that change it - liftoff, touchdown, ignition, Max Q, MECO,
// orbit, a deorbit burn, an escape, a powered descent, docking's approach,
// fuel running low, the hull heating, sunrise, a new sphere of influence.
//
// Pure logic: each step it is given a FlightSample (what the vessel is
// doing: read from Orbiter by FlightSampler, or from a recorded trace in the
// tests) and publishes the step's FlightEvents, in order. The crew's voice
// (Callouts), the ground's radio (Radio) and the music read them; how each
// answers - which line, when, how often - is theirs.
//
// Under time acceleration (quiet) it only keeps up: nothing published, the
// state taken afresh once the time runs normally again.
// =======================================================================

#ifndef __AUDIOFLIGHTSTATE_H
#define __AUDIOFLIGHTSTATE_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace audio {

/// What the vessel in focus is doing this step
struct FlightSample {
	double t = 0.0, dt = 0.0;     ///< the simulation's time and step [s]
	// height and motion
	double alt = 0.0;             ///< its lowest touchdown point above the ground [m]
	double altitude = 0.0;        ///< its altitude (its centre's) [m]
	double airspeed = 0.0, groundspeed = 0.0;
	double vs = 0.0;              ///< vertical airspeed (up: positive) [m/s]
	double hvUp = 0.0;            ///< vertical groundspeed [m/s]
	double atmP = 0.0;            ///< the air's pressure [Pa]
	double mach = 0.0;            ///< its Mach number (0: out of the air, below ATM_P)
	double q = 0.0;               ///< dynamic pressure [Pa]
	bool contact = false;         ///< touching the ground (or landed)
	double gear = -1.0;           ///< 0 up .. 1 down (< 0: not known)
	// engines
	double main = 0.0, hover = 0.0, retro = 0.0; ///< the groups' levels
	bool hasMain = false;         ///< main engines it has
	bool mainUp = false;          ///< they point up (a rocket on its pad, not a plane)
	double rollRate = 0.0;        ///< turning about their thrust axis [rad/s]
	// orbit
	bool hasRef = false;          ///< a body it has a periapsis about
	bool orbit = false;           ///< the periapsis above the air (or the ground)
	bool escape = false;          ///< an open path about it
	bool airless = false;         ///< the body has no atmosphere
	const void *elRef = nullptr;  ///< the body it orbits (its sphere of influence)
	std::string elRefName;
	std::string surfaceName;      ///< the body under it
	// docking
	double dockDist = -1.0;       ///< its first port to the nearest docking signal [m] (< 0: none)
	bool docked = false;
	// modes
	int nav = 0, rcs = 0, af = 0; ///< the autopilot's mode (0: none), RCS mode, aerodynamic controls
	// fuel: the part left (< 0: no such tank); the main engines on another vessel's tank
	double mainFuel = -1.0, rcsFuel = -1.0;
	bool mainExternal = false;
	// the hull's heat, the structure's load (1: what it stands)
	double heat = 0.0, load = 0.0;
	// light
	bool dark = false;            ///< the Sun not on it
	bool lowAir = false;          ///< in the air, below 100 km
};

struct FlightEvent {
	enum Kind {
		// on the ground, in the air (FLIGHT)
		TOUCHDOWN,       ///< value: the sink rate [m/s]
		SURFACE,         ///< down on another world than Earth, from orbit
		ROLLOUT,         ///< stopped rolling after a landing
		LANDED,          ///< ... home from space
		LIFTOFF,         ///< value: 1 a rocket's (lit on its pad, engines up), 0 a plane's, a hover's
		POSITIVE_RATE,   ///< a runway takeoff climbing away (its first half minute)
		APPROACH,        ///< (DOCKING) a docking port within 1 km
		NAV,             ///< (MODES) value: the autopilot's mode (0: off)
		RCS_MODE, AF_MODE, ///< value: the mode
		// (ASCENT)
		IGNITION, DEORBIT, CAPTURE, ESCAPE, DESCENT, ROLL_PROGRAM, MAXQ, MECO, ORBIT,
		// (FUEL) value: 0 depleted, 1 low (5 %), 10 or 20 percent
		MAIN_FUEL, RCS_FUEL,
		HEAT,            ///< (HEAT) value: 1 hot (80 %), 2 critical (95 %)
		STRUCTURAL,      ///< the structure near its limit
		LIGHT,           ///< sunset (value 1) or sunrise (0), above the air
		SOI,             ///< text: the body it orbits now
		DEEPSPACE,       ///< from Earth's sphere into the Sun's
	};
	Kind kind;
	double value = 0.0;
	std::string text;
};

class FlightState {
public:
	/// [Pa] the air counted from (Mach, MECO)
	static constexpr double ATM_P = 10.0;

	/// A step of the vessel in focus (vessel: its identity - another one,
	/// or none, starts afresh); quiet: time accelerated (followed, nothing
	/// published). Returns this step's events (also Events ())
	/// (s null: nothing to read of it this step - destroyed: no events, the state kept)
	const std::vector<FlightEvent> &Step (uint64_t vessel, const FlightSample *s, bool quiet);
	/// Something happened to it (System::Event): a splashdown ends an ascent
	void Splashdown () { st.ascent = false; }
	void Reset ();

	const std::vector<FlightEvent> &Events () const { return events; }
	/// The vessel followed (its identity, BodyRef::serial; 0: none)
	uint64_t Vessel () const { return vessel; }
	/// This step's sample, and the previous one's values (Flight's: the
	/// voice's crossings are between them)
	const FlightSample &Now () const { return now; }
	double PrevAlt () const { return before.alt; }
	double PrevVs () const { return before.vs; }
	double PrevAirspeed () const { return before.airspeed; }
	double PrevMach () const { return before.mach; }
	/// The docking target's distance last step, this step (< 0: none)
	double PrevDock () const { return prevDock; }
	double DockDistance () const { return st.prevDist; }

	/// Where its flight is: how this step went (FIRST: state only), and
	/// the phase
	enum Branch { NONE, FIRST, TOUCHDOWN_STEP, ROLLING, LEAVING, FLYING };
	Branch StepBranch () const { return branch; }
	bool Flying () const { return st.flying; }
	bool Ascending () const { return st.ascent; }
	bool InOrbit () const { return st.inOrbit; }
	/// [s] since it left the ground (flying)
	double AirborneAt () const { return st.airborneAt; }
	/// A first step (afresh, or after time acceleration): no crossings yet
	bool First () const { return wasFirst; }
	/// It was read this step (false: none, destroyed, no module)
	bool Sampled () const { return sampled; }

private:
	void Flight (const FlightSample &s);
	void Docking (const FlightSample &s);
	void Modes (const FlightSample &s);
	void Ascent (const FlightSample &s);
	void Fuel (const FlightSample &s);
	void Heat (const FlightSample &s);
	void Daylight (const FlightSample &s, bool quiet);
	void Soi (const FlightSample &s);
	void Emit (FlightEvent::Kind k, double value = 0.0, const std::string &text = std::string ())
	{ events.push_back ({k, value, text}); }

	uint64_t vessel = 0;
	FlightSample now;
	std::vector<FlightEvent> events;
	Branch branch = NONE;
	bool wasFirst = false, sampled = false;
	double prevDock = -1.0;
	struct { double alt = -1.0, vs = 0.0, airspeed = -1.0, mach = -1.0; } before; // (the last step's, for this one)
	struct State {
		bool first = true;
		double prevAlt = -1.0, prevVs = 0.0, prevAirspeed = -1.0, prevMach = -1.0;
		double airborneAt = 0.0, heldSince = -1.0;
		bool flying = false, rolledOut = true;
		bool padIgnition = false, runway = false, posRateSaid = false;
		double prevDist = -1.0;
		int prevNav = -1, prevRcs = -1, prevAf = -1;
		bool ascent = false, maxqSaid = false, mecoSaid = false, orbitSaid = false;
		double qMax = 0.0, mainOffAt = -1.0, prevMain = 0.0;
		bool flewHigh = false;
		bool inOrbit = false, orbitAtBurn = false, prevBurning = false, orbited = false;
		int heatSaid = 0;
		bool loadSaid = false;
		bool escapeAtBurn = false, approachSaid = false;
		double liftoffAt = -1e9;
		bool rollSaid = false;
		double rolling = 0.0;
		bool descentSaid = false;
		double deorbitAt = -1e9;
		double prevMainFuel = -1.0, prevRcsFuel = -1.0;
	} st;
	// (kept across time acceleration)
	const void *soiRef = nullptr;
	std::string soiName;
	double soiAt = -1e9;
	int dark = -1;
	double lightChangedAt = -1.0;
};

/// The events' names (the log, the tests)
const char *FlightEventName (FlightEvent::Kind k);

/// A trace of samples: one line each (WriteSample), read back by ReadSample
/// (ORBITER_AUDIO_FLIGHT_TRACE; Tests/Audio/*.trace)
void WriteSample (FILE *f, const FlightSample &s);
bool ReadSample (const std::string &line, FlightSample &s);

} // namespace audio

#endif // !__AUDIOFLIGHTSTATE_H
