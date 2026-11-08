// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Smoke volume (volumetric particles): a grid aligned with the view
// frustum (froxels), depth slices spaced exponentially from 1 m to 20 km.
// Requires frame.glsl.

const ivec3 VOL_SIZE = ivec3 (240, 135, 128);
const float VOL_NEAR = 1.0, VOL_FAR = 20000.0;
// larger particles go into coarser grids over the same frustum (half and
// a quarter of the resolution: smoke_splat.comp), added in by
// smoke_resolve.comp
const ivec3 VOL_MID = ivec3 (120, 68, 64), VOL_COARSE = ivec3 (60, 34, 32);
ivec3 VolLevelSize (int l) { return l == 0 ? VOL_SIZE : l == 1 ? VOL_MID : VOL_COARSE; }
// the accumulators of each level: extinction, coverage, light x coverage
// (rgb), the sun's share x coverage, the largest coverage of one particle
// (the erosion's core: overlapping old puffs still erode, as sprites do)
const int VA_D = 0, VA_C = 1, VA_R = 2, VA_G = 3, VA_B = 4, VA_S = 5, VA_M = 6, VA_COUNT = 7;
// the columns holding smoke, in tiles of 8 x 8 (smoke_resolve.comp goes
// over these only)
const ivec2 VOL_TILES = ivec2 (30, 17);
const uint VOL_TILE_COUNT = 512u; // (per frame of the occupancy buffer)
// accumulation (fixed point): extinction [1/m], coverage (opacity, no
// unit) and the light weighted by coverage
const float VOL_FIXED_D = 262144.0, VOL_FIXED_C = 65536.0;
// erosion of the smoke by noise (smoke_march.comp; its mean in
// smoke_resolve.comp): what is kept, from the noise n (0..1, mean 0.5) and
// the smoke's core (the largest coverage of one particle: 0 at the edges of
// the particles, 1 inside dense young ones), relative to the particles, not
// in absolute units, so that large thin puffs keep their shape
float VolKeep (float n, float core) { return smoothstep (0.35, 0.6, n * 0.8 + core * 0.45); }
float VolCore (float coverage) { return min (coverage * 1.5, 1.0); }

float VolSlice (float z) { return log (max (z, VOL_NEAR) / VOL_NEAR) / log (VOL_FAR / VOL_NEAR); }
float VolDepth (float s) { return VOL_NEAR * exp (s * log (VOL_FAR / VOL_NEAR)); }

// grid coordinates (0..1) of a view-space point
vec3 VolCoord (vec3 v)
{
	vec4 c = frame.proj * vec4 (v, 1.0);
	vec2 ndc = c.xy / max (c.w, 1e-6) - frame.jitter.xy;
	return vec3 (ndc * 0.5 + 0.5, VolSlice (v.z));
}
