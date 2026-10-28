// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Smoke volume: the view ray through the smoke up to the opaque scene,
// accumulating the light scattered by the smoke and its transmittance;
// the smoke's edges are eroded by noise anchored to the world (drifting),
// so that the coarse grid shows billows

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "smokevol.glsl"
layout(set = 1, binding = 3) uniform sampler3D detailNoise;
layout(set = 1, binding = 4) uniform sampler2D prepass;     // view depth (w; 0: none)
layout(set = 1, binding = 17) uniform sampler3D volTex;     // light (rgb), extinction (a)
layout(set = 1, binding = 18) uniform sampler2D volColumns;

layout(push_constant) uniform Params {
	vec4 noiseOrg;  // noise coordinates of the camera (xyz), w: time [s]
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Hash (uvec2 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
	return float (h & 0xffffu) / 65535.0;
}

void main ()
{
	// the smoke's depth range in this column and its neighbours (the volume
	// is filtered across froxels); the columns' own ranges (filtered, the
	// ranges of neighbours at other depths would blend into one holding
	// neither: blocks missing along the ground)
	ivec2 col = ivec2 (vUV * vec2 (VOL_SIZE.xy));
	vec2 rng = vec2 (1.0, 0.0);
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++) {
			vec2 c = texelFetch (volColumns, clamp (col + ivec2 (x, y), ivec2 (0), VOL_SIZE.xy - 1), 0).rg;
			if (c.x < c.y) rng = vec2 (min (rng.x, c.x), max (rng.y, c.y));
		}
	if (rng.x >= rng.y) discard;
	vec2 ndc = vUV * 2.0 - 1.0;
	vec3 d = normalize (vec3 (ndc.x / frame.proj[0][0], ndc.y / frame.proj[1][1], 1.0));
	float zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	float zEnd = zs > 0.0 ? zs : VOL_FAR;
	float s0 = max (rng.x - 1.0 / float (VOL_SIZE.z), 0.0), s1 = min (rng.y + 1.0 / float (VOL_SIZE.z), VolSlice (min (zEnd, VOL_FAR)));
	if (s1 <= s0) discard;
	const int N = 48;
	// (interleaved gradient noise: an even spread of the steps' offsets)
	vec2 fc = gl_FragCoord.xy + 5.588238 * float (uint (pc.noiseOrg.w * 60.0) & 63u);
	float jit = fract (52.9829189 * fract (dot (fc, vec2 (0.06711056, 0.00583715))));
	float T = 1.0;
	vec3 L = vec3 (0.0);
	mat3 v2g = mat3 (frame.viewToGlobal);
	float ds = (s1 - s0) / float (N);
	for (int i = 0; i < N; i++) {
		float sa = s0 + (float (i) + jit) * ds;
		float z0 = VolDepth (s0 + float (i) * ds), z1 = min (VolDepth (s0 + float (i + 1) * ds), zEnd);
		float dt = (z1 - z0) / d.z;
		if (dt <= 0.0) break;
		vec4 v = textureLod (volTex, vec3 (vUV, sa), 0.0);
		if (v.a <= 1e-5) continue;
		vec3 p = d * (VolDepth (sa) / d.z);
		// erosion: billows of a few metres, drifting upwards slowly
		vec3 w = v2g * p * 0.04 + pc.noiseOrg.xyz + vec3 (0.0, -pc.noiseOrg.w * 0.02, 0.0);
		float n = texture (detailNoise, w).r * 0.55 + texture (detailNoise, w * 2.7).r * 0.3 + texture (detailNoise, w * 7.1).r * 0.15;
		float dens = v.a;
		float core = min (dens * 3.0, 1.0);
		float keep = smoothstep (0.4, 0.65, n * 0.9 + core * 0.4);
		float sigma = dens * keep * 1.6;
		float Ts = exp (-sigma * dt);
		// (crevices between billows, where the noise eats in, are darker)
		L += T * (1.0 - Ts) * v.rgb * mix (0.55, 1.05, keep);
		T *= Ts;
		if (T < 0.005) break;
	}
	float a = 1.0 - T;
	if (a < 0.002) discard;
	outColor = vec4 (L, a);
	outReactive = clamp (a, 0.0, 0.9);
}
