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
	vec4 heat;       // xyz: towards the companion whose tide draws it out (star's frame), w: 1 (0: none)
	vec4 maps;
} draw;

layout(location = 0) in vec3 vPosView;
layout(location = 1) in vec3 vNormalView;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 outColor;

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
// the active band holding a group with a probability rising with activity:
// a leading and a following spot along the longitude (bipolar pairs), their
// sizes drawn from a steep distribution (most small, a few large), ragged
// edges; umbra the inner 40%, penumbra the rest; plage (faculae) about the
// group, wider. A cool dwarf's spots larger, at all latitudes
void Spots (vec3 o, float act, float seed, float fw, out float umbra, out float penumbra, out float plage)
{
	umbra = penumbra = plage = 0.0;
	const float K = 8.0;
	vec3 p = o * K, ip = floor (p);
	float bandC = 0.3 - 0.1 * act, bandW = 0.12 + 0.5 * act * act;
	float rough = Fbm (o * 90.0 + seed) - 0.5; // (ragged edges)
	for (int z = -1; z <= 1; z++)
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				vec3 cid = ip + vec3 (x, y, z);
				vec3 h = Hash33 (cid + seed * 0.37);
				vec3 cen = normalize ((cid + 0.2 + 0.6 * h) / K);
				float cl = (abs (cen.y) - bandC) / bandW;
				float prob = (0.005 + 0.2 * act * act) * exp (-cl * cl); // (a G star's ~0.02: a dozen groups; a flare star's ~0.13)
				if (Hash13 (cid * 1.7 + seed) > prob) continue;
				// sizes [rad]: a steep distribution, up to ~0.03 (a cool dwarf's
				// up to ~0.05; within the cell's reach: no straight edges)
				float r0 = (0.006 + 0.024 * h.z * h.z * h.z * h.z) * (1.0 + act);
				vec3 ce = normalize (cross (vec3 (0.0, 1.0, 0.0), cen) + vec3 (1e-6, 0.0, 0.0));
				for (int k = 0; k < 2; k++) {
					float r = k == 0 ? r0 : r0 * (0.4 + 0.5 * h.x);
					vec3 sc = normalize (cen + ce * (k == 0 ? 1.0 : -1.3) * r0);
					float d = length (o - sc) / max (r, fw); // (no smaller than a pixel: no shimmer)
					d += 0.7 * rough;
					penumbra = max (penumbra, 1.0 - smoothstep (0.85, 1.0, d));
					umbra = max (umbra, 1.0 - smoothstep (0.32, 0.42, d));
					plage = max (plage, 1.0 - smoothstep (1.0, 3.0, d));
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
	if (draw.heat.w > 0.5) col *= 1.0 - 0.45 * smoothstep (0.3, 1.0, dot (o, draw.heat.xyz));
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
