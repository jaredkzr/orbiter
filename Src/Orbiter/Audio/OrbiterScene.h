// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Orbiter as the sound's scene (Sim.h): the camera, the bodies and the
// planets' air, read from the simulation - the one place the world's
// input comes from Orbiter's globals.
// =======================================================================

#ifndef __AUDIOORBITERSCENE_H
#define __AUDIOORBITERSCENE_H

#include "Sim.h"

namespace audio {

class OrbiterScene : public Scene {
public:
	/// This step's snapshot (t: the simulation's time, or the replay's)
	/// (playT: the clock of what is heard - Snapshot::playT)
	Snapshot Take (double t, double playT) const;

	bool PoseOf (const Body *b, Pose &p) const override;
	const void *StructureOf (const Body *b) const override;
	double SizeOf (const Body *b) const override;
	Air AirAt (const void *frame, const V3 &global) const override;
};

} // namespace audio

#endif // !__AUDIOORBITERSCENE_H
