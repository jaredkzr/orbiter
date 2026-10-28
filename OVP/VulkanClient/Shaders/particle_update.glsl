// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: ageing and motion (drag towards the air, turbulence,
// rising hot smoke), the shift to the new camera position, and collisions
// of smoke with the ground, bases and vessels: it slides along them
// (included by particle_update.comp and particle_update_rt.comp)

layout(local_size_x = 64) in;

#include "particle_common.glsl"

#ifdef RT
// the nearest surface of the instances in mask along o + t d (view space),
// t in [tMin, tMax]: distance, normal (either side), and the owner and its
// velocity (global frame; terrain: 0)
bool RTStrike (vec3 o, vec3 d, float tMin, float tMax, uint mask, out float t, out vec3 N, out uint owner, out vec3 vel)
{
	if (!RayFinite (o, d, tMax)) return false;
	rayQueryEXT rq;
	rayQueryInitializeEXT (rq, tlas, gl_RayFlagsOpaqueEXT, mask, o, tMin, d, tMax);
	while (rayQueryProceedEXT (rq)) {}
	if (rayQueryGetIntersectionTypeEXT (rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
	t = rayQueryGetIntersectionTEXT (rq, true);
	RTInstance r = inst[rayQueryGetIntersectionInstanceCustomIndexEXT (rq, true) + rayQueryGetIntersectionGeometryIndexEXT (rq, true)];
	owner = r.owner, vel = r.vel.xyz;
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
	vec3 vel;
	if (!RTStrike (o, d, 0.0, tMax, 0x02u, t, N, owner, vel)) return false;
	if (dot (N, up) < 0.0) N = -N; // (the upper side)
	return true;
}
#endif

// the velocity of the ground (and the air) at pos (camera-relative): the
// planet's, turning with it
vec3 GroundVel (vec3 pos)
{
	return pc.bodyVel.xyz + cross (pos - pc.bodyPos.xyz, pc.omega.xyz);
}

// a speck (fragment, spark) that strikes a surface in this step bounces off
// it, in the surface's frame (vessels, debris and the ground move at
// kilometres per second in the global frame): its velocity across the
// surface, less some by friction, and some of that into it, reversed.
// pos: where it was (camera-relative now), vel: its velocity; true if it
// struck (pos, vel: after the bounce)
bool Bounce (inout vec3 pos, inout vec3 vel, float dt, float gap)
{
	const float RESTITUTION = 0.35, FRICTION = 0.25;
	vec3 vs; // the surface's velocity
	vec3 Ng; // its normal (global axes), against the speck's motion
	vec3 hit;
#ifdef RT
	// (a first look in the frame of the camera, which moves with what it
	// follows; what it strikes, looked at again in that one's own frame)
	mat3 toView = transpose (mat3 (frame.viewToGlobal));
	vec3 vRef = -pc.camShift.xyz / dt;
	float t;
	vec3 N;
	uint owner;
	bool struck = false;
	for (int k = 0; k < 2; k++) {
		vec3 o = pos + vRef * dt, m = (vel - vRef) * dt;
		float len = length (m);
		if (len < 1e-5) return false;
		vec3 vo;
		if (!RTStrike (toView * o, toView * (m / len), 0.01, len + gap, 0x03u, t, N, owner, vo)) return false;
		vs = owner != 0u ? vo : GroundVel (o + m * (t / len));
		hit = o + m * (t / len);
		Ng = mat3 (frame.viewToGlobal) * N;
		if (dot (Ng, m) > 0.0) Ng = -Ng;
		// (the first look was in that frame, close enough: struck)
		if (length (vs - vRef) * dt < 0.25 * gap + 0.01) { struck = true; break; }
		vRef = vs;
	}
	if (!struck) return false;
	pos = hit + Ng * gap;
#else
	// the ground under the camera, as a sphere
	vec3 up = pos + vel * dt - pc.bodyPos.xyz;
	float r = length (up);
	if (pc.ground.x <= 0.0 || r >= pc.ground.x + gap) return false;
	Ng = up / r;
	vs = GroundVel (pos);
	pos = pc.bodyPos.xyz + Ng * (pc.ground.x + gap);
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
	uint i = gl_GlobalInvocationID.x;
	if (i >= uint (pc.omega.w)) return;
	Particle p = particles[i];
	if (p.velLife.w <= 0.0) return;
	float dt = pc.camShift.w;
	p.posAge.w += dt;
	if (p.posAge.w >= p.velLife.w) { particles[i].velLife.w = 0.0; return; }

	vec3 pos = p.posAge.xyz + pc.camShift.xyz; // (relative to the camera now)
	vec3 vel = p.velLife.xyz;
	bool inAir = p.shape.w > 0.0 && pc.bodyPos.w > 0.0;
	if (inAir && dt > 0.0) {
		// slowed down towards the velocity of the co-rotating air
		vec3 air = pc.bodyVel.xyz + cross (pos - pc.bodyPos.xyz, pc.omega.xyz);
		vel = air + (vel - air) * exp (-p.shape.w * dt);
		// turbulence of the air: eddies a few particle sizes across
		vec3 q = pos * pc.noiseOrg.w + pc.noiseOrg.xyz + pc.bodyVel.w * 0.03;
		vec3 n = vec3 (texture (detailNoise, q).r, texture (detailNoise, q + vec3 (0.37, 0.71, 0.13)).r,
			texture (detailNoise, q + vec3 (0.59, 0.23, 0.87)).r) - 0.5;
		vel += n * (6.0 * p.shape.w * max (p.shape.x, 0.5) * dt);
		// hot smoke rises, less as it cools and mixes
		if ((p.flags & 16u) != 0u)
			vel += normalize (pos - pc.bodyPos.xyz) * (4.0 * exp (-p.posAge.w / 6.0) * dt);
	}
	bool speck = (p.flags & 128u) != 0u;
	bool ballistic = (p.flags & (32u | 128u)) != 0u && pc.bodyPos.w > 0.0; // (specks too)
	if (ballistic) vel -= normalize (pos - pc.bodyPos.xyz) * (pc.ground.y * dt); // (thrown dust falls back)
	// specks bounce off whatever they strike
	if (speck && dt > 0.0 && Bounce (pos, vel, dt, 0.02)) {
		p.posAge.xyz = pos;
		p.velLife.xyz = vel;
		particles[i] = p;
		return;
	}
	vec3 move = vel * dt;
	// smoke in the air and dust meet the ground: they stay above it (their
	// centre a third of their size away) and slide along it
	if ((inAir || ballistic) && !speck && (p.flags & 4u) == 0u && dt > 0.0) {
		float gap = 0.3 * ParticleRadius (p);
#ifdef RT
		mat3 toView = transpose (mat3 (frame.viewToGlobal));
		float len = length (move);
		vec3 d = len > 1e-4 ? move / len : -normalize (pos - pc.bodyPos.xyz);
		float t;
		vec3 N;
		// (only from above: smoke that starts beneath the terrain, e.g. in a
		// flame trench below the tiles' surface, gets out)
		vec3 upV = toView * normalize (pos - pc.bodyPos.xyz);
		if (RTSurface (toView * pos, toView * d, len + gap, upV, t, N) && dot (toView * d, N) < 0.0) {
			if (ballistic && !inAir) { particles[i].velLife.w = 0.0; return; } // (dust in vacuum: lands)
			vec3 Ng = mat3 (frame.viewToGlobal) * N;
			pos += d * max (t - gap, 0.0);
			float into = dot (vel, Ng);
			if (into < 0.0) vel -= Ng * into;
			vel *= ballistic ? 0.5 : 0.85; // (friction)
			move = vec3 (0.0);
		}
#else
		// the ground under the camera, as a sphere
		vec3 up = pos + move - pc.bodyPos.xyz;
		float r = length (up);
		if (pc.ground.x > 0.0 && r < pc.ground.x + gap) {
			if (ballistic && !inAir) { particles[i].velLife.w = 0.0; return; }
			up /= r;
			move += up * (pc.ground.x + gap - r);
			float into = dot (vel, up);
			if (into < 0.0) vel -= up * into;
			vel *= 0.85;
		}
#endif
	}
	p.posAge.xyz = pos + move;
	p.velLife.xyz = vel;
	particles[i] = p;
}
