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
} pc;

// current opacity and radius of a particle (as drawn)
float ParticleAlpha (Particle p)
{
	float t = p.posAge.w / p.velLife.w;
	return p.shape.z * (1.0 - t) * smoothstep (0.0, 0.08, t);
}
float ParticleRadius (Particle p) { return 0.5 * (p.shape.x + p.shape.y * p.posAge.w); }

// grid coordinates of a camera-relative position (0..1 inside)
vec3 ShadowCoord (vec3 pos)
{
	return vec3 (dot (pos, pc.lightX.xyz) / pc.lightX.w, dot (pos, pc.lightY.xyz) / pc.lightX.w,
		dot (pos, pc.lightZ.xyz) / pc.lightY.w) * 0.5 + 0.5;
}
