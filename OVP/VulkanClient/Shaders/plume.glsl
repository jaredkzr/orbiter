// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Engine plume volumes (Plumes.cpp): the plumes of this frame, the medium
// of a plume and the march of a ray through it. Plume space: z along the
// flow from the nozzle exit, lengths in exit radii.
// The jet expands (or contracts) from the nozzle to the radius where its
// pressure matches the ambient pressure; in air, shock cells form along it
// (bright Mach discs), and after its core it mixes with the air, where
// fuel-rich exhaust burns (afterburning). In vacuum it spreads into a wide,
// faint bell. A crossflow bends it downstream. Where it meets a surface,
// the gas spreads along it (a wall jet).
// The including shader defines PLUME_SET and PLUME_BINDING (the buffer; the
// noise volumes follow it).

struct PlumeParams { // must match PlumeRenderer::Params
	mat4 toLocal;  // view -> plume space [m]
	vec4 shape;    // exit radius [m], length, jet radius after the exit, spread (tan of the half angle)
	vec4 shape2;   // shock cell spacing, shock diamond strength, -, -
	vec4 core;     // core emission (rgb), vacuum fraction
	vec4 diamond;  // shock diamond emission (rgb), spread of the mixing layer
	vec4 outer;    // afterburning emission (rgb), -
	vec4 misc;     // time [s], turbulence drift [exit radii/s], smoke extinction, turbulence
	vec4 misc2;    // texture tint amount, seed, sunlight reaching the plume, -
	vec4 bend;     // crossflow direction (xy, plume space), bending scale (z; 0: none), length of the stiff core (w)
	vec4 boxMin;   // bounding box (plume space, exit radii)
	vec4 boxMax;
	vec4 hit;      // where the jet meets a surface (xyz, plume space), w: its heat there (0: no surface)
	vec4 hitN;     // the surface normal (xyz, plume space, towards the jet), w: jet radius there
	vec4 hit2;     // reach of the gas spreading along the surface, -, -, -
};

const uint MAX_PLUMES = 64u; // per frame slot

layout(set = PLUME_SET, binding = PLUME_BINDING, std430) readonly buffer PlumeBuffer {
	uvec4 plumeCount;      // plumes per frame slot
	PlumeParams plumes[];  // slot * MAX_PLUMES + index
};
layout(set = PLUME_SET, binding = PLUME_BINDING + 1) uniform sampler3D plumeShapeNoise;
layout(set = PLUME_SET, binding = PLUME_BINDING + 2) uniform sampler3D plumeDetailNoise;

float PlumeSq (float x) { return x * x; }

// lateral displacement of the jet axis by the crossflow at distance x
// (jet in crossflow: x / (r d) = 1.6 (y / (r d))^(1/3), slope capped),
// after the jet's stiff core; the bent jet mixes out quickly (PlumeMedium)
float PlumeBend (PlumeParams P, float x)
{
	float rd = P.bend.z;
	if (rd <= 0.0) return 0.0;
	const float SC = 1.46; // where the slope reaches 4
	float z = max (x - P.bend.w, 0.0);
	float s = z / (1.6 * rd);
	return s < SC ? rd * s * s * s : rd * SC * SC * SC + 4.0 * (z - 1.6 * rd * SC);
}

// slope of the jet axis (lateral displacement per distance) at x
float PlumeBendSlope (PlumeParams P, float x)
{
	float rd = P.bend.z;
	if (rd <= 0.0) return 0.0;
	float s = max (x - P.bend.w, 0.0) / (1.6 * rd);
	return min (1.875 * s * s, 4.0);
}

// emission (rgb) and smoke extinction (a) per exit radius at p
vec4 PlumeMedium (PlumeParams P, vec3 p)
{
	vec3 p0 = p;
	if (p.z < -0.15 && P.hit.w <= 0.0) return vec4 (0.0);
	// the bent jet: distances across and along its curved axis
	float bend = PlumeBend (P, p.z), slope = PlumeBendSlope (P, p.z);
	p.xy -= P.bend.xy * bend;
	float along = dot (p.xy, P.bend.xy);
	p.xy += P.bend.xy * (along * inversesqrt (1.0 + slope * slope) - along);
	float x = p.z > 0.0 ? length (vec2 (p.z, bend)) : p.z;
	float r = length (p.xy);
	float len = P.shape.y, rj = P.shape.z, spread = P.shape.w;
	float cell = P.shape2.x, D = P.shape2.y;
	float xi = max (x, 0.0) / len;
	// turbulence: laminar at the exit, growing downstream
	float turb = P.misc.w * smoothstep (0.02, 0.5, xi);
	vec3 q = vec3 (p.xy, x - P.misc.x * P.misc.y) + P.misc2.y * 7.31;
	float n1 = texture (plumeShapeNoise, q * 0.045).r;
	float n2 = texture (plumeDetailNoise, q * 0.23 + 0.4 * n1).r;

	// envelope radius
	float R = 1.0 + (rj - 1.0) * (1.0 - exp (-max (x, 0.0) * 0.8)) + max (x, 0.0) * spread
		+ max (0.0, x - 0.5 * len) * P.diamond.w + 0.3 * bend;
	R *= 1.0 + 0.12 * D * sin (6.2831853 * x / cell) * exp (-x / (5.0 * cell));
	R *= 1.0 + turb * (n1 - 0.5) * 0.9;
	float u = r / R;
	// radial profile: a sharp jet boundary at the exit, diffusing downstream
	float mixing = smoothstep (0.0, 0.6, xi);
	float edge = mix (0.05, 0.5, mixing);
	float prof = (1.0 - smoothstep (1.0 - edge, 1.0 + edge, u)) * mix (1.0, exp (-1.2 * u * u), mixing);
	float rho = prof / (R * R); // density relative to the exit
	float fadeIn = smoothstep (-0.15, 0.1, x), fadeOut = smoothstep (1.6, 1.15, xi);
	// a jet bent over by a crossflow entrains air several times faster than
	// a free jet: it cools and dilutes within a few diameters of turning
	if (P.bend.z > 0.0) fadeOut *= exp (-PlumeSq (bend / (1.2 * P.bend.z + 4.0)));

	// hot core, cooling as the jet mixes
	float lam = mix (1.0, 0.3 + 1.4 * n2, min (turb, 1.0));
	vec3 e = P.core.rgb * rho * exp (-3.0 * xi * xi) * lam;
	// shock cells: a bright Mach disc across the jet in each, and the
	// oblique shocks between them (the diamonds)
	if (D > 0.01) {
		float c = x / cell - 0.6;
		float k = max (floor (c + 0.5), 0.0);
		float dx = (c - k) * cell;
		float fade = exp (-k / 2.5) * smoothstep (-0.6, -0.45, c);
		float disc = exp (-PlumeSq (dx / (0.07 * cell))) * exp (-PlumeSq (PlumeSq (r / (0.5 * rj))));
		float tri = 1.0 - abs (2.0 * fract (x / cell + 0.4) - 1.0);
		float oblique = exp (-PlumeSq ((r - rj * 0.85 * tri) / (0.07 * rj)));
		e += P.diamond.rgb * (D * fade * (disc + 0.25 * oblique));
	}
	// afterburning in the mixing layer
	float ab = smoothstep (0.08, 0.35, xi) * exp (-1.5 * xi) * exp (-PlumeSq ((u - 0.75) / 0.5)) / R;
	e += P.outer.rgb * ab * (0.15 + 2.0 * n1 * n2);
	// smoke downstream
	float smoke = P.misc.z * smoothstep (0.2, 1.0, xi) * exp (-1.5 * u * u) / R * (0.3 + 1.4 * n1);
	vec4 m = vec4 (e, smoke) * fadeIn * fadeOut;

	// a surface in the way: the jet ends there, and its gas spreads along
	// the surface in a thin layer, thickening and cooling outwards
	if (P.hit.w > 0.0) {
		vec3 rel = p0 - P.hit.xyz;
		float h = dot (rel, P.hitN.xyz), Rh = P.hitN.w;
		m *= smoothstep (0.0, 0.4 * Rh, h);
		float rw = length (rel - h * P.hitN.xyz);
		float reach = P.hit2.x;
		float delta = 0.3 * Rh + 0.12 * rw; // layer thickness
		float wall = exp (-max (h, 0.0) / delta) * smoothstep (-0.1 * Rh, 0.0, h) * exp (-PlumeSq (rw / reach))
			* Rh / (delta * (Rh + rw));
		float cool = exp (-rw / reach);
		float lamW = 0.25 + 1.5 * n1 * n2 * 2.0;
		vec3 ew = (P.core.rgb * P.hit.w * cool + P.outer.rgb * 0.5 * (1.0 - cool)) * wall * lamW;
		m += vec4 (ew, P.misc.z * wall * Rh * (0.3 + 1.4 * n1));
	}
	return m;
}

// the ray o + t d (view space, |d| = 1) through the plume's box: entry and
// exit (t in exit radii, plume space origin and direction)
bool PlumeBox (PlumeParams P, vec3 o, vec3 d, out vec3 lo, out vec3 ld, out float tIn, out float tOut)
{
	float re = P.shape.x;
	lo = (P.toLocal * vec4 (o, 1.0)).xyz / re;
	ld = mat3 (P.toLocal) * d;
	vec3 inv = 1.0 / mix (ld, vec3 (1e-8), lessThan (abs (ld), vec3 (1e-8)));
	vec3 ta = (P.boxMin.xyz - lo) * inv, tb = (P.boxMax.xyz - lo) * inv;
	vec3 tlo = min (ta, tb), thi = max (ta, tb);
	tIn = max (max (tlo.x, tlo.y), tlo.z);
	tOut = min (min (thi.x, thi.y), thi.z);
	return tOut > max (tIn, 0.0);
}

struct PlumeLight {
	vec3 emit;  // emitted light reaching the ray origin
	vec3 scat;  // light scattered by smoke towards the ray origin
	float T;    // transmittance
	float tAvg; // emission-weighted distance [m]
};

// March from t0 to t1 (exit radii, plume space ray lo + t ld) in n steps;
// lit: light on the smoke (sun and ambient, without the phase function)
PlumeLight MarchPlume (PlumeParams P, vec3 lo, vec3 ld, float t0, float t1, int n, float jit, vec3 sunLit, vec3 ambLit, vec3 sunDir)
{
	PlumeLight r = PlumeLight (vec3 (0.0), vec3 (0.0), 1.0, 0.0);
	float dt = (t1 - t0) / float (n);
	// smoke: forward scattering of sunlight
	vec3 Ls = mat3 (P.toLocal) * sunDir;
	float g = 0.4, cosT = dot (ld, Ls);
	float phase = (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5); // (times 4 pi)
	vec3 lit = 0.9 * (sunLit * P.misc2.z * phase * 0.5 + ambLit);
	float wSum = 0.0;
	for (int i = 0; i < n; i++) {
		float t = t0 + (float (i) + jit) * dt;
		vec4 m = PlumeMedium (P, lo + ld * t);
		float Ts = exp (-m.a * dt);
		float w = m.a > 1e-5 ? (1.0 - Ts) / m.a : dt; // emission absorbed within the step
		vec3 e = r.T * w * m.rgb;
		r.emit += e;
		r.scat += r.T * (1.0 - Ts) * lit;
		float we = dot (e, vec3 (1.0)) + r.T * (1.0 - Ts);
		r.tAvg += we * t;
		wSum += we;
		r.T *= Ts;
		if (r.T < 0.01) break;
	}
	r.tAvg = wSum > 0.0 ? r.tAvg / wSum * P.shape.x : 0.0;
	return r;
}
