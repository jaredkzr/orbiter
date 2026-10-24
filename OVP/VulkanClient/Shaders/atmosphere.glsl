// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Single-scattering atmosphere (Rayleigh and Mie), ray-marched.
// All positions are in the planet frame, in metres. The including shader
// defines ATM_SET, the descriptor set of the atmosphere uniform block.

layout(set = ATM_SET, binding = 0) uniform Atmosphere {
	mat4 viewToPlanet;  // view space -> planet frame (rotation)
	vec4 camPos;        // camera position, planet frame [m]
	vec4 sunDir;        // unit vector towards the sun, planet frame
	vec4 betaR;         // Rayleigh scattering coefficients [1/m] (rgb), scale height [m] (w)
	vec4 betaM;         // Mie scattering coefficients [1/m] (rgb), scale height [m] (w)
	vec4 radii;         // planet radius, atmosphere radius, Mie asymmetry g, sun intensity
	vec4 camC;          // |camPos|^2 - r^2 for the planet (x) and atmosphere (y) radius
	vec4 misc;          // x: atmosphere present, y: 1/fx, z: 1/fy
} atm;

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

// Optical depth (Rayleigh, Mie) from p towards the sun; x >= ATM_NONE if
// the planet blocks the sun
vec2 SunOpticalDepth (vec3 p)
{
	vec3 s = atm.sunDir.xyz;
	vec2 tp = RaySphere (p, s, atm.radii.x);
	if (tp.x > 0.0 && tp.x <= tp.y) return vec2 (ATM_NONE);
	float t1 = RaySphere (p, s, atm.radii.y).y;
	if (t1 <= 0.0) return vec2 (0.0);
	const int N = 4;
	float ds = t1 / float (N);
	vec2 od = vec2 (0.0);
	for (int i = 0; i < N; i++) {
		float h = length (p + s * (ds * (float (i) + 0.5))) - atm.radii.x;
		od += exp (-max (h, 0.0) / vec2 (atm.betaR.w, atm.betaM.w)) * ds;
	}
	return od;
}

// Transmittance of sunlight arriving at p
vec3 SunTransmittance (vec3 p)
{
	vec2 od = SunOpticalDepth (p);
	if (od.x >= ATM_NONE) return vec3 (0.0);
	return exp (-(atm.betaR.rgb * od.x + atm.betaM.rgb * 1.1 * od.y));
}

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
	vec2 od = vec2 (0.0);
	vec3 sumR = vec3 (0.0), sumM = vec3 (0.0), sumMS = vec3 (0.0);
	for (int i = 0; i < N; i++) {
		vec3 p = o + d * (t0 + ds * (float (i) + 0.5));
		float h = length (p) - atm.radii.x;
		vec2 dens = exp (-max (h, 0.0) / vec2 (atm.betaR.w, atm.betaM.w)) * ds;
		od += dens;
		vec2 ls = SunOpticalDepth (p);
		if (ls.x >= ATM_NONE) continue; // in the planet's shadow
		vec3 att = exp (-(atm.betaR.rgb * (od.x + ls.x) + atm.betaM.rgb * 1.1 * (od.y + ls.y)));
		sumR += att * dens.x;
		sumM += att * dens.y;
		// Multiple scattering, approximated as isotropic light whose extinction
		// along the view path is balanced by scattering into it from other
		// directions. This keeps long horizontal paths from turning orange.
		vec3 sunAtt = exp (-(atm.betaR.rgb * ls.x + atm.betaM.rgb * 1.1 * ls.y));
		sumMS += sunAtt * (atm.betaR.rgb * dens.x + atm.betaM.rgb * dens.y) * exp (-atm.betaM.rgb * 1.1 * od.y);
	}
	T = exp (-(atm.betaR.rgb * od.x + atm.betaM.rgb * 1.1 * od.y));

	float mu = dot (d, atm.sunDir.xyz);
	float g = atm.radii.z;
	float pR = 3.0 / (16.0 * ATM_PI) * (1.0 + mu * mu);
	float pM = 3.0 / (8.0 * ATM_PI) * ((1.0 - g * g) * (1.0 + mu * mu)) /
		((2.0 + g * g) * pow (max (1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
	const float MS_FACTOR = 0.25; // fraction of single-scattered light scattered again, per 4 pi
	return atm.radii.w * (sumR * atm.betaR.rgb * pR + sumM * atm.betaM.rgb * pM + sumMS * MS_FACTOR / (4.0 * ATM_PI));
}

// Map scattered radiance into displayable range
vec3 AtmTonemap (vec3 L)
{
	return 1.0 - exp (-L);
}
