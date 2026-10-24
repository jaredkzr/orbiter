// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Smoke volume (volumetric particles): a grid aligned with the view
// frustum (froxels), depth slices spaced exponentially from 1 m to 20 km.
// Requires frame.glsl.

const ivec3 VOL_SIZE = ivec3 (240, 135, 128);
const float VOL_NEAR = 1.0, VOL_FAR = 20000.0;
const float VOL_FIXED = 4096.0; // (accumulation scale)

float VolSlice (float z) { return log (max (z, VOL_NEAR) / VOL_NEAR) / log (VOL_FAR / VOL_NEAR); }
float VolDepth (float s) { return VOL_NEAR * exp (s * log (VOL_FAR / VOL_NEAR)); }

// grid coordinates (0..1) of a view-space point
vec3 VolCoord (vec3 v)
{
	vec4 c = frame.proj * vec4 (v, 1.0);
	vec2 ndc = c.xy / max (c.w, 1e-6) - frame.jitter.xy;
	return vec3 (ndc * 0.5 + 0.5, VolSlice (v.z));
}
