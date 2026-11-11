// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Crash fireballs: the ray through the pixel marched through a fireball's
// grid (its box), up to the opaque scene (the prepass depth: soft where it
// meets the ground or the sea), at half resolution (fireball_comp.frag):
// - its density eroded by detail noise carried with it (thin tongues and
//   holes at its edge, the dense inside kept)
// - extinction by the burning gas and (far more) its soot: opaque where
//   sooty, the hot inside glimpsed through its gaps
// - emission: the blackbody radiance at its temperature (FbPlanck) times
//   the absorbed share; scattering: the sunlight through it (its grid of
//   optical depths: self-shadowed, with a floor of light scattered many
//   times), forward-peaked, and the ambient light (darker underneath),
//   times its albedo (pale hot gas, dark soot)
// - the air in front dims it and adds its own light (aerial perspective)
// Out: its light (rgb) and opacity (premultiplied; blended into the half
// images far to near), the reactive mask, and where it begins and turns
// opaque (the sprites behind it hidden: gpuparticle.frag)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#define FB_SET 1
#define FB_BINDING 0
#include "fireball.glsl"
layout(set = 1, binding = 1) uniform sampler3D gridA;       // density, temperature [kK], soot
layout(set = 1, binding = 2) uniform sampler3D gridB;       // optical depth towards the sun, the sky
layout(set = 1, binding = 3) uniform sampler3D gridC;       // the noise's coordinates (carried by the flow)
layout(set = 1, binding = 4) uniform sampler3D shapeNoise;  // (repeating)
layout(set = 1, binding = 5) uniform sampler3D detailNoise;
layout(set = 1, binding = 6) uniform sampler2D prepass;     // view depth (w; 0: none)
#define ATM_SET 2
#include "atmosphere.glsl"

layout(push_constant) uniform Draw {
	uint index;  // into the buffer
	uint frame;  // (the jitter's sequence)
	uint scale;  // 2: at half resolution (fireball_comp.frag), 1: full
} pc;

layout(location = 0) in vec3 vPos;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;
layout(location = 2) out vec4 outOcc;

float Hash (uvec3 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h) / 4294967296.0;
}

void main ()
{
	FireballParams P = fireballs[pc.index];
	vec3 d = normalize (vPos);
	// into ball space: the ray through its box
	vec3 o = (P.toBall * vec4 (0.0, 0.0, 0.0, 1.0)).xyz;
	vec3 db = mat3 (P.toBall) * d;
	vec3 inv = 1.0 / mix (db, vec3 (1e-8), lessThan (abs (db), vec3 (1e-8)));
	vec3 ta = (P.boxMin.xyz - o) * inv, tb = (P.boxMax.xyz - o) * inv;
	vec3 tlo = min (ta, tb), thi = max (ta, tb);
	float tIn = max (max (tlo.x, tlo.y), tlo.z), t1 = min (min (thi.x, thi.y), thi.z);
	if (t1 <= max (tIn, 0.0)) discard;
	// each pixel once: from the far side of the box
	if (length (vPos) < 0.5 * (tIn + t1)) discard;
	float t0 = max (tIn, 0.0);
	// (at half resolution: up to the nearest surface of the four pixels;
	// fireball_comp.frag takes the texels by that depth)
	int sc = int (pc.scale);
	ivec2 f = ivec2 (gl_FragCoord.xy) * sc, fmax = textureSize (prepass, 0) - 1;
	float zs = 0.0;
	for (int k = 0; k < sc * sc; k++) {
		float z = texelFetch (prepass, min (f + ivec2 (k % sc, k / sc), fmax), 0).w;
		if (z > 0.0 && (zs <= 0.0 || z < zs)) zs = z;
	}
	float tSurf = zs > 0.0 ? zs / d.z : 1e30;
	t1 = min (t1, tSurf);
	if (t1 <= t0) discard;

	float r = P.shape.x;
	vec3 size = P.boxMax.xyz - P.boxMin.xyz, isz = 1.0 / size;
	float slab = P.boxMin.w;
	float dt = P.boxMax.w;
	int n = min (int (ceil ((t1 - t0) / dt)), 256);
	float jit = Hash (uvec3 (uvec2 (gl_FragCoord.xy), pc.frame));
	vec3 seed = fract (vec3 (0.71, 0.37, 0.53) * P.time.w);
	// the light on it: the sun through the air to its centre (in the
	// planet's shadow at night), forward-peaked (the sunlit edges of the
	// billows seen against the sun bright), and the ambient light
	vec3 sun = frame.sunColor.rgb;
	if (atm.misc.x > 0.5) sun *= SurfaceSunlight (mat3 (atm.viewToPlanet) * P.centre.xyz + atm.camPos.xyz);
	float g = 0.35, cosT = dot (d, frame.sunDir.xyz);
	float phase = 0.6 * (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5) + 0.4; // (mean 1)
	vec3 sunL = sun * phase;
	vec3 amb = frame.ambient.rgb * frame.ambient.w;
	float soft = 1.0 / (0.12 * r);  // (fading out over this before the ground or the sea)
	float sig0 = P.flow.w * P.heat.w / r; // (fading out as a whole as its soot passes to the smoke volume)

	vec3 L = vec3 (0.0);
	float T = 1.0, tW = 0.0, wSum = 0.0, zIn = 0.0, zOp = 0.0, tLast = t0;
	for (int i = 0; i < n; i++) {
		float t = t0 + (float (i) + jit) * dt;
		if (t >= t1) break;
		vec3 pb = o + db * t;
		vec3 gc = FbGridCoord ((pb - P.boxMin.xyz) * isz, slab);
		vec4 A = textureLod (gridA, gc, 0.0);
		if (A.x < 0.002) continue;
		vec3 q = textureLod (gridC, gc, 0.0).xyz;
		// detail: small billows eating into its thin parts (the edge ragged,
		// tongues and holes), the dense inside kept
		// (normalized: about +-1; the shape noise's sd 0.073 about 0.67, the detail's 0.12 about 0.475)
		float dn = 0.6 * (textureLod (shapeNoise, q * 2.3 + seed, 0.0).r - 0.67) * 6.8 + 0.4 * (textureLod (detailNoise, q * 3.1 + seed.zxy, 0.0).r - 0.475) * 4.2;
		// (and finer: about 1/25 of its radius, the flame's texture close up)
		float df = (textureLod (shapeNoise, q * 6.1 + seed.yzx, 0.0).r - 0.67) * 6.8;
		dn += 0.35 * df;
		// (soot more: its small billows crisp - the cap's cauliflower)
		float ero = clamp ((0.3 + 0.3 * dn) * (1.0 + 0.5 * A.z) * P.tune.z, 0.0, 0.95);
		float rho = clamp ((A.x - ero) / (1.0 - ero), 0.0, 1.0);
		rho = smoothstep (0.0, 0.5, rho); // (a crisp edge to the billows: the density rising over a short way)
		rho *= clamp ((tSurf - t) * soft, 0.0, 1.0);
		if (rho <= 0.0) continue;
		float soot = A.z;
		float sig = rho * (FB_SIGMA_FLAME + FB_SIGMA_SOOT * soot) * sig0;
		float alb = mix (0.05, 0.17, soot); // (the hot gas scatters little: its soot does)
		// (its temperature varying a little with the detail: brighter and
		// darker flecks over the billows, not a smooth glow; more read as
		// streaks of marble)
		float Tk = A.y * 1000.0 * (1.0 + 0.03 * dn + 0.015 * df);
		vec2 tau = textureLod (gridB, FbGridCoordB ((pb - P.boxMin.xyz) * isz, slab), 0.0).xy;
		float sunF = 0.08 + 0.92 * (0.7 * exp (-0.7 * tau.x) + 0.3 * exp (-0.12 * tau.x));
		vec3 S = alb * (sunL * sunF + amb * (0.15 + 0.85 * exp (-0.5 * tau.y)));
		vec3 E = (1.0 - alb) * FbPlanck (Tk) * P.tune.w;
		float a = 1.0 - exp (-sig * dt);
		float w = T * a;
		L += w * (S + E);
		tW += w * t, wSum += w;
		T *= 1.0 - a;
		tLast = t;
		if (zIn <= 0.0 && T < 0.95) zIn = t * d.z;
		if (zOp <= 0.0 && T < 0.4) zOp = t * d.z;
		if (T < 0.004) break;
	}
	float alpha = 1.0 - T;
	if (alpha < 1e-4) discard;
	// aerial perspective: the air in front dims it; where it hides the
	// background, the air's own light (already in the background) is kept
	if (atm.misc.x > 0.5 && wSum > 0.0) {
		vec3 Ta;
		vec3 Lin = AtmTonemap (Inscatter (normalize (mat3 (atm.viewToPlanet) * d), tW / wSum, Ta));
		L = L * Ta + alpha * Lin;
	}
	outColor = vec4 (L, alpha);
	float lum = dot (L, vec3 (0.2126, 0.7152, 0.0722));
	outReactive = clamp (max (alpha, lum / (1.0 + lum)), 0.0, 0.9);
	if (zIn <= 0.0) zIn = tLast * d.z;
	if (zOp <= 0.0) zOp = tLast * d.z;
	outOcc = vec4 (1.0 / max (zIn, 1e-3), 1.0 / max (zOp, 1e-3), 0.0, T);
}
