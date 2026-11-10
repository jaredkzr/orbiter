// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sea's swell near the camera (Water W8): the core's components about
// the point under the camera (Scene::FillSwell; Src/Orbiter/SeaSwell.h)
// raise the sea's surface, so that a vessel afloat, which the physics
// holds on the same swell, rides the waves drawn. Only vertices on the
// water's mean surface (within SWELL_TOL of it) are moved, within the range,
// fading out towards it; components shorter than a few times the mesh's
// spacing at the distance are left out (they would alias), as the same for
// the prepass and the colour pass. Requires frame.glsl.

const float SWELL_TOL = 0.3;   // [m]
const float SWELL_SKIRT = 50.0; // [m] (the skirts' depth: 2% of a tile)

// p: the vertex (view space); n: its normal, tilted onto the swell's slope
vec3 SwellDisplace (vec3 p, inout vec3 n)
{
	float Rs = frame.swellCentre.w;
	if (Rs <= 0.0) return p;
	vec3 up = normalize (frame.swellRef.xyz - frame.swellCentre.xyz);
	vec3 d = p - frame.swellRef.xyz;
	float x = dot (d, frame.swellEast.xyz), z = dot (d, frame.swellNorth.xyz);
	float r2 = x * x + z * z;
	float range = frame.swellRef.w;
	if (r2 > range * range) return p;
	// (height above the water's mean surface: the tangent plane, and the sphere's drop from it)
	float hgt = dot (d, up) + 0.5 * r2 / Rs;
	// (the tiles' skirts hang below their edges: moved with the surface and
	// shaded as it, they fill the cracks where a coarser neighbour's edge
	// can't follow the waves)
	if (hgt > SWELL_TOL || hgt < -SWELL_SKIRT) return p;
	bool skirt = hgt < -SWELL_TOL;
	float dist = length (p);
	float fade = 1.0 - smoothstep (0.6 * range, range, sqrt (r2));
	// (shortest wave drawn: about 4 mesh spacings at the distance)
	float lcut = max (2.0, 0.08 * dist);
	int nc = int (frame.swellEast.w);
	float h = 0.0, sx = 0.0, sz = 0.0;
	for (int i = 0; i < nc; i++) {
		vec4 c = frame.swellComp[i];
		float lam = 6.2831853 / c.y;
		float w = smoothstep (lcut, 2.0 * lcut, lam);
		if (w <= 0.0) continue;
		float ph = c.y * (c.z * x + c.w * z) + frame.swellPhase[i / 4][i % 4];
		float a = c.x * w;
		h += a * cos (ph);
		float s = a * c.y * sin (ph);
		sx -= s * c.z, sz -= s * c.w;
	}
	if (skirt) n = up;
	n = normalize (n - (frame.swellEast.xyz * sx + frame.swellNorth.xyz * sz) * fade);
	return p + up * (h * fade);
}

// A point on the sea the swell moved (swell.glsl): true, and o lifted to
// just above the water's mean surface (where the terrain's rays start)
bool SwellBand (vec3 o, out vec3 oT)
{
	oT = o;
	float Rs = frame.swellCentre.w;
	if (Rs <= 0.0) return false;
	vec3 up = normalize (frame.swellRef.xyz - frame.swellCentre.xyz);
	vec3 d = o - frame.swellRef.xyz;
	float x = dot (d, frame.swellEast.xyz), z = dot (d, frame.swellNorth.xyz);
	float r2 = x * x + z * z;
	if (r2 > frame.swellRef.w * frame.swellRef.w) return false;
	float hgt = dot (d, up) + 0.5 * r2 / Rs;
	if (hgt > 4.0 || hgt < -4.0) return false;
	oT = o + up * (max (-hgt, 0.0) + 0.2);
	return true;
}

