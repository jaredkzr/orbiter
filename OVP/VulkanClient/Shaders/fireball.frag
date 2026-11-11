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
#include "fireball_flow.glsl"
layout(set = 1, binding = 1) uniform sampler3D gridA;       // density, temperature [kK], soot
layout(set = 1, binding = 2) uniform sampler3D gridB;       // optical depth towards the sun, the sky
layout(set = 1, binding = 3) uniform sampler3D gridC;       // the warp of the noise's coordinates
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
	vec3 seedG = fract (vec3 (0.137, 0.291, 0.443) * P.time.w); // (the grid's: the roll's lopsidedness)
	vec3 sdA = seed + FbPhaseSeed (P.adv2.z), sdB = seed + FbPhaseSeed (P.adv2.w);
	// (a phase of the flow near its end or its start weighs little: its
	// taps are left out - phase 6, a sixth of the march's noise taps)
	// (a phase's taps skipped while its weight is small - phase 6 - its
	// weight taken down to nought continuously over it: cut at 0.07, its
	// share of the billows' edge went in a frame, a lobe's limb popping in
	// or out each time a phase began or ended)
	float wA = max (P.adv.y - 0.07, 0.0) / 0.93, wB = max (P.adv.w - 0.07, 0.0) / 0.93;
	float wN = inversesqrt (max (wA * wA + wB * wB, 1e-4));
	float fineW = P.asym.z; // (the finest octave: only where it can be seen)
	// the light on it: the sun through the air to its centre (in the
	// planet's shadow at night), forward-peaked (the sunlit edges of the
	// billows seen against the sun bright), and the ambient light
	vec3 sun = frame.sunColor.rgb;
	if (atm.misc.x > 0.5) sun *= SurfaceSunlight (mat3 (atm.viewToPlanet) * P.centre.xyz + atm.camPos.xyz);
	float g = 0.35, cosT = dot (d, frame.sunDir.xyz);
	float phase = 0.6 * (1.0 - g * g) / pow (1.0 + g * g - 2.0 * g * cosT, 1.5) + 0.4; // (mean 1)
	vec3 sunL = sun * phase;
	float gV = 0.5, phaseV = 0.5 * (1.0 - gV * gV) / pow (1.0 + gV * gV - 2.0 * gV * cosT, 1.5) + 0.5; // (the free cloud's droplets and ice: brightest against the sun)
	vec3 amb = frame.ambient.rgb * frame.ambient.w;
	float soft = 1.0 / (0.12 * r);  // (fading out over this before the ground or the sea)
	float sig0 = P.flow.w * P.heat.w / r; // (fading out as a whole as its soot passes to the smoke volume)
	// the sun's direction in ball space, and a few cells of its way out
	// through the grid at full resolution (the sunlit side of each billow
	// bright, its far side dark: the grid of optical depths is coarse and
	// soft, and the ray starts into it deep enough to shade the lit surface)
	vec3 sunB = normalize (mat3 (P.toBall) * frame.sunDir.xyz);
	float cell = min (min (size.x / float (FB_GX), size.y / float (FB_GY)), size.z / float (FB_GZ));
	const float SUN_TAPS[3] = float[3] (0.8, 1.8, 3.2); // [cells] along the sun
	const float SUN_FAR = 4.2;
	float sigB = (1.0 / r) * P.flow.w * P.heat.w; // (the grid's optical depth per metre per unit density, times the extinction)
	// its soot's albedo: fresh soot near black, a little lighter as it burns
	// out and the cap ages (diluted by the air it draws in). (Up to 0.3 the
	// aging cap turned a pale mid-grey by 1.6 t_f - a hydrocarbon fire's
	// smoke stays charcoal for minutes - and handed over to the smoke
	// volume's cap a shade lighter still)
	float u = P.time.y;
	float albSoot = mix (0.12, 0.16, clamp ((u - 0.9) / 0.9, 0.0, 1.0));
	// (and as the ball fades out - into the smoke volume's cap, or to its
	// sprites - its thinning soot loses its self-shadowing and would turn a
	// pale sunlit grey for its last frames (a two-frame pale flash at 1.8
	// t_f, phase 5's sea video): its albedo goes down to the cap's 0.08 with
	// its extinction instead - phase 6)
	albSoot *= mix (0.25, 1.0, smoothstep (0.0, 0.5, P.heat.w));
	// (in vacuum: a thin vapour shell - pale, lit by the sun, its light the
	// flash's for a frame; its opacity carries the look, not its emission)
	bool vac = P.mode.x > 0.5;
	float flash = vac ? P.mode.w : P.heat.z;
	// the fire's own light on its soot: looking in from the soot towards
	// the fire (the grid: luminance), the colour of its hot gas
	vec3 inCol = FbPlanck (0.9 * P.heat.x);
	inCol /= max (dot (inCol, vec3 (0.2126, 0.7152, 0.0722)), 1e-4);
	float kIn = 0.7 * P.tune.w;

	vec3 L = vec3 (0.0);
	float T = 1.0, tW = 0.0, wSum = 0.0, zIn = 0.0, zOp = 0.0, tLast = t0;
	for (int i = 0; i < n; i++) {
		float t = t0 + (float (i) + jit) * dt;
		if (t >= t1) break;
		vec3 pb = o + db * t;
		vec3 gc = FbGridCoord ((pb - P.boxMin.xyz) * isz, slab);
		vec4 A = textureLod (gridA, gc, 0.0);
		if (A.x < 0.002) continue;
		float sig;
		vec3 S, E;
		if (vac) {
			// the free cloud (VACUUM_PLAN.md 3.3): its density the grid's, its
			// finer streamers one tap (their coordinates in C); its extinction
			// mode.z, falling as 1 / s^3 as it grows
			vec3 wv = textureLod (gridC, gc, 0.0).xyz;
			float dn = (textureLod (detailNoise, wv * 0.5, 0.0).r - 0.475) * 4.2;
			float rho = A.x * clamp (1.0 + 0.5 * dn, 0.2, 1.7) * clamp ((tSurf - t) * soft, 0.0, 1.0);
			if (rho <= 0.0) continue;
			sig = P.mode.z * P.heat.w * rho;
			// the sunlight through it: the Gaussian core's optical depth towards
			// the sun from here, in closed form (sqrt(pi)/2 s exp(-b^2) erfc(a));
			// a floor of light scattered many times (its droplets and ice: white)
			float sc = max (P.mode.y, 0.1);
			float aS = dot (pb, sunB) / sc, b2 = max (dot (pb, pb) / (sc * sc) - aS * aS, 0.0);
			float erfA = tanh (aS * (1.128379 + 0.100886 * aS * aS));
			float tauS = P.mode.z * P.heat.w * sc * 0.886227 * exp (-b2) * (1.0 - erfA);
			float sunF = 0.4 + 0.6 * exp (-0.5 * tauS); // (white: light scattered many times fills its shade - at 0.12 its dense core seen from the shade side was a dark, smoke-like blob)
			S = 0.9 * (sun * phaseV * sunF + amb);
			// its hot core: a blackbody cooling as it expands (heat.x, the
			// grid's profile), white as it flashes
			E = FbPlanck (A.y * 1000.0) * 0.7 * P.tune.w;
			if (flash > 0.0) E = mix (E, vec3 (4.0, 3.7, 3.0), flash);
		} else {
			// the noise's coordinates as the grid's (fireball_grid.comp): the
			// point in units of 0.9 r, leaning, traced back by the ring's turn,
			// warped (the warp from the grid), carried outward by the two
			// phases of the flow, cross-faded
			vec3 x = pb / (0.9 * r);
			x.xy -= P.asym.xy * max (x.z + 0.6, 0.0);
			vec3 wv = textureLod (gridC, gc, 0.0).xyz;
			vec3 qA = (FbRolled (x, P.adv2.x, P.flow.y, seedG.x) + wv) * P.adv.x;
			vec3 qB = (FbRolled (x, P.adv2.y, P.flow.y, seedG.x) + wv) * P.adv.z;
			// detail: small billows eating into its thin parts (the edge ragged,
			// tongues and holes), the dense inside kept
			// (normalized: about +-1; the shape noise's sd 0.073 about 0.67, the detail's 0.12 about 0.475)
			float dn = 0.0;
			if (wA > 0.0) dn += wA * (0.6 * (textureLod (shapeNoise, qA * 2.3 + sdA, 0.0).r - 0.67) * 6.8 + 0.4 * (textureLod (detailNoise, qA * 3.1 + sdA.zxy, 0.0).r - 0.475) * 4.2);
			if (wB > 0.0) dn += wB * (0.6 * (textureLod (shapeNoise, qB * 2.3 + sdB, 0.0).r - 0.67) * 6.8 + 0.4 * (textureLod (detailNoise, qB * 3.1 + sdB.zxy, 0.0).r - 0.475) * 4.2);
			dn *= wN;
			// (and finer: about 1/25 of its radius, the flame's texture close
			// up; not carried: too fine to be seen moving; left out when it is
			// under a pixel or so - fineW)
			float df = 0.0;
			if (fineW > 0.0) df = fineW * (textureLod (shapeNoise, (x + wv) * 6.1 + seed.yzx, 0.0).r - 0.67) * 6.8;
			dn += 0.35 * df;
			// (soot a little more: its small billows crisp - the cap's
			// cauliflower; at 1.5x the sooty outer layer was eaten away as it
			// spread and the silhouette shrank with the burn-out - phase 4b)
			float ero = clamp ((0.3 + 0.3 * dn) * (1.0 + 0.2 * A.z) * P.tune.z, 0.0, 0.95);
			float rho = clamp ((A.x - ero) / (1.0 - ero), 0.0, 1.0);
			rho = smoothstep (0.0, 0.5, rho); // (a crisp edge to the billows: the density rising over a short way)
			rho *= clamp ((tSurf - t) * soft, 0.0, 1.0);
			if (rho <= 0.0) continue;
			float soot = A.z;
			float sig_ = rho * (FB_SIGMA_FLAME + FB_SIGMA_SOOT * soot) * sig0;
			float alb = mix (0.05, albSoot, soot); // (the hot gas scatters little: its soot does)
			// (its temperature varying a little with the detail: brighter and
			// darker flecks over the billows, not a smooth glow; more read as
			// streaks of marble)
			float Tk = A.y * 1000.0 * (1.0 + 0.03 * dn + 0.015 * df);
			// the sunlight through it: the first cells towards the sun at full
			// resolution, the rest from the grid of optical depths beyond them
			float tauS = 0.0;
			if (soot > 0.02 || alb > 0.06) {
				for (int k = 0; k < 3; k++) {
					vec4 As = textureLod (gridA, FbGridCoord ((pb + sunB * (SUN_TAPS[k] * cell) - P.boxMin.xyz) * isz, slab), 0.0);
					float ds = (k == 0 ? SUN_TAPS[0] + 0.5 * (SUN_TAPS[1] - SUN_TAPS[0]) : k == 2 ? SUN_FAR - 0.5 * (SUN_TAPS[2] + SUN_TAPS[1]) : 0.5 * (SUN_TAPS[2] - SUN_TAPS[0])) * cell;
					tauS += As.x * (FB_SIGMA_FLAME + FB_SIGMA_SOOT * As.z) * ds;
				}
				tauS *= sigB;
			}
			vec3 tau = textureLod (gridB, FbGridCoordB ((pb + sunB * (SUN_FAR * cell) - P.boxMin.xyz) * isz, slab), 0.0).xyz;
			tauS += tau.x;
			// (soot scatters little more than once: a low floor of light
			// scattered many times; the shade side dark)
			float sunF = 0.05 + 0.95 * (0.85 * exp (-tauS) + 0.15 * exp (-0.2 * tauS));
			vec3 tauU = textureLod (gridB, FbGridCoordB ((pb - P.boxMin.xyz) * isz, slab), 0.0).xyz;
			// (the dense soot scatters the sun about evenly - its lit side seen
			// from the sun a lighter grey, from the shade side dark with a bright
			// rim; the thin hot gas forward: with the forward lobe alone the
			// cap's sunlit side was the darker)
			vec3 S_ = alb * (sunL * mix (1.0, 0.3 + 0.7 / phase, soot) * sunF + amb * (0.15 + 0.85 * exp (-0.5 * tauU.y)) + inCol * (kIn * tauU.z));
			vec3 E_ = (1.0 - alb) * FbPlanck (Tk) * P.tune.w;
			// the ignition flash: white-hot (all three channels over the knee
			// below), handed over to the orange ball within a tenth of a second
			if (flash > 0.0) E_ = mix (E_, vec3 (4.0, 3.7, 3.0), flash);
			sig = sig_;
			S = S_, E = E_;
		}
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
	// its light compressed channel by channel as a camera's sensor clips
	// (red first, then green: an orange brighter than its knee goes
	// yellow, then white - the fire of photographs), with a knee at a
	// sunlit white: the display's curve above that mixes towards white
	// and turned the ball's saturated orange a pale peach. Its body (its
	// red channel at the knee, its green well under it) stays a saturated
	// orange, its hottest cores a yellow-white
	{
		float knee = 1.0 * P.tune.w;
		vec3 c = L / alpha;
		vec3 lin = min (c, 0.6 * knee);
		c = lin + 0.4 * knee * (1.0 - exp (-max (c - 0.6 * knee, 0.0) / (0.4 * knee)));
		L = c * alpha;
	}
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
