// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: ageing and motion (drag towards the air, turbulence),
// the shift to the new camera position, and the sunlight reaching each
// particle: through the atmosphere, in the planet's shadow, and (with ray
// queries) in the shadows of vessels and bases
// (included by particle_update.comp and particle_update_rt.comp)

layout(local_size_x = 64) in;

#include "particles.glsl"
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler3D detailNoise;

#ifdef RT
#include "rt.glsl" // (frame set 1, atmosphere set 4)
#else
#define FRAME_SET 1
#include "frame.glsl"
#define ATM_SET 2
#include "atmosphere.glsl"
#endif

layout(push_constant) uniform Params {
	vec4 camShift;  // previous camera position - this one (global axes), w: step [s]
	vec4 bodyPos;   // centre of the planet nearby (camera-relative), w: its radius (0: none)
	vec4 bodyVel;   // its velocity (global frame), w: time [s]
	vec4 omega;     // its angular velocity (global frame), w: number of particles
	vec4 noiseOrg;  // turbulence noise coordinates of the camera position (xyz), w: their scale [1/m]
} pc;

void main ()
{
	uint i = gl_GlobalInvocationID.x;
	if (i >= uint (pc.omega.w)) return;
	Particle p = particles[i];
	if (p.velLife.w <= 0.0) return;
	float dt = pc.camShift.w;
	p.posAge.w += dt;
	if (p.posAge.w >= p.velLife.w) { particles[i].velLife.w = 0.0; return; }

	vec3 pos = p.posAge.xyz;
	vec3 vel = p.velLife.xyz;
	if (p.shape.w > 0.0 && pc.bodyPos.w > 0.0 && dt > 0.0) {
		// slowed down towards the velocity of the co-rotating air
		vec3 air = pc.bodyVel.xyz + cross (pos - pc.bodyPos.xyz, pc.omega.xyz);
		vel = air + (vel - air) * exp (-p.shape.w * dt);
		// turbulence of the air: eddies a few particle sizes across
		vec3 q = pos * pc.noiseOrg.w + pc.noiseOrg.xyz + pc.bodyVel.w * 0.03;
		vec3 n = vec3 (texture (detailNoise, q).r, texture (detailNoise, q + vec3 (0.37, 0.71, 0.13)).r,
			texture (detailNoise, q + vec3 (0.59, 0.23, 0.87)).r) - 0.5;
		vel += n * (6.0 * p.shape.w * max (p.shape.x, 0.5) * dt);
	}
	pos += vel * dt + pc.camShift.xyz;
	p.posAge.xyz = pos;
	p.velLife.xyz = vel;

	// sunlight reaching the particle (not needed for emissive ones)
	uint flags = floatBitsToUint (p.light.w);
	if ((flags & 4u) == 0u) {
		mat3 toView = transpose (mat3 (frame.viewToGlobal));
		vec3 vp = toView * pos;
		vec3 sun = frame.sunColor.rgb;
		if (atm.misc.x > 0.5) {
			vec3 pp = mat3 (atm.viewToPlanet) * vp + atm.camPos.xyz;
			sun *= SunTransmittance (pp) * CloudShadow (pp);
		} else if (pc.bodyPos.w > 0.0) {
			// airless: the planet's shadow (a cylinder behind it)
			vec3 d = pos - pc.bodyPos.xyz, s = mat3 (frame.viewToGlobal) * frame.sunDir.xyz;
			float along = dot (d, s);
			if (along < 0.0) sun *= smoothstep (0.995 * pc.bodyPos.w, 1.005 * pc.bodyPos.w, length (d - s * along));
		}
#ifdef RT
		sun *= RTSunVisible (vp + frame.sunDir.xyz * 0.5, frame.sunDir.xyz);
#endif
		p.light.rgb = sun;
	}
	particles[i] = p;
}
