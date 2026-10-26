// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Atmosphere (Rayleigh, Mie, ozone): single scattering ray-marched along
// the view, with sunlight transmittance and all higher scattering orders
// from precomputed tables (atmlut.glsl). All positions are in the planet
// frame, in metres. The including shader defines ATM_SET, the descriptor
// set of the atmosphere uniform block.

layout(set = ATM_SET, binding = 0) uniform Atmosphere {
	mat4 viewToPlanet;  // view space -> planet frame (rotation)
	vec4 camPos;        // camera position, planet frame [m]
	vec4 sunDir;        // unit vector towards the sun, planet frame
	vec4 betaR;         // Rayleigh scattering coefficients [1/m] (rgb), scale height [m] (w)
	vec4 betaM;         // Mie scattering coefficients [1/m] (rgb), scale height [m] (w)
	vec4 radii;         // planet radius, atmosphere radius, Mie asymmetry g, sun intensity
	vec4 camC;          // |camPos|^2 - r^2 for the planet (x) and atmosphere (y) radius
	vec4 misc;          // x: atmosphere present, y: 1/fx, z: 1/fy, w: airglow strength
	vec4 ground;        // planet colour (rgb), w: surface textures present
	vec4 cloud;         // cloud layer radius, rotation angle, shadow depth, w: present
	vec4 water;         // camera relative to the 1 km-snapped origin of surface patterns, w: time [s] (< 0: no ripples)
	vec4 ozone;         // absorption [1/m] (rgb): ozone layer, or (w = 1) aerosols (dust)
	vec4 detail;        // procedural surface detail: type (0 none, 1 airless, 2 Mars, 3 Earth), strength, range [m]
	vec4 aurora;        // geomagnetic north pole (unit, planet frame), w: aurora strength (0: none)
	vec4 mieLobe;       // aerosols' forward lobe: asymmetry g per colour (rgb), its share (w; 0: none)
	vec4 cloudMap;      // volumetric clouds near the camera: coverage map centre (planet frame), half size (w; 0: none)
	vec4 cloudNoise;    //   shape noise offset (xyz), scale (w)
	vec4 cloudShell;    //   shell base and top radius, extinction [1/m], coverage scale
	vec4 detailPrm;     // terrain detail: ridged share, crater cells, crater depth, fresh craters
	vec4 surfPrm;       // x: colour saturation of the surface textures; yzw: diffuse radiance of overcast atmospheres (per unit sun intensity)
} atm;

// planet surface (western and eastern hemisphere), for reflections
layout(set = ATM_SET, binding = 1) uniform sampler2D atmGround0;
layout(set = ATM_SET, binding = 2) uniform sampler2D atmGround1;
// cloud layer (western and eastern hemisphere), for cloud shadows
layout(set = ATM_SET, binding = 3) uniform sampler2D atmCloud0;
layout(set = ATM_SET, binding = 4) uniform sampler2D atmCloud1;
// precomputed tables
layout(set = ATM_SET, binding = 5) uniform sampler2D atmTransmittance;
layout(set = ATM_SET, binding = 6) uniform sampler2D atmMultiscat;
// the volumetric clouds' coverage map and shape noise (Clouds.cpp)
layout(set = ATM_SET, binding = 7) uniform sampler2D atmCloudMap;
layout(set = ATM_SET, binding = 8) uniform sampler3D atmCloudShape;

#include "atmlut.glsl"


const float ATM_PI = 3.14159265;
const float ATM_NONE = 1e30;

// Intersections of the ray o + t d (|d| = 1) with a sphere around the origin,
// given c = |o|^2 - r^2 (computed accurately by the caller)
// Mie phase function (Cornette-Shanks) at the cosine mu of the scattering
// angle: asymmetry g (atm.radii.z), and for aerosols with a diffraction
// peak a narrower forward lobe per colour (atm.mieLobe)
float AtmCS (float g, float mu)
{
	return 3.0 / (8.0 * ATM_PI) * ((1.0 - g * g) * (1.0 + mu * mu)) /
		((2.0 + g * g) * pow (max (1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
}
vec3 AtmMiePhase (float mu)
{
	float p = AtmCS (atm.radii.z, mu);
	if (atm.mieLobe.w <= 0.0) return vec3 (p);
	vec3 lobe = vec3 (AtmCS (atm.mieLobe.r, mu), AtmCS (atm.mieLobe.g, mu), AtmCS (atm.mieLobe.b, mu));
	return mix (vec3 (p), lobe, atm.mieLobe.w);
}

vec2 RaySphereC (vec3 o, vec3 d, float c)
{
	float b = dot (o, d);
	float disc = b * b - c;
	if (disc < 0.0) return vec2 (ATM_NONE, -ATM_NONE);
	float s = sqrt (disc);
	return vec2 (-b - s, -b + s);
}

vec2 RaySphere (vec3 o, vec3 d, float r)
{
	float lo = length (o);
	return RaySphereC (o, d, (lo - r) * (lo + r));
}

// Transmittance of sunlight arriving at p (radius r, sun zenith cosine muS).
// Near the planet's shadow the sun sets gradually (its disc and refraction
// spread the transition over about a degree), so that the shadow cast into
// the air has no hard edge.
vec3 AtmSunTransmittance (vec3 p, float r, float muS)
{
	float Rg = atm.radii.x;
	float rr = max (r, Rg);
	float muH = -sqrt (max (1.0 - (Rg / rr) * (Rg / rr), 0.0)); // horizon
	float vis = smoothstep (-0.01, 0.01, muS - muH);
	if (vis <= 0.0) return vec3 (0.0);
	rr = min (rr, atm.radii.y);
	vec2 uv = AtmTransmittanceUv (rr, max (muS, muH + 1e-4), Rg, atm.radii.y);
	return textureLod (atmTransmittance, uv, 0.0).rgb * vis;
}

vec3 SunTransmittance (vec3 p)
{
	float r = length (p);
	return AtmSunTransmittance (p, r, dot (p, atm.sunDir.xyz) / r);
}

float AtmSq (float x) { return x * x; }

// Light scattered towards the camera along the view ray d up to distance
// tMax, and the transmittance T of that path
vec3 Inscatter (vec3 d, float tMax, out vec3 T)
{
	T = vec3 (1.0);
	vec3 o = atm.camPos.xyz;
	vec2 ta = RaySphereC (o, d, atm.camC.y);
	if (ta.x > ta.y || ta.y <= 0.0) return vec3 (0.0);
	float t0 = max (ta.x, 0.0), t1 = min (ta.y, tMax);
	if (t1 <= t0) return vec3 (0.0);

	const int N = 16;
	float ds = (t1 - t0) / float (N);
	vec3 tau = vec3 (0.0);
	vec3 sumR = vec3 (0.0), sumM = vec3 (0.0), sumMS = vec3 (0.0);
	float Rg = atm.radii.x, Rt = atm.radii.y;
	for (int i = 0; i < N; i++) {
		vec3 p = o + d * (t0 + ds * (float (i) + 0.5));
		float r = length (p);
		vec3 dens = AtmDensities (r - Rg, atm.betaR.w, atm.betaM.w) * ds;
		vec3 scat = atm.betaR.rgb * dens.x + atm.betaM.rgb * dens.y;
		vec3 ext = scat + atm.betaM.rgb * 0.1 * dens.y + atm.ozone.rgb * (atm.ozone.w > 0.5 ? dens.y : dens.z);
		vec3 Tv = exp (-(tau + 0.5 * ext)); // to the middle of the step
		tau += ext;
		float muS = dot (p, atm.sunDir.xyz) / r;
		vec3 Ts = AtmSunTransmittance (p, r, muS);
		sumR += Tv * Ts * dens.x;
		sumM += Tv * Ts * dens.y;
		// all higher orders, isotropic; under an overcast (Venus, Titan) the
		// diffuse field the table cannot carry through the thick haze
		// (where the sun is up)
		vec3 psi = textureLod (atmMultiscat, AtmMultiscatUv (clamp (r, Rg, Rt), muS, Rg, Rt), 0.0).rgb;
		psi += atm.surfPrm.yzw * smoothstep (-0.1, 0.15, muS);
		sumMS += Tv * scat * psi;
	}
	T = exp (-tau);

	// airglow: a thin emitting shell (oxygen, 557.7 nm) about 95 km up, seen
	// as a green line along the limb on the night side; the path length
	// through the shell (between the radii at 90 and 100 km) up to tMax
	vec3 glow = vec3 (0.0);
	if (atm.misc.w > 0.0) {
		float b2 = dot (o, o) - AtmSq (dot (o, d)); // squared distance of the ray's line from the centre
		float tc = -dot (o, d);
		float ro = Rg + 100e3, ri = Rg + 90e3;
		float path = 0.0;
		if (b2 < ro * ro) {
			float ho = sqrt (ro * ro - b2), hi = b2 < ri * ri ? sqrt (ri * ri - b2) : 0.0;
			// the two segments (before and after the closest point), clipped to [0, tMax]
			path += max (0.0, min (tc - hi, tMax) - max (tc - ho, 0.0));
			path += max (0.0, min (tc + ho, tMax) - max (tc + hi, 0.0));
		}
		// (lost in the sunlit sky: only where the shell is in the night)
		vec3 pc = o + d * clamp (tc, 0.0, tMax);
		float night = smoothstep (0.1, -0.15, dot (normalize (pc), atm.sunDir.xyz));
		glow = vec3 (0.25, 1.0, 0.35) * (path * 6e-9 * atm.misc.w * night);
	}

	// aurora: curtains along the auroral ovals (about 20 degrees from the
	// geomagnetic poles), 100-250 km up, green below and red above, folded
	// and drifting; marched through the shell on the night side
	if (atm.aurora.w > 0.0) {
		float ri = Rg + 100e3, ro = Rg + 250e3;
		vec2 so = RaySphere (o, d, ro);
		float a0 = max (so.x, 0.0), a1 = min (so.y, tMax);
		if (a1 > a0) {
			const int NA = 24;
			float ds = (a1 - a0) / float (NA);
			vec3 sum = vec3 (0.0);
			vec3 pole = atm.aurora.xyz;
			vec3 e1 = normalize (cross (pole, abs (pole.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
			vec3 e2 = cross (pole, e1);
			float time = atm.water.w > 0.0 ? atm.water.w : 0.0;
			for (int i = 0; i < NA; i++) {
				vec3 p = o + d * (a0 + ds * (float (i) + 0.5));
				float r = length (p);
				if (r < ri) continue;
				vec3 u = p / r;
				float h = r - Rg;
				float night = smoothstep (0.15, -0.1, dot (u, atm.sunDir.xyz));
				if (night <= 0.0) continue;
				// geomagnetic colatitude and longitude (both hemispheres)
				float cl = acos (clamp (abs (dot (u, pole)), 0.0, 1.0));
				float ml = atan (dot (u, e2), dot (u, e1));
				// the oval, its edge folded by slow waves
				float fold = 0.02 * sin (ml * 7.0 + time * 0.05) + 0.012 * sin (ml * 23.0 - time * 0.13);
				float band = exp (-AtmSq ((cl - 0.36 - fold) / 0.012));
				// rays along the curtain
				float rays = 0.4 + 0.6 * pow (0.5 + 0.5 * sin (ml * 180.0 + 3.0 * sin (ml * 11.0 + time * 0.3)), 3.0);
				float green = exp (-AtmSq ((h - 115e3) / 15e3)) + 0.3 * smoothstep (130e3, 110e3, h) * step (110e3, h);
				float red = 0.35 * exp (-AtmSq ((h - 210e3) / 40e3));
				sum += (vec3 (0.15, 1.0, 0.3) * green + vec3 (1.0, 0.15, 0.3) * red) * band * rays * night;
			}
			glow += sum * ds * 3e-7 * atm.aurora.w;
		}
	}

	float mu = dot (d, atm.sunDir.xyz);
	float pR = 3.0 / (16.0 * ATM_PI) * (1.0 + mu * mu);
	vec3 pM = AtmMiePhase (mu);
	return atm.radii.w * (sumR * atm.betaR.rgb * pR + sumM * atm.betaM.rgb * pM + sumMS) + glow;
}

// Sunlit planet surface seen from afar, at the unit normal n (planet
// frame): the level-4 surface map (or the planet colour), lit by the sun
vec3 AtmGround (vec3 n)
{
	vec3 alb = atm.ground.rgb;
	if (atm.ground.w > 0.5) {
		float lat = asin (clamp (n.y, -1.0, 1.0)), lng = atan (n.z, n.x);
		float v = (0.5 * ATM_PI - lat) / ATM_PI;
		alb = lng < 0.0 ? texture (atmGround0, vec2 ((lng + ATM_PI) / ATM_PI, v)).rgb
		                : texture (atmGround1, vec2 (lng / ATM_PI, v)).rgb;
	}
	vec3 sunT = atm.misc.x > 0.5 ? SunTransmittance (n * atm.radii.x) : vec3 (1.0);
	return alb * sunT * max (dot (n, atm.sunDir.xyz), 0.0);
}

// Map scattered radiance into displayable range
vec3 AtmTonemap (vec3 L)
{
	return 1.0 - exp (-L);
}

// Fraction of sunlight passing the cloud layer above p (planet frame)
float CloudShadow (vec3 p)
{
	if (atm.cloud.w < 0.5) return 1.0;
	vec3 s = atm.sunDir.xyz;
	vec2 tc = RaySphere (p, s, atm.cloud.x);
	if (tc.y <= 0.0 || tc.x > tc.y) return 1.0; // above the clouds
	vec3 q = normalize (p + s * tc.y);
	const float PI_ = 3.14159265;
	float lat = asin (clamp (q.y, -1.0, 1.0));
	float lng = atan (q.z, q.x) - atm.cloud.y; // into the rotating cloud frame
	lng = mod (lng + PI_, 2.0 * PI_) - PI_;
	float v = (0.5 * PI_ - lat) / PI_;
	float a = lng < 0.0 ? texture (atmCloud0, vec2 ((lng + PI_) / PI_, v)).a
	                    : texture (atmCloud1, vec2 (lng / PI_, v)).a;
	if (atm.cloudMap.w <= 0.0) return 1.0 - atm.cloud.z * a;

	// near the camera, the volumetric clouds: their density along the sun
	// ray through the shell (as clouds.comp's, without the detail noise)
	float base = atm.cloudShell.x, top = atm.cloudShell.y;
	vec2 tt = RaySphere (p, s, top), tb = RaySphere (p, s, base);
	float t0 = length (p) < base ? max (tb.y, 0.0) : 0.0, t1 = tt.y;
	if (t1 <= t0) return 1.0;
	vec3 C = atm.cloudMap.xyz, up = normalize (C);
	vec3 east = normalize (cross (abs (up.y) < 0.999 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0), up));
	vec3 north = cross (up, east);
	const int N = 6;
	float dt = (t1 - t0) / float (N), od = 0.0, edge = 0.0;
	for (int i = 0; i < N; i++) {
		vec3 x = p + s * (t0 + (float (i) + 0.5) * dt);
		vec3 q = x - C;
		vec2 mc = vec2 (dot (q, east), dot (q, north)) / atm.cloudMap.w;
		edge = max (edge, max (abs (mc.x), abs (mc.y)));
		if (edge >= 1.0) break;
		float hf = (length (x) - base) / (top - base);
		float profile = smoothstep (0.0, 0.07, hf) * smoothstep (1.0, 0.3, hf);
		float cov = smoothstep (0.15, 0.85, textureLod (atmCloudMap, mc * 0.5 + 0.5, 0.0).r * atm.cloudShell.w);
		if (cov <= 0.01) continue;
		float shape = textureLod (atmCloudShape, (x + atm.cloudNoise.xyz) * atm.cloudNoise.w, 0.0).r;
		od += clamp ((shape * profile - (1.0 - cov)) / cov, 0.0, 1.0) * dt;
	}
	float vol = 1.0 - exp (-atm.cloudShell.z * od);
	// (the 2D layer towards the map's edge)
	return 1.0 - atm.cloud.z * mix (vol, a, smoothstep (0.8, 1.0, edge));
}
