// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Heat shimmer: the hot exhaust of engine plumes (also beyond the visible
// flame) refracts the light of what lies behind it. The hot gas along the
// view ray is integrated through each plume's volume; the background is
// sampled from a copy of the scene, displaced by turbulent eddies drifting
// with the flow, in proportion to the heat. Drawn before the plumes and
// particles, so that only the background is distorted.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D source;   // the scene so far
#define PLUME_SET 0
#define PLUME_BINDING 1
#include "plume.glsl"
layout(set = 0, binding = 4) uniform sampler2D prepass;  // view depth (w; 0: none)

layout(push_constant) uniform Params {
	vec2 focal;     // projection factors fx, fy
	vec2 jitter;    // projection jitter (NDC)
	vec2 size;      // [pixels]
	float slot;     // frame slot of the plumes
	float strength; // displacement [pixels] at unit heat
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// hot gas per exit radius at p (plume space): denser and lasting longer
// than the glow; none in vacuum
float PlumeHeat (PlumeParams P, vec3 p)
{
	if (p.z < 0.0) return 0.0;
	float bend = PlumeBend (P, p.z);
	p.xy -= P.bend.xy * bend;
	float x = length (vec2 (p.z, bend));
	float xi = x / P.shape.y;
	float R = 1.0 + (P.shape.z - 1.0) * (1.0 - exp (-x * 0.8)) + x * P.shape.w
		+ max (0.0, x - 0.5 * P.shape.y) * P.diamond.w + 0.3 * bend;
	R *= 1.3; // (the hot gas is wider than the bright core)
	float u = length (p.xy) / R;
	float h = exp (-2.0 * u * u) / R * exp (-0.6 * xi) * smoothstep (1.6, 1.0, xi);
	if (P.bend.z > 0.0) h *= exp (-PlumeSq (bend / (1.5 * P.bend.z + 5.0)));
	return h * (1.0 - P.core.w);
}

void main ()
{
	ivec2 pix = ivec2 (gl_FragCoord.xy);
	vec2 ndc = vUV * 2.0 - 1.0 - pc.jitter;
	vec3 d = normalize (vec3 (ndc.x / pc.focal.x, -ndc.y / pc.focal.y, 1.0));
	float zs = texelFetch (prepass, pix, 0).w;
	uint slot = uint (pc.slot), n = plumeCount[slot];
	vec2 offset = vec2 (0.0);
	for (uint i = 0u; i < n; i++) {
		PlumeParams P = plumes[slot * MAX_PLUMES + i];
		vec3 lo, ld;
		float t0, t1;
		if (!PlumeBox (P, vec3 (0.0), d, lo, ld, t0, t1)) continue;
		float re = P.shape.x;
		t0 = max (t0, 0.0);
		if (zs > 0.0) t1 = min (t1, zs / (d.z * re));
		if (t1 <= t0) continue;
		const int N = 10;
		float dt = (t1 - t0) / float (N), heat = 0.0;
		vec3 hp = vec3 (0.0);
		for (int k = 0; k < N; k++) {
			vec3 p = lo + ld * (t0 + (float (k) + 0.5) * dt);
			float h = PlumeHeat (P, p);
			heat += h * dt;
			hp += p * h * dt;
		}
		if (heat < 1e-3) continue;
		hp /= heat;
		// eddies about an exit radius across, drifting downstream; fine
		// eddies cancel out when they are smaller than a pixel
		float dist = max (length (hp * re - (P.toLocal * vec4 (0.0, 0.0, 0.0, 1.0)).xyz), 1.0);
		float eddyPx = re * pc.focal.y * 0.5 * pc.size.y / dist;
		vec3 q = vec3 (hp.xy, hp.z - P.misc.x * P.misc.y * 0.5) * 0.35 + P.misc2.y * 3.7;
		vec2 v = vec2 (texture (plumeDetailNoise, q).r, texture (plumeDetailNoise, q + vec3 (0.31, 0.57, 0.13)).r) * 4.0 - 2.0; // (about -1..1)
		offset += v * (min (heat, 2.0) * pc.strength * smoothstep (1.0, 6.0, eddyPx));
	}
	if (dot (offset, offset) < 0.01) discard;
	outColor = vec4 (textureLod (source, vUV + offset / pc.size, 0.0).rgb, 1.0);
}
