// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: the sets and parameters of the compute passes after
// emission (motion, smoke shadows, lighting); with RT, the ray tracing
// scene for collisions and shadows of vessels and bases

#include "particles.glsl"
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler3D detailNoise;
// smoke shadows: a grid aligned with the sunlight around the camera
layout(set = 0, binding = 5, r32ui) uniform uimage3D shadowAccum; // smoke per cell (fixed point)
layout(set = 0, binding = 6, r16f) uniform image3D shadowDepth;   // optical depth towards the sun
layout(set = 0, binding = 7) uniform sampler3D shadowDepthTex;

// the wind of the planet nearby at each kilometre of altitude (x east, y
// north) [m/s] (oapiGetModelWind: its layers, whether or not the physics
// has wind)
layout(set = 0, binding = 20, std430) buffer WindTable {
	vec4 windLayer[64];
	uint nearVol; // the nearest volumetric particle's distance (float bits; particle_update.glsl, read back)
	uint statsOn; // telemetry (ORBITER_VK_PARTICLE_STATS): particles counted by particle_sortkeys.comp
	uint liveCount, liveVol;
};

const ivec3 SHADOW_GRID = ivec3 (192, 192, 64);
const float SHADOW_FIXED = 1024.0; // (accumulation scale)

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
	vec4 lightX;    // shadow grid axes (global): across the sunlight, w: half width [m]
	vec4 lightY;    //   w: half depth along the sunlight [m]
	vec4 lightZ;    //   towards the sun, w: extinction per unit of smoke
	vec4 ground;    // x: radius of the ground under the camera (no ray tracing), y: surface gravity [m/s^2]
	vec4 trailNoise; // noise coordinates of the camera position at the trails' eddies' scale (xyz), w: that scale [1/m]
} pc;

// current opacity and radius of a particle (as drawn)
float ParticleAlpha (Particle p)
{
	float t = p.posAge.w / p.velLife.w;
	float a = p.shape.z * (1.0 - t) * smoothstep (0.0, 0.08, t);
	// (a trail's smoke, slowdown < 0: spread over more as it grows (the
	// puffs at a fixed spacing overlap the more, the trail as thick); dense
	// at first, it stays opaque until four times its size
	// (and fading in as the engine's smoke fades: over 25 s, not the
	// lifetime's 8 percent)
	if (p.shape.w < 0.0) a *= min (4.0 * p.shape.x / max (p.shape.x + p.shape.y * p.posAge.w, 1e-3), 1.0) * smoothstep (0.0, 25.0, p.posAge.w) / max (smoothstep (0.0, 0.08, t), 1e-3);
	return a;
}
float ParticleRadius (Particle p) { return 0.5 * (p.shape.x + p.shape.y * p.posAge.w); }

// grid coordinates of a camera-relative position (0..1 inside)
vec3 ShadowCoord (vec3 pos)
{
	return vec3 (dot (pos, pc.lightX.xyz) / pc.lightX.w, dot (pos, pc.lightY.xyz) / pc.lightX.w,
		dot (pos, pc.lightZ.xyz) / pc.lightY.w) * 0.5 + 0.5;
}
