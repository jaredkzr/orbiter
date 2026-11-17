// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Sounds of events in the world (not of a vessel's own running):
//
//  - explosions: a vessel breaking up with propellant aboard - its blast
//    wave (BlastSynth), from where it broke up, heard when it arrives
//  - fires: burning wreckage (Debris::Ignite) - the roar of its flames
//    (FireSynth), as loud as a turbulent flame's share of its heat release
//  - with the contacts' recordings (ORBITER_AUDIO_CONTACTS=2, a manifest:
//    VesselSounds::Bank), the aftermath from them instead: a blast's boom
//    (an `explosion` take and its echo, under the blast's onset), each
//    piece's flames (a looped `fire` bed in place of FireSynth), then its
//    smoulder (the bed low and dulled) and hot metal ticking as it cools
//    (`tick`), and each piece landing (`debris_land`; else `clatter`,
//    `impact_metal`) - the laws in Contacts.h (BoomHeard, FireBedFor,
//    SmoulderLevel, TickRate, DebrisLands, DebrisLandFor, DebrisGate)
//  - thunder: a lightning flash (oapiAudioLightning: the graphics client's
//    storms) - its channel's N-waves (ThunderSynth, Few's model)
//  - the structure creaking under load (CreakSynth): the vessel in focus,
//    in its cabin, subtle
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
#include "Contacts.h"
#include "Vecmat.h"
#include <vector>

class Vessel;
class Planet;
class Debris;

namespace audio {

class BlastSynth;
class FireSynth;
class SplashSynth;
class ThunderSynth;
class CreakSynth;
class VesselSounds;
class SampleBank;

class Effects {
public:
	/// vs: the contacts' recordings (VesselSounds::Bank) for the aftermath of
	/// a blast and a wreck - the boom, fires, smoulder, ticks, debris landing;
	/// without them (null, or none there) the synthesized blast and fires
	Effects (Engine &e, World &w, VesselSounds *vs = nullptr): engine (e), world (w), sounds (vs) {}
	~Effects () { Clear (); }

	/// A vessel breaks up with a mass of propellant [kg] aboard
	void Explosion (const Vessel *v, double propellant);
	/// A lightning flash in a planet's air: its channel from `start` along
	/// `dir` (global frame), `length` [m], its energy per length [J/m], its
	/// strokes over `duration` [s]
	void Lightning (const Planet *pl, const Vector &start, const Vector &dir, double length, double El, int strokes, double duration, uint32_t seed);

	/// Each step (t: the simulation's time)
	void Update (double t);
	void Clear ();

	/// The blasts waiting or sounding (tests)
	size_t Blasts () const { return blasts.size (); }
	size_t Fires () const;
	/// (tests) the pieces of wreckage heard from recordings (fire, smoulder,
	/// ticks, landing)
	size_t Wrecks () const { return wrecks.size (); }

private:
	Engine &engine;
	World &world;
	VesselSounds *sounds = nullptr;
	SampleBank *Bank () const;
	struct Blast {
		EmitterId em;         // (a BlastSynth's, on the planet: gone with it)
		const Planet *planet = nullptr;
		double t0 = 0.0;      // when it went off
		double W = 0.0;       // TNT equivalent [kg]
		double p = 101325.0;  // the air at the source (cinematic: at least a
		double c = 340.3;     // quarter of sea level's)
		double coupling = 1.0; // the world's coupling into the air there
		bool fired = false;
		EmitterId boom;       // (the recordings': its boom and echo, a ContactVoices on the planet)
		double boomLen = 0.0; // [s] the boom and its echo
		double firedT = 0.0;
	};
	std::vector<Blast> blasts;
	struct Boom { EmitterId em; double until = 0.0; };
	std::vector<Boom> booms;      // (heard on after their blast's synth is done)
	struct Fire { BodyRef debris; EmitterId em; double out = -1.0; };
	std::vector<Fire> fires;
	void UpdateFires (double t);
	/// The wreckage from recordings: each piece's state, an emitter on it once
	/// it burns or lands (removed when it's gone or long quiet)
	struct Wreck {
		BodyRef debris;
		EmitterId em;
		float refDist = 2.0f;
		double prevImpact = 0.0, lastLand = -1e9, lastEvent = -1e9;
		bool burned = false;     // it caught fire (its smoulder, ticks after)
		double fireP = 0.0;      // [Pa RMS at refDist] its flames at full
		double outT = -1.0;      // its flames out (the propellant burned)
		double nextTick = -1.0;
		bool takes = false;      // (its beds' takes given)
		int ticks = 0, lands = 0;
	};
	std::vector<Wreck> wrecks;
	DebrisGate debrisGate;
	void UpdateWrecks (double t, SampleBank &bank);
	Emitter *WreckEmitter (Wreck &w, const Debris *d, float refDist);
	uint32_t wreckRng = 0x6c8e9cf5u;
	double WreckRand () { wreckRng ^= wreckRng << 13; wreckRng ^= wreckRng >> 17; wreckRng ^= wreckRng << 5; return (wreckRng >> 8) * (1.0 / 16777216.0); }
	struct Water {
		BodyRef v; EmitterId em;
		bool wet = false; double dryAt = -1e10; int entries = 0, heard = 0; double lastNear = 0.0;
		double vDown = 0.0, vAlong = 0.0; // (the step before)
	};
	std::vector<Water> waters;
	struct Thunder { EmitterId em; double t0 = 0.0; bool fired = false; };
	std::vector<Thunder> thunders;
	struct Creak { BodyRef v; EmitterId em; double stress = -1.0, debt = 0.0, t = 0.0; int count[2] = {}, heard[2] = {};
		EmitterId bed; } creak;   // (bed: a black hole's tides near tearing it - a structure groaning, looped)
	void UpdateCreaks (double t);
	uint32_t creakRng = 0x2545f491u;
	double CreakRand () { creakRng ^= creakRng << 13; creakRng ^= creakRng >> 17; creakRng ^= creakRng << 5; return (creakRng >> 8) * (1.0 / 16777216.0); }
	void UpdateWater (double t);
	uint32_t seed = 101;
};

} // namespace audio

#endif // !__AUDIOEFFECTS_H
