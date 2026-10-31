// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The zodiacal cloud (as Zodiacal.cpp's copy on the CPU): the smooth
// cloud of Kelsall et al. (1998, ApJ 508, 44; COBE/DIRBE), a fan about a
// plane tilted to the ecliptic, its centre off the Sun; lit by the Sun
// and scattering by the phase function of Hong (1985, A&A 146, 67).
// Positions: heliocentric ecliptic (right-handed) [AU].

const float ZOD_ALPHA = 1.34, ZOD_BETA = 4.14, ZOD_GAMMA = 0.942, ZOD_MU = 0.189;
const float ZOD_INCL = 0.035430, ZOD_NODE = 1.356170; // 2.03 deg, 77.7 deg
const vec3 ZOD_OFFSET = vec3 (0.0119, 0.00548, -0.00215);

// dust density (arbitrary units)
float ZodDensity (vec3 x)
{
	vec3 q = x - ZOD_OFFSET;
	float R = max (length (q), 1e-3);
	float z = q.x * sin (ZOD_NODE) * sin (ZOD_INCL) - q.y * cos (ZOD_NODE) * sin (ZOD_INCL) + q.z * cos (ZOD_INCL);
	float zeta = abs (z) / R;
	float g = zeta < ZOD_MU ? zeta * zeta / (2.0 * ZOD_MU) : zeta - 0.5 * ZOD_MU;
	return pow (R, -ZOD_ALPHA) * exp (-ZOD_BETA * pow (g, ZOD_GAMMA));
}

// scattering phase function (three Henyey-Greenstein terms; c: cosine of
// the scattering angle)
float ZodPhase (float c)
{
	const vec3 g = vec3 (0.7, -0.2, -0.81), w = vec3 (0.665, 0.330, 0.005);
	vec3 d = 1.0 + g * g - 2.0 * g * c;
	return dot (w, (1.0 - g * g) / (d * sqrt (d))) * 0.0795775; // (1 / 4 pi)
}

// sunlight scattered toward an observer at o along d [model units]: samples
// spaced evenly in the angle seen from the Sun (the light falls off as
// 1 / r^2 from it, and the dust's density as r^-1.34)
float ZodLight (vec3 o, vec3 d, int n)
{
	const float SMAX = 8.0; // [AU] beyond the observer's distance from the Sun
	float tc = -dot (o, d);
	float b = max (length (o + d * tc), 0.004);
	float s1 = length (o) + SMAX;
	float a0 = atan ((0.0 - tc) / b), a1 = atan ((s1 - tc) / b), da = (a1 - a0) / float (n);
	float I = 0.0;
	for (int i = 0; i < n; i++) {
		float s = tc + b * tan (a0 + (float (i) + 0.5) * da);
		float ds = da * (b * b + (s - tc) * (s - tc)) / b;
		vec3 x = o + d * s;
		float r2 = max (dot (x, x), 1e-6);
		I += ZodDensity (x) * ZodPhase (dot (x, -d) * inversesqrt (r2)) / r2 * ds;
	}
	return I;
}
