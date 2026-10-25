// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The procedural terrain detail's building blocks, shared by the tile
// shading (terraindetail.glsl) and the tile meshes (tile_mesh.comp):
// hashes, gradient noise, the octave lattices, the craters' shape. They
// match the core's TerrainDetail.cpp (keep them identical).

#ifndef DETAILCORE_GLSL
#define DETAILCORE_GLSL

uint DetPcg (uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint DetHash (ivec3 c, uint seed)
{
	return DetPcg (uint (c.x) + DetPcg (uint (c.y) + DetPcg (uint (c.z) + DetPcg (seed))));
}

vec3 DetGrad (uint h)
{
	switch (h % 12u) {
	case 0u:  return vec3 ( 1.0,  1.0,  0.0);
	case 1u:  return vec3 (-1.0,  1.0,  0.0);
	case 2u:  return vec3 ( 1.0, -1.0,  0.0);
	case 3u:  return vec3 (-1.0, -1.0,  0.0);
	case 4u:  return vec3 ( 1.0,  0.0,  1.0);
	case 5u:  return vec3 (-1.0,  0.0,  1.0);
	case 6u:  return vec3 ( 1.0,  0.0, -1.0);
	case 7u:  return vec3 (-1.0,  0.0, -1.0);
	case 8u:  return vec3 ( 0.0,  1.0,  1.0);
	case 9u:  return vec3 ( 0.0, -1.0,  1.0);
	case 10u: return vec3 ( 0.0,  1.0, -1.0);
	default:  return vec3 ( 0.0, -1.0, -1.0);
	}
}

// gradient noise at lattice cell i, position f in the cell: value (w) and
// derivatives (xyz)
vec4 DetNoise (ivec3 i, vec3 f, uint seed)
{
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
	vec3 ga = DetGrad (DetHash (i + ivec3 (0, 0, 0), seed)), gb = DetGrad (DetHash (i + ivec3 (1, 0, 0), seed));
	vec3 gc = DetGrad (DetHash (i + ivec3 (0, 1, 0), seed)), gd = DetGrad (DetHash (i + ivec3 (1, 1, 0), seed));
	vec3 ge = DetGrad (DetHash (i + ivec3 (0, 0, 1), seed)), gf = DetGrad (DetHash (i + ivec3 (1, 0, 1), seed));
	vec3 gg = DetGrad (DetHash (i + ivec3 (0, 1, 1), seed)), gh = DetGrad (DetHash (i + ivec3 (1, 1, 1), seed));
	float va = dot (ga, f), vb = dot (gb, f - vec3 (1, 0, 0));
	float vc = dot (gc, f - vec3 (0, 1, 0)), vd = dot (gd, f - vec3 (1, 1, 0));
	float ve = dot (ge, f - vec3 (0, 0, 1)), vf = dot (gf, f - vec3 (1, 0, 1));
	float vg = dot (gg, f - vec3 (0, 1, 1)), vh = dot (gh, f - vec3 (1, 1, 1));
	float k1 = vb - va, k2 = vc - va, k3 = ve - va;
	float k4 = va - vb - vc + vd, k5 = va - vc - ve + vg, k6 = va - vb - ve + vf;
	float k7 = -va + vb + vc - vd + ve - vf - vg + vh;
	float v = va + u.x * k1 + u.y * k2 + u.z * k3 + u.x * u.y * k4 + u.y * u.z * k5 + u.z * u.x * k6 + u.x * u.y * u.z * k7;
	vec3 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga)
		+ u.x * u.y * (ga - gb - gc + gd) + u.y * u.z * (ga - gc - ge + gg) + u.z * u.x * (ga - gb - ge + gf)
		+ u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh)
		+ du * (vec3 (k1, k2, k3) + u.yzx * vec3 (k4, k5, k6) + u.zxy * vec3 (k6, k4, k5) + u.yzx * u.zxy * k7);
	return vec4 (d, v);
}

const int DET_JMIN = -4;

float DetUnit16 (uint h) { return float (h & 0xffffu) / 65535.0; }
// a unit vector from a hash (as TerrainDetail.cpp's UnitFromHash)
vec3 DetUnitFromHash (uint h)
{
	uint h2 = DetPcg (h), h3 = DetPcg (h2);
	vec3 v = vec3 (DetUnit16 (h), DetUnit16 (h2), DetUnit16 (h3)) * 2.0 - 1.0;
	float l = length (v);
	return l > 1e-6 ? v / l : vec3 (1.0, 0.0, 0.0);
}

// position in lattice m at scale R/size (as TerrainDetail.cpp's LatticePoint):
// cell (out), position in it (returned); p, sg: the axis mapping
vec3 DetLattice (int m, dvec3 d, double scale, out ivec3 cell, out ivec3 p, out vec3 sg)
{
	const ivec3 P[6] = ivec3[] (ivec3 (0,1,2), ivec3 (1,2,0), ivec3 (2,0,1), ivec3 (0,2,1), ivec3 (1,0,2), ivec3 (2,1,0));
	const vec3 S[4] = vec3[] (vec3 (1,1,1), vec3 (1,-1,-1), vec3 (-1,1,-1), vec3 (-1,-1,1));
	int k = m % 24;
	p = P[k % 6];
	sg = S[k / 6] * ((k % 6) < 3 ? 1.0 : -1.0);
	dvec3 q = dvec3 (sg.x * d[p.x], sg.y * d[p.y], sg.z * d[p.z]) * scale
		+ dvec3 (double ((m * 37) & 63), double ((m * 23) & 63), double ((m * 53) & 63)) / 64.0;
	dvec3 fl = floor (q);
	cell = ivec3 (fl);
	return vec3 (q - fl);
}

// a lattice gradient back to the planet frame (d moves by 1/R per metre
// along the surface): dq_i/dd[p_i] = sg_i * scale
vec3 DetToPlanet (vec3 g, ivec3 p, vec3 sg)
{
	vec3 r = vec3 (0.0);
	r[p.x] += g.x * sg.x, r[p.y] += g.y * sg.y, r[p.z] += g.z * sg.z;
	return r;
}

// a planet frame direction in the lattice (as DetLattice maps positions)
vec3 DetToLattice (vec3 v, ivec3 p, vec3 sg)
{
	return vec3 (sg.x * v[p.x], sg.y * v[p.y], sg.z * v[p.z]);
}


// (as TerrainDetail.cpp: octaves 2^j m, crater cells 2^k m)
const int DET_JMAX = 20, DET_KMIN = -2, DET_KMAX = 16;
const float DET_CRATER_D = 0.22, DET_CRATER_G = 0.1, DET_PEBBLE_G = 0.15;

float DetSmoothStep (float a, float b, float x)
{
	float t = clamp ((x - a) / (b - a), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// A pebble cell (as TerrainDetail.cpp's PebbleLayer): the stone of the
// cell (m: its lattice, k: its size 2^k m) at position f in the cell, if
// it has one (stone: false); crag > 0, shading only: its faces roughened
const float DET_PEBBLE_SOFT = 0.08;
float DetPebbleCell (ivec3 cell, int m, int k, float density, vec3 f, float crag, out bool stone)
{
	stone = false;
	float s = exp2 (float (k));
	uint h0 = DetHash (cell, 3000u + uint (m));
	if (DetUnit16 (h0) >= density * (k == -1 ? 0.4 : k == -2 ? 0.7 : 1.0)) return 0.0;
	stone = true;
	uint h1 = DetPcg (h0), h2 = DetPcg (h1), h3 = DetPcg (h2), h4 = DetPcg (h3);
	float r = 0.12 + 0.12 * DetUnit16 (h4), hgt = 0.25 + 0.3 * DetUnit16 (h4 >> 16);
	float ext = 1.05 * r;
	vec3 c = ext + (1.0 - 2.0 * ext) * vec3 (DetUnit16 (h1), DetUnit16 (h2), DetUnit16 (h3));
	vec3 v = f - c;
	if (length (v) >= ext) return 0.0;
	float x = 0.0;
	uint he = h4;
	for (int e = 0; e < 5; e++) {
		he = DetPcg (he ^ 0x51ed27u);
		x += exp (dot (v, DetUnitFromHash (he)) / (r * (0.75 + 0.25 * DetUnit16 (he >> 16))) / DET_PEBBLE_SOFT);
	}
	x = DET_PEBBLE_SOFT * log (x);
	if (x >= 1.0) return 0.0;
	float top = DetSmoothStep (0.0, 0.6, 1.0 - x);
	float tilt = 1.0 + 0.5 * dot (v, DetUnitFromHash (DetPcg (h3 ^ 0x2545f491u))) / r;
	float h = hgt * r * s * top * max (tilt, 0.3);
	if (crag > 0.0) {
		// (facets and pits: noise at a third and a sixth of the stone, in
		// the cell's own coordinates)
		vec3 q = v / (0.33 * r);
		float n = DetNoise (ivec3 (floor (q)), fract (q), h4).w;
		q *= 2.0;
		n += 0.5 * DetNoise (ivec3 (floor (q)), fract (q), h4 + 1u).w;
		h += crag * 0.05 * r * s * n * min (1.0, 5.0 * (1.0 - x));
	}
	return h;
}

// One layer of pebbles and clods [m] (as TerrainDetail.cpp's PebbleLayer)
float DetPebbleLayer (dvec3 d, int k, int layer, double R, float density, float crag)
{
	int m = k - DET_JMIN + 64 + 29 * layer;
	ivec3 cell, p; vec3 sg;
	vec3 f = DetLattice (m, d, R / double (exp2 (float (k))), cell, p, sg);
	bool stone;
	return DetPebbleCell (cell, m, k, density, f, crag, stone);
}


// One crater layer's height [m] (as TerrainDetail.cpp's CraterLayer)
float DetCraterLayer (dvec3 d, int k, int layer, double R, float density, float depthScale)
{
	int m = k - DET_JMIN + 32 + 29 * layer;
	float s = exp2 (float (k));
	ivec3 cell, p; vec3 sg;
	vec3 f = DetLattice (m, d, R / double (s), cell, p, sg);
	uint h0 = DetHash (cell, 1000u + uint (m));
	if (DetUnit16 (h0) >= density) return 0.0;
	uint h1 = DetPcg (h0), h2 = DetPcg (h1), h3 = DetPcg (h2), h4 = DetPcg (h3);
	// (MakeCrater)
	float rL = 0.06 + 0.10 * DetUnit16 (h4);
	float age = DetUnit16 (h4 >> 16);
	float cdep = 0.32 * depthScale * (1.0 - 0.7 * age);
	float crim = 0.08 * depthScale * (1.0 - 0.85 * age);
	float ck = 1.6 + 2.0 * age, wr = 0.6 + 0.2 * age;
	rL *= 1.0 + 0.12 * age;
	uint h5 = DetPcg (h4 ^ 0x9e3779b9u), h6 = DetPcg (h5), h7 = DetPcg (h6);
	float ecc = 0.12 * DetUnit16 (h5), lobe = 0.06 * DetUnit16 (h5 >> 16);
	vec3 a1 = DetUnitFromHash (h5), a2 = DetUnitFromHash (h6), a3 = DetUnitFromHash (h7);
	float ext = rL * (1.0 + ecc + lobe) * (1.0 + wr);
	vec3 c = ext + (1.0 - 2.0 * ext) * vec3 (DetUnit16 (h1), DetUnit16 (h2), DetUnit16 (h3));
	vec3 v = f - c;
	float dist = length (v);
	if (dist >= ext) return 0.0;
	// (CraterHeight)
	if (dist < 1e-9) return -cdep * rL * s;
	vec3 u = v / dist;
	float c1 = dot (u, a1), c2 = dot (u, a2), c3 = dot (u, a3);
	float rr = rL * (1.0 + ecc * (2.0 * c1 * c1 - 1.0) + lobe * (4.0 * c2 * c2 * c2 - 3.0 * c2));
	float x = dist / rr;
	float rim = crim * (1.0 + 0.4 * c3);
	float h;
	if (x < 1.0) h = rim - (rim + cdep) * pow (1.0 - x * x, ck);
	else {
		float t = (x - 1.0) / wr;
		h = t >= 1.0 ? 0.0 : rim * (1.0 - t * t) * (1.0 - t * t);
	}
	return h * rL * s;
}

// The detail's height [m] at unit direction d (as TerrainDetail::Height,
// after its amplitude A and fade f, which the caller has from the core):
// spacing: node spacing of the data [m], lmin: shortest wavelength [m]
// (at least the planet's lambdaMin); prm: hurst, ridge, crater cells,
// crater depth
// The pebbles' part of the detail [m] (crag: see DetPebbleLayer)
float DetPebbles (dvec3 d, double R, float f, float spacing, float lmin, float pebbles, float crag)
{
	float h = 0.0;
	if (pebbles <= 0.0 || f <= 0.0 || spacing <= 0.0) return 0.0;
	int nl = int (ceil (pebbles));
	for (int k = -1; k >= -4; k--) {
		float s = exp2 (float (k)), lg = DET_PEBBLE_G * s;
		if (lg < 0.5 * lmin) break;
		float w = DetSmoothStep (0.5 * lmin, lmin, lg) * DetSmoothStep (0.5, 1.5, log2 (spacing / s));
		if (w <= 0.0) continue;
		for (int l = 0; l < nl; l++)
			h += w * f * DetPebbleLayer (d, k, l, R, pebbles / float (nl), crag);
	}
	return h;
}

float DetHeight (dvec3 d, double R, float A, float f, float spacing, float lmin, vec4 prm, float pebbles)
{
	if (spacing <= 0.0 || f <= 0.0 || lmin >= spacing) return 0.0;
	float h = 0.0;
	if (A > 0.0) {
		int j1 = min (DET_JMAX, int (floor (log2 (spacing))));
		for (int j = j1; j >= DET_JMIN; j--) {
			float lambda = exp2 (float (j));
			if (lambda < 0.5 * lmin) break;
			float uo = log2 (spacing / lambda);
			float w = DetSmoothStep (0.5, 1.5, uo) * DetSmoothStep (0.5 * lmin, lmin, lambda);
			if (w <= 0.0) continue;
			float a = A * pow (lambda / spacing, prm.x);
			int m = j - DET_JMIN;
			ivec3 cell, p; vec3 sg;
			vec3 fr = DetLattice (m, d, R / double (lambda), cell, p, sg);
			float n = DetNoise (cell, fr, uint (m)).w;
			float r = prm.y * clamp (float (j - 3) / 3.0, 0.0, 1.0);
			h += a * w * ((1.0 - 0.4 * r) * n + 0.8 * r * (0.35 - abs (n)));
		}
	}
	if (prm.z > 0.0) {
		int k1 = min (DET_KMAX, int (floor (log2 (spacing / DET_CRATER_D))));
		int nl = int (ceil (prm.z));
		for (int k = k1; k >= DET_KMIN; k--) {
			float s = exp2 (float (k)), lambda = DET_CRATER_D * s, lg = DET_CRATER_G * s;
			if (lg < 0.5 * lmin) break;
			float w = DetSmoothStep (0.5, 1.5, log2 (spacing / lambda)) * DetSmoothStep (0.5 * lmin, lmin, lg);
			if (w <= 0.0) continue;
			for (int l = 0; l < nl; l++)
				h += w * f * DetCraterLayer (d, k, l, R, prm.z / float (nl), prm.w);
		}
	}
	return h + DetPebbles (d, R, f, spacing, lmin, pebbles, 0.0);
}

#endif
