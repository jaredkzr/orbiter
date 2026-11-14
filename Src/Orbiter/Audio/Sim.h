// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The simulation as the sound sees it: the only input of the world (World)
//
//  - Snapshot: plain data, taken once per step - the clocks, the listener
//    (the camera: where, which way, in a cockpit of which structure), the
//    frame of the air (the planet whose air the listener is in or near:
//    positions are taken in it, the air moving with it)
//  - Scene: the bodies the sound is on, asked for as needed - where each
//    is, which structure it belongs to, its size; the air of the frame's
//    planet at a place
//
// Orbiter's (its globals: the camera, the bodies, the planets' atmospheres)
// is read in one place, OrbiterScene; tests give their own.
// =======================================================================

#ifndef __AUDIOSIM_H
#define __AUDIOSIM_H

#include "Medium.h"
#include "Track.h"

class Body;

namespace audio {

/// Where something is and how it is turned: global frame; rot row-major,
/// its own frame to the global one (Orbiter's GRot)
struct Pose {
	V3 pos;
	double rot[9] = {1,0,0, 0,1,0, 0,0,1};
};

struct Snapshot {
	double t = 0.0;               ///< the simulation's time (or a replay's) [s]: sound's travel
	double playT = 0.0;           ///< the clock of what is heard [s]: real time while the simulation's
	                              ///< sounds are (held in a pause, above WARP_MUTE): a boom's length, the voice cap
	double dt = 0.0;              ///< the step's real length [s]: till the next snapshot (levels ramp over it)
	bool listener = false;        ///< there is one (a camera)
	Pose ear;                     ///< the listener (its frame: x right, y up, z ahead)
	bool inside = false;          ///< in a cockpit ...
	const void *insideOf = nullptr; ///< ... of this structure (Scene::StructureOf)
	const void *frame = nullptr;  ///< the air's frame (a planet; null: none - the global frame, no air)
	Pose framePose;               ///< its pose
	Air air;                      ///< the air at the listener
};

class Scene {
public:
	virtual ~Scene () = default;
	/// Where a body is (false: no such body now)
	virtual bool PoseOf (const Body *b, Pose &p) const = 0;
	/// The structure it belongs to (vessels docked or attached together: one)
	virtual const void *StructureOf (const Body *b) const = 0;
	/// Its size (radius) [m]
	virtual double SizeOf (const Body *b) const = 0;
	/// The air of the frame's planet at a global position (none: beyond its
	/// atmosphere, or no frame)
	virtual Air AirAt (const void *frame, const V3 &global) const = 0;
};

} // namespace audio

#endif // !__AUDIOSIM_H
