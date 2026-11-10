// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The splash, spray and wake of a body in the water (Water W9), from the
// core's water state (VESSEL::GetWaterState, oapiGetDebrisWaterState):
// - a burst for each blow of an entry: a crown of spray around the footprint,
//   a column thrown up by a steep one, sheets thrown out to the sides by a
//   fast shallow one; rings and foam on the water (WaterFx)
// - moving through the water: sheets of spray off both sides of the wetted
//   footprint, a rooster tail behind it when fast, the wake (WaterFx)
// All of it scaled by the body's size (its radius; the footprint from the
// wetted and waterplane areas) and its speed: how much water is thrown, how
// big its clumps are, how high and how far it flies.
// =======================================================================

#ifndef __WATERSPRAY_H
#define __WATERSPRAY_H

#include "Particles.h"

class Scene;

class WaterSpray {
public:
	/// A frame: the body (global position gpos, velocity vel; radius size [m])
	/// in the water of planet hp, as its state ws says (not wet, or at rest
	/// afloat: nothing). key: the body's (its wake's WaterFx source)
	void Update (Scene *scene, OBJHANDLE hp, const WATERSTATE &ws, const VECTOR3 &gpos, const VECTOR3 &vel,
		double size, double camDist, uint64_t key);
	/// the live rings drawn (VObject::DrawParticles)
	void Draw (Scene *scene) const;
	/// the body's visual gone: the rings retired (their particles live on)
	void Release (ParticleSystem *sys);

	static constexpr double RANGE = 8000.0; ///< [m] no spray from farther from the camera

private:
	struct Ring : ParticleRing { double accum = 0.0; };
	Ring crown, column, sheet, tail;
	VECTOR3 prev{};          // the footprint's centre on the water last frame (global)
	bool havePrev = false;
	double entry = 0.0, tWet = -1e10; // the last entry's blow so far [m/s]; when it began [s]

	struct Frame;
	void Burst (const Frame &f, double v);
	void Sheet (const Frame &f, double s);
	/// ring r made ready for rate [1/s] particles living life [s], or a
	/// burst of that many; false: no room
	static bool Reserve (ParticleSystem *sys, Ring &r, double rate, double life, uint32_t burst);
};

#endif // !__WATERSPRAY_H
