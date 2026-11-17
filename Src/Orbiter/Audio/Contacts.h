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
// The surface under a vessel (Vessel::ContactSurface): paved (a base's
// runways, graded areas, pads and decks), regolith (an airless body), and
// on other ground by the planet's surface texture as the ground scatter
// reads it (SurfaceFromCover): soil (green: grass, meadow, bush, forest
// floor, farmland), rock (not green: bare, arid, desert - gravelly), snow,
// water. Each its layers (LayersFor): soil, rock and snow roll and scrape
// on the ground's own takes, a dirt spray where a tyre would chirp; on
// regolith a crunch through the structure at every blow.
//
// Pure logic (no Orbiter): the tests feed it samples of their own.
// =======================================================================

#ifndef __AUDIOCONTACTS_H
#define __AUDIOCONTACTS_H

#include "Track.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace audio {

enum class ContactKind { WHEEL, LEG, HULL, OBSTACLE, VESSEL };
enum class SurfaceKind { PAVED, SOIL, REGOLITH, ROCK, SNOW, WATER };

const char *ContactKindName (ContactKind k);
const char *SurfaceKindName (SurfaceKind s);

/// The ground's kind from its surface texture's colour (linear 0-1) and the
/// water mask (0 or 1) at a point - the same rules as the ground scatter's
/// cover (GroundScatter::CoverFactor): water; snow (bright, grey, not warm); soil
/// (green); else rock (bare, arid, desert). have: false - no surface tiles
/// there (soil)
SurfaceKind SurfaceFromCover (double r, double g, double b, double water, bool have = true);
/// Whether white ground can be snow there: far from the equator (|lat|
/// over SNOW_LAT) or high up (over SNOW_ELEV) - a bright flat at low
/// latitudes and heights is a dry lake's bed, a salt pan, sand, as white
/// on the surface texture (Rogers Dry Lake: 0.91 0.91 0.94)
bool SnowLikely (double latDeg, double elev);
constexpr double SNOW_LAT = 50.0, SNOW_ELEV = 1500.0;   // [deg], [m]

/// What a surface sounds of (layers of Sound/Contacts/contacts.txt; null:
/// none), each falling back to the paved one where its own is missing
struct SurfaceLayers {
	const char *rolling;   ///< the rolling bed
	const char *scrape;    ///< the scrape bed (hull, legs, obstacles)
	const char *touch;     ///< a wheel touching down sliding (Chirps): a tyre's chirp, a spray of dirt; null: none
	const char *blow;      ///< more at a blow on the ground (spray: the hull's; crunch: every one); null: none
	bool blowGear;         ///< ... also at the wheels' and legs' blows
	bool blowAir;          ///< ... heard outside too (else through the structure only)
	double rollGain;       ///< the rolling bed's level, of the paved law's
};
SurfaceLayers LayersFor (SurfaceKind s);
/// the paved layer a surface's own one falls back to (null: none)
const char *FallbackLayer (const char *layer);

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
	double scrapeP = 0.0, scrapeRate = 1.0, scrapeCutoff = 20000.0;   ///< (stage 3) [Pa], rate, its low-pass [Hz]
};

/// The tyres' rolling and the brakes' groan from a step's levels:
///  - rolling: ROLL_P at ROLL_V on ROLL_LOAD, as v^1.5 sqrt(load) (silent
///    at rest, or with no load on the wheels); playback rate ROLL_RATE0 at
///    rest to ROLL_RATE1 from ROLL_RATE_V on (more than an octave: a rumble
///    is noise - a smaller change isn't heard)
///  - braking and the tyres' scrub: BRAKE_P at BRAKE_W of friction power,
///    as its root (capped at 4x); rate BRAKE_RATE0 at rest to BRAKE_RATE1
///    from BRAKE_RATE_V on
///  - scraping (stage 3: the hull, legs, obstacles, other vessels): SCRAPE_P
///    at SCRAPE_W of friction power, as its root (capped at SCRAPE_CAP x);
///    faster slipping: higher (SCRAPE_RATE0 at rest to SCRAPE_RATE1 from
///    SCRAPE_V on) and brighter (its low-pass SCRAPE_FC0 to SCRAPE_FC1, on
///    a log scale)
ContactBeds BedsFor (const ContactLevels &l);
/// (a delta-glider on its belly: its hull's friction 3 - at 100 m/s some
/// 35 MW, at the cap: 2 Pa, 100 dB at 10 m, sustained; its hull's blows at
/// touchdown, ~1e4 J, ~1 Pa RMS at 10 m over their 0.3 s and peaks some 10 dB
/// over that - the slams cut through the grind, not under it)
constexpr double SCRAPE_P = 0.5, SCRAPE_W = 1e6, SCRAPE_CAP = 4.0;
constexpr double SCRAPE_RATE0 = 0.7, SCRAPE_RATE1 = 1.3, SCRAPE_V = 40.0;
constexpr double SCRAPE_FC0 = 800.0, SCRAPE_FC1 = 12000.0;

/// A scrape's start (or a re-contact after leaving SCRAPE_GAP): louder at
/// first - BURST_GAIN over its level, easing back over BURST_T (the first
/// bite of metal on the ground)
class ScrapeBurst {
public:
	static constexpr double SCRAPE_GAP = 0.2, BURST_GAIN = 2.0, BURST_T = 0.3;
	/// A step at time t with this friction power [W]: the factor on its level
	double Step (double t, double power);
private:
	double lastOn = -1e9, startT = -1e9;
};

/// A part breaking off (BREAK): the tear's energy of sound [J], the debris'
/// clatter's, the tear's playback rate, the clatter's delay [s]. The blow's
/// energy: the piece's at the speed it was struck (at least BREAK_VMIN: a
/// part torn off by load is still torn), ETA_TEAR of it the tear (a hull
/// blow's ETA_METAL a tenth: the energy goes into tearing, not ringing),
/// CLATTER_SHARE of that the clatter; heavier pieces lower: (m / 1000 kg)^-0.1
/// within TEAR_RATE
struct BreakSound { double tearE = 0.0, clatterE = 0.0, rate = 1.0, clatterDelay = 0.2; };
BreakSound BreakFor (double mass, double hitSpeed, double u01);
constexpr double BREAK_VMIN = 5.0, ETA_TEAR = 1e-5, CLATTER_SHARE = 0.5;
constexpr double TEAR_RATE[2] = {0.7, 1.3}, CLATTER_DELAY[2] = {0.1, 0.3};

/// A crash (the vessel destroyed striking the ground): ETA_CRASH of its
/// kinetic energy as sound (the design's 1e-4: a 10 t vessel at 50 m/s,
/// 12.5 MJ, ~125 dB at 10 m - a car crash's 110 dB at a hundredth of it),
/// shared out: the crash's take CRASH_SHARE, CRASH_TEARS tears staggered over
/// CRASH_TEAR_T (TEAR_SHARE each), the clatter after CRASH_CLATTER_T
/// (CLATTER_SHARE_C)
struct CrashPart { int layer; double t, energy, rate; };   // (layer: 0 crash, 1 tear, 2 clatter; t after the crash [s])
std::vector<CrashPart> CrashFor (double energy, double mass, double u01a, double u01b);
constexpr double ETA_CRASH = 1e-4, CRASH_SHARE = 0.6, TEAR_SHARE = 0.08, CLATTER_SHARE_C = 0.16;
constexpr int CRASH_TEARS = 3;
constexpr double CRASH_TEAR_T = 0.3, CRASH_CLATTER_T = 0.5;
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

// -----------------------------------------------------------------------
// Stage 4: docking's and undocking's mechanisms, the animations' motors
// -----------------------------------------------------------------------

/// Uniform draws in [0, 1) from a seed (the timelines' spread: repeatable)
struct Uniforms {
	explicit Uniforms (uint32_t seed): s (seed * 2654435761u + 0x9e3779b9u) {}
	double operator() () { s = s * 1664525u + 1013904223u; return (double)(s >> 8) / 16777216.0; }
	uint32_t s;
};

/// A mechanism's sound in a timeline: what (its layer), when after the
/// moment [s], how loud - the blows (BLOW, PUSH) by their energy of sound
/// [J] (as a hull blow's: ETA_METAL of the docking's, the separation's), the
/// mechanisms at a fixed level in the cabin [Pa] (heard outside MECH_AIR of
/// it, at the reference distance) - and its playback rate
struct MechShot {
	enum What { BLOW, LATCH, HOOK, RELEASE, SEAL, PUSH };
	What what;
	double t = 0.0, level = 0.0, rate = 1.0;
};

/// Docking (the design's row J, its timeline over 0 to ~2 s): the ports'
/// blow at once (impact_metal by the docking's energy 1/2 m_red v^2), the
/// capture latch DOCK_LATCH_T later, a ripple of DOCK_HOOKS hooks closing
/// HOOK_GAP apart from HOOK_AFTER after it, the seal's thump once they are
/// in (SEAL_AFTER after the last, within DOCK_SEAL_T of the contact)
std::vector<MechShot> DockTimeline (double speed, double mred, Uniforms &u);
/// Undocking: the release at once, UNDOCK_HOOKS hooks opening (the ripple
/// reversed, fewer) from HOOK_AFTER on, the push-off after them by the
/// separation's energy (1/2 (m/2) v^2: m the vessel's mass)
std::vector<MechShot> UndockTimeline (double speed, double mass, Uniforms &u);
constexpr double LATCH_P = 0.3, HOOK_P = 0.12, RELEASE_P = 0.25, SEAL_P = 0.35;   // [Pa] in the cabin (design: 0.1-0.4)
constexpr double MECH_AIR = 0.178;      // (outside: -15 dB, at the reference distance; in vacuum nothing)
constexpr double DOCK_LATCH_T[2] = {0.1, 0.3}, HOOK_AFTER = 0.25, HOOK_GAP[2] = {0.08, 0.16};
constexpr int DOCK_HOOKS[2] = {4, 8}, UNDOCK_HOOKS[2] = {2, 4};
constexpr double SEAL_AFTER = 0.25, DOCK_SEAL_T[2] = {1.0, 2.0}, PUSH_AFTER = 0.15;
constexpr double MECH_ETA = 1e-4;       // (the blows: as a hull's, ETA_METAL)

/// An animation's motor (the design's row K): moving or not, from its state
/// step to step; the moments - it started; it stopped at an end (within
/// MOTOR_END of 0 or 1: it closed, locked); it stopped on the way
class MotorTrack {
public:
	enum Event { NONE, START, END, STOP };
	static constexpr double MOTOR_END = 0.01, MOVING = 1e-4;   // (of its travel; [1/s])
	/// Its state (0..1) at time t, dt after the last: the moment, if one
	Event Step (double t, double dt, double state);
	bool Moving () const { return moving; }
	double Speed () const { return speed; }   ///< [travels/s]
	double Since () const { return startT; }  ///< [s] when it started
private:
	double last = -1.0, speed = 0.0, startT = -1e9;
	bool moving = false;
};

/// A motor heard: its level in the cabin [Pa] and playback rate, for its
/// kind (0 a motor/servo, 1 a door, 2 the gear: AUDIO_ANIM_* - 1), its
/// level (the vessel's setting), its speed [travels/s], the time [s] (a
/// slow wobble; phase: its own). Levels MOTOR_P (design 0.03-0.1 Pa), rates
/// MOTOR_RATE by kind, +-MOTOR_PITCH with the speed (MOTOR_SPEED_REF: a
/// travel in 4 s, full), wobbling MOTOR_WOBBLE at MOTOR_WOBBLE_HZ; outside
/// MOTOR_AIR of it (-20 dB)
struct MotorSound { double p = 0.0, rate = 1.0; };
MotorSound MotorFor (int kind, double level, double speed, double t, double phase);
constexpr double MOTOR_P[3] = {0.04, 0.06, 0.09}, MOTOR_RATE[3] = {1.12, 1.0, 0.9};
constexpr double MOTOR_PITCH = 0.12, MOTOR_SPEED_REF = 0.25, MOTOR_WOBBLE = 0.02, MOTOR_WOBBLE_HZ = 0.37;
constexpr double MOTOR_AIR = 0.1;
/// At an end: the lock (a latch take) LOCK_T after the motor stops, at
/// LOCK_P by kind [Pa]; the gear's low thud (impact_gear, soft) GEAR_THUD_P,
/// a door's seal SEAL_DOOR_P after DOOR_SEAL_T
constexpr double LOCK_T = 0.08, LOCK_P[3] = {0.1, 0.25, 0.3}, GEAR_THUD_P = 0.3, SEAL_DOOR_P = 0.25, DOOR_SEAL_T = 0.12;

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
