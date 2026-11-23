// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The frame's clock and frames of reference, sampled once a frame at the
// end of the step (Scene::UpdateCamera), and the "carry with the ground":
// how a point at rest on the ground nearby moves from one sample to the
// next, in camera-relative coordinates.
//
// The convention (repeated in particles.glsl):
// 1. One time per frame. Everything read from the core in clbkUpdate and
//    clbkRenderScene is for the end of the step simulated for that frame
//    (t_n). Anything older is named ...Prev (or stored body-fixed).
// 2. Frames.
//    - G, Orbiter's global frame: heliocentric (barycentric), so the ground
//      moves through it at tens of km/s (1.5 km in a 0.05 s step near
//      Earth); its origin may jump (Orbiter::UpdateOrigin). Never keep a G
//      position across frames without the Snapshot that it belongs to.
//    - B, the camera's proxy body's rotating frame (oapiGlobalToLocal):
//      persistent world things live here.
//    - C_n: G axes, relative to the camera at frame n (GPU positions).
//    - V_n: view space, C_n turned by the camera's rotation.
// 3. GPU particles. Between frames their positions are in C_{n-1} at
//    t_{n-1}, their velocities in G. The update carries them first (with
//    the ground, CarryBetween); from then on, in every later pass, their
//    positions are in C_n at t_n.
// 4. Emissions. p0 is the source at t_{n-1}, p1 at t_n, both in G, with
//    vel in G. A segment at a single time goes through
//    ParticleSystem::Emission::Segment (p0 = a - vel dt).
// 5. Cut and jump tests use motion relative to the ground (|T|) or to the
//    camera's target, never motion in G.
// =======================================================================

#ifndef __FRAMESTATE_H
#define __FRAMESTATE_H

#include "OrbiterAPI.h"
#include <cstdint>

namespace vkframe {

/// The pose of the camera's proxy body (G, at the sample's time)
struct BodyPose {
	OBJHANDLE h = nullptr;
	VECTOR3 pos{}, vel{};   ///< centre, velocity
	MATRIX3 rot{};          ///< body -> global axes
	VECTOR3 omega{};        ///< angular velocity (global axes; the particles' convention: ground velocity = vel + (x - pos) x omega)
	double radius = 0.0;
};

/// The camera and its frames at one instant
struct Snapshot {
	bool valid = false;
	double t = 0.0;         ///< simulation time [s]
	VECTOR3 cam{};          ///< camera position (G)
	MATRIX3 camRot{};       ///< camera -> global axes (shake included)
	BodyPose body;
	VECTOR3 origin{};       ///< G's origin relative to the system's barycentre (oapiGetGlobalOrigin)
	uint32_t epoch = 0;     ///< discontinuities (time jumps) before it: none between two of the same epoch
};

/// Sample the core's state now (camera at cam, rot)
Snapshot Sample (const VECTOR3 &cam, const MATRIX3 &camRot, uint32_t epoch);

/// The frame points are carried in from snapshot a to snapshot b
enum class CarryFrame : int {
	Global = 0, ///< G (no body, another body, or a discontinuity): the old camera-relative shift
	Body = 1,   ///< moving with the body's centre, not turning (far from it)
	Ground = 2  ///< turning with the body: at rest on its ground (and in its still air)
};

/// A camera-relative position at a (C_a) carried to b (C_b): x_b = Q x_a + T
struct Carry {
	CarryFrame frame = CarryFrame::Global;
	bool continuous = false; ///< no discontinuity between them
	VECTOR3 T{};             ///< translation [m]
	MATRIX3 Q{};             ///< rotation (R_b R_a^T; identity unless Ground)
	VECTOR3 v{};             ///< Q's rotation as sin (angle) * axis: Q x = x + v x x + v x (v x x) / (1 + cos (angle))
	VECTOR3 shift{};         ///< a.cam - b.cam, both in b's G: a point at rest in G
	VECTOR3 D0{};            ///< T - shift: the frame's displacement over the interval (G) at a's camera
	double dist = 0.0;       ///< |T|: the camera's move relative to the frame (cut and reuse tests)
	VECTOR3 Apply (const VECTOR3 &x) const { return mul (Q, x) + T; }
};

/// How a point moves from a to b if at rest in the frame chosen: the
/// ground's when both have the same body and b's camera is within
/// GROUND_RANGE of its radii, its centre's beyond, G otherwise
Carry CarryBetween (const Snapshot &a, const Snapshot &b);
/// the frame CarryBetween picks
CarryFrame FrameOf (const Snapshot &a, const Snapshot &b);
constexpr double GROUND_RANGE = 2.0;

/// The frame's state: this frame's snapshot and the last one's
class FrameState {
public:
	/// a new frame (prev = the current one, cur = now)
	void Advance (const VECTOR3 &cam, const MATRIX3 &camRot);
	/// a time jump or similar: nothing carried across, no history
	void MarkDiscontinuity () { epoch++; }
	const Snapshot &Now () const { return cur; }
	const Snapshot &Prev () const { return prev; }
	uint32_t Epoch () const { return epoch; }
	bool Continuous () const { return prev.valid && cur.valid && prev.epoch == cur.epoch; }
	double Dt () const { return prev.valid ? cur.t - prev.t : 0.0; }
	/// the last frame's camera-relative positions carried to this one's
	Carry FrameCarry () const { return CarryBetween (prev, cur); }
	/// |T| of FrameCarry: the camera's move relative to the ground
	double CameraGroundMove () const { return FrameCarry ().dist; }
	/// the velocity of the ground (and still air) at a G position now
	VECTOR3 GroundVel (const VECTOR3 &g) const;
	/// a G position of the last snapshot, at rest in the carry frame
	/// (CarryBetween's: on the ground nearby), where it is now (G now)
	VECTOR3 CarryPrevToNow (const VECTOR3 &g) const;
	/// the carry frame's velocity at a G position now (the ground's, the
	/// body's centre's, or 0)
	VECTOR3 FrameVel (const VECTOR3 &g) const;
	/// G now <-> the body's frame (relative to its centre, its axes)
	VECTOR3 ToBody (const VECTOR3 &g) const { return tmul (cur.body.rot, g - cur.body.pos); }
	VECTOR3 FromBody (const VECTOR3 &b) const { return cur.body.pos + mul (cur.body.rot, b); }
	/// a G position of the last snapshot in this one's G (an origin shift
	/// between them taken out)
	VECTOR3 PrevToNowG (const VECTOR3 &g) const { return g - (cur.origin - prev.origin); }
private:
	Snapshot cur, prev;
	uint32_t epoch = 0;
};

} // namespace vkframe

#endif // !__FRAMESTATE_H
