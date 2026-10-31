// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Nebulae of the simulated sky: the gas and dust around the catalogue's
// luminous stars, lit by them (Nebulae.cpp). Each source star lights a
// sphere:
// - hot stars ionise the gas around them out to the ionisation front,
//   where their ionising photons are used up (a Stroemgren region shaped
//   by the gas, which follows the dust map); the ionised gas shines in its
//   emission lines (hydrogen, oxygen, nitrogen, ...);
// - the dust around every luminous star scatters its light (reflection
//   nebulae).
// The gas and dust are clumpy below the map's resolution: a log-normal
// field of noise multiplies them. (Needs galaxy.glsl.)

const int NEB_DIRS = 16;      // the front table: 16 x 16 directions (octahedral) per source
const int NEB_TILE = 32;      // texels per side of a tile of the bake's source lists

struct NebSource {
	vec4 pos;   // xyz: heliocentric Galactic [pc]; w: radius of the lit sphere [pc]
	vec4 light; // the star's light per band [L_sun,band]
	vec4 line;  // ionised gas: emission per band per n_e^2 [L_sun,band / pc^3 / cm^-6] (0: none), least excited,
	vec4 excite;// and the change to the most excited (NebExcitation)
	vec4 misc;  // x: first entry in the front table (< 0: none), y: largest front radius [pc],
	            // zw: radii of the natal cloud's core and envelope [pc] (NebNatalRadius)
	vec4 ion;   // x: log10 of the ionising photons [1/s]; the dust about the star the map misses,
	            // a shell (the star clears its surroundings) of density A x / (1 + x)^3 / r0,
	            // x = r / r0, A / 2 its column: y: A [A_V], z: r0 [pc]; w: -
};

#ifndef NEB_FRONT_PASS
layout(std430, set = 0, binding = 3) readonly buffer NebSources { NebSource neb[]; };
layout(std430, set = 0, binding = 4) readonly buffer NebFronts { float nebFront[]; };
// the bake's lists: first entry of each tile (6 faces x tiles x tiles, + 1), then the entries
layout(std430, set = 0, binding = 5) readonly buffer NebTiles { uint nebTile[]; };
#endif

// --- clumps: log-normal noise. The gas density is the smooth one times
// NebClump (mean square 1, so that the clumps keep the light of the
// ionised gas; the front pass uses the same field); the dust's times
// NebClumpDust (mean 1)

float NebHash (ivec3 p)
{
	uint h = uint (p.x) * 73856093u ^ uint (p.y) * 19349663u ^ uint (p.z) * 83492791u;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return float (h) * (2.0 / 4294967296.0) - 1.0;
}

float NebValueNoise (vec3 x)
{
	ivec3 i = ivec3 (floor (x));
	vec3 f = x - floor (x);
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float a = mix (NebHash (i), NebHash (i + ivec3 (1, 0, 0)), u.x);
	float b = mix (NebHash (i + ivec3 (0, 1, 0)), NebHash (i + ivec3 (1, 1, 0)), u.x);
	float c = mix (NebHash (i + ivec3 (0, 0, 1)), NebHash (i + ivec3 (1, 0, 1)), u.x);
	float d = mix (NebHash (i + ivec3 (0, 1, 1)), NebHash (i + ivec3 (1, 1, 1)), u.x);
	return mix (mix (a, b, u.y), mix (c, d, u.y), u.z);
}

const int NEB_OCTAVES = 5;
const float NEB_NOISE_VAR = 0.135; // variance of NebValueNoise

// fBm at x [pc], octaves finer than ~2 footprints [pc] faded out; var:
// its variance
float NebFbm (vec3 x, float footprint, out float var)
{
	float f = 0.0, lambda = gal.neb[1].z, amp = 1.0;
	var = 0.0;
	for (int k = 0; k < NEB_OCTAVES; k++) {
		float w = clamp (lambda / (2.0 * footprint) - 1.0, 0.0, 1.0);
		if (w <= 0.0) break;
		f += w * amp * NebValueNoise (x / lambda + float (k) * 17.31);
		var += w * w * amp * amp * NEB_NOISE_VAR;
		lambda *= 0.5, amp *= 0.5;
	}
	return f;
}

// (gas: mean square 1; dust: mean 1)
void NebClump (vec3 x, float footprint, out float gas, out float dust)
{
	float var;
	float f = NebFbm (x, footprint, var);
	float s = gal.neb[0].w; // sigma of the log density per unit fBm
	gas = exp (s * f - s * s * var);
	dust = exp (s * f - 0.5 * s * s * var);
}

// ionised gas density [cm^-3] where the smooth dust is rho [A_V / pc]
float NebGas (float rho) { return max (gal.neb[0].x * rho, gal.neb[0].y); }

// The natal cloud of an ionising star (young stars sit in the dense gas
// they formed from, which the dust map can't resolve): a dense core and an
// envelope, each n0 (1 + r/r0)^-2, its radius r0 [pc] such that it takes a
// share of the star's ionising photons (budget: Q / (4 pi alpha_B)
// [cm^-6 pc^3])
float NebNatalRadius (float budget, float n0, float share)
{
	return pow (3.0 * share * budget / (n0 * n0), 1.0 / 3.0);
}
// (a part's density squared at r; widened to at least w [pc] keeping its
// light: the mean square density over a texel)
float NebNatal2 (float r, float n0, float r0, float w)
{
	if (r0 <= 0.0) return 0.0;
	float re = max (r0, w);
	float x = 1.0 + r / re;
	float n = n0 * pow (r0 / re, 1.5) / (x * x);
	return n * n;
}
// the ionised gas density [cm^-3] at r from the star, with the smooth dust
// rho there [A_V / pc]; r0: the cloud's radii
float NebIonGas (float rho, float r, vec2 r0, float w)
{
	float g = NebGas (rho);
	return sqrt (g * g + NebNatal2 (r, gal.neb[2].x, r0.x, w) + NebNatal2 (r, gal.neb[2].z, r0.y, w));
}

// --- the front table: the radius of the ionised region by direction

vec2 NebOct (vec3 n) // direction -> [0,1]^2
{
	n /= abs (n.x) + abs (n.y) + abs (n.z);
	vec2 p = n.xy;
	if (n.z < 0.0) p = (1.0 - abs (p.yx)) * vec2 (p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return p * 0.5 + 0.5;
}

vec3 NebOctDir (vec2 uv) // [0,1]^2 -> direction
{
	vec2 p = uv * 2.0 - 1.0;
	vec3 n = vec3 (p, 1.0 - abs (p.x) - abs (p.y));
	if (n.z < 0.0) n.xy = (1.0 - abs (n.yx)) * vec2 (n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return normalize (n);
}

#ifndef NEB_FRONT_PASS
// (interpolated between the table's directions; y: the least of them)
vec2 NebFront2 (int base, vec3 dir)
{
	vec2 t = clamp (NebOct (dir) * float (NEB_DIRS) - 0.5, vec2 (0.0), vec2 (float (NEB_DIRS) - 1.001));
	ivec2 i = ivec2 (t);
	vec2 f = t - vec2 (i);
	int k = base + i.y * NEB_DIRS + i.x;
	vec4 r = vec4 (nebFront[k], nebFront[k + 1], nebFront[k + NEB_DIRS], nebFront[k + NEB_DIRS + 1]);
	return vec2 (mix (mix (r.x, r.y, f.x), mix (r.z, r.w, f.x), f.y), min (min (r.x, r.y), min (r.z, r.w)));
}
float NebFront (int base, vec3 dir) { return NebFront2 (base, dir).x; }

// Excitation of the ionised gas (0..1) at r [pc] from a star of log10 Q
// ionising photons/s, gas density n [cm^-3]: by the ionisation parameter
// U = Q / (4 pi r^2 n c): diluted light makes low-excitation gas (the
// diffuse ionised gas: weak [OIII], strong [NII], [SII])
float NebExcitation (float logQ, float r, float n)
{
	float logU = logQ - 48.554 - 2.0 * log (max (r, 1e-3)) / log (10.0) - log (max (n, 1e-3)) / log (10.0);
	return smoothstep (-3.5, -2.0, logU);
}

// Henyey-Greenstein phase function (per steradian)
float NebPhase (float c, float g)
{
	float d = 1.0 + g * g - 2.0 * g * c;
	return (1.0 - g * g) / (4.0 * GAL_PI * d * sqrt (d));
}

// Light [L_sun,band / pc^2 along the ray] of source si from the ray o + s d
// within [s0, s1], behind A0 (A_V) of dust from the observer at s0; pix:
// a texel's angle [rad]; jit: offset of the samples (0..1, per texel)
vec4 NebChord (int si, vec3 o, vec3 d, float s0, float s1, float A0, float pix, float jit)
{
	NebSource src = neb[si];
	vec3 c = src.pos.xyz;
	vec4 ext = 0.921 * gal.extinction; // (ln 10 / 2.5)
	float albedo = gal.neb[1].x, g = gal.neb[1].y;
	vec4 L = vec4 (0.0);
	float cell;

	// ionised gas: through the sphere holding its front, samples spaced
	// evenly in the angle seen from the star (the natal cloud's light is
	// concentrated about it)
	float tc = dot (c - o, d), b = max (sqrt (max (dot (c - o, c - o) - tc * tc, 0.0)), 0.01), rI = src.misc.y;
	float i0 = s0, i1 = s0;
	if (src.misc.x >= 0.0 && b < rI) {
		int base = int (src.misc.x);
		float hw = sqrt (rI * rI - b * b);
		i0 = max (s0, tc - hw), i1 = min (s1, tc + hw);
		// (narrowed to where the ray is within the front: the largest
		// radius can be far beyond it in this direction)
		const int NP = 16;
		float dp = (i1 - i0) / float (NP);
		int first = NP, last = -1;
		for (int k = 0; k < NP; k++) {
			vec3 v = o + d * (i0 + (float (k) + 0.5) * dp) - c;
			float r = length (v);
			if (r < NebFront (base, v / max (r, 1e-6))) first = min (first, k), last = k;
		}
		if (last < 0) i1 = i0; // (misses it)
		else {
			float j0 = i0 + float (max (first - 1, 0)) * dp, j1 = i0 + float (min (last + 2, NP)) * dp;
			i0 = j0, i1 = j1;
		}
	}
	if (src.misc.x >= 0.0 && b < rI && i1 > i0) {
		int base = int (src.misc.x);
		// (samples by the chord's length: the light can come from thin
		// rims, where the front meets denser gas)
		int N = int (clamp (ceil ((i1 - i0) / max (0.5, 2.0 * tc * pix)), 16.0, 96.0));
		float a0 = atan ((i0 - tc) / b), a1 = atan ((i1 - tc) / b);
		float da = (a1 - a0) / float (N);
		float wNatal = 0.5 * abs (tc) * pix; // (half a texel at the star)
		// (the dust between the lit sphere's entry and the ionised one's)
		float A = A0 + GalExtinctionN (o + d * s0, o + d * i0, 4), sPrev = i0, rhoPrev = -1.0;
		for (int i = 0; i < N; i++) {
			float s = tc + b * tan (a0 + (float (i) + jit) * da);
			float ds = da * (b * b + (s - tc) * (s - tc)) / b;
			vec3 x = o + d * s;
			float rho = GalDust (x, cell);
			A += (rhoPrev < 0.0 ? rho : 0.5 * (rho + rhoPrev)) * (s - sPrev);
			rhoPrev = rho, sPrev = s;
			vec3 v = x - c;
			float r = length (v);
			float gas, dust;
			NebClump (x, max (s * pix, (i1 - i0) / float (N)), gas, dust); // (no clumps finer than the samples: noise)
			float n = NebIonGas (rho, r, src.misc.zw, wNatal) * gas;
			// (the front between the table's directions: where the gas is
			// dense, the nearest that bounds it: the table's directions are
			// far apart (about 13 deg), and interpolating from one that
			// misses a cloud to one that meets it would light the cloud's
			// dense gas beyond where the star's photons run out; in thin gas
			// the light is little either way)
			vec2 rf2 = NebFront2 (base, v / max (r, 1e-6));
			float rf = mix (rf2.x, rf2.y, smoothstep (2.0, 20.0, n));
			float w = 1.0 - smoothstep (0.9 * rf, rf, r);
			if (w <= 0.0) continue;
			w *= 1.0 - smoothstep (0.6, 1.0, r / gal.neb[0].z); // (no edge where the region ends at the largest radius)
			vec4 line = src.line + NebExcitation (src.ion.x, r, n) * src.excite;
			L += line * (n * n * w) * exp (-A * ext) * ds;
		}
	}

	// dust lit by the star: samples spaced evenly in the angle seen from
	// the star (light falls off as 1 / r^2)
	{
		const int N = 16;
		float a0 = atan ((s0 - tc) / b), a1 = atan ((s1 - tc) / b);
		float da = (a1 - a0) / float (N);
		float A = A0, sPrev = s0, rhoPrev = -1.0;
		// (the local shell widened to at least half a texel, keeping its
		// column)
		float rl = max (max (src.ion.z, 0.5 * abs (tc) * pix), 1e-3), Al = src.ion.y;
		for (int i = 0; i < N; i++) {
			float a = a0 + (float (i) + jit) * da;
			float s = tc + b * tan (a);
			vec3 x = o + d * s;
			vec3 v = x - c;
			float r2 = max (dot (v, v), 1e-4), r = sqrt (r2);
			float xl = r / rl;
			float rhoL = Al * xl / (rl * (1.0 + xl) * (1.0 + xl) * (1.0 + xl));
			float rho = GalDust (x, cell) + rhoL;
			A += (rhoPrev < 0.0 ? rho : 0.5 * (rho + rhoPrev)) * (s - sPrev);
			rhoPrev = rho, sPrev = s;
			float ds = da * (b * b + (s - tc) * (s - tc)) / b;
			float gas, dust;
			NebClump (x, max (s * pix, (s1 - s0) / float (N)), gas, dust);
			float tauStar = (rho - rhoL) * r + 0.5 * Al * xl * xl / ((1.0 + xl) * (1.0 + xl)); // A_V from the star (the map's dust as here all the way)
			float ph = NebPhase (dot (v, -d) / r, g);
			L += albedo * (rho * dust / 1.086) * gal.extinction * src.light * ph / r2
				* exp (-(A + tauStar) * ext) * ds;
		}
	}
	return L;
}
#endif
