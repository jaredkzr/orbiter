// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The sounds every vessel makes by itself, from what it is and does:
//
//  - engines: its thrusters (all but the attitude thrusters), clustered by
//    where they are and where they point: one rocket synthesizer each, as
//    loud as the jet's power makes it (about 0.5 % of it becomes sound,
//    Lighthill/NASA SP-8072; the exhaust's speed counted at most 4.5 km/s,
//    a chemical rocket's), loudest aft (-20 dB ahead)
//  - attitude thrusters (the vessel's ATT_* groups): clustered by place,
//    their pulses' bangs and hiss
//  - the air rushing over it: as loud as the dynamic pressure and speed
//    make it (boundary-layer noise, p ~ q U), buffeting; it booms when
//    supersonic
//  - its contacts (ContactSynth), and the motors of its animations named
//    for sound (Config/Sound/<class>.cfg "Anim = <index> <motor|door|gear>
//    [level]", or oapiAudioAnimation from its module)
//
// Config/Sound/<class>.cfg also tunes it ("engine_level = 1.2", ...: Tune)
//
// In the cockpit: engines and thrusters through the structure (also in
// vacuum), the air through the hull. Vessels within RANGE of the camera.
// =======================================================================

#ifndef __VESSELSOUNDS_H
#define __VESSELSOUNDS_H

#include "Contacts.h"
#include "ContactVoices.h"
#include "SampleBank.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Vessel;

namespace audio {

class RocketSynth;
class RcsSynth;
class WindSynth;
class ContactSynth;

/// A vessel's contacts over a time step, summed (ContactSynth's)
struct ContactStep {
	double wheelLoad = 0.0;      // the wheels' normal force [N]
	double squealPower = 0.0;    // the wheels' friction times their slip [W]
	double scrapePower = 0.0;    // the hull's friction times its slip [W]
	double scrapeSpeed = 0.0;    // its slip [m/s]
	double blowWheel = 0.0;      // closing speeds of contacts begun this step [m/s]
	double blowHull = 0.0;
	double blowVessel = 0.0;
	V3 where;                    // the contacts' centre (vessel frame) [m]
	bool any = false;
};

class VesselSounds {
public:
	/// Vessels farther from the camera are not heard [m]
	static constexpr double RANGE = 60e3;

	VesselSounds (Engine &engine, World &world);
	~VesselSounds ();

	/// Once per time step, before the world's update, at time t (the
	/// simulation's, or a replay's: then the air over the vessels from their
	/// states shown, no contacts)
	void Update (double t, bool replay);
	/// A vessel's contact points of a step (Vessel::SoundContacts), to time t, of length h
	void Contacts (const Vessel *v, const std::vector<ContactSample> &pts, SurfaceKind surface, double t, double h);
	/// Docking and undocking (at the vessel's port, vessel frame; speed:
	/// closing, separating [m/s]; mred: the reduced mass [kg])
	void Docked (const Vessel *v, const V3 &port, bool dock, double speed, double mred);
	/// A part broke off (its centre, vessel frame; its mass; struck at hit [m/s])
	void PartBroke (const Vessel *v, const V3 &pos, double mass, double hit);
	/// The vessel was destroyed in an impact
	void Crash (const Vessel *v, double speed, double mass, double energy);

	/// How a vessel sounds, tuned (Config/Sound/<class>.cfg, oapiAudioTune):
	/// factors on its sounds' levels and pitches
	struct Tune {
		double engineLevel = 1.0, enginePitch = 1.0, rumble = 1.0, whine = 1.0, crackle = 1.0;
		double rcsLevel = 1.0, rcsPitch = 1.0, airLevel = 1.0, contactLevel = 1.0;
		/// Set by its key (engine_level, engine_pitch, rumble, whine, crackle,
		/// rcs_level, rcs_pitch, air_level, contact_level); false: no such key
		bool Set (const std::string &key, double value);
	};
	/// Animations with a sound: a motor moving them (AUDIO_ANIM_*)
	enum AnimKind { ANIM_NONE = 0, ANIM_MOTOR = 1, ANIM_DOOR = 2, ANIM_GEAR = 3 };
	bool TuneVessel (const Vessel *v, const std::string &key, double value);
	/// Its own sounds, each on or off (AUDIO_SOUND_*: a module playing its
	/// own in their place); the tune kept
	enum Sound { SOUND_ENGINES = 0, SOUND_RCS = 1, SOUND_AIR = 2, SOUND_CONTACTS = 3, NSOUND };
	bool Enable (const Vessel *v, int sound, bool on);
	void Animation (const Vessel *v, unsigned anim, int kind, double level);
	/// The animation given the gear's sound (-1: none)
	int GearAnim (const Vessel *v) const;
	void Clear ();

	/// Part of a rocket jet's power that becomes sound
	static constexpr double JET_EFFICIENCY = 0.005;
	/// Acoustic power [W] of a rocket jet of thrust F [N] and exhaust speed ve [m/s]
	static double JetPower (double F, double ve)
	{ return JET_EFFICIENCY * 0.5 * std::max (F, 0.0) * std::max (ve, 0.0); }
	/// RMS pressure [Pa] at distance r [m] of a source of power W [W] in
	/// sea-level air, free field (I = W / 4 pi r^2, p^2 = I rho c)
	static double PressureAt (double W, double r)
	{ return std::sqrt (std::max (W, 0.0) * RHO0 * C0 / (4.0 * 3.14159265358979)) / std::max (r, 0.01); }

private:
	struct Cluster {
		std::vector<int> th;       // thruster indices
		double maxth = 0.0;        // their summed rating [N]
		double p0 = 0.0;           // its pressure at full, at its reference distance [Pa] (before its tune)
		EmitterId em;              // (a RocketSynth's, or an RcsSynth's)
	};
	struct PerVessel {
		Vessel *v;                 // (while ref is alive)
		BodyRef ref;
		size_t nth = 0;            // thrusters when the clusters were made
		std::vector<Cluster> engines, rcs;
		EmitterId wind;            // (a WindSynth's)
		EmitterId contact;         // (a ContactSynth's)
		double contactMass = 1.0;  // (its loudness by the vessel's mass, when made)
		ContactStep step;          // this step's contacts (summed until the update)
		ContactModel contacts;     // what its contacts do (the moments, the levels)
		// (ORBITER_AUDIO_CONTACTS=2) its contacts heard outside, through its structure (ContactVoices)
		EmitterId contactAir, contactStruct;
		double contactRef = 3.0;   // the air one's reference distance [m]
		Chirps chirps;             // (which wheels' touchdowns chirp)
		bool bedTakes[ContactVoices::BEDS] = {}; // (its beds' takes given: rolling, braking, scraping, the motors)
		float motorAir[4] = {}, motorCab[4] = {}; // (stage 4: the two motor beds' gain, rate - outside, in the cabin)
		int motorSlot[2] = {-1, -1};   // (the animations playing in them: their animation's index; -1 none)
		Uniforms mechRng{1};       // (the mechanisms' timelines' spread)
		ScrapeBurst scrapeBurst;   // (a scrape's first bite)
		double bedPushT = -1e9;    // [s] its beds' levels last pushed
		double bedLogT = -1e9;     // [s] (ORBITER_AUDIO_LOG states) last logged
		double motorLogT = -1e9;   // [s] (... the motors')
		double lastRattle = -1e9, rattleA = 0.0, lastContactT = -1e9;
		double peakA = 0.0, peakT = 0.0;   // (the cabin's rattle: this blow's peak deceleration, when)
		bool rattleOn = false, rattleFired = false;
		double lastBlow[3] = {-1e9, -1e9, -1e9}; // [s] the last blow of each kind (wheel, hull, vessel)
		int blows[8] = {};         // blows struck so far, by kind (recalled with the travel time)
		float blowStrength[8] = {};
		int heardBlows[8] = {};
		float motor[3] = {};       // motors running (animations), by kind
		bool seen = false;
	};
	PerVessel *Find (const Vessel *v);

	struct AnimSound { unsigned anim; int kind; float level; double last = -1.0; bool moving = false;
		MotorTrack track; double phase = 0.0; };   // (stage 4: its motor's moments; its wobble's phase)
	/// A vessel's configuration: from its class's file, then the API
	struct VesselConfig { BodyRef ref; Tune tune; std::vector<AnimSound> anims; bool on[NSOUND] = {true, true, true, true}; };
	VesselConfig &Config (const Vessel *v);
	std::map<uint64_t, VesselConfig> configs;   // (by the vessel's BodyRef serial)
	void Build (PerVessel &pv);
	void Drop (PerVessel &pv);
	void UpdateVessel (PerVessel &pv);
	double now = 0.0, dtNow = 0.0;  // (this update's time and step)
	bool replaying = false;
	void UpdateContacts (PerVessel &pv, double t);
	/// Its contacts' moments heard (ContactVoices: the blows, the struts, the
	/// cabin's rattle), at time t
	void PlayContacts (PerVessel &pv, const std::vector<ContactSample> &pts, double t);
	/// Its contacts' beds (rolling, braking, scraping: ContactVoices) as heard
	/// now; their takes given once there; silence pushed when it has no contacts
	void UpdateContactBeds (PerVessel &pv);
	/// (stage 4) Its animations' motors: started, running (the two motor
	/// beds), stopped at an end (the lock, the gear's thud, a door's seal)
	void UpdateMotors (PerVessel &pv, VesselConfig &cfg);
	/// (stage 4) A take of a layer at a fixed level in the cabin p [Pa] (its
	/// RMS), on its structure's emitter, and airShare of it outside
	bool PlayFixed (PerVessel &pv, const char *layer, int tier, double p, double airShare, int kind, double tAt, float rate, float cutoff, const V3 &pos);
	/// (stage 4) A blow of a layer by its energy of sound E [J] at a strength
	/// 0..1 (its tier), on both of its emitters
	bool PlayBlow (PerVessel &pv, const char *layer, double strength, double E, int kind, double tAt, const V3 &pos);
	/// Sounds that outlive their vessel (a part torn off, a crash): their own
	/// ContactVoices on the planet, where it happened - the travel time, the
	/// air, as any; removed once surely heard out
	struct Detached { EmitterId em; double until = 0.0; };
	std::vector<Detached> detached;
	/// One for a vessel's event at pos (its frame), heard out by `until` [s]
	/// (the world's time) - a travel time on; null: no planet, no contacts
	Emitter *Detach (PerVessel &pv, const V3 &pos, double dur, const char *what);
	/// A layer's take as a one-shot at energy E [J] of sound (its gain at
	/// refDist r0 [m]) on em, and on em2 (the same take; may be null)
	bool PlayEnergy (Emitter *em, Emitter *em2, const char *layer, double E, double r0, float rate, float cutoff, double tAt, int kind);
	uint32_t rng = 0x9e3779b9u;
	double Rand01 () { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.0 / 16777216.0); }
	SampleBank bank;
	int bankState = 0;             // (0: not read yet, 1: read, -1: none)
	bool ContactBank ();
	/// Its contacts' moments: logged (ORBITER_AUDIO_LOG), then cleared
	void LogContacts (const Vessel *v, ContactModel &m);

	Engine &engine;
	World &world;
	std::vector<std::unique_ptr<PerVessel>> vessels;
	uint32_t seed = 1;
};

} // namespace audio

#endif // !__VESSELSOUNDS_H
