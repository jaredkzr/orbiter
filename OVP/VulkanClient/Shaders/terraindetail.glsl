// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Procedural terrain detail (the core's TerrainDetail): fractal gradient
// noise on the sphere, octaves of wavelengths 2^j m. The tile meshes
// contain the octaves down to a few node spacings (oapiSurfaceDetail); the
// shader adds the finer ones to the shading normal, evaluating the same
// function, so that an octave keeps its shape when a finer mesh takes it
// over. All in single precision: points as tile-relative offsets from a
// reference point split into integer and fraction (DetPoint).
// Requires atmosphere.glsl (atm) and the tile push constants.

#include "detailcore.glsl"

struct DetailSample {
	vec3 grad;     // gradient, planet frame [m/m]
	float height;  // height of the noise [m]
	float albedo;  // albedo factor of the craters (fresh ejecta)
	float shadow;  // sunlight factor: shadows of the crater rims
};

// The detail finer than the mesh (as TerrainDetail::Height)
// pt: the point, d: its unit direction; A: noise amplitude factor [m],
// fade: crater fade factor, spacing: node spacing of the elevation data
// [m], lmesh: shortest wavelength in the mesh [m], hurst; footprint:
// pixel size [m]
// sunP: direction to the sun, planet frame
DetailSample TerrainDetailEval (DetPoint pt, vec3 d, float A, float fade, float spacing, float lmesh, float hurst, float footprint, vec3 sunP)
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
		vec3 f = DetLattice (m, pt, j, cell, p, sg);
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
			vec3 f = DetLattice (m, pt, k, cell, p, sg);
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
				vec3 up = DetToLattice (d, p, sg), sl = DetToLattice (sunP, p, sg);
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
	// the point on the sphere: from the tile's reference point D0 = d0 R
	// (tile.refI + refF), the position relative to d0 (R + h0) (refRel):
	// D - D0 = d0 R f + e R/(R+h0) (1 + f), f = (1 + u)^-1/2 - 1 (written
	// without cancellation, exact also for the large offsets of coarse tiles)
	vec3 e = v2p * posView - tile.refRel.xyz;
	float R = tile.refF.w, Rh = tile.refRel.w;
	vec3 d0 = normalize (vec3 (tile.refI.xyz) + tile.refF.xyz);
	float u = 2.0 * dot (d0, e) / Rh + dot (e, e) / (Rh * Rh);
	float su = sqrt (max (1.0 + u, 1e-6));
	float fu = -u / (su * (1.0 + su));
	vec3 dl = d0 * (R * fu) + e * ((R / Rh) * (1.0 + fu));
	DetPoint pt = DetPointAt (tile.refI.xyz, tile.refF.xyz, dl);
	vec3 d = normalize (d0 + dl / R);
	float dist = length (posView);
	// pixel size [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (posView / dist, Nv)), 0.1);
	// (the mesh's band reaches twice as far when morphed to the parent's)
	DetailSample ds = TerrainDetailEval (pt, d, Af.x, Af.y, tile.detail.x, tile.detail.y * (1.0 + vMorph), tile.detail.z, footprint, atm.sunDir.xyz);
	// pebbles: their slopes per pixel (the meshes have their shape, but
	// normals of the ground under them), by differences along the surface,
	// with crags on their faces; down to stones of a few pixels
	// (all stones of two pixels and more: the meshes have the larger ones'
	// shape, but not their slopes)
	if (tile.morph.w > 0.0 && footprint < 0.03 && Af.y > 0.0) {
		vec3 df0 = d;
		vec3 t1 = normalize (cross (df0, abs (df0.y) < 0.9 ? vec3 (0.0, 1.0, 0.0) : vec3 (1.0, 0.0, 0.0)));
		vec3 t2 = cross (df0, t1);
		float e = max (0.5 * footprint, 0.001);
		int nl = int (ceil (tile.morph.w));
		vec3 g = vec3 (0.0);
		float hs = 0.0;
		for (int k = -1; k >= -4; k--) {
			float s = exp2 (float (k));
			float w = DetSmoothStep (1.5, 3.5, 0.18 * s / footprint) * DetSmoothStep (0.5, 1.5, log2 (tile.detail.x / s)) * Af.y;
			if (w <= 0.0) continue;
			// (the crags fade out when the stone is a few pixels)
			float crag = 1.0 - smoothstep (0.015 * s, 0.04 * s, footprint);
			for (int l = 0; l < nl; l++) {
				int m = k - DET_JMIN + 64 + 29 * l;
				ivec3 cell, p; vec3 sg;
				vec3 f = DetLattice (m, pt, k, cell, p, sg);
				bool stone;
				float h0 = DetPebbleCell (cell, m, k, tile.morph.w / float (nl), f, crag, stone);
				if (!stone) continue;
				vec3 o1 = DetToLattice (t1, p, sg) * (e / s), o2 = DetToLattice (t2, p, sg) * (e / s);
				float h1 = DetPebbleCell (cell, m, k, tile.morph.w / float (nl), f + o1, crag, stone);
				float h2 = DetPebbleCell (cell, m, k, tile.morph.w / float (nl), f + o2, crag, stone);
				g += w * (t1 * ((h1 - h0) / e) + t2 * ((h2 - h0) / e));
				hs += w * h0;
			}
		}
		ds.grad += g;
		ds.albedo *= mix (1.0, 0.9, smoothstep (0.001, 0.004, hs)); // (stones a little darker than the dust)
	}
	sunShadow *= ds.shadow;
	base *= clamp (1.0 + ds.height / max (Af.x, 1.0), 0.8, 1.2) * ds.albedo;
	vec3 g = ds.grad;
	vec3 df = d;
	g -= df * dot (g, df);         // (along the surface)
	vec3 gv = transpose (v2p) * g; // view space
	gv -= Nv * dot (gv, Nv);
	Nv = normalize (Nv - gv);
}
