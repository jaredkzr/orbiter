// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Smoke volume: the half-resolution march (smoke_march.comp) over the
// scene: each pixel takes the four nearest rays, each up to whichever of its
// two depths (nearest, farthest surface) is closer to the pixel's own, and
// weighs them by how close that is (a depth-aware upsample: no smoke halos
// around nearer objects, none drawn over them from behind)

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "smokevol.glsl"
layout(set = 1, binding = 4) uniform sampler2D prepass;  // view depth (w; 0: none)
layout(set = 1, binding = 41) uniform sampler2D marchNear;
layout(set = 1, binding = 42) uniform sampler2D marchFar;
layout(set = 1, binding = 43) uniform sampler2D marchDepth;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

void main ()
{
	ivec2 hs = textureSize (marchNear, 0);
	float z = texelFetch (prepass, ivec2 (gl_FragCoord.xy), 0).w;
	if (z <= 0.0) z = 2.0 * VOL_FAR;
	vec2 h = gl_FragCoord.xy * 0.5 - 0.5; // (half-resolution texel centres)
	ivec2 b = ivec2 (floor (h));
	vec2 f = h - vec2 (b);
	vec4 sum = vec4 (0.0);
	float wsum = 0.0;
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2 (k & 1, k >> 1);
		ivec2 t = clamp (b + o, ivec2 (0), hs - 1);
		vec2 dz = texelFetch (marchDepth, t, 0).xy;
		bool near = abs (z - dz.x) <= abs (z - dz.y);
		vec4 v = near ? texelFetch (marchNear, t, 0) : texelFetch (marchFar, t, 0);
		float e = abs (z - (near ? dz.x : dz.y)) / (0.03 * z + 0.5);
		float wb = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
		float w = (wb + 1e-3) / (1e-2 + e * e);
		sum += v * w, wsum += w;
	}
	vec4 v = sum / wsum;
	float a = 1.0 - v.a;
	if (a < 0.002) discard;
	outColor = vec4 (v.rgb, a);
	outReactive = clamp (a, 0.0, 0.9);
}
