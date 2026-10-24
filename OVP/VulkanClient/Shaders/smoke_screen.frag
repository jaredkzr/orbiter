// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Screen-space smoke: the blurred thickness buffer composited as one
// surface: absorption by its thickness, light (the particles' average)
// shaded by the thickness gradient (billows facing the sun brighter),
// edges broken up by noise

#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
layout(set = 1, binding = 3) uniform sampler3D detailNoise;
layout(set = 1, binding = 19) uniform sampler2D thickTex;

layout(push_constant) uniform Params {
	vec4 noiseOrg; // w: time [s]
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out float outReactive;

void main ()
{
	vec4 s = textureLod (thickTex, vUV, 0.0);
	if (s.a <= 1e-4) discard;
	vec2 tx = 1.0 / vec2 (textureSize (thickTex, 0));
	float n = texture (detailNoise, vec3 (vUV * vec2 (6.0, 3.4), pc.noiseOrg.w * 0.01)).r * 0.6
		+ texture (detailNoise, vec3 (vUV * vec2 (17.0, 9.5), pc.noiseOrg.w * 0.02 + 0.5)).r * 0.4;
	float th = s.a * smoothstep (0.15, 0.6, n + min (s.a, 1.0) * 0.5);
	// the surface's slope from the thickness (as a height field)
	float hx = textureLod (thickTex, vUV + vec2 (tx.x, 0.0), 0.0).a - textureLod (thickTex, vUV - vec2 (tx.x, 0.0), 0.0).a;
	float hy = textureLod (thickTex, vUV + vec2 (0.0, tx.y), 0.0).a - textureLod (thickTex, vUV - vec2 (0.0, tx.y), 0.0).a;
	vec3 N = normalize (vec3 (-hx * 6.0, hy * 6.0, -1.0));
	float sun = clamp ((dot (N, frame.sunDir.xyz) + 0.4) / 1.4, 0.0, 1.0);
	vec3 col = s.rgb / s.a * (0.65 + 0.5 * sun);
	float a = 1.0 - exp (-2.5 * th);
	outColor = vec4 (col * a, a);
	outReactive = clamp (a, 0.0, 0.9);
}
