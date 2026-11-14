// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The ideal drive's sound (the Relativistic Travel MFD's trip autopilot,
// RelTravel.h), heard in the cockpit: a hum spooling up as it engages, a
// rising sweep as the first burn begins and a falling one as the braking
// burn does, a steady hum while it burns, winding down as it disengages;
// and a beat later, the crew says so ("Drive engaged.", "Braking burn.").
// Physical sound: in the cockpit only (outside, vacuum); cinematic: in the
// outside views too. Time acceleration fades it as it does every sound.
// Generated sounds (Sound/Generated/warp_*.flac, Utils/Audio/make_sfx.py).
// =======================================================================

#ifndef __AUDIOWARPDRIVE_H
#define __AUDIOWARPDRIVE_H

#include "World.h"

class Vessel;

namespace audio {

class System;

class WarpDrive {
public:
	explicit WarpDrive (System &s): sys (s) {}
	/// Each step: the vessel in focus, the camera in its cockpit
	void Step (const Vessel *focus, bool cockpit, double dt);
	void Clear ();

private:
	System &sys;
	BodyRef vessel;           // (the vessel followed)
	int prevPhase = -1;
	int hum = 0;              // the burn's hum (a handle)
	int humBus = -1;          // (the bus it plays on)
	float humLevel = 0.0f;    // (faded in and out)
	const char *line = nullptr; // the crew's word on it, a beat after its sound
	double lineIn = 0.0;        // [s] (until then)
	void Play (int bus, const char *file, double level);
};

} // namespace audio

#endif // !__AUDIOWARPDRIVE_H
