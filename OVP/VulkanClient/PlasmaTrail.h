// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Reentry afterglow: the ionised air left along a reentering body's path,
// recombining for a few seconds - a faint streak kilometres long that shows
// a reentry from afar at night.
//
// Drawn as a ribbon: the body lays points along its path (at rest in the
// air from then on); each frame the points are joined into a strip facing
// the camera, widening and cooling (from the sheath's colour to violet) and
// fading with their age; streaks along it (plasma_trail.frag). Unbroken
// whatever the frame rate, the distance or the particle pool's state (the
// particles it replaces were spaced along the path - beads, from afar).
// Additive, with the reactive mask. A body gone (out of range, burnt up),
// its trail is kept until it has faded.
// =======================================================================

#ifndef __PLASMATRAIL_H
#define __PLASMATRAIL_H

#include "VkContext.h"
#include "OrbiterAPI.h"
#include <deque>
#include <unordered_map>

class PlasmaTrails {
public:
	/// a point of a trail where its body is now
	struct Lay {
		VECTOR3 pos;        // (global)
		VECTOR3 airVel;     // the air's velocity there (global): the point moves with it
		double speed;       // the body's speed through the air [m/s]
		double simt;
		float width;        // [m] across, laid
		float growth;       // [m/s] its widening
		float life;         // [s]
		float alpha;        // radiance at its centre, laid (times its colour)
		float col[3];       // its colour, laid (cooling to violet as it ages)
		float fadeIn;       // [m] along it from the body: fading in over this
	};
	bool Init (vkc::Context *ctx, VkDescriptorSetLayout frameLayout);
	void Shutdown ();
	/// the trail id's point where its body is now (id 0: a new trail); returns
	/// its id
	uint32_t Add (uint32_t id, const Lay &l);
	/// its body gone: the trail drawn until it has faded
	void Release (uint32_t id);
	/// the trails (scene with the reactive mask bound)
	void Draw (VkDescriptorSet frameSet, const VECTOR3 &camPos, const MATRIX3 &camRot);
	/// points drawn the last frame
	uint32_t Count () const { return count; }
	bool enabled = true;

private:
	struct Point {
		VECTOR3 pos, vel;   // global at t0, the air's velocity
		double t0;
		double along;       // [m] along the trail from its start
		float w0, growth, life, alpha, col[3];
		bool brk;           // not joined to the point before
	};
	struct Trail {
		std::deque<Point> pts; // (the last: the body's - moved with it while laid)
		double lastLay = -1e30;
		float fadeIn = 0.0f, unit = 1.0f;
		bool orphan = false;
	};
	std::unordered_map<uint32_t, Trail> trails;
	uint32_t nextId = 1;

	vkc::Context *ctx = nullptr;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	uint32_t count = 0;
};

#endif // !__PLASMATRAIL_H
