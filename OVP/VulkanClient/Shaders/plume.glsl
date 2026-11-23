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
	vec4 shape2;   // shock cell spacing, shock diamond strength, attitude thruster cone (1), cells over which the diamonds fade (1/e)
	vec4 core;     // core emission (rgb), vacuum fraction
	vec4 diamond;  // shock diamond emission (rgb), spread of the mixing layer
	vec4 outer;    // afterburning emission (rgb), streakiness (0..1: the lower the Isp, the more streaks)
	vec4 misc;     // time [s], turbulence drift [exit radii/s], smoke extinction, turbulence
	vec4 misc2;    // texture tint amount, seed, sunlight reaching the plume, shutter blur (how far the gas moves in an exposure [exit radii])
	vec4 bend;     // crossflow direction (xy, plume space), bending scale (z; 0: none), length of the stiff core (w)
	vec4 boxMin;   // bounding box (plume space, exit radii)
	vec4 boxMax;
	vec4 hit;      // where the jet meets a surface (xyz, plume space), w: its heat there (0: no surface)
	vec4 hitN;     // the surface normal (xyz, plume space, towards the jet), w: jet radius there
	vec4 hit2;     // reach of the gas spreading along the surface, fraction of the jet blocked, -, -
};

const uint MAX_PLUMES = 64u; // per frame slot

layout(set = PLUME_SET, binding = PLUME_BINDING, std430) readonly buffer PlumeBuffer {
	uvec4 plumeCount;      // plumes per frame slot
	PlumeParams plumes[];  // slot * MAX_PLUMES + index
};
layout(set = PLUME_SET, binding = PLUME_BINDING + 1) uniform sampler3D plumeShapeNoise;
layout(set = PLUME_SET, binding = PLUME_BINDING + 2) uniform sampler3D plumeDetailNoise;

float PlumeSq (float x) { return x * x; }

// a value in [0,1) per integer cell (the low-Isp streaks' pattern)
float PlumeHash (ivec3 c)
{
	uint h = uint (c.x) * 0x8da6b343u ^ uint (c.y) * 0xd8163841u ^ uint (c.z) * 0xcb1ab31fu;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h >> 8) / 16777216.0;
}

// smooth value noise over a (across, along) plane: cells of 1 x 1
float PlumeValue2 (vec2 q, int seed)
{
	ivec2 c = ivec2 (floor (q));
	vec2 f = fract (q);
	f = f * f * (3.0 - 2.0 * f);
	float a = PlumeHash (ivec3 (c, seed)), b = PlumeHash (ivec3 (c + ivec2 (1, 0), seed));
	float d = PlumeHash (ivec3 (c + ivec2 (0, 1), seed)), e = PlumeHash (ivec3 (c + ivec2 (1, 1), seed));
	return mix (mix (a, b, f.x), mix (d, e, f.x), f.y);
}

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
// (ld: the ray's direction, plume space: the low-Isp streaks are seen across it)
vec4 PlumeMedium (PlumeParams P, vec3 p, vec3 ld)
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
	// (beyond the envelope's widest reach, three times the radius it could
	// have there, nothing of it is left (all terms fall off as exp (-u^2)
	// or faster): no noise fetched there, most of a cone's box)
	if (P.hit.w <= 0.0) {
		float Rmax = (1.0 + (rj - 1.0) * (1.0 - exp (-max (x, 0.0) * 0.8)) + max (x, 0.0) * spread
			+ max (0.0, x - 0.5 * len) * P.diamond.w + 0.3 * bend) * (1.0 + 0.12 * D) * (1.0 + 0.45 * turb);
		if (r > 3.0 * max (Rmax, rj) || xi > 1.6) return vec4 (0.0);
	}
	// (the noise stretched along the flow, about three times: streaks along
	// the jet, as photographs show, not round cells)
	// Blurred by the shutter: in an exposure the gas moves B exit radii
	// (misc2.w: at the exit, at the exhaust's speed, from the Isp), slowing
	// to a tenth as it mixes with the air. Each noise, averaged along the
	// flow over B, is stretched to its own length (L1, L2: the cells' along
	// the flow) plus B, its contrast down by the square root of the ratio:
	// a fast jet's core steady streaks, as in photographs, its eddies seen
	// downstream where it has slowed. The drift itself (misc.y) is a
	// visible rate, not the gas's (at its speed the noise moved several of
	// its periods a frame: a random flicker).
	// Boiling: the fine noise drifts faster than the coarse one that warps
	// it, so the eddies change shape as they go, not only slide.
	const float L1 = 16.0, L2 = 3.0;
	float B = P.misc2.w * mix (1.0, 0.1, smoothstep (0.05, 1.0, xi));
	float s1 = L1 / (L1 + B), s2 = L2 / (L2 + B);
	vec3 q = vec3 (p.xy, x) + P.misc2.y * 7.31;
	float t = P.misc.x * P.misc.y;
	const vec3 STRETCH = vec3 (1.0, 1.0, 0.35);
	float n1 = texture (plumeShapeNoise, vec3 (q.xy, (q.z - t) * s1) * STRETCH * 0.045).r;
	n1 = 0.5 + (n1 - 0.5) * sqrt (s1);
	float n2 = texture (plumeDetailNoise, vec3 (q.xy, (q.z - 1.7 * t) * s2) * STRETCH * 0.23 + 0.4 * n1).r;
	n2 = 0.5 + (n2 - 0.5) * sqrt (s2);
	// Darker streaks along the flow now and then: cooler, fuel-rich film
	// off the nozzle wall or the injector's edge, as photographs show. A
	// few azimuths at a time (the noise around a circle: no seam), each
	// for a second or so (the noise's third axis: time, and the distance
	// down the jet over twice its length, so that one rides out with the
	// flow), from near the exit to where the jet has mixed; none on the
	// axis, where the azimuth means nothing (STREAK_DARK: the core's
	// light taken there)
	const float STREAK_DARK = 0.7, STREAK_DARK_LOW = 0.85;
	float streak = 0.0, streakLow = 0.0, streakEnv = smoothstep (0.0, 0.08, xi) * (1.0 - smoothstep (0.7, 1.2, xi));
	if (P.shape2.z < 0.5 && xi < 1.2) {
		float ang = atan (p.y, p.x);
		float sk = texture (plumeShapeNoise, vec3 (0.5 * cos (ang), 0.5 * sin (ang), P.misc.x * 0.35 + (x - t) / (2.0 * len)) + P.misc2.y * 0.137).r;
		streak = smoothstep (0.6, 0.7, sk) * streakEnv * smoothstep (0.05, 0.3, r);
	}

	// envelope radius
	float R = 1.0 + (rj - 1.0) * (1.0 - exp (-max (x, 0.0) * 0.8)) + max (x, 0.0) * spread
		+ max (0.0, x - 0.5 * len) * P.diamond.w + 0.3 * bend;
	R *= 1.0 + 0.12 * D * sin (6.2831853 * x / cell) * exp (-x / (5.0 * cell));
	// (ragged by the finer noise too as the jet mixes: a smooth cone was it)
	R *= 1.0 + turb * ((n1 - 0.5) * 0.9 + (n2 - 0.5) * 0.4 * smoothstep (0.1, 0.5, xi));
	float u = r / R;
	// A low-Isp engine's streaks (streakiness, outer.w) are many more and
	// darker (STREAK_DARK_LOW), fibres rather than bands: two sizes (about
	// six and sixteen cells across the jet, about half dark at once),
	// broken along the flow into filaments a sixth of the jet long (the
	// finer ones shorter) that ride out with it, wandering a little across
	// it, fewer as the jet mixes. A sooty plume is seen by its near side,
	// so they are sheets along the line of sight, by the place across the
	// jet as seen (fine wedges around the axis, as the occasional ones, a
	// ray crosses dozens of: they averaged out); none seen end-on.
	float K = P.outer.w;
	if (K > 0.0 && P.shape2.z < 0.5 && xi < 1.2) {
		float side = length (ld.xy);
		vec2 w = side > 1e-4 ? vec2 (-ld.y, ld.x) / side : vec2 (1.0, 0.0);
		float sl = dot (p.xy, w) / R; // -1 to 1 across the jet as seen
		// (value noise: the clouds' billowy noise is mostly above any
		// threshold that leaves gaps; the cells' along the flow wrapped to
		// keep their indices small)
		int seed = int (P.misc2.y);
		float along = mod (P.misc.x * 0.5 + (x - t) / (0.18 * len), 4096.0);
		sl += 0.24 * (PlumeValue2 (vec2 (0.4 * along, 0.5), seed + 11) - 0.5);
		float sf = 0.6 * PlumeValue2 (vec2 (sl * 3.0, along), seed)
			+ 0.4 * PlumeValue2 (vec2 (sl * 8.0, 1.7 * along + 37.0), seed + 5);
		streakLow = K * smoothstep (0.44, 0.55, sf) * smoothstep (0.15, 0.5, side) * streakEnv
			* (1.0 - 0.5 * smoothstep (0.2, 0.8, xi));
	}
	// radial profile: a sharp jet boundary at the exit, diffusing downstream
	float mixing = smoothstep (0.0, 0.6, xi);
	float edge = mix (0.05, 0.5, mixing);
	float prof = (1.0 - smoothstep (1.0 - edge, 1.0 + edge, u)) * mix (1.0, exp (-1.2 * u * u), mixing);
	float rho = prof / (R * R); // density relative to the exit
	float fadeIn = smoothstep (-0.15, 0.1, x), fadeOut = smoothstep (1.6, 1.15, xi);
	// a jet bent over by a crossflow entrains air several times faster than
	// a free jet: it cools and dilutes within a few diameters of turning
	if (P.bend.z > 0.0) fadeOut *= exp (-PlumeSq (bend / (0.75 * P.bend.z + 2.5)));

	// hot core, cooling as the jet mixes
	float lam = mix (1.0, 0.3 + 1.4 * n2, min (turb, 1.0)) * (1.0 - STREAK_DARK * streak);
	vec3 e = P.core.rgb * rho * exp (-3.0 * xi * xi) * lam;
	if (P.shape2.z > 0.5) {
		// attitude thrusters: a narrow cone whose brightness across falls off
		// slowly (density times path, about 1/R), with a brighter axis
		float cone = prof / R * exp (-1.6 * xi) * (0.35 + 0.65 * exp (-6.0 * u * u));
		e = P.core.rgb * cone * 0.1 * lam;
		// (white-hot where it leaves the nozzle)
		float peak = max (P.core.r, max (P.core.g, P.core.b));
		e += mix (P.core.rgb, vec3 (peak), 0.6) * (exp (-max (x, 0.0) / 1.2) * exp (-4.0 * u * u) * 0.5);
	}
	// shock cells: a bright Mach disc across the jet in each, and the
	// oblique shocks between them (the diamonds)
	if (D > 0.01) {
		float c = x / cell - 0.6;
		float k = max (floor (c + 0.5), 0.0);
		float dx = (c - k) * cell;
		float fade = exp (-k / P.shape2.w) * smoothstep (-0.6, -0.45, c);
		float disc = exp (-PlumeSq (dx / (0.07 * cell))) * exp (-PlumeSq (PlumeSq (r / (0.5 * rj))));
		float tri = 1.0 - abs (2.0 * fract (x / cell + 0.4) - 1.0);
		float oblique = exp (-PlumeSq ((r - rj * 0.85 * tri) / (0.07 * rj)));
		e += P.diamond.rgb * (D * fade * (disc + 0.25 * oblique));
	}
	// afterburning in the mixing layer
	float ab = smoothstep (0.08, 0.35, xi) * exp (-1.5 * xi) * exp (-PlumeSq ((u - 0.75) / 0.5)) / R;
	e += P.outer.rgb * ab * (0.15 + 2.0 * n1 * n2) * (1.0 - 0.6 * STREAK_DARK * streak);
	// smoke downstream
	float smoke = P.misc.z * smoothstep (0.2, 1.0, xi) * exp (-1.5 * u * u) / R * (0.3 + 1.4 * n1);
	// (the low-Isp streaks: all of the jet's light, deeply - so bright a
	// jet is far up the tone curve's shoulder, where 70 % less is hardly
	// seen - and evenly: kept redder, a near-white jet's turned pink; soot
	// as smoke scattered the sunlight: grey haze, not darker)
	e *= 1.0 - STREAK_DARK_LOW * streakLow;
	vec4 m = vec4 (e, smoke) * fadeIn * fadeOut;

	// a surface in the way: the jet ends there, and its gas spreads along
	// the surface in a thin layer, thickening and cooling outwards
	if (P.hit.w > 0.0) {
		vec3 rel = p0 - P.hit.xyz;
		float h = dot (rel, P.hitN.xyz), Rh = P.hitN.w;
		float blocked = P.hit2.y;
		m *= mix (1.0, smoothstep (0.0, 0.4 * Rh, h), blocked);
		float rw = length (rel - h * P.hitN.xyz);
		float reach = P.hit2.x;
		float delta = 0.3 * Rh + 0.12 * rw; // layer thickness
		// (to nothing at the box's side, 1.3 reach + Rh from the hit:
		// Plumes.cpp; cut off there, a long jet's layer showed its edges)
		float wall = exp (-max (h, 0.0) / delta) * smoothstep (-0.1 * Rh, 0.0, h) * exp (-PlumeSq (rw / reach))
			* (1.0 - smoothstep (reach, 1.3 * reach + Rh, rw)) * Rh / (delta * (Rh + rw)) * blocked;
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

// The ray lo + t ld (plume space) clipped to the cone outside which
// PlumeMedium is nothing (its early-out: r > 3 max (Rmax, rj), z < -0.15):
// r <= A + B (z + 0.15), A and B its bound at the exit and its widening
// with the turbulence's and shock cells' most. Not for bent jets or one
// meeting a surface. False: the ray misses it. (Rays through a box's
// corners stepped through nothing: attitude thrusters' boxes in front of
// a cockpit's windows, 9 ms)
bool PlumeClip (PlumeParams P, vec3 lo, vec3 ld, inout float t0, inout float t1)
{
	if (P.bend.z > 0.0 || P.hit.w > 0.0) return true;
	float F = 3.0 * (1.0 + 0.12 * P.shape2.y) * (1.0 + 0.45 * P.misc.w);
	float A = F * max (P.shape.z, 1.0), B = F * (P.shape.w + P.diamond.w);
	// the cone r = B w, w from its apex
	float w0 = lo.z + 0.15 + A / B;
	float B2 = B * B;
	float a = dot (ld.xy, ld.xy) - B2 * ld.z * ld.z;
	float b = 2.0 * (dot (lo.xy, ld.xy) - B2 * w0 * ld.z);
	float c = dot (lo.xy, lo.xy) - B2 * w0 * w0;
	float disc = b * b - 4.0 * a * c;
	if (abs (a) < 1e-6 * (dot (ld, ld) + B2)) return true; // (along its side: unclipped)
	if (disc < 0.0) return a < 0.0; // (all inside, or all outside)
	float s = sqrt (disc);
	float r1 = (-b - sign (a) * s) / (2.0 * a), r2 = c / (a * r1); // (r1 r2 = c / a; stable)
	if (abs (a * r1) < 1e-20) return true;
	float lo_ = min (r1, r2), hi = max (r1, r2);
	if (a > 0.0) { // inside between the roots
		t0 = max (t0, lo_), t1 = min (t1, hi);
	} else { // inside before and after them (one of the two nappes, the other one behind the nozzle, outside the box)
		if (t1 <= lo_ || t0 >= hi) return true;
		if (t0 >= lo_) t0 = max (t0, hi);
		else if (t1 <= hi) t1 = min (t1, lo_);
	}
	return t1 > t0;
}

// marched in steps that follow the jet's width (MarchPlume): attitude
// thrusters, and the smooth jets without shock cells or a surface in the way
bool PlumeAdaptive (PlumeParams P)
{
	return P.shape2.z > 0.5 || (P.shape2.y < 0.01 && P.hit.w <= 0.0);
}

struct PlumeLight {
	vec3 emit;  // emitted light reaching the ray origin
	vec3 scat;  // light scattered by smoke towards the ray origin
	float T;    // transmittance
	float tAvg; // emission-weighted distance [m]
};

// March from t0 to t1 (exit radii, plume space ray lo + t ld) in n steps;
// lit: light on the smoke (sun and ambient, without the phase function);
// stepScale: of the adaptive steps (finer: < 1)
PlumeLight MarchPlume (PlumeParams P, vec3 lo, vec3 ld, float t0, float t1, int n, float jit, vec3 sunLit, vec3 ambLit, vec3 sunDir, bool adaptive, float stepScale)
{
	PlumeLight r = PlumeLight (vec3 (0.0), vec3 (0.0), 1.0, 0.0);
	float dt = (t1 - t0) / float (n);
	// smoke: forward scattering of sunlight
	vec3 Ls = mat3 (P.toLocal) * sunDir;
	float g = 0.4, cosT = dot (ld, Ls);
	float phase = (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5); // (times 4 pi)
	vec3 lit = 0.9 * (sunLit * P.misc2.z * phase * 0.5 + ambLit);
	float wSum = 0.0;
	// (adaptive, attitude thrusters: steps of a quarter of the cone's radius where the
	// ray is, 0.3 to 1.5 exit radii (the detail noise's wavelength is 4):
	// fine at the nozzle, coarser as the smooth cone widens. A uniform 0.3
	// took the 96 steps across every box; thirty of them near the camera, a
	// Shuttle holding attitude, cost 14 ms a frame; adaptive 5, the image
	// the same (rcs_day: mean difference 0.3 of 255, the runs' own 0.05).
	// Smooth main jets likewise, by their envelope radius: the wide bells
	// in vacuum filled the screen behind the engines (Shuttle at 390 s
	// 5.7 -> 1.8 ms, plume_vac 4.3 -> 1.2; ORBITER_VK_FRAMETIME=0.016
	// renders differ by about what another jitter seed gives). Never
	// fewer than n steps' worth of the ray: long rays were cut short)
	// Jets with shock cells or meeting a surface (the others: adapt): the
	// fine steps within the jet's envelope up to some six cells (where the
	// cells have faded), adaptive in the rest of it and in the gas spread
	// over the surface, and across the empty box half the distance to where
	// the medium can begin - the Shuttle's SRB flame drawn its full 120 m at
	// sea level meets the pad's ground, its box a cube of 180 m each pixel
	// of which took 192 steps (plumes 0.4 -> 2.4 ms in a view of the pad)
	bool adapt = adaptive && PlumeAdaptive (P); // (then n: the most steps)
	bool tail = adaptive && !adapt;
	bool att = P.shape2.z > 0.5;
	float dtMin = max (0.3 * stepScale, (t1 - t0) / float (n)); // (n steps reach t1)
	float dtFine = (t1 - t0) / float (n), xCells = 6.0 * P.shape2.x;
	float tc = t0;
	for (int i = 0; i < n; i++) {
		if (adapt || tail) {
			if (tc >= t1) break;
			vec3 pc = lo + ld * tc;
			float x = max (pc.z, 0.0);
			float R = att ? 1.0 + x * P.shape.w : 1.0 + (P.shape.z - 1.0) * (1.0 - exp (-0.8 * x)) + x * P.shape.w;
			dt = clamp (0.25 * stepScale * R, dtMin, max (1.5 * stepScale, dtMin));
			if (tail) {
				// (the envelope: PlumeMedium's, with room for its bend, widening
				// past half the length, turbulence and cells; nothing beyond three
				// times it)
				float Re = (R + max (0.0, x - 0.5 * P.shape.y) * P.diamond.w) * 1.6;
				float freeJet = length (pc.xy) - abs (PlumeBend (P, x)) - 3.0 * Re;
				float free = freeJet;
				if (P.hit.w > 0.0) {
					vec3 rel = pc - P.hit.xyz;
					float h = dot (rel, P.hitN.xyz), rw = length (rel - h * P.hitN.xyz);
					free = min (free, max (h - 5.0 * (0.3 * P.hitN.w + 0.12 * rw), rw - 2.5 * P.hit2.x));
				}
				if (free > 0.0) dt = max (dt, 0.5 * free);
				else if (freeJet <= 0.0 && x < xCells) dt = dtFine;
			}
			dt = min (dt, t1 - tc);
		}
		float t = adapt || tail ? tc + jit * dt : t0 + (float (i) + jit) * dt;
		tc += dt;
		vec4 m = PlumeMedium (P, lo + ld * t, ld);
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
