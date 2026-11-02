// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// The sky near a black hole: each pixel's light from where its ray comes
// from (lens_trace.comp's grid, bilinear between its corners): the Milky
// Way's baked cube (as milkyway.frag) and the extragalactic background
// (cosmicbg.glsl), shifted by g (gravity and the camera's motion: blue
// falling in or hovering low, red climbing out) in their bands. Where
// the corners of a pixel's cell agree, their direction is interpolated;
// at the edges of images and of the shadow, the colours of the corners
// are. Cells that are not smooth (lenscell.glsl) take their pixels' own
// corners (the refining pass's map). The shadow is black. Translucent gas
// (the overlay: the disc's fading edge, its lit outer parts, the stream,
// the jets) in a pass of its own after the opaque scene (gas.x = 1): at
// the depth of its first layer, over what is farther (a donor star),
// hidden by what is nearer.
// The stars (lens_stars.comp's bins) are gathered: those inside the
// footprint on the sky of a grid cell near the pixel are where the cell
// maps them back to (inverse bilinear), brightened by the cell's solid
// angle over its footprint's (the magnification: images near the ring
// flare, those wound about the photon orbit fade), spread over the
// nearest pixels (bilinear); every image of a star counts. Cells whose
// footprints span more than STAR_SPAN (demagnified hard) are left out.

#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube skyA;
layout(set = 0, binding = 1) uniform samplerCube skyB;
layout(set = 0, binding = 2) uniform samplerCube zodiacal;
layout(set = 1, binding = 0) uniform sampler2D lensMap;
layout(set = 1, binding = 3) uniform sampler2D overlayMap;
layout(set = 1, binding = 4) uniform sampler2D fineMap;     // (every pixel's corners, where refined)
layout(set = 1, binding = 5) uniform sampler2D fineOverlay;
layout(std430, set = 1, binding = 1) readonly buffer Offsets { uint offsets[]; };
layout(std430, set = 1, binding = 2) readonly buffer Records { uvec4 records[]; };

#include "relativity.glsl"
#include "relsky.glsl"
#include "cosmicbg.glsl"
#include "lensstars.glsl"
#include "lenscell.glsl"

layout(push_constant) uniform Params {
	vec4 colour[3]; // bands (S10) -> scene RGB (incl. the eye's adaptation)
	vec4 tone;      // pivot luminance, contrast, saturation, highlight shoulder (0: none)
	vec4 grid;      // the grid's spacing [px], fade to skyB, the extragalactic background (1: drawn), cells refined (1)
	vec4 bg;        // xyz: the hole's velocity relative to the cosmic background (Galactic, units of c); w: the stars (1: drawn)
	vec4 disc;      // x: the radiance of a surface of the Sun's (scene) per 1 S10 through colour (STAR_BRIGHTNESS / gain)
	vec4 gas;       // x: 1 the gas pass; y: the near plane [m]; z: the hole's mass M [m]
} pc;

const float STAR_SPAN = 0.9994; // cos 2 deg

vec3 Shade (vec3 d, float g)
{
	vec4 b = texture (skyA, d);
	if (pc.grid.y > 0.0) b = mix (b, texture (skyB, d), pc.grid.y);
	b = RelShiftBands (b, g);
	// (the background where it is shifted to be seen, as cosmicbg.frag's)
	float Dbg = g * RelDopplerSeen (d, pc.bg.xyz);
	if (pc.grid.z > 0.0 && Dbg > 1.5) b += CosmicBands (Dbg);
	vec3 c = max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
	if (pc.tone.w > 0.0 || pc.tone.y != 1.0 || pc.tone.z != 1.0) {
		float Y = max (dot (c, vec3 (0.2126, 0.7152, 0.0722)), 1e-20);
		float y = pow (Y / pc.tone.x, pc.tone.y);
		if (pc.tone.w > 0.0) y /= 1.0 + y / pc.tone.w;
		c = max (mix (vec3 (Y), c, pc.tone.z), vec3 (0.0)) * (pc.tone.x * y / Y);
	}
	return c;
}

// the accretion disc's light seen at temperature T (a black body), in the
// sky's bands relative to the Sun's surface, times its brightness em
vec3 DiscShade (float T, float em)
{
	const int NS = 6;
	vec4 b;
	for (int i = 0; i < 4; i++) {
		float dl = (REL_BAND_HI[i] - REL_BAND_LO[i]) / float (NS), s = 0.0, sun = 0.0;
		for (int k = 0; k < NS; k++) {
			float l = REL_BAND_LO[i] + dl * (float (k) + 0.5);
			float ls = RelLogPlanck (l, REL_TSUN);
			s += exp (RelLogPlanck (l, T) - ls);
			sun += 1.0;
		}
		b[i] = s / sun;
	}
	b *= pc.disc.x * em;
	return max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
}

// a corner's light: the sky (w > 0), the disc (w < 0), the hole (0)
vec3 Corner (vec4 c)
{
	if (c.w > 0.0) return Shade (c.xyz, c.w);
	if (c.w < 0.0) return DiscShade (c.x, c.y);
	return vec3 (0.0);
}

float Cross2 (vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

// (u, v) of p in the quad a (0, 0), b (1, 0), c (1, 1), d (0, 1); outside: < 0 or > 1
vec2 InvBilinear (vec2 p, vec2 a, vec2 b, vec2 c, vec2 d)
{
	vec2 e = b - a, f = d - a, g = a - b + c - d, h = p - a;
	float k2 = Cross2 (g, f), k1 = Cross2 (e, f) + Cross2 (h, g), k0 = Cross2 (h, e);
	float v;
	if (abs (k2) < 1e-6 * abs (k1)) v = -k0 / k1;
	else {
		float w = k1 * k1 - 4.0 * k0 * k2;
		if (w < 0.0) return vec2 (-1.0);
		w = sqrt (w);
		v = (-k1 - w) / (2.0 * k2);
		if (v < 0.0 || v > 1.0) v = (-k1 + w) / (2.0 * k2);
	}
	vec2 den = e + g * v;
	float u = abs (den.x) > abs (den.y) ? (h.x - f.x * v) / den.x : (h.y - f.y * v) / den.y;
	return vec2 (u, v);
}

// the stars' light on the pixel at px (centre, render pixels), in the bands
// (radiance, S10), shifted
vec4 Stars (vec2 px)
{
	vec4 sum = vec4 (0.0);
	float s = pc.grid.x;
	ivec2 sz = textureSize (lensMap, 0);
	ivec2 a0 = clamp (ivec2 (floor ((px - 1.0) / s)), ivec2 (0), sz - 2);
	ivec2 a1 = clamp (ivec2 (floor ((px + 1.0) / s)), ivec2 (0), sz - 2);
	for (int cy = a0.y; cy <= a1.y; cy++)
		for (int cx = a0.x; cx <= a1.x; cx++) {
			vec4 k00 = texelFetch (lensMap, ivec2 (cx, cy), 0), k10 = texelFetch (lensMap, ivec2 (cx + 1, cy), 0);
			vec4 k01 = texelFetch (lensMap, ivec2 (cx, cy + 1), 0), k11 = texelFetch (lensMap, ivec2 (cx + 1, cy + 1), 0);
			if (k00.w <= 0.0 || k10.w <= 0.0 || k01.w <= 0.0 || k11.w <= 0.0) continue;
			vec3 C = normalize (k00.xyz + k10.xyz + k01.xyz + k11.xyz);
			float cosr = min (min (dot (C, k00.xyz), dot (C, k10.xyz)), min (dot (C, k01.xyz), dot (C, k11.xyz)));
			if (cosr < STAR_SPAN) continue;
			// (the footprint in the plane tangent at C)
			vec3 t1 = normalize (cross (C, abs (C.z) < 0.9 ? vec3 (0.0, 0.0, 1.0) : vec3 (1.0, 0.0, 0.0))), t2 = cross (C, t1);
			vec2 q00 = vec2 (dot (k00.xyz, t1), dot (k00.xyz, t2)) / dot (k00.xyz, C);
			vec2 q10 = vec2 (dot (k10.xyz, t1), dot (k10.xyz, t2)) / dot (k10.xyz, C);
			vec2 q01 = vec2 (dot (k01.xyz, t1), dot (k01.xyz, t2)) / dot (k01.xyz, C);
			vec2 q11 = vec2 (dot (k11.xyz, t1), dot (k11.xyz, t2)) / dot (k11.xyz, C);
			float area = 0.5 * abs (Cross2 (q11 - q00, q01 - q10)) * 3282.8063; // [deg^2]
			if (area <= 0.0) continue;
			vec4 cell = vec4 (0.0);
			vec2 uv;
			int f = LensFace (C, uv);
			ivec2 b = LensCellXY (uv);
			int nb = 1;
			for (int by = max (b.y - nb, 0); by <= min (b.y + nb, LENS_N - 1); by++)
				for (int bx = max (b.x - nb, 0); bx <= min (b.x + nb, LENS_N - 1); bx++) {
					int ci = LensCellIndex (f, ivec2 (bx, by));
					uint e = offsets[ci + 1];
					for (uint i = offsets[ci]; i < e; i++) {
						uvec4 rec = records[i];
						vec3 d = LensOctDecode (rec.x);
						if (dot (d, C) < cosr - 1e-6) continue;
						vec2 q = vec2 (dot (d, t1), dot (d, t2)) / dot (d, C);
						vec2 w2 = InvBilinear (q, q00, q10, q11, q01);
						if (any (lessThan (w2, vec2 (0.0))) || any (greaterThan (w2, vec2 (1.0)))) continue;
						vec2 sp = (vec2 (cx, cy) + w2) * s; // [px]
						vec2 t = max (vec2 (0.0), 1.0 - abs (sp - px));
						cell += vec4 (unpackHalf2x16 (rec.y), unpackHalf2x16 (rec.z)) * (t.x * t.y);
					}
				}
			if (cell == vec4 (0.0)) continue;
			float g = 0.25 * (k00.w + k10.w + k01.w + k11.w);
			sum += RelShiftBands (cell * (s * s / (area * LENS_FSCALE)), g);
		}
	return sum;
}

// a cell's light at w (its corners' weights): one image interpolated, else
// the corners' colours
vec3 Cell (vec4 c00, vec4 c10, vec4 c01, vec4 c11, vec4 w)
{
	if (LensCellSmooth (c00, c10, c01, c11)) {
		vec4 m = c00 * w.x + c10 * w.y + c01 * w.z + c11 * w.w;
		return m.w > 0.0 ? Shade (normalize (m.xyz), m.w) : (m.w < 0.0 ? DiscShade (m.x, m.y) : vec3 (0.0));
	}
	return w.x * Corner (c00) + w.y * Corner (c10) + w.z * Corner (c01) + w.w * Corner (c11);
}

void main ()
{
	vec2 mc = gl_FragCoord.xy / pc.grid.x;
	ivec2 sz = textureSize (lensMap, 0);
	ivec2 i0 = clamp (ivec2 (floor (mc)), ivec2 (0), sz - 2);
	vec2 fr = clamp (mc - vec2 (i0), 0.0, 1.0);
	vec4 c00 = texelFetch (lensMap, i0, 0), c10 = texelFetch (lensMap, i0 + ivec2 (1, 0), 0);
	vec4 c01 = texelFetch (lensMap, i0 + ivec2 (0, 1), 0), c11 = texelFetch (lensMap, i0 + ivec2 (1, 1), 0);
	vec4 o00 = texelFetch (overlayMap, i0, 0), o10 = texelFetch (overlayMap, i0 + ivec2 (1, 0), 0);
	vec4 o01 = texelFetch (overlayMap, i0 + ivec2 (0, 1), 0), o11 = texelFetch (overlayMap, i0 + ivec2 (1, 1), 0);
	vec4 w = vec4 ((1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y, fr.x * fr.y);
	if (!(pc.grid.w < 0.5 || LensCellSmooth (c00, c10, c01, c11))) {
		// (refined: the pixel's own corners)
		ivec2 j0 = clamp (ivec2 (floor (gl_FragCoord.xy)), ivec2 (0), textureSize (fineMap, 0) - 2);
		vec2 fj = clamp (gl_FragCoord.xy - vec2 (j0), 0.0, 1.0);
		w = vec4 ((1.0 - fj.x) * (1.0 - fj.y), fj.x * (1.0 - fj.y), (1.0 - fj.x) * fj.y, fj.x * fj.y);
		c00 = texelFetch (fineMap, j0, 0), c10 = texelFetch (fineMap, j0 + ivec2 (1, 0), 0);
		c01 = texelFetch (fineMap, j0 + ivec2 (0, 1), 0), c11 = texelFetch (fineMap, j0 + ivec2 (1, 1), 0);
		o00 = texelFetch (fineOverlay, j0, 0), o10 = texelFetch (fineOverlay, j0 + ivec2 (1, 0), 0);
		o01 = texelFetch (fineOverlay, j0 + ivec2 (0, 1), 0), o11 = texelFetch (fineOverlay, j0 + ivec2 (1, 1), 0);
	}
	if (pc.gas.x > 0.5) {
		// the gas: its light over what it hides (premultiplied), at the
		// depth of its nearest corner's first layer
		vec4 o = o00 * w.x + o10 * w.y + o01 * w.z + o11 * w.w;
		if (o.z <= 1e-4 && o.y <= 1e-6) discard;
		float dep = min (min (o00.w, o10.w), min (o01.w, o11.w));
		gl_FragDepth = clamp (pc.gas.y / max (dep * pc.gas.z, pc.gas.y), 0.0, 1.0);
		vec3 e = o.y > 0.0 && o.x > 100.0 ? DiscShade (o.x, o.y) : vec3 (0.0);
		outColor = vec4 (min (e, vec3 (65000.0)), clamp (o.z, 0.0, 1.0));
		return;
	}
	vec3 c = Cell (c00, c10, c01, c11, w);
	if (pc.bg.w > 0.0) {
		vec4 b = Stars (gl_FragCoord.xy);
		c += max (vec3 (dot (pc.colour[0], b), dot (pc.colour[1], b), dot (pc.colour[2], b)), vec3 (0.0));
	}
	outColor = vec4 (min (c, vec3 (65000.0)), 1.0);
}
