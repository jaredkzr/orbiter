// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Crash fireballs as volumes (Fireball.cpp): the fireballs of this frame
// and their grids. Ball space: the fireball's centre, x along its path
// (horizontal), y across it, z up [m]. Each fireball has a slab of the
// grid images (FB_GZ cells high each, stacked in z).
// The including shader defines FB_SET and FB_BINDING (the buffer).

struct FireballParams { // must match FireballRenderer::Params
	mat4 toBall;   // view -> ball space [m]
	vec4 centre;   // its centre (view space), w: the temperature of its surface (the mixing layer) [K]
	vec4 boxMin;   // the grid's box (ball space) [m], w: its slab
	vec4 boxMax;   //   w: the march's step [m]
	vec4 shape;    // r [m], R grown [m], drawn out along its path (>= 1), lift-off (0..1)
	vec4 axis;     // the direction it was thrown along (ball space), w: the ground below (ball z [m]; -1e9: none)
	vec4 time;     // age [s], u = age / t_f, t_f [s], seed
	vec4 heat;     // the hottest gas (its core) [K], how far it burned out (base of the progress), the flash (0..1), its extinction (fading out: 0..1)
	vec4 flow;     // the vortex ring's turn [rad], its radius (/ r), churn (billows boiling), extinction scale
	vec4 tune;     // (ORBITER_VK_FIREBALL_TUNE: debugging factors, 1 = as tuned)
	vec4 mode;     // in vacuum: a thin shell (1), its radius [m], its width [m], its flash (0..1)
	vec4 asym;     // its lean (ball space xy, per unit height), the finest octave's weight, its blend weight
	vec4 adv;      // the noise carried outward, two phases cross-faded (phase 4c): A's scale and weight, B's scale and weight
	vec4 adv2;     // the ring's turn since each phase began [rad] (A, B); each phase's number (its seed) (A, B)
};

const uint FB_MAX = 4u;                          // fireballs per frame slot (FireballRenderer::MAX_MARCHED)
const int FB_GX = 96, FB_GY = 96, FB_GZ = 112;    // FireballRenderer::GX, GY, GZ

layout(set = FB_SET, binding = FB_BINDING, std430) readonly buffer FireballBuffer {
	FireballParams fireballs[];   // frame slot * FB_MAX + index
};

// extinction of the burning gas and of soot, times the radius (optical
// depth over a radius of each)
const float FB_SIGMA_FLAME = 14.0, FB_SIGMA_SOOT = 16.0;

// texture coordinates of the grids for a point of the box (0..1) in slab s
// (clamped within the slab: no bleeding from the next)
vec3 FbGridCoord (vec3 uvw, float s)
{
	float z = clamp (uvw.z * float (FB_GZ), 0.5, float (FB_GZ) - 0.5);
	return vec3 (uvw.xy, (s * float (FB_GZ) + z) / float (FB_GZ * int (FB_MAX)));
}
// the same for the grid of optical depths towards the sun and the sky (B:
// half the resolution - its shadows are soft)
const int FB_BX = FB_GX / 2, FB_BY = FB_GY / 2, FB_BZ = FB_GZ / 2;
vec3 FbGridCoordB (vec3 uvw, float s)
{
	float z = clamp (uvw.z * float (FB_BZ), 0.5, float (FB_BZ) - 0.5);
	return vec3 (uvw.xy, (s * float (FB_BZ) + z) / float (FB_BZ * int (FB_MAX)));
}

// radiance of a blackbody at temperature T [K], scene-linear (1 ~ a sunlit
// white surface), as a camera balanced for daylight sees it: its
// brightness about as steep as a real one's up to 2100 K: 1 there
// (yellow-orange), 0.3 at 1850 K (a dark orange), 0.1 at 1700 K; above it
// held to about 1.6 (the cores, where the mixing layer is thin, and the
// flash: a yellow-white under the march's knee, not a pale cream over the
// whole ball - fireball.frag); cooled soot black. Its colour from
// Planck's law at three wavelengths (red, green, blue) at a lower colour
// temperature (0.82 T: photographs' fire orange is about (1, 0.35, 0.01)
// linear - at its own temperature it displayed a pale tan), rising to
// 1.1 T for the hottest (the flash's white-yellow)
vec3 FbPlanck (float T)
{
	const vec3 lam = vec3 (0.61, 0.55, 0.465); // [um]
	T = max (T, 500.0);
	float Tc = T * mix (0.82, 1.1, smoothstep (2200.0, 2500.0, T));
	vec3 b = 1.0 / (lam * lam * lam * lam * lam * (exp (14388.0 / (lam * Tc)) - 1.0));
	float y = dot (b, vec3 (0.2126, 0.7152, 0.0722));
	float L = exp (-20000.0 * (1.0 / T - 1.0 / 2100.0));
	if (L > 1.0) L = 1.0 + 0.6 * (1.0 - exp (-(L - 1.0) / 0.6));
	return b / max (y, 1e-30) * L;
}
