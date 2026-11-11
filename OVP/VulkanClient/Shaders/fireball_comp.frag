// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Crash fireballs marched at half resolution (fireball.frag) over the
// scene: their light (rgb) and transmittance (a) blended as dst * a + rgb;
// the reactive mask the strongest. Each pixel takes the four nearest
// half-size texels by their distance and by how near the surface each was
// clipped at is to its own (as plume_comp.frag: the ball does not bleed
// over the edges of a vessel or the horizon). The smoke volume in front of
// the fireball (its integral up to the depth where the ball begins) dims
// its light and adds the smoke's light where the ball hides what is behind
// (as the sprites: gpuparticle.frag); the ball itself is drawn over the
// volume's smoke behind it.

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "smokevol.glsl"

layout(set = 1, binding = 0) uniform sampler2D halfColor;
layout(set = 1, binding = 1) uniform sampler2D halfMask;
layout(set = 1, binding = 2) uniform sampler2D halfOcc;
layout(set = 1, binding = 3) uniform sampler2D prepass; // view depth (w; 0: none)
layout(set = 1, binding = 4) uniform sampler3D volInteg; // smoke in front: light (rgb), transmittance (a)

layout(push_constant) uniform Comp {
	uint fog; // the smoke volume is drawn this frame
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

float Dist (float z) { return z > 0.0 ? z : 1e30; }

float HalfDepth (ivec2 h, ivec2 fsize, int s)
{
	ivec2 f = h * s;
	float d = 1e30;
	for (int k = 0; k < s * s; k++)
		d = min (d, Dist (texelFetch (prepass, min (f + ivec2 (k % s, k / s), fsize - 1), 0).w));
	return d;
}

void main ()
{
	ivec2 fp = ivec2 (gl_FragCoord.xy);
	ivec2 hsize = textureSize (halfColor, 0), fsize = textureSize (prepass, 0);
	int s = max (1, (fsize.x + hsize.x - 1) / hsize.x);
	vec2 hp = (vec2 (fp) + 0.5) / float (s) - 0.5;
	ivec2 h0 = ivec2 (floor (hp));
	vec2 f = hp - vec2 (h0);
	vec4 c[4], oc[4];
	float m[4];
	ivec2 hk[4];
	bool any = false;
	for (int k = 0; k < 4; k++) {
		hk[k] = clamp (h0 + ivec2 (k & 1, k >> 1), ivec2 (0), hsize - 1);
		c[k] = texelFetch (halfColor, hk[k], 0);
		m[k] = texelFetch (halfMask, hk[k], 0).r;
		oc[k] = texelFetch (halfOcc, hk[k], 0);
		any = any || c[k].a < 1.0;
	}
	if (!any) discard; // (no fireball here)
	float z = Dist (texelFetch (prepass, fp, 0).w);
	vec4 sc = vec4 (0.0);
	float sm = 0.0, ws = 0.0, bestRel = 1e30, front = 0.0;
	int best = 0;
	for (int k = 0; k < 4; k++) {
		float wb = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		float dz = HalfDepth (hk[k], fsize, s);
		float rel = abs (dz - z) / min (dz, z); // (both none: 0)
		if (rel < bestRel) bestRel = rel, best = k;
		float w = wb * exp (-rel * rel * 2500.0); // (within about 2 % of the distance)
		sc += w * c[k];
		sm += w * m[k];
		front = max (front, oc[k].r);
		ws += w;
	}
	vec4 col;
	float rm;
	if (ws > 1e-4) col = sc / ws, rm = sm / ws;
	else col = c[best], rm = m[best]; // (no neighbour at this depth: the nearest in depth)
	if (pc.fog != 0u && front > 0.0) {
		vec4 fog = textureLod (volInteg, vec3 (gl_FragCoord.xy / frame.jitter.zw, VolSlice (1.0 / front)), 0.0);
		col.rgb = col.rgb * fog.a + (1.0 - col.a) * fog.rgb;
	}
	outColor = col;
	outReactive = rm;
}
