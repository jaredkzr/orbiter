// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Stellar systems beyond the Milky Way's model (SkyObjects.cpp): other
// galaxies (components: exponential disks, with spiral arms; exponential
// spheroids; Gaussian bars and star-forming knots; a dust disk or ring)
// and globular clusters (Plummer spheres of the Milky Way's old
// population: their stars brighter than the limit, drawn as points, are
// left out, as the Milky Way's). Seen behind the Milky Way's dust; a
// galaxy's light through its own. (Needs galaxy.glsl.)

struct SkyObject {
	vec4 pos;     // xyz: centre, heliocentric Galactic [pc]; w: bounding radius [pc]
	vec4 ax0;     // xyz: the major axis (x of the object's frame); w: kind (0: galaxy, 1: globular cluster)
	vec4 ax1;     // xyz: the disk's second axis (y); w: number of components
	vec4 ax2;     // xyz: the disk's normal (z); w: the slab about the disk's plane holding the light [pc]
	vec4 dust;    // scale length, scale height [pc], A_V per pc at the centre (or the ring), ring radius [pc]
	vec4 dust2;   // ring sigma [pc], arms: number, tan pitch, contrast
	vec4 comp[12];// 4 components: geometry 0 (type, scales), geometry 1 (offset, angle), light per band at the
	              // centre [L_sun,band / pc^3] (globular cluster: type 5, Plummer radius, tidal radius, volume of
	              // the Sun's old population holding its stars [pc^3])
};

layout(std430, set = 0, binding = 6) readonly buffer SkyObjects { uvec4 skyHead; SkyObject skyObj[]; };

// spiral arms: 1 + contrast ((1 + cos (n (phi - ln R / tan pitch))) / 2)^4
float SkyArms (vec2 p, vec4 d2)
{
	float R = max (length (p), 1.0);
	float c = 0.5 + 0.5 * cos (d2.y * (atan (p.y, p.x) - log (R) / d2.z));
	c *= c;
	return 1.0 + d2.w * c * c;
}

// (a scale widened to at least w, and the factor keeping the light of a
// component of that many dimensions)
float SkyWiden (inout float s, float w, float dims)
{
	float e = max (s, w), f = pow (s / e, dims);
	s = e;
	return f;
}

// A galaxy's light per band [L_sun,band / pc^3] and dust [A_V / pc] at q
// (in its frame); w: the smallest scale resolved [pc]
void SkyGalaxyAt (SkyObject o, vec3 q, float w, out vec4 j, out float rho)
{
	j = vec4 (0.0);
	int n = int (o.ax1.w);
	for (int k = 0; k < n; k++) {
		vec4 g0 = o.comp[k * 3], g1 = o.comp[k * 3 + 1], lt = o.comp[k * 3 + 2];
		int t = int (g0.x + 0.5);
		float f = 0.0;
		if (t == 1) { // disk
			float hz = g0.z, k1 = SkyWiden (hz, w, 1.0);
			f = k1 * exp (-length (q.xy) / g0.y - abs (q.z) / hz);
			if (g0.w > 0.0) f *= SkyArms (q.xy, o.dust2);
		}
		else if (t == 2) { // spheroid
			float a = g0.y, k1 = SkyWiden (a, w, 3.0);
			f = k1 * exp (-length (vec3 (q.xy, q.z / g0.z)) / a);
		}
		else if (t == 3) { // bar
			vec3 p = q - g1.xyz;
			float c = cos (g1.w), s = sin (g1.w);
			vec3 r = vec3 (c * p.x + s * p.y, -s * p.x + c * p.y, p.z);
			vec3 sg = g0.yzw;
			float k1 = SkyWiden (sg.x, w, 1.0) * SkyWiden (sg.y, w, 1.0) * SkyWiden (sg.z, w, 1.0);
			r /= sg;
			f = k1 * exp (-0.5 * dot (r, r));
		}
		else if (t == 4) { // knot
			vec3 p = q - g1.xyz;
			float sg = g0.y, k1 = SkyWiden (sg, w, 3.0);
			f = k1 * exp (-0.5 * dot (p, p) / (sg * sg));
		}
		j += f * lt;
	}
	rho = 0.0;
	if (o.dust.z > 0.0) {
		float R = length (q.xy);
		float rad = o.dust.w > 0.0 ? exp (-0.5 * (R - o.dust.w) * (R - o.dust.w) / (o.dust2.x * o.dust2.x)) : exp (-R / o.dust.x);
		float hz = o.dust.y, k1 = SkyWiden (hz, w, 1.0);
		rho = o.dust.z * k1 * rad * exp (-abs (q.z) / hz);
		if (o.dust2.y > 0.0) rho *= SkyArms (q.xy, o.dust2);
	}
}

// Light [L_sun,band / pc^2 along the ray] of object i from the ray o + s d
// within [s0, s1], behind A0 (A_V) of the Milky Way's dust; pix: a texel's
// angle [rad]; jit: offset of the samples (0..1); mLim: the limiting
// magnitude of the stars drawn as points
vec4 SkyObjectLight (int i, vec3 o, vec3 d, float s0, float s1, float A0, float pix, float jit, float mLim)
{
	SkyObject ob = skyObj[i];
	vec3 c = ob.pos.xyz;
	vec4 ext = 0.921 * gal.extinction; // (ln 10 / 2.5)
	vec4 L = vec4 (0.0);
	float tc = dot (c - o, d);
	if (ob.ax0.w > 0.5) {
		// globular cluster: samples spaced evenly in the angle seen from
		// its centre; its stars brighter than the limit from here left out
		float b = max (length (o + d * tc - c), 0.01);
		float a = ob.comp[0].y;
		SkyWiden (a, 0.5 * abs (tc) * pix, 0.0);
		float mCut = mLim - 5.0 * log (max (tc, 1.0) / 10.0) / log (10.0) - A0;
		vec4 T = exp (-A0 * ext);
		const int N = 16;
		float a0 = atan ((s0 - tc) / b), a1 = atan ((s1 - tc) / b), da = (a1 - a0) / float (N);
		for (int k = 0; k < N; k++) {
			float s = tc + b * tan (a0 + (float (k) + jit) * da);
			float ds = da * (b * b + (s - tc) * (s - tc)) / b;
			vec3 v = o + d * s - c;
			float x = 1.0 + dot (v, v) / (a * a);
			float n = ob.comp[0].w * 0.2387324 / (a * a * a) / (x * x * sqrt (x)); // (3 / 4 pi) a^-3 (1 + r^2/a^2)^-5/2
			L += GalLight (2, n, mCut) * T * ds;
		}
		return L;
	}
	// galaxy: through the slab about its disk's plane, front to back
	float zc = dot (o - c, ob.ax2.xyz), dz = dot (d, ob.ax2.xyz), Z = ob.ax2.w;
	if (abs (dz) > 1e-6) {
		float t0 = (-Z - zc) / dz, t1 = (Z - zc) / dz;
		s0 = max (s0, min (t0, t1)), s1 = min (s1, max (t0, t1));
	}
	else if (abs (zc) > Z) return L;
	if (s1 <= s0) return L;
	const int N = 96;
	float ds = (s1 - s0) / float (N);
	float w = max (0.5 * abs (tc) * pix, 0.5 * ds * abs (dz));
	float A = A0;
	for (int k = 0; k < N; k++) {
		float s = s0 + (float (k) + jit) * ds;
		vec3 p = o + d * s - c;
		vec3 q = vec3 (dot (p, ob.ax0.xyz), dot (p, ob.ax1.xyz), dot (p, ob.ax2.xyz));
		vec4 j;
		float rho;
		SkyGalaxyAt (ob, q, w, j, rho);
		A += 0.5 * rho * ds;
		L += j * exp (-A * ext) * ds;
		A += 0.5 * rho * ds;
	}
	return L;
}
