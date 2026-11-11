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
// brightness about as steep as a real one's: 1 at 2100 K (yellow-orange;
// a saturated orange displays only up to about 0.4 - brighter, the tone
// curve whitens it to a pale peach, as nothing in photographs of
// fireballs is but their cores), 0.3 at 1850 K (what is seen of a
// billow's middle: orange), 0.1 at 1700 K (the cooler mixing layer at its
// rim: a deep orange), 2 at 2300 K and 4 at 2500 K (the cores, where the
// mixing layer is thin, blown out to white-yellow, blooming); cooled
// soot black. Its colour from Planck's law at three wavelengths (red,
// green, blue) at a lower colour temperature (0.82 T: photographs' fire
// orange is about (1, 0.3, 0.02) linear - at its own temperature it
// displayed a pale tan), rising to 1.3 T for the hottest (2500 K: blown
// out, the tone curve's whitening of an orange gave salmon; of this
// yellow, a cream white)
vec3 FbPlanck (float T)
{
	const vec3 lam = vec3 (0.61, 0.55, 0.465); // [um]
	T = max (T, 500.0);
	float Tc = T * mix (0.82, 1.3, smoothstep (1900.0, 2500.0, T));
	vec3 b = 1.0 / (lam * lam * lam * lam * lam * (exp (14388.0 / (lam * Tc)) - 1.0));
	float y = dot (b, vec3 (0.2126, 0.7152, 0.0722));
	return b / max (y, 1e-30) * exp (-20000.0 * (1.0 / T - 1.0 / 2100.0));
}
