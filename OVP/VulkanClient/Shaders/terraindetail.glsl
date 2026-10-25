// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Procedural terrain detail (the core's TerrainDetail): fractal gradient
// noise on the sphere, octaves of wavelengths 2^j m. The tile meshes
// contain the octaves down to a few node spacings (oapiSurfaceDetail); the
// shader adds the finer ones to the shading normal, evaluating the same
// function, so that an octave keeps its shape when a finer mesh takes it
// over. The sphere position needs double precision (1e-10 of the radius).
// Requires atmosphere.glsl (atm) and the tile push constants.

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

struct DetailSample {
	vec3 grad;     // gradient, planet frame [m/m]
	float height;  // height of the noise [m]
	float albedo;  // albedo factor of the craters (fresh ejecta)
	float shadow;  // sunlight factor: shadows of the crater rims
};

// The detail finer than the mesh (as TerrainDetail::Height)
// d: unit direction (double), R: radius; A: noise amplitude factor [m],
// fade: crater fade factor, spacing: node spacing of the elevation data
// [m], lmesh: shortest wavelength in the mesh [m], hurst; footprint:
// pixel size [m]
// sunP: direction to the sun, planet frame
DetailSample TerrainDetailEval (dvec3 d, double R, float A, float fade, float spacing, float lmesh, float hurst, float footprint, vec3 sunP)
{
	DetailSample ds;
	ds.grad = vec3 (0.0), ds.height = 0.0, ds.albedo = 1.0, ds.shadow = 1.0;
	int j1 = min (20, int (floor (log2 (spacing))));
	// noise octaves
	for (int j = j1; j >= DET_JMIN && A > 0.0; j--) {
		float lambda = exp2 (float (j));
		float wA = 1.0 - smoothstep (0.15, 0.35, footprint / lambda); // (craters down to 3 to 7 pixels) // (5 to 12 pixels: no aliasing, no grain)
		if (wA <= 0.0) break;
		float wT = smoothstep (0.5, 1.5, log2 (spacing / lambda));
		float wM = lambda >= 0.5 * lmesh ? smoothstep (0.5 * lmesh, lmesh, lambda) : 0.0;
		float w = wA * wT * (1.0 - wM);
		if (w <= 0.0) continue;
		int m = j - DET_JMIN;
		ivec3 cell, p; vec3 sg;
		vec3 f = DetLattice (m, d, R * double (exp2 (float (-j))), cell, p, sg);
		vec4 n = DetNoise (cell, f, uint (m));
		// the octaves' shape (as TerrainDetail.cpp's Shape)
		float r = atm.detailPrm.x * clamp (float (j - 3) / 3.0, 0.0, 1.0);
		float shape = (1.0 - 0.4 * r) * n.w + 0.8 * r * (0.35 - abs (n.w));
		n.xyz *= (1.0 - 0.4 * r) - 0.8 * r * sign (n.w);
		float a = A * pow (lambda / spacing, hurst);
		ds.grad += DetToPlanet (n.xyz, p, sg) * (a * w / lambda);
		ds.height += a * w * shape;
	}
	// craters (as TerrainDetail.cpp's CraterLayer)
	float density = atm.detailPrm.y;
	// (weights for the typical diameter, CRATER_D of the cell size, against
	// the data and the pixels; for the resolution the rims need, CRATER_G,
	// against the mesh, as TerrainDetail.cpp)
	const float CRATER_D = 0.22, CRATER_G = 0.1;
	int k1 = min (16, int (floor (log2 (spacing / CRATER_D))));
	for (int k = k1; k >= -2 && density > 0.0 && fade > 0.0; k--) {
		float s = exp2 (float (k)), lambda = CRATER_D * s, lg = CRATER_G * s;
		float wA = 1.0 - smoothstep (0.15, 0.35, footprint / lambda); // (craters down to 3 to 7 pixels)
		if (wA <= 0.0) break;
		float wT = smoothstep (0.5, 1.5, log2 (spacing / lambda));
		float wM = lg >= 0.5 * lmesh ? smoothstep (0.5 * lmesh, lmesh, lg) : 0.0;
		float w = wA * wT * (1.0 - wM) * fade;
		if (w <= 0.0) continue;
		// (densities above 1: several lattices)
		int nl = int (ceil (density));
		for (int l = 0; l < nl; l++) {
			int m = k - DET_JMIN + 32 + 29 * l;
			ivec3 cell, p; vec3 sg;
			vec3 f = DetLattice (m, d, R * double (exp2 (float (-k))), cell, p, sg);
			uint h0 = DetHash (cell, 1000u + uint (m));
			if (DetUnit16 (h0) >= density / float (nl)) continue;
			uint h1 = DetPcg (h0), h2 = DetPcg (h1), h3 = DetPcg (h2), h4 = DetPcg (h3);
			// the crater's shape (as TerrainDetail.cpp's MakeCrater)
			float rL = 0.06 + 0.10 * DetUnit16 (h4);
			float age = DetUnit16 (h4 >> 16);
			float cdep = 0.32 * atm.detailPrm.z * (1.0 - 0.7 * age);
			float crim = 0.08 * atm.detailPrm.z * (1.0 - 0.85 * age);
			float ck = 1.6 + 2.0 * age, wr = 0.6 + 0.2 * age;
			rL *= 1.0 + 0.12 * age;
			uint h5 = DetPcg (h4 ^ 0x9e3779b9u), h6 = DetPcg (h5), h7 = DetPcg (h6);
			float ecc = 0.12 * DetUnit16 (h5), lobe = 0.06 * DetUnit16 (h5 >> 16);
			vec3 a1 = DetUnitFromHash (h5), a2 = DetUnitFromHash (h6), a3 = DetUnitFromHash (h7);
			float ext = rL * (1.0 + ecc + lobe) * (1.0 + wr);
			vec3 c = ext + (1.0 - 2.0 * ext) * vec3 (DetUnit16 (h1), DetUnit16 (h2), DetUnit16 (h3));
			vec3 dv = f - c;
			float dist = length (dv);
			if (dist >= ext || dist < 1e-6) continue;
			vec3 u = dv / dist;
			float c1 = dot (u, a1), c2 = dot (u, a2);
			float rr = rL * (1.0 + ecc * (2.0 * c1 * c1 - 1.0) + lobe * (4.0 * c2 * c2 * c2 - 3.0 * c2));
			float x = dist / rr, rimH = crim * (1.0 + 0.4 * dot (u, a3));
			float hx, dhx; // height and slope along x [radii]
			if (x < 1.0) {
				float q = 1.0 - x * x;
				hx = rimH - (rimH + cdep) * pow (q, ck);
				dhx = (rimH + cdep) * ck * pow (max (q, 1e-6), ck - 1.0) * 2.0 * x;
			} else {
				float t = (x - 1.0) / wr;
				if (t >= 1.0) continue;
				hx = rimH * (1.0 - t * t) * (1.0 - t * t);
				dhx = rimH * 2.0 * (1.0 - t * t) * (-2.0 * t) / wr;
			}
			float rm = rL * s; // [m]
			// (gradient along the radius: the outline's turn is left out)
			vec3 dhdq = u * (dhx * rm / rr);
			ds.grad += DetToPlanet (dhdq, p, sg) * (w / s);
			// the rim's shadow in the bowl: shadowed where the rim towards
			// the sun rises above the line to the sun (the shading can't
			// cast shadows); its edge softened
			if (x < 1.0) {
				vec3 up = DetToLattice (vec3 (d), p, sg), sl = DetToLattice (sunP, p, sg);
				float se = dot (sl, up); // sine of the sun's elevation
				vec3 sh = sl - up * se;
				float shl = length (sh);
				if (se > 0.0 && shl > 1e-4) {
					sh /= shl;
					vec3 dt = dv - up * dot (dv, up);
					float b = dot (dt, sh), cc = dot (dt, dt) - rr * rr;
					float tr = -b + sqrt (max (b * b - cc, 0.0)); // to the rim towards the sun [cells]
					float rise = (rimH - hx) * rm;                // rim crest above the point [m]
					float need = rise - tr * s * se / shl;
					ds.shadow *= 1.0 - w * smoothstep (-0.08 * rm * (rimH + cdep), 0.08 * rm * (rimH + cdep), need);
				}
			}
			// fresh craters: bright ejecta about the rim
			float fresh = atm.detailPrm.w;
			if (age < fresh) {
				float e = (x - 1.15) / 0.45;
				if (abs (e) < 1.0) ds.albedo *= 1.0 + 0.4 * w * (1.0 - age / fresh) * (1.0 - e * e) * (1.0 - e * e);
			}
		}
	}
	return ds;
}

// Perturb the shading normal Nv (view space) of the surface at posView,
// and its albedo (weathered: darker in the hollows, lighter on the crests;
// fresh crater ejecta)
void TerrainDetailShading (vec3 posView, inout vec3 Nv, inout vec3 base, vec2 Af, inout float sunShadow)
{
	if (tile.detail.w < 0.5 || (Af.x <= 0.0 && (Af.y <= 0.0 || atm.detailPrm.y <= 0.0))) return;
	mat3 v2p = mat3 (atm.viewToPlanet);
	dvec3 cam = dvec3 (atm.detailCamHi.xyz) + dvec3 (atm.detailCamLo.xyz);
	double R = double (atm.detailCamHi.w) + double (atm.detailCamLo.w);
	dvec3 q = cam + dvec3 (v2p * posView);
	dvec3 d = q / length (q);
	float dist = length (posView);
	// pixel size [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (posView / dist, Nv)), 0.1);
	// (the mesh's band reaches twice as far when morphed to the parent's)
	DetailSample ds = TerrainDetailEval (d, R, Af.x, Af.y, tile.detail.x, tile.detail.y * (1.0 + vMorph), tile.detail.z, footprint, atm.sunDir.xyz);
	sunShadow *= ds.shadow;
	base *= clamp (1.0 + ds.height / max (Af.x, 1.0), 0.8, 1.2) * ds.albedo;
	vec3 g = ds.grad;
	vec3 df = vec3 (d);
	g -= df * dot (g, df);         // (along the surface)
	vec3 gv = transpose (v2p) * g; // view space
	gv -= Nv * dot (gv, Nv);
	Nv = normalize (Nv - gv);
}
