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
//
// In the cockpit: engines and thrusters through the structure (also in
// vacuum), the air through the hull. Vessels within RANGE of the camera.
// =======================================================================

#ifndef __VESSELSOUNDS_H
#define __VESSELSOUNDS_H

#include "World.h"
#include <algorithm>
#include <cmath>
#include <memory>
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

	/// Once per time step, before the world's update
	void Update ();
	/// A vessel's contacts of the step (Vessel::ContactsResolved)
	void Contacts (const Vessel *v, const ContactStep &c);
	/// Docking and undocking (at the vessel's port, vessel frame)
	void Docked (const Vessel *v, const V3 &port, bool dock);
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
		int blows[5] = {};         // blows struck so far, by kind (recalled with the travel time)
		float blowStrength[5] = {};
		int heardBlows[5] = {};
		bool seen = false;
	};
	PerVessel *Find (const Vessel *v);
	void Build (PerVessel &pv);
	void Drop (PerVessel &pv);
	void UpdateVessel (PerVessel &pv);
	void UpdateContacts (PerVessel &pv, double t);

	Engine &engine;
	World &world;
	std::vector<std::unique_ptr<PerVessel>> vessels;
	uint32_t seed = 1;
};

} // namespace audio

#endif // !__VESSELSOUNDS_H
