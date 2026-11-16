// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// What a vessel's contacts do, as the sound will need it (the design:
// ~/audio-gen/casting/contacts-design.md; stage 0: the inputs, no sound):
//
//  - each step, a sample of each of its contact points (Vessel::SoundContacts):
//    which point (a touchdown point's index), what kind (a wheel, a leg, the
//    hull, an obstacle, another vessel), the force on it, its slip, how fast
//    it closed, its effective mass
//  - from them, ContactModel tells the moments: a point touching anew (an
//    impact: its closing speed, the energy of the blow 1/2 m_eff v^2), and
//    the step's continuous levels (the wheels' load and rolling speed, the
//    tyres' and brakes' friction power, the hull's scrape)
//  - and the moments that come from elsewhere: docking (its closing speed,
//    the vessels' reduced mass), undocking, a part breaking off, a crash
//
// The surface under a vessel: paved (a base's runways, graded areas, pads
// and decks), regolith (an airless body), soil (any other ground, until the
// surfaces are told apart).
//
// Pure logic (no Orbiter): the tests feed it samples of their own.
// =======================================================================

#ifndef __AUDIOCONTACTS_H
#define __AUDIOCONTACTS_H

#include "Track.h"
#include <map>
#include <string>
#include <vector>

namespace audio {

enum class ContactKind { WHEEL, LEG, HULL, OBSTACLE, VESSEL };
enum class SurfaceKind { PAVED, SOIL, REGOLITH, ROCK, SNOW };

const char *ContactKindName (ContactKind k);
const char *SurfaceKindName (SurfaceKind s);

/// A contact point of a vessel over a step (after the contact solver)
struct ContactSample {
	int id = -1;                 ///< which point: a touchdown point's index (-1: none - hull vertices, obstacles, vessels)
	ContactKind kind = ContactKind::HULL;
	V3 pos;                      ///< where (vessel frame) [m]
	double Fn = 0.0;             ///< the normal force [N]
	double Flong = 0.0, Flat = 0.0; ///< the friction force along its rolling direction, across it [N]
	double slipLong = 0.0;       ///< sliding speed along its rolling direction [m/s] (a wheel: its rolling speed - no spin yet)
	double slipLat = 0.0;        ///< ... across it [m/s]
	double vn = 0.0;             ///< closing speed at the step's start (beyond the step's fall) [m/s]
	double meff = 0.0;           ///< the effective mass along the normal [kg] (1/Kn)
	double mu = 0.0;             ///< friction coefficient along the rolling direction
	double brake = 0.0;          ///< the wheel's brake (0..1)
};

/// A moment of a vessel's contacts
struct ContactEvent {
	enum Type { IMPACT, DOCK, UNDOCK, BREAK, CRASH };
	Type type = IMPACT;
	double t = 0.0;              ///< [s] simulation time
	ContactKind kind = ContactKind::HULL; ///< (IMPACT) what struck
	int id = -1;                 ///< (IMPACT) which point (-1: none)
	int points = 1;              ///< (IMPACT) how many points struck together
	double speed = 0.0;          ///< closing (DOCK, IMPACT), separation (UNDOCK), hit (BREAK), impact (CRASH) speed [m/s]
	double energy = 0.0;         ///< the blow's energy [J] (IMPACT: 1/2 m_eff v^2, the most of its points'; DOCK: 1/2 m_red v^2; CRASH: kinetic)
	double mass = 0.0;           ///< DOCK: the reduced mass; BREAK: the piece's; CRASH: the vessel's [kg]
	V3 pos;                      ///< where (vessel frame) [m]
	SurfaceKind surface = SurfaceKind::SOIL; ///< (IMPACT on the ground)
};

/// The log's line of an event (without the vessel's name)
std::string Describe (const ContactEvent &e);

/// A vessel's contacts over a step, summed: what sounds on through it
struct ContactLevels {
	double wheelLoad = 0.0;      ///< the wheels' normal force [N]
	double rollSpeed = 0.0;      ///< the fastest wheel's rolling speed [m/s]
	double scrubPower = 0.0;     ///< the tyres' friction power across [W]
	double brakePower = 0.0;     ///< ... along, braked [W]
	double scrapePower = 0.0;    ///< the hull's, legs' and obstacles' friction power [W]
	double scrapeSpeed = 0.0;    ///< their fastest slip [m/s]
	double vesselScrape = 0.0;   ///< against other vessels [W]
	bool any = false;
};

/// What the wheels' rolling and the brakes sound like over a step (stage 2):
/// pressures at 10 m [Pa] (as heard outside; the beds' levels), playback rates
struct ContactBeds {
	double rollP = 0.0, rollRate = 1.0;
	double brakeP = 0.0, brakeRate = 1.0;
};

/// The tyres' rolling and the brakes' groan from a step's levels:
///  - rolling: ROLL_P at ROLL_V on ROLL_LOAD, as v^1.5 sqrt(load) (silent
///    at rest, or with no load on the wheels); playback rate ROLL_RATE0 at
///    rest to ROLL_RATE1 from ROLL_RATE_V on (more than an octave: a rumble
///    is noise - a smaller change isn't heard)
///  - braking and the tyres' scrub: BRAKE_P at BRAKE_W of friction power,
///    as its root (capped at 4x); rate BRAKE_RATE0 at rest to BRAKE_RATE1
///    from BRAKE_RATE_V on
ContactBeds BedsFor (const ContactLevels &l);
/// (the design said 0.2 Pa - 80 dB at 10 m - at 40 m/s for 15 t; that buries
/// a gentle touchdown's thump at landing speed: a 1 m/s touchdown's take is
/// about 0.2 Pa RMS at 10 m. 0.06 Pa keeps it ~10 dB over rolling at 95 m/s)
constexpr double ROLL_P = 0.06, ROLL_V = 40.0, ROLL_LOAD = 15000.0 * 9.81, ROLL_VMAX = 150.0, ROLL_RATE_V = 100.0;
constexpr double ROLL_RATE0 = 0.6, ROLL_RATE1 = 1.4;
constexpr double BRAKE_P = 0.08, BRAKE_W = 1e6, BRAKE_RATE0 = 0.7, BRAKE_RATE1 = 1.3, BRAKE_RATE_V = 80.0;
/// A chirp's rate by the wheel's sliding speed: CHIRP_RATE0 at 15 m/s to
/// CHIRP_RATE1 from CHIRP_RATE_V on
constexpr double CHIRP_RATE0 = 0.9, CHIRP_RATE1 = 1.1, CHIRP_RATE_V = 120.0;

/// A wheel's chirp at touchdown: the still wheel spun up by the runway's
/// friction - how long that takes, and the friction's work meanwhile
struct Chirp {
	bool on = false;
	int id = -1;
	double t = 0.0;        ///< [s] when (the wheel's touchdown)
	double speed = 0.0;    ///< the slip at touchdown (the ground speed) [m/s]
	double spinUp = 0.0;   ///< [s]
	double work = 0.0;     ///< [J] the friction's: mu Fn (v/2) spinUp
};

/// Which wheel impacts chirp. A wheel touching down (an impact of a wheel
/// in the air at least CHIRP_AIR before, not chirped within CHIRP_GAP)
/// sliding over CHIRP_V chirps. Its spin-up, from a guess of the wheel:
/// its mass WHEEL_MASS of the static load it carries (an airliner's main
/// wheel and tyre, ~200 kg for ~20 t; the load: the vessel's weight over
/// its wheels, at least 3), its inertia 0.6 m r^2 (the tyre's mass outward),
/// the runway's friction MU_TYRE pressing with the larger of the sample's
/// force and a third of its static load: t = 0.6 m v / (mu Fn), clamped to
/// CHIRP_T; the work mu Fn (v/2) t = 0.3 m v^2 (the spun wheel's energy)
class Chirps {
public:
	static constexpr double CHIRP_V = 15.0, CHIRP_AIR = 0.25, CHIRP_GAP = 0.5;
	static constexpr double WHEEL_MASS = 0.01, MU_TYRE = 0.7, CHIRP_T[2] = {0.04, 0.4};
	/// A step's: of its impacts (m.Events ()), those that chirp; mass: the
	/// vessel's [kg]. Call once per step, after ContactModel::Step
	std::vector<Chirp> Step (double t, const std::vector<ContactSample> &pts, const std::vector<ContactEvent> &events, double mass);
	void Reset () { seen.clear (), chirped.clear (); }
private:
	std::map<int, double> seen, chirped;   // (by wheel: last in contact, last chirped)
	int wheels = 0;
};

class ContactModel {
public:
	/// A point touching anew closing at least this fast strikes [m/s]
	static constexpr double STRIKE_V = 0.05;
	/// A point in contact all along strikes again when its closing speed rises
	/// this much over its slowest since it last struck (a bump rolled over, a
	/// bounce within the step; not a strut compressing ever more slowly) [m/s]
	static constexpr double RESTRIKE_V = 0.5;
	/// The shortest time between two strikes of one point [s]
	static constexpr double STRIKE_GAP = 0.1;
	/// A point left this long ago (steps without it) is touching anew [s]
	static constexpr double LEFT_AFTER = 0.05;

	/// A step of the vessel's contacts at time t [s] (its end), of length h [s],
	/// on surface s. Appends the impacts to Events (); Levels (): this step's
	void Step (double t, double h, const std::vector<ContactSample> &pts, SurfaceKind s);
	void Dock (double t, const V3 &port, double speed, double mred);
	void Undock (double t, const V3 &port, double speed);
	void Break (double t, const V3 &pos, double mass, double hitSpeed);

	const std::vector<ContactEvent> &Events () const { return events; }
	void ClearEvents () { events.clear (); }
	const ContactLevels &Levels () const { return levels; }
	/// Forget which points touch (time accelerated, a replay)
	void Reset () { touch.clear (); }

private:
	struct Touch { double lastSeen = -1e9, lastStrike = -1e9, vlow = 0.0; };
	std::map<int, Touch> touch;  // (by Key)
	static int Key (const ContactSample &p) { return p.id >= 0 ? p.id : -1 - (int)p.kind; }
	std::vector<ContactEvent> events;
	ContactLevels levels;
};

} // namespace audio

#endif // !__AUDIOCONTACTS_H
