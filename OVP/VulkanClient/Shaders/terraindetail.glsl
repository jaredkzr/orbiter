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

// Site-scale materials (F3) compiled in: the tile pipeline variant for worlds with them
// (specialization constant; the default variant has the whole site path folded away)
layout(constant_id = 0) const bool SITE_MATERIALS = false;

struct DetailSample {
	vec3 grad;     // gradient, planet frame [m/m]
	float height;  // height of the noise [m]
	float albedo;  // albedo factor of the craters (fresh ejecta)
	float shadow;  // sunlight factor: shadows of the crater rims
};

// A pebble cell's height [m] (w) and its gradient in the cell [m per cell]
// (xyz); the height as DetPebbleCell's (keep them identical, with the
// core's PebbleLayer); top: the profile's value (0 without a stone),
// tone: the stone's albedo offset (-1 to 1)
vec4 DetPebbleCellGrad (ivec3 cell, int m, int k, float density, vec3 f, float crag, DetPebbleShape S, bool soft, out float top, out float tone)
{
	top = 0.0, tone = 0.0;
	float s = exp2 (float (k));
	uint h0 = DetHash (cell, 3000u + uint (m));
	float sc = 1.0; // (site materials: the soft threshold, as DetPebbleCell's)
	if (soft) {
		float tt0 = density * (k == -1 ? 0.4 : k == -2 ? 0.7 : 1.0);
		float u0 = DetUnit16 (h0);
		if (u0 >= tt0 + DET_SITE_SOFT) return vec4 (0.0);
		sc = 1.0 - DetSmoothStep (tt0 - DET_SITE_SOFT, tt0 + DET_SITE_SOFT, u0);
	}
	else if (DetUnit16 (h0) >= density * (k == -1 ? 0.4 : k == -2 ? 0.7 : 1.0)) return vec4 (0.0);
	uint h1 = DetPcg (h0), h2 = DetPcg (h1), h3 = DetPcg (h2), h4 = DetPcg (h3);
	float r = S.size * (0.12 + 0.12 * DetUnit16 (h4)), hgt = S.height * (0.25 + 0.3 * DetUnit16 (h4 >> 16));
	float ext = 1.05 * r / (1.0 - S.elong);
	vec3 c = ext + (1.0 - 2.0 * ext) * vec3 (DetUnit16 (h1), DetUnit16 (h2), DetUnit16 (h3));
	vec3 v = f - c;
	if (length (v) >= ext) return vec4 (0.0);
	// the soft maximum of the facet planes, x, and its gradient (the
	// planes' normals weighted by their shares of the sum)
	// (elongated: the polygon of vp = M v, M = I - elong a aT)
	vec3 vp = v, ea = vec3 (0.0);
	if (S.elong > 0.0) { ea = DetUnitFromHash (DetPcg (h2 ^ 0x7f4a7c15u)); vp = v - ea * (S.elong * dot (v, ea)); }
	float x = 0.0;
	vec3 dx = vec3 (0.0);
	uint he = h4;
	for (int e = 0; e < 5; e++) {
		he = DetPcg (he ^ 0x51ed27u);
		vec3 n = DetUnitFromHash (he);
		float re = r * (0.75 + 0.25 * DetUnit16 (he >> 16));
		float t = exp (dot (vp, n) / re / DET_PEBBLE_SOFT);
		x += t;
		dx += t * n / re;
	}
	dx /= x;
	if (S.elong > 0.0) dx -= ea * (S.elong * dot (dx, ea)); // (M dx)
	x = DET_PEBBLE_SOFT * log (x);
	if (S.round > 0.0) {
		// (mixed towards the ellipse)
		vec3 dxr = vp / (r * max (length (vp), 1e-6));
		if (S.elong > 0.0) dxr -= ea * (S.elong * dot (dxr, ea));
		x += (length (vp) / r - x) * S.round;
		dx += (dxr - dx) * S.round;
	}
	if (x >= 1.0) return vec4 (0.0);
	float tt = clamp ((1.0 - x) / S.bevel, 0.0, 1.0);
	top = tt * tt * (3.0 - 2.0 * tt) * sc;
	tone = DetUnit16 (DetPcg (h4 ^ 0x68e31da4u)) * 2.0 - 1.0;
	vec3 dtop = -dx * (6.0 * tt * (1.0 - tt) / S.bevel);
	vec3 nt = DetUnitFromHash (DetPcg (h3 ^ 0x2545f491u));
	float tilt = 1.0 + S.tilt * dot (v, nt) / r;
	vec3 dtilt = tilt > 0.3 ? S.tilt * nt / r : vec3 (0.0);
	tilt = max (tilt, 0.3);
	float a = hgt * r * s;
	float h = a * top * tilt;
	vec3 dh = a * (dtop * tilt + top * dtilt);
	if (crag > 0.0) {
		vec3 q = v / (0.33 * r);
		vec4 n1 = DetNoise (ivec3 (floor (q)), fract (q), h4);
		q *= 2.0;
		vec4 n2 = DetNoise (ivec3 (floor (q)), fract (q), h4 + 1u);
		float n = n1.w + 0.5 * n2.w;
		vec3 dn = (n1.xyz + n2.xyz) / (0.33 * r); // (the second octave: half the amplitude, twice the frequency)
		float mf = min (1.0, 5.0 * (1.0 - x));
		vec3 dmf = mf < 1.0 ? -5.0 * dx : vec3 (0.0);
		float b = crag * 0.05 * r * s;
		h += b * n * mf;
		dh += b * (dn * mf + n * dmf);
	}
	return vec4 (dh * sc, h * sc);
}

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
		if (atm.pebPrm.z > 0.0 && j <= 1) a = max (a, atm.pebPrm.z * lambda * fade);   // (the slope floor, as DetHeight's micro)
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
			float ext = rL * (1.0 + ecc + lobe) * (1.0 + wr);
			vec3 c = ext + (1.0 - 2.0 * ext) * vec3 (DetUnit16 (h1), DetUnit16 (h2), DetUnit16 (h3));
			vec3 dv = f - c;
			float dist = length (dv);
			if (dist >= ext || dist < 1e-6) continue;
			vec3 a1 = DetUnitFromHash (h5), a2 = DetUnitFromHash (h6), a3 = DetUnitFromHash (h7);
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

// Whether stones may lie on a texel of colour c (Earth, shading only): not
// on vegetation, snow and bright flats (dry lakes, salt)
float DetPebbleGate (vec3 c)
{
	float mx = max (c.r, max (c.g, c.b)), mn = min (c.r, min (c.g, c.b));
	float lum = dot (c, vec3 (0.2126, 0.7152, 0.0722)), sat = (mx - mn) / max (mx, 1e-4);
	float veg = smoothstep (0.02, 0.08, (c.g - max (c.r, c.b)) / max (c.g + max (c.r, c.b), 1e-4));
	float bright = smoothstep (0.30, 0.45, lum) * (1.0 - smoothstep (0.10, 0.22, sat));
	return (1.0 - veg) * (1.0 - bright);
}

const float DET_SKY_EXP = 2.0;

// Playa (B2): the weight comes per vertex from the core (oapiSurfaceDetail: colour, climate, relief), Af.z
// Playa colour (pass 2, P1): the textures draw dry lakes and salt flats as a neutral, often clipped white
// (snow, to the eye). Their brightness relative to the desert around them matches the measured albedos
// (Rogers' lakebed 0.80 against the Mojave alluvium's 0.58 in the texture's units; about 0.40 against
// 0.27 measured), so the texel's luminance stays, under a soft cap of the clipped whites, and the hue
// becomes a pale buff (clay playas; salt flats read a little paler through their brighter texels).
// SurfaceQuery.cpp's PlayaColour (the dust's colour) is the same.
// The salt flats (Config/VK/Playa.cfg: the textures can't tell them from clay) are an off-white crust.
const vec3 PLAYA_HUE = vec3 (1.16, 0.98, 0.76); // (luminance 1)
const vec3 PLAYA_SALT_HUE = vec3 (1.03, 1.0, 0.94);
const float PLAYA_KNEE = 0.7;
// the salt share at a planet-frame direction (1 inside a listed flat, fading over its outer fifth)
float PlayaSalt (vec3 dPl)
{
	float s = 0.0;
	int n = int (atm.playaPrm.y);
	for (int i = 0; i < n; i++) {
		float r = atm.saltFlat[i].w;
		s = max (s, 1.0 - smoothstep (0.8 * r, r, length (dPl - atm.saltFlat[i].xyz)));
	}
	return s;
}
vec3 PlayaColour (vec3 c, float salt)
{
	float L = dot (c, vec3 (0.2126, 0.7152, 0.0722));
	if (L > PLAYA_KNEE) L = PLAYA_KNEE + 0.5 * (L - PLAYA_KNEE);
	return L * mix (PLAYA_HUE, PLAYA_SALT_HUE, salt);
}

const float PLAYA_JITTER = 0.75, PLAYA_WARP = 0.05, PLAYA_CURL_W = 0.15, PLAYA_EDGE = 2.2, PLAYA_TONE = 0.03;
// layers: macro, primary, secondary
const int PLAYA_K[3] = int[3] (2, -2, -4);
const int PLAYA_M[3] = int[3] (210, 206, 204);
const float PLAYA_HW[3] = float[3] (0.0025, 0.012, 0.016);
const float PLAYA_CURL[3] = float[3] (0.0, 0.02, 0.015);
const float PLAYA_DARK[3] = float[3] (0.55, 0.30, 0.55);

// One layer of desiccation cracks (cells 2^k m, lattice m). Returns the crack's coverage of the
// pixel (box filter: fpc = the pixel's half size in cells); adds the curled rims' slope (planet
// frame, m/m, times wg) to grad; tone: the polygon's albedo offset (-1..1)
float PlayaCrackLayer (DetPoint pt, vec3 d, int k, int m, float hw, float curl, float fpc, float wg, inout vec3 grad, out float tone)
{
	ivec3 cell, p; vec3 sg;
	vec3 f = DetLattice (m, pt, k, cell, p, sg);
	f += PLAYA_WARP * sin (12.5663706 * f.yzx + vec3 (1.3, 4.1, 2.7)); // (wavy edges; period 1/2 cell: continuous across cells)
	vec3 up = DetToLattice (d, p, sg);
	float d1 = 1e9, d2 = 1e9; vec3 q1 = vec3 (0.0), q2 = vec3 (0.0); uint hn = 0u;
	for (int z = -1; z <= 1; z++)
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		ivec3 o = ivec3 (x, y, z);
		uint h = DetHash (cell + o, 5000u + uint (m));
		vec3 q = vec3 (o) + 0.5 + PLAYA_JITTER * (vec3 (h & 1023u, (h >> 10) & 1023u, (h >> 20) & 1023u) / 1023.0 - 0.5);
		vec3 v = q - f;
		float dd = dot (v, v);
		if (dd < d1) { d2 = d1; q2 = q1; d1 = dd; q1 = q; hn = h; }
		else if (dd < d2) { d2 = dd; q2 = q; }
	}
	vec3 n = normalize (q2 - q1);
	vec3 nt = n - up * dot (n, up);
	float ln = length (nt), lt = max (ln, 0.25);
	float wf = DetSmoothStep (0.2, 0.45, ln); // (a face lying flat in the slice: no crack, instead of a smear)
	float e = dot (0.5 * (q1 + q2) - f, n) / lt;   // distance to the edge along the ground [cells]
	float cov = clamp ((min (e + fpc, hw) - max (e - fpc, -hw)) / (2.0 * fpc), 0.0, 1.0) * wf;
	float t = (e - hw) / PLAYA_CURL_W;              // curled rims: h = curl (1 - t)^2 [cells] for 0 < t < 1
	if (curl > 0.0 && wg > 0.0 && t > 0.0 && t < 1.0)
		grad += DetToPlanet ((-2.0 * curl * (1.0 - t) / PLAYA_CURL_W) * (-nt / lt), p, sg) * (wg * wf);
	tone = DetUnit16 (DetPcg (hn ^ 0x3c6ef372u)) * 2.0 - 1.0;
	return cov;
}

// Site materials (F3): the ground colour by the weights w (coarse, fine, flat) of the
// materials' tints and absolute colours. Keep in sync with SurfaceQuery.cpp's SiteColour.
vec3 SiteColour (vec3 base, vec3 w)
{
	vec3 acc = base * (1.0 - (w.x + w.y + w.z));
	for (int i = 0; i < 3; i++) {
		if (w[i] <= 0.0) continue;
		vec3 c = min (base * atm.siteMat[3 * i].rgb, vec3 (0.92));
		c = mix (c, atm.siteMat[3 * i + 1].rgb, atm.siteMat[3 * i].a);
		acc += w[i] * c;
	}
	return acc;
}

// Perturb the shading normal Nv (view space) of the surface at posView,
// and its albedo (weathered: darker in the hollows, lighter on the crests;
// fresh crater ejecta)
// the pixel's point on the sphere (DetPoint) and its unit direction d, from its view-space position
DetPoint DetPixelPoint (vec3 posView, out vec3 d)
{
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 e = v2p * posView - tile.refRel.xyz;
	float R = tile.refF.w, Rh = tile.refRel.w;
	vec3 d0 = normalize (vec3 (tile.refI.xyz) + tile.refF.xyz);
	float u = 2.0 * dot (d0, e) / Rh + dot (e, e) / (Rh * Rh);
	float su = sqrt (max (1.0 + u, 1e-6));
	float fu = -u / (su * (1.0 + su));
	vec3 dl = d0 * (R * fu) + e * ((R / Rh) * (1.0 + fu));
	d = normalize (d0 + dl / R);
	return DetPointAt (tile.refI.xyz, tile.refF.xyz, dl);
}

// Far mottling (pass 2, T1): the ground's brightness varies at 256, 64 and 16 m (the textures of many
// bodies have texels of a kilometre and more: beyond the pebbles' and cracks' reach the ground was one
// flat tone, a cliff at 15 m). On the precise lattice (no repeat); each octave fades to its mean as it
// shrinks below a few pixels. amp: about the octaves' sum's spread. footprint: the pixel's size along
// the ground [m] (stretched at grazing angles)
float DetFarMottle (vec3 posView, float footprint, float amp)
{
	if (amp <= 0.0) return 1.0;
	vec3 d;
	DetPoint pt = DetPixelPoint (posView, d);
	float m = 0.0;
	const float W[3] = float[3] (0.5, 0.35, 0.25);
	for (int i = 0; i < 3; i++) {
		int e = 8 - 2 * i;
		float v = 1.0 - smoothstep (0.12, 0.3, footprint / exp2 (float (e)));
		if (v <= 0.0) break;
		ivec3 cell, p; vec3 sg;
		vec3 f = DetLattice (231 + i, pt, e, cell, p, sg);
		m += W[i] * v * DetNoise (cell, f, 9300u + uint (i)).w;
	}
	return max (1.0 + amp * m, 0.5);
}

// Sea ice of the procedural ocean worlds (pass 2, I1; shading only, the ground stays flat). The pack is
// drawn by the textures as a bluish white, which reads as pale sand under a red dwarf's light; what makes
// it ice is its structure: floes (Voronoi cells of about 128 m on the detail's lattice, each with its own
// tone: snow-covered or bare) and between them pressure ridges (piles of broken blocks about 1.5 m high),
// refrozen leads (darker new ice) or plain cracks; hummocks on the floes. w: the pack's share here.
const float ICE_RIDGE_H = 1.5, ICE_RIDGE_W = 4.0, ICE_LEAD_W = 1.5; // [m]: ridge height, ridge half width, lead half width
void SeaIceShading (vec3 posView, inout vec3 Nv, inout vec3 base, float w)
{
	if (w <= 0.0) return;
	mat3 v2p = mat3 (atm.viewToPlanet);
	vec3 d;
	DetPoint pt = DetPixelPoint (posView, d);
	float dist = length (posView);
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0) / max (abs (dot (posView / dist, Nv)), 0.1);
	if (footprint > 30.0) return;
	// the floes: a 2-D Voronoi pattern in the lattice plane facing up most directly (9 cells, not the 27 of
	// a 3-D one: perf; where that plane changes, a line on the planet, the pattern breaks like a lead)
	const int K = 7, M = 221;
	const float CELL = 128.0;
	ivec3 cell, p; vec3 sg;
	vec3 f = DetLattice (M, pt, K, cell, p, sg);
	vec3 up = DetToLattice (d, p, sg);
	vec3 au = abs (up);
	int ax = au.x > au.y ? (au.x > au.z ? 0 : 2) : (au.y > au.z ? 1 : 2);
	int ia = ax == 0 ? 1 : 0, ib = ax == 2 ? 1 : 2;
	vec2 f2 = vec2 (f[ia], f[ib]);
	ivec2 c2 = ivec2 (cell[ia], cell[ib]);
	float d1 = 1e9, d2 = 1e9; vec2 q1 = vec2 (0.0), q2 = vec2 (0.0); uint h1 = 0u, h2 = 0u;
	for (int y = -1; y <= 1; y++)
	for (int x = -1; x <= 1; x++) {
		ivec2 o = ivec2 (x, y);
		uint h = DetHash (ivec3 (c2 + o, ax), 9000u);
		vec2 q = vec2 (o) + 0.5 + 0.8 * (vec2 (h & 1023u, (h >> 10) & 1023u) / 1023.0 - 0.5);
		vec2 v = q - f2;
		float dd = dot (v, v);
		if (dd < d1) { d2 = d1; q2 = q1; h2 = h1; d1 = dd; q1 = q; h1 = h; }
		else if (dd < d2) { d2 = dd; q2 = q; h2 = h; }
	}
	vec2 n2 = normalize (q2 - q1);
	vec3 n = vec3 (0.0);
	n[ia] = n2.x, n[ib] = n2.y;
	vec3 nt = n - up * dot (n, up);
	float ln = length (nt), lt = max (ln, 0.25);
	float wf = 1.0;
	float eM = dot (0.5 * (q1 + q2) - f2, n2) / lt * CELL;      // distance to the edge along the ground [m]
	vec3 de = DetToPlanet (-nt / lt, p, sg) / CELL;            // its gradient per metre (planet frame)
	uint pair = DetPcg (min (h1, h2) * 747796405u ^ max (h1, h2));
	uint kind = pair % 8u;                                     // 0-4: ridge, 5-6: lead, 7: crack
	float tone = DetUnit16 (DetPcg (h1 ^ 0x3c6ef372u));       // the floe's own tone (0: bare, 1: snow)
	vec3 g = vec3 (0.0);
	float ridge = 0.0, lead = 0.0;
	if (kind <= 4u) {
		float hw = ICE_RIDGE_W * (0.6 + 0.8 * DetUnit16 (pair >> 8)), H = ICE_RIDGE_H * (0.5 + DetUnit16 (pair >> 16));
		float t = clamp (abs (eM) / hw, 0.0, 1.0);
		float aa = 1.0 - smoothstep (0.5, 1.5, footprint / hw);
		ridge = (1.0 - t * t * (3.0 - 2.0 * t)) * wf;
		g += de * (-H * 6.0 * t * (1.0 - t) / hw * sign (eM) * aa * wf);
		// the blocks of the ridge (1-2 m)
		float aaB = 1.0 - smoothstep (0.25, 0.5, footprint / 2.0);
		if (ridge > 0.01 && aaB > 0.0) {
			ivec3 c2, p2; vec3 s2;
			vec3 f2 = DetLattice (M + 1, pt, 1, c2, p2, s2);
			vec4 nb = DetNoise (c2, f2, 9010u);
			g += DetToPlanet (nb.xyz, p2, s2) * (0.6 * H * ridge * aaB * 0.5);
		}
	} else if (kind <= 6u) {
		float t = clamp (abs (eM) / ICE_LEAD_W, 0.0, 1.0);
		lead = (1.0 - t * t * (3.0 - 2.0 * t)) * (1.0 - smoothstep (0.5, 2.0, footprint / ICE_LEAD_W)) * wf;
	} else {
		float t = clamp (abs (eM) / 0.3, 0.0, 1.0);
		lead = 0.5 * (1.0 - t * t * (3.0 - 2.0 * t)) * (1.0 - smoothstep (0.15, 0.6, footprint)) * wf;
	}
	// hummocks on the floes (8 m)
	float aaH = 1.0 - smoothstep (0.25, 0.5, footprint / 8.0);
	if (aaH > 0.0) {
		ivec3 c3, p3; vec3 s3;
		vec3 f3 = DetLattice (M + 2, pt, 3, c3, p3, s3);
		vec4 nh = DetNoise (c3, f3, 9020u);
		g += DetToPlanet (nh.xyz, p3, s3) * (0.3 * mix (0.4, 1.0, 1.0 - tone) / 8.0 * aaH);
	}
	g -= d * dot (g, d);
	vec3 gv = transpose (v2p) * (g * w);
	gv -= Nv * dot (gv, Nv);
	Nv = normalize (Nv - gv);
	// colours: snow-covered floes a little brighter, bare ones bluer and darker; ridges fresh and bright;
	// leads the grey of new ice
	vec3 floe = mix (vec3 (0.86, 0.92, 1.0), vec3 (1.04, 1.04, 1.04), tone);
	vec3 alb = mix (floe, vec3 (1.1), ridge) * (1.0 - 0.45 * lead);
	base *= mix (vec3 (1.0), alb, w);
}

void TerrainDetailShading (vec3 posView, inout vec3 Nv, inout vec3 base, vec3 Af, inout float sunShadow, float land, inout float skyVis, out float playa, float siteG, out float siteSmooth)
{
	playa = 0.0;
	siteSmooth = 0.0;
	// stones: not on water (land), and on Earth by the texel's colour
	float pg = land * (atm.pebPrm.w > 0.5 ? DetPebbleGate (base) : 1.0);
	bool stones = (tile.morph.w > 0.0 || atm.pebPrm.y > 0.0) && pg > 0.0;
	if (tile.detail.w < 0.5 || (Af.x <= 0.0 && (Af.y <= 0.0 || (atm.detailPrm.y <= 0.0 && !stones)))) return;
	vec3 gs = vec3 (0.0); // (the stones' own slopes)
	DetPebbleShape S = DetPebbleShape (atm.pebShape.x, atm.pebShape.y, atm.pebShape.z, atm.pebShape.w, atm.pebShape2.x, atm.pebShape2.y);
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
	playa = (SITE_MATERIALS && atm.sitePrm.x > 0.5) ? 0.0 : atm.playaPrm.x * Af.z * land; // (the core's weight: where it levels the ground; not on drawn water)
	float dist = length (posView);
	// pixel size [m] (stretched along the view at grazing angles)
	float footprint = dist * 2.0 * atm.misc.z / max (frame.jitter.w, 1.0)
		/ max (abs (dot (posView / dist, Nv)), 0.1);
	// (the mesh's band reaches twice as far when morphed to the parent's)
	// site materials: the weights of the coarse, fine and flat material (the ground share
	// siteG and the land mask keep them off the sea, ice floes and snow)
	float siteDens = 0.0; // (the density factor of the materials: the only site state live past the block)
	float gritDens = atm.pebPrm.y;
	bool site = SITE_MATERIALS && atm.sitePrm.x > 0.5;
	if (site) {
		DetSite st = DetSite (1.0, uint (atm.sitePrm.y), atm.sitePrm.z, atm.sitePrm.w, atm.siteThr.xyz,
			vec3 (atm.siteMat[2].x, atm.siteMat[5].x, atm.siteMat[8].x));
		// (the mask's fields come from the vertices, Af.z; per pixel only the noise's raggedness would add: not worth its cost)
		float fA, ws;
		DetSiteDecode (Af.z, fA, ws);
		vec3 sw = DetSiteShadeShare (fA, ws, st, footprint, atm.siteCover.xyz) * (siteG * land);
		base = SiteColour (base, sw);
		vec3 kg = vec3 (atm.siteMat[1].a, atm.siteMat[4].a, atm.siteMat[7].a);
		gritDens = clamp (atm.pebPrm.y * (1.0 + dot (sw, kg - 1.0)), 0.0, 1.0);
		playa = max (playa, dot (sw, vec3 (atm.siteMat[2].y, atm.siteMat[5].y, atm.siteMat[8].y)));
		siteSmooth = dot (sw, vec3 (atm.siteMat[2].z, atm.siteMat[5].z, atm.siteMat[8].z));
		siteDens = DetSiteDensity (sw, st);
	}
	DetailSample ds = TerrainDetailEval (pt, d, Af.x, Af.y, tile.detail.x, tile.detail.y * (1.0 + vMorph), tile.detail.z, footprint, atm.sunDir.xyz);
	// pebbles: their slopes per pixel (the meshes have their shape, but
	// normals of the ground under them), with crags on their faces; down
	// to stones of a few pixels
	// (all stones of two pixels and more: the meshes have the larger ones'
	// shape, but not their slopes)
	if (tile.morph.w > 0.0 && footprint < 0.03 && Af.y > 0.0 && pg > 0.0) {
		// (site materials: the lattices fill in sequence, as DetPebbles)
		int nl = site ? int (ceil (tile.morph.w * atm.sitePrm.w)) : int (ceil (tile.morph.w));
		float P = tile.morph.w * siteDens;
		float hs = 0.0, ts = 0.0;
		for (int k = -1; k >= -4; k--) {
			float s = exp2 (float (k));
			float w = DetSmoothStep (1.5, 3.5, 0.18 * s / footprint) * DetSmoothStep (0.5, 1.5, log2 (tile.detail.x / s)) * Af.y * pg;
			if (w <= 0.0) continue;
			// (the sky-visibility term fades where the stone's bevel band is under a few pixels)
			float wv = DetSmoothStep (1.5, 4.0, atm.pebShape2.x * 0.18 * atm.pebShape.x * s / footprint);
			// (the crags fade out when the stone is a few pixels)
			float crag = (1.0 - smoothstep (0.015 * s, 0.04 * s, footprint)) * atm.pebShape2.z;
			for (int l = 0; l < nl; l++) {
				int m = k - DET_JMIN + 64 + 29 * l;
				float dl = site ? clamp (P - float (l), 0.0, 1.0) : tile.morph.w / float (nl);
				if (dl <= 0.0) break;
				ivec3 cell, p; vec3 sg;
				vec3 f = DetLattice (m, pt, k, cell, p, sg);
				float top, tone;
				vec4 hg = DetPebbleCellGrad (cell, m, k, dl, f, crag, S, site, top, tone);
				vec3 gk = DetToPlanet (hg.xyz, p, sg) * (w / s);
				ds.grad += gk;
				gs += gk * wv;
				hs += w * hg.w;
				ts += w * hg.w * tone;
			}
		}
		// (stones: the body's albedo factor, and the spread between them)
		ds.albedo *= mix (1.0, atm.pebPrm.x * (1.0 + atm.pebShape2.w * ts / max (hs, 1e-6)), smoothstep (0.001, 0.004, hs));
	}
	// grit: grains of a few mm (shading only, never in meshes or physics)
	if (atm.pebPrm.y > 0.0 && footprint < 0.004 && Af.y > 0.0 && pg > 0.0) {
		float gc = 0.0, gt = 0.0;
		for (int k = -5; k >= -6; k--) {
			float s = exp2 (float (k));
			float w = DetSmoothStep (1.5, 3.5, 0.18 * s / footprint) * Af.y * pg;
			if (w <= 0.0) continue;
			float wv = DetSmoothStep (1.5, 4.0, atm.pebShape2.x * 0.18 * atm.pebShape.x * s / footprint);
			int m = k - DET_JMIN + 180;
			ivec3 cell, p; vec3 sg;
			vec3 f = DetLattice (m, pt, k, cell, p, sg);
			float top, tone;
			vec4 hg = DetPebbleCellGrad (cell, m, k, gritDens, f, 0.0, S, false, top, tone);
			vec3 gk = DetToPlanet (hg.xyz, p, sg) * (w / s);
			ds.grad += gk;
			gs += gk * wv;
			gc += w * top;
			gt += w * top * tone;
		}
		ds.albedo *= mix (1.0, atm.pebPrm.x * (1.0 + atm.pebShape2.w * gt / max (gc, 1e-4)), 0.7 * smoothstep (0.15, 0.5, gc));
	}
	// desiccation cracks of the playas (shading only; the slopes of the rims)
	if (playa > 0.0) {
		float alb = 1.0, ptone = 0.0;
		for (int L = 0; L < 3; L++) {
			float s = exp2 (float (PLAYA_K[L]));
			float meanCov = 2.0 * PLAYA_HW[L] * PLAYA_EDGE;          // the network's mean coverage
			float w = L == 0 ? 0.0 : DetSmoothStep (6.0, 16.0, s / footprint); // cells of 6-16 pixels and more: drawn (perf: the thinner cracks are sub-pixel before; DESIGN 3.10) (the macro layer only as its mean: perf, DESIGN 3.10)
			float cov = meanCov;
			if (w > 0.0) {
				float wc = w * DetSmoothStep (1.0, 3.0, PLAYA_CURL_W * s / footprint) * playa; // (rims of 1-3 pixels and more)
				float tone;
				float c = PlayaCrackLayer (pt, d, PLAYA_K[L], PLAYA_M[L], PLAYA_HW[L], PLAYA_CURL[L], max (0.5 * footprint / s, 1e-4), wc, ds.grad, tone);
				cov = mix (meanCov, c, w);
				if (L == 1) ptone = tone * w;
			}
			alb *= 1.0 - (1.0 - PLAYA_DARK[L]) * cov;
		}
		ds.albedo *= mix (1.0, alb * (1.0 + PLAYA_TONE * ptone), playa);
	}
	// sky visibility of the stones: the uniform sky seen by a facet tilted
	// by the stones' slope, (1 + cos)/2, squared for the neighbours' occlusion
	// (the ambient light ignores the normal, so the stones would be flat)
	gs -= d * dot (gs, d);
	skyVis *= pow (0.5 + 0.5 * inversesqrt (1.0 + dot (gs, gs)), DET_SKY_EXP);
	sunShadow *= ds.shadow;
	base *= clamp (1.0 + ds.height / max (Af.x, 1.0), 0.8, 1.2) * ds.albedo;
	vec3 g = ds.grad;
	vec3 df = d;
	g -= df * dot (g, df);         // (along the surface)
	vec3 gv = transpose (v2p) * g; // view space
	gv -= Nv * dot (gv, Nv);
	Nv = normalize (Nv - gv);
}
