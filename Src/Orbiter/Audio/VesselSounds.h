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

/// A vessel's contacts over a time step (from the contact solver)
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
	/// A vessel's contacts of the step (Vessel::ContactsResolved)
	void Contacts (const Vessel *v, const ContactStep &c);
	/// Docking and undocking (at the vessel's port, vessel frame)
	void Docked (const Vessel *v, const V3 &port, bool dock);

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
	void Animation (const Vessel *v, unsigned anim, int kind, double level);
	/// The animation given the gear's sound (-1: none)
	int GearAnim (const Vessel *v) const;
	void Forget (const Body *body);
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
		Emitter *em = nullptr;
		RocketSynth *rocket = nullptr;
		RcsSynth *rcs = nullptr;
	};
	struct PerVessel {
		Vessel *v;
		size_t nth = 0;            // thrusters when the clusters were made
		std::vector<Cluster> engines, rcs;
		Emitter *wind = nullptr;
		WindSynth *windSynth = nullptr;
		Emitter *contact = nullptr;
		ContactSynth *contactSynth = nullptr;
		ContactStep step;          // this step's contacts (summed until the update)
		double lastBlow[3] = {-1e9, -1e9, -1e9}; // [s] the last blow of each kind (wheel, hull, vessel)
		int blows[8] = {};         // blows struck so far, by kind (recalled with the travel time)
		float blowStrength[8] = {};
		int heardBlows[8] = {};
		float motor[3] = {};       // motors running (animations), by kind
		bool rebuild = false;      // its tune changed
		bool seen = false;
	};
	PerVessel *Find (const Vessel *v);

	struct AnimSound { unsigned anim; int kind; float level; double last = -1.0; bool moving = false; };
	/// A vessel's configuration: from its class's file, then the API
	struct VesselConfig { Tune tune; std::vector<AnimSound> anims; };
	VesselConfig &Config (const Vessel *v);
	std::map<const Vessel*, VesselConfig> configs;
	void Build (PerVessel &pv);
	void Drop (PerVessel &pv);
	void UpdateVessel (PerVessel &pv);
	double now = 0.0, dtNow = 0.0;  // (this update's time and step)
	bool replaying = false;
	void UpdateContacts (PerVessel &pv, double t);

	Engine &engine;
	World &world;
	std::vector<std::unique_ptr<PerVessel>> vessels;
	uint32_t seed = 1;
};

} // namespace audio

#endif // !__VESSELSOUNDS_H
