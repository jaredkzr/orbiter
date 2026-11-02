// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// A star's disc (the Sun and the others drawn as bodies): its photosphere,
// unlit, far brighter than any lit surface.
// - Limb darkening: a quadratic law per colour (stronger in blue: the Sun's
//   u1, u2 about 0.60, 0.18 in blue, 0.38, 0.24 in red), the disc's edge
//   darker and redder.
// - Granulation: convection cells (cellular noise, bright centres and dark
//   lanes, ~1000 km on the Sun) evolving over minutes, and supergranulation;
//   each shown where a pixel spans few enough of them.
// - Starspots in the active latitudes (about the rotation axis, the
//   star's y), dark umbrae in lighter penumbrae, faculae bright towards the
//   limb; more and larger on an active star (activity).
// - Flares (Scene::StarFlares): hot, blue-white patches, rising in a
//   minute and fading over minutes.
// The pattern turns with the star (its rotation is the model's).

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D tex; // (the mesh layout's; unused)

layout(push_constant) uniform Draw {
	mat4 modelView;  // object -> view (the star's rotation, its radius)
	vec4 diffuse;    // rgb: the photosphere's radiance at the disc's centre (scene)
	vec4 emissive;   // x: activity (0..1), y: time [s], z: granule size [rad], w: seed
	vec4 specular;   // flare 1: direction (star's frame), w: brightness (of the disc's centre; 0: none)
	vec4 light;      // flare 2
	vec4 pbr;        // x: a flare's angular radius [rad]
	vec4 heat;       // xyz: towards the companion whose tide draws it out (star's frame), w: 1 (0: none; 2: filling its Roche lobe, the mesh scaled by ROCHE_BOX)
	vec4 maps;
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// a star filling its Roche lobe about a far heavier companion (Hill's
// approximation): the equipotential through the inner Lagrange point, a
// cusp there (x: towards the companion; z: the orbit's axis), in Hill's
// units; its back rounded a little (an artistic liberty: a teardrop)
const vec3 ROCHE_BOX = vec3 (1.45, 0.93, 0.97); // (the mesh: x, the orbit's axis, the third, in mean radii)
const float ROCHE_K = 0.4918;                   // (Hill's units per mean radius: L1 at 1.41 of it)
const float ROCHE_PHI = -2.16337 - 0.003;       // (the surface: just within L1's)
const float ROCHE_BACK = 1.5;
vec3 RocheH (vec3 m) { return vec3 (m.x * ROCHE_BOX.x, m.z * ROCHE_BOX.z, m.y * ROCHE_BOX.y) * ROCHE_K; } // (mesh -> Hill: x, y in the plane, z axis)
float RochePhi (vec3 h)
{
	float bk = max (-h.x, 0.0);
	return -1.0 / length (h) - 1.5 * h.x * h.x + 0.5 * h.z * h.z + ROCHE_BACK * bk * bk * bk;
}
vec3 RocheGrad (vec3 h)
{
	float r = length (h), r3 = r * r * r, bk = max (-h.x, 0.0);
	return vec3 (h.x / r3 - 3.0 * h.x - 3.0 * ROCHE_BACK * bk * bk, h.y / r3, h.z / r3 + h.z);
}

float Hash13 (vec3 p)
{
	p = fract (p * 0.1031);
	p += dot (p, p.zyx + 31.32);
	return fract ((p.x + p.y) * p.z);
}

vec3 Hash33 (vec3 p)
{
	p = vec3 (dot (p, vec3 (127.1, 311.7, 74.7)), dot (p, vec3 (269.5, 183.3, 246.1)), dot (p, vec3 (113.5, 271.9, 124.6)));
	return fract (sin (p) * 43758.5453123);
}

float ValueNoise (vec3 p)
{
	vec3 i = floor (p), f = fract (p);
	f = f * f * (3.0 - 2.0 * f);
	return mix (mix (mix (Hash13 (i), Hash13 (i + vec3 (1, 0, 0)), f.x), mix (Hash13 (i + vec3 (0, 1, 0)), Hash13 (i + vec3 (1, 1, 0)), f.x), f.y),
	            mix (mix (Hash13 (i + vec3 (0, 0, 1)), Hash13 (i + vec3 (1, 0, 1)), f.x), mix (Hash13 (i + vec3 (0, 1, 1)), Hash13 (i + vec3 (1, 1, 1)), f.x), f.y), f.z);
}

float Fbm (vec3 p)
{
	float s = 0.0, a = 0.5;
	for (int k = 0; k < 4; k++) { s += a * ValueNoise (p); p *= 2.03; a *= 0.5; }
	return s;
}

// cellular noise: F2 - F1 (0 on the lanes between cells), cells drifting with t
float Cells (vec3 p, float t)
{
	vec3 i = floor (p), f = fract (p);
	float d1 = 8.0, d2 = 8.0;
	for (int z = -1; z <= 1; z++)
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				vec3 g = vec3 (x, y, z);
				vec3 h = Hash33 (i + g);
				vec3 c = g + 0.5 + 0.4 * sin (6.2831853 * (h + t * (0.3 + 0.4 * h.yzx)));
				float d = length (c - f);
				if (d < d1) d2 = d1, d1 = d;
				else if (d < d2) d2 = d;
			}
	return d2 - d1;
}

// spot groups: on a lattice over the star (cells about 7 deg), a cell in
// the active band holding a group with a probability rising with activity.
// Groups of their own kinds (per cell): a single round spot; a bipolar
// pair, leader and follower, with pores between; a complex cluster of
// several spots. Tilted from the east-west line (Joy's law: leaders nearer
// the equator), sizes drawn from a steep distribution (most small, a few
// large), each spot its own shape: elongated, a ragged edge at its own
// scale, umbra a third to a half of it (pores: umbra alone), its penumbra
// streaked radially (filaments, where resolved); plage (faculae) about the
// group, wider. A cool dwarf's spots larger, at all latitudes. The edges
// no sharper than a pixel
void Spots (vec3 o, float act, float seed, float fw, out float umbra, out float penumbra, out float plage)
{
	umbra = penumbra = plage = 0.0;
	const float K = 8.0;
	vec3 p = o * K, ip = floor (p);
	float bandC = 0.3 - 0.1 * act, bandW = 0.12 + 0.5 * act * act;
	for (int z = -1; z <= 1; z++)
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				vec3 cid = ip + vec3 (x, y, z);
				vec3 h = Hash33 (cid + seed * 0.37), h2 = Hash33 (cid * 3.1 + seed * 1.3);
				vec3 cen = normalize ((cid + 0.2 + 0.6 * h) / K);
				float cl = (abs (cen.y) - bandC) / bandW;
				float prob = (0.005 + 0.2 * act * act) * exp (-cl * cl); // (a G star's ~0.02: a dozen groups; a flare star's ~0.13)
				if (Hash13 (cid * 1.7 + seed) > prob) continue;
				// the group's size [rad] (a steep distribution, up to ~0.03; a
				// cool dwarf's larger) and kind; its axis: east-west, tilted
				// (within the lattice's reach: a group's spots and plage no
				// farther than ~0.15 rad from its centre)
				float r0 = min ((0.005 + 0.025 * h.z * h.z * h.z * h.z) * (1.0 + act), 0.034);
				int kind = h2.x < 0.3 ? 0 : (h2.x < 0.75 ? 1 : 2), n = kind == 0 ? 1 : (kind == 1 ? 2 + int (h2.y * 3.0) : 3 + int (h2.y * 4.0));
				vec3 east = normalize (cross (vec3 (0.0, 1.0, 0.0), cen) + vec3 (1e-6, 0.0, 0.0)), north = cross (cen, east);
				float tilt = sign (cen.y) * (0.1 + 0.4 * h2.z) + 0.3 * (h.x - 0.5);
				vec3 ax = east * cos (tilt) + north * sin (tilt), bx = cross (cen, ax);
				for (int k = 0; k < 7; k++) {
					if (k >= n) break;
					vec3 hk = Hash33 (cid * 7.3 + float (k) * 13.1 + seed);
					// (where along the axis and across it, how large: the leader
					// first, the follower at the far end, pores and spots between)
					float along, across, r;
					if (kind == 0) along = 0.0, across = 0.0, r = r0;
					else if (k == 0) along = 1.0, across = 0.0, r = r0;
					else if (k == 1 && kind == 1) along = -1.4 - 0.6 * hk.x, across = 0.4 * (hk.y - 0.5), r = r0 * (0.35 + 0.5 * hk.z);
					else along = 2.4 * hk.x - 1.4, across = 1.2 * (hk.y - 0.5), r = r0 * (kind == 2 ? 0.25 + 0.6 * hk.z * hk.z : 0.12 + 0.2 * hk.z);
					vec3 sc = normalize (cen + (ax * along + bx * across) * r0 * 1.3);
					// (its own shape: elongated along a direction of its own, ragged at its scale)
					vec3 dv = o - sc;
					float ang = 6.2831853 * hk.x, el = 1.0 + 0.5 * hk.y;
					vec3 e1 = ax * cos (ang) + bx * sin (ang), e2 = cross (cen, e1);
					float rr = max (r, fw);
					vec2 q = vec2 (dot (dv, e1) / el, dot (dv, e2)) / rr;
					float d = length (q);
					d += 0.35 * (Fbm (vec3 (q * 2.2, hk.z * 37.0 + seed)) - 0.5) * smoothstep (0.0, 0.6, d);
					float aa = max (0.04, 0.75 * fw / rr); // (no sharper than a pixel)
					bool pore = r < 0.4 * r0 && hk.z < 0.5;
					float uf = pore ? 0.85 : 0.32 + 0.18 * hk.z;
					float pen = 1.0 - smoothstep (1.0 - aa, 1.0 + aa, d);
					// (filaments: the penumbra streaked radially, where a pixel resolves them)
					float fil = 1.0 - (1.0 - smoothstep (0.1, 0.35, aa)) * 0.25 * ValueNoise (vec3 (q / max (length (q), 1e-4) * 5.0, d * 2.0 + hk.x * 50.0));
					penumbra = max (penumbra, pen * fil);
					umbra = max (umbra, 1.0 - smoothstep (uf - aa, uf + aa, d));
					plage = max (plage, 1.0 - smoothstep (1.0, 2.2, d));
				}
			}
}

void main ()
{
	// the surface's normal where the pixel's ray meets the star (exact: the
	// mesh's interpolated normals vary facet by facet across a large disc):
	// in the star's frame a unit sphere (an ellipsoid in view, a star drawn
	// out by its companion's tide); the far side (both faces drawn) and the
	// limb's edge left out
	mat3 M3 = mat3 (draw.modelView), Mi = inverse (M3);
	vec3 d = normalize (vPosView);
	vec3 ro = Mi * (-draw.modelView[3].xyz), rd = Mi * d;
	float A = dot (rd, rd), b = dot (ro, rd), q = b * b - A * (dot (ro, ro) - 1.0);
	if (q < 0.0) discard;
	vec3 o = normalize (ro + rd * ((-b - sqrt (q)) / A)); // (the surface's direction in the star's frame: the pattern turns with it)
	vec3 n = normalize (transpose (Mi) * o), v = -d;
	bool roche = draw.heat.w > 1.5;
	vec3 hH = vec3 (0.0), gH = vec3 (0.0, 0.0, 1.0);
	if (roche) {
		// (the ray marched through the mesh's ellipsoid to the lobe's surface, then bisected)
		float t0 = max ((-b - sqrt (q)) / A, 0.0), t1 = (-b + sqrt (q)) / A;
		const int NM = 48;
		float dt = (t1 - t0) / float (NM), ta = t0, tb = -1.0;
		for (int k = 1; k <= NM; k++) {
			float tt = t0 + dt * float (k);
			if (RochePhi (RocheH (ro + rd * tt)) < ROCHE_PHI) { tb = tt; break; }
			ta = tt;
		}
		if (tb < 0.0) discard;
		for (int k = 0; k < 10; k++) {
			float tm = 0.5 * (ta + tb);
			if (RochePhi (RocheH (ro + rd * tm)) < ROCHE_PHI) tb = tm; else ta = tm;
		}
		vec3 m = ro + rd * tb;
		hH = RocheH (m);
		gH = RocheGrad (hH);
		vec3 gm = vec3 (gH.x * ROCHE_BOX.x, gH.z * ROCHE_BOX.y, gH.y * ROCHE_BOX.z) * ROCHE_K;
		o = normalize (m);
		n = normalize (transpose (Mi) * gm);
	}
	float mu = clamp (dot (n, v), 0.0, 1.0);
	float act = draw.emissive.x, t = draw.emissive.y, seed = draw.emissive.w;

	// limb darkening (quadratic, per colour)
	vec3 u1 = vec3 (0.38, 0.47, 0.60), u2 = vec3 (0.24, 0.23, 0.18);
	vec3 ld = 1.0 - u1 * (1.0 - mu) - u2 * (1.0 - mu) * (1.0 - mu);

	// granulation and supergranulation, faded in as a pixel spans few cells
	float fw = max (length (fwidth (o)), 1e-9); // [rad] of the surface a pixel spans
	float g = 1.0;
	float cell = draw.emissive.z;
	float vis = 1.0 - smoothstep (0.25 * cell, cell, fw);
	if (vis > 0.0) {
		float c = Cells (o / cell, t / 600.0);
		g *= 1.0 + vis * 0.16 * (smoothstep (0.0, 0.45, c) - 0.6);
	}
	float scell = cell * 30.0;
	float svis = 1.0 - smoothstep (0.25 * scell, scell, fw);
	if (svis > 0.0) g *= 1.0 + svis * 0.03 * (smoothstep (0.0, 0.5, Cells (o / scell + seed, t / 86400.0)) - 0.5);

	// starspots in the active latitudes, faculae about them
	float umbra, penumbra, plage;
	Spots (o, act, seed, fw, umbra, penumbra, plage);
	float spot = 1.0 - 0.55 * penumbra - 0.4 * umbra;
	float fac = plage * (1.0 - penumbra) * (0.05 + 0.6 * (1.0 - mu) * (1.0 - mu));

	vec3 col = draw.diffuse.rgb * ld * g * spot * (1.0 + fac);
	// (drawn out by a tide: gravity darkening, the surface's gravity least
	// towards the inner Lagrange point, its flux as g^0.32 (convective))
	if (roche) {
		// (the lobe's surface gravity, least at the cusp, over the pole's;
		// the side facing the companion's hot disc heated (an artistic
		// liberty: really a few per cent))
		col *= pow (clamp (length (gH) / 5.57, 0.05, 1.5), 0.32);
		float irr = 0.25 * smoothstep (-0.2, 1.0, normalize (gH).x);
		col *= (1.0 + irr) * mix (vec3 (1.0), vec3 (0.97, 1.0, 1.1), irr * 3.0);
	} else if (draw.heat.w > 0.5) col *= 1.0 - 0.45 * smoothstep (0.3, 1.0, dot (o, draw.heat.xyz));
	// (umbrae cooler: redder)
	col *= mix (vec3 (1.0), vec3 (1.0, 0.75, 0.55), umbra);

	// flares: hot, blue-white
	float fr = max (draw.pbr.x, 1e-3);
	for (int k = 0; k < 2; k++) {
		vec4 f = k == 0 ? draw.specular : draw.light;
		if (f.w <= 0.0) continue;
		float a = acos (clamp (dot (o, normalize (f.xyz)), -1.0, 1.0));
		col += draw.diffuse.rgb * f.w * exp (-(a * a) / (fr * fr)) * vec3 (0.85, 0.95, 1.25);
	}
	outColor = vec4 (min (col, vec3 (65000.0)), 1.0);
}
