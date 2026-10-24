// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Procedural surface detail close to the camera, below the resolution of
// the surface textures: a height field of craters (airless bodies, eroded
// on Mars), rocks and grain, shading the surface through its normal, and
// albedo variations (fresh crater ejecta, dark rocks). Patterns are
// anchored to the planet (positions relative to the 1 km-snapped origin,
// cell sizes dividing 1 km), so they stay put as the origin moves.
// Requires atmosphere.glsl (atm) and Hash/ValueNoise of tile.frag.

const int DETAIL_NONE = 0, DETAIL_AIRLESS = 1, DETAIL_MARS = 2, DETAIL_EARTH = 3;

vec4 DetailHash4 (ivec3 c, int period, int seed)
{
	c = c - period * ivec3 (floor (vec3 (c) / float (period)));
	uint h = uint (c.x) * 73856093u ^ uint (c.y) * 19349663u ^ uint (c.z) * 83492791u ^ uint (seed) * 2654435761u;
	vec4 r;
	for (int k = 0; k < 4; k++) {
		h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
		r[k] = float (h & 0xffffu) / 65535.0;
		h += 0x9e3779b9u;
	}
	return r;
}

float DetailSq (float x) { return x * x; }

// one scale of craters (cells of size s): height [m], and the albedo
// factor of fresh ejecta (out)
float CraterLayer (vec3 p, float s, float density, float fresh, int seed, inout float albedo)
{
	ivec3 c = ivec3 (floor (p / s));
	vec4 r = DetailHash4 (c, int (1000.0 / s + 0.5), seed);
	if (r.w > density) return 0.0;
	vec3 centre = (vec3 (c) + 0.2 + 0.6 * r.xyz) * s;
	float R = s * (0.08 + 0.12 * fract (r.w * 7.31 / max (density, 1e-3)));
	float x = length (p - centre) / R;
	if (x > 2.2) return 0.0;
	float age = fract (r.x * 13.7 + r.y * 3.1); // 0: fresh
	float soft = mix (1.0, 0.45, age);          // older craters are shallower, softer
	float h = 0.0;
	if (x < 1.0) h = R * 0.22 * soft * (x * x - 1.0);                   // bowl
	h += R * 0.07 * soft * exp (-DetailSq ((x - 1.0) / (0.22 + 0.2 * age))); // rim
	if (age < fresh) albedo *= 1.0 + 0.35 * (1.0 - age / fresh) * exp (-DetailSq ((x - 1.2) / 0.6)); // bright ejecta
	return h;
}

// rocks: small bumps, darker than the regolith
float RockLayer (vec3 p, float s, float density, int seed, inout float albedo)
{
	ivec3 c = ivec3 (floor (p / s));
	vec4 r = DetailHash4 (c, int (1000.0 / s + 0.5), seed);
	if (r.w > density) return 0.0;
	vec3 centre = (vec3 (c) + 0.3 + 0.4 * r.xyz) * s;
	float R = s * (0.1 + 0.15 * r.x);
	float x = length (p - centre) / R;
	if (x >= 1.0) return 0.0;
	albedo *= 0.75 + 0.2 * r.y;
	return R * 0.7 * sqrt (1.0 - x * x);
}

// height of the detail at p [m] (planet frame, relative to the pattern
// origin); footprint: pixel size [m] (finer detail fades out)
float DetailHeight (vec3 p, int type, float footprint, out float albedo)
{
	albedo = 1.0;
	float h = 0.0;
	// (a scale shows when it covers several pixels)
	#define VIS(s) (1.0 - smoothstep (0.15, 0.5, footprint / (s)))
	if (type == DETAIL_AIRLESS || type == DETAIL_MARS) {
		float dens = type == DETAIL_AIRLESS ? 1.0 : 0.12;
		float fresh = type == DETAIL_AIRLESS ? 0.3 : 0.0;
		const float S[6] = float[] (200.0, 62.5, 25.0, 8.0, 4.0, 2.0);
		const float D[6] = float[] (0.35, 0.45, 0.55, 0.7, 0.75, 0.8);
		for (int i = 0; i < 6; i++) {
			float v = VIS (S[i]);
			if (v <= 0.0) break;
			float a = 1.0;
			h += v * CraterLayer (p, S[i], D[i] * dens, fresh, 11 + i, a);
			albedo *= mix (1.0, a, v);
		}
		float rd = type == DETAIL_MARS ? 0.6 : 0.15;
		for (int i = 0; i < 3; i++) {
			float s = i == 0 ? 2.0 : i == 1 ? 1.0 : 0.5;
			float v = VIS (s);
			if (v <= 0.0) break;
			float a = 1.0;
			h += v * RockLayer (p, s, rd, 31 + i, a);
			albedo *= mix (1.0, a, v);
		}
	}
	// grain and undulation (all types)
	float g = 0.0, amp = type == DETAIL_EARTH ? 0.25 : 0.8;
	for (int i = 0; i < 5; i++) {
		const float GS[5] = float[] (40.0, 10.0, 4.0, 1.0, 0.5); // (sizes dividing 1 km)
		float s = GS[i];
		float v = VIS (s);
		if (v <= 0.0) break;
		g += v * amp * s * 0.04 * (ValueNoise (p, s, int (1000.0 / s + 0.5)) - 0.5);
	}
	h += g;
	// albedo mottling
	float m = ValueNoise (p, 8.0, 125) * 0.4 + ValueNoise (p, 2.0, 500) * 0.6;
	albedo *= mix (1.0, 0.75 + 0.5 * m, VIS (2.0));
	#undef VIS
	return h;
}

// Perturb the shading normal Nv (view space) and the albedo of the surface
// at posView (distance dist from the camera)
void SurfaceDetail (vec3 posView, float dist, inout vec3 Nv, inout vec3 base)
{
	int type = int (atm.detail.x + 0.5);
	if (type == DETAIL_NONE || atm.radii.x <= 0.0) return;
	float fade = 1.0 - smoothstep (0.4 * atm.detail.z, atm.detail.z, dist);
	if (fade <= 0.0) return;
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 p = v2p * posView + atm.water.xyz;
	vec3 Np = v2p * Nv;
	// pixel size [m] (stretched at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0);
	// (one evaluation: the gradient from the screen-space derivatives of
	// the displaced surface)
	float a0;
	float h0 = DetailHeight (p, type, footprint, a0) * atm.detail.y * fade;
	vec3 q = posView + Nv * h0;
	vec3 n = cross (dFdx (q), dFdy (q));
	float ln = length (n);
	if (ln > 1e-12) {
		n /= ln;
		if (dot (n, Nv) < 0.0) n = -n;
		Nv = normalize (mix (Nv, n, fade));
	}
	base *= mix (1.0, a0, fade);
}

// Reflectance of the regolith of airless bodies: between Lambert and
// Lommel-Seeliger (no limb darkening), with the opposition surge (shadow
// hiding: brighter looking away from the sun). Returns the factor to use
// instead of N.L.
float RegolithReflectance (vec3 N, vec3 L, vec3 V, vec3 Ng)
{
	float mu0 = max (dot (N, L), 0.0), mu = max (dot (Ng, V), 1e-3);
	float ls = 2.0 * mu0 / (mu0 + mu);
	float g = acos (clamp (dot (L, V), -1.0, 1.0)); // phase angle
	float B = 0.6 / (1.0 + tan (0.5 * g) / 0.06);
	return mix (mu0, 0.5 * ls, 0.6) * (1.0 + B);
}
