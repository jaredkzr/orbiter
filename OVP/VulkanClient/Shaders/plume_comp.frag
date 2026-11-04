// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Engine plumes marched at half resolution (plume.frag, scale 2) over the
// scene: the light of the plumes (rgb) and the transmittance of their
// smoke (a) are blended as dst * a + rgb, the same as drawing the plumes
// one by one; the reactive mask takes the strongest. Each pixel takes the
// four nearest half-size texels by their distance and by how near the
// surface each was clipped at is to its own (the depth of a 2x2 block's
// nearest pixel): the plume does not bleed over the edges of a vessel.

#version 450

layout(set = 0, binding = 0) uniform sampler2D halfColor;
layout(set = 0, binding = 1) uniform sampler2D halfMask;
layout(set = 0, binding = 2) uniform sampler2D prepass; // view depth (w; 0: none)

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Dist (float z) { return z > 0.0 ? z : 1e30; }

// the depth the half-size texel h was clipped at (plume.frag)
float HalfDepth (ivec2 h, ivec2 fsize)
{
	ivec2 f = h * 2;
	float d = Dist (texelFetch (prepass, min (f, fsize - 1), 0).w);
	d = min (d, Dist (texelFetch (prepass, min (f + ivec2 (1, 0), fsize - 1), 0).w));
	d = min (d, Dist (texelFetch (prepass, min (f + ivec2 (0, 1), fsize - 1), 0).w));
	d = min (d, Dist (texelFetch (prepass, min (f + ivec2 (1, 1), fsize - 1), 0).w));
	return d;
}

void main ()
{
	ivec2 fp = ivec2 (gl_FragCoord.xy);
	ivec2 hsize = textureSize (halfColor, 0), fsize = textureSize (prepass, 0);
	vec2 hp = (vec2 (fp) + 0.5) * 0.5 - 0.5;
	ivec2 h0 = ivec2 (floor (hp));
	vec2 f = hp - vec2 (h0);
	vec4 c[4];
	float m[4];
	ivec2 hk[4];
	bool any = false;
	for (int k = 0; k < 4; k++) {
		hk[k] = clamp (h0 + ivec2 (k & 1, k >> 1), ivec2 (0), hsize - 1);
		c[k] = texelFetch (halfColor, hk[k], 0);
		m[k] = texelFetch (halfMask, hk[k], 0).r;
		any = any || c[k].a < 1.0 || dot (c[k].rgb, vec3 (1.0)) > 0.0 || m[k] > 0.0;
	}
	if (!any) discard; // (no plume here)
	float z = Dist (texelFetch (prepass, fp, 0).w);
	vec4 sc = vec4 (0.0);
	float sm = 0.0, ws = 0.0, bestRel = 1e30;
	int best = 0;
	for (int k = 0; k < 4; k++) {
		float wb = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		float dz = HalfDepth (hk[k], fsize);
		float rel = abs (dz - z) / min (dz, z); // (both none: 0)
		if (rel < bestRel) bestRel = rel, best = k;
		float w = wb * exp (-rel * rel * 2500.0); // (within about 2 % of the distance)
		sc += w * c[k];
		sm += w * m[k];
		ws += w;
	}
	if (ws > 1e-4) {
		outColor = sc / ws;
		outReactive = sm / ws;
	} else { // (no neighbour at this depth: the nearest in depth)
		outColor = c[best];
		outReactive = m[best];
	}
}
