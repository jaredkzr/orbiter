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
// smoke shadows: grids aligned with the sunlight around the camera, three
// cascades (each 8 times the last's size) stacked along z
layout(set = 0, binding = 5, r32ui) uniform uimage3D shadowAccum; // smoke per cell (fixed point)
layout(set = 0, binding = 6, r16f) uniform image3D shadowDepth;   // optical depth towards the sun
layout(set = 0, binding = 7) uniform sampler3D shadowDepthTex;
// the column tiles (8 x 8) of each cascade holding smoke, two frames
// (particle_shadow_sum.comp goes over those)
layout(set = 0, binding = 28, std430) buffer ShadowOcc { uint shadowOcc[]; };

// the allocated blocks of the pool (256 particles each): the passes over
// the particles go over these (ParticleSlot)
layout(set = 0, binding = 10, std430) readonly buffer BlockList { uint blockList[]; };

const ivec3 SHADOW_GRID = ivec3 (192, 192, 64); // (each cascade)
const int SHADOW_CASCADES = 3;
const float SHADOW_RATIO = 8.0;     // (a cascade's size over the last's)
const int SHADOW_TILES = 24;        // (tiles of 8 x 8 columns across a cascade)
const uint SHADOW_TILE_COUNT = 2048u; // (per frame of the occupancy buffer)
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
	vec4 vol;        // x: smoke volume pass (smoke_resolve.comp: 1 coarse), y: its frame parity (occupancy), z: bit 0 all its tiles,
	                 // bit 1 the smoke shadows' frame parity, w: the volume's erosion noise scale [1/m]
} pc;

// a camera-relative position (global axes) in the planet's axes
vec3 NoiseFrame (vec3 pos) { return vec3 (dot (pc.noiseRot[0].xyz, pos), dot (pc.noiseRot[1].xyz, pos), dot (pc.noiseRot[2].xyz, pos)); }

// the slot of the particle of invocation g of a pass over the allocated
// blocks (~0u: none)
uint ParticleSlot (uint g)
{
	return g < uint (pc.omega.w) ? blockList[g >> 8] * 256u + (g & 255u) : ~0u;
}

// coordinates in cascade c of the shadow grid of a camera-relative position
// (0..1 inside)
vec3 ShadowCoord (vec3 pos, int c)
{
	float k = pow (SHADOW_RATIO, float (c));
	return vec3 (dot (pos, pc.lightX.xyz) / (pc.lightX.w * k), dot (pos, pc.lightY.xyz) / (pc.lightX.w * k),
		dot (pos, pc.lightZ.xyz) / (pc.lightY.w * k)) * 0.5 + 0.5;
}
bool ShadowInside (vec3 sc) { return all (greaterThan (sc, vec3 (0.0))) && all (lessThan (sc, vec3 (1.0))); }
uint ShadowParity () { return (uint (pc.vol.z) >> 1) & 1u; }

// the sunlight let through by the smoke between a camera-relative position
// and the sun (the shadow grid; outside it: all). Light scattered many
// times inside dense smoke still gets through: a strong attenuation and a
// weak one, after cloud renderers
float SmokeTau (vec3 pos)
{
	// (each particle is in one cascade: the optical depths of all cascades
	// holding the point add up)
	float tau = 0.0;
	for (int c = 0; c < SHADOW_CASCADES; c++) {
		vec3 sc = ShadowCoord (pos, c);
		// (beyond the cascade's far side from the sun: its whole column)
		if (any (lessThanEqual (sc.xy, vec2 (0.0))) || any (greaterThanEqual (sc.xy, vec2 (1.0))) || sc.z >= 1.0) continue;
		float z = clamp (sc.z * float (SHADOW_GRID.z), 0.5, float (SHADOW_GRID.z) - 0.5) + float (c * SHADOW_GRID.z);
		tau += textureLod (shadowDepthTex, vec3 (sc.xy, z / float (SHADOW_GRID.z * SHADOW_CASCADES)), 0.0).r;
	}
	return tau;
}
float SmokeShadow (vec3 pos)
{
	float tau = SmokeTau (pos);
	return 0.55 * exp (-tau) + 0.45 * exp (-0.08 * tau);
}
