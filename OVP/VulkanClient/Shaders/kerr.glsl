// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The Kerr spacetime in Kerr-Schild coordinates for light rays (the GPU's
// copy of Src/Orbiter/Kerr.h's geodesics): g = eta + f l l, f = 2 M r^3 /
// (r^4 + a^2 z^2), l = (1, (r x + a y) / (r^2 + a^2), (r y - a x) / (r^2 +
// a^2), z / r); units of the hole's mass (M = 1), the spin a along +z.
// Hamilton's equations in the covariant momentum p (p_t conserved).

#ifndef KERR_GLSL
#define KERR_GLSL

// the spheroidal radius: r^4 - (R^2 - a^2) r^2 - a^2 z^2 = 0
float KerrR (vec3 x, float a)
{
	float a2 = a * a, z2 = x.z * x.z;
	float w = dot (x, x) - a2;
	float D = sqrt (w * w + 4.0 * a2 * z2);
	float r2 = w >= 0.0 ? 0.5 * (w + D) : (D - w > 0.0 ? 2.0 * a2 * z2 / (D - w) : 0.0);
	return sqrt (max (r2, 0.0));
}

// dx^m/dlambda (dx: xyz, dt: t) and dp_i/dlambda (dp) at x for momentum p
// (p.w = p_t)
void KerrDerivs (vec3 x, vec4 p, float a, out vec3 dx, out float dt, out vec3 dp)
{
	float a2 = a * a, z = x.z, z2 = z * z;
	float w = dot (x, x) - a2;
	float D = sqrt (w * w + 4.0 * a2 * z2);
	float r2 = w >= 0.0 ? 0.5 * (w + D) : (D - w > 0.0 ? 2.0 * a2 * z2 / (D - w) : 0.0);
	float r = sqrt (max (r2, 1e-12));
	D = max (D, 1e-12);
	vec3 drv = vec3 (r * x.x / D, r * x.y / D, (r2 + a2) * z / (r * D));
	float ra = r2 + a2, q = r2 * r2 + a2 * z2;
	float f = 2.0 * r2 * r / q;
	vec3 l = vec3 ((r * x.x + a * x.y) / ra, (r * x.y - a * x.x) / ra, z / r);
	float L = -p.w + dot (l, p.xyz);
	dt = -p.w + f * L;
	dx = p.xyz - f * L * l;
	float dfdr = 2.0 * r2 * (3.0 * a2 * z2 - r2 * r2) / (q * q);
	float dfdz = -4.0 * r2 * r * a2 * z / (q * q);
	float ra2 = ra * ra;
	for (int i = 0; i < 3; i++) {
		float d = drv[i];
		float dl1 = ((d * x.x + (i == 0 ? r : 0.0) + (i == 1 ? a : 0.0)) * ra - (r * x.x + a * x.y) * 2.0 * r * d) / ra2;
		float dl2 = ((d * x.y + (i == 1 ? r : 0.0) - (i == 0 ? a : 0.0)) * ra - (r * x.y - a * x.x) * 2.0 * r * d) / ra2;
		float dl3 = (i == 2 ? 1.0 / r : 0.0) - z * d / r2;
		float df = dfdr * d + (i == 2 ? dfdz : 0.0);
		dp[i] = 0.5 * df * L * L + f * L * (dl1 * p.x + dl2 * p.y + dl3 * p.z);
	}
}

// the spatial momentum of light at x moving along d (unit) with p_t = pt:
// the norm's root, p = s d (-pt^2 + s^2 - f (-pt + s l.d)^2 = 0)
vec3 KerrLightMomentum (vec3 x, vec3 d, float pt, float a)
{
	float r = KerrR (x, a), r2 = r * r, a2 = a * a, ra = r2 + a2;
	float f = 2.0 * r2 * r / (r2 * r2 + a2 * x.z * x.z);
	vec3 l = vec3 ((r * x.x + a * x.y) / ra, (r * x.y - a * x.x) / ra, x.z / r);
	float ld = dot (l, d);
	float A = 1.0 - f * ld * ld, B = 2.0 * f * pt * ld, C = -pt * pt * (1.0 + f);
	float s = (-B + sqrt (max (B * B - 4.0 * A * C, 0.0))) / (2.0 * A);
	return d * s;
}

#endif
