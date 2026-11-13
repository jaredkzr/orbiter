// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sounds of events in the world (not of a vessel's own running):
//
//  - explosions: a vessel breaking up with propellant aboard - its blast
//    wave (BlastSynth), from where it broke up, heard when it arrives
//  - fires: burning wreckage (Debris::Ignite) - the roar of its flames
//    (FireSynth), as loud as a turbulent flame's share of its heat release
//  - water: a vessel entering it (SplashSynth: the slam, the cavity's and
//    the spray's bubbles), moving or floating in it (the wake's bubbles)
//
// Each is an emitter on the planet, at the place it happened, so the world
// carries it as any sound: travel time, spreading, the air's absorption,
// into a cockpit muffled, not at all across vacuum. Its moment is kept in
// the emitter's history (Push): it is heard when the sound from that
// moment arrives.
// =======================================================================

#ifndef __AUDIOEFFECTS_H
#define __AUDIOEFFECTS_H

#include "World.h"
#include <vector>

class Vessel;
class Planet;

namespace audio {

class BlastSynth;
class FireSynth;
class SplashSynth;

class Effects {
public:
	Effects (Engine &e, World &w): engine (e), world (w) {}
	~Effects () { Clear (); }

	/// A vessel breaks up with a mass of propellant [kg] aboard
	void Explosion (const Vessel *v, double propellant);

	/// Each step (t: the simulation's time)
	void Update (double t);
	void Forget (const Body *body);
	void Clear ();

	/// The blasts waiting or sounding (tests)
	size_t Blasts () const { return blasts.size (); }
	size_t Fires () const { return fires.size (); }

private:
	Engine &engine;
	World &world;
	struct Blast {
		Emitter *em = nullptr;
		BlastSynth *synth = nullptr;
		const Planet *planet = nullptr;
		double t0 = 0.0;      // when it went off
		double W = 0.0;       // TNT equivalent [kg]
		double p = 101325.0;  // the air at the source (cinematic: at least a
		double c = 340.3;     // quarter of sea level's)
		double coupling = 1.0; // the world's coupling into the air there
		bool fired = false;
	};
	std::vector<Blast> blasts;
	struct Fire { const Body *debris = nullptr; Emitter *em = nullptr; FireSynth *synth = nullptr; double out = -1.0; };
	std::vector<Fire> fires;
	void UpdateFires (double t);
	struct Water {
		const Vessel *v = nullptr; Emitter *em = nullptr; SplashSynth *synth = nullptr;
		bool wet = false; double dryAt = -1e10; int entries = 0, heard = 0; double lastNear = 0.0;
		double vDown = 0.0, vAlong = 0.0; // (the step before)
	};
	std::vector<Water> waters;
	void UpdateWater (double t);
	uint32_t seed = 101;
};

} // namespace audio

#endif // !__AUDIOEFFECTS_H
