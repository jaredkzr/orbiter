// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// GPU particles: the sets and parameters of the compute passes after
// emission (motion, smoke shadows, lighting); with RT, the ray tracing
// scene for collisions and shadows of vessels and bases

#define PARTICLE_WIND_SET 0 // (the wind table: particles.glsl)
#define PARTICLE_WIND_QUAL
#include "particles.glsl"
layout(set = 0, binding = 2) uniform sampler3D shapeNoise;
layout(set = 0, binding = 3) uniform sampler3D detailNoise;
// smoke shadows: a grid aligned with the sunlight around the camera
layout(set = 0, binding = 5, r32ui) uniform uimage3D shadowAccum; // smoke per cell (fixed point)
layout(set = 0, binding = 6, r16f) uniform image3D shadowDepth;   // optical depth towards the sun
layout(set = 0, binding = 7) uniform sampler3D shadowDepthTex;

// the allocated blocks of the pool (256 particles each): the passes over
// the particles go over these (ParticleSlot)
layout(set = 0, binding = 10, std430) readonly buffer BlockList { uint blockList[]; };

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
	vec4 omega;     // its angular velocity (global frame), w: particles in the allocated blocks (passes over them)
	vec4 noiseOrg;  // turbulence noise coordinates of the camera position (xyz), w: their scale [1/m]
	vec4 lightX;    // shadow grid axes (global): across the sunlight, w: half width [m]
	vec4 lightY;    //   w: half depth along the sunlight [m]
	vec4 lightZ;    //   towards the sun, w: extinction per unit of smoke
	vec4 ground;    // x: radius of the ground under the camera (no ray tracing), y: surface gravity [m/s^2],
	                // z: keys sorted (particle_sortkeys.comp), w: 1 = volumetric particles near are in the smoke volume (not drawn)
	vec4 trailNoise; // noise coordinates of the camera position at the trails' eddies' scale (xyz), w: that scale [1/m]
	// the noise is anchored to the planet (turning with it): positions
	// (global axes) turned into its frame by these rows; the noise
	// coordinates of the camera wrapped (all scales: integer ratios)
	vec4 noiseRot[3]; // rows of global -> planet axes, w: the smoke volume's erosion noise coordinates of the camera
	vec4 vol;        // smoke volume: x: pass (smoke_resolve.comp: 1 coarse), y: frame parity (occupancy), z: 1 = all tiles, w: erosion scale [1/m]
} pc;

// a camera-relative position (global axes) in the planet's axes
vec3 NoiseFrame (vec3 pos) { return vec3 (dot (pc.noiseRot[0].xyz, pos), dot (pc.noiseRot[1].xyz, pos), dot (pc.noiseRot[2].xyz, pos)); }

// the slot of the particle of invocation g of a pass over the allocated
// blocks (~0u: none)
uint ParticleSlot (uint g)
{
	return g < uint (pc.omega.w) ? blockList[g >> 8] * 256u + (g & 255u) : ~0u;
}

// grid coordinates of a camera-relative position (0..1 inside)
vec3 ShadowCoord (vec3 pos)
{
	return vec3 (dot (pos, pc.lightX.xyz) / pc.lightX.w, dot (pos, pc.lightY.xyz) / pc.lightX.w,
		dot (pos, pc.lightZ.xyz) / pc.lightY.w) * 0.5 + 0.5;
}
