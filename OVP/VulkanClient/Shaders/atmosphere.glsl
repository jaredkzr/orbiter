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

#include "atmlut.glsl"


const float ATM_PI = 3.14159265;
const float ATM_NONE = 1e30;

// Intersections of the ray o + t d (|d| = 1) with a sphere around the origin,
// given c = |o|^2 - r^2 (computed accurately by the caller)
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
		// all higher orders, isotropic
		vec3 psi = textureLod (atmMultiscat, AtmMultiscatUv (clamp (r, Rg, Rt), muS, Rg, Rt), 0.0).rgb;
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

	float mu = dot (d, atm.sunDir.xyz);
	float g = atm.radii.z;
	float pR = 3.0 / (16.0 * ATM_PI) * (1.0 + mu * mu);
	float pM = 3.0 / (8.0 * ATM_PI) * ((1.0 - g * g) * (1.0 + mu * mu)) /
		((2.0 + g * g) * pow (max (1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
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
	return 1.0 - atm.cloud.z * a;
}
