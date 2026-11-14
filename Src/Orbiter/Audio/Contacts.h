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

class ContactModel {
public:
	/// A point touching anew closing at least this fast strikes [m/s]
	static constexpr double STRIKE_V = 0.05;
	/// A point in contact all along, closing faster than this, strikes again
	/// (a bounce within the step, a bump rolled over) [m/s]
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
	struct Touch { double lastSeen = -1e9, lastStrike = -1e9; };
	std::map<int, Touch> touch;  // (by Key)
	static int Key (const ContactSample &p) { return p.id >= 0 ? p.id : -1 - (int)p.kind; }
	std::vector<ContactEvent> events;
	ContactLevels levels;
};

} // namespace audio

#endif // !__AUDIOCONTACTS_H
