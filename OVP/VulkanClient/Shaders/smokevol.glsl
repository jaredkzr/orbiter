// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Smoke volume (volumetric particles): a grid aligned with the view
// frustum (froxels), depth slices spaced exponentially from 1 m to 20 km.
// Requires frame.glsl.

const ivec3 VOL_SIZE = ivec3 (240, 135, 128);
const float VOL_NEAR = 1.0, VOL_FAR = 20000.0;
// large particles go into a coarse grid over the same frustum (a quarter
// of the resolution: smoke_splat.comp), added in by smoke_resolve.comp
const ivec3 VOL_COARSE = ivec3 (60, 34, 32);
// the columns holding smoke, in tiles of 8 x 8 (smoke_resolve.comp goes
// over these only)
const ivec2 VOL_TILES = ivec2 (30, 17);
const uint VOL_TILE_COUNT = 512u; // (per frame of the occupancy buffer)
// accumulation (fixed point): extinction [1/m], coverage (opacity, no
// unit) and the light weighted by coverage
const float VOL_FIXED_D = 262144.0, VOL_FIXED_C = 65536.0;
// erosion of the smoke by noise (smoke_march.comp; its mean in
// smoke_resolve.comp): what is kept, from the noise n (0..1, mean 0.5) and
// the smoke's core (its coverage: 0 at the edges of the particles, 1 inside
// dense smoke), relative to the particles, not in absolute units, so that
// large thin puffs keep their shape
float VolKeep (float n, float core) { return smoothstep (0.3, 0.55, n * 0.8 + core * 0.5); }
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
