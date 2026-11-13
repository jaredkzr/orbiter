// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// What surrounds the listener:
//
//  - the cabin (the camera in the focus vessel's cockpit): its air
//    handling - fans, ducts - at a crewed spacecraft's level (some 56 dB:
//    the ISS 50-60 dBA, the Shuttle's middeck 60-65), XRSound's recording
//    looped; now and then a pump or valve (XRSound's cabin ambience,
//    every 7-35 s, quieter)
//  - outdoors, at rest near the ground (within 2 km, slower than 30 m/s
//    over it): the wind at the ear and the sea nearby (AmbienceSynth),
//    from the planet's wind model and its sea; from a cockpit muffled as
//    any sound from outside
// =======================================================================

#ifndef __AMBIENCE_H
#define __AMBIENCE_H

#include "PathNode.h"
#include "AudioEngine.h"
#include "Vecmat.h"
#include <memory>

namespace audio {

class System;
class AmbienceSynth;

class Ambience {
public:
	explicit Ambience (System &s);
	~Ambience ();
	/// Each step (t: simulation time; dt: the step)
	void Update (double t, double dt);
	void Clear ();

	/// The cabin's level [Pa RMS]
	static constexpr double CABIN_P = 0.0126;   // (56 dB)
	static constexpr double CABIN_EVENT_P = 0.0063; // (50 dB)

	/// (tests: the outdoor sound's pressure now [Pa], wind and sea)
	double WindP () const { return windP; }
	double SeaP () const { return seaP; }

private:
	System &sys;
	int cabin = 0;              // the cabin's loop (a handle)
	int cabinEvent = 0;
	double nextEvent = -1.0;
	uint32_t rng = 0x6a09e667u;
	double Rand () { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.0 / 16777216.0); }

	std::unique_ptr<PathNode> node;
	std::unique_ptr<AmbienceSynth> synth;
	double windP = 0.0, seaP = 0.0;
	Vector lastLoc;
	double lastT = -1.0;
	bool Init ();
};

} // namespace audio

#endif // !__AMBIENCE_H
