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

// Gradient of the detail octaves finer than the mesh, planet frame [m/m]
// (xyz), and their height [m] (w)
// d: unit direction (double), R: radius; A: amplitude factor [m],
// spacing: node spacing of the elevation data [m], lmesh: shortest
// wavelength in the mesh [m], hurst; footprint: pixel size [m]
vec4 TerrainDetailGrad (dvec3 d, double R, float A, float spacing, float lmesh, float hurst, float footprint)
{
	const ivec3 P[6] = ivec3[] (ivec3 (0,1,2), ivec3 (1,2,0), ivec3 (2,0,1), ivec3 (0,2,1), ivec3 (1,0,2), ivec3 (2,1,0));
	const vec3 S[4] = vec3[] (vec3 (1,1,1), vec3 (1,-1,-1), vec3 (-1,1,-1), vec3 (-1,-1,1));
	vec3 grad = vec3 (0.0);
	float height = 0.0;
	int j1 = min (20, int (floor (log2 (spacing))));
	for (int j = j1; j >= DET_JMIN; j--) {
		float lambda = exp2 (float (j));
		float wA = 1.0 - smoothstep (0.08, 0.2, footprint / lambda); // (5 to 12 pixels: no aliasing, no grain)
		if (wA <= 0.0) break;
		float wT = smoothstep (0.5, 1.5, log2 (spacing / lambda));
		float wM = lambda >= 0.5 * lmesh ? smoothstep (0.5 * lmesh, lmesh, lambda) : 0.0;
		float w = wA * wT * (1.0 - wM);
		if (w <= 0.0) continue;
		// the octave's lattice position (as TerrainDetail.cpp's OctavePoint)
		int m = j - DET_JMIN, k = m % 24;
		ivec3 p = P[k % 6];
		vec3 sg = S[k / 6] * ((k % 6) < 3 ? 1.0 : -1.0);
		double scale = R * double (exp2 (float (-j)));
		dvec3 q = dvec3 (sg.x * d[p.x], sg.y * d[p.y], sg.z * d[p.z]) * scale
			+ dvec3 (double ((m * 37) & 63), double ((m * 23) & 63), double ((m * 53) & 63)) / 64.0;
		dvec3 fl = floor (q);
		vec4 n = DetNoise (ivec3 (fl), vec3 (q - fl), uint (m));
		float r = clamp (float (j - 3) / 3.0, 0.0, 1.0); // (the octaves' shape, as TerrainDetail.cpp's Shape)
		n.xyz *= (1.0 - 0.4 * r) - 0.8 * r * sign (n.w);
		float shape = (1.0 - 0.4 * r) * n.w + 0.8 * r * (0.35 - abs (n.w));
		// back to the planet frame: dq_i/dd[p_i] = sg_i * scale, and d moves
		// by 1/R per metre along the surface
		vec3 g = vec3 (0.0);
		g[p.x] += n.x * sg.x, g[p.y] += n.y * sg.y, g[p.z] += n.z * sg.z;
		float a = A * pow (lambda / spacing, hurst);
		grad += g * (a * w / lambda);
		height += a * w * shape;
	}
	return vec4 (grad, height);
}

// Perturb the shading normal Nv (view space) of the surface at posView,
// and its albedo (weathered: darker in the hollows, lighter on the crests)
void TerrainDetailShading (vec3 posView, inout vec3 Nv, inout vec3 base, float A)
{
	if (tile.detail.w < 0.5 || A <= 0.0) return;
	mat3 v2p = mat3 (atm.viewToPlanet);
	dvec3 cam = dvec3 (atm.detailCamHi.xyz) + dvec3 (atm.detailCamLo.xyz);
	double R = double (atm.detailCamHi.w) + double (atm.detailCamLo.w);
	dvec3 q = cam + dvec3 (v2p * posView);
	dvec3 d = q / length (q);
	float dist = length (posView);
	// pixel size [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (posView / dist, Nv)), 0.1);
	vec4 gh = TerrainDetailGrad (d, R, A, tile.detail.x, tile.detail.y, tile.detail.z, footprint);
	vec3 g = gh.xyz;
	base *= clamp (1.0 + gh.w / max (A, 1.0), 0.8, 1.2);
	vec3 df = vec3 (d);
	g -= df * dot (g, df);         // (along the surface)
	vec3 gv = transpose (v2p) * g; // view space
	gv -= Nv * dot (gv, Nv);
	Nv = normalize (Nv - gv);
}
