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
// other hot air (PlumeRenderer::HeatSource: crash fireballs, site fires)
struct HeatSrc {
	vec4 pos;    // view space [m], w: the ball's radius r [m]
	vec4 up;     // the local vertical (view space), w: the heat at its surface (0..1)
	vec4 col;    // the column's height [m], its heat, the shell's radius [m], its width [m]
	vec4 misc;   // the shell's strength, the eddies' drift up so far [r]
};
layout(set = 0, binding = 5, std430) readonly buffer HeatBuffer {
	uint heatCount[4];
	HeatSrc heats[];   // slot * HEAT_MAX + index
};
const uint HEAT_MAX = 8u;

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
	// crash fireballs and site fires: a ball of hot gas with the air at its
	// surface shimmering (the ball itself is drawn over it), a column of hot
	// air rising from it, and, for the first tenths of a second, the thin
	// shell of air its front pushed out (a refraction ring, the background
	// drawn in and out across it: no shock - PLAN.md section 3)
	uint nh = heatCount[slot];
	for (uint i = 0u; i < nh; i++) {
		HeatSrc H = heats[slot * HEAT_MAX + i];
		vec3 c = H.pos.xyz;
		float r = H.pos.w;
		vec3 u = H.up.xyz;
		float colH = H.col.x, ringR = H.col.z, ringW = max (H.col.w, 0.01 * r);
		// the bounding sphere of all of it: the halo (1.6 r), the column
		// above, the shell
		float rb = max (1.6 * r, H.misc.x > 0.0 ? ringR + 3.0 * ringW : 0.0);
		vec3 cb = c + u * (0.5 * colH);
		rb = max (rb, 0.5 * colH + 0.5 * r + 0.25 * colH);
		float tc = dot (cb, d);
		float h2 = dot (cb, cb) - tc * tc;
		if (h2 > rb * rb) continue;
		float hw = sqrt (rb * rb - h2);
		float t0 = max (tc - hw, 0.0), t1 = tc + hw;
		if (zs > 0.0) t1 = min (t1, zs / d.z);
		if (t1 <= t0) continue;
		const int N = 12;
		float dt = (t1 - t0) / float (N);
		float heat = 0.0, ringH = 0.0;
		vec3 hp = vec3 (0.0);
		for (int k = 0; k < N; k++) {
			vec3 p = d * (t0 + (float (k) + 0.5) * dt);
			vec3 q = p - c;
			float z = dot (q, u);
			float dist = length (q);
			float rad = length (q - u * z);
			// the hot air at its surface (and the ball: drawn over this), a
			// layer about a quarter of its radius thick (cut at 1.5 r: the
			// tails of it reached over the whole sea)
			float e = (dist / r - 1.0) / 0.25;
			float h = H.up.w * exp (-e * e) * smoothstep (1.6, 1.35, dist / r);
			// the column above it: its hot air rising, widening a little
			if (colH > 0.0 && z > 0.0) {
				float w = 0.5 * r + 0.25 * z;
				float a = rad / w;
				h += H.col.y * exp (-a * a) * smoothstep (2.0, 1.4, a) * smoothstep (0.0, 0.6 * r, z) * (1.0 - smoothstep (0.5 * colH, colH, z));
			}
			if (H.misc.x > 0.0) {
				float s = (dist - ringR) / ringW;
				ringH += H.misc.x * exp (-s * s) * dt / r;
			}
			heat += h * dt / r;
			hp += p * h * dt / r;
		}
		if (heat > 1e-3) {
			hp /= heat;
			float dist = max (length (hp), 1.0);
			// eddies about a seventh of its radius, carried up with the hot air
			float eddy = 0.15 * r;
			float eddyPx = eddy * pc.focal.y * 0.5 * pc.size.y / dist;
			vec3 q = (hp - c) / eddy * 0.35;
			q -= u * (H.misc.y * r / eddy * 0.35);
			vec2 v = vec2 (texture (plumeDetailNoise, q).r, texture (plumeDetailNoise, q + vec3 (0.31, 0.57, 0.13)).r) * 4.0 - 2.0;
			// (a fireball's hot air bends more than an exhaust's: 2.5 times the plumes' at unit heat)
			offset += v * (min (4.0 * heat, 3.0) * pc.strength * smoothstep (1.0, 6.0, eddyPx));
		}
		if (ringH > 1e-3) {
			// the shell refracts the background in across its inside and
			// out across its outside, most at its limb: a ring on the screen
			// (its centre on the screen; the pixel's radial direction)
			vec2 cn = vec2 (c.x / max (c.z, 1e-3) * pc.focal.x, -c.y / max (c.z, 1e-3) * pc.focal.y);
			vec2 cpx = (cn + 1.0 + pc.jitter) * 0.5 * pc.size;
			vec2 rd = gl_FragCoord.xy - cpx;
			float lr = length (rd);
			if (lr > 0.5) {
				float tcc = dot (c, d);
				float b = sqrt (max (dot (c, c) - tcc * tcc, 0.0)); // (the ray's closest approach to the centre)
				float side = tanh ((b - ringR) / (1.5 * ringW));
				offset += rd / lr * (side * min (ringH, 2.0) * pc.strength * 1.5);
			}
		}
	}
	if (dot (offset, offset) < 0.01) discard;
	outColor = vec4 (textureLod (source, vUV + offset / pc.size, 0.0).rgb, 1.0);
}
