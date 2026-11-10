// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Planet surface tiles (fragment shader body: tile.frag, and tile_fp64.frag
// with the procedural terrain detail)

#define FRAME_AO
#include "frame.glsl"

#include "shadow.glsl"
#include "lights.glsl"

layout(set = 1, binding = 0) uniform sampler2D surfTex;
layout(set = 2, binding = 0) uniform sampler2D maskTex;

layout(push_constant) uniform Tile {
	mat4 modelView;
	vec4 surfUV;
	vec4 maskUV;
	vec4 params;
	vec4 detail;
	vec4 morph;
	vec4 vessel;
	ivec4 refI;    // terrain detail: the tile's reference point d0 R [m], integer part (xyz)
	vec4 refF;     // ... fraction (xyz); w: the planet's radius R [m]
	vec4 refRel;   // d0 (R + h0) relative to the camera, planet frame (xyz); w: R + h0 [m]
	vec4 tracks;   // ground tracks on the tile: first entry, count
} tile;

// ground tracks (GroundTracks.h), view space
struct TrackSeg {
	vec4 a;  // start; w: half width [m]
	vec4 b;  // end; w: depth [m]
	vec4 up; // surface normal; w: 0 track, 1 imprint
};
layout(std430, set = 0, binding = 8) readonly buffer Tracks { TrackSeg seg[]; } tracks;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vDetailA;
layout(location = 4) in float vMorph;

layout(location = 0) out vec4 outColor;

#define ATM_SET 3
#include "atmosphere.glsl"
#include "brdf.glsl"
#ifdef TERRAIN_DETAIL
#include "terraindetail.glsl"
#endif

// Periodic value noise (period in cells), for patterns anchored to the
// 1 km-snapped origin (cell sizes divide 1 km)
float Hash (ivec3 c)
{
	uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return float(h & 0xffffu) / 65535.0;
}

float ValueNoise (vec3 p, float cell, int period)
{
	vec3 q = p / cell;
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	f = f * f * (3.0 - 2.0 * f);
	float n[8];
	for (int k = 0; k < 8; k++) {
		// % is undefined for negative operands in GLSL: wrap explicitly
		ivec3 c = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
		c = c - period * ivec3(floor(vec3(c) / float(period)));
		n[k] = Hash(c);
	}
	return mix(mix(mix(n[0], n[1], f.x), mix(n[2], n[3], f.x), f.y),
	           mix(mix(n[4], n[5], f.x), mix(n[6], n[7], f.x), f.y), f.z);
}

// ValueNoise (x) and its gradient (yzw, per metre)
vec4 ValueNoiseD(vec3 p, float cell, int period)
{
	vec3 q = p / cell;
	ivec3 i = ivec3(floor(q));
	vec3 f = fract(q);
	vec3 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
	float n[8];
	for (int k = 0; k < 8; k++) {
		ivec3 c = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
		c = c - period * ivec3(floor(vec3(c) / float(period)));
		n[k] = Hash(c);
	}
	float k1 = n[1] - n[0], k2 = n[2] - n[0], k3 = n[4] - n[0];
	float k4 = n[0] - n[1] - n[2] + n[3], k5 = n[0] - n[2] - n[4] + n[6], k6 = n[0] - n[1] - n[4] + n[5];
	float k7 = -n[0] + n[1] + n[2] - n[3] + n[4] - n[5] - n[6] + n[7];
	float v = n[0] + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x + k7 * u.x * u.y * u.z;
	vec3 g = du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
	                   k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
	                   k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
	return vec4(v, g / cell);
}

#include "surfdetail.glsl"

// Disturbances of the water (WaterFx.h; atm.waterFx, atm.waterFxSrc): where
// jets blow on it, gear stands in it, something splashed. Each source:
//  - its footprint (R0): the wind's waves pressed flat, a shallow cavity
//  - the wall jet running out from it (to Rw): chop (finer than the pixels:
//    roughness)
//  - rings of waves running out (three wavelengths, deep-water dispersion:
//    the longer ones ahead), spreading and dying out
//  - foam: a ragged patch, lacy within
const float WFX_LRATIO = 0.6;                 // each ring wave's wavelength / the previous one's (WaterFx.cpp)
const vec3 WFX_RING_W = vec3(1.0, 0.7, 0.45); // slope weights of the ring waves
const vec3 FOAM_ALBEDO = vec3(0.60, 0.62, 0.63);

struct WaterFxOut { vec3 g; float flatw; float var; float foam; float chop; };

// p: pixel relative to the camera, Np: up (planet frame); footprint: pixel size on the water [m]
WaterFxOut WaterFxEval (vec3 p, vec3 Np, float footprint)
{
	WaterFxOut o;
	o.g = vec3(0.0); o.flatw = 0.0; o.var = 0.0; o.foam = 0.0; o.chop = 0.0;
	int n = min(int(atm.waterFx.x + 0.5), WATERFX_MAX);
	for (int s = 0; s < n; s++) {
		int b = s * WATERFX_VEC4;
		vec4 v0 = atm.waterFxSrc[b], v1 = atm.waterFxSrc[b + 1], v4 = atm.waterFxSrc[b + 4];
		float R0 = v0.w, Rw = v1.w, k0 = v4.x;
		vec3 rel = p - v0.xyz;
		rel -= Np * dot(rel, Np);
		float r = length(rel);
		float lambda0 = 2.0 * PI / k0;
		if (r > max(1.15 * Rw, v4.y + lambda0)) continue; // (beyond its reach)
		vec4 v2 = atm.waterFxSrc[b + 2], v3 = atm.waterFxSrc[b + 3], v5 = atm.waterFxSrc[b + 5];
		float J = v3.x, depth = v3.y, foamLvl = v3.z, ringA = v3.w;
		vec3 rh = r > 1e-3 ? rel / r : vec3(0.0);
		// the footprint: the wind's waves pressed flat; a cavity h = -depth exp(-(r/R0)^2)
		o.flatw = max(o.flatw, J * (1.0 - smoothstep(0.8 * R0, 1.6 * R0, r)));
		float q = r / R0;
		o.g += rh * (depth * 2.0 * q / R0 * exp(-q * q));
		// rings running out: phase k (r - R0) - omega t (v5: omega t, wrapped)
		if (ringA > 0.0) {
			float Rr = lambda0 / 1.6;        // ring source radius (RING_L0 in WaterFx.cpp)
			float Lr = max(v5.w, 1e-3);      // damping length (CPU)
			float amp = ringA * sqrt(Rr / max(r, Rr)) * exp(-max(r - Rr, 0.0) / Lr);
			float k = k0, cgr = 1.0, ring = 0.0;
			for (int j = 0; j < 3; j++) {
				float lam = 2.0 * PI / k;
				float front = R0 + (v4.y - R0) * cgr; // (group speed ~ sqrt(lambda))
				float env = 1.0 - smoothstep(front - 0.5 * lam, front, r);
				float inner = R0 + v4.z * cgr;
				env *= v4.z > 0.0 ? smoothstep(inner - 0.5 * lam, inner, r) : smoothstep(0.6 * R0, R0, r);
				float aa = 1.0 - smoothstep(2.0, 6.0, footprint * k * (2.0 / PI));
				ring += WFX_RING_W[j] * env * aa * cos(k * (r - R0) - v5[j]);
				k /= WFX_LRATIO;
				cgr *= sqrt(WFX_LRATIO);
			}
			o.g += rh * (amp * ring);
		}
		// chop and foam: noise in the source's own frame (anchored where it was born)
		float Rf = R0 + 0.35 * (Rw - R0);
		float cMax = J * (1.0 - smoothstep(0.35 * Rw, 1.05 * Rw, r - 0.075 * Rw));
		bool doFoam = foamLvl > 0.003 && r < 1.2 * Rf;
		if (cMax > 0.01 || doFoam) {
			vec3 t1 = normalize(v2.xyz - Np * dot(v2.xyz, Np)), t2 = cross(Np, t1);
			vec3 a = p - v1.xyz;
			vec2 uv = vec2(dot(a, t1), dot(a, t2));
			float fch = v2.w; // foam churn, wrapped at 2048
			if (cMax > 0.01) {
				// ragged edge and patchy strength (cat's paws)
				float nR = ValueNoise(vec3(uv + 7.3, fch), 4.0, 512);
				float rr = r + 0.15 * Rw * (nR - 0.5);
				float c = J * (1.0 - smoothstep(0.35 * Rw, 1.05 * Rw, rr));
				c *= mix(0.6, 1.0, smoothstep(0.25, 0.75, ValueNoise(vec3(uv + 3.1, fch), 4.0, 512)));
				o.chop = max(o.chop, c);
				// (octaves finer than the pixels: their slopes go into the roughness)
				float aa1 = 1.0 - smoothstep(0.25, 0.5, footprint / 0.5);
				float aa2 = 1.0 - smoothstep(0.25, 0.5, footprint / 0.2);
				vec2 gr = vec2(0.0);
				if (aa1 > 0.0) gr += 0.06 * aa1 * ValueNoiseD(vec3(uv, v4.w), 0.5, 1024).yz;
				if (aa2 > 0.0) gr += 0.02 * aa2 * ValueNoiseD(vec3(uv, v4.w), 0.2, 1024).yz;
				o.g += c * (t1 * gr.x + t2 * gr.y);
				o.var += c * c * (0.0144 * (1.0 - aa1) + 0.01 * (1.0 - aa2)) + 0.004 * c;
			}
			if (doFoam) {
				float nC = ValueNoise(vec3(uv, fch), 2.0, 1024);
				float edge = 1.0 - smoothstep(0.3 * Rf, Rf, r + 0.35 * Rf * (nC - 0.5));
				float core = 1.0 - smoothstep(0.15 * Rf, 0.55 * Rf, r);
				float aaL = 1.0 - smoothstep(0.25, 0.5, footprint / 0.5);
				float aaM = 1.0 - smoothstep(0.25, 0.5, footprint / 0.25);
				float l1 = 0.22, l2 = 0.22; // (mean filament coverage when finer than the pixels)
				if (aaL > 0.0) {
					// domain warp (+-0.5 m), then filaments (ridges of value noise, isotropic)
					vec2 w = vec2(ValueNoise(vec3(uv, fch), 2.0, 1024), ValueNoise(vec3(uv + 11.7, fch), 2.0, 1024)) - 0.5;
					vec2 wuv = uv + w;
					vec2 q1 = wuv;
					float n1 = ValueNoise(vec3(q1, fch), 0.5, 1024);
					l1 = mix(0.22, smoothstep(0.62, 0.92, 1.0 - abs(2.0 * n1 - 1.0)), aaL);
					if (aaM > 0.0) {
						vec2 q2 = mat2(0.8, -0.6, 0.6, 0.8) * wuv + vec2(5.3, 9.1);
						float n2 = ValueNoise(vec3(q2, fch), 0.25, 1024);
						l2 = mix(0.22, smoothstep(0.62, 0.92, 1.0 - abs(2.0 * n2 - 1.0)), aaM);
					}
				}
				float lace = 1.0 - (1.0 - l1) * (1.0 - 0.7 * l2);
				lace *= 1.0 - 0.4 * smoothstep(0.5 * Rf, Rf, r);
				float f = clamp(foamLvl * edge * max(core, 1.2 * lace), 0.0, 1.0);
				o.foam = 1.0 - (1.0 - o.foam) * (1.0 - f);
			}
		}
	}
	return o;
}

// Wave normal of water surfaces (view space): a sum of WAVE_COUNT waves
// whose wave vectors and phases the CPU sets (Atmosphere.cpp: anchored to
// the planet, the body's own gravity in their dispersion), longest first.
// Their spectrum follows the wind (SeaState: the longest wavelength, the
// slope per wave, the direction downwind). waveVar out: the mean-square
// slope of the sea (seaPrm.y) that the waves drawn here do not resolve
// (finer than the pixels, beyond the fade, below the shortest wave): the
// glitter's roughness takes it
vec3 WaterNormalWaves (vec3 Nv, vec3 posView, float dist, out float waveVar)
{
	waveVar = atm.seaPrm.y;
	float slope = atm.wavePrm.x;
	if (slope <= 0.0) return Nv;
	float fade = 1.0 - smoothstep (300.0, 3000.0, dist);
	if (fade <= 0.0) return Nv;
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 p = v2p * posView; // relative to the camera (planet frame)
	vec3 Np = v2p * Nv;
	// pixel size on the water [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (normalize (posView), Nv)), 0.05);
	vec3 g = vec3 (0.0);
	float res = 0.0;
	for (int i = 0; i < WAVE_COUNT; i++) {
		vec4 w = atm.waves[i];
		float k = length (w.xyz);
		// (waves shorter than 8 px fade out and are gone at 4 px (x = 4 footprint / lambda): finer pure-sinusoid
		// waves read as a regular lattice, not ripples. Their slope variance goes into the glitter roughness via waveVar.)
		float aa = 1.0 - smoothstep (0.5, 1.0, footprint * k * (2.0 / PI)); // x = 4 footprint / lambda
		if (aa <= 0.0) break;
		g += w.xyz * (slope * cos (dot (p, w.xyz) + w.w) * aa / k);
		res += aa * aa;
	}
	waveVar = max (atm.seaPrm.y - 0.5 * slope * slope * res * fade * fade, 0.0);
	g -= Np * dot (g, Np); // slope in the tangent plane
	return transpose (v2p) * normalize (Np - g * fade);
}

// With disturbances (WaterFx): the wind's waves pressed flat under them, plus
// their cavity, rings and chop; fxVar (slope variance, into roughness) and
// fxFoam (coverage) out; fl: how much of the wind's sea is left (1: all)
vec3 WaterNormal (vec3 Nv, vec3 posView, float dist, out float fxVar, out float fxFoam, out float waveVar, out float fl)
{
	fxVar = 0.0; fxFoam = 0.0; fl = 1.0;
	if (atm.waterFx.x < 0.5 || dist > 6000.0) return WaterNormalWaves (Nv, posView, dist, waveVar);
	waveVar = atm.seaPrm.y;
	float slope = atm.wavePrm.x;
	float fade = 1.0 - smoothstep (300.0, 3000.0, dist);
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 p = v2p * posView;
	vec3 Np = v2p * Nv;
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (normalize (posView), Nv)), 0.05);
	WaterFxOut fx = WaterFxEval (p, Np, footprint);
	vec3 g = vec3 (0.0);
	float res = 0.0;
	fl = (1.0 - 0.85 * fx.flatw) * (1.0 - 0.5 * fx.chop); // the jet presses the wind's sea flat
	if (slope > 0.0 && fade > 0.0) {
		for (int i = 0; i < WAVE_COUNT; i++) {
			vec4 w = atm.waves[i];
			float k = length (w.xyz);
			// (as in WaterNormalWaves)
			float aa = 1.0 - smoothstep (0.5, 1.0, footprint * k * (2.0 / PI));
			if (aa <= 0.0) break;
			g += w.xyz * (slope * cos (dot (p, w.xyz) + w.w) * aa / k);
			res += aa * aa;
		}
		g *= fade * fl;
	}
	waveVar = max (fl * fl * (atm.seaPrm.y - 0.5 * slope * slope * res * fade * fade), 0.0);
	g += fx.g;
	g -= Np * dot (g, Np);
	fxVar = fx.var; fxFoam = fx.foam;
	return transpose (v2p) * normalize (Np - g);
}

// Whitecaps and the shoreline foam band (G3).
// Whitecaps: where the crests of the longest CAP_WAVES waves pile up, the crest
// index S = slope sum sin(phi_i) (their downward acceleration over g, linear
// wave theory) exceeds a threshold that the CPU solves for the coverage the
// wind gives (Atmosphere::FoamState, Stramska & Petelski 2003). The active
// crest breaks on the forward face of wave 0. What it leaves behind (the
// residual foam) stays where it was made and fades: the deposit at x is the
// crest index at x when the last two crests of wave 0 passed it
// (phi_i(t - tau) = phi_i + omega_i tau), so at a fixed point it is constant
// between passages and decays exponentially, as foam does on the water.
// Noise is world-anchored (no swimming). Beyond the waves' fade the sea takes
// the CPU's mean foam. The constants are as in Atmosphere.cpp (keep in sync).
#define CAP_WAVES 6
#define CAP_EDGE 3.0
#define CAP_WIDTH 0.35
#define CAP_SPREAD 0.3
#define CAP_TF 0.4
#define CAP_LIFE 2.0
#define SIGMA_RHO 7.28e-5

// ragged edges of the foam patches: octaves 8 / 2 / 0.5 m, each fading to its
// mean by its own footprint/cell. wsum out: the analytic per-pixel change of
// the noise [noise units] (it widens the threshold's edge: anti-aliasing)
float CapEdgeNoise (vec3 pw, float fp, out float wsum)
{
	float a8 = 1.0 - smoothstep (0.25, 0.5, fp / 8.0), a2 = 1.0 - smoothstep (0.25, 0.5, fp / 2.0), a05 = 1.0 - smoothstep (0.25, 0.5, fp / 0.5);
	float n = -0.5;
	wsum = 0.45 * a8 * min (fp / 8.0, 1.0) + 0.35 * a2 * min (fp / 2.0, 1.0) + 0.2 * a05 * min (fp / 0.5, 1.0);
	n += 0.45 * (a8 > 0.0 ? mix (0.5, ValueNoise (pw + vec3 (9.7, 1.9, 4.1), 8.0, 125), a8) : 0.5);
	n += 0.35 * (a2 > 0.0 ? mix (0.5, ValueNoise (pw, 2.0, 500), a2) : 0.5);
	n += 0.2 * (a05 > 0.0 ? mix (0.5, ValueNoise (pw + vec3 (17.1, 3.7, 9.3), 0.5, 2000), a05) : 0.5);
	return n;
}

// filaments of the foam; mean: its average at full resolution. Beyond the
// filaments' resolution a 4 m mottle (mean 1) keeps the patch from being flat
float FoamLace (vec3 pw, float fp, float mean)
{
	float aL = 1.0 - smoothstep (0.25, 0.5, fp / 1.0);
	float aT = 1.0 - smoothstep (0.25, 0.5, fp / 4.0);
	float mot = mean;
	if (aT > 0.0) mot = min (mean * mix (1.0, 0.55 + 0.9 * ValueNoise (pw + vec3 (21.3, 8.1, 3.7), 4.0, 250), aT), 1.0);
	if (aL <= 0.0) return mot;
	vec3 q = pw + (vec3 (ValueNoise (pw, 2.0, 500), ValueNoise (pw + vec3 (11.7, 5.3, 2.9), 2.0, 500),
	                     ValueNoise (pw + vec3 (3.1, 7.9, 13.3), 2.0, 500)) - 0.5);
	float l1 = smoothstep (0.55, 0.9, 1.0 - abs (2.0 * ValueNoise (q, 1.0, 1000) - 1.0));
	float aM = 1.0 - smoothstep (0.25, 0.5, fp / 0.4);
	float l2 = 0.3;
	if (aM > 0.0)
		l2 = mix (0.3, smoothstep (0.55, 0.9, 1.0 - abs (2.0 * ValueNoise (q + vec3 (5.3, 9.1, 2.2), 0.4, 2500) - 1.0)), aM);
	float fil = 1.0 - (1.0 - l1) * (1.0 - 0.7 * l2);
	return mix (mot, 0.25 + 0.75 * fil, aL);
}

// foam coverage of the sea (0..1); capVar out: the breaking crests' extra
// roughness (slope variance). p: planet frame, relative to the camera; pw: world-anchored
float Whitecaps (vec3 p, vec3 pw, float footprint, float fade, out float capVar)
{
	capVar = 0.0;
	if (atm.seaPrm.w <= 0.0) return 0.0;
	float slope = atm.wavePrm.x, th = atm.foamPrm.x, sig = atm.foamPrm.y, fMean = atm.foamPrm.z;
	if (slope <= 0.0 || fade <= 0.0) return fMean;
	float g = atm.wavePrm.y;
	float phi[CAP_WAVES], om[CAP_WAVES], aa[CAP_WAVES];
	float S = 0.0, sa2 = 0.0;
	for (int i = 0; i < CAP_WAVES; i++) {
		vec4 w = atm.waves[i];
		float k = length (w.xyz);
		aa[i] = 1.0 - smoothstep (0.5, 1.0, footprint * k * (2.0 / PI));
		phi[i] = dot (p, w.xyz) + w.w;
		om[i] = sqrt (g * k + SIGMA_RHO * k * k * k);
		S += aa[i] * sin (phi[i]);
		sa2 += aa[i] * aa[i];
	}
	float vis = fade * aa[2];
	if (vis <= 0.0) return fMean;
	// (the variance holds as the short waves drop out)
	float rn = slope * sqrt (float (CAP_WAVES) / max (sa2, 1.0));
	S *= rn;
	float T0 = 2.0 * PI / om[0];
	float dphi = mod (phi[0] - 0.5 * PI + PI, 2.0 * PI) - PI;
	float win = smoothstep (-0.15, 0.0, dphi) * (1.0 - smoothstep (0.45, 0.9, dphi));
	// the residual foam of the last two crests
	float tc = mod (0.5 * PI - phi[0], 2.0 * PI) / om[0];
	float E = exp (-CAP_LIFE / CAP_TF);
	float tauN[2], oN[2], SdN[2];
	float bound = win > 0.0 ? S : -1e9;
	for (int n = 0; n < 2; n++) {
		float tau = tc + float (n) * T0;
		tauN[n] = tau;
		oN[n] = max ((exp (-tau / (CAP_TF * T0)) - E) / (1.0 - E), 0.0);
		SdN[n] = 0.0;
		if (oN[n] <= 0.0) continue;
		float Sd = 0.0;
		for (int i = 0; i < CAP_WAVES; i++) Sd += aa[i] * sin (phi[i] + om[i] * tau);
		SdN[n] = Sd * rn;
		bound = max (bound, SdN[n] + CAP_SPREAD * sig * tau / T0);
	}
	// nothing reaches the threshold even with the edge noise at its largest
	if (bound + 0.5 * CAP_EDGE * sig < th) return mix (fMean, 0.0, vis);
	float wsum;
	float e = CapEdgeNoise (pw, footprint, wsum) * CAP_EDGE * sig;
	float wE = CAP_WIDTH * sig + CAP_EDGE * sig * wsum;
	// the active crest: on the forward face of wave 0
	float act = 0.0;
	if (win > 0.0) {
		float LA = mix (0.875, mix (0.75, 1.0, ValueNoise (pw + vec3 (5.9, 1.3, 7.7), 1.0, 1000)), 1.0 - smoothstep (0.25, 0.5, footprint));
		float A = mix (0.85, mix (0.7, 1.0, ValueNoise (pw + vec3 (13.3, 6.1, 2.7), 2.5, 400)), 1.0 - smoothstep (0.25, 0.5, footprint / 2.5));
		act = smoothstep (th, th + wE, S + e) * win * LA * A;
	}
	float keep = 1.0 - act;
	float lace = -1.0;
	for (int n = 0; n < 2; n++) {
		if (oN[n] <= 0.0) continue;
		float thn = th - CAP_SPREAD * sig * tauN[n] / T0;
		float r = smoothstep (thn, thn + wE, SdN[n] + e) * oN[n];
		if (r <= 0.0) continue;
		if (lace < 0.0) lace = FoamLace (pw, footprint, atm.foamPrm.w);
		keep *= 1.0 - r * lace;
	}
	capVar = 0.02 * act * vis;
	return mix (fMean, 1.0 - keep, vis);
}

// distance [m] from the water's drawn edge (the mask's 0.5 contour), from the
// 2 x 2 texels under the pixel (-1: unknown, no land among them). texM: the
// mask's texel [m]
float ShoreDistance (vec2 muv, float texM)
{
	if (texM <= 0.0) return -1.0;
	vec2 sz = vec2 (textureSize (maskTex, 0));
	vec4 a4 = textureGather (maskTex, muv, 3); // x: (i0,j1), y: (i1,j1), z: (i1,j0), w: (i0,j0)
	if (max (max (a4.x, a4.y), max (a4.z, a4.w)) < 0.02) return -1.0;
	vec2 f = fract (muv * sz - 0.5);
	float a = mix (mix (a4.w, a4.z, f.x), mix (a4.x, a4.y, f.x), f.y);
	vec2 gr = vec2 (mix (a4.z - a4.w, a4.y - a4.x, f.y), mix (a4.x - a4.w, a4.y - a4.z, f.x));
	return max (0.5 - a, 0.0) / max (length (gr), 0.3) * texM;
}

// surf: a swash line at the water's edge and bores running in, in a band
// that widens with the sea (shorePrm, from the developed sea's Hs and period)
float ShoreFoam (vec3 pw, float d, float texM, float footprint)
{
	float W = min (atm.shorePrm.x, 1.5 * texM);
	if (texM <= 0.0 || d < 0.0 || W <= 0.0 || d >= W) return 0.0;
	float vis = 1.0 - smoothstep (1.0, 2.5, footprint / W);
	if (vis <= 0.0) return 0.0;
	float x = d / W;
	float ph = atm.shorePrm.z, ls = atm.shorePrm.y;
	float n1 = ValueNoise (pw, 20.0, 50), n2 = ValueNoise (pw + vec3 (7.1, 2.3, 4.9), 4.0, 250);
	float env = 1.0 - smoothstep (0.55, 1.0, x + 0.25 * (n2 - 0.5));
	float u = fract (d / ls + ph + 0.35 * (n1 - 0.5));
	float eps = clamp (footprint / ls, 0.02, 0.5);
	float bore = max (exp (-2.5 * u), smoothstep (1.0 - eps, 1.0, u));
	float reach = 0.18 * W * (0.6 + 0.4 * sin (2.0 * PI * (ph + n1)));
	float swash = 1.0 - smoothstep (0.5 * reach, reach, d);
	float f = max (0.9 * swash, 1.0 * bore * env) * mix (FoamLace (pw, footprint, atm.foamPrm.w), 1.0, 0.5);
	return clamp (f * atm.shorePrm.w * vis, 0.0, 1.0);
}

// Tracks of wheels and imprints of gear: a groove with raised edges (the
// nearest segment), the tyres' tread across it, the disturbed soil a
// little darker
void TrackShading(vec3 P, inout vec3 N, inout vec3 base)
{
	int first = int(tile.tracks.x), count = int(tile.tracks.y);
	if (count <= 0) return;
	float best = 1.8;
	vec3 off = vec3(0.0), dir = vec3(0.0);
	float along = 0.0;
	TrackSeg s;
	// scorch marks overlap: blended (the darkest burn, the craters' slopes
	// added), not the nearest alone (seams where one meets the next)
	float burnMax = 0.0;
	vec3 scorchGrad = vec3(0.0);
	for (int i = 0; i < count; i++) {
		TrackSeg c = tracks.seg[first + i];
		vec3 up = c.up.xyz;
		vec3 ab = c.b.xyz - c.a.xyz, ap = P - c.a.xyz;
		ab -= up * dot(ab, up), ap -= up * dot(ap, up);
		float L2 = dot(ab, ab);
		float t = L2 > 1e-8 ? clamp(dot(ap, ab) / L2, 0.0, 1.0) : 0.0;
		vec3 o = ap - ab * t;
		float x = length(o) / c.a.w;
		if (c.up.w > 1.5) {
			if (x >= 1.8) continue;
			// scorch mark: blackened ground, most at its centre, its rim
			// irregular (a few lobes, by the direction; phases from its size)
			// and soft, a shallow crater with a low rim
			float hw = c.a.w, depth = c.b.w;
			vec3 lat = x > 1e-4 ? o / (x * hw) : vec3(0.0);
			vec3 ref = normalize(abs(up.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0));
			float ang = atan(dot(lat, cross(up, ref)), dot(lat, ref));
			float rag = 0.12 * sin(ang * 2.0 + 1.7 * hw) + 0.08 * sin(ang * 3.0 + 2.9 * hw) + 0.05 * sin(ang * 5.0 + 4.3 * hw)
				+ 0.025 * sin(ang * 9.0 + 6.1 * hw);
			burnMax = max(burnMax, 1.0 - smoothstep(0.2 + rag, 1.0 + rag, x));
			float qc = max(1.0 - x * x, 0.0);
			float ec = exp(-pow((x - 1.05) / 0.25, 2.0));
			float dh = 4.0 * x * qc * 0.5 - 0.3 * ec * 2.0 * (x - 1.05) / 0.0625;
			vec3 g = lat * (depth * dh / hw);
			scorchGrad += g - up * dot(g, up);
			continue;
		}
		if (x < best) {
			best = x, off = o, s = c;
			dir = L2 > 1e-8 ? ab / sqrt(L2) : vec3(0.0);
			along = L2 > 1e-8 ? dot(ap, dir) : 0.0;
		}
	}
	if (burnMax > 0.0 || dot(scorchGrad, scorchGrad) > 0.0) {
		base *= mix(1.0, 0.1, burnMax * (0.7 + 0.3 * burnMax));
		vec3 g = scorchGrad - N * dot(scorchGrad, N);
		N = normalize(N - g);
	}
	if (best >= 1.8) return;
	float x = best, hw = s.a.w, depth = s.b.w;
	vec3 lat = x > 1e-4 ? off / (x * hw) : vec3(0.0);
	// profile [depths]: groove -(1 - x^2)^2, edges pushed up beside it
	float q = max(1.0 - x * x, 0.0);
	float e = exp(-pow((x - 1.2) / 0.3, 2.0));
	float dhdx = 4.0 * x * q - 0.35 * e * 2.0 * (x - 1.2) / 0.09;
	vec3 grad = lat * (depth * dhdx / hw);
	// tread across the track (faded out when finer than the pixels)
	float px = length(P) * 2.0 * atm.misc.z / max(frame.jitter.w, 1.0);
	if (s.up.w < 0.5 && x < 1.0) {
		const float PERIOD = 0.08;
		float w = (1.0 - smoothstep(0.004, 0.012, px)) * smoothstep(1.0, 0.7, x);
		grad += dir * (0.25 * depth * w * 6.2832 / PERIOD * cos(along * 6.2832 / PERIOD));
	}
	vec3 up = s.up.xyz;
	grad -= up * dot(grad, up);
	grad -= N * dot(grad, N);
	N = normalize(N - grad);
	base *= 1.0 - 0.15 * q; // (disturbed soil, darker)
}

// Streets of the cities around the camera (CityField): the road map over
// the tangent plane at its centre, a fine level within a coarse one
// irradiance under a street lamp (the sun's: 1)
const float CITY_LAMP_IRR = 0.3;

// The cities' streets on the ground (CityField's road map: r asphalt, g
// pavement, b light of the street lamps, a its share of white LED light);
// returns the lamps' light (irradiance, for the night); cover: where the
// map lies (1; fading out at its edges), where the cities' own lights
// replace the night light texture
vec3 cityEmis = vec3(0.0); // (CityGround: the roof carpet's lit windows)
float cityCarpet = 0.0;     // (CityGround: the roof carpet's share)
// The hand-off to the night light texture by the camera's altitude above
// the ground (frame.city[3].w, CityField): the procedural city (roof carpet,
// its lit windows, the streets, their lamps' light) fades out over 3 to 6
// km (CityField FADE_LO, FADE_HI; building.vert has the same numbers), the
// texture takes over (CityHandOff, 0 below 3 km, 1 from 6 km up)
float CityHandOff()
{
	return smoothstep(3000.0, 6000.0, frame.city[3].w);
}
vec3 CityGround (vec3 posView, inout vec3 base, out float cover)
{
	cover = 0.0;
	if (frame.city[0].w < 0.5) return vec3(0.0);
	float cityP = 1.0 - CityHandOff(); // (the procedural city's share)
	if (cityP <= 0.0) return vec3(0.0);
	vec3 d = posView - frame.city[0].xyz;
	vec2 p = vec2(dot(d, frame.city[1].xyz), dot(d, frame.city[2].xyz));
	// Beyond the small buildings (building.vert's fade, 1.3 to 2.3 km): a
	// carpet of roofs where the ground is built up (the far map: the streets'
	// share about), in the blocks between the streets (the road map, where
	// it reaches), so that the city drawn in three dimensions continues
	// flat; lit windows at night (cityEmis)
	vec2 ud = p * frame.city[3].z + 0.5;
	float ed = max(abs(ud.x - 0.5), abs(ud.y - 0.5));
	float wc = ed < 0.5 ? smoothstep(1800.0, 2600.0, length(posView)) * smoothstep(0.5, 0.45, ed) * cityP : 0.0;
	float built = wc > 0.0 ? smoothstep(0.10, 0.30, texture(cityMap[2], ud).r) : 0.0;
	cityCarpet = wc * built;
	vec2 uc = p * frame.city[3].y + 0.5;
	float ec = max(abs(uc.x - 0.5), abs(uc.y - 0.5));
	vec4 m = vec4(0.0);
	float fade = 0.0;
	if (ec < 0.5) {
		m = texture(cityMap[1], uc);
		vec2 uf = p * frame.city[3].x + 0.5;
		float ef = max(abs(uf.x - 0.5), abs(uf.y - 0.5));
		if (ef < 0.5) m = mix(texture(cityMap[0], uf), m, smoothstep(0.4, 0.5, ef));
		fade = smoothstep(0.5, 0.4, ec) * cityP;
		cover = fade;
	}
	float w = wc * built * (1.0 - max(m.r, m.g) * fade);
	if (w > 0.0) {
		vec2 q = p / 14.0;
		ivec2 c = ivec2(floor(q));
		vec2 f = fract(q);
		float h = Hash(ivec3(c, 7));
		vec3 roof = h < 0.35 ? vec3(0.40, 0.39, 0.38) : h < 0.55 ? vec3(0.27, 0.27, 0.29)
			: h < 0.72 ? vec3(0.50, 0.30, 0.22) : h < 0.88 ? vec3(0.36, 0.40, 0.44) : vec3(0.17, 0.25, 0.14);
		roof *= 0.85 + 0.3 * Hash(ivec3(c, 8));
		roof = mix(roof, vec3(0.37, 0.35, 0.33), 0.65 * smoothstep(1800.0, 6000.0, length(posView))); // (far: a town's mean, not a checkerboard)
		float gap = 1.0 - smoothstep(0.0, 0.12, min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y))); // (the buildings' sides, in shade)
		roof *= 1.0 - 0.45 * gap * (1.0 - smoothstep(1000.0, 4000.0, length(posView))); // (far: they average out)
		base = mix(base, roof, 0.85 * w);
		float lit = Hash(ivec3(c, 9)) < 0.35 ? 1.0 : 0.0;
		cityEmis = vec3(1.0, 0.78, 0.5) * (0.04 * lit * w);
	}
	if (fade <= 0.0) return vec3(0.0);
	// (from afar the streets of the coarse road map, metres wider than they
	// are, drew a black grid over the town: their contrast fades with
	// distance, as the narrow streets and their trees average out)
	float far = smoothstep(600.0, 3000.0, length(posView));
	base = mix(base, vec3(0.20, 0.19, 0.18), m.g * fade * (1.0 - 0.5 * far)); // pavement
	base = mix(base, mix(vec3(0.075, 0.075, 0.08), vec3(0.17, 0.165, 0.16), far), m.r * fade * (1.0 - 0.7 * far)); // asphalt
	// (sodium lamps orange, LEDs white)
	vec3 lamp = mix(vec3(1.0, 0.55, 0.18), vec3(0.85, 0.9, 1.0), m.a);
	return lamp * (CITY_LAMP_IRR * m.b * fade);
}

// Texture gradients towards the poles: the textures' texels (equal steps
// of longitude and latitude) narrow to slivers, cos(lat) as wide as they are
// long; the columns' own noise showed as streaks along the meridians. The
// footprint is widened east-west to at least one texel's length (in metres)
// north-south, which the anisotropic filter averages (at once, the mip levels
// further on). cos(lat): the ratio of the surface's change along u (longitude)
// to v (latitude), from the screen derivatives (a tile spans equal angles).
vec2 polarGx, polarGy;
bool polarDone = false;
void PolarGradients(vec2 uvT, vec2 hs)
{
	vec2 gx = dFdx(uvT), gy = dFdy(uvT);
	polarGx = gx, polarGy = gy, polarDone = true;
	vec3 px = dFdx(vPosView), py = dFdy(vPosView);
	vec2 ux = dFdx(vUV), uy = dFdy(vUV);
	float det = ux.x * uy.y - uy.x * ux.y;
	if (abs(det) < 1e-20) return;
	vec3 dPdu = (px * uy.y - py * ux.y) / det, dPdv = (py * ux.x - px * uy.x) / det;
	float lv = length(dPdv);
	if (lv <= 0.0) return;
	float c = clamp(length(dPdu) / lv, 1e-3, 1.0);
	float w = smoothstep(0.5, 0.25, c); // (from 60 degrees on: none at lower latitudes)
	if (w <= 0.0) return;
	float su = max(abs(gx.x), abs(gy.x)), sv = max(max(abs(gx.y), abs(gy.y)), 2.0 * hs.y);
	polarGx = mix(gx, vec2(max(su, sv / c), 0.0), w);
	polarGy = mix(gy, vec2(0.0, sv), w);
}
vec2 PolarGradX(vec2 uvT, vec2 hs) { if (!polarDone) PolarGradients(uvT, hs); return polarGx; }
vec2 PolarGradY(vec2 uvT, vec2 hs) { if (!polarDone) PolarGradients(uvT, hs); return polarGy; }

// A cloud layer's texture magnified (Earth's finest clouds: 1.2 km a
// texel, compressed in blocks of 4x4; from 70 km a block filled 60 pixels,
// a mosaic of blurred squares): a cubic B-spline over the texels (no
// bilinear diamonds, the blocks' edges smoothed), and detail below a
// texel's size from the volumetric clouds' noise, eroding the thin clouds
// and the edges into puffs (thick cloud stays). w: how magnified (0: not,
// the texture as it is)
vec4 TexBSpline(sampler2D t, vec2 uv)
{
	vec2 size = vec2(textureSize(t, 0));
	vec2 p = uv * size - 0.5, i = floor(p), f = p - i;
	vec2 f2 = f * f, f3 = f2 * f;
	vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
	vec2 g0 = w0 + w1, g1 = w2 + w3;
	vec2 h0 = (i - 0.5 + w1 / g0) / size, h1 = (i + 1.5 + w3 / g1) / size;
	return g0.y * (g0.x * textureLod(t, vec2(h0.x, h0.y), 0.0) + g1.x * textureLod(t, vec2(h1.x, h0.y), 0.0))
	     + g1.y * (g0.x * textureLod(t, vec2(h0.x, h1.y), 0.0) + g1.x * textureLod(t, vec2(h1.x, h1.y), 0.0));
}

float CloudLayerMagnification(vec2 uvT)
{
	vec2 tx = uvT * vec2(textureSize(surfTex, 0));
	float texPerPx = max(length(dFdx(tx)), length(dFdy(tx)));
	return smoothstep(1.0, 0.25, texPerPx); // (fully from a texel of four pixels)
}

// the layer's opacity a (0..1) with detail; p: the point (planet frame)
float CloudLayerDetail(float a, vec3 p, float w)
{
	float pxSize = max(length(dFdx(p)), length(dFdy(p))); // (before the branch: derivatives in uniform flow)
	if (w <= 0.0) return a;
	// two octaves, each a difference of two samples (zero mean); the finer
	// one only where it is larger than a few pixels
	vec3 q = p / 9000.0;
	float n = textureLod(atmCloudShape, q, 0.0).r - textureLod(atmCloudShape, q * 1.07 + vec3(0.37, 0.61, 0.13), 0.0).r;
	vec3 q2 = p / 2600.0;
	float n2 = textureLod(atmCloudShape, q2, 0.0).r - textureLod(atmCloudShape, q2 * 0.93 + vec3(0.71, 0.29, 0.53), 0.0).r;
	n += 0.6 * n2 * smoothstep(150.0, 40.0, pxSize);
	// strongest in thin cloud and at the edges, none in clear sky or overcast
	float edge = 4.0 * a * (1.0 - a);
	float d = clamp(a + 1.1 * n * edge, 0.0, 1.0);
	d = mix(d, smoothstep(0.1, 0.9, d), 0.6); // (crisper puffs)
	// (a veil of the layer's own stays under them: thin cloud thinned, not
	// cleared, the cover kept)
	float v = 0.55 * a, k = 0.15; // (a smooth maximum: a crease showed as a dark rim in the relief)
	float hm = clamp(0.5 + 0.5 * (d - v) / k, 0.0, 1.0);
	d = mix(v, d, hm) + k * hm * (1.0 - hm);
	return mix(a, d, w);
}

void main()
{
	vec3 N = normalize(vNormalView);
	vec3 Ng = N; // (geometric: water, night side)
	vec3 L = frame.sunDir.xyz;
	// clamp to the texel centres at the tile edges (the tiles' sampler clamps
	// there too: the footprint widened towards the poles, about a hundred
	// texels east-west, wrapped round to the tile's far edge and showed as a
	// band along every tile edge)
	vec2 hs = 0.5 / vec2(textureSize(surfTex, 0));
	vec2 uvT = clamp(vUV * tile.surfUV.zw + tile.surfUV.xy, hs, 1.0 - hs);
	vec4 bt = textureGrad(surfTex, uvT, PolarGradX(uvT, hs), PolarGradY(uvT, hs));
	vec3 base = bt.rgb;
	// procedural worlds: the surface tile's alpha is the ground share of the site materials (ProcSurface.cpp)
	float siteG = (SITE_MATERIALS && atm.sitePrm.x > 0.5 && tile.params.z < 0.5) ? bt.a : 0.0;
	float cloudA = 0.0, cloudA0 = 0.0; // (a cloud layer: its opacity, with detail where magnified; without)
	if (tile.params.z > 0.5) {
		float w = CloudLayerMagnification(uvT);
		vec4 c = texture(surfTex, uvT);
		if (w > 0.0) {
			vec4 cs = TexBSpline(surfTex, uvT);
			c = mix(c, cs, w);
			base = mix(base, cs.rgb, w);
		}
		vec3 pc = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		cloudA0 = c.a;
		cloudA = CloudLayerDetail(c.a, pc, atm.misc.x > 0.5 ? w : 0.0);
	}
	if (tile.params.z < 0.5 && atm.surfPrm.x != 1.0) // (the textures' colour tints, per planet)
		base = max(mix(vec3(dot(base, vec3(0.2126, 0.7152, 0.0722))), base, atm.surfPrm.x), 0.0);
	// procedural detail below the textures' resolution (terrain only)
	float detailSky = 1.0; // (sky visibility of the stones)
	float detailShadow = 1.0; // (shadows of the procedural craters)
	vec3 cityLight = vec3(0.0); // (street lamps)
	float cityCover = 0.0;
	float playa = 0.0; // (dry lake factor of the terrain detail)
	float siteSmooth = 0.0; // (smooth site materials: damps the grain like a playa)
	if (tile.params.z < 0.5) {
#ifdef TERRAIN_DETAIL
		float land = 1.0; // (no stones on water: the mask's alpha)
		if (tile.params.x > 0.5) {
			vec2 hmL = 0.5 / vec2(textureSize(maskTex, 0));
			land = smoothstep(0.3, 0.7, texture(maskTex, clamp(vUV * tile.maskUV.zw + tile.maskUV.xy, hmL, 1.0 - hmL)).a);
		}
		// the playas' colour (the core's weight per vertex: also on the tiles without detail shading)
		if (atm.playaPrm.x > 0.5 && !(SITE_MATERIALS && atm.sitePrm.x > 0.5) && vDetailA.z > 0.0) {
			float salt = PlayaSalt(normalize(mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz));
			base = mix(base, PlayaColour(base, salt), clamp(vDetailA.z, 0.0, 1.0) * land);
		}
		TerrainDetailShading(vPosView, N, base, vDetailA, detailShadow, land, detailSky, playa, siteG, siteSmooth);
#endif
		cityLight = CityGround(vPosView, base, cityCover);
		TrackShading(vPosView, N, base);
		SurfaceDetail(vPosView, length(vPosView), N, base, max(playa, siteSmooth));
	} else if (dot(vPosView, N) < 0.0) {
		// cloud tops seen from above: relief from the cloud density (thicker
		// clouds are taller; exaggerated for the texture's smoothness), shading their sides (surface
		// gradient bump mapping from the screen-space derivatives)
		// (the detail's relief less: at full strength its puffs' crevices
		// read as cracks)
		float h = 5000.0 * mix(cloudA0, cloudA, 0.2);
		vec3 sx = dFdx(vPosView), sy = dFdy(vPosView);
		vec3 r1 = cross(sy, N), r2 = cross(N, sx);
		float det = dot(sx, r1);
		vec3 grad = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
		if (abs(det) > 1e-9) N = normalize(abs(det) * N - grad);
	}
	float ndl = dot(N, L);
	bool hasAtm = atm.misc.x > 0.5;
	vec3 sunT = vec3(tile.morph.z); // (eclipses)
	vec3 pPlanet = vec3(0.0), dPlanet = vec3(0.0);
	if (hasAtm) {
		pPlanet = mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz;
		dPlanet = normalize(mat3(atm.viewToPlanet) * vPosView);
		// terrain under the clouds (and, under an overcast, the haze's light from about the sun)
		sunT = tile.params.z < 0.5 ? SurfaceSunlight(pPlanet) : SunTransmittance(pPlanet);
	}
	vec3 sunAtm = sunT; // (before the shadows)
	// terrain receives shadows: ray traced, or from the shadow maps
	bool rtVis = RTLighting() && tile.params.z < 0.5;
	vec4 lv = rtVis ? RTLightVisibility() : vec4(1.0);
	if (tile.params.z < 0.5) sunT *= (rtVis ? RTVisibility().x : SunShadow(vPosView, N)) * detailShadow;
	float ao = tile.params.z < 0.5 ? AmbientOcclusion() : 1.0; // clouds are not in the prepass
	float aoDirect = mix(1.0, ao, frame.aoParams.z); // contact darkening in direct light too
	// the night side gets no skylight: only a fraction of the ambient level
	// remains (moon and starlight)
	float ambNight = tile.params.z < 0.5 ? mix(0.15, 1.0, smoothstep(-0.25, 0.0, dot(Ng, L))) : 1.0;
	// regolith of airless bodies: no limb darkening, opposition surge
	float sunRefl = int(atm.detail.x + 0.5) == DETAIL_AIRLESS && tile.params.z < 0.5
		? RegolithReflectance(N, L, normalize(-vPosView), Ng) : max(ndl, 0.0);
	// under an overcast: the sky-shaped ambient (C3), dimmer towards the ground below
	vec3 ambT = frame.ambient.rgb;
	vec3 upV = vec3(0.0, 1.0, 0.0);
	if (tile.params.z < 0.5) {
		upV = normalize(transpose(mat3(atm.viewToPlanet)) * normalize(mat3(atm.viewToPlanet) * vPosView + atm.camPos.xyz));
		if (hasAtm && frame.overcast.w > 0.0 && AtmOvercast())
			ambT += OvercastAmbientDelta(frame.overcast.rgb, frame.overcast.rgb, base, clamp(dot(N, upV), -1.0, 1.0));
	}
	vec3 irr = ambT * ao * ambNight * detailSky + (frame.sunColor.rgb * sunT * sunRefl + LocalLighting(vPosView, N, lv)) * aoDirect;
	vec3 col = base * irr;
	// street lamps, lit when the sun is down
	col += base * cityLight * smoothstep(0.05, -0.03, dot(Ng, L));
	col += cityEmis * smoothstep(0.05, -0.03, dot(Ng, L));
	// sunlight bounced by the ground around onto faces tilted away from the
	// vertical (the shaded sides of stones and crater walls): a sunlit
	// Lambertian half-space of the local albedo gives the irradiance
	// albedo * E_sun * (1 - cos tilt) / 2
	if (tile.params.z < 0.5) {
		float sunUp = max(dot(L, upV), 0.0);
		float tiltF = 0.5 * (1.0 - clamp(dot(N, upV), -1.0, 1.0));
		col += base * base * frame.sunColor.rgb * (sunUp * tiltF * ao) * (hasAtm ? (AtmOvercast() ? sunAtm : SunTransmittance(pPlanet)) : vec3(1.0)) * tile.morph.z;
	}

	if (tile.params.z > 0.5) { // cloud layer: lit from above, also seen from below
		float a = cloudA * tile.params.y;
		// near the camera the volumetric clouds replace the layer
		if (tile.params.w > 0.0) a *= smoothstep(0.75 * tile.params.w, tile.params.w, length(vPosView));
		// seen from below: the underside, shaded by the cloud's own thickness
		if (dot(vPosView, normalize(vNormalView)) > 0.0) col *= mix(1.0, 0.35, a);
		if (hasAtm) {
			vec3 T;
			vec3 L = Inscatter(dPlanet, length(vPosView), T);
			col = col * T + AtmTonemap(L);
		}
		outColor = vec4(col, a);
		return;
	}

	if (tile.params.x > 0.5) {
		vec2 hm = 0.5 / vec2(textureSize(maskTex, 0));
		vec4 m = texture(maskTex, clamp(vUV * tile.maskUV.zw + tile.maskUV.xy, hm, 1.0 - hm));
		// night lights fade in beyond the terminator
		float night = clamp(0.2 - dot(Ng, L) * 4.0, 0.0, 1.0);
		// the night light texture represents lights seen from altitude; close up
		// it would flood the ground, so it fades out near the camera
		// (from 6 km of altitude up, with the city's own lights gone, at full strength: CityHandOff)
		float cityT = CityHandOff();
		night *= mix(smoothstep(1000.0, 10000.0, length(vPosView)), 1.0, cityT);
		// (where the cities draw their own lights: theirs; near them, the
		// texture's brightness for views from orbit toned down to theirs)
		// (the cities' lights: of the buildings drawn, of all of them only so
		// near as the small ones, building.vert's fade at 3 km, and of the
		// roof carpet beyond; elsewhere the texture's)
		night *= (1.0 - max(cityCover * (1.0 - smoothstep(1300.0, 2300.0, length(vPosView))), cityCarpet)) * mix(mix(0.15, 1.0, smoothstep(8000.0, 80000.0, length(vPosView))), 1.0, cityT);
		// (the texture's lights are blurred averages of point sources: cores
		// brightened, dim spill suppressed, so that cities stand out as in
		// photographs; the finest levels' halftone of lit texels blurred
		// further: a glow, not dots)
		vec2 muv = vUV * tile.maskUV.zw + tile.maskUV.xy, mt = 2.0 * hm; // (a texel)
		vec3 ml = vec3(0.0);
		for (int i = -1; i <= 1; i++)
			for (int j = -1; j <= 1; j++)
				ml += texture(maskTex, clamp(muv + vec2(i, j) * 1.5 * mt, hm, 1.0 - hm)).rgb * ((i == 0 ? 2.0 : 1.0) * (j == 0 ? 2.0 : 1.0) / 16.0);
		// the mask's texel [m] (shore foam), from derivatives: here, in uniform control flow
		float texM = 0.0;
		if (atm.shorePrm.x > 0.0 && atm.shorePrm.w > 0.0) {
			vec2 msz = vec2(textureSize(maskTex, 0));
			vec2 ma = dFdx(muv) * msz, mb = dFdy(muv) * msz;
			texM = sqrt(length(cross(dFdx(vPosView), dFdy(vPosView))) / max(abs(ma.x * mb.y - ma.y * mb.x), 1e-12));
		}
		float lm = max (ml.r, max (ml.g, ml.b));
		col += ml * (3.0 * lm * smoothstep (0.04, 0.25, lm)) * night * tile.params.y;
		// water (mask alpha 0): reflection of the sky and the sun's glitter,
		// which spreads out when seen from afar
		if (m.a < 0.5) {
			float dist = length(vPosView);
			vec3 V = normalize(-vPosView);
			float fxVar, fxFoam, waveVar;
			float fl;
			vec3 Nw = WaterNormal(Ng, vPosView, dist, fxVar, fxFoam, waveVar, fl);
			// whitecaps and shore foam join the jets' foam
			float capVar = 0.0, g3 = 0.0, fxJet = fxFoam;
			if (atm.seaPrm.w > 0.0 || atm.shorePrm.x > 0.0) {
				float fadeW = 1.0 - smoothstep(300.0, 3000.0, dist);
				vec3 pP = mat3(atm.viewToPlanet) * vPosView, pw = pP + atm.water.xyz;
				float footprint = dist * 2.0 * atm.misc.z / max(frame.jitter.w, 1.0) / max(abs(dot(normalize(vPosView), Ng)), 0.05);
				float cap = Whitecaps(pP, pw, footprint, fadeW, capVar) * fl;
				capVar *= fl;
				float shore = ShoreFoam(pw, ShoreDistance(clamp(muv, hm, 1.0 - hm), texM), texM, footprint);
				if (cap > 0.0 || shore > 0.0) {
					g3 = 1.0 - (1.0 - cap) * (1.0 - shore);
					fxFoam = 1.0 - (1.0 - fxJet) * (1.0 - g3);
				}
			}
			float NoV = max(dot(Nw, V), 1e-3);
			float fres = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
			float rgh = clamp(fxVar / 0.008, 0.0, 1.0); // roughened by a jet: matte, less reflective
			fres *= mix(1.0, 0.55, rgh);
			if (hasAtm) {
				vec3 R = reflect(-V, Nw);
				// (reflections towards the water itself would see it again: kept
				// above the horizon)
				float rh = dot(R, N);
				if (rh < 0.02) R = normalize(R + N * (0.02 - rh));
				vec3 T;
				vec3 sky = AtmTonemap(Inscatter(normalize(mat3(atm.viewToPlanet) * R), ATM_NONE, T));
				// screen-space reflections (traced with the smooth surface),
				// displaced by the waves
				vec3 dN = Nw - N;
				vec4 s = ScreenReflection(vec2(dN.x, -dN.y) * 0.03);
				s *= 1.0 - 0.8 * rgh; // (rgb is premultiplied: rough water breaks up the vessel's image)
				sky = sky * (1.0 - s.a) + s.rgb;
				if (frame.probe.w > 0.5) { // debug view
					outColor = vec4(s.rgb + vec3(0.0, 0.0, 0.3) * (1.0 - s.a), 1.0);
					return;
				}
				col = mix(col, sky, fres * (1.0 - fxFoam));
			}
			// the glitter is widened by the sea's slopes that the drawn waves don't
			// resolve (finer than the pixels, beyond the 3 km fade, below the
			// shortest wave), so that it has the same total at every distance
			// (Cox-Munk); bodies without a sea state keep the old ramp
			float rough = atm.seaPrm.y > 0.0 ? min(sqrt(sqrt(6.25e-6 + waveVar)), 0.6)
				: mix(0.05, 0.3, smoothstep(2e3, 3e5, dist));
			if (fxVar + capVar > 0.0) rough = min(sqrt(sqrt(rough * rough * rough * rough + 2.0 * (fxVar + capVar))), 0.6);
			float roughS = frame.overcast.w > 0.0 ? sqrt(min(rough * rough + 0.5 * frame.overcast.w, 1.0)) : rough; // (a large source, C3)
			col += frame.sunColor.rgb * sunT * SpecularBRDF(Nw, V, L, vec3(0.02), roughS) * PI * (1.0 - fxFoam);
			if (fxJet > 0.0) col = mix(col, FOAM_ALBEDO * irr, fxJet);
			if (g3 > 0.0) {
				// whitecaps and shore foam are lit by the waves' normal
				vec3 irrW = frame.ambient.rgb * ao * ambNight + (frame.sunColor.rgb * sunT * max(dot(Nw, L), 0.0) + LocalLighting(vPosView, Nw, lv)) * aoDirect;
				col = mix(col, FOAM_ALBEDO * irrW, g3);
			}
		}
	}
	if (hasAtm) { // aerial perspective
		vec3 T;
		vec3 L = Inscatter(dPlanet, length(vPosView), T);
		col = col * T + AtmTonemap(L);
	}
	outColor = vec4(col, 1.0);
}
