// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Engine plumes: the ray through the pixel is marched through the plume's
// bounding box, up to the opaque scene (prepass depth), accumulating the
// emission of the hot gas and the extinction and scattering of smoke.
// Plume space: z along the flow from the nozzle exit, lengths in exit radii.
// The jet expands (or contracts) from the nozzle to the radius where its
// pressure matches the ambient pressure; in air, shock cells form along it
// (bright Mach discs), and after its core it mixes with the air, where
// fuel-rich exhaust burns (afterburning). In vacuum it spreads into a wide,
// faint bell.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D prepass;    // view depth (w; 0: none)
layout(set = 1, binding = 1) uniform sampler3D shapeNoise;
layout(set = 1, binding = 2) uniform sampler3D detailNoise;
layout(set = 2, binding = 0) uniform sampler2D exhaustTex; // add-on exhaust texture (tint)

layout(push_constant) uniform Plume {
	mat4 toLocal;  // view -> plume space [m]
	vec4 shape;    // exit radius [m], length, jet radius after the exit, spread (tan of the half angle)
	vec4 shape2;   // shock cell spacing, shock diamond strength, box length, box radius
	vec4 core;     // core emission (rgb), vacuum fraction
	vec4 diamond;  // shock diamond emission (rgb), spread of the mixing layer
	vec4 outer;    // afterburning emission (rgb), -
	vec4 misc;     // time [s], turbulence drift [exit radii/s], smoke extinction, turbulence
	vec4 misc2;    // texture tint amount, seed, sunlight reaching the plume, -
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

float Sq (float x) { return x * x; }

float Hash (uvec3 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h) / 4294967296.0;
}

// emission (rgb) and smoke extinction (a) per exit radius at p
vec4 Medium (vec3 p)
{
	float x = p.z;
	if (x < -0.15) return vec4 (0.0);
	float r = length (p.xy);
	float len = pc.shape.y, rj = pc.shape.z, spread = pc.shape.w;
	float cell = pc.shape2.x, D = pc.shape2.y;
	float xi = max (x, 0.0) / len;
	// turbulence: laminar at the exit, growing downstream
	float turb = pc.misc.w * smoothstep (0.02, 0.5, xi);
	vec3 q = vec3 (p.xy, x - pc.misc.x * pc.misc.y) + pc.misc2.y * 7.31;
	float n1 = texture (shapeNoise, q * 0.045).r;
	float n2 = texture (detailNoise, q * 0.23 + 0.4 * n1).r;

	// envelope radius
	float R = 1.0 + (rj - 1.0) * (1.0 - exp (-max (x, 0.0) * 0.8)) + max (x, 0.0) * spread
		+ max (0.0, x - 0.5 * len) * pc.diamond.w;
	R *= 1.0 + 0.12 * D * sin (2.0 * PI * x / cell) * exp (-x / (5.0 * cell));
	R *= 1.0 + turb * (n1 - 0.5) * 0.9;
	float u = r / R;
	// radial profile: a sharp jet boundary at the exit, diffusing downstream
	float mixing = smoothstep (0.0, 0.6, xi);
	float edge = mix (0.05, 0.5, mixing);
	float prof = (1.0 - smoothstep (1.0 - edge, 1.0 + edge, u)) * mix (1.0, exp (-1.2 * u * u), mixing);
	float rho = prof / (R * R); // density relative to the exit
	float fadeIn = smoothstep (-0.15, 0.1, x), fadeOut = smoothstep (1.6, 1.15, xi);

	// hot core, cooling as the jet mixes
	float lam = mix (1.0, 0.3 + 1.4 * n2, min (turb, 1.0));
	vec3 e = pc.core.rgb * rho * exp (-3.0 * xi * xi) * lam;
	// shock cells: a bright Mach disc across the jet in each, and the
	// oblique shocks between them (the diamonds)
	if (D > 0.01) {
		float c = x / cell - 0.6;
		float k = max (floor (c + 0.5), 0.0);
		float dx = (c - k) * cell;
		float fade = exp (-k / 2.5) * smoothstep (-0.6, -0.45, c);
		float disc = exp (-Sq (dx / (0.07 * cell))) * exp (-Sq (Sq (r / (0.5 * rj))));
		float tri = 1.0 - abs (2.0 * fract (x / cell + 0.4) - 1.0);
		float oblique = exp (-Sq ((r - rj * 0.85 * tri) / (0.07 * rj)));
		e += pc.diamond.rgb * (D * fade * (disc + 0.25 * oblique));
	}
	// afterburning in the mixing layer
	float ab = smoothstep (0.08, 0.35, xi) * exp (-1.5 * xi) * exp (-Sq ((u - 0.75) / 0.5)) / R;
	e += pc.outer.rgb * ab * (0.15 + 2.0 * n1 * n2);
	// smoke downstream
	float smoke = pc.misc.z * smoothstep (0.2, 1.0, xi) * exp (-1.5 * u * u) / R * (0.3 + 1.4 * n1);
	return vec4 (e, smoke) * fadeIn * fadeOut;
}

void main ()
{
	float re = pc.shape.x;
	vec3 d = normalize (vPos);
	vec3 o = (pc.toLocal * vec4 (0.0, 0.0, 0.0, 1.0)).xyz / re;
	vec3 dl = mat3 (pc.toLocal) * d;
	// the ray through the box (t in exit radii)
	vec3 bmax = vec3 (pc.shape2.w, pc.shape2.w, pc.shape2.z), bmin = vec3 (-bmax.xy, -0.2);
	vec3 inv = 1.0 / mix (dl, vec3 (1e-8), lessThan (abs (dl), vec3 (1e-8)));
	vec3 ta = (bmin - o) * inv, tb = (bmax - o) * inv;
	vec3 tlo = min (ta, tb), thi = max (ta, tb);
	float tIn = max (max (tlo.x, tlo.y), tlo.z);
	float t1 = min (min (thi.x, thi.y), thi.z);
	// each pixel once: from the far side of the box
	if (length (vPos) / re < 0.5 * (tIn + t1)) discard;
	float t0 = max (tIn, 0.0);
	float zs = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (zs > 0.0) t1 = min (t1, zs / (d.z * re));
	if (t1 <= t0) discard;

	float chord = t1 - t0;
	int n = int (clamp (ceil (chord / 0.3), 12.0, 96.0));
	float dt = chord / float (n);
	float jit = Hash (uvec3 (uvec2 (gl_FragCoord.xy), floatBitsToUint (pc.misc.x)));

	// smoke lighting: sunlight (forward scattering), ambient light
	vec3 Ls = mat3 (pc.toLocal) * frame.sunDir.xyz;
	float g = 0.4, cosT = dot (dl, Ls);
	float phase = (1.0 - g * g) / (4.0 * PI * pow (1.0 + g * g - 2.0 * g * cosT, 1.5));
	vec3 lit = 0.9 * (frame.sunColor.rgb * pc.misc2.z * phase * 4.0 * PI * 0.5 + frame.ambient.rgb);

	float T = 1.0;
	vec3 emit = vec3 (0.0), scat = vec3 (0.0);
	for (int i = 0; i < n; i++) {
		vec3 p = o + dl * (t0 + (float (i) + jit) * dt);
		vec4 m = Medium (p);
		float Ts = exp (-m.a * dt);
		float w = m.a > 1e-5 ? (1.0 - Ts) / m.a : dt; // emission absorbed within the step
		emit += T * w * m.rgb;
		scat += T * (1.0 - Ts) * lit;
		T *= Ts;
		if (T < 0.01) break;
	}
	// add-on exhaust textures tint the emission
	if (pc.misc2.x > 0.0) {
		vec3 tc = textureLod (exhaustTex, vec2 (0.12, 0.3), 3.0).rgb;
		float l = dot (tc, vec3 (0.2126, 0.7152, 0.0722));
		if (l > 1e-3) emit *= mix (vec3 (1.0), tc / l, pc.misc2.x);
	}
	outColor = vec4 (emit + scat, 1.0 - T);
}
