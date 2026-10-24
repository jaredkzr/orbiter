// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: ageing and motion (drag towards the air, turbulence,
// rising hot smoke), the shift to the new camera position, and collisions
// of smoke with the ground, bases and vessels: it slides along them
// (included by particle_update.comp and particle_update_rt.comp)

layout(local_size_x = 64) in;

#include "particle_common.glsl"

#ifdef RT
// the nearest terrain along o + t d (view space), within tMax: distance and
// normal (bases and vessels are left out: smoke deflected into a launch
// pad's flame trench would be trapped under the deck, which in reality it
// leaves through the trench's exits)
bool RTSurface (vec3 o, vec3 d, float tMax, vec3 up, out float t, out vec3 N)
{
	rayQueryEXT rq;
	rayQueryInitializeEXT (rq, tlas, gl_RayFlagsOpaqueEXT, 0x02, o, 0.0, d, tMax);
	while (rayQueryProceedEXT (rq)) {}
	if (rayQueryGetIntersectionTypeEXT (rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
	t = rayQueryGetIntersectionTEXT (rq, true);
	RTInstance r = inst[rayQueryGetIntersectionInstanceCustomIndexEXT (rq, true) + rayQueryGetIntersectionGeometryIndexEXT (rq, true)];
	uint prim = uint (rayQueryGetIntersectionPrimitiveIndexEXT (rq, true));
	vec3 v[3];
	RTFloats f = RTFloats (r.vtx);
	for (int k = 0; k < 3; k++) {
		uint vi = RTIndex (r.idx, r.firstIndex + prim * 3u + uint (k)) * 8u;
		v[k] = vec3 (f.f[vi], f.f[vi + 1u], f.f[vi + 2u]);
	}
	N = normalize (mat3 (rayQueryGetIntersectionObjectToWorldEXT (rq, true)) * cross (v[1] - v[0], v[2] - v[0]));
	if (dot (N, up) < 0.0) N = -N; // (the upper side)
	return true;
}
#endif

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
	vec3 move = vel * dt;
	// smoke in the air meets the ground: it stays above it (its centre a
	// third of its size away) and slides along it
	if (inAir && (p.flags & 4u) == 0u && dt > 0.0) {
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
			vec3 Ng = mat3 (frame.viewToGlobal) * N;
			pos += d * max (t - gap, 0.0);
			float into = dot (vel, Ng);
			if (into < 0.0) vel -= Ng * into;
			vel *= 0.85; // (friction)
			move = vec3 (0.0);
		}
#else
		// the ground under the camera, as a sphere
		vec3 up = pos + move - pc.bodyPos.xyz;
		float r = length (up);
		if (pc.ground.x > 0.0 && r < pc.ground.x + gap) {
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
