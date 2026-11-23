// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: ageing and motion (drag towards the air, turbulence,
// rising hot smoke), the shift to the new camera position, and collisions
// of smoke with the ground, bases and vessels: it slides along them
// (included by particle_update.comp and particle_update_rt.comp)
//
// The particles are carried with the ground to this frame first (CarryPrev
// below; FrameState.h, particle_common.glsl): the air, the noise, the ground jets,
// the planet and the ray tracing scene are all given for the end of the
// step, and only a particle's motion relative to the ground is left - its
// collisions are looked for along that (a few metres), not along its
// motion in the global frame (a kilometre and a half in a 0.05 s step).

layout(local_size_x = 64) in;
const float VOL_REACH_M = 20000.0; // (smokevol.glsl VOL_FAR)

#include "particle_common.glsl"

#ifdef RT
// the nearest surface of the instances in mask along o + t d (view space),
// t in [tMin, tMax]: distance, normal (either side), and the owner and its
// velocity (global frame; terrain: 0; w: 1 at rest on the ground - a
// planet's bases: the ground's velocity there, not the planet's centre's)
bool RTStrike (vec3 o, vec3 d, float tMin, float tMax, uint mask, out float t, out vec3 N, out uint owner, out vec4 vel)
{
	if (!RayFinite (o, d, tMax)) return false;
	rayQueryEXT rq;
	rayQueryInitializeEXT (rq, tlas, gl_RayFlagsOpaqueEXT, mask, o, tMin, d, tMax);
	while (rayQueryProceedEXT (rq)) {}
	if (rayQueryGetIntersectionTypeEXT (rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
	t = rayQueryGetIntersectionTEXT (rq, true);
	RTInstance r = inst[rayQueryGetIntersectionInstanceCustomIndexEXT (rq, true) + rayQueryGetIntersectionGeometryIndexEXT (rq, true)];
	owner = r.owner, vel = r.vel;
	uint prim = uint (rayQueryGetIntersectionPrimitiveIndexEXT (rq, true));
	vec3 v[3];
	RTFloats f = RTFloats (r.vtx);
	for (int k = 0; k < 3; k++) {
		uint vi = RTIndex (r.idx, r.firstIndex + prim * 3u + uint (k)) * 8u;
		v[k] = vec3 (f.f[vi], f.f[vi + 1u], f.f[vi + 2u]);
	}
	N = normalize (mat3 (rayQueryGetIntersectionObjectToWorldEXT (rq, true)) * cross (v[1] - v[0], v[2] - v[0]));
	return true;
}

// the nearest terrain along o + t d (view space), within tMax: distance and
// normal (bases and vessels are left out: smoke deflected into a launch
// pad's flame trench would be trapped under the deck, which in reality it
// leaves through the trench's exits)
bool RTSurface (vec3 o, vec3 d, float tMax, vec3 up, out float t, out vec3 N)
{
	uint owner;
	vec4 vel;
	if (!RTStrike (o, d, 0.0, tMax, 0x02u, t, N, owner, vel)) return false;
	if (dot (N, up) < 0.0) N = -N; // (the upper side)
	return true;
}
#endif

// the carry (particles.glsl frameCarry): Q x - x, Q's turn of a point x
// (Rodrigues with v = sin (angle) axis: v x x + v x (v x x) / (1 + cos))
vec3 CarryTurn (vec3 x)
{
	vec3 v = frameCarry[1].xyz, vx = cross (v, x);
	return vx + cross (v, vx) / (1.0 + sqrt (max (1.0 - dot (v, v), 0.0)));
}
// a position relative to the last update's camera at its time (C_{n-1})
// carried with the frame (the ground nearby) to this one's (C_n, t_n)
vec3 CarryPrev (vec3 x) { return x + frameCarry[0].xyz + CarryTurn (x); }
// the frame's displacement over the step (global) at x (C_{n-1}): where a
// point at rest in it went (a particle's own motion over the step, vel dt,
// less this: its motion relative to the ground)
vec3 FrameMove (vec3 x) { return frameCarry[2].xyz + CarryTurn (x); }

// the height of a camera-relative point above the ground under the camera
// (a sphere): the camera's (camGround, from doubles) and the rest exact to
// millimetres - the large numbers only in the denominator. (Its distance
// from the centre less the ground's radius, both some 6.4e6 m in floats,
// resolved 0.5 m on Earth, 8 m at Jupiter's: dust resting on the ground
// jittered as the camera moved)
float GroundHeight (vec3 x)
{
	float rc = length (pc.bodyPos.xyz);
	vec3 u = -pc.bodyPos.xyz / rc; // (the camera's up)
	return camGround.x + (2.0 * rc * dot (x, u) + dot (x, x)) / (rc + length (x - pc.bodyPos.xyz));
}

// the velocity of the air at pos (camera-relative, global frame): the
// planet's, turning with it, and its wind
vec3 AirVel (vec3 pos) { return AirVelocity (pos, pc.bodyPos, pc.bodyVel.xyz, pc.omega.xyz); }

// the velocity of the ground (and the air) at pos (camera-relative): the
// planet's, turning with it
vec3 GroundVel (vec3 pos)
{
	return pc.bodyVel.xyz + cross (pos - pc.bodyPos.xyz, pc.omega.xyz);
}

// smoke or dust meeting the ground (normal Ng, global axes) at pos: its
// motion into it stopped and its motion along it slowed by friction, both
// in the ground's frame (the global frame moves at tens of km/s: there a
// fraction of the velocity, or its part along the normal, is a kick of
// kilometres per second, which threw smoke touching the ground a kilometre
// into the sky)
vec3 SlideOnGround (vec3 pos, vec3 vel, vec3 Ng, bool ballistic, bool dust)
{
	vec3 vg = GroundVel (pos);
	vec3 rel = vel - vg;
	float into = dot (rel, Ng);
	if (into < 0.0) rel -= Ng * into;
	// (friction; dust in air the least: carried along the ground by a jet's
	// outflow (GroundJetWind), at 0.85 a step it parked on the runway)
	return vg + rel * (ballistic ? 0.5 : dust ? 0.97 : 0.85);
}

// a speck (fragment, spark) that strikes a surface in this step bounces off
// it, in the surface's frame (vessels, debris and the ground move at
// kilometres per second in the global frame): its velocity across the
// surface, less some by friction, and some of that into it, reversed.
// pos: where it is, carried with the ground (CarryPrev), frameMove: the
// ground's move over the step there (FrameMove), vel: its velocity; true
// if it struck (pos, vel: after the bounce)
bool Bounce (inout vec3 pos, inout vec3 vel, float dt, float gap, vec3 frameMove)
{
	const float RESTITUTION = 0.35, FRICTION = 0.25;
	vec3 vs; // the surface's velocity
	vec3 Ng; // its normal (global axes), against the speck's motion
	vec3 hit;
#ifdef RT
	// (a first look in the ground's frame; what it strikes, looked at again
	// in that one's own frame - a vessel's, a piece of debris')
	mat3 toView = transpose (mat3 (frame.viewToGlobal));
	vec3 pos0 = pos - frameMove; // (where it was: the step's start, camera-relative now)
	vec3 ref = frameMove;        // (the reference frame's move over the step)
	float t;
	vec3 N;
	uint owner;
	bool struck = false;
	for (int k = 0; k < 2; k++) {
		vec3 o = pos0 + ref, m = vel * dt - ref;
		float len = length (m);
		if (len < 1e-5) return false;
		vec4 vo;
		if (!RTStrike (toView * o, toView * (m / len), 0.01, len + gap, 0x03u, t, N, owner, vo)) return false;
		vs = owner != 0u && vo.w < 0.5 ? vo.xyz : GroundVel (o + m * (t / len)); // (terrain, bases: the ground's)
		hit = o + m * (t / len);
		Ng = mat3 (frame.viewToGlobal) * N;
		if (dot (Ng, m) > 0.0) Ng = -Ng;
		// (the first look was in that frame, close enough: struck)
		if (length (vs * dt - ref) < 0.25 * gap + 0.01) { struck = true; break; }
		ref = vs * dt;
	}
	if (!struck) return false;
	pos = hit + Ng * gap;
#else
	// the ground under the camera, as a sphere
	vec3 e = pos + vel * dt - frameMove; // (where it ends the step)
	float h = GroundHeight (e);
	if (pc.ground.x <= 0.0 || h >= gap) return false;
	Ng = normalize (e - pc.bodyPos.xyz);
	vs = GroundVel (pos);
	pos = e + Ng * (gap - h);
#endif
	vec3 rel = vel - vs;
	float into = dot (rel, Ng);
	if (into >= 0.0) return false;
	vec3 across = rel - Ng * into;
	vel = vs + across * (1.0 - FRICTION) - Ng * (into * RESTITUTION);
	return true;
}

void main ()
{
	uint i = ParticleSlot (gl_GlobalInvocationID.x);
	if (i == ~0u) return;
	Particle p = particles[i];
	if (p.velLife.w <= 0.0) return;
	float dt = pc.camShift.w;
	p.posAge.w += dt;
	if (p.posAge.w >= p.velLife.w) { particles[i].velLife.w = 0.0; return; }

	// (carried with the ground to the step's end, relative to the camera now:
	// every evaluation below is at t_n; frameMove the ground's move there)
	vec3 pos = CarryPrev (p.posAge.xyz);
	vec3 frameMove = FrameMove (p.posAge.xyz);
	vec3 vel = p.velLife.xyz;
	// (a trail's smoke, long-lived and large: carried by the wind, with
	// gentle eddies)
	bool trail = (p.flags & PF_TRAIL) != 0u;
	float slow = p.shape.w;
	bool inAir = slow > 0.0 && pc.bodyPos.w > 0.0;
	if (inAir) { slow *= AirShare (pos); inAir = slow > 0.0; } // (in vacuum: none of it, whatever the emitter asked)
	if (inAir && dt > 0.0) {
		// slowed down towards the velocity of the air (co-rotating, and its wind)
		vec3 air = AirVel (pos);
		vel = air + (vel - air) * exp (-slow * dt);
		// turbulence of the air: eddies a few particle sizes across (a
		// trail's: a few metres a second, slowly varying, so that it
		// meanders and spreads unevenly)
		// (in the planet's frame: the eddies move with the air)
		vec3 pl = NoiseFrame (pos);
		vec3 q = trail ? pl * pc.trailNoise.w + pc.trailNoise.xyz + pc.bodyVel.w * 0.002
			: pl * pc.noiseOrg.w + pc.noiseOrg.xyz + pc.bodyVel.w * 0.03;
		vec3 n = vec3 (texture (detailNoise, q).r, texture (detailNoise, q + vec3 (0.37, 0.71, 0.13)).r,
			texture (detailNoise, q + vec3 (0.59, 0.23, 0.87)).r) - 0.5;
		// (engine smoke: eddies of the noise's size, whatever the puff's - a
		// puff of 75 m (the SRBs' in thin air) was thrown about at some
		// 200 m/s, the column torn into clumps)
		float eddy = ParticleClass (p.flags) == PC_ENGINE_SMOKE ? min (p.shape.x, 12.0) : p.shape.x;
		vel += n * (trail ? 8.0 * slow * dt : 6.0 * slow * max (eddy, 0.5) * dt);
		// hot smoke rises, less as it cools and mixes
		if ((p.flags & PF_HOT) != 0u)
			vel += normalize (pos - pc.bodyPos.xyz) * (4.0 * exp (-p.posAge.w / 6.0) * dt);
	}
	bool speck = (p.flags & PF_SPECK) != 0u;
	bool ballistic = (p.flags & (PF_BALLISTIC | PF_SPECK)) != 0u && pc.bodyPos.w > 0.0; // (specks too)
	if (ballistic) vel -= normalize (pos - pc.bodyPos.xyz) * (pc.ground.y * dt); // (thrown dust falls back)
	// specks bounce off whatever they strike (not across a time jump)
	if (speck && dt > 0.0 && frameCarry[1].w > 0.5 && Bounce (pos, vel, dt, 0.02, frameMove)) {
		if (statsOn == 2u) atomicAdd (telemetry[0].z, 1u);
		p.posAge.xyz = pos;
		p.velLife.xyz = vel;
		particles[i] = p;
		return;
	}
	// (its motion relative to the ground over the step)
	vec3 move = vel * dt - frameMove;
	// smoke in the air and dust meet the ground: they stay above it (their
	// centre a third of their size away) and slide along it
	if ((inAir || ballistic) && !speck && (p.flags & PF_EMISSIVE) == 0u && dt > 0.0) {
		float gap = 0.3 * ParticleRadius (p);
#ifdef RT
		// (met at half the gap: the old ray, a step of the orbital motion
		// long, came down at some 30 degrees and stopped dust about there -
		// the look of the dust at a jet's foot was made with that; at the
		// full gap, more of it caught by the ground hid the splash's glow)
		gap *= 0.5;
		mat3 toView = transpose (mat3 (frame.viewToGlobal));
		// (along its motion relative to the ground over the step, from a few
		// metres above where it is: one that came to rest on the surface, a
		// hair under the next triangle of the terrain, is still found -
		// looked for from where it was only, it fell through: some 15 % of a
		// hovering DG's dust under the ground; from 1 m above, 1 %)
		vec3 upG = normalize (pos - pc.bodyPos.xyz);
		vec3 o = pos + upG * (gap + 3.0), seg = pos + move - o;
		float len = length (seg);
		vec3 d = seg / len;
		float t;
		vec3 N;
		// (only from above: smoke that starts beneath the terrain, e.g. in a
		// flame trench below the tiles' surface, gets out)
		vec3 upV = toView * upG;
		if (statsOn == 2u) atomicAdd (telemetry[0].w, uint (length (move) + 0.5));
		if (RTSurface (toView * o, toView * d, len + gap, upV, t, N) && dot (toView * d, N) < 0.0) {
			if (ballistic && !inAir) { particles[i].velLife.w = 0.0; return; } // (dust in vacuum: lands)
			vec3 Ng = mat3 (frame.viewToGlobal) * N;
			pos = o + d * t + Ng * gap; // (its centre a gap from the surface)
			vel = SlideOnGround (pos, vel, Ng, ballistic, ParticleClass (p.flags) == PC_DUST && inAir);
			move = vec3 (0.0);
		}
#else
		// the ground under the camera, as a sphere
		float h = GroundHeight (pos + move);
		if (pc.ground.x > 0.0 && h < gap) {
			if (ballistic && !inAir) { particles[i].velLife.w = 0.0; return; }
			vec3 up = normalize (pos + move - pc.bodyPos.xyz);
			move += up * (gap - h);
			vel = SlideOnGround (pos, vel, up, ballistic, ParticleClass (p.flags) == PC_DUST && inAir);
		}
#endif
	}
	p.posAge.xyz = pos + move;
	p.velLife.xyz = vel;
	particles[i] = p;
	// (telemetry: smoke and dust in air near the camera, their speed relative to it)
	if (statsOn == 2u && inAir && !speck && (p.flags & PF_EMISSIVE) == 0u && dot (p.posAge.xyz, p.posAge.xyz) < 9e6) {
		atomicAdd (telemetry[1].x, 1u);
		atomicAdd (telemetry[1].y, uint (min (length (vel - AirVel (p.posAge.xyz)) * 100.0, 1e6)));
	}
	// (telemetry: under the ground near the camera - its sphere, within 3 km)
	if (statsOn == 2u && pc.ground.x > 0.0 && (inAir || ballistic) && (p.flags & PF_EMISSIVE) == 0u && dot (p.posAge.xyz, p.posAge.xyz) < 9e6
		&& GroundHeight (p.posAge.xyz) < -(speck ? 1.0 : 3.0))
	{
		if (speck) atomicAdd (telemetry[0].y, 1u);
		else atomicAdd (telemetry[0].x, 1u);
	}

	// the nearest volumetric particle (the client leaves out the smoke volume
	// while none is within its reach)
	if ((p.flags & PF_VOLUMETRIC) != 0u) {
		// (as if at the volume's reach where it is handed over to sprites
		// sooner: a trail's smoke)
		float dh = VOL_REACH_M - VolHandoff (p.flags).y;
		uint d = floatBitsToUint (max (length (p.posAge.xyz) - ParticleRadius (p), 0.0) + dh);
		if (d < nearVol) atomicMin (nearVol, d);
	}
}
