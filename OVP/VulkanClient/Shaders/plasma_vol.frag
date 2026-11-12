// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Reentry plasma volume (Plasma.cpp): the ray through the pixel marched
// through the box around a heated vessel, up to the opaque scene (the
// prepass depth), in the flow's frame (x, y across, s downstream, units of
// the vessel's size), the gas's emission from the vessel's field (its
// outline seen from upstream, plasma_jfa.comp):
// - the shock layer ahead of the windward surface, wrapping round its
//   outline: white-yellow at the hull, salmon, pink, violet at its outer
//   edge; brighter where the flow meets the surface head-on
// - the sheets of hot gas peeling off the outline and streaming
//   downstream, widening, drifting outwards, cooling to magenta and
//   violet and fading over some vessel sizes
// - the dim whitish lee wake behind the hull
// - streaks along the flow, drifting downstream
// The near part (the vessel and its sheets) marched finely, the far part
// (the wake) coarsely. Additive, with the reactive mask.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D fieldTex;   // r: distance to the outline, g, b: windward, leeward s, a: incidence
layout(set = 1, binding = 1) uniform sampler2D prepass;    // view depth (w; 0: none)
layout(set = 1, binding = 2) uniform sampler3D detailNoise;

layout(push_constant) uniform Draw {
	mat4 toFlow;  // view -> flow frame
	vec4 tile;    // atlas offset (uv), tile scale (uv), the box's half width
	vec4 box;     // s: start, near/far split, end; the streaks' phase (noise periods, mod 2)
	vec4 look;    // glow, the streaks' stretch along the flow, shock layer thickness, size [m]
	vec4 misc;    // jitter frame, sheet length, wake length, intensity
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Hash (uvec3 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h) / 4294967296.0;
}

vec4 Field (vec2 xy)
{
	const float H = 0.5 / 256.0;
	vec2 uv = clamp (xy / pc.tile.w * 0.5 + 0.5, H, 1.0 - H);
	return textureLod (fieldTex, pc.tile.xy + uv * pc.tile.z, 0.0);
}

// the gas's colour by its place in the layer: 0 at the hull (white-yellow),
// salmon, pink-magenta, magenta-violet, 1 at its outer edge (violet-blue)
vec3 Ramp (float c)
{
	const vec3 k[5] = vec3[5] (vec3 (1.00, 0.85, 0.55), vec3 (1.00, 0.45, 0.32), vec3 (1.00, 0.20, 0.50),
		vec3 (0.70, 0.16, 0.80), vec3 (0.40, 0.28, 1.00));
	c = clamp (c, 0.0, 1.0) * 4.0;
	int i = min (int (c), 3);
	return mix (k[i], k[i + 1], c - float (i));
}

// streaks along the flow, moving downstream with the gas, longer the faster
// it goes (Plasma.cpp: phase, stretch) (0.35 .. 1.8, mean about 1)
float Streaks (vec3 q)
{
	float P = pc.box.w, g = pc.look.y;
	float n = textureLod (detailNoise, vec3 (q.xy * 1.6, q.z * 0.10 * g - P), 0.0).r * 0.65
		+ textureLod (detailNoise, vec3 (q.xy * 4.0 + 0.37, q.z * 0.25 * g - 2.5 * P), 0.0).r * 0.35;
	return clamp ((n - 0.475) * 2.6 + 1.0, 0.35, 1.8);
}

vec3 Emit (vec3 q)
{
	vec4 F = Field (q.xy);
	float d = F.r, sF = F.g, sB = F.b, ci = F.a;
	float s = q.z;
	float glow = pc.look.x, D = pc.look.z;
	float Lc = pc.misc.y, Lw = pc.misc.z;
	float px = 2.0 * pc.tile.w / 256.0;
	float shift = 0.3 * (1.0 - glow); // (a faint glow: no white-hot core, pink and violet)

	// the shock layer: ahead of the windward surface (its thickness along
	// the normal), wrapping round the outline
	const float EDGE = 0.35;
	float h = -1.0, heat = 0.0, fade = 1.0;
	float inner = smoothstep (0.0, 4.0 * px, -d);
	if (d < 0.0) {
		if (s <= sF) {
			h = (sF - s) * mix (EDGE, max (ci, 0.12), inner);
			heat = mix (0.6, sqrt (ci), inner);
		}
	} else {
		h = length (vec2 (d, max (sF - s, 0.0) * EDGE));
		heat = 0.6;
		fade = 1.0 - smoothstep (0.0, 4.0 * D, s - sF); // (into the sheets streaming off)
	}
	float eS = 0.0, xS = 0.0;
	if (h >= 0.0) {
		xS = h / D;
		eS = exp (-1.8 * xS) * (1.0 - smoothstep (0.8, 1.3, xS)) * heat * fade / D;
	}
	// the sheets peeling off the outline: centred a little out from it,
	// widening and drifting outwards downstream (their light spread over
	// their width), fading
	float u = s - sF, uu = max (u, 0.0);
	float eC = 0.0, yC = 0.0;
	if (u > -D) {
		float dc = 0.4 * D + 0.05 * uu, w = 0.6 * D + 0.045 * uu;
		yC = (d - dc) / w;
		eC = exp (-yC * yC) * exp (-uu / Lc) * smoothstep (-D, 3.0 * D, u) / w;
	}
	// the lee wake: behind the leeward surface, inside the outline, a faint
	// swirl nearest the outline (the gas drawn in from the sheets)
	float eW = 0.0;
	if (d < 0.0 && s > sB) {
		float v = s - sB;
		eW = exp (-v / Lw) * smoothstep (0.0, 0.5, v) * smoothstep (0.0, 0.1, -d) * exp (d / 0.4);
	}
	// (faded out before the box's end and sides: no edge where it is cut)
	float rim = (1.0 - smoothstep (0.7 * pc.box.z, pc.box.z, s)) * (1.0 - smoothstep (0.8 * pc.tile.w, pc.tile.w, max (abs (q.x), abs (q.y))));
	eS *= rim, eC *= rim, eW *= rim;
	if (eS + eC + eW < 1e-3) return vec3 (0.0);
	float N = Streaks (q);
	return Ramp (0.9 * xS + shift) * (eS * (0.8 + 0.2 * N))
		+ Ramp (0.06 + 0.22 * yC + 0.65 * sqrt (uu / Lc) + shift) * (0.3 * N * N * eC)
		+ vec3 (0.80, 0.66, 0.95) * (0.04 * N * eW);
}

void main ()
{
	vec3 dir = normalize (vPos);
	vec3 o = (pc.toFlow * vec4 (0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 dq = mat3 (pc.toFlow) * dir;
	float W = pc.tile.w;
	vec3 bmin = vec3 (-W, -W, pc.box.x), bmax = vec3 (W, W, pc.box.z);
	vec3 inv = 1.0 / mix (dq, vec3 (1e-12), lessThan (abs (dq), vec3 (1e-12)));
	vec3 ta = (bmin - o) * inv, tb = (bmax - o) * inv;
	vec3 tlo = min (ta, tb), thi = max (ta, tb);
	float tIn = max (max (tlo.x, tlo.y), tlo.z), tOut = min (min (thi.x, thi.y), thi.z);
	if (tOut <= max (tIn, 0.0)) discard;
	// each pixel once: from the far side of the box
	if (length (vPos) < 0.5 * (tIn + tOut)) discard;
	float t0 = max (tIn, 0.0), t1 = tOut;
	ivec2 f = min (ivec2 (gl_FragCoord.xy), textureSize (prepass, 0) - 1);
	float z = texelFetch (prepass, f, 0).w;
	if (z > 0.0) t1 = min (t1, z / dir.z);
	if (t1 <= t0) discard;

	// the near part (s below the split: the vessel, its shock layer and
	// sheets) and the far part (the wake)
	float sN = pc.box.y;
	float ts = abs (dq.z) > 1e-12 ? (sN - o.z) / dq.z : (o.z < sN ? 1e30 : -1e30);
	float a0, a1, b0, b1;
	if (dq.z >= 0.0) a0 = t0, a1 = min (t1, ts), b0 = max (t0, ts), b1 = t1;
	else a0 = max (t0, ts), a1 = t1, b0 = t0, b1 = min (t1, ts);
	float jit = Hash (uvec3 (uvec2 (gl_FragCoord.xy), uint (pc.misc.x)));
	float size = pc.look.w;
	vec3 L = vec3 (0.0);
	if (a1 > a0) {
		int n = clamp (int (ceil ((a1 - a0) / size / 0.045)), 4, 48);
		float dt = (a1 - a0) / float (n);
		vec3 acc = vec3 (0.0);
		for (int i = 0; i < n; i++) acc += Emit (o + dq * (a0 + (float (i) + jit) * dt));
		L += acc * (dt / size);
	}
	if (b1 > b0) {
		int n = clamp (int (ceil ((b1 - b0) / size / 0.3)), 2, 16);
		float dt = (b1 - b0) / float (n);
		vec3 acc = vec3 (0.0);
		for (int i = 0; i < n; i++) acc += Emit (o + dq * (b0 + (float (i) + jit) * dt));
		L += acc * (dt / size);
	}
	L *= pc.misc.w;
	// (a soft cap: seen edge-on along its sheets, or along its wake, the
	// optically thin gas would add up to tens of times its face-on light -
	// the exposure taken down to it, all else black)
	float lumL = dot (L, vec3 (0.2126, 0.7152, 0.0722)), cap = 5.0 * max (pc.misc.w, 0.05);
	L *= 1.0 / (1.0 + lumL / cap);
	outColor = vec4 (L, 0.0);
	float lum = dot (L, vec3 (0.2126, 0.7152, 0.0722));
	outReactive = clamp (lum / (1.0 + lum), 0.0, 0.9);
}
